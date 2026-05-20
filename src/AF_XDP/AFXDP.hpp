//
// AFXDP — xdpsock-derived hot-path TX/RX on AFXDPSocket.
// Mode: RxOnly | TxOnly | RxTx
//

#ifndef ABTRDA3_AFXDP_HPP
#define ABTRDA3_AFXDP_HPP

#include "AFXDPSocket.hpp"
#include "../common/RxFrame.hpp"

#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <sys/socket.h>

enum class AFXDPMode : std::uint8_t { RxOnly, TxOnly, RxTx };

static constexpr std::uint32_t BATCH_SIZE = 64;

template<AFXDPMode M>
class AFXDP {
  static constexpr bool HAS_RX = (M != AFXDPMode::TxOnly);
  static constexpr bool HAS_TX = (M != AFXDPMode::RxOnly);

public:
  explicit AFXDP(AFXDPSocket& sock);
  ~AFXDP() = default;

  AFXDP(const AFXDP&) = delete;
  AFXDP& operator=(const AFXDP&) = delete;
  AFXDP(AFXDP&&) noexcept = default;
  AFXDP& operator=(AFXDP&&) noexcept = default;

  // ── RX ────────────────────────────────────────────────────────────
  // Peek EXACTLY one descriptor. xsk_ring_cons__peek advances cached_cons by
  // the number returned, and release() only frees one — so peeking more than
  // we release would desync cached_cons and silently drop frames 2..N.
  [[nodiscard, gnu::always_inline]]
  inline RxFrame tryReceive() noexcept requires (HAS_RX) {
    __u32 idx;
    unsigned int rcvd = xsk_ring_cons__peek(m_rxRing, 1, &idx);
    if (rcvd == 0) [[likely]] {
      ::recvfrom(m_fd, nullptr, 0, MSG_DONTWAIT, nullptr, nullptr);
      return {};
    }
    const struct xdp_desc* desc = xsk_ring_cons__rx_desc(m_rxRing, idx);
    m_pendingRxAddr = desc->addr;
    return { .data = {m_umem + desc->addr, desc->len}, .sec = 0, .nsec = 0, .status = 1 };
  }

  [[gnu::always_inline]]
  inline void release() noexcept requires (HAS_RX) {
    xsk_ring_cons__release(m_rxRing, 1);

    // xdpsock rx_drop: put frame back to fill ring
    __u32 idx_fq;
    if (xsk_ring_prod__reserve(m_fillRing, 1, &idx_fq) != 1) {
      ::recvfrom(m_fd, nullptr, 0, MSG_DONTWAIT, nullptr, nullptr);
      if (xsk_ring_prod__reserve(m_fillRing, 1, &idx_fq) != 1)
        return;  // truly full, drop
    }
    *xsk_ring_prod__fill_addr(m_fillRing, idx_fq) = m_pendingRxAddr;
    xsk_ring_prod__submit(m_fillRing, 1);
  }

  // ── TX ────────────────────────────────────────────────────────────
  // TX frames live in their own pool [0 .. m_txPoolFrames) and never overlap
  // the RX/fill pool. m_txFrameNb cycles within that pool; completed frames
  // come back implicitly as the counter wraps (xdpsock txonly model).
  [[nodiscard, gnu::always_inline]]
  inline std::uint8_t* acquire(std::uint32_t frameLen) noexcept requires (HAS_TX) {
    completeTx();
    m_pendingTxAddr = static_cast<std::uint64_t>(m_txFrameNb) * m_frameSize;
    m_pendingTxLen  = frameLen;
    return m_umem + m_pendingTxAddr;
  }

  [[gnu::always_inline]]
  inline void commit() noexcept requires (HAS_TX) {
    __u32 idx;
    // TX ring full — drain completions (which kicks TX) until a slot frees.
    while (xsk_ring_prod__reserve(m_txRing, 1, &idx) != 1)
      completeTx();

    xsk_ring_prod__tx_desc(m_txRing, idx)->addr    = m_pendingTxAddr;
    xsk_ring_prod__tx_desc(m_txRing, idx)->len     = m_pendingTxLen;
    xsk_ring_prod__tx_desc(m_txRing, idx)->options = 0;
    xsk_ring_prod__submit(m_txRing, 1);
    m_outstandingTx++;
    m_txFrameNb = (m_txFrameNb + 1) % m_txPoolFrames;

    completeTx();  // xdpsock tx_only: kick TX + reap completions after submit
  }

  [[nodiscard, gnu::always_inline]]
  inline bool send(std::span<const std::uint8_t> frame) noexcept requires (HAS_TX) {
    auto* dst = acquire(static_cast<std::uint32_t>(frame.size()));
    if (!dst) [[unlikely]] return false;
    std::memcpy(dst, frame.data(), frame.size());
    commit();
    return true;
  }

  // Prefill ONLY the TX pool. RX-pool frames are already owned by the kernel
  // (submitted to the fill ring in the ctor), so touching them here would race
  // with inbound DMA.
  void prefillRing(std::span<const std::uint8_t> frameTemplate) noexcept requires (HAS_TX) {
    for (std::uint32_t i = 0; i < m_txPoolFrames; i++) {
      std::uint64_t addr = static_cast<std::uint64_t>(i) * m_frameSize;
      std::memcpy(m_umem + addr, frameTemplate.data(), frameTemplate.size());
    }
  }

  int fd() const noexcept { return m_fd; }

private:
  // xdpsock: complete_tx_only — kick TX, then reap the completion ring.
  // Completed TX frames return to the TX pool implicitly (m_txFrameNb wraps);
  // they must NOT be donated to the fill ring (that is the in-place l2fwd
  // model, which would merge the disjoint TX and RX pools).
  inline void completeTx() noexcept requires (HAS_TX) {
    if (!m_outstandingTx) return;

    if (!m_needWakeup || xsk_ring_prod__needs_wakeup(m_txRing))
      ::sendto(m_fd, nullptr, 0, MSG_DONTWAIT, nullptr, 0);

    __u32 idx;
    unsigned int rcvd = xsk_ring_cons__peek(m_compRing, BATCH_SIZE, &idx);
    if (rcvd > 0) {
      xsk_ring_cons__release(m_compRing, rcvd);
      m_outstandingTx -= rcvd;
    }
  }

  // ── Members ──────────────────────────────────────────────────────
  std::uint8_t*  m_umem;
  xsk_ring_cons* m_rxRing;
  xsk_ring_prod* m_txRing;
  xsk_ring_prod* m_fillRing;
  xsk_ring_cons* m_compRing;
  int            m_fd;
  bool           m_needWakeup;
  std::uint32_t  m_frameSize;
  std::uint32_t  m_frameCount;

  std::uint64_t  m_pendingRxAddr{};
  std::uint64_t  m_pendingTxAddr{};
  std::uint32_t  m_pendingTxLen{};
  std::uint32_t  m_txFrameNb{0};
  std::uint32_t  m_outstandingTx{0};

  // UMEM frame partition: TX uses [0 .. m_txPoolFrames), RX/fill uses the rest.
  std::uint32_t  m_txPoolFrames{0};
};

// ── Constructor (xdpsock: xsk_populate_fill_ring) ──────────────────────────

template<AFXDPMode M>
AFXDP<M>::AFXDP(AFXDPSocket& sock)
  : m_umem{sock.umemArea()},
    m_rxRing{&sock.rxRing()},
    m_txRing{&sock.txRing()},
    m_fillRing{&sock.fillRing()},
    m_compRing{&sock.compRing()},
    m_fd{sock.fd()},
    m_needWakeup{sock.needWakeup()},
    m_frameSize{sock.frameSize()},
    m_frameCount{sock.numFrames()}
{
  // Partition the UMEM into two DISJOINT pools so TX-generated frames never
  // alias the frames the kernel owns for RX. In RxTx mode split 50/50; a
  // single-direction socket gets the whole UMEM.
  if constexpr (HAS_TX)
    m_txPoolFrames = HAS_RX ? (m_frameCount / 2) : m_frameCount;
  else
    m_txPoolFrames = 0;

  if constexpr (HAS_RX) {
    // Fill ring gets ONLY the RX-pool frames [rxBase .. m_frameCount).
    const std::uint32_t rxBase = m_txPoolFrames;             // 0 when RxOnly
    const std::uint32_t rxPool = m_frameCount - m_txPoolFrames;
    __u32 idx;
    unsigned int ret = xsk_ring_prod__reserve(m_fillRing, rxPool, &idx);
    for (std::uint32_t i = 0; i < ret; i++)
      *xsk_ring_prod__fill_addr(m_fillRing, idx++) =
        static_cast<std::uint64_t>(rxBase + i) * m_frameSize;
    xsk_ring_prod__submit(m_fillRing, ret);

    // ARM the hardware NOW — without this, first packets are dropped.
    ::recvfrom(m_fd, nullptr, 0, MSG_DONTWAIT, nullptr, nullptr);
  }

  m_txFrameNb = 0;  // TX pool base is frame 0
}

#endif // ABTRDA3_AFXDP_HPP
