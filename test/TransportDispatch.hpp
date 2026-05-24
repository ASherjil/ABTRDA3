#pragma once

#include "TestConfig.hpp"
#include "Server.hpp"
#include "Client.hpp"
#include "TxGenerator.hpp"
#include "RxSink.hpp"
#include "SocketOps.hpp"
#include "PacketMmapRx.hpp"
#include "PacketMmapTx.hpp"
#include "AFXDP.hpp"
#include "common/HugePageHelpers.hpp"
#include "Intel_I210.hpp"
#include "Cadence_GEM.hpp"
#include "TapBridge.hpp"

#include <fmt/core.h>

#include <pthread.h>
#include <sched.h>

#include <stop_token>
#include <string>
#include <string_view>
#include <thread>

// =============================================================================
// Transport dispatch — creates the transport and runs the selected mode
// =============================================================================

enum class RunMode { Server, Client, TxGen, RxSink };

inline const char* runModeName(RunMode m) {
    switch (m) {
        case RunMode::Server: return "Server";
        case RunMode::Client: return "Client";
        case RunMode::TxGen:  return "TxGen";
        case RunMode::RxSink: return "RxSink";
    }
    return "Unknown";
}

// ── Dispatch a mode on concrete Tx/Rx objects ───────────────────────────────

template<TxRing Tx, RxRing Rx>
void dispatchMode(Tx& tx, Rx& rx, RunMode mode, const TestConfig& cfg,
                  std::uint32_t count, std::stop_token stop) {
    switch (mode) {
        case RunMode::Server: run_server(tx, rx, cfg, stop);           break;
        case RunMode::Client: run_client(tx, rx, cfg, count, stop);    break;
        case RunMode::TxGen:  run_txgen(tx, cfg, count, stop);         break;
        case RunMode::RxSink: run_rxsink(rx, cfg, stop);               break;
    }
}

// ── Spawn a low-priority thread that runs a TapBridge ──────────────────────
//
// The bridge itself MUST outlive the returned jthread, so the caller owns it.
// This helper just handles the thread attributes (demote from SCHED_FIFO,
// clear inherited CPU affinity) and spawns the jthread.
template<TxRing Tx, RxRing Rx>
[[nodiscard]]
std::jthread spawnTapDeviceThread(TapBridge<Tx, Rx>& tap) {
    auto t = std::jthread([&tap](std::stop_token st) {
        sched_param sp{};
        pthread_setschedparam(pthread_self(), SCHED_OTHER, &sp);

        cpu_set_t all;
        CPU_ZERO(&all);
        for (int c = 0; c < CPU_SETSIZE; ++c) CPU_SET(c, &all);
        pthread_setaffinity_np(pthread_self(), sizeof(all), &all);

        tap(st);
    });
    // The child inherits our SCHED_FIFO + CPU-1 pin from the runtime setup
    // and is non-preemptible at equal priority until it runs the
    // sched_setscheduler/sched_setaffinity prologue above.  Yield long
    // enough for it to execute that prologue and migrate to another core,
    // otherwise it never gets CPU during the hot test loop and Q0 fills up
    // un-drained.  Same trick used in startRxWatcher() during development.
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    return t;
}

// ── Transport creation + dispatch ───────────────────────────────────────────

[[nodiscard]]
inline int runTransport(const TestConfig& cfg, const RoleConfig& role,
                        RunMode mode, std::uint32_t count, std::stop_token stop) {

    const char* roleName = runModeName(mode);
    const std::string& transport = role.transport;

    if (transport == "packet_mmap") {
        RingConfig tx_cfg{};
        tx_cfg.interface     = role.interface.c_str();
        tx_cfg.direction     = RingDirection::TX;
        tx_cfg.blockSize     = cfg.mmapBlockSize;
        tx_cfg.blockNumber   = cfg.mmapBlockNumber;
        tx_cfg.protocol      = cfg.etherType;
        tx_cfg.packetVersion = TPACKET_V2;
        tx_cfg.qdiscBypass   = true;

        RingConfig rx_cfg{};
        rx_cfg.interface     = role.interface.c_str();
        rx_cfg.direction     = RingDirection::RX;
        rx_cfg.blockSize     = cfg.mmapBlockSize;
        rx_cfg.blockNumber   = cfg.mmapBlockNumber;
        rx_cfg.protocol      = cfg.etherType;
        rx_cfg.hwTimeStamp   = false;

        PacketMmapTx tx(tx_cfg);
        PacketMmapRx rx(rx_cfg);

        fmt::println("[{}] Transport: packet_mmap on {}", roleName, role.interface);
        dispatchMode(tx, rx, mode, cfg, count, stop);
        return 0;
    }

    if (transport == "af_xdp") {
        // Direction is a compile-time template param (AFXDP.hpp). Pick the REAL
        // mode per role so each socket is minimal (TxOnly skips FILL/XSKMAP/
        // steering; RxOnly skips the TX ring) AND so the hot-path TX kick differs
        // by mode: RxTx kicks unconditionally (interleaved TX+RX needs it — see
        // kickTx), TxOnly keeps the cheaper needs_wakeup-gated kick for pure
        // throughput. interface + queue are the only runtime params; the BPF
        // (af_xdp_kern.o) redirects everything on the queue to userspace.
        auto setup = [&](auto& xsk) -> bool {
            if (!xsk.init()) {
                fmt::println(stderr, "Error: AF_XDP init failed on {}", role.interface);
                return false;
            }
            // Coalescing MUST be zeroed AFTER the bind — xsk_socket__create()
            // resets it, so NicTuner can't do it during pre-bind construction.
            if (cfg.nicTunerMode != NicTunerMode::Off)
                NicTuner::setCoalescingZero(role.interface.c_str());
            fmt::println("[{}] Transport: af_xdp on {} (queue {})",
                         roleName, role.interface, role.xdpQueueId);
            return true;
        };

        switch (mode) {
            case RunMode::TxGen: {
                AFXDP<AFXDPMode::TxOnly> xsk(role.interface.c_str(), role.xdpQueueId);
                if (!setup(xsk)) return 1;
                run_txgen(xsk, cfg, count, stop);
                return 0;
            }
            case RunMode::RxSink: {
                AFXDP<AFXDPMode::RxOnly> xsk(role.interface.c_str(), role.xdpQueueId);
                if (!setup(xsk)) return 1;
                run_rxsink(xsk, cfg, stop);
                return 0;
            }
            case RunMode::Server:
            case RunMode::Client: {
                AFXDP<AFXDPMode::RxTx> xsk(role.interface.c_str(), role.xdpQueueId);
                if (!setup(xsk)) return 1;
                dispatchMode(xsk, xsk, mode, cfg, count, stop);
                return 0;
            }
        }
        return 0;
    }

    if (transport == "intel_i210") {
        if (!ensureHugepages(16)) {
            fmt::println(stderr, "Error: hugepage allocation failed");
            return 1;
        }

        const std::string_view drv = role.driver.empty() ? std::string_view{"igb"} : std::string_view{role.driver};

        Intel_I210<DriverMode::RxTx> nic(role.interface, 0, drv);
        if (!nic.init()) {
            fmt::println(stderr, "Error: Intel I210 init failed");
            return 1;
        }
        fmt::println("[{}] Transport: intel_i210 (PMD) on {} (driver={})",
                     roleName, role.interface, drv);
        dispatchMode(nic, nic, mode, cfg, count, stop);
        return 0;
    }

    if (transport == "cadence_gem") {
        if (!ensureHugepages(16)) {
            fmt::println(stderr, "Error: hugepage allocation failed");
            return 1;
        }

        const std::string_view drv = role.driver.empty() ? std::string_view{"macb"} : std::string_view{role.driver};

        Cadence_GEM<GEMDriverMode::RxTx> nic(role.interface, drv);
        if (!nic.init()) {
            fmt::println(stderr, "Error: Cadence GEM init failed");
            return 1;
        }

        // Kernel traffic (NFS, SSH, ARP) must keep flowing while the PMD owns
        // the hardware — bridge Q_SLOW (Q0) to a TAP device. Q_HOT (Q1)
        // carries our latency-critical traffic and never touches the kernel.
        auto& slow = nic.slowPath();
        const std::string tapName = "tap_" + role.interface;
        TapBridge tap(slow, slow, tapName);

        // Hand the TAP the NIC's L3 identity — without this, the kernel won't
        // recognise traffic the bridge forwards (wrong MAC) and won't have a
        // route or address to reply on, so SSH/NFS/ICMP all drop.
        (void)tap.setupAlias(nic.macAddress(), nic.savedAddr(), nic.savedGateway());

        std::jthread tapThread = spawnTapDeviceThread(tap);

        fmt::println("[{}] Transport: cadence_gem (PMD) on {} (driver={}, tap={})",
                     roleName, role.interface, drv, tapName);
        dispatchMode(nic, nic, mode, cfg, count, stop);

        // Scope destruction order is what we need:
        //   ~tapThread  → request_stop() + join()   (bridge loop exits)
        //   ~tap        → closes /dev/net/tun fd
        //   ~nic        → disables RX/TX, rebinds macb, end0 reappears
        return 0;
    }

    fmt::println(stderr, "Error: unknown transport '{}'", transport);
    return 1;
}
