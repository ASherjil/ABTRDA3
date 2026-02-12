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

} // namespace

// =============================================================================
// Constructor — apply all tuning (idempotent, no-op if already applied)
// =============================================================================

NicTuner::NicTuner(const char* interface, int cpuCore)
{
    // --- 0. Stop irqbalance ---
    if (std::system("systemctl stop irqbalance 2>/dev/null") == 0)
        std::fprintf(stderr, "[NicTuner] irqbalance stopped\n");

    m_ethtoolFd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (m_ethtoolFd < 0)
        std::fprintf(stderr, "[NicTuner] Warning: socket() failed: %s\n", std::strerror(errno));

    char path[128];

    // --- 1. Interrupt coalescing: zero delay ---
    if (m_ethtoolFd >= 0) {
        ethtool_coalesce ec{};
        ec.cmd = ETHTOOL_GCOALESCE;
        if (ethtoolIoctl(m_ethtoolFd, interface, &ec)) {
            auto oldRx = ec.rx_coalesce_usecs;
            auto oldTx = ec.tx_coalesce_usecs;
            ec.cmd = ETHTOOL_SCOALESCE;
            ec.rx_coalesce_usecs = 0;
            ec.tx_coalesce_usecs = 0;
            if (ethtoolIoctl(m_ethtoolFd, interface, &ec))
                std::fprintf(stderr, "[NicTuner] rx-usecs: %u -> 0, tx-usecs: %u -> 0\n",
                             oldRx, oldTx);
        }

        // --- 2. NIC offloads: disable GRO, GSO, TSO ---
        auto disable = [&](std::uint32_t get, std::uint32_t set, const char* name) {
            ethtool_value ev{};
            ev.cmd = get;
            if (ethtoolIoctl(m_ethtoolFd, interface, &ev) && ev.data != 0) {
                ev.cmd = set;
                ev.data = 0;
                if (ethtoolIoctl(m_ethtoolFd, interface, &ev))
                    std::fprintf(stderr, "[NicTuner] %s: on -> off\n", name);
            }
        };
        disable(ETHTOOL_GGRO, ETHTOOL_SGRO, "GRO");
        disable(ETHTOOL_GGSO, ETHTOOL_SGSO, "GSO");
        disable(ETHTOOL_GTSO, ETHTOOL_STSO, "TSO");
    }

    // --- 3. RT throttling: disable 50ms forced sleep ---
    int oldRt = 0;
    auto s = readFile("/proc/sys/kernel/sched_rt_runtime_us");
    if (!s.empty()) oldRt = std::atoi(s.c_str());
    if (writeInt("/proc/sys/kernel/sched_rt_runtime_us", -1))
        std::fprintf(stderr, "[NicTuner] sched_rt_runtime_us: %d -> -1\n", oldRt);

    // --- 4. Raise ksoftirqd on target core to SCHED_FIFO:50 ---
    char ksoftName[32];
    std::snprintf(ksoftName, sizeof(ksoftName), "ksoftirqd/%d", cpuCore);
    int kpid = findPidByComm(ksoftName);
    if (kpid > 0) {
        sched_param sp{};
        int oldPrio = 0;
        sched_getparam(kpid, &sp);
        oldPrio = sp.sched_priority;
        sp.sched_priority = 50;
        if (sched_setscheduler(kpid, SCHED_FIFO, &sp) == 0)
            std::fprintf(stderr, "[NicTuner] %s (pid %d): prio %d -> FIFO:50\n",
                         ksoftName, kpid, oldPrio);
    }

    // --- 5. Pin NIC IRQs to target core, move everything else off ---
    auto nicIrqs = findNicIrqs(interface);
    auto isNicIrq = [&](int irq) {
        for (int n : nicIrqs) if (n == irq) return true;
        return false;
    };

    // Pin NIC IRQ(s) to target core
    char coreStr[8];
    std::snprintf(coreStr, sizeof(coreStr), "%d", cpuCore);
    for (int irq : nicIrqs) {
        std::snprintf(path, sizeof(path), "/proc/irq/%d/smp_affinity_list", irq);
        if (writeFile(path, coreStr))
            std::fprintf(stderr, "[NicTuner] IRQ %d (%s) pinned to core %d\n",
                         irq, interface, cpuCore);
    }

    // Move all other IRQs off target core
    std::string mask = cpuListExcluding(cpuCore);
    int moved = 0;
    DIR* irqDir = ::opendir("/proc/irq");
    if (irqDir) {
        while (auto* entry = ::readdir(irqDir)) {
            if (entry->d_name[0] < '0' || entry->d_name[0] > '9') continue;
            int irq = std::atoi(entry->d_name);
            if (irq == 0 || isNicIrq(irq)) continue;
            std::snprintf(path, sizeof(path), "/proc/irq/%d/smp_affinity_list", irq);
            if (writeFile(path, mask.c_str())) moved++;
        }
        ::closedir(irqDir);
    }
    if (moved > 0)
        std::fprintf(stderr, "[NicTuner] Moved %d other IRQs off core %d\n", moved, cpuCore);

    std::fprintf(stderr, "[NicTuner] Tuning applied for %s on core %d\n", interface, cpuCore);
}

// =============================================================================
// Destructor — settings are non-persistent (revert on reboot)
// =============================================================================

NicTuner::~NicTuner() {
    if (m_ethtoolFd >= 0) ::close(m_ethtoolFd);
    std::fprintf(stderr, "[NicTuner] Done. Settings persist until reboot.\n");
}
