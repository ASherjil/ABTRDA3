// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Areeb Sherjil

#ifndef ABTRDA3_ETHERFABRICVIRTUALINTERFACE_HPP
#define ABTRDA3_ETHERFABRICVIRTUALINTERFACE_HPP

#include <net/if.h>
#include <sys/mman.h>

#include <etherfabric/capabilities.h>
#include <etherfabric/ef_vi.h>
#include <etherfabric/memreg.h>
#include <etherfabric/pd.h>
#include <etherfabric/vi.h>

// Older glibc <sys/mman.h> lacks the explicit-size hugetlb flags (21 = log2(2MiB))
#ifndef MAP_HUGE_SHIFT
#define MAP_HUGE_SHIFT 26
#endif
#ifndef MAP_HUGE_2MB
#define MAP_HUGE_2MB (21 << MAP_HUGE_SHIFT)
#endif

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include <fmt/format.h>

#include "../common/Profiling.hpp"

// =============================================================================
// EtherFabricVirtualInterface — ef_vi (Solarflare/AMD kernel bypass) transport.
//
// WHY: the X2522-25G-PLUS is the third 25G silicon in the campaign (mlx5 verbs
// 1.266us, tuned DPDK/mlx5 3.379us median RTT). ef_vi is Solarflare's native
// datapath — the layer Onload itself is built on — and its CTPIO TX writes the
// frame THROUGH a write-combined MMIO aperture straight into the NIC's TX FIFO:
// no descriptor, no doorbell, no NIC DMA read in the send path. The vendor's
// own eflatency measures ~1.86us RTT on this card; this transport must match
// that number through the same API, then race DPDK's sfc PMD (vfio, full
// userspace) on identical silicon.
//
// SCOPE: EF10-class Solarflare NICs (X2522 = SFC9250/Medford2) via Onload 9.0.2
// (/dev/sfc_char + libciul1). The port STAYS on the kernel sfc driver and must
// be admin-UP (like verbs/mlx5 — no vfio, no unbind). X3/EFCT's rx_ref datapath
// is deliberately NOT implemented; init() refuses an EFCT adapter. Requires
// root (VI allocation) and the onload/sfc_char/sfc_resource modules loaded.
//
// DESIGN:
//   * One ef_pd + ef_vi per port object (pd_flags = 0: EF_PD_EXPRESS is an X4
//     concept). One event queue carries BOTH RX events and TX completions.
//   * RX = CLASSIC descriptor model: NbRxBufs slots posted via
//     ef_vi_receive_post(dma_id = slot index); EF_EVENT_TYPE_RX returns the id,
//     release() re-posts that one slot — same shape as every other transport.
//     EF10 pushes RX descriptors to hardware in MULTIPLES OF 8, so single
//     re-posts coalesce silently until 8 accumulate; with NbRxBufs posted the
//     ring never starves (it just trails by <8 slots).
//   * TX = CTPIO with the MANDATORY paired fallback: every
//     ef_vi_transmit_ctpio() MUST be followed by ef_vi_transmit_ctpio_fallback()
//     — on EF10 the fallback IS the plain DMA descriptor that commits the send
//     and generates the TX completion; skipping it breaks the send entirely.
//     If the NIC times out mid-CTPIO (slow writer, oversize frame) it falls
//     back to DMA-reading that descriptor's buffer, so the frame goes out
//     either way. EF_EVENT_TX_CTPIO on the completion says which path won —
//     counted and reported at shutdown (wins vs fallbacks).
//   * TX buffers ROTATE through NbTxBufs slots, reclaimed by TX completions —
//     a single reused slot (the Verbs trick) is UNSAFE here: verbs
//     IBV_SEND_INLINE copies the frame into the WQE so the buffer is free on
//     return, but the CTPIO fallback descriptor points AT our buffer and the
//     NIC may DMA-read it any time until its completion arrives. Serial
//     ping-pong never notices; pipelined --single/--txgen would overwrite a
//     buffer mid-DMA and put corrupt frames on the wire.
//   * The evq cursor {m_evs, m_nEv, m_evIdx} PERSISTS across calls —
//     ef_eventq_poll returns several events and tryReceive() must hand back ONE
//     frame, so unprocessed events (including the TX completions that free TX
//     slots) stay in the cursor for the next call. Draining is reachable from
//     BOTH sides: tryReceive() walks it, and acquire()/commit() walk it too
//     (TX-only walk that stops at an RX event and leaves it for tryReceive) —
//     required in TxOnly mode where nobody calls tryReceive(), and when the
//     fallback ring is momentarily full (-EAGAIN).
//   * FILTER = dst-MAC (ef_filter_spec_set_eth_local, VLAN_ANY) — the exact
//     counterpart of the Verbs flow rule. NOT an ethertype filter: firmware
//     silently accepts-and-ignores eth_type filters for IPv4/IPv6 (vi.h:
//     "will return no error"), so MAC steering is the one option that can never
//     silently no-op. SFC filters STEER (not tee): the kernel stops seeing
//     matching frames while the VI exists; they revert on ef_vi_free.
//
// CTPIO THRESHOLD (CtThreshold): bytes the NIC buffers before starting to emit.
//   >= frame_len => store-and-forward for that frame (no underrun possible);
//   EF_VI_CTPIO_CT_THRESHOLD_SNF (0xffff) => S&F at every size. The libciul
//   writer paces itself for a 10G link (ctpio.c "Supporting 10Gbit link only
//   for now"), SLOWER than a 25G MAC drains, so true cut-through on 25G can
//   underrun => the NIC poisons the frame (bad FCS) and re-sends the good copy
//   from the fallback — the peer sees the poison as EF_EVENT_TYPE_RX_DISCARD /
//   CRC_BAD (counted, re-posted, reported at shutdown). At frame_size 64 the
//   default threshold 64 is already >= frame_len, so poison is impossible;
//   at 128B frames this becomes a live A/B (64 vs SNF vs NO_POISON).
//
// TUNABLES (compile-time template params; only the interface name is runtime
// config): NbRxBufs, NbTxBufs (power of two), BufSize (slot stride),
// CtThreshold, UseCtpio (false = plain DMA descriptor sends, the doorbell
// baseline for the CTPIO-vs-DMA A/B on identical silicon). Satisfies the
// TxRing/RxRing concepts (tryReceive/release + acquire/commit/send/prefillRing)
// like every other transport.
//
// GOTCHAS:
//   * The CTPIO frame buffer is reusable the moment ef_vi_transmit_ctpio
//     returns — but the FALLBACK buffer is not (see slot rotation above).
//   * EF_EVENT_RX_BYTES includes the RX prefix; the frame starts at
//     slot + ef_vi_receive_prefix_len() (0 with default flags, nonzero if
//     timestamps/event-merge engage).
//   * HW timestamps are deliberately OFF: the campaign times every transport
//     with rdtscp so instrument != transport (HwTimestamps=false).
//   * -EPERM from ef_vi_alloc_from_pd = firmware forcing event merging; retried
//     with EF_VI_RX_EVENT_MERGE (logged loudly — merge is a throughput feature
//     and RX then arrives as EF_EVENT_TYPE_RX_MULTI).
//   * A discard consumes the RX descriptor — every discard path re-posts.
//   * shutdown() order: vi (removes filters) -> memreg -> pd -> driver handle.
// =============================================================================

enum class EtherFabricMode : std::uint8_t {
    RxOnly,
    TxOnly,
    RxTx
};

// RX payload polling (event-queue bypass). On EF10 the NIC DMAs the FRAME into
// the posted buffer first and composes the EVQ event as a SEPARATE later write —
// everyone who waits on the EVQ pays for that second DMA plus event decode.
// RX buffers complete in strict FIFO ring order, so tryReceive() instead spins
// on the next buffer's ETHERTYPE (offset 12 — the only field our raw-L2 protocol
// guarantees nonzero; MAC bytes and seq can legitimately be 0). release() clears
// the marker before re-posting. The EVQ still gets drained on every poll miss —
// TX completions and counters ride there — but RX events become bookkeeping
// no-ops. Trade-offs, deliberate for this point-to-point CRC-clean rig:
//   * discard filtering is bypassed — a bad frame would be delivered before its
//     discard event is seen (rig history: 0 discards in 437M+ samples; the
//     discard counter still runs and flags any occurrence at shutdown);
//   * frame length is not known at delivery (span covers the whole slot);
//   * only bytes 0..63 (the marker's cacheline/TLP) are guaranteed present at
//     detection — fine for this protocol, which never reads past byte 25;
//   * the EVQ drain rides the poll MISS path — a saturating RX stream with no
//     misses (RxOnly at line rate) would starve it until the EVQ overflows
//     (visible as unexpected_events). RxTx ping-pong misses every round.
inline constexpr bool kEfViRxPayloadPoll = true;

template <EtherFabricMode M, std::uint16_t NbRxBufs = 256, std::uint16_t NbTxBufs = 8,
          std::uint32_t BufSize = 2048, unsigned CtThreshold = 64, bool UseCtpio = true,
          bool HwTimestamps = false>
class EtherFabricVirtualInterface {
    static constexpr bool HAS_RX = (M == EtherFabricMode::RxOnly || M == EtherFabricMode::RxTx);
    static constexpr bool HAS_TX = (M == EtherFabricMode::TxOnly || M == EtherFabricMode::RxTx);

    static_assert((NbTxBufs & (NbTxBufs - 1)) == 0, "NbTxBufs must be a power of two");
    static_assert((NbRxBufs & (NbRxBufs - 1)) == 0,
                  "NbRxBufs must be a power of two (payload-poll FIFO cursor wraps with & mask)");
    static_assert(NbRxBufs % 8 == 0, "EF10 pushes RX descriptors in multiples of 8");
    static_assert(BufSize >= 64, "slot must hold a minimum ethernet frame");

    static constexpr int kEvPollBatch = 8;

    static constexpr std::uint32_t kRxPrefixTsOffset = 10;
    static constexpr std::uint32_t kOneSecQns        = 4000000000u;
    static constexpr std::uint32_t kQnsOverrun       = 20;
    static_assert(kEvPollBatch >= EF_VI_EVENT_POLL_MIN_EVS);

    // One unused guard slot on EACH side of the TX region. Measured (2x 5-min
    // histogram runs, 144M sends each): CTPIO noncontig fallbacks concentrate
    // 94-98% on the EDGE TX slots — client slot 0 (its cacheline neighborhood
    // abuts the DDIO-hot RX region) and server slot NbTxBufs-1 (abuts foreign
    // heap) — deterministically across runs, with per-run severity from ~150ppm
    // to ~100% of that slot's sends. The guards make every live TX slot interior.
    static constexpr std::uint32_t kTxGuardSlots = 1;

public:
    explicit EtherFabricVirtualInterface(std::string_view ifname) noexcept;
    ~EtherFabricVirtualInterface();

    EtherFabricVirtualInterface(const EtherFabricVirtualInterface&)            = delete;
    EtherFabricVirtualInterface& operator=(const EtherFabricVirtualInterface&) = delete;
    EtherFabricVirtualInterface(EtherFabricVirtualInterface&&)                 = delete;
    EtherFabricVirtualInterface& operator=(EtherFabricVirtualInterface&&)      = delete;

    [[nodiscard]] bool init() noexcept;
    void               shutdown() noexcept;

    [[nodiscard]] std::array<std::uint8_t, 6> macAddress() const noexcept;

    void prefillRing(std::span<const std::uint8_t> frameTemplate) noexcept
        requires (M == EtherFabricMode::TxOnly || M == EtherFabricMode::RxTx);

    [[nodiscard, gnu::always_inline, gnu::hot]]
    inline std::uint8_t* acquire(std::uint32_t frameLen) noexcept
        requires (M == EtherFabricMode::TxOnly || M == EtherFabricMode::RxTx);

    [[gnu::always_inline, gnu::hot]]
    inline void commit() noexcept
        requires (M == EtherFabricMode::TxOnly || M == EtherFabricMode::RxTx);

    [[nodiscard, gnu::always_inline, gnu::hot]]
    inline bool send(std::span<const std::uint8_t> frame) noexcept
        requires (M == EtherFabricMode::TxOnly || M == EtherFabricMode::RxTx);

    [[nodiscard, gnu::always_inline, gnu::hot]]
    inline std::span<const std::uint8_t> tryReceive() noexcept
        requires (M == EtherFabricMode::RxOnly || M == EtherFabricMode::RxTx);

    [[gnu::always_inline, gnu::hot]]
    inline void release() noexcept
        requires (M == EtherFabricMode::RxOnly || M == EtherFabricMode::RxTx);

    [[nodiscard, gnu::always_inline]]
    std::uint32_t hwRxTimestamp() const noexcept
        requires (HwTimestamps && (M == EtherFabricMode::RxOnly || M == EtherFabricMode::RxTx))
    {
        return m_rxTsRaw;
    }

    struct TxStamp {
        std::uint32_t seq;
        std::uint32_t minor;
    };

    [[nodiscard, gnu::always_inline]]
    std::optional<TxStamp> pollTxTimestamp() noexcept
        requires (HwTimestamps && (M == EtherFabricMode::TxOnly || M == EtherFabricMode::RxTx))
    {
        if (m_txTsCursor == m_txTail) {
            return std::nullopt;
        }
        const std::uint64_t entry = m_txStamp[m_txTsCursor & (NbTxBufs - 1)];
        const std::uint32_t seq   = static_cast<std::uint32_t>(entry >> 32);
        if (seq != m_txTsCursor) [[unlikely]] {
            ++m_txTsOverflow;
            m_txTsCursor = m_txTail;
            return std::nullopt;
        }
        ++m_txTsCursor;
        return TxStamp{seq, static_cast<std::uint32_t>(entry)};
    }

    [[nodiscard]] std::uint32_t hwRxTimestampCorrection() const noexcept
        requires (HwTimestamps)
    {
        return m_rxTsCorrection;
    }

    [[nodiscard]] static std::optional<std::uint64_t> hwRoundTrip(std::uint64_t packed,
                                                                  std::uint32_t rxCorrection) noexcept
        requires (HwTimestamps)
    {
        const std::optional<std::uint32_t> rx = hwFoldRx(static_cast<std::uint32_t>(packed >> 32),
                                                         rxCorrection);
        if (!rx) {
            return std::nullopt;
        }
        return hwDelta(*rx, static_cast<std::uint32_t>(packed));
    }

    [[nodiscard]] static std::optional<std::uint64_t> hwTurnaround(std::uint64_t packed,
                                                                   std::uint32_t rxCorrection) noexcept
        requires (HwTimestamps)
    {
        const std::optional<std::uint32_t> rx = hwFoldRx(static_cast<std::uint32_t>(packed >> 32),
                                                         rxCorrection);
        if (!rx) {
            return std::nullopt;
        }
        return hwDelta(static_cast<std::uint32_t>(packed), *rx);
    }

private:
    struct RxEv {
        std::uint32_t id;
        std::uint32_t len;
    };

    [[nodiscard]] [[gnu::always_inline]] std::uint8_t* rxSlot(std::uint32_t i) const noexcept;
    [[nodiscard]] [[gnu::always_inline]] std::uint8_t* txSlot(std::uint32_t s) const noexcept;
    [[gnu::hot]] inline bool                           pollEvent(RxEv& out, bool wantRx) noexcept;
    [[gnu::hot]] inline void readRxTimestamp(const std::uint8_t* prefixBase) noexcept;

    [[nodiscard]] static std::optional<std::uint32_t> hwFoldRx(std::uint32_t raw,
                                                               std::uint32_t correction) noexcept {
        if (raw == 0xFFFFFFFFu) {
            return std::nullopt;
        }
        std::uint32_t minor = raw + correction;
        if (minor >= kOneSecQns) {
            if (minor < kOneSecQns + kQnsOverrun + 2) {
                minor -= kOneSecQns;
            } else {
                minor += kOneSecQns;
            }
        }
        return minor;
    }

    [[nodiscard]] static std::uint64_t hwDelta(std::uint32_t end, std::uint32_t begin) noexcept {
        std::int64_t d = static_cast<std::int64_t>(end) - static_cast<std::int64_t>(begin);
        if (d < 0) {
            d += kOneSecQns;
        }
        return static_cast<std::uint64_t>(d);
    }

    inline void        handleDiscard(std::uint32_t id, unsigned subtype) noexcept;
    [[nodiscard]] bool readMac() noexcept;

    [[nodiscard]] [[gnu::always_inline]] bool evqHasEvent() const noexcept {
        const std::uint32_t           off  = m_vi.ep_state->evq.evq_ptr & m_vi.evq_mask;
        const volatile std::uint64_t* slot = reinterpret_cast<const volatile std::uint64_t*>(m_vi.evq_base +
                                                                                             off);
        const std::uint64_t           v    = *slot;
        return (static_cast<std::uint32_t>(v) != 0xFFFFFFFFu) &&
               (static_cast<std::uint32_t>(v >> 32) != 0xFFFFFFFFu);
    }

    std::string                 m_ifname;
    std::array<std::uint8_t, 6> m_mac{};

    ef_driver_handle m_dh{-1};
    ef_pd            m_pd{};
    ef_vi            m_vi{};
    ef_memreg        m_memreg{};
    bool             m_haveDriver{false};
    bool             m_havePd{false};
    bool             m_haveVi{false};
    bool             m_haveMemreg{false};
    bool             m_evMerge{false};

    std::uint8_t* m_mem{nullptr};
    std::size_t   m_memBytes{0};
    bool          m_memHuge{false};
    int           m_rxPrefix{0};

    std::array<ef_addr, NbRxBufs> m_rxDma{};
    std::array<ef_addr, NbTxBufs> m_txDma{};

    ef_event m_evs[kEvPollBatch]{};
    int      m_nEv{0};
    int      m_evIdx{0};

    std::uint32_t m_heldId{0};
    std::uint32_t m_rxNextIdx{0};   // payload-poll FIFO cursor: next buffer to complete
    std::uint32_t m_rxPendingPush{0};
    std::uint32_t m_txHead{0};
    std::uint32_t m_txTail{0};
    std::uint32_t m_txSlot{0};
    std::uint32_t m_txPad{0};
    std::uint32_t m_txBase{NbRxBufs + kTxGuardSlots};
    std::uint32_t m_txLen{0};
    std::uint8_t* m_txPtr{nullptr};   // acquire() computes txSlot() once; commit() reuses it

    std::uint64_t                       m_ctpioWins{0};
    std::uint64_t                       m_ctpioFallbacks{0};
    std::array<std::uint64_t, NbTxBufs> m_fallbackPerSlot{};
    std::uint64_t                       m_rxPollHits{0};       // payload-poll deliveries
    std::uint64_t                       m_rxEvReconciled{0};   // RX events consumed as bookkeeping no-ops
    std::uint64_t                       m_rxDiscards{0};
    std::uint64_t                       m_rxCrcBad{0};
    std::uint64_t                       m_rxDropped{0};
    std::uint64_t                       m_txErrors{0};
    std::uint64_t                       m_evUnexpected{0};

    std::uint32_t                       m_rxTsRaw{0};
    std::uint32_t                       m_rxTsCorrection{0};
    std::array<std::uint64_t, NbTxBufs> m_txStamp{};
    std::uint32_t                       m_txTsCursor{0};
    std::uint64_t                       m_txTsOverflow{0};
    std::uint64_t                       m_txTsEvents{0};
    std::uint64_t                       m_tsVerified{0};
    std::uint64_t                       m_tsMismatch{0};
    std::uint64_t                       m_tsLibFail{0};
};

// =============================================================================

template <EtherFabricMode M, std::uint16_t NbRxBufs, std::uint16_t NbTxBufs, std::uint32_t BufSize,
          unsigned CtThreshold, bool UseCtpio, bool HwTimestamps>
EtherFabricVirtualInterface<M, NbRxBufs, NbTxBufs, BufSize, CtThreshold, UseCtpio,
                            HwTimestamps>::EtherFabricVirtualInterface(std::string_view ifname) noexcept
    : m_ifname{ifname} {
}

template <EtherFabricMode M, std::uint16_t NbRxBufs, std::uint16_t NbTxBufs, std::uint32_t BufSize,
          unsigned CtThreshold, bool UseCtpio, bool HwTimestamps>
EtherFabricVirtualInterface<M, NbRxBufs, NbTxBufs, BufSize, CtThreshold, UseCtpio,
                            HwTimestamps>::~EtherFabricVirtualInterface() {
    shutdown();
}

template <EtherFabricMode M, std::uint16_t NbRxBufs, std::uint16_t NbTxBufs, std::uint32_t BufSize,
          unsigned CtThreshold, bool UseCtpio, bool HwTimestamps>
bool EtherFabricVirtualInterface<M, NbRxBufs, NbTxBufs, BufSize, CtThreshold, UseCtpio,
                                 HwTimestamps>::init() noexcept {
    if (!readMac()) {
        return false;
    }

    const unsigned ifindex = if_nametoindex(m_ifname.c_str());
    if (ifindex == 0) {
        fmt::println(stderr, "[ef_vi] {}: no such interface (port must stay on the kernel sfc driver)",
                     m_ifname);
        return false;
    }

    if (ef_driver_open(&m_dh) != 0) {
        fmt::println(
            stderr,
            "[ef_vi] {}: ef_driver_open failed (/dev/sfc_char — onload/sfc_char modules loaded? root?)",
            m_ifname);
        return false;
    }
    m_haveDriver = true;

    if (ef_pd_alloc(&m_pd, m_dh, static_cast<int>(ifindex), static_cast<enum ef_pd_flags>(0)) != 0) {
        fmt::println(stderr, "[ef_vi] {}: ef_pd_alloc failed", m_ifname);
        return false;
    }
    m_havePd = true;

    enum ef_vi_flags viFlags = EF_VI_FLAGS_DEFAULT;
    if constexpr (HAS_TX && UseCtpio) {
        unsigned long cap = 0;
        if (ef_vi_capabilities_get(m_dh, static_cast<int>(ifindex), EF_VI_CAP_CTPIO, &cap) != 0 || cap == 0) {
            fmt::println(stderr,
                         "[ef_vi] {}: NIC does not support CTPIO (set UseCtpio=false for the DMA path)",
                         m_ifname);
            return false;
        }
        viFlags = static_cast<enum ef_vi_flags>(viFlags | EF_VI_TX_CTPIO);
    }

    if constexpr (HwTimestamps) {
        unsigned long cap = 0;
        if constexpr (HAS_RX) {
            if (ef_vi_capabilities_get(m_dh, static_cast<int>(ifindex), EF_VI_CAP_HW_RX_TIMESTAMPING, &cap) !=
                    0 ||
                cap == 0) {
                fmt::println(stderr, "[ef_vi] {}: NIC does not report HW RX timestamping", m_ifname);
                return false;
            }
            viFlags = static_cast<enum ef_vi_flags>(viFlags | EF_VI_RX_TIMESTAMPS);
        }
        if constexpr (HAS_TX) {
            if (ef_vi_capabilities_get(m_dh, static_cast<int>(ifindex), EF_VI_CAP_HW_TX_TIMESTAMPING, &cap) !=
                    0 ||
                cap == 0) {
                fmt::println(stderr, "[ef_vi] {}: NIC does not report HW TX timestamping", m_ifname);
                return false;
            }
            viFlags = static_cast<enum ef_vi_flags>(viFlags | EF_VI_TX_TIMESTAMPS);
        }
    }

    int rc = ef_vi_alloc_from_pd(&m_vi, m_dh, &m_pd, m_dh, -1, -1, -1, nullptr, -1, viFlags);
    if constexpr (HwTimestamps) {
        if (rc == -ENOKEY) {
            fmt::println(stderr, "[ef_vi] {}: ef_vi_alloc_from_pd -ENOKEY — the adapter is not licensed to",
                         m_ifname);
            fmt::println(stderr,
                         "        subscribe to time-sync events (firmware EPERM). Check: sudo sfkey "
                         "--adapter={} --report",
                         m_ifname);
            return false;
        }
    }
    if (rc == -EPERM) {
        fmt::println(stderr,
                     "[ef_vi] {}: firmware forces RX event merging — retrying with EF_VI_RX_EVENT_MERGE "
                     "(throughput mode, NOT the latency config)",
                     m_ifname);
        viFlags   = static_cast<enum ef_vi_flags>(viFlags | EF_VI_RX_EVENT_MERGE);
        rc        = ef_vi_alloc_from_pd(&m_vi, m_dh, &m_pd, m_dh, -1, -1, -1, nullptr, -1, viFlags);
        m_evMerge = true;
    }
    if (rc != 0) {
        fmt::println(stderr, "[ef_vi] {}: ef_vi_alloc_from_pd failed ({})", m_ifname, rc);
        return false;
    }
    m_haveVi = true;

    if (m_vi.nic_type.arch == EF_VI_ARCH_EFCT) {
        fmt::println(stderr, "[ef_vi] {}: EFCT/X3 adapter — the rx_ref datapath is not implemented here",
                     m_ifname);
        return false;
    }

    // All buffer slots live in explicit 2MiB hugetlb pages (one page fits the
    // default 265-slot config). With 4K heap pages, every process start drew fresh
    // page placement per slot, and that draw set the CTPIO tear severity of the
    // phase-chosen slot (150ppm..100% — the per-run lottery). A 2M-aligned page
    // fixes every address bit below 2M across runs. The 2M size MUST be explicit:
    // rtserver's default hugepagesz is 1G (the DPDK pool). The 2M pool needs
    // creating once per boot:
    //   echo 8 | sudo tee /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages
    constexpr std::size_t kHugePage = static_cast<std::size_t>(2u * 1024u * 1024u);

    // TX region pad. The rotation-origin sweep proved the CTPIO tear is bound to
    // an ADDRESS (a fragile cell at a fixed offset within the 2M page — slot 5's
    // 0x83000 in the default layout), NOT to the doorbell phase: the victim slot
    // never moved with the rotation origin. This knob slides the whole TX region
    // by N slots (N x BufSize bytes) inside the page so no TX slot occupies a
    // fragile cell. Init-time only. ABTRDA3_TX_PAD=<slots>, per-port override
    // ABTRDA3_TX_PAD_<ifname>. Prediction of the cell model: victim index
    // decrements as pad grows (pad=1 -> slot 4 ...), histogram flat once the
    // region clears the cell (pad >= 6 for the default layout).
    if constexpr (HAS_TX) {
        const std::string padPerPort = fmt::format("ABTRDA3_TX_PAD_{}", m_ifname);
        const char*       pad        = std::getenv(padPerPort.c_str());
        if (pad == nullptr) {
            pad = std::getenv("ABTRDA3_TX_PAD");
        }
        if (pad != nullptr) {
            m_txPad = static_cast<std::uint32_t>(std::atoi(pad));
            m_txPad = std::min<uint32_t>(m_txPad, 512);
        }
    }
    m_txBase = NbRxBufs + kTxGuardSlots + m_txPad;

    const std::size_t slotBytes =
        static_cast<std::size_t>(NbRxBufs + NbTxBufs + 2 * kTxGuardSlots + m_txPad) * BufSize;
    m_memBytes = (slotBytes + kHugePage - 1) & ~(kHugePage - 1);
    m_mem      = static_cast<std::uint8_t*>(mmap(nullptr, m_memBytes, PROT_READ | PROT_WRITE,
                                                 MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB | MAP_HUGE_2MB, -1, 0));
    if (m_mem == MAP_FAILED) {
        m_mem = nullptr;
        fmt::println(stderr, "[ef_vi] {}: 2MiB hugepage alloc failed ({} bytes) — empty 2M pool? create it:",
                     m_ifname, m_memBytes);
        fmt::println(stderr,
                     "        echo 8 | sudo tee /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages");
        return false;
    }
    m_memHuge = true;
    std::memset(m_mem, 0, m_memBytes);

    if (ef_memreg_alloc(&m_memreg, m_dh, &m_pd, m_dh, m_mem, m_memBytes) != 0) {
        fmt::println(stderr, "[ef_vi] {}: ef_memreg_alloc failed", m_ifname);
        return false;
    }
    m_haveMemreg = true;

    for (std::uint32_t i = 0; i < NbRxBufs; ++i) {
        m_rxDma[i] = ef_memreg_dma_addr(&m_memreg, static_cast<std::size_t>(i) * BufSize);
    }
    for (std::uint32_t s = 0; s < NbTxBufs; ++s) {
        m_txDma[s] = ef_memreg_dma_addr(&m_memreg, static_cast<std::size_t>(m_txBase + s) * BufSize);
    }

    if constexpr (HAS_RX) {
        ef_filter_spec fs;
        ef_filter_spec_init(&fs, EF_FILTER_FLAG_NONE);
        if (ef_filter_spec_set_eth_local(&fs, EF_FILTER_VLAN_ID_ANY, m_mac.data()) != 0 ||
            ef_vi_filter_add(&m_vi, m_dh, &fs, nullptr) != 0) {
            fmt::println(stderr, "[ef_vi] {}: dst-MAC filter add failed", m_ifname);
            return false;
        }
        for (std::uint32_t i = 0; i < NbRxBufs && ef_vi_receive_space(&m_vi) > 0; ++i) {
            if (ef_vi_receive_post(&m_vi, m_rxDma[i], static_cast<ef_request_id>(i)) != 0) {
                break;
            }
        }
        m_rxPrefix = ef_vi_receive_prefix_len(&m_vi);
        if constexpr (HwTimestamps) {
            if (m_rxPrefix == 0) {
                fmt::println(stderr,
                             "[ef_vi] {}: RX timestamps requested but prefix_len==0 — refusing to run",
                             m_ifname);
                return false;
            }
            if (m_vi.ts_format != TS_FORMAT_SECONDS_QTR_NANOSECONDS) {
                fmt::println(
                    stderr,
                    "[ef_vi] {}: adapter timestamp format {} is not quarter-nanoseconds — refusing to run",
                    m_ifname, static_cast<int>(m_vi.ts_format));
                return false;
            }
            m_rxTsCorrection = static_cast<std::uint32_t>(m_vi.rx_ts_correction);
        }
    }

    // The mode libciul's CTPIO writer will use — read from OUR environment, the
    // same place ef_vi_transmit_ctpio reads it. Prints the truth even when a
    // launcher script (sudo env_reset!) silently strips the variable.
    const char* ctpioModeEnv = std::getenv("EF_VI_CTPIO_MODE");
    fmt::println(stderr,
                 "[ef_vi] {} ready — {} arch={} rxq {} txq {} tx={} ct_thresh={} prefix {}B bufs=2M-huge "
                 "txpad={} hw_ts={} ctpio_mode={} rx={}{}",
                 m_ifname, ef_vi_version_str(), m_vi.nic_type.arch == EF_VI_ARCH_EF10 ? "EF10" : "other",
                 NbRxBufs, NbTxBufs, UseCtpio ? "CTPIO" : "DMA", UseCtpio ? CtThreshold : 0U, m_rxPrefix,
                 m_txPad, HwTimestamps ? (HAS_RX && HAS_TX ? "rx+tx" : (HAS_RX ? "rx" : "tx")) : "off",
                 ctpioModeEnv != nullptr ? ctpioModeEnv : "default(paced)",
                 kEfViRxPayloadPoll ? "payload-poll" : "evq", m_evMerge ? " [EVENT-MERGE FORCED]" : "");
    return true;
}

template <EtherFabricMode M, std::uint16_t NbRxBufs, std::uint16_t NbTxBufs, std::uint32_t BufSize,
          unsigned CtThreshold, bool UseCtpio, bool HwTimestamps>
void EtherFabricVirtualInterface<M, NbRxBufs, NbTxBufs, BufSize, CtThreshold, UseCtpio,
                                 HwTimestamps>::shutdown() noexcept {
    if (m_haveVi) {
        if constexpr (HAS_TX) {
            if constexpr (UseCtpio) {
                fmt::println(stderr, "[ef_vi] {}: tx ctpio_wins={} ctpio_fallbacks={} tx_errors={}", m_ifname,
                             m_ctpioWins, m_ctpioFallbacks, m_txErrors);
                if (m_ctpioFallbacks > 0) {
                    std::string slots;
                    for (std::uint32_t s = 0; s < NbTxBufs; ++s) {
                        slots += fmt::format("{}{}", s == 0 ? "" : " ", m_fallbackPerSlot[s]);
                    }
                    fmt::println(stderr, "[ef_vi] {}: fallbacks_per_slot=[{}]", m_ifname, slots);
                }
            } else {
                fmt::println(stderr, "[ef_vi] {}: tx dma_sends={} tx_errors={}", m_ifname, m_ctpioFallbacks,
                             m_txErrors);
            }
        }
        if constexpr (HAS_RX) {
            fmt::println(stderr, "[ef_vi] {}: rx discards={} (crc_bad={}) dropped={} unexpected_events={}",
                         m_ifname, m_rxDiscards, m_rxCrcBad, m_rxDropped, m_evUnexpected);
            if constexpr (kEfViRxPayloadPoll) {
                // Sanity pair: every poll delivery should eventually produce one RX
                // event; a persistent gap means ring/cursor desync.
                fmt::println(stderr, "[ef_vi] {}: rx poll_hits={} events_reconciled={}", m_ifname,
                             m_rxPollHits, m_rxEvReconciled);
            }
        }
        if constexpr (HwTimestamps) {
            if constexpr (HAS_RX) {
                fmt::println(stderr, "[ef_vi] {}: hw_ts rx_ts_correction={}", m_ifname,
                             static_cast<std::int32_t>(m_rxTsCorrection));
                if constexpr (prof::kDebugProfiling) {
                    fmt::println(stderr, "[ef_vi] {}: hw_ts verify: checked={} mismatch={} lib_fail={}",
                                 m_ifname, m_tsVerified, m_tsMismatch, m_tsLibFail);
                }
            }
            if constexpr (HAS_TX) {
                fmt::println(stderr, "[ef_vi] {}: hw_ts tx_events={} tx_ts_overflow={}", m_ifname,
                             m_txTsEvents, m_txTsOverflow);
            }
        }
        ef_vi_free(&m_vi, m_dh);
        m_haveVi = false;
    }
    if (m_haveMemreg) {
        ef_memreg_free(&m_memreg, m_dh);
        m_haveMemreg = false;
    }
    if (m_mem) {
        if (m_memHuge) {
            munmap(m_mem, m_memBytes);
        } else {
            std::free(m_mem);
        }
        m_mem = nullptr;
    }
    if (m_havePd) {
        ef_pd_free(&m_pd, m_dh);
        m_havePd = false;
    }
    if (m_haveDriver) {
        ef_driver_close(m_dh);
        m_haveDriver = false;
    }
}

template <EtherFabricMode M, std::uint16_t NbRxBufs, std::uint16_t NbTxBufs, std::uint32_t BufSize,
          unsigned CtThreshold, bool UseCtpio, bool HwTimestamps>
std::array<std::uint8_t, 6> EtherFabricVirtualInterface<M, NbRxBufs, NbTxBufs, BufSize, CtThreshold, UseCtpio,
                                                        HwTimestamps>::macAddress() const noexcept {
    return m_mac;
}

template <EtherFabricMode M, std::uint16_t NbRxBufs, std::uint16_t NbTxBufs, std::uint32_t BufSize,
          unsigned CtThreshold, bool UseCtpio, bool HwTimestamps>
void EtherFabricVirtualInterface<M, NbRxBufs, NbTxBufs, BufSize, CtThreshold, UseCtpio, HwTimestamps>::
    prefillRing(std::span<const std::uint8_t> frameTemplate) noexcept
    requires (M == EtherFabricMode::TxOnly || M == EtherFabricMode::RxTx)
{
    for (std::uint32_t s = 0; s < NbTxBufs; ++s) {
        std::memcpy(txSlot(s), frameTemplate.data(), std::min<std::size_t>(frameTemplate.size(), BufSize));
    }
}

template <EtherFabricMode M, std::uint16_t NbRxBufs, std::uint16_t NbTxBufs, std::uint32_t BufSize,
          unsigned CtThreshold, bool UseCtpio, bool HwTimestamps>
inline std::uint8_t* EtherFabricVirtualInterface<M, NbRxBufs, NbTxBufs, BufSize, CtThreshold, UseCtpio,
                                                 HwTimestamps>::acquire(std::uint32_t frameLen) noexcept
    requires (M == EtherFabricMode::TxOnly || M == EtherFabricMode::RxTx)
{
    if (frameLen > BufSize) [[unlikely]] {
        return nullptr;
    }
    if (m_txHead - m_txTail >= NbTxBufs) [[unlikely]] {
        RxEv scratch{};
        pollEvent(scratch, false);
        if (m_txHead - m_txTail >= NbTxBufs) {
            return nullptr;
        }
    }
    m_txSlot = m_txHead & (NbTxBufs - 1);
    m_txLen  = frameLen;
    m_txPtr  = txSlot(m_txSlot);
    return m_txPtr;
}

template <EtherFabricMode M, std::uint16_t NbRxBufs, std::uint16_t NbTxBufs, std::uint32_t BufSize,
          unsigned CtThreshold, bool UseCtpio, bool HwTimestamps>
inline void EtherFabricVirtualInterface<M, NbRxBufs, NbTxBufs, BufSize, CtThreshold, UseCtpio,
                                        HwTimestamps>::commit() noexcept
    requires (M == EtherFabricMode::TxOnly || M == EtherFabricMode::RxTx)
{
    if constexpr (UseCtpio) {
        ef_vi_transmit_ctpio(&m_vi, m_txPtr, m_txLen, CtThreshold);
        for (;;) {
            const int rc = ef_vi_transmit_ctpio_fallback(&m_vi, m_txDma[m_txSlot], static_cast<int>(m_txLen),
                                                         static_cast<ef_request_id>(m_txSlot));
            if (rc == 0) [[likely]] {
                break;
            }
            if (rc != -EAGAIN) [[unlikely]] {
                ++m_txErrors;
                return;
            }
            RxEv scratch{};
            pollEvent(scratch, false);
        }
    } else {
        for (;;) {
            const int rc = ef_vi_transmit(&m_vi, m_txDma[m_txSlot], static_cast<int>(m_txLen),
                                          static_cast<ef_request_id>(m_txSlot));
            if (rc == 0) [[likely]] {
                break;
            }
            if (rc != -EAGAIN) [[unlikely]] {
                ++m_txErrors;
                return;
            }
            RxEv scratch;
            pollEvent(scratch, false);
        }
    }
    ++m_txHead;
}

template <EtherFabricMode M, std::uint16_t NbRxBufs, std::uint16_t NbTxBufs, std::uint32_t BufSize,
          unsigned CtThreshold, bool UseCtpio, bool HwTimestamps>
inline bool EtherFabricVirtualInterface<M, NbRxBufs, NbTxBufs, BufSize, CtThreshold, UseCtpio,
                                        HwTimestamps>::send(std::span<const std::uint8_t> frame) noexcept
    requires (M == EtherFabricMode::TxOnly || M == EtherFabricMode::RxTx)
{
    auto* dst = acquire(static_cast<std::uint32_t>(frame.size()));
    if (dst == nullptr) [[unlikely]] {
        return false;
    }
    std::memcpy(dst, frame.data(), frame.size());
    commit();
    return true;
}

template <EtherFabricMode M, std::uint16_t NbRxBufs, std::uint16_t NbTxBufs, std::uint32_t BufSize,
          unsigned CtThreshold, bool UseCtpio, bool HwTimestamps>
inline std::span<const std::uint8_t> EtherFabricVirtualInterface<
    M, NbRxBufs, NbTxBufs, BufSize, CtThreshold, UseCtpio, HwTimestamps>::tryReceive() noexcept
    requires (M == EtherFabricMode::RxOnly || M == EtherFabricMode::RxTx)
{
    if constexpr (kEfViRxPayloadPoll) {
        std::uint8_t*                 buf    = rxSlot(m_rxNextIdx);
        const volatile std::uint16_t* marker = reinterpret_cast<const volatile std::uint16_t*>(
            buf + m_rxPrefix + 12);
        if (*marker != 0) [[unlikely]] {
            m_heldId    = m_rxNextIdx;
            m_rxNextIdx = (m_rxNextIdx + 1) & (NbRxBufs - 1);
            ++m_rxPollHits;
            if constexpr (HwTimestamps) {
                readRxTimestamp(buf);
            }
            return {buf + m_rxPrefix, BufSize - static_cast<std::uint32_t>(m_rxPrefix)};
        }

        RxEv ev{};
        (void)pollEvent(ev, true);
        return {};
    } else {
        RxEv ev;
        if (!pollEvent(ev, true)) [[likely]] {
            return {};
        }
        m_heldId = ev.id;
        if constexpr (HwTimestamps) {
            readRxTimestamp(rxSlot(ev.id));
        }
        return {rxSlot(ev.id) + m_rxPrefix, ev.len - static_cast<std::uint32_t>(m_rxPrefix)};
    }
}

template <EtherFabricMode M, std::uint16_t NbRxBufs, std::uint16_t NbTxBufs, std::uint32_t BufSize,
          unsigned CtThreshold, bool UseCtpio, bool HwTimestamps>
inline void EtherFabricVirtualInterface<M, NbRxBufs, NbTxBufs, BufSize, CtThreshold, UseCtpio,
                                        HwTimestamps>::release() noexcept
    requires (M == EtherFabricMode::RxOnly || M == EtherFabricMode::RxTx)
{
    if constexpr (kEfViRxPayloadPoll) {
        std::uint8_t* slot = rxSlot(m_heldId);
        std::memset(slot + m_rxPrefix + 12, 0, 2);
        asm volatile("clflushopt %0" : : "m"(*reinterpret_cast<volatile char*>(slot)));
        asm volatile("clflushopt %0" : : "m"(*reinterpret_cast<volatile char*>(slot + 64)));
    }
    ef_vi_receive_init(&m_vi, m_rxDma[m_heldId], static_cast<ef_request_id>(m_heldId));
    ++m_rxPendingPush;
    if (m_rxPendingPush >= 64) [[unlikely]] {
        ef_vi_receive_push(&m_vi);
        m_rxPendingPush &= 7;
    }
}

template <EtherFabricMode M, std::uint16_t NbRxBufs, std::uint16_t NbTxBufs, std::uint32_t BufSize,
          unsigned CtThreshold, bool UseCtpio, bool HwTimestamps>
inline void
EtherFabricVirtualInterface<M, NbRxBufs, NbTxBufs, BufSize, CtThreshold, UseCtpio,
                            HwTimestamps>::readRxTimestamp(const std::uint8_t* prefixBase) noexcept {
    std::memcpy(&m_rxTsRaw, prefixBase + kRxPrefixTsOffset, sizeof m_rxTsRaw);
    if constexpr (prof::kDebugProfiling) {
        ef_precisetime ts{};
        if (ef_vi_receive_get_precise_timestamp(&m_vi, prefixBase, &ts) != 0) {
            ++m_tsLibFail;
        } else {
            ++m_tsVerified;
            const std::uint32_t lib = static_cast<std::uint32_t>(ts.tv_nsec) * 4u +
                                      (static_cast<std::uint32_t>(ts.tv_nsec_frac) >> 14);
            const std::optional<std::uint32_t> ours = hwFoldRx(m_rxTsRaw, m_rxTsCorrection);
            if (!ours || lib != *ours) {
                if (m_tsMismatch < 8) {
                    fmt::println(stderr, "[ef_vi] {}: hw_ts MISMATCH lib={} raw={}", m_ifname, lib,
                                 m_rxTsRaw);
                }
                ++m_tsMismatch;
            }
        }
    }
}

template <EtherFabricMode M, std::uint16_t NbRxBufs, std::uint16_t NbTxBufs, std::uint32_t BufSize,
          unsigned CtThreshold, bool UseCtpio, bool HwTimestamps>
inline std::uint8_t* EtherFabricVirtualInterface<M, NbRxBufs, NbTxBufs, BufSize, CtThreshold, UseCtpio,
                                                 HwTimestamps>::rxSlot(std::uint32_t i) const noexcept {
    return m_mem + static_cast<std::size_t>(i) * BufSize;
}

template <EtherFabricMode M, std::uint16_t NbRxBufs, std::uint16_t NbTxBufs, std::uint32_t BufSize,
          unsigned CtThreshold, bool UseCtpio, bool HwTimestamps>
inline std::uint8_t* EtherFabricVirtualInterface<M, NbRxBufs, NbTxBufs, BufSize, CtThreshold, UseCtpio,
                                                 HwTimestamps>::txSlot(std::uint32_t s) const noexcept {
    return m_mem + static_cast<std::size_t>(m_txBase + s) * BufSize;
}

template <EtherFabricMode M, std::uint16_t NbRxBufs, std::uint16_t NbTxBufs, std::uint32_t BufSize,
          unsigned CtThreshold, bool UseCtpio, bool HwTimestamps>
inline void EtherFabricVirtualInterface<M, NbRxBufs, NbTxBufs, BufSize, CtThreshold, UseCtpio,
                                        HwTimestamps>::handleDiscard(std::uint32_t id,
                                                                     unsigned      subtype) noexcept {
    ++m_rxDiscards;
    if (subtype == EF_EVENT_RX_DISCARD_CRC_BAD) {
        ++m_rxCrcBad;
    }
    if constexpr (kEfViRxPayloadPoll) {
        // Payload mode: the bad frame's marker already fired (or will) and the app
        // delivers + releases this buffer like any other — re-initing it here would
        // double-post. The nonzero discard counter at shutdown is the alarm; this
        // rig has never produced one (0 in 437M+ samples, FEC off, DAC, CRC clean).
        return;
    }
    ef_vi_receive_init(&m_vi, m_rxDma[id], static_cast<ef_request_id>(id));
    ++m_rxPendingPush;
}

// Walks the shared event queue. TX completions and discards are consumed in
// place; the walk STOPS at an RX event — consumed and returned when wantRx,
// left at the cursor head for tryReceive() when !wantRx (the TX-side drain
// must never eat a frame). One eventq_poll refill per call keeps the empty
// path (the hottest code here — the app spins on it) at a handful of insns.
template <EtherFabricMode M, std::uint16_t NbRxBufs, std::uint16_t NbTxBufs, std::uint32_t BufSize,
          unsigned CtThreshold, bool UseCtpio, bool HwTimestamps>
inline bool EtherFabricVirtualInterface<M, NbRxBufs, NbTxBufs, BufSize, CtThreshold, UseCtpio,
                                        HwTimestamps>::pollEvent(RxEv& out, bool wantRx) noexcept {
    for (int pass = 0;; ++pass) {
        while (m_evIdx < m_nEv) {
            const ef_event& ev = m_evs[m_evIdx];
            switch (EF_EVENT_TYPE(ev)) {
                case EF_EVENT_TYPE_RX: {
                    if constexpr (kEfViRxPayloadPoll) {
                        // Delivery already happened (or is about to) via the payload marker —
                        // the event is pure bookkeeping.
                        ++m_rxEvReconciled;
                        ++m_evIdx;
                        break;
                    }
                    if (!wantRx) {
                        return false;
                    }
                    out.id  = static_cast<std::uint32_t>(EF_EVENT_RX_RQ_ID(ev));
                    out.len = static_cast<std::uint32_t>(EF_EVENT_RX_BYTES(ev));
                    ++m_evIdx;
                    return true;
                }
                case EF_EVENT_TYPE_TX: {
                    ef_request_id ids[EF_VI_TRANSMIT_BATCH];
                    const int     n = ef_vi_transmit_unbundle(&m_vi, &ev, ids);
                    m_txTail += static_cast<std::uint32_t>(n);
                    if constexpr (UseCtpio) {
                        if (EF_EVENT_TX_CTPIO(ev)) {
                            m_ctpioWins += static_cast<std::uint64_t>(n);
                        } else {
                            m_ctpioFallbacks += static_cast<std::uint64_t>(n);
                            // Per-slot histogram: a measured 5-min run failed at EXACTLY 1/8 =
                            // one cursed slot of the NbTxBufs rotation (per-run: the buffer +
                            // aperture are allocated fresh each start). dma_id = slot index.
                            for (int k = 0; k < n; ++k) {
                                ++m_fallbackPerSlot[static_cast<std::uint32_t>(ids[k]) & (NbTxBufs - 1)];
                            }
                        }
                    } else {
                        m_ctpioFallbacks += static_cast<std::uint64_t>(n);
                    }
                    ++m_evIdx;
                    break;
                }
                case EF_EVENT_TYPE_TX_WITH_TIMESTAMP: {
                    if constexpr (HwTimestamps && HAS_TX) {
                        const std::uint32_t slot =
                            static_cast<std::uint32_t>(EF_EVENT_TX_WITH_TIMESTAMP_RQ_ID(ev)) & (NbTxBufs - 1);
                        const std::uint32_t minor =
                            static_cast<std::uint32_t>(EF_EVENT_TX_WITH_TIMESTAMP_NSEC(ev)) * 4u +
                            (static_cast<std::uint32_t>(EF_EVENT_TX_WITH_TIMESTAMP_NSEC_FRAC16(ev)) >> 14);
                        m_txStamp[slot] = (static_cast<std::uint64_t>(m_txTail) << 32) | minor;
                        ++m_txTail;
                        ++m_txTsEvents;
                        if constexpr (UseCtpio) {
                            if (EF_EVENT_TX_CTPIO(ev)) {
                                ++m_ctpioWins;
                            } else {
                                ++m_ctpioFallbacks;
                                ++m_fallbackPerSlot[slot];
                            }
                        } else {
                            ++m_ctpioFallbacks;
                        }
                    } else {
                        ++m_evUnexpected;
                    }
                    ++m_evIdx;
                    break;
                }
                case EF_EVENT_TYPE_RX_DISCARD: {
                    handleDiscard(static_cast<std::uint32_t>(EF_EVENT_RX_RQ_ID(ev)),
                                  static_cast<unsigned>(EF_EVENT_RX_DISCARD_TYPE(ev)));
                    ++m_evIdx;
                    break;
                }
                case EF_EVENT_TYPE_RX_MULTI: {
                    if constexpr (kEfViRxPayloadPoll) {
                        ef_request_id ids[EF_VI_RECEIVE_BATCH];
                        const int     n = ef_vi_receive_unbundle(&m_vi, &ev, ids);
                        m_rxEvReconciled += static_cast<std::uint64_t>(n > 0 ? n : 0);
                        ++m_evIdx;
                        break;
                    }
                    if (!wantRx) {
                        return false;
                    }
                    ef_request_id ids[EF_VI_RECEIVE_BATCH];
                    const int     n = ef_vi_receive_unbundle(&m_vi, &ev, ids);
                    ++m_evIdx;
                    if (n <= 0) {
                        break;
                    }
                    for (int k = 1; k < n; ++k) {
                        ++m_rxDropped;
                        ef_vi_receive_init(&m_vi, m_rxDma[static_cast<std::uint32_t>(ids[k])], ids[k]);
                        ++m_rxPendingPush;
                    }
                    std::uint16_t bytes = 0;
                    ef_vi_receive_get_bytes(&m_vi, rxSlot(static_cast<std::uint32_t>(ids[0])), &bytes);
                    out.id  = static_cast<std::uint32_t>(ids[0]);
                    out.len = static_cast<std::uint32_t>(bytes) + static_cast<std::uint32_t>(m_rxPrefix);
                    return true;
                }
                case EF_EVENT_TYPE_RX_MULTI_DISCARD: {
                    ef_request_id ids[EF_VI_RECEIVE_BATCH];
                    const int     n = ef_vi_receive_unbundle(&m_vi, &ev, ids);
                    for (int k = 0; k < n; ++k) {
                        handleDiscard(static_cast<std::uint32_t>(ids[k]), 0);
                    }
                    ++m_evIdx;
                    break;
                }
                case EF_EVENT_TYPE_TX_ERROR: {
                    ++m_txErrors;
                    ++m_txTail;
                    ++m_evIdx;
                    break;
                }
                default: {
                    ++m_evUnexpected;
                    ++m_evIdx;
                    break;
                }
            }
        }
        if (pass > 0) {
            return false;
        }
        // Fast empty path: skip the out-of-line ef_eventq_poll call entirely when
        // the EVQ head slot says no event exists (see evqHasEvent()). The refill
        // below runs only when there is real work.
        if (!evqHasEvent()) [[likely]] {
            if constexpr (HAS_RX) {
                // Idle RX push: the doorbell fires from the quiet spin, keeping its
                // MMIO write (and the NIC's delayed descriptor fetch it triggers) away
                // from release()'s position nanoseconds behind a CTPIO burst. Under
                // the in_order CTPIO writer (config of record) doorbell phase cannot
                // tear a burst at all — this placement is now about PCIe tidiness,
                // not correctness. wantRx excludes the TX-side drain.
                if (wantRx && m_rxPendingPush >= 8) {
                    ef_vi_receive_push(&m_vi);
                    m_rxPendingPush &= 7;
                }
            }
            return false;
        }
        m_nEv   = ef_eventq_poll(&m_vi, m_evs, kEvPollBatch);
        m_evIdx = 0;
        if (m_nEv == 0) [[unlikely]] {
            // evqHasEvent raced a half-written event — absent by the full test;
            // the next spin iteration picks it up.
            return false;
        }
    }
}

template <EtherFabricMode M, std::uint16_t NbRxBufs, std::uint16_t NbTxBufs, std::uint32_t BufSize,
          unsigned CtThreshold, bool UseCtpio, bool HwTimestamps>
bool EtherFabricVirtualInterface<M, NbRxBufs, NbTxBufs, BufSize, CtThreshold, UseCtpio,
                                 HwTimestamps>::readMac() noexcept {
    const std::string p = "/sys/class/net/" + m_ifname + "/address";
    FILE*             f = std::fopen(p.c_str(), "r");
    if (!f) {
        fmt::println(stderr, "[ef_vi] {}: cannot read MAC", m_ifname);
        return false;
    }
    unsigned  b[6]{};
    const int n = std::fscanf(f, "%x:%x:%x:%x:%x:%x", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]);
    std::fclose(f);
    if (n != 6) {
        return false;
    }
    for (int i = 0; i < 6; ++i) {
        m_mac[i] = static_cast<std::uint8_t>(b[i]);
    }
    return true;
}

#endif   // ABTRDA3_ETHERFABRICVIRTUALINTERFACE_HPP
