#pragma once

// TransportTraits — one traits struct per transport; TransportDispatch is written once.
// Transport is chosen at RUNTIME (TOML string) but is a COMPILE-TIME template param; the
// boundary is crossed once, in withTransport(). All static + inline, monomorphized:
// no virtuals, nothing on the hot path.
//
// TO ADD A TRANSPORT: write a traits struct + one line in withTransport().
//
// The CRTP base supplies defaults; override only what differs:
//   kName / kPairedRxTx / kSingleCapable   TOML string; separate Tx+Rx objects; --single ok
//   TxOnly / RxOnly / RxTx                 per-mode transport types
//   makeTx / makeRx / makeRxTx             construct one (prvalue => elision; move-deleted ok)
//   makeSingleTx / makeSingleRx            --single overrides (default: makeTx/makeRx)
//   init / banner / preflight              bring up; log; run-once precondition
//   withAux / withSinglePair               auxiliary machinery; the --single lifecycle

#include "TestConfig.hpp"
#include "RingConcepts.hpp"
#include "SocketOps.hpp"
#include "PacketMmapRx.hpp"
#include "PacketMmapTx.hpp"
#include "AFXDP.hpp"
#include "DPDK.hpp"
#include "Verbs.hpp"
#include "EtherFabricVirtualInterface.hpp"
#include "Intel_I210.hpp"
#include "PciHelpers.hpp"
#include "NicTuner.hpp"
#include "common/HugePageHelpers.hpp"

#include <string>
#include <string_view>

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

// ── i40e RX write-back policy (silicon-wide: DPDK, DPDK-over-AF_XDP, native AF_XDP) ──
// The XXV710 gates RX-descriptor write-back on the per-queue ITR even in pure polling.
// Fix = NoITR (QINT_RQCTL.ITR_INDX = 3): datasheet 332464 §8.3.3.1.4.2 "descriptors...
// reported instantly". DPDK uses index 3 only for FDIR, never the data path.
// Measured 2x2 (XXV710, one frame in flight):
//   interval 32us + INDX 0 -> ~30us (stock)   interval 0 + INDX 0 -> 9.028us
//   interval 0    + INDX 3 -> 9.027us         interval 32us + INDX 3 -> 9.029us
// Either knob alone reaches the same ~9us silicon floor. NoITR preferred: one write,
// immune to adaptive ITR rewriting the interval on the kernel path. Positive control:
// kI40eItrProbe (20us index) moved the median 9.03 -> 20.65us.
inline constexpr bool kI40eNoItr = true;

// Diagnostic: re-demonstrate that BAR0 writes reach the silicon.
inline constexpr bool          kI40eItrProbe         = false;
inline constexpr std::uint32_t kI40eItrProbeIdx      = 1;    // NoITR leaves index 0 alone
inline constexpr std::uint32_t kI40eItrProbeInterval = 10;   // x2us = 20us

// ITR-interval clear on top of NoITR — measured redundant. Kept callable, OFF.
inline constexpr bool kI40eAlsoClearItr = false;

static_assert(!(kI40eNoItr && kI40eItrProbe), "NoITR and the ITR probe both write ITR_INDX");
static_assert(kI40eNoItr || kI40eAlsoClearItr || kI40eItrProbe,
              "i40e with neither NoITR nor the ITR clear sits at the PMD's 32us default (~30us RTT)");

// Apply + verify on any stack. `bdf` must be pre-resolved (netdev gone after vfio unbind).
inline void i40eApplyWritebackPolicy(const std::string& bdf, std::string_view ifname,
                                     const char* tag) {
    if (kI40eAlsoClearItr) pci::i40eClearItr(bdf, ifname);
    if (kI40eNoItr)        pci::i40eSetNoItr(bdf, ifname);
    if (kI40eItrProbe)     pci::i40eItrProbe(bdf, ifname, kI40eItrProbeIdx, kI40eItrProbeInterval);
    pci::reportItr(bdf, ifname, tag);   // under NoITR a NONZERO ITRN is fine — it is ignored
}

// ── af_xdp ──────────────────────────────────────────────────────────────────
// Direction is compile-time: each role gets a minimal socket (TxOnly skips FILL/XSKMAP,
// RxOnly skips the TX ring). Kick policy differs by type: RxTx + --single kick
// unconditionally (NeedWakeup=false); TxGen keeps the cheaper gated kick.
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
        // Kernel-bound throughout, so the netdev exists and the BDF resolves by name.
        const bool  isI40e = pci::boundToI40e(role.interface);
        const std::string bdf = isI40e ? pci::resolveBdf(role.interface) : std::string{};

        // Pre-bind ITR read: pins down that the XSK bind itself zeroes PFINT_ITRN.
        if (isI40e) pci::reportItr(bdf, role.interface, "AFXDP-ITR pre-bind");

        if (!xsk.init(cfg.afxdpEnableDeferral, cfg.afxdpEnableIrqSuspend))
            return false;
        // AFTER the bind — xsk_socket__create() resets coalescing.
        if (cfg.nicTunerMode != NicTunerMode::Off)
            NicTuner::setCoalescingZero(role.interface.c_str());
        // AFTER xsk.init() — the bind reconfigures the queue pair and rewrites QINT_RQCTL.
        if (isI40e) i40eApplyWritebackPolicy(bdf, role.interface, "AFXDP-ITR post-bind");
        return true;
    }

    static void banner(const TestConfig&, const RoleConfig& role, const char* roleName) {
        fmt::println("[{}] Transport: af_xdp on {} (queue {})",
                     roleName, role.interface, role.xdpQueueId);
    }
};

// ── dpdk ────────────────────────────────────────────────────────────────────
// The DPDK class is PMD-agnostic; ALL driver-specific policy lives here.

// igc at 1 GbE: the 2.5GBASE-T PHY costs ~2.4us fixed latency per traversal.
// Measured A/B: 17.108 -> 13.388us RTT. docs/Benchmarks.md §5.2.
inline constexpr bool kIgcAdvertise1GOnly = true;

// mlx5 latency devargs (A/B on CX4 Lx): legacy per-queue TxQ alloc (consecutive-umem
// fails on the 2nd single-queue port in-process); full Tx inlining at 1 queue — frame
// rides in the WQE, no payload DMA read (-520ns; prerequisite for setTxInlineReuse);
// plain 64B CQEs. sq_db_nc=0 was a null (1ns): the cost is the WQE DMA read-back
// (BlueFlame), not the doorbell write.
inline constexpr const char* kMlx5LatencyDevargs =
    "txq_mem_algn=0,txqs_min_inline=0,rxq_cqe_comp_en=0";

// 64, not the 256 default: mlx5 bulk-refills min(64, NbRxDesc>>2) mbufs at the top of
// rx_burst — 256 pays a 64-mbuf refill mid-poll, 64 pays 16. Measured (30s, CX4):
// P99.9 -145ns, median unmoved, Vector SSE retained. mlx5 ONLY — i40e's vector rearm
// threshold is a compile-time 64, leaving a 64-ring no headroom.
inline constexpr std::uint16_t kMlx5NbRxDesc = 64;

// sfc latency devargs (X2522). perf_profile defaults to throughput; low-latency
// (license-gated INIT_EVQ on Medford2) enables RX/event cut-through, disables RX
// batching. rx ef10 = native X2 datapath, pinned for determinism. tx ef10_simple =
// fastest TX (single-seg, one mempool, ignores refcounts) — do NOT pair with
// setTxInlineReuse (needs refcounts), and there is no full-inline TX here anyway
// (no CTPIO in the PMD; that gap IS ef_vi's win). stats_update_period_ms=0 kills the
// default 1Hz MC stats DMA push (same disturbance class as MCDI temp reads).
inline constexpr const char* kSfcLatencyDevargs =
    "perf_profile=low-latency,rx_datapath=ef10,tx_datapath=ef10_simple,"
    "stats_update_period_ms=0";

// 64, not 256 — same lever as kMlx5NbRxDesc: ef10 refills when free >= max(fill/8, 8)
// and pays the whole backlog inside one rx_burst; fill 256 = 32-mbuf bursts, 64 = 8.
// The EF10 hardware ring stays at its 512 minimum regardless (nb_rx_desc only sets the
// fill level) — no headroom risk with one frame in flight.
inline constexpr std::uint16_t kSfcNbRxDesc = 64;

// BEFORE prepare(): set knobs + resolve the BDF while the netdev still exists (vfio
// unbind removes it). setDevargs must land before prepare() registers the BDF.
template<class Nic>
[[nodiscard]] inline std::string dpdkPreInit(Nic& nic, const RoleConfig& role) {
    const std::string bdf = pci::resolveBdf(role.interface);
    if (role.driver == "igc" && !role.dpdkAfxdpPmd) {
        if (kIgcAdvertise1GOnly) nic.setLinkSpeeds(RTE_ETH_LINK_SPEED_1G);
        nic.setSymmetricQueues(true);        // nb_txq=0 silently breaks igc RX (KDI §2.2)
    }
    if (role.driver.find("mlx5") != std::string::npos && !role.dpdkAfxdpPmd) {
        nic.setDevargs(kMlx5LatencyDevargs);
        nic.setTxInlineReuse(true);
        nic.setNbRxDesc(kMlx5NbRxDesc);      // shrinks the in-poll refill burst 64 -> 16
    }
    if (role.driver == "sfc" && !role.dpdkAfxdpPmd) {
        nic.setDevargs(kSfcLatencyDevargs);  // no inline reuse — see kSfcLatencyDevargs
        nic.setNbRxDesc(kSfcNbRxDesc);       // shrinks the in-poll refill burst 32 -> 8
    }
    return bdf;
}

// AFTER dev_start (a reset during start wipes these). "i40e" covers both the vfio PMD
// and DPDK-over-AF_XDP — same silicon, same policy. BDF passed explicitly (see above).
inline void dpdkPostInitFixes(const std::string& bdf, const RoleConfig& role) {
    if (role.driver == "i40e")
        i40eApplyWritebackPolicy(bdf, role.interface, "DPDK-ITR");
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

    // DPDK's own --single lifecycle: EAL is process-global — register BOTH ports before
    // the first init() triggers rte_eal_init; a loopback link needs both PHYs up, so
    // start both (init(false) = no link wait), then wait both. One core drives both. KDI §3.
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
// RAW_PACKET QP on mlx5 — the sub-2us path: DPDK's dispatch minus ethdev/mbuf, same
// silicon. mlx5-only; port stays on the kernel driver, admin-UP; driver/lcore ignored.
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

// ── ef_vi ────────────────────────────────────────────────────────────────────
// Solarflare/AMD kernel bypass on the X2522 (EF10): CTPIO TX writes the frame through
// a write-combined MMIO aperture straight into the NIC TX FIFO. Bifurcated like verbs
// (kernel sfc keeps the port, admin-UP), no EAL/vfio; driver/lcore ignored. Needs
// onload + sfc_char + sfc_resource and root. The DPDK comparison (sfc PMD) needs a
// vfio bind — the two cannot hold the card at once.

// Cut-through threshold (bytes buffered before emit): >= frame_len = store-and-forward
// for that frame (frame 64 @ threshold 64 => no underrun/poison possible);
// EF_VI_CTPIO_CT_THRESHOLD_SNF (0xffff) = S&F at every size. Live lever at 128B frames.
inline constexpr unsigned kEfViCtThreshold = 64;
// false = DMA-descriptor sends through the same VI (doorbell + NIC buffer read) — the
// sfc-PMD datapath class. A/B 2026-07-16: CTPIO 1.854us median / DMA 3.417 / DPDK-sfc
// 3.503 => CTPIO-vs-DMA (1.563us RTT) is the datapath; DMA-vs-DPDK (86ns) is the DPDK
// framework tax. docs/Benchmarks.md.
inline constexpr bool kEfViUseCtpio = true;

struct EtherFabricTraits : TransportBase<EtherFabricTraits> {
    static constexpr std::string_view kName = "ef_vi";

    using TxOnly = EtherFabricVirtualInterface<EtherFabricMode::TxOnly, 256, 8, 2048, kEfViCtThreshold, kEfViUseCtpio>;
    using RxOnly = EtherFabricVirtualInterface<EtherFabricMode::RxOnly, 256, 8, 2048, kEfViCtThreshold, kEfViUseCtpio>;
    using RxTx   = EtherFabricVirtualInterface<EtherFabricMode::RxTx,   256, 8, 2048, kEfViCtThreshold, kEfViUseCtpio>;

    static TxOnly makeTx(const TestConfig&, const RoleConfig& role) { return TxOnly(role.interface); }
    static RxOnly makeRx(const TestConfig&, const RoleConfig& role) { return RxOnly(role.interface); }
    static RxTx   makeRxTx(const TestConfig&, const RoleConfig& role) { return RxTx(role.interface); }

    template<class Nic>
    static bool init(Nic& nic, const TestConfig&, const RoleConfig&) {
        return nic.init();
    }

    static void banner(const TestConfig&, const RoleConfig& role, const char* roleName) {
        fmt::println("[{}] Transport: ef_vi on {} (CTPIO)", roleName, role.interface);
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
