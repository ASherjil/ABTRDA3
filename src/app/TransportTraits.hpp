#pragma once

// TransportTraits — ONE traits struct per transport, so TransportDispatch can be
// written exactly once. The transport is chosen at RUNTIME (a TOML string) but is a
// COMPILE-TIME template param (DPDK<DpdkMode::TxOnly>, AFXDP<...>); that boundary is
// crossed ONCE, in withTransport(). All static + inline, monomorphized per transport:
// no virtuals, no type erasure, nothing on the hot path.
//
// TO ADD A TRANSPORT: write a traits struct + add one line to withTransport().
//
// The CRTP base supplies the defaults; a traits struct overrides only what differs:
//   kName / kPairedRxTx / kSingleCapable   TOML string; needs SEPARATE Tx+Rx objects
//                                          for Server/Client; supports --single
//   TxOnly / RxOnly / RxTx                 per-mode transport types
//   makeTx / makeRx / makeRxTx             construct one. Returns a PRVALUE => guaranteed
//                                          copy elision, so even move-deleted types (Verbs) work
//   makeSingleTx / makeSingleRx            --single may need OTHER types (default: makeTx/makeRx)
//   init / banner / preflight              bring up; log; run-once precondition
//   withAux                                keep auxiliary machinery alive for the run
//   withSinglePair                         the --single lifecycle

#include "TestConfig.hpp"
#include "RingConcepts.hpp"
#include "SocketOps.hpp"
#include "PacketMmapRx.hpp"
#include "PacketMmapTx.hpp"
#include "AFXDP.hpp"
#include "DPDK.hpp"
#include "Verbs.hpp"
#include "Intel_I210.hpp"
#include "Cadence_GEM.hpp"
#include "TapBridge.hpp"
#include "PciHelpers.hpp"
#include "NicTuner.hpp"
#include "common/HugePageHelpers.hpp"

#include <fmt/core.h>

#include <pthread.h>
#include <sched.h>

#include <chrono>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

// ── CRTP base: the defaults every traits struct inherits ────────────────────
template<class Self>
struct TransportBase {
    static constexpr bool kPairedRxTx    = false;
    static constexpr bool kSingleCapable = true;

    static bool preflight(const TestConfig&, const RoleConfig&) noexcept { return true; }

    // Deduced return type + prvalue return keeps the elision chain (no move ctor needed).
    static auto makeSingleTx(const TestConfig& cfg, const RoleConfig& role) {
        return Self::makeTx(cfg, role);
    }
    static auto makeSingleRx(const TestConfig& cfg, const RoleConfig& role) {
        return Self::makeRx(cfg, role);
    }

    template<class Nic, class Fn>
    static void withAux(Nic&, const TestConfig&, const RoleConfig&, Fn&& fn) {
        fn();
    }

    // --single: Tx on the client port, Rx on the server port, both in THIS process.
    template<class Fn>
    static int withSinglePair(const TestConfig& cfg, Fn&& fn) {
        auto tx = Self::makeSingleTx(cfg, cfg.client);
        auto rx = Self::makeSingleRx(cfg, cfg.server);
        if (!Self::init(tx, cfg, cfg.client)) {
            fmt::println(stderr, "Error: Tx {} init failed on {}", Self::kName, cfg.client.interface);
            return 1;
        }
        if (!Self::init(rx, cfg, cfg.server)) {
            fmt::println(stderr, "Error: Rx {} init failed on {}", Self::kName, cfg.server.interface);
            return 1;
        }
        fn(tx, rx);
        return 0;
    }
};

// ── packet_mmap ─────────────────────────────────────────────────────────────
// The only PAIRED transport: no duplex form, so Server/Client runs two objects on the
// same interface. The ctor opens the ring — there is no init().
struct PacketMmapTraits : TransportBase<PacketMmapTraits> {
    static constexpr std::string_view kName        = "packet_mmap";
    static constexpr bool             kPairedRxTx  = true;

    using TxOnly = PacketMmapTx;
    using RxOnly = PacketMmapRx;
    using RxTx   = void;                       // unused: kPairedRxTx

    static PacketMmapTx makeTx(const TestConfig& cfg, const RoleConfig& role) {
        RingConfig rc{};
        rc.interface     = role.interface.c_str();
        rc.direction     = RingDirection::TX;
        rc.blockSize     = cfg.mmapBlockSize;
        rc.blockNumber   = cfg.mmapBlockNumber;
        rc.protocol      = cfg.etherType;
        rc.packetVersion = TPACKET_V2;
        rc.qdiscBypass   = true;
        return PacketMmapTx(rc);
    }

    static PacketMmapRx makeRx(const TestConfig& cfg, const RoleConfig& role) {
        RingConfig rc{};
        rc.interface   = role.interface.c_str();
        rc.direction   = RingDirection::RX;
        rc.blockSize   = cfg.mmapBlockSize;
        rc.blockNumber = cfg.mmapBlockNumber;
        rc.protocol    = cfg.etherType;
        rc.hwTimeStamp = false;
        return PacketMmapRx(rc);
    }

    template<class Nic>
    static bool init(Nic&, const TestConfig&, const RoleConfig&) noexcept {
        return true;
    }

    static void banner(const TestConfig&, const RoleConfig& role, const char* roleName) {
        fmt::println("[{}] Transport: packet_mmap on {}", roleName, role.interface);
    }
};

// ── af_xdp ──────────────────────────────────────────────────────────────────
// Direction is a compile-time param, so each role gets a MINIMAL socket (TxOnly skips
// FILL/XSKMAP/steering; RxOnly skips the TX ring) and the TX kick policy can differ by
// type: RxTx + the --single TX socket kick UNCONDITIONALLY (NeedWakeup=false); TxGen
// keeps the cheaper gated kick.
struct AfxdpTraits : TransportBase<AfxdpTraits> {
    static constexpr std::string_view kName = "af_xdp";

    using TxOnly   = AFXDP<AFXDPMode::TxOnly>;
    using RxOnly   = AFXDP<AFXDPMode::RxOnly>;
    using RxTx     = AFXDP<AFXDPMode::RxTx, 2048, 2048, 4096, false>;
    using SingleTx = AFXDP<AFXDPMode::TxOnly, 2048, 2048, 4096, false>;

    static TxOnly makeTx(const TestConfig&, const RoleConfig& role) {
        return TxOnly(role.interface.c_str(), role.xdpQueueId);
    }
    static RxOnly makeRx(const TestConfig&, const RoleConfig& role) {
        return RxOnly(role.interface.c_str(), role.xdpQueueId);
    }
    static RxTx makeRxTx(const TestConfig&, const RoleConfig& role) {
        return RxTx(role.interface.c_str(), role.xdpQueueId);
    }
    static SingleTx makeSingleTx(const TestConfig&, const RoleConfig& role) {
        return SingleTx(role.interface.c_str(), role.xdpQueueId);
    }

    template<class Nic>
    static bool init(Nic& xsk, const TestConfig& cfg, const RoleConfig& role) {
        if (!xsk.init(cfg.afxdpEnableDeferral, cfg.afxdpEnableIrqSuspend))
            return false;
        // Coalescing MUST be zeroed AFTER the bind — xsk_socket__create() resets it, so
        // NicTuner cannot do it during pre-bind construction.
        if (cfg.nicTunerMode != NicTunerMode::Off)
            NicTuner::setCoalescingZero(role.interface.c_str());
        // Read-only: confirm in SILICON that the data-queue RX ITR really is 0 after
        // ethtool/NicTuner — a nonzero PFINT_ITRN is the RX-writeback throttle, not a
        // NAPI-cadence limit (i40e only; KDI §1).
        if (pci::boundToI40e(role.interface))
            pci::reportItr(role.interface, "AFXDP-ITR");
        return true;
    }

    static void banner(const TestConfig&, const RoleConfig& role, const char* roleName) {
        fmt::println("[{}] Transport: af_xdp on {} (queue {})",
                     roleName, role.interface, role.xdpQueueId);
    }
};

// ── dpdk ────────────────────────────────────────────────────────────────────
// The DPDK class is PMD-agnostic; ALL driver-specific policy lives here.

// igc runs at 1 GbE: the 2.5GBASE-T PHY costs ~2.4us/traversal of FIXED pipeline
// latency. Measured A/B: 17.108 -> 13.388us RTT. Flip + rebuild for native 2.5G.
// docs/Benchmarks.md §5.2.
inline constexpr bool kIgcAdvertise1GOnly = true;

// mlx5 latency devargs (A/B-tested on ConnectX-4 Lx): legacy per-queue TxQ alloc
// (consecutive-umem fails on the 2nd single-queue port in-process); FULL Tx inlining at
// 1 queue — the frame rides inside the WQE, no payload DMA read (-520ns uniform, and it
// is what makes setTxInlineReuse valid); plain 64B CQEs, no RX decompression.
// sq_db_nc=0 (WC doorbell mapping) was A/B'd and is a NULL: median moved 1ns. The cost is
// the NIC's DMA read-back of the WQE (BlueFlame), not how the CPU writes the doorbell.
inline constexpr const char* kMlx5LatencyDevargs =
    "txq_mem_algn=0,txqs_min_inline=0,rxq_cqe_comp_en=0";

// mlx5 RX ring: 64, not the 256 default. mlx5 bulk-refills min(64, NbRxDesc>>2) mbufs at the
// TOP of rx_burst, so a 256 ring pays a 64-mbuf refill INSIDE a poll while the echo is in
// flight; 64 cuts that to 16. Measured (30s A/B, CX4): P99.9 3.793 -> 3.648us (-145ns),
// median UNMOVED at 3.379, Vector SSE retained. mlx5 ONLY — i40e's vector rearm threshold is
// a compile-time 64, so a 64-entry ring would leave it no headroom.
inline constexpr std::uint16_t kMlx5NbRxDesc = 64;

// BEFORE prepare(): set the knobs and resolve the BDF while the netdev still exists (the
// vfio unbind removes it, and renamed ports like "xxv0" cannot be re-derived after).
// setDevargs must land before prepare() registers the BDF.
template<class Nic>
[[nodiscard]] inline std::string dpdkPreInit(Nic& nic, const RoleConfig& role) {
    const std::string bdf = pci::resolveBdf(role.interface);
    if (role.driver == "igc" && !role.dpdkAfxdpPmd) {
        if (kIgcAdvertise1GOnly) nic.setLinkSpeeds(RTE_ETH_LINK_SPEED_1G);
        nic.setSymmetricQueues(true);        // nb_txq=0 silently breaks igc RX (KDI §3.1)
    }
    if (role.driver.find("mlx5") != std::string::npos && !role.dpdkAfxdpPmd) {
        nic.setDevargs(kMlx5LatencyDevargs);
        nic.setTxInlineReuse(true);
        nic.setNbRxDesc(kMlx5NbRxDesc);      // shrinks the in-poll refill burst 64 -> 16
    }
    return bdf;
}

// AFTER dev_start (a reset during start would wipe these). "i40e" covers both the vfio
// PMD and DPDK-over-AF_XDP on a kernel-bound i40e — same silicon, same write-back
// throttle. Both helpers read back what landed.
inline void dpdkPostInitFixes(const std::string& bdf, const RoleConfig& role) {
    if (role.driver == "i40e")
        pci::i40eClearItr(bdf, role.interface);
    if (role.driver == "igc" && !role.dpdkAfxdpPmd)
        pci::igcDisableEee(bdf, role.interface);
}

struct DpdkTraits : TransportBase<DpdkTraits> {
    static constexpr std::string_view kName = "dpdk";

    using TxOnly = DPDK<DpdkMode::TxOnly>;
    using RxOnly = DPDK<DpdkMode::RxOnly>;
    using RxTx   = DPDK<DpdkMode::RxTx>;

    static TxOnly makeTx(const TestConfig&, const RoleConfig& role) {
        return TxOnly(role.interface, role.cpuCore, role.driver);
    }
    static RxOnly makeRx(const TestConfig&, const RoleConfig& role) {
        return RxOnly(role.interface, role.cpuCore, role.driver);
    }
    static RxTx makeRxTx(const TestConfig&, const RoleConfig& role) {
        return RxTx(role.interface, role.cpuCore, role.driver);
    }

    template<class Nic>
    static bool init(Nic& nic, const TestConfig& cfg, const RoleConfig& role) {
        const std::string bdf = dpdkPreInit(nic, role);
        if (!nic.init(true, role.dpdkBifurcated, role.dpdkAfxdpPmd, cfg.afxdpEnableDeferral))
            return false;
        dpdkPostInitFixes(bdf, role);
        return true;
    }

    static void banner(const TestConfig&, const RoleConfig& role, const char* roleName) {
        fmt::println("[{}] Transport: dpdk on {} (driver={}, lcore {})",
                     roleName, role.interface, role.driver, role.cpuCore);
    }

    // DPDK needs its OWN --single lifecycle: EAL is process-global, so BOTH ports must be
    // registered (prepare) before the first init() triggers the one rte_eal_init; and a
    // loopback link only comes up once BOTH PHYs are up, so start both (init(false) = no
    // link wait), then wait both. One hot-path core drives both ports. KDI §4.1.
    template<class Fn>
    static int withSinglePair(const TestConfig& cfg, Fn&& fn) {
        TxOnly tx(cfg.client.interface, cfg.recHotPathCore, cfg.client.driver);
        RxOnly rx(cfg.server.interface, cfg.recHotPathCore, cfg.server.driver);

        const std::string txBdf = dpdkPreInit(tx, cfg.client);   // knobs + BDF, pre-unbind
        const std::string rxBdf = dpdkPreInit(rx, cfg.server);

        if (!tx.prepare(cfg.client.dpdkBifurcated, cfg.client.dpdkAfxdpPmd) ||
            !rx.prepare(cfg.server.dpdkBifurcated, cfg.server.dpdkAfxdpPmd)) {
            fmt::println(stderr, "Error: DPDK prepare (vfio bind) failed");
            return 1;
        }
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

        if (!tx.waitLink()) {
            fmt::println(stderr, "Error: Tx DPDK link down on {}", cfg.client.interface);
            return 1;
        }
        if (!rx.waitLink()) {
            fmt::println(stderr, "Error: Rx DPDK link down on {}", cfg.server.interface);
            return 1;
        }
        fn(tx, rx);
        return 0;
    }
};

// ── verbs ───────────────────────────────────────────────────────────────────
// RAW_PACKET QP on mlx5 — the sub-2us path: same dispatch as DPDK minus the ethdev/mbuf
// layer, on identical silicon. mlx5-only (Intel has no verbs provider); the port stays
// on the kernel driver and must be admin-UP. driver/lcore are ignored (the RDMA device
// is resolved from the netdev name).
struct VerbsTraits : TransportBase<VerbsTraits> {
    static constexpr std::string_view kName = "verbs";

    using TxOnly = Verbs<VerbsMode::TxOnly>;
    using RxOnly = Verbs<VerbsMode::RxOnly>;
    using RxTx   = Verbs<VerbsMode::RxTx>;

    static TxOnly makeTx(const TestConfig&, const RoleConfig& role) { return TxOnly(role.interface); }
    static RxOnly makeRx(const TestConfig&, const RoleConfig& role) { return RxOnly(role.interface); }
    static RxTx   makeRxTx(const TestConfig&, const RoleConfig& role) { return RxTx(role.interface); }

    template<class Nic>
    static bool init(Nic& nic, const TestConfig&, const RoleConfig&) {
        return nic.init();
    }

    static void banner(const TestConfig&, const RoleConfig& role, const char* roleName) {
        fmt::println("[{}] Transport: verbs on {} (RAW_PACKET QP)", roleName, role.interface);
    }
};

// ── intel_i210 (custom PMD) ─────────────────────────────────────────────────
// One duplex object serves every mode. No --single (that needs two ports in one process).
struct I210Traits : TransportBase<I210Traits> {
    static constexpr std::string_view kName          = "intel_i210";
    static constexpr bool             kSingleCapable = false;

    using RxTx   = Intel_I210<DriverMode::RxTx>;
    using TxOnly = RxTx;
    using RxOnly = RxTx;

    static std::string_view driverOf(const RoleConfig& role) {
        return role.driver.empty() ? std::string_view{"igb"} : std::string_view{role.driver};
    }

    static bool preflight(const TestConfig&, const RoleConfig&) {
        if (ensureHugepages(16)) return true;
        fmt::println(stderr, "Error: hugepage allocation failed");
        return false;
    }

    static RxTx makeRxTx(const TestConfig&, const RoleConfig& role) {
        return RxTx(role.interface, 0, driverOf(role));
    }
    static RxTx makeTx(const TestConfig& cfg, const RoleConfig& role) { return makeRxTx(cfg, role); }
    static RxTx makeRx(const TestConfig& cfg, const RoleConfig& role) { return makeRxTx(cfg, role); }

    template<class Nic>
    static bool init(Nic& nic, const TestConfig&, const RoleConfig&) {
        return nic.init();
    }

    static void banner(const TestConfig&, const RoleConfig& role, const char* roleName) {
        fmt::println("[{}] Transport: intel_i210 (PMD) on {} (driver={})",
                     roleName, role.interface, driverOf(role));
    }
};

// ── cadence_gem (custom PMD) ────────────────────────────────────────────────
// Same one-duplex-object shape as the I210, PLUS a TAP bridge: the PMD owns the hardware,
// so kernel traffic (NFS/SSH/ARP) is bridged from Q_SLOW (Q0) to a TAP device while Q_HOT
// (Q1) carries our traffic and never touches the kernel. The bridge must outlive the
// benchmark -> withAux.
struct GemTraits : TransportBase<GemTraits> {
    static constexpr std::string_view kName          = "cadence_gem";
    static constexpr bool             kSingleCapable = false;

    using RxTx   = Cadence_GEM<GEMDriverMode::RxTx>;
    using TxOnly = RxTx;
    using RxOnly = RxTx;

    static std::string_view driverOf(const RoleConfig& role) {
        return role.driver.empty() ? std::string_view{"macb"} : std::string_view{role.driver};
    }

    static bool preflight(const TestConfig&, const RoleConfig&) {
        if (ensureHugepages(16)) return true;
        fmt::println(stderr, "Error: hugepage allocation failed");
        return false;
    }

    static RxTx makeRxTx(const TestConfig&, const RoleConfig& role) {
        return RxTx(role.interface, driverOf(role));
    }
    static RxTx makeTx(const TestConfig& cfg, const RoleConfig& role) { return makeRxTx(cfg, role); }
    static RxTx makeRx(const TestConfig& cfg, const RoleConfig& role) { return makeRxTx(cfg, role); }

    template<class Nic>
    static bool init(Nic& nic, const TestConfig&, const RoleConfig&) {
        return nic.init();
    }

    static void banner(const TestConfig&, const RoleConfig& role, const char* roleName) {
        fmt::println("[{}] Transport: cadence_gem (PMD) on {} (driver={}, tap=tap_{})",
                     roleName, role.interface, driverOf(role), role.interface);
    }

    // Destruction order at scope exit is exactly what we need:
    //   ~tapThread -> request_stop() + join()  ~tap -> close the tun fd
    //   ~nic (caller's scope) -> disable RX/TX, rebind macb, end0 reappears
    template<class Nic, class Fn>
    static void withAux(Nic& nic, const TestConfig&, const RoleConfig& role, Fn&& fn) {
        auto&             slow    = nic.slowPath();
        const std::string tapName = "tap_" + role.interface;
        TapBridge         tap(slow, slow, tapName);

        // The TAP needs the NIC's L3 identity, or the kernel won't recognise bridged
        // traffic (wrong MAC) and has no address/route to reply on — SSH/NFS/ICMP drop.
        (void)tap.setupAlias(nic.macAddress(), nic.savedAddr(), nic.savedGateway());

        std::jthread tapThread = spawnTapBridgeThread(tap);
        fn();
    }

private:
    // `tap` MUST outlive the returned jthread, so the CALLER owns it. Here: set the thread
    // attributes (SCHED_OTHER, clear the inherited pin) and spawn.
    template<TxRing Tx, RxRing Rx>
    [[nodiscard]] static std::jthread spawnTapBridgeThread(TapBridge<Tx, Rx>& tap) {
        auto t = std::jthread([&tap](std::stop_token st) {
            sched_param sp{};
            pthread_setschedparam(pthread_self(), SCHED_OTHER, &sp);

            cpu_set_t all;
            CPU_ZERO(&all);
            for (int c = 0; c < CPU_SETSIZE; ++c) CPU_SET(c, &all);
            pthread_setaffinity_np(pthread_self(), sizeof(all), &all);

            tap(st);
        });
        // The child inherits our pinned-core affinity and fights the hot loop for that one
        // core until it runs the prologue above — yield long enough for it to migrate, or
        // it starves during the run and Q0 fills up un-drained.
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        return t;
    }
};
