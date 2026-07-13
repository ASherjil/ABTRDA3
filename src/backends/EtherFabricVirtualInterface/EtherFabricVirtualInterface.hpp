#ifndef ABTRDA3_ETHERFABRICVIRTUALINTERFACE_HPP
#define ABTRDA3_ETHERFABRICVIRTUALINTERFACE_HPP

#include <etherfabric/ef_vi.h>
#include <etherfabric/vi.h>
#include <etherfabric/pd.h>
#include <etherfabric/memreg.h>
#include <etherfabric/capabilities.h>

#include <net/if.h>
#include <sys/mman.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <span>
#include <string>
#include <string_view>

#include <fmt/core.h>

#include "../common/RxFrame.hpp"

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
//     with rdtscp so instrument != transport (RxFrame.sec/nsec stay 0).
//   * -EPERM from ef_vi_alloc_from_pd = firmware forcing event merging; retried
//     with EF_VI_RX_EVENT_MERGE (logged loudly — merge is a throughput feature
//     and RX then arrives as EF_EVENT_TYPE_RX_MULTI).
//   * A discard consumes the RX descriptor — every discard path re-posts.
//   * shutdown() order: vi (removes filters) -> memreg -> pd -> driver handle.
// =============================================================================

enum class EtherFabricMode : std::uint8_t { RxOnly, TxOnly, RxTx };

template<EtherFabricMode M, std::uint16_t NbRxBufs = 256, std::uint16_t NbTxBufs = 8, std::uint32_t BufSize = 2048, unsigned CtThreshold = 64, bool UseCtpio = true>
class EtherFabricVirtualInterface {
  static constexpr bool HAS_RX = (M == EtherFabricMode::RxOnly || M == EtherFabricMode::RxTx);
  static constexpr bool HAS_TX = (M == EtherFabricMode::TxOnly || M == EtherFabricMode::RxTx);

  static_assert((NbTxBufs & (NbTxBufs - 1)) == 0, "NbTxBufs must be a power of two");
  static_assert(NbRxBufs % 8 == 0, "EF10 pushes RX descriptors in multiples of 8");
  static_assert(BufSize >= 64, "slot must hold a minimum ethernet frame");

  static constexpr int kEvPollBatch = 8;
  static_assert(kEvPollBatch >= EF_VI_EVENT_POLL_MIN_EVS);

public:
  explicit EtherFabricVirtualInterface(std::string_view ifname) noexcept;
  ~EtherFabricVirtualInterface();

  EtherFabricVirtualInterface(const EtherFabricVirtualInterface&)            = delete;
  EtherFabricVirtualInterface& operator=(const EtherFabricVirtualInterface&) = delete;
  EtherFabricVirtualInterface(EtherFabricVirtualInterface&&)                 = delete;
  EtherFabricVirtualInterface& operator=(EtherFabricVirtualInterface&&)      = delete;

  [[nodiscard]] bool init() noexcept;
  void shutdown() noexcept;

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
  inline RxFrame tryReceive() noexcept
      requires (M == EtherFabricMode::RxOnly || M == EtherFabricMode::RxTx);

  [[gnu::always_inline, gnu::hot]]
  inline void release() noexcept
      requires (M == EtherFabricMode::RxOnly || M == EtherFabricMode::RxTx);

private:
  struct RxEv {
    std::uint32_t id;
    std::uint32_t len;
  };

  [[gnu::always_inline]] std::uint8_t* rxSlot(std::uint32_t i) const noexcept;
  [[gnu::always_inline]] std::uint8_t* txSlot(std::uint32_t s) const noexcept;
  [[gnu::hot]] inline bool pollEvent(RxEv& out, bool wantRx) noexcept;
  inline void handleDiscard(std::uint32_t id, unsigned subtype) noexcept;
  [[nodiscard]] bool readMac() noexcept;

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

  ef_event m_evs[kEvPollBatch];
  int      m_nEv{0};
  int      m_evIdx{0};

  std::uint32_t m_heldId{0};
  std::uint32_t m_txHead{0};
  std::uint32_t m_txTail{0};
  std::uint32_t m_txSlot{0};
  std::uint32_t m_txLen{0};

  std::uint64_t m_ctpioWins{0};
  std::uint64_t m_ctpioFallbacks{0};
  std::uint64_t m_rxDiscards{0};
  std::uint64_t m_rxCrcBad{0};
  std::uint64_t m_rxDropped{0};
  std::uint64_t m_txErrors{0};
  std::uint64_t m_evUnexpected{0};
};

// =============================================================================

template<EtherFabricMode M, std::uint16_t NbRxBufs, std::uint16_t NbTxBufs, std::uint32_t BufSize, unsigned CtThreshold, bool UseCtpio>
EtherFabricVirtualInterface<M, NbRxBufs, NbTxBufs, BufSize, CtThreshold, UseCtpio>::EtherFabricVirtualInterface(std::string_view ifname) noexcept
  : m_ifname{ifname} {}

template<EtherFabricMode M, std::uint16_t NbRxBufs, std::uint16_t NbTxBufs, std::uint32_t BufSize, unsigned CtThreshold, bool UseCtpio>
EtherFabricVirtualInterface<M, NbRxBufs, NbTxBufs, BufSize, CtThreshold, UseCtpio>::~EtherFabricVirtualInterface() {
  shutdown();
}

template<EtherFabricMode M, std::uint16_t NbRxBufs, std::uint16_t NbTxBufs, std::uint32_t BufSize, unsigned CtThreshold, bool UseCtpio>
bool EtherFabricVirtualInterface<M, NbRxBufs, NbTxBufs, BufSize, CtThreshold, UseCtpio>::init() noexcept {
  if (!readMac()) return false;

  const unsigned ifindex = if_nametoindex(m_ifname.c_str());
  if (ifindex == 0) {
    fmt::println(stderr, "[ef_vi] {}: no such interface (port must stay on the kernel sfc driver)", m_ifname);
    return false;
  }

  if (ef_driver_open(&m_dh) != 0) {
    fmt::println(stderr, "[ef_vi] {}: ef_driver_open failed (/dev/sfc_char — onload/sfc_char modules loaded? root?)", m_ifname);
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
      fmt::println(stderr, "[ef_vi] {}: NIC does not support CTPIO (set UseCtpio=false for the DMA path)", m_ifname);
      return false;
    }
    viFlags = static_cast<enum ef_vi_flags>(viFlags | EF_VI_TX_CTPIO);
  }

  int rc = ef_vi_alloc_from_pd(&m_vi, m_dh, &m_pd, m_dh, -1, -1, -1, nullptr, -1, viFlags);
  if (rc == -EPERM) {
    fmt::println(stderr, "[ef_vi] {}: firmware forces RX event merging — retrying with EF_VI_RX_EVENT_MERGE (throughput mode, NOT the latency config)", m_ifname);
    viFlags = static_cast<enum ef_vi_flags>(viFlags | EF_VI_RX_EVENT_MERGE);
    rc = ef_vi_alloc_from_pd(&m_vi, m_dh, &m_pd, m_dh, -1, -1, -1, nullptr, -1, viFlags);
    m_evMerge = true;
  }
  if (rc != 0) {
    fmt::println(stderr, "[ef_vi] {}: ef_vi_alloc_from_pd failed ({})", m_ifname, rc);
    return false;
  }
  m_haveVi = true;

  if (m_vi.nic_type.arch == EF_VI_ARCH_EFCT) {
    fmt::println(stderr, "[ef_vi] {}: EFCT/X3 adapter — the rx_ref datapath is not implemented here", m_ifname);
    return false;
  }

  unsigned long minPage = 4096;
  if (ef_vi_capabilities_get(m_dh, static_cast<int>(ifindex), EF_VI_CAP_MIN_BUFFER_MODE_SIZE, &minPage) != 0 || minPage < 4096) {
    minPage = 4096;
  }
  const std::size_t slotBytes = static_cast<std::size_t>(NbRxBufs + NbTxBufs) * BufSize;
  m_memBytes = (std::max(slotBytes, static_cast<std::size_t>(minPage)) + 4095) & ~static_cast<std::size_t>(4095);
  if (minPage >= 2 * 1024 * 1024) {
    m_mem = static_cast<std::uint8_t*>(mmap(nullptr, m_memBytes, PROT_READ | PROT_WRITE,
                                            MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB, -1, 0));
    if (m_mem == MAP_FAILED) {
      m_mem = nullptr;
      fmt::println(stderr, "[ef_vi] {}: hugepage alloc of {} bytes failed", m_ifname, m_memBytes);
      return false;
    }
    m_memHuge = true;
  } else {
    if (posix_memalign(reinterpret_cast<void**>(&m_mem), minPage, m_memBytes) != 0) {
      fmt::println(stderr, "[ef_vi] {}: posix_memalign({}) failed", m_ifname, m_memBytes);
      return false;
    }
  }
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
    m_txDma[s] = ef_memreg_dma_addr(&m_memreg, static_cast<std::size_t>(NbRxBufs + s) * BufSize);
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
      if (ef_vi_receive_post(&m_vi, m_rxDma[i], static_cast<ef_request_id>(i)) != 0) break;
    }
    m_rxPrefix = ef_vi_receive_prefix_len(&m_vi);
  }

  fmt::println(stderr, "[ef_vi] {} ready — {} arch={} rxq {} txq {} tx={} ct_thresh={} prefix {}B{}",
               m_ifname, ef_vi_version_str(),
               m_vi.nic_type.arch == EF_VI_ARCH_EF10 ? "EF10" : "other",
               NbRxBufs, NbTxBufs,
               UseCtpio ? "CTPIO" : "DMA",
               UseCtpio ? CtThreshold : 0U,
               m_rxPrefix,
               m_evMerge ? " [EVENT-MERGE FORCED]" : "");
  return true;
}

template<EtherFabricMode M, std::uint16_t NbRxBufs, std::uint16_t NbTxBufs, std::uint32_t BufSize, unsigned CtThreshold, bool UseCtpio>
void EtherFabricVirtualInterface<M, NbRxBufs, NbTxBufs, BufSize, CtThreshold, UseCtpio>::shutdown() noexcept {
  if (m_haveVi) {
    if constexpr (HAS_TX) {
      if constexpr (UseCtpio) {
        fmt::println(stderr, "[ef_vi] {}: tx ctpio_wins={} ctpio_fallbacks={} tx_errors={}",
                     m_ifname, m_ctpioWins, m_ctpioFallbacks, m_txErrors);
      } else {
        fmt::println(stderr, "[ef_vi] {}: tx dma_sends={} tx_errors={}",
                     m_ifname, m_ctpioFallbacks, m_txErrors);
      }
    }
    if constexpr (HAS_RX) {
      fmt::println(stderr, "[ef_vi] {}: rx discards={} (crc_bad={}) dropped={} unexpected_events={}",
                   m_ifname, m_rxDiscards, m_rxCrcBad, m_rxDropped, m_evUnexpected);
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

template<EtherFabricMode M, std::uint16_t NbRxBufs, std::uint16_t NbTxBufs, std::uint32_t BufSize, unsigned CtThreshold, bool UseCtpio>
std::array<std::uint8_t, 6> EtherFabricVirtualInterface<M, NbRxBufs, NbTxBufs, BufSize, CtThreshold, UseCtpio>::macAddress() const noexcept {
  return m_mac;
}

template<EtherFabricMode M, std::uint16_t NbRxBufs, std::uint16_t NbTxBufs, std::uint32_t BufSize, unsigned CtThreshold, bool UseCtpio>
void EtherFabricVirtualInterface<M, NbRxBufs, NbTxBufs, BufSize, CtThreshold, UseCtpio>::prefillRing(std::span<const std::uint8_t> frameTemplate) noexcept requires (M == EtherFabricMode::TxOnly || M == EtherFabricMode::RxTx) {
  for (std::uint32_t s = 0; s < NbTxBufs; ++s) {
    std::memcpy(txSlot(s), frameTemplate.data(),
                std::min<std::size_t>(frameTemplate.size(), BufSize));
  }
}

template<EtherFabricMode M, std::uint16_t NbRxBufs, std::uint16_t NbTxBufs, std::uint32_t BufSize, unsigned CtThreshold, bool UseCtpio>
inline std::uint8_t* EtherFabricVirtualInterface<M, NbRxBufs, NbTxBufs, BufSize, CtThreshold, UseCtpio>::acquire(std::uint32_t frameLen) noexcept requires (M == EtherFabricMode::TxOnly || M == EtherFabricMode::RxTx) {
  if (frameLen > BufSize) [[unlikely]] return nullptr;
  if (m_txHead - m_txTail >= NbTxBufs) [[unlikely]] {
    RxEv scratch;
    pollEvent(scratch, false);
    if (m_txHead - m_txTail >= NbTxBufs) return nullptr;
  }
  m_txSlot = m_txHead & (NbTxBufs - 1);
  m_txLen  = frameLen;
  return txSlot(m_txSlot);
}

template<EtherFabricMode M, std::uint16_t NbRxBufs, std::uint16_t NbTxBufs, std::uint32_t BufSize, unsigned CtThreshold, bool UseCtpio>
inline void EtherFabricVirtualInterface<M, NbRxBufs, NbTxBufs, BufSize, CtThreshold, UseCtpio>::commit() noexcept requires (M == EtherFabricMode::TxOnly || M == EtherFabricMode::RxTx) {
  if constexpr (UseCtpio) {
    ef_vi_transmit_ctpio(&m_vi, txSlot(m_txSlot), m_txLen, CtThreshold);
    for (;;) {
      const int rc = ef_vi_transmit_ctpio_fallback(&m_vi, m_txDma[m_txSlot],
                                                   static_cast<int>(m_txLen),
                                                   static_cast<ef_request_id>(m_txSlot));
      if (rc == 0) [[likely]] break;
      if (rc != -EAGAIN) [[unlikely]] {
        ++m_txErrors;
        return;
      }
      RxEv scratch;
      pollEvent(scratch, false);
    }
  } else {
    for (;;) {
      const int rc = ef_vi_transmit(&m_vi, m_txDma[m_txSlot],
                                    static_cast<int>(m_txLen),
                                    static_cast<ef_request_id>(m_txSlot));
      if (rc == 0) [[likely]] break;
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

template<EtherFabricMode M, std::uint16_t NbRxBufs, std::uint16_t NbTxBufs, std::uint32_t BufSize, unsigned CtThreshold, bool UseCtpio>
inline bool EtherFabricVirtualInterface<M, NbRxBufs, NbTxBufs, BufSize, CtThreshold, UseCtpio>::send(std::span<const std::uint8_t> frame) noexcept requires (M == EtherFabricMode::TxOnly || M == EtherFabricMode::RxTx) {
  auto* dst = acquire(static_cast<std::uint32_t>(frame.size()));
  if (dst == nullptr) [[unlikely]] return false;
  std::memcpy(dst, frame.data(), frame.size());
  commit();
  return true;
}

template<EtherFabricMode M, std::uint16_t NbRxBufs, std::uint16_t NbTxBufs, std::uint32_t BufSize, unsigned CtThreshold, bool UseCtpio>
inline RxFrame EtherFabricVirtualInterface<M, NbRxBufs, NbTxBufs, BufSize, CtThreshold, UseCtpio>::tryReceive() noexcept requires (M == EtherFabricMode::RxOnly || M == EtherFabricMode::RxTx) {
  RxEv ev;
  if (!pollEvent(ev, true)) [[likely]] return {};
  m_heldId = ev.id;
  return { .data   = { rxSlot(ev.id) + m_rxPrefix, ev.len - static_cast<std::uint32_t>(m_rxPrefix) },
           .sec    = 0,
           .nsec   = 0,
           .status = 1 };
}

template<EtherFabricMode M, std::uint16_t NbRxBufs, std::uint16_t NbTxBufs, std::uint32_t BufSize, unsigned CtThreshold, bool UseCtpio>
inline void EtherFabricVirtualInterface<M, NbRxBufs, NbTxBufs, BufSize, CtThreshold, UseCtpio>::release() noexcept requires (M == EtherFabricMode::RxOnly || M == EtherFabricMode::RxTx) {
  ef_vi_receive_post(&m_vi, m_rxDma[m_heldId], static_cast<ef_request_id>(m_heldId));
}

template<EtherFabricMode M, std::uint16_t NbRxBufs, std::uint16_t NbTxBufs, std::uint32_t BufSize, unsigned CtThreshold, bool UseCtpio>
inline std::uint8_t* EtherFabricVirtualInterface<M, NbRxBufs, NbTxBufs, BufSize, CtThreshold, UseCtpio>::rxSlot(std::uint32_t i) const noexcept {
  return m_mem + static_cast<std::size_t>(i) * BufSize;
}

template<EtherFabricMode M, std::uint16_t NbRxBufs, std::uint16_t NbTxBufs, std::uint32_t BufSize, unsigned CtThreshold, bool UseCtpio>
inline std::uint8_t* EtherFabricVirtualInterface<M, NbRxBufs, NbTxBufs, BufSize, CtThreshold, UseCtpio>::txSlot(std::uint32_t s) const noexcept {
  return m_mem + static_cast<std::size_t>(NbRxBufs + s) * BufSize;
}

template<EtherFabricMode M, std::uint16_t NbRxBufs, std::uint16_t NbTxBufs, std::uint32_t BufSize, unsigned CtThreshold, bool UseCtpio>
inline void EtherFabricVirtualInterface<M, NbRxBufs, NbTxBufs, BufSize, CtThreshold, UseCtpio>::handleDiscard(std::uint32_t id, unsigned subtype) noexcept {
  ++m_rxDiscards;
  if (subtype == EF_EVENT_RX_DISCARD_CRC_BAD) ++m_rxCrcBad;
  ef_vi_receive_post(&m_vi, m_rxDma[id], static_cast<ef_request_id>(id));
}

// Walks the shared event queue. TX completions and discards are consumed in
// place; the walk STOPS at an RX event — consumed and returned when wantRx,
// left at the cursor head for tryReceive() when !wantRx (the TX-side drain
// must never eat a frame). One eventq_poll refill per call keeps the empty
// path (the hottest code here — the app spins on it) at a handful of insns.
template<EtherFabricMode M, std::uint16_t NbRxBufs, std::uint16_t NbTxBufs, std::uint32_t BufSize, unsigned CtThreshold, bool UseCtpio>
inline bool EtherFabricVirtualInterface<M, NbRxBufs, NbTxBufs, BufSize, CtThreshold, UseCtpio>::pollEvent(RxEv& out, bool wantRx) noexcept {
  for (int pass = 0; ; ++pass) {
    while (m_evIdx < m_nEv) {
      ef_event& ev = m_evs[m_evIdx];
      switch (EF_EVENT_TYPE(ev)) {
      case EF_EVENT_TYPE_RX: {
        if (!wantRx) return false;
        out.id  = static_cast<std::uint32_t>(EF_EVENT_RX_RQ_ID(ev));
        out.len = static_cast<std::uint32_t>(EF_EVENT_RX_BYTES(ev));
        ++m_evIdx;
        return true;
      }
      case EF_EVENT_TYPE_TX: {
        ef_request_id ids[EF_VI_TRANSMIT_BATCH];
        const int n = ef_vi_transmit_unbundle(&m_vi, &ev, ids);
        m_txTail += static_cast<std::uint32_t>(n);
        if constexpr (UseCtpio) {
          if (EF_EVENT_TX_CTPIO(ev)) {
            m_ctpioWins += static_cast<std::uint64_t>(n);
          } else {
            m_ctpioFallbacks += static_cast<std::uint64_t>(n);
          }
        } else {
          m_ctpioFallbacks += static_cast<std::uint64_t>(n);
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
        if (!wantRx) return false;
        ef_request_id ids[EF_VI_RECEIVE_BATCH];
        const int n = ef_vi_receive_unbundle(&m_vi, &ev, ids);
        ++m_evIdx;
        if (n <= 0) break;
        for (int k = 1; k < n; ++k) {
          ++m_rxDropped;
          ef_vi_receive_post(&m_vi, m_rxDma[static_cast<std::uint32_t>(ids[k])], ids[k]);
        }
        std::uint16_t bytes = 0;
        ef_vi_receive_get_bytes(&m_vi, rxSlot(static_cast<std::uint32_t>(ids[0])), &bytes);
        out.id  = static_cast<std::uint32_t>(ids[0]);
        out.len = static_cast<std::uint32_t>(bytes) + static_cast<std::uint32_t>(m_rxPrefix);
        return true;
      }
      case EF_EVENT_TYPE_RX_MULTI_DISCARD: {
        ef_request_id ids[EF_VI_RECEIVE_BATCH];
        const int n = ef_vi_receive_unbundle(&m_vi, &ev, ids);
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
    if (pass > 0) return false;
    m_nEv   = ef_eventq_poll(&m_vi, m_evs, kEvPollBatch);
    m_evIdx = 0;
    if (m_nEv == 0) [[likely]] return false;
  }
}

template<EtherFabricMode M, std::uint16_t NbRxBufs, std::uint16_t NbTxBufs, std::uint32_t BufSize, unsigned CtThreshold, bool UseCtpio>
bool EtherFabricVirtualInterface<M, NbRxBufs, NbTxBufs, BufSize, CtThreshold, UseCtpio>::readMac() noexcept {
  const std::string p = "/sys/class/net/" + m_ifname + "/address";
  FILE* f = std::fopen(p.c_str(), "r");
  if (!f) {
    fmt::println(stderr, "[ef_vi] {}: cannot read MAC", m_ifname);
    return false;
  }
  unsigned b[6]{};
  const int n = std::fscanf(f, "%x:%x:%x:%x:%x:%x", &b[0],&b[1],&b[2],&b[3],&b[4],&b[5]);
  std::fclose(f);
  if (n != 6) return false;
  for (int i = 0; i < 6; ++i) m_mac[i] = static_cast<std::uint8_t>(b[i]);
  return true;
}

#endif // ABTRDA3_ETHERFABRICVIRTUALINTERFACE_HPP
