//
// PciHelpers — generic PCI / sysfs lookups shared across transports.
//
// resolveBdf(): map a kernel network interface name (e.g. "enp1s0f1np1") to its
// PCI domain:bus:device.function id (e.g. "0000:01:00.1"). The DPDK transport
// addresses devices by BDF, so it resolves the name WHILE the netdev still
// exists — before the device is unbound from the kernel driver onto vfio-pci,
// which removes the interface name.
//
// (Intel_I210 / Cadence_GEM do NOT use this: they reach the PCIe / AXI bus
// through the ABTEdge backend, which handles bus access for them.)
//
#pragma once

#include <fcntl.h>
#include <unistd.h>

#include <cstddef>
#include <cstdio>
#include <string>
#include <string_view>

namespace pci {

// basename of a sysfs symlink target (".../0000:01:00.1" -> "0000:01:00.1").
// Empty if the path is not a symlink / does not exist.
[[nodiscard]] inline std::string readlinkBasename(const std::string& path) noexcept {
  char buf[256];
  const ssize_t n = ::readlink(path.c_str(), buf, sizeof(buf) - 1);
  if (n <= 0) return {};
  buf[n] = '\0';
  std::string s(buf);
  const auto pos = s.find_last_of('/');
  return pos == std::string::npos ? s : s.substr(pos + 1);
}

// Interface name -> PCI BDF, via /sys/class/net/<if>/device. Empty if the netdev
// doesn't exist (e.g. already bound to vfio-pci) or isn't backed by a PCI device.
[[nodiscard]] inline std::string resolveBdf(std::string_view ifname) noexcept {
  return readlinkBasename("/sys/class/net/" + std::string(ifname) + "/device");
}

// The driver currently bound to a PCI device (basename of its driver symlink),
// or empty if the device is bound to no driver.
[[nodiscard]] inline std::string currentDriver(std::string_view bdf) noexcept {
  return readlinkBasename("/sys/bus/pci/devices/" + std::string(bdf) + "/driver");
}

// Size of a PCI BAR (in bytes) for the given device, read from
// /sys/bus/pci/devices/<bdf>/resource. The file has one line per BAR:
//   0xSTART 0xEND 0xFLAGS
// Size = END - START + 1, or 0 if the BAR is unused. Returns 0 on any error.
[[nodiscard]] inline std::size_t barSize(std::string_view bdf, int barIndex) noexcept {
  std::string path = "/sys/bus/pci/devices/" + std::string(bdf) + "/resource";
  std::FILE* f = std::fopen(path.c_str(), "r");
  if (!f) return 0;
  char line[160]{};
  for (int i = 0; i <= barIndex; ++i) {
    if (!std::fgets(line, sizeof(line), f)) { std::fclose(f); return 0; }
  }
  std::fclose(f);
  unsigned long long start = 0, end = 0;
  if (std::sscanf(line, "%llx %llx", &start, &end) == 2 && end >= start && start != 0)
    return static_cast<std::size_t>(end - start + 1ULL);
  return 0;
}

// Derive a PCI BDF from a systemd "predictable" interface name
// (enp<bus>s<dev>[f<func>]), which is itself assigned from the firmware/BIOS PCI
// location. This resolves the BDF even AFTER the kernel driver is unbound (the
// netdev is gone, so resolveBdf() can't help). The numbers in the name are
// DECIMAL (e.g. 0000:3b:00.0 -> enp59s0); we convert to the hex BDF and verify a
// matching PCI device exists. Empty if the name isn't the enp form or no device
// matches.
[[nodiscard]] inline std::string bdfFromName(std::string_view ifname) noexcept {
  if (ifname.size() < 5 || ifname[0] != 'e' || ifname[1] != 'n' || ifname[2] != 'p')
    return {};
  std::size_t i = 3;
  auto readDec = [&](unsigned& out) -> bool {
    const std::size_t start = i;
    out = 0;
    while (i < ifname.size() && ifname[i] >= '0' && ifname[i] <= '9')
      out = out * 10u + static_cast<unsigned>(ifname[i++] - '0');
    return i > start;
  };
  unsigned bus = 0, dev = 0, func = 0;
  if (!readDec(bus)) return {};
  if (i >= ifname.size() || ifname[i] != 's') return {};
  ++i;
  if (!readDec(dev)) return {};
  if (i < ifname.size() && ifname[i] == 'f') { ++i; if (!readDec(func)) return {}; }

  char bdf[16];
  std::snprintf(bdf, sizeof(bdf), "0000:%02x:%02x.%x",
                bus & 0xffu, dev & 0xffu, func & 0xfu);
  if (::access((std::string("/sys/bus/pci/devices/") + bdf).c_str(), F_OK) != 0)
    return {};
  return bdf;
}

}  // namespace pci
