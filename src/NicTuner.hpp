#pragma once

/// Applies low-latency NIC and kernel tuning on construction.
/// Settings are non-persistent (revert on reboot). Requires root.
class NicTuner {
public:
    NicTuner(const char* interface, int cpuCore);
    ~NicTuner();

    NicTuner(const NicTuner&)            = delete;
    NicTuner& operator=(const NicTuner&) = delete;

private:
    int m_ethtoolFd = -1;
    int m_dmaLatencyFd = -1;  // /dev/cpu_dma_latency — held open to keep C-states disabled
};
