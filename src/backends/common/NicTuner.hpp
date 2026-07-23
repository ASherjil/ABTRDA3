// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Areeb Sherjil

#pragma once

enum class NicTunerMode { Off, NfsSafe, Full };

/// Applies low-latency NIC and kernel tuning on construction.
/// Settings are non-persistent (revert on reboot). Requires root.
///
/// Modes:
///   Full    — aggressive tuning for dedicated interfaces. Pins NIC IRQs to
///             app core, disables GRO/GSO/TSO, steers all RSS to queue 0.
///   NfsSafe — same tuning as Full, plus boosts the sshd master process to
///             SCHED_RR:1 so SSH sessions survive RT starvation on shared
///             interfaces (PXE boot systems where NFS is just a file share).
///   Off     — no tuning applied.
class NicTuner {
public:
    NicTuner(const char* interface, int cpuCore, NicTunerMode mode);
    ~NicTuner();

    NicTuner(const NicTuner&)            = delete;
    NicTuner& operator=(const NicTuner&) = delete;

    // Disable interrupt coalescing (adaptive off, rx/tx-usecs 0).
    // MUST be called AFTER the AF_XDP socket is bound: xsk_socket__create()
    // reprograms the NIC and resets coalescing, so applying it during NicTuner
    // construction (which runs before the bind) has no lasting effect. Static so
    // the transport can call it post-bind without holding the NicTuner instance.
    static bool setCoalescingZero(const char* interface);

private:
    int m_ethtoolFd = -1;
};
