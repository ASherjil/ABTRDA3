#include "DPDKEal.hpp"

#include "PciHelpers.hpp"

#include <rte_eal.h>
#include <rte_errno.h>

#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {

// The process-global EAL state. File-scope (not class members) so DPDK.hpp stays free
// of the vectors and every consumer TU sees only the declarations.
std::vector<std::string> g_allowedBdfs;
std::vector<std::string> g_vdevs;
bool                     g_ealInited = false;

// Dedup on the part before the first ',' — the BDF or the vdev name — so an entry that
// carries devargs is never shadowed by a later bare registration of the same device.
void addUnique(std::vector<std::string>& v, const std::string& entry) {
  const auto key = entry.substr(0, entry.find(','));
  for (const auto& e : v)
    if (e.substr(0, e.find(',')) == key) return;
  v.push_back(entry);
}

}  // namespace

void DPDKEal::addAllowedBdf(const std::string& bdf) {
  addUnique(g_allowedBdfs, bdf);
}

void DPDKEal::addVdev(const std::string& arg) {
  addUnique(g_vdevs, arg);
}

std::size_t DPDKEal::vdevCount() noexcept {
  return g_vdevs.size();
}

bool DPDKEal::ealInit(const std::string& bdf, int lcore) noexcept {
  if (g_ealInited) return true;

  if (g_vdevs.empty()) addAllowedBdf(bdf);   // BDF mode only; AF_XDP vdevs register in prepare()

  // One EAL file-prefix per device set, so the RTT server and client can run as two
  // DPDK primaries on the same looped host.
  const std::string& base = !g_vdevs.empty() ? g_vdevs.front() : g_allowedBdfs.front();
  std::string prefix = "abtrda3_" + base.substr(0, base.find(','));
  for (char& c : prefix) if (c == ':' || c == '.' || c == ',') c = '_';
  const std::string lcoreStr = std::to_string(lcore);

  std::vector<std::string> args = {
    "abtrda3", "-l", lcoreStr, "--main-lcore", lcoreStr,
    "-n", "4", "--file-prefix", prefix,
    "--force-max-simd-bitwidth=" + std::to_string(kMaxSimdBitwidth)
  };
  if (!g_vdevs.empty()) {
    args.emplace_back("--no-pci");      // the AF_XDP PMD is a vdev; no PCI probe
    args.emplace_back("--in-memory");   // no lock files => 2 af_xdp primaries per host
    for (const auto& v : g_vdevs) {
      args.emplace_back("--vdev");
      args.emplace_back(v);
    }
  } else {
    for (const auto& b : g_allowedBdfs) {
      args.emplace_back("-a");
      args.emplace_back(b);
    }
  }

  std::vector<char*> argv;
  argv.reserve(args.size() + 1);
  for (auto& a : args) argv.push_back(a.data());
  argv.push_back(nullptr);

  // EAL derives the control cpuset from the CALLER's affinity minus the dataplane
  // lcores. Narrow ourselves to kControlThreadCore across init, then restore our pin.
  const bool park = (kControlThreadCore != lcore);
  cpu_set_t savedAff;
  CPU_ZERO(&savedAff);
  bool haveSaved = false;
  if (park) {
    haveSaved = (pthread_getaffinity_np(pthread_self(), sizeof(savedAff), &savedAff) == 0);
    cpu_set_t ctrlSet;
    CPU_ZERO(&ctrlSet);
    CPU_SET(kControlThreadCore, &ctrlSet);
    pthread_setaffinity_np(pthread_self(), sizeof(ctrlSet), &ctrlSet);
  }

  const int rc = rte_eal_init(static_cast<int>(args.size()), argv.data());

  if (park && haveSaved)
    pthread_setaffinity_np(pthread_self(), sizeof(savedAff), &savedAff);

  if (rc < 0) {
    std::fprintf(stderr, "[DPDK] rte_eal_init failed: %s\n", rte_strerror(rte_errno));
    return false;
  }
  g_ealInited = true;
  return true;
}

bool DPDKEal::writeSysfs(const std::string& path, std::string_view val) noexcept {
  const int fd = ::open(path.c_str(), O_WRONLY | O_CLOEXEC);
  if (fd < 0) return false;
  const ssize_t n = ::write(fd, val.data(), val.size());
  ::close(fd);
  return n == static_cast<ssize_t>(val.size());
}

// driver_override + drivers_probe, exactly as dpdk-devbind does it.
bool DPDKEal::bindToVfio(const std::string& bdf) noexcept {
  if (::access("/sys/bus/pci/drivers/vfio-pci", F_OK) != 0) {
    // Best-effort: the module may be built-in, or modprobe may be absent. The real
    // verdict is the drivers_probe below, so the exit status is only logged.
    const int rc = std::system("modprobe vfio-pci");
    if (rc != 0)
      std::fprintf(stderr, "[DPDK] modprobe vfio-pci returned %d (continuing)\n", rc);
  }
  if (pci::currentDriver(bdf) == "vfio-pci") return true;
  const std::string dev = "/sys/bus/pci/devices/" + bdf;
  (void)writeSysfs(dev + "/driver_override", "vfio-pci");
  (void)writeSysfs(dev + "/driver/unbind", bdf);
  return writeSysfs("/sys/bus/pci/drivers_probe", bdf);
}
