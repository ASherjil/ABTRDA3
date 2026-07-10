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
#include "PciHelpers.hpp"   // pci:: BDF resolution + driver-specific BAR0 fixes (dpdkPreInit/dpdkPostInitFixes)
#include "SingleRecorder.hpp"
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
        case RunMode::Client: run_client(tx, rx, cfg, stop);           break;
        case RunMode::TxGen:  run_txgen(tx, cfg, count, stop);         break;
        case RunMode::RxSink: run_rxsink(rx, cfg, stop);               break;
    }
}

// ── DPDK driver-specific policy ──────────────────────────────────────────────
// The DPDK transport class is PMD-agnostic. Everything a SPECIFIC NIC needs —
// generic knobs before init, BAR0 register fixes after — is decided HERE, from
// the TOML driver name.

// igc (i225): advertise 1 GbE only. The 2.5GBASE-T PHY carries ~2.4 µs/traversal
// of FIXED pipeline latency (802.3bz = quarter-clocked 10GBASE-T; Intel's own
// PTP constants). Measured A/B: 17.108 -> 13.388 µs RTT. Flip to false + rebuild
// for a native-2.5G run. See docs/Benchmarks.md §5.2.
inline constexpr bool kIgcAdvertise1GOnly = true;

// mlx5 latency devargs (all A/B-tested on ConnectX-4 Lx):
//   txq_mem_algn=0    — legacy per-queue TxQ allocation (the consecutive-umem
//                       feature fails on the 2nd single-queue port in-process)
//   txqs_min_inline=0 — FULL Tx inlining at 1 queue: the whole small frame rides
//                       inside the WQE, no payload DMA read (-520ns uniform)
//   rxq_cqe_comp_en=0 — plain 64B CQEs, no RX decompression (neutral at one
//                       frame in flight; zero cost)
// Full inlining is also what makes the transport's single-mbuf TX-reuse fast
// path valid (the PMD copies the frame inside tx_burst; buffer free on return).
inline constexpr const char* kMlx5LatencyDevargs =
    "txq_mem_algn=0,txqs_min_inline=0,rxq_cqe_comp_en=0";

// Before init()/prepare(): apply the driver's generic knobs and resolve the BDF
// while the netdev still exists (the vfio unbind removes it, and renamed ports
// like "xxv0" cannot be re-derived afterwards). Returns the BDF for
// dpdkPostInitFixes(). Must run BEFORE prepare() — setDevargs feeds the BDF
// registration.
template<typename Nic>
[[nodiscard]] inline std::string dpdkPreInit(Nic& nic, const RoleConfig& role) {
    const std::string bdf = pci::resolveBdf(role.interface);
    if (role.driver == "igc" && !role.dpdkAfxdpPmd) {
        if (kIgcAdvertise1GOnly) nic.setLinkSpeeds(RTE_ETH_LINK_SPEED_1G);
        nic.setSymmetricQueues(true);   // nb_txq=0 silently breaks igc RX (KDI §3.1)
    }
    if (role.driver.find("mlx5") != std::string::npos && !role.dpdkAfxdpPmd) {
        nic.setDevargs(kMlx5LatencyDevargs);
        nic.setTxInlineReuse(true);
    }
    return bdf;
}

// After init() (port started — a reset during start would wipe register fixes):
// the BAR0 pokes. driver == "i40e" covers BOTH the vfio-pci PMD and
// DPDK-over-AF_XDP on a kernel-bound i40e — same silicon, same write-back
// throttle. Both helpers read back and log what actually landed.
inline void dpdkPostInitFixes(const std::string& bdf, const RoleConfig& role) {
    if (role.driver == "i40e")
        pci::i40eClearItr(bdf, role.interface);
    if (role.driver == "igc" && !role.dpdkAfxdpPmd)
        pci::igcDisableEee(bdf, role.interface);
}

// ── Spawn a low-priority thread that runs a TapBridge ──────────────────────
//
// The bridge itself MUST outlive the returned jthread, so the caller owns it.
// This helper just handles the thread attributes (set SCHED_OTHER, clear
// inherited CPU affinity) and spawns the jthread.
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
    // The child inherits our pinned-core affinity from the runtime setup and
    // competes with the hot loop on that one core until it runs the
    // sched_setscheduler/sched_setaffinity prologue above.  Yield long
    // enough for it to execute that prologue and migrate to another core,
    // otherwise it gets little CPU during the hot test loop and Q0 fills up
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
            if (!xsk.init(cfg.afxdpEnableDeferral, cfg.afxdpEnableIrqSuspend)) {
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
                AFXDP<AFXDPMode::RxTx, 2048, 2048, 4096, false> xsk(role.interface.c_str(), role.xdpQueueId);
                if (!setup(xsk)) return 1;
                dispatchMode(xsk, xsk, mode, cfg, count, stop);
                return 0;
            }
        }
        return 0;
    }

    if (transport == "dpdk") {
        // Configured by interface NAME + kernel DRIVER (like the other transports).
        // The binding model is EXPLICIT per-role TOML (no driver-name sniffing):
        // default = vfio-pci pass-through (Intel i40e/igc), dpdk_bifurcated = keep
        // the kernel driver (mlx5-class), dpdk_afxdp_pmd = DPDK-over-AF_XDP vdev
        // (pre-conditioned + validated like the native AF_XDP transport; the
        // [af_xdp] enable_deferral toggle applies to it too). Poll-mode = no
        // interrupts, so no coalescing/NicTuner tuning is needed. lcore =
        // role.cpuCore (must match RuntimeSetup's pin). Per-role mode:
        // TxGen->TxOnly, RxSink->RxOnly, Server/Client->RxTx.
        auto setup = [&](auto& nic) -> bool {
            const std::string bdf = dpdkPreInit(nic, role);   // knobs + BDF, pre-unbind
            if (!nic.init(true, role.dpdkBifurcated, role.dpdkAfxdpPmd,
                          cfg.afxdpEnableDeferral)) {
                fmt::println(stderr, "Error: DPDK init failed on {}", role.interface);
                return false;
            }
            dpdkPostInitFixes(bdf, role);   // i40e ITR clear / igc EEE (readback-logged)
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

    if (transport == "verbs") {
        // Raw-verbs RAW_PACKET QP on mlx5 (bifurcated — kernel keeps the port, which
        // must be admin-UP; mlx5-only, i40e/igc have no verbs provider). Same RTT
        // dispatch as DPDK but minus the ethdev/mbuf layer — the sub-2us path, wired
        // here to localise the DPDK-vs-verbs gap (and its max) on identical silicon.
        // Per-role mode: TxGen->TxOnly, RxSink->RxOnly, Server/Client->RxTx (one QP
        // both sends and reflects). driver/lcore fields are ignored (the RDMA device
        // is resolved from the netdev name). Same Server/Client loops as every other
        // transport, so the relprof prof brackets apply unchanged.
        auto setup = [&](auto& nic) -> bool {
            if (!nic.init()) {
                fmt::println(stderr, "Error: verbs init failed on {}", role.interface);
                return false;
            }
            fmt::println("[{}] Transport: verbs on {} (RAW_PACKET QP)", roleName, role.interface);
            return true;
        };

        switch (mode) {
            case RunMode::TxGen: {
                Verbs<VerbsMode::TxOnly> nic(role.interface);
                if (!setup(nic)) return 1;
                run_txgen(nic, cfg, count, stop);
                return 0;
            }
            case RunMode::RxSink: {
                Verbs<VerbsMode::RxOnly> nic(role.interface);
                if (!setup(nic)) return 1;
                run_rxsink(nic, cfg, stop);
                return 0;
            }
            case RunMode::Server:
            case RunMode::Client: {
                Verbs<VerbsMode::RxTx> nic(role.interface);
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
// port) in ONE process and hands them to Bench, which runs three SCHED_OTHER
// threads — TxThread, RxThread, HistThread — each self-pinned to its own isolated
// core. The Tx stamps each frame with tsc::now(); the Rx stamps arrival and
// computes one-way latency from the in-band stamp. One host's invariant TSC backs
// both ends (cross-core, single-socket — see SingleRecorder.hpp), so there is no
// clock-sync problem.
//
// Transport-generic: Bench<Tx,Rx> is templated on TxRing/RxRing, so this helper
// just constructs the right Tx+Rx pair and runs them. Both ports must use the
// same transport (client.transport == server.transport).

// Run the recorder once the Tx and Rx transports are constructed + init'd.
template<TxRing Tx, RxRing Rx>
inline void driveSingleRecorder(Tx& tx, Rx& rx, const TestConfig& cfg,
                                std::stop_token stop) {
    // Bench owns the shared SPSC + the three SCHED_OTHER threads (Tx/Rx/Hist),
    // each self-pinned to its own isolated core. run() blocks until the soak
    // completes (Tx duration -> RX drains + sentinel -> Hist reports).
    Bench<Tx, Rx> bench(cfg, tx, rx);
    bench.run(cfg.recDurationSec, stop);
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

    if (cfg.recHotPathCore == cfg.recRxCore ||
        cfg.recHotPathCore == cfg.recRecorderCore ||
        cfg.recRxCore == cfg.recRecorderCore) {
        fmt::println(stderr, "Error: single_recorder needs three DISTINCT cores "
                             "(tx={} rx={} recorder={})",
                     cfg.recHotPathCore, cfg.recRxCore, cfg.recRecorderCore);
        return 1;
    }

    fmt::println("[SingleRecorder] {} | Tx={} (core {}) Rx={} (core {}) Hist core {} "
                 "duration={}s",
                 transport, cfg.client.interface, cfg.recHotPathCore,
                 cfg.server.interface, cfg.recRxCore, cfg.recRecorderCore,
                 cfg.recDurationSec);

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
        // NAPI regime from [af_xdp] (enable_deferral / enable_irq_suspend); default
        // both-off = defer=0/gro=0 (re-arms inline, no busy-poll gap needed — the
        // low-latency regime). Orthogonal to the SCHED_OTHER thread policy.
        if (!tx.init(cfg.afxdpEnableDeferral, cfg.afxdpEnableIrqSuspend)) { fmt::println(stderr, "Error: Tx AF_XDP init failed on {}", cfg.client.interface); return 1; }
        if (!rx.init(cfg.afxdpEnableDeferral, cfg.afxdpEnableIrqSuspend)) { fmt::println(stderr, "Error: Rx AF_XDP init failed on {}", cfg.server.interface); return 1; }
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
        // Driver knobs + BDF resolution BEFORE prepare (setDevargs feeds the BDF
        // registration, and prepare's vfio unbind removes the netdev).
        const std::string txBdf = dpdkPreInit(tx, cfg.client);
        const std::string rxBdf = dpdkPreInit(rx, cfg.server);
        if (!tx.prepare(cfg.client.dpdkBifurcated, cfg.client.dpdkAfxdpPmd) ||
            !rx.prepare(cfg.server.dpdkBifurcated, cfg.server.dpdkAfxdpPmd)) {
            fmt::println(stderr, "Error: DPDK prepare (vfio bind) failed");
            return 1;
        }
        // Loopback pair (port0 TX <-> port1 RX on one card): START BOTH ports
        // (init(false) = no link wait) BEFORE waiting EITHER link, because a
        // loopback link only comes up once both PHYs are up. Then wait both.
        // (prepare() already ran, so init()'s flag copies are ignored — only
        // applyDeferral is consumed here.)
        if (!tx.init(false, cfg.client.dpdkBifurcated, cfg.client.dpdkAfxdpPmd,
                     cfg.afxdpEnableDeferral)) {
            fmt::println(stderr, "Error: Tx DPDK init failed on {}", cfg.client.interface);
            return 1;
        }
        if (!rx.init(false, cfg.server.dpdkBifurcated, cfg.server.dpdkAfxdpPmd,
                     cfg.afxdpEnableDeferral)) {
            fmt::println(stderr, "Error: Rx DPDK init failed on {}", cfg.server.interface);
            return 1;
        }
        dpdkPostInitFixes(txBdf, cfg.client);
        dpdkPostInitFixes(rxBdf, cfg.server);
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
