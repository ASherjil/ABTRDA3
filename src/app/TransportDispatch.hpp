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
#include "DPDK.hpp"
#include "Verbs.hpp"
#include "PciHelpers.hpp"   // pci::reportItr / boundToI40e (i40e ITR check)
#include "SingleRecorder.hpp"
#include "RuntimeSetup.hpp"
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

enum class RunMode { Server, Client, TxGen, RxSink, SingleRecorder };

inline const char* runModeName(RunMode m) {
    switch (m) {
        case RunMode::Server:         return "Server";
        case RunMode::Client:         return "Client";
        case RunMode::TxGen:          return "TxGen";
        case RunMode::RxSink:         return "RxSink";
        case RunMode::SingleRecorder: return "SingleRecorder";
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
            // STEP 1 verification: does the ethtool/coalescing path ACTUALLY leave
            // i40e ITR=0 in silicon? (DPDK needed a BAR0 write; ethtool may not.)
            // A NONZERO read here means the old AF_XDP "17us wall" is the same
            // RX-writeback throttle that capped DPDK at 30us — not a NAPI-cadence
            // limit. Read-only; i40e only.
            // Read-only guard: confirm in silicon that the data-queue RX ITR
            // (PFINT_ITRN) is actually 0 after NicTuner/ethtool. (A/B 2026-06-01
            // proved the misc-vector PFINT_ITR0=122us is irrelevant to RX — it
            // does not gate our path; ITRN is the one that matters.)
            if (pci::boundToI40e(role.interface))
                pci::reportItr(role.interface, "AFXDP-ITR");
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

    if (transport == "dpdk") {
        // Configured by interface NAME + kernel DRIVER (like the other transports).
        // DPDK init() resolves the name -> PCI BDF, then for Intel (i40e/igc) unbinds
        // the kernel driver onto vfio-pci, or for mlx5 (driver name contains "mlx5")
        // leaves it on the kernel driver (bifurcated). Poll-mode = no interrupts, so
        // no coalescing/NicTuner tuning is needed. lcore = role.cpuCore (must match
        // RuntimeSetup's pin). Per-role mode: TxGen->TxOnly, RxSink->RxOnly,
        // Server/Client->RxTx.
        auto setup = [&](auto& nic) -> bool {
            if (!nic.init()) {
                fmt::println(stderr, "Error: DPDK init failed on {}", role.interface);
                return false;
            }
            // STEP 1 verification: confirm DPDK init()'s BAR0 ITR write actually
            // landed (device is on vfio-pci now -> bdfFromName resolves it). Read-
            // only; i40e only. Expect CLEARED here; if NONZERO the write missed.
            if (role.driver == "i40e")
                pci::reportItr(role.interface, "DPDK-ITR");
            fmt::println("[{}] Transport: dpdk on {} (driver={}, lcore {})",
                         roleName, role.interface, role.driver, role.cpuCore);
            return true;
        };

        switch (mode) {
            case RunMode::TxGen: {
                DPDK<DpdkMode::TxOnly> nic(role.interface, role.cpuCore, role.driver);
                if (!setup(nic)) return 1;
                run_txgen(nic, cfg, count, stop);
                return 0;
            }
            case RunMode::RxSink: {
                DPDK<DpdkMode::RxOnly> nic(role.interface, role.cpuCore, role.driver);
                if (!setup(nic)) return 1;
                run_rxsink(nic, cfg, stop);
                return 0;
            }
            case RunMode::Server:
            case RunMode::Client: {
                DPDK<DpdkMode::RxTx> nic(role.interface, role.cpuCore, role.driver);
                if (!setup(nic)) return 1;
                dispatchMode(nic, nic, mode, cfg, count, stop);
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

// ── Single-process one-way latency recorder ─────────────────────────────────
//
// Builds a Tx transport (on the client port) and an Rx transport (on the server
// port) in ONE process, runs LoopBack on the calling thread (already pinned to
// hot_path_core + SCHED_FIFO by the RuntimeSetup in main) and Recorder on a
// second thread self-pinned to benchmark_recorder_core. One host TSC brackets
// every packet, so there is no clock-sync problem (see SingleRecorder.hpp).
//
// Transport-generic: LoopBack/Recorder are templated on TxRing/RxRing, so this
// helper just constructs the right Tx+Rx pair and runs them. Both ports must use
// the same transport (client.transport == server.transport).

// Run the recorder once the Tx and Rx transports are constructed + init'd.
template<TxRing Tx, RxRing Rx>
inline void driveSingleRecorder(Tx& tx, Rx& rx, const TestConfig& cfg,
                                std::stop_token stop) {
    RecorderChannel ch(cfg.recQueueCapacity);

    // Recorder on its own core. CRITICAL: it MUST be demoted to SCHED_OTHER. The
    // jthread is spawned AFTER RuntimeSetup promoted the main thread to
    // SCHED_FIFO:49, so it INHERITS FIFO:49. If left at FIFO:49 on a core that
    // also handles the RX NIC IRQ, it starves ksoftirqd there — which silently
    // kills packet_mmap RX (the kernel softirq can never fill the mmap ring, so
    // tryReceive() spins forever: observed sent=1 recv=0). AF_XDP busy-polls RX
    // inline so it survived this, but the kernel-softirq transports do not.
    // Setting affinity alone does NOT change policy — demote explicitly.
    std::jthread recThread([&](std::stop_token st) {
        sched_param sp{};                       // priority 0
        pthread_setschedparam(pthread_self(), SCHED_OTHER, &sp);
        cpu_set_t set;
        CPU_ZERO(&set);
        CPU_SET(cfg.recRecorderCore, &set);
        pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
        Recorder rec(ch, cfg.outputPath, cfg.recDurationSec);
        rec(st);
    });

    // Hot path on THIS thread (already pinned to hot_path_core + SCHED_FIFO).
    LoopBack<Tx, Rx> hot(tx, rx, cfg, ch, cfg.recDurationSec);
    hot(stop);
    // hot() set ch.done; recThread drains + reports, then joins at scope exit.
}

[[nodiscard]]
inline int runSingleRecorder(const TestConfig& cfg, std::stop_token stop) {
    const std::string& transport = cfg.client.transport;
    if (cfg.server.transport != transport) {
        fmt::println(stderr, "Error: single_recorder needs client.transport == "
                             "server.transport (Tx=client port, Rx=server port); "
                             "got tx='{}' rx='{}'",
                     transport, cfg.server.transport);
        return 1;
    }

    fmt::println("[SingleRecorder] {} | Tx={} (hot core {}) Rx={} (rec core {}) duration={}s",
                 transport, cfg.client.interface, cfg.recHotPathCore,
                 cfg.server.interface, cfg.recRecorderCore, cfg.recDurationSec);

    if (transport == "packet_mmap") {
        RingConfig txc{};
        txc.interface = cfg.client.interface.c_str();
        txc.direction = RingDirection::TX;
        txc.blockSize = cfg.mmapBlockSize; txc.blockNumber = cfg.mmapBlockNumber;
        txc.protocol  = cfg.etherType;     txc.packetVersion = TPACKET_V2;
        txc.qdiscBypass = true;

        RingConfig rxc{};
        rxc.interface = cfg.server.interface.c_str();
        rxc.direction = RingDirection::RX;
        rxc.blockSize = cfg.mmapBlockSize; rxc.blockNumber = cfg.mmapBlockNumber;
        rxc.protocol  = cfg.etherType;     rxc.hwTimeStamp = false;

        PacketMmapTx tx(txc);
        PacketMmapRx rx(rxc);
        driveSingleRecorder(tx, rx, cfg, stop);
        return 0;
    }

    if (transport == "af_xdp") {

        AFXDP<AFXDPMode::TxOnly, 2048, 2048, 4096, false> tx(cfg.client.interface.c_str(), cfg.client.xdpQueueId);
        AFXDP<AFXDPMode::RxOnly> rx(cfg.server.interface.c_str(), cfg.server.xdpQueueId);
        if (!tx.init()) { fmt::println(stderr, "Error: Tx AF_XDP init failed on {}", cfg.client.interface); return 1; }
        if (!rx.init()) { fmt::println(stderr, "Error: Rx AF_XDP init failed on {}", cfg.server.interface); return 1; }
        if (cfg.nicTunerMode != NicTunerMode::Off) {
            NicTuner::setCoalescingZero(cfg.client.interface.c_str());
            NicTuner::setCoalescingZero(cfg.server.interface.c_str());
        }
        if (pci::boundToI40e(cfg.server.interface)) pci::reportItr(cfg.server.interface, "RX-ITR");
        driveSingleRecorder(tx, rx, cfg, stop);
        return 0;
    }

    if (transport == "dpdk") {
        // EAL is process-global: bind BOTH ports to vfio + register BOTH BDFs
        // (prepare) BEFORE either init() triggers the single EAL init, so the
        // -a allowlist covers both. lcore = the hot-path core for both objects
        // (the one thread that polls both ports).
        DPDK<DpdkMode::TxOnly> tx(cfg.client.interface, cfg.recHotPathCore, cfg.client.driver);
        DPDK<DpdkMode::RxOnly> rx(cfg.server.interface, cfg.recHotPathCore, cfg.server.driver);
        if (!tx.prepare() || !rx.prepare()) {
            fmt::println(stderr, "Error: DPDK prepare (vfio bind) failed");
            return 1;
        }
        // Loopback pair (port0 TX <-> port1 RX on one card): START BOTH ports
        // (init(false) = no link wait) BEFORE waiting EITHER link, because a
        // loopback link only comes up once both PHYs are up. Then wait both.
        if (!tx.init(false)) { fmt::println(stderr, "Error: Tx DPDK init failed on {}", cfg.client.interface); return 1; }
        if (!rx.init(false)) { fmt::println(stderr, "Error: Rx DPDK init failed on {}", cfg.server.interface); return 1; }
        if (!tx.waitLink()) { fmt::println(stderr, "Error: Tx DPDK link down on {}", cfg.client.interface); return 1; }
        if (!rx.waitLink()) { fmt::println(stderr, "Error: Rx DPDK link down on {}", cfg.server.interface); return 1; }
        driveSingleRecorder(tx, rx, cfg, stop);
        return 0;
    }

    if (transport == "verbs") {
        // Raw-verbs RAW_PACKET QPs on the SAME mlx5 silicon as the DPDK path,
        // minus the ethdev/mbuf layers. Kernel driver keeps the port (bifurcated
        // like DPDK/mlx5); ports must be admin-UP. mlx5-only — Intel NICs have
        // no verbs provider and keep the dpdk transport.
        Verbs<VerbsMode::TxOnly> tx(cfg.client.interface);
        Verbs<VerbsMode::RxOnly> rx(cfg.server.interface);
        if (!tx.init()) { fmt::println(stderr, "Error: Tx verbs init failed on {}", cfg.client.interface); return 1; }
        if (!rx.init()) { fmt::println(stderr, "Error: Rx verbs init failed on {}", cfg.server.interface); return 1; }
        driveSingleRecorder(tx, rx, cfg, stop);
        return 0;
    }

    fmt::println(stderr, "Error: single_recorder: unsupported transport '{}'", transport);
    return 1;
}
