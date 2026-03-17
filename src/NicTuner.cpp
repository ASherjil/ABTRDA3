#include "NicTuner.hpp"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <dirent.h>
#include <fcntl.h>
#include <sched.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <linux/ethtool.h>
#include <linux/sockios.h>
#include <net/if.h>

// =============================================================================
// Helpers (file-local)
// =============================================================================

namespace {

bool writeFile(const char* path, const char* value) {
    int fd = ::open(path, O_WRONLY | O_TRUNC);
    if (fd < 0) return false;
    ssize_t n = ::write(fd, value, std::strlen(value));
    ::close(fd);
    return n > 0;
}

bool writeInt(const char* path, int value) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%d", value);
    return writeFile(path, buf);
}

std::string readFile(const char* path) {
    int fd = ::open(path, O_RDONLY);
    if (fd < 0) return {};
    char buf[256];
    ssize_t n = ::read(fd, buf, sizeof(buf) - 1);
    ::close(fd);
    if (n <= 0) return {};
    buf[n] = '\0';
    if (buf[n - 1] == '\n') buf[n - 1] = '\0';
    return buf;
}

bool ethtoolIoctl(int fd, const char* iface, void* cmd) {
    ifreq ifr{};
    std::strncpy(ifr.ifr_name, iface, IFNAMSIZ - 1);
    ifr.ifr_data = static_cast<char*>(cmd);
    return ::ioctl(fd, SIOCETHTOOL, &ifr) == 0;
}

int findPidByComm(const char* name) {
    DIR* dir = ::opendir("/proc");
    if (!dir) return -1;
    while (auto* entry = ::readdir(dir)) {
        if (entry->d_name[0] < '0' || entry->d_name[0] > '9') continue;
        char path[64];
        std::snprintf(path, sizeof(path), "/proc/%s/comm", entry->d_name);
        if (readFile(path) == name) {
            int pid = std::atoi(entry->d_name);
            ::closedir(dir);
            return pid;
        }
    }
    ::closedir(dir);
    return -1;
}

std::vector<int> findNicIrqs(const char* iface) {
    std::vector<int> irqs;
    FILE* f = std::fopen("/proc/interrupts", "r");
    if (!f) return irqs;
    char line[512];
    while (std::fgets(line, sizeof(line), f)) {
        if (std::strstr(line, iface)) {
            int irq = std::atoi(line);
            if (irq > 0) irqs.push_back(irq);
        }
    }
    std::fclose(f);
    return irqs;
}

std::string cpuListExcluding(int core) {
    long nproc = ::sysconf(_SC_NPROCESSORS_ONLN);
    std::string result;
    for (int i = 0; i < nproc; ++i) {
        if (i == core) continue;
        if (!result.empty()) result += ',';
        result += std::to_string(i);
    }
    return result;
}

// Move all kernel threads (children of kthreadd, PID 2) to core 0.
int migrateKernelThreads() {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(0, &cpuset);

    int moved = 0;
    DIR* dir = ::opendir("/proc");
    if (!dir) return 0;
    while (auto* entry = ::readdir(dir)) {
        if (entry->d_name[0] < '0' || entry->d_name[0] > '9') continue;
        int pid = std::atoi(entry->d_name);
        if (pid <= 2) continue;

        char path[64];
        std::snprintf(path, sizeof(path), "/proc/%d/stat", pid);
        auto stat = readFile(path);
        if (stat.empty()) continue;

        auto* closeParen = std::strrchr(stat.c_str(), ')');
        if (!closeParen) continue;
        int ppid = 0;
        if (std::sscanf(closeParen + 2, "%*c %d", &ppid) != 1) continue;
        if (ppid != 2) continue;

        if (::sched_setaffinity(pid, sizeof(cpuset), &cpuset) == 0)
            moved++;
    }
    ::closedir(dir);
    return moved;
}

// Redirect all kernel workqueues to core 0 (cpumask 0x1).
int migrateWorkqueues() {
    int moved = 0;
    DIR* dir = ::opendir("/sys/devices/virtual/workqueue");
    if (!dir) return 0;
    while (auto* entry = ::readdir(dir)) {
        if (entry->d_name[0] == '.') continue;
        char path[256];
        std::snprintf(path, sizeof(path),
                      "/sys/devices/virtual/workqueue/%s/cpumask", entry->d_name);
        if (writeFile(path, "1"))
            moved++;
    }
    ::closedir(dir);
    return moved;
}

} // namespace

// =============================================================================
// Constructor — apply all tuning (idempotent, no-op if already applied)
// =============================================================================

NicTuner::NicTuner(const char* interface, int cpuCore)
{
    // --- 0. Stop irqbalance ---
    std::system("systemctl stop irqbalance 2>/dev/null");

    // --- 0a. Move all kernel threads to core 0 ---
    if (migrateKernelThreads() == 0)
        std::fprintf(stderr, "[NicTuner] FAIL: could not migrate any kernel threads to core 0\n");

    // --- 0b. Move all kernel workqueues to core 0 ---
    if (migrateWorkqueues() == 0)
        std::fprintf(stderr, "[NicTuner] FAIL: could not redirect any workqueues to core 0\n");

    m_ethtoolFd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (m_ethtoolFd < 0)
        std::fprintf(stderr, "[NicTuner] FAIL: socket() for ethtool: %s\n", std::strerror(errno));

    char path[128];

    // --- 0c. Hold /dev/cpu_dma_latency at 0 to prevent deep C-states ---
    m_dmaLatencyFd = ::open("/dev/cpu_dma_latency", O_WRONLY);
    if (m_dmaLatencyFd >= 0) {
        std::int32_t lat = 0;
        if (::write(m_dmaLatencyFd, &lat, sizeof(lat)) != sizeof(lat))
            std::fprintf(stderr, "[NicTuner] FAIL: cpu_dma_latency write\n");
    } else {
        std::fprintf(stderr, "[NicTuner] FAIL: open /dev/cpu_dma_latency: %s\n", std::strerror(errno));
    }

    // --- 0d. CPU frequency governor → performance ---
    std::snprintf(path, sizeof(path),
                  "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_governor", cpuCore);
    if (!writeFile(path, "performance"))
        std::fprintf(stderr, "[NicTuner] FAIL: set governor on CPU%d\n", cpuCore);

    // --- 0e. Kernel busy-poll ---
    if (!writeFile("/proc/sys/net/core/busy_poll", "50"))
        std::fprintf(stderr, "[NicTuner] FAIL: set net.core.busy_poll\n");
    if (!writeFile("/proc/sys/net/core/busy_read", "50"))
        std::fprintf(stderr, "[NicTuner] FAIL: set net.core.busy_read\n");

    // --- 0f. Reduce VM stat timer from 1s to 120s ---
    if (!writeFile("/proc/sys/vm/stat_interval", "120"))
        std::fprintf(stderr, "[NicTuner] FAIL: set vm.stat_interval\n");

    // --- 1. Interrupt coalescing: zero delay ---
    if (m_ethtoolFd >= 0) {
        ethtool_coalesce ec{};
        ec.cmd = ETHTOOL_GCOALESCE;
        if (ethtoolIoctl(m_ethtoolFd, interface, &ec)) {
            ec.cmd = ETHTOOL_SCOALESCE;
            ec.rx_coalesce_usecs = 0;
            ec.tx_coalesce_usecs = 0;
            if (!ethtoolIoctl(m_ethtoolFd, interface, &ec))
                std::fprintf(stderr, "[NicTuner] FAIL: set interrupt coalescing to 0\n");
        }

        // --- 2. NIC offloads: disable GRO, GSO, TSO ---
        auto disable = [&](std::uint32_t get, std::uint32_t set, const char* name) {
            ethtool_value ev{};
            ev.cmd = get;
            if (ethtoolIoctl(m_ethtoolFd, interface, &ev) && ev.data != 0) {
                ev.cmd = set;
                ev.data = 0;
                if (!ethtoolIoctl(m_ethtoolFd, interface, &ev))
                    std::fprintf(stderr, "[NicTuner] FAIL: disable %s\n", name);
            }
        };
        disable(ETHTOOL_GGRO, ETHTOOL_SGRO, "GRO");
        disable(ETHTOOL_GGSO, ETHTOOL_SGSO, "GSO");
        disable(ETHTOOL_GTSO, ETHTOOL_STSO, "TSO");

        // --- 2b. RSS: steer all RX traffic to queue 0 ---
        ethtool_rxnfc rxnfc{};
        rxnfc.cmd = ETHTOOL_GRXRINGS;
        std::uint32_t numQueues = 0;
        if (ethtoolIoctl(m_ethtoolFd, interface, &rxnfc))
            numQueues = static_cast<std::uint32_t>(rxnfc.data);

        if (numQueues > 0) {
            ethtool_rxfh_indir indirHdr{};
            indirHdr.cmd = ETHTOOL_GRXFHINDIR;
            indirHdr.size = 0;
            ethtoolIoctl(m_ethtoolFd, interface, &indirHdr);

            if (indirHdr.size > 0) {
                auto bytes = sizeof(ethtool_rxfh_indir) + indirHdr.size * sizeof(std::uint32_t);
                auto* indir = static_cast<ethtool_rxfh_indir*>(std::calloc(1, bytes));
                indir->cmd  = ETHTOOL_SRXFHINDIR;
                indir->size = indirHdr.size;
                if (!ethtoolIoctl(m_ethtoolFd, interface, indir))
                    std::fprintf(stderr, "[NicTuner] FAIL: set RSS indirection table\n");
                std::free(indir);
            }
        }
    }

    // --- 3. RT throttling: disable 50ms forced sleep ---
    if (!writeInt("/proc/sys/kernel/sched_rt_runtime_us", -1))
        std::fprintf(stderr, "[NicTuner] FAIL: disable RT throttling\n");

    // --- 4. Raise ksoftirqd on target core to SCHED_FIFO:50 ---
    char ksoftName[32];
    std::snprintf(ksoftName, sizeof(ksoftName), "ksoftirqd/%d", cpuCore);
    int kpid = findPidByComm(ksoftName);
    if (kpid > 0) {
        sched_param sp{};
        sp.sched_priority = 50;
        if (sched_setscheduler(kpid, SCHED_FIFO, &sp) != 0)
            std::fprintf(stderr, "[NicTuner] FAIL: ksoftirqd/%d SCHED_FIFO:50: %s\n",
                         cpuCore, std::strerror(errno));
    } else {
        std::fprintf(stderr, "[NicTuner] FAIL: ksoftirqd/%d not found\n", cpuCore);
    }

    // --- 5. Pin NIC IRQs to target core, move everything else off ---
    auto nicIrqs = findNicIrqs(interface);
    if (nicIrqs.empty())
        std::fprintf(stderr, "[NicTuner] FAIL: no IRQs found for %s\n", interface);

    char coreStr[8];
    std::snprintf(coreStr, sizeof(coreStr), "%d", cpuCore);
    for (int irq : nicIrqs) {
        std::snprintf(path, sizeof(path), "/proc/irq/%d/smp_affinity_list", irq);
        if (!writeFile(path, coreStr))
            std::fprintf(stderr, "[NicTuner] FAIL: pin IRQ %d to core %d\n", irq, cpuCore);
    }

    auto isNicIrq = [&](int irq) {
        for (int n : nicIrqs) if (n == irq) return true;
        return false;
    };
    std::string mask = cpuListExcluding(cpuCore);
    DIR* irqDir = ::opendir("/proc/irq");
    if (irqDir) {
        while (auto* entry = ::readdir(irqDir)) {
            if (entry->d_name[0] < '0' || entry->d_name[0] > '9') continue;
            int irq = std::atoi(entry->d_name);
            if (irq == 0 || isNicIrq(irq)) continue;
            std::snprintf(path, sizeof(path), "/proc/irq/%d/smp_affinity_list", irq);
            writeFile(path, mask.c_str());
        }
        ::closedir(irqDir);
    }

    // --- Summary ---
    std::fprintf(stderr, "[NicTuner] Applied for %s on core %d\n", interface, cpuCore);
}

// =============================================================================
// Destructor — settings are non-persistent (revert on reboot)
// =============================================================================

NicTuner::~NicTuner() {
    if (m_dmaLatencyFd >= 0) ::close(m_dmaLatencyFd);
    if (m_ethtoolFd >= 0) ::close(m_ethtoolFd);
}
