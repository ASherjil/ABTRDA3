#pragma once

enum class NicTunerMode { Off, NfsSafe, Full };

/// Applies low-latency NIC and kernel tuning on construction.
/// Settings are non-persistent (revert on reboot). Requires root.
///
/// Modes:
///   Full    — aggressive tuning for dedicated interfaces. Pins NIC IRQs to
///             app core, disables GRO/GSO/TSO, steers all RSS to queue 0.
///   NfsSafe — isolates app core without breaking NFS. Pins NIC IRQs to a
///             dedicated irqCore with boosted ksoftirqd. Disables GRO/GSO/TSO
///             (safe with dedicated IRQ core handling the increased packet rate).
///   Off     — no tuning applied.
class NicTuner {
public:
    NicTuner(const char* interface, int cpuCore, NicTunerMode mode, int irqCore = -1);
    ~NicTuner();

    NicTuner(const NicTuner&)            = delete;
    NicTuner& operator=(const NicTuner&) = delete;

private:
    int m_ethtoolFd = -1;
    int m_dmaLatencyFd = -1;
};
