// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Areeb Sherjil

#include "NicTuner.hpp"

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <dirent.h>
#include <fcntl.h>
#include <linux/ethtool.h>
#include <linux/sockios.h>
#include <net/if.h>
#include <sched.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

// =============================================================================
// Helpers (file-local)
// =============================================================================

namespace {

bool writeFile(const char* path, const char* value) {
    const int fd = ::open(path, O_WRONLY | O_TRUNC);
    if (fd < 0) {
        return false;
    }
    const ssize_t n = ::write(fd, value, std::strlen(value));
    ::close(fd);
    return n > 0;
}

bool writeInt(const char* path, int value) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%d", value);
    return writeFile(path, buf);
}

std::string readFile(const char* path) {
    const int fd = ::open(path, O_RDONLY);
    if (fd < 0) {
        return {};
    }
    char          buf[256];
    const ssize_t n = ::read(fd, buf, sizeof(buf) - 1);
    ::close(fd);
    if (n <= 0) {
        return {};
    }
    buf[n] = '\0';
    if (buf[n - 1] == '\n') {
        buf[n - 1] = '\0';
    }
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
    if (!dir) {
        return -1;
    }
    while (auto* entry = ::readdir(dir)) {
        if (entry->d_name[0] < '0' || entry->d_name[0] > '9') {
            continue;
        }
        char path[64];
        std::snprintf(path, sizeof(path), "/proc/%s/comm", entry->d_name);
        if (readFile(path) == name) {
            const int pid = std::atoi(entry->d_name);
            ::closedir(dir);
            return pid;
        }
    }
    ::closedir(dir);
    return -1;
}

std::vector<int> findNicIrqs(const char* iface) {
    std::vector<int> irqs;
    FILE*            f = std::fopen("/proc/interrupts", "r");
    if (!f) {
        return irqs;
    }
    char line[512];
    while (std::fgets(line, sizeof(line), f)) {
        if (std::strstr(line, iface)) {
            const int irq = std::atoi(line);
            if (irq > 0) {
                irqs.push_back(irq);
            }
        }
    }
    std::fclose(f);
    return irqs;
}

int findSshdMasterPid() {
    DIR* dir = ::opendir("/proc");
    if (!dir) {
        return -1;
    }
    while (auto* entry = ::readdir(dir)) {
        if (entry->d_name[0] < '0' || entry->d_name[0] > '9') {
            continue;
        }
        const int pid = std::atoi(entry->d_name);
        if (pid <= 1) {
            continue;
        }

        char path[64];
        std::snprintf(path, sizeof(path), "/proc/%s/comm", entry->d_name);
        if (readFile(path) != "sshd") {
            continue;
        }

        // Master sshd has ppid=1 (systemd)
        std::snprintf(path, sizeof(path), "/proc/%s/stat", entry->d_name);
        auto stat = readFile(path);
        if (stat.empty()) {
            continue;
        }
        auto* closeParen = std::strrchr(stat.c_str(), ')');
        if (!closeParen) {
            continue;
        }
        int ppid = 0;
        if (std::sscanf(closeParen + 2, "%*c %d", &ppid) != 1) {
            continue;
        }
        if (ppid == 1) {
            ::closedir(dir);
            return pid;
        }
    }
    ::closedir(dir);
    return -1;
}

std::string cpuListExcluding(int core) {
    const long  nproc = ::sysconf(_SC_NPROCESSORS_ONLN);
    std::string result;
    for (int i = 0; i < nproc; ++i) {
        if (i == core) {
            continue;
        }
        if (!result.empty()) {
            result += ',';
        }
        result += std::to_string(i);
    }
    return result;
}

int migrateKernelThreads() {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(0, &cpuset);

    int  moved = 0;
    DIR* dir   = ::opendir("/proc");
    if (!dir) {
        return 0;
    }
    while (auto* entry = ::readdir(dir)) {
        if (entry->d_name[0] < '0' || entry->d_name[0] > '9') {
            continue;
        }
        const int pid = std::atoi(entry->d_name);
        if (pid <= 2) {
            continue;
        }

        char path[64];
        std::snprintf(path, sizeof(path), "/proc/%d/stat", pid);
        auto stat = readFile(path);
        if (stat.empty()) {
            continue;
        }

        auto* closeParen = std::strrchr(stat.c_str(), ')');
        if (!closeParen) {
            continue;
        }
        int ppid = 0;
        if (std::sscanf(closeParen + 2, "%*c %d", &ppid) != 1) {
            continue;
        }
        if (ppid != 2) {
            continue;
        }

        if (::sched_setaffinity(pid, sizeof(cpuset), &cpuset) == 0) {
            moved++;
        }
    }
    ::closedir(dir);
    return moved;
}

int migrateWorkqueues() {
    int  moved = 0;
    DIR* dir   = ::opendir("/sys/devices/virtual/workqueue");
    if (!dir) {
        return 0;
    }
    while (auto* entry = ::readdir(dir)) {
        if (entry->d_name[0] == '.') {
            continue;
        }
        char path[256];
        std::snprintf(path, sizeof(path), "/sys/devices/virtual/workqueue/%s/cpumask", entry->d_name);
        if (writeFile(path, "1")) {
            moved++;
        }
    }
    ::closedir(dir);
    return moved;
}

}   // namespace

// =============================================================================
// Constructor
// =============================================================================

NicTuner::NicTuner(const char* interface, int cpuCore, NicTunerMode mode) {
    if (mode == NicTunerMode::Off) {
        std::fprintf(stderr, "[NicTuner] Off\n");
        return;
    }

    const bool nfsSafe = (mode == NicTunerMode::NfsSafe);

    // ── Common to both modes ────────────────────────────────────────────

    std::system("systemctl stop irqbalance 2>/dev/null");

    if (migrateKernelThreads() == 0) {
        std::fprintf(stderr, "[NicTuner] FAIL: could not migrate any kernel threads to core 0\n");
    }

    if (migrateWorkqueues() == 0) {
        std::fprintf(stderr, "[NicTuner] FAIL: could not redirect any workqueues to core 0\n");
    }

    m_ethtoolFd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (m_ethtoolFd < 0) {
        std::fprintf(stderr, "[NicTuner] FAIL: socket() for ethtool: %s\n", std::strerror(errno));
    }

    char path[128];

    std::snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_governor", cpuCore);
    if (!writeFile(path, "performance")) {
        std::fprintf(stderr, "[NicTuner] FAIL: set governor on CPU%d\n", cpuCore);
    }

    writeFile("/proc/sys/net/core/busy_poll", "50");
    writeFile("/proc/sys/net/core/busy_read", "50");

    if (!writeFile("/proc/sys/vm/stat_interval", "120")) {
        std::fprintf(stderr, "[NicTuner] FAIL: set vm.stat_interval\n");
    }

    // NOTE: interrupt coalescing is intentionally NOT set here. AF_XDP's
    // xsk_socket__create() resets the NIC's coalescing, so any value applied
    // pre-bind is wiped out. The transport calls NicTuner::setCoalescingZero()
    // AFTER the socket is bound instead.
    if (m_ethtoolFd >= 0) {
        // Disable GRO/GSO/TSO — reduces per-packet latency.
        auto disable = [&](std::uint32_t get, std::uint32_t set, const char* name) {
            ethtool_value ev{};
            ev.cmd = get;
            if (ethtoolIoctl(m_ethtoolFd, interface, &ev) && ev.data != 0) {
                ev.cmd  = set;
                ev.data = 0;
                if (!ethtoolIoctl(m_ethtoolFd, interface, &ev)) {
                    std::fprintf(stderr, "[NicTuner] FAIL: disable %s\n", name);
                }
            }
        };
        disable(ETHTOOL_GGRO, ETHTOOL_SGRO, "GRO");
        disable(ETHTOOL_GGSO, ETHTOOL_SGSO, "GSO");
        disable(ETHTOOL_GTSO, ETHTOOL_STSO, "TSO");
    }

    if (!writeInt("/proc/sys/kernel/sched_rt_runtime_us", -1)) {
        std::fprintf(stderr, "[NicTuner] FAIL: disable RT throttling\n");
    }

    // Boost ksoftirqd on the APP core so it can preempt our FIFO:49 app
    // to deliver timing packets from the mmap ring.
    char ksoftName[32];
    std::snprintf(ksoftName, sizeof(ksoftName), "ksoftirqd/%d", cpuCore);
    const int kpid = findPidByComm(ksoftName);
    if (kpid > 0) {
        sched_param sp{};
        sp.sched_priority = 50;
        if (sched_setscheduler(kpid, SCHED_FIFO, &sp) != 0) {
            std::fprintf(stderr, "[NicTuner] FAIL: ksoftirqd/%d SCHED_FIFO:50: %s\n", cpuCore,
                         std::strerror(errno));
        }
    } else {
        std::fprintf(stderr, "[NicTuner] FAIL: ksoftirqd/%d not found\n", cpuCore);
    }

    // ── Core isolation via tuna (more thorough than manual kthread migration) ──
    // Moves ALL threads (kernel + user + timers) off the app core.
    // Safe because SSH is already boosted to SCHED_RR:1 above.
    {
        char cmd[64];
        std::snprintf(cmd, sizeof(cmd), "tuna --cpus=%d --isolate 2>/dev/null", cpuCore);
        if (std::system(cmd) == 0) {
            std::fprintf(stderr, "[NicTuner] Core %d isolated via tuna\n", cpuCore);
        }
    }

    // Disable kernel watchdog on the app core to prevent NMI jitter
    writeInt("/proc/sys/kernel/watchdog", 0);

    // ── Single RX queue steering MOVED to AFXDP ─────────────────────────
    // `ethtool -L <if> combined 1` (collapse to one RX queue so all RX lands on
    // queue 0, the XSK's queue) is now done by AFXDP::init() just before the
    // socket bind — see AFXDP.hpp steerAllTrafficToQueue(). It belongs with the
    // transport that owns the queue and the bind, and must run right before the
    // bind (it reprograms the NIC's queues). NOTE: because that now runs AFTER
    // NicTuner, the IRQ pinning below operates on the PRE-collapse IRQ set — the
    // surviving queue-0 IRQ stays pinned, but if RX reliability ever regresses,
    // suspect a renumbered post-collapse IRQ.

    // ── IRQ handling ─────────────────────────────────────────────────────
    // Pin NIC IRQs TO the app core (same core = hot cache),
    // move everything else OFF.

    auto nicIrqs = findNicIrqs(interface);
    if (nicIrqs.empty()) {
        std::fprintf(stderr, "[NicTuner] FAIL: no IRQs found for %s\n", interface);
    }

    auto isNicIrq = [&](int irq) {
        for (const int n : nicIrqs) {
            if (n == irq) {
                return true;
            }
        }
        return false;
    };

    char coreStr[8];
    std::snprintf(coreStr, sizeof(coreStr), "%d", cpuCore);
    for (const int irq : nicIrqs) {
        std::snprintf(path, sizeof(path), "/proc/irq/%d/smp_affinity_list", irq);
        if (!writeFile(path, coreStr)) {
            std::fprintf(stderr, "[NicTuner] FAIL: pin IRQ %d to core %d\n", irq, cpuCore);
        }
    }

    const std::string mask   = cpuListExcluding(cpuCore);
    DIR*              irqDir = ::opendir("/proc/irq");
    if (irqDir) {
        while (auto* entry = ::readdir(irqDir)) {
            if (entry->d_name[0] < '0' || entry->d_name[0] > '9') {
                continue;
            }
            const int irq = std::atoi(entry->d_name);
            if (irq == 0 || isNicIrq(irq)) {
                continue;
            }
            std::snprintf(path, sizeof(path), "/proc/irq/%d/smp_affinity_list", irq);
            writeFile(path, mask.c_str());
        }
        ::closedir(irqDir);
    }

    // ── NfsSafe: boost sshd so SSH survives RT starvation ──────────────
    // The master sshd (ppid=1) is boosted to SCHED_RR:1. Forked children
    // (new SSH sessions) inherit the policy automatically.

    if (nfsSafe) {
        const int sshdPid = findSshdMasterPid();
        if (sshdPid > 0) {
            sched_param sp{};
            sp.sched_priority = 1;
            if (::sched_setscheduler(sshdPid, SCHED_RR, &sp) == 0) {
                std::fprintf(stderr, "[NicTuner] sshd (pid %d) → SCHED_RR:1\n", sshdPid);
            } else {
                std::fprintf(stderr, "[NicTuner] FAIL: sshd SCHED_RR:1: %s\n", std::strerror(errno));
            }
        } else {
            std::fprintf(stderr, "[NicTuner] WARN: sshd master not found, SSH may be unresponsive\n");
        }
    }

    // ── Summary ─────────────────────────────────────────────────────────

    std::fprintf(stderr, "[NicTuner] Applied (%s) for %s on core %d\n", nfsSafe ? "nfs_safe" : "full",
                 interface, cpuCore);
}

// =============================================================================
// Post-bind coalescing (called AFTER xsk_socket__create resets it)
// =============================================================================

bool NicTuner::setCoalescingZero(const char* interface) {
    const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        std::fprintf(stderr, "[NicTuner] FAIL: coalescing socket: %s\n", std::strerror(errno));
        return false;
    }
    // GET first so unsupported fields keep their driver defaults; then turn
    // adaptive ITR OFF and zero the usecs (== `ethtool -C <if> adaptive-rx off
    // adaptive-tx off rx-usecs 0 tx-usecs 0`). The previous code never cleared
    // use_adaptive_*_coalesce, so adaptive ITR overrode the zeros and the SET
    // was rejected — hence the "FAIL: set interrupt coalescing to 0" log.
    ethtool_coalesce ec{};
    ec.cmd = ETHTOOL_GCOALESCE;
    if (ethtoolIoctl(fd, interface, &ec)) {
        ec.cmd                      = ETHTOOL_SCOALESCE;
        ec.use_adaptive_rx_coalesce = 0;
        ec.use_adaptive_tx_coalesce = 0;
        ec.rx_coalesce_usecs        = 0;
        ec.tx_coalesce_usecs        = 0;
        ethtoolIoctl(fd, interface, &ec);
    }

    // Verify by read-back: adaptive ITR must be OFF and the usecs must be 0,
    // otherwise the NIC still batches interrupts and latency stays high.
    ethtool_coalesce rb{};
    rb.cmd        = ETHTOOL_GCOALESCE;
    const bool ok = ethtoolIoctl(fd, interface, &rb) && rb.use_adaptive_rx_coalesce == 0 &&
                    rb.use_adaptive_tx_coalesce == 0 && rb.rx_coalesce_usecs == 0 &&
                    rb.tx_coalesce_usecs == 0;
    ::close(fd);
    if (ok) {
        std::fprintf(stderr, "[NicTuner] OK: %s coalescing off (adaptive off, rx/tx-usecs 0)\n", interface);
    } else {
        std::fprintf(stderr,
                     "[NicTuner] FAIL: %s coalescing not zeroed (adaptive rx=%u tx=%u, "
                     "rx-usecs=%u tx-usecs=%u) — run: sudo ethtool -C %s adaptive-rx off "
                     "adaptive-tx off rx-usecs 0 tx-usecs 0\n",
                     interface, rb.use_adaptive_rx_coalesce, rb.use_adaptive_tx_coalesce,
                     rb.rx_coalesce_usecs, rb.tx_coalesce_usecs, interface);
    }
    return ok;
}

// =============================================================================
// Destructor
// =============================================================================

NicTuner::~NicTuner() {
    if (m_ethtoolFd >= 0) {
        ::close(m_ethtoolFd);
    }
}
