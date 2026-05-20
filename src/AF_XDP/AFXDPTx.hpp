//
// Created by asherjil on 3/3/26.
// Refactored 5/2026 — libxdp ring operations replace raw producer/consumer pointers.
//

#ifndef ABTRDA3_AFXDPTX_H
#define ABTRDA3_AFXDPTX_H

#include "AFXDPSocket.hpp"

#include <cstdint>
#include <cstring>
#include <fmt/core.h>
#include <memory>
#include <span>
#include <sys/socket.h>

class AFXDPTx {
public:
  explicit AFXDPTx(AFXDPSocket& sock);
  AFXDPTx(const AFXDPTx&) = delete;
  AFXDPTx& operator=(const AFXDPTx&) = delete;
  AFXDPTx(AFXDPTx&& other) noexcept;
  AFXDPTx& operator=(AFXDPTx&& other) noexcept;

  // ── Copy-based send (convenience API) ────────────────────────────────
  [[nodiscard, gnu::always_inline]]
  inline bool send(std::span<const std::uint8_t> frame) noexcept {
    auto* dst = acquire(static_cast<std::uint32_t>(frame.size()));
    if (!dst) [[unlikely]] return false;
    std::memcpy(dst, frame.data(), frame.size());
    commit();
    return true;
  }

  [[nodiscard, gnu::always_inline]]
  inline std::uint8_t* acquire(std::uint32_t frameLen) noexcept {
    if (m_freeTop == 0) [[unlikely]] {
      reclaimCompleted();
      if (m_freeTop == 0) [[unlikely]]
        return nullptr;
    }
    m_pendingAddr = m_freeStack[--m_freeTop];
    m_pendingLen  = frameLen;
    return m_umem + m_pendingAddr;
  }

  [[gnu::always_inline]]
  inline void commit() noexcept {
    __u32 idx;
    if (xsk_ring_prod__reserve(m_txRing, 1, &idx) == 0) {
      reclaimCompleted();
      if (xsk_ring_prod__reserve(m_txRing, 1, &idx) == 0) {
        // TX ring full — kick TX to make room
        if (xsk_ring_prod__needs_wakeup(m_txRing) || !m_needWakeup)
          ::sendto(m_fd, nullptr, 0, MSG_DONTWAIT, nullptr, 0);
        reclaimCompleted();
        if (xsk_ring_prod__reserve(m_txRing, 1, &idx) == 0) {
          m_freeStack[m_freeTop++] = m_pendingAddr; // return, don't leak
          return;
        }
      }
    }

    xsk_ring_prod__tx_desc(m_txRing, idx)->addr    = m_pendingAddr;
    xsk_ring_prod__tx_desc(m_txRing, idx)->len     = m_pendingLen;
    xsk_ring_prod__tx_desc(m_txRing, idx)->options = 0;

    xsk_ring_prod__submit(m_txRing, 1);

    // NEED_WAKEUP: TX won't happen without sendto(), and RX
    // won't see new packets without recvfrom().  Both trigger
    // the same NAPI poll but the kernel tracks TX/RX wakeup
    // separately, so we must poke both.
    if (xsk_ring_prod__needs_wakeup(m_txRing) || !m_needWakeup) {
      ::sendto(m_fd, nullptr, 0, MSG_DONTWAIT, nullptr, 0);
      ::recvfrom(m_fd, nullptr, 0, MSG_DONTWAIT, nullptr, nullptr);
    }
  }

  int  fd() const noexcept { return m_fd; }
  void prefillRing(std::span<const std::uint8_t> frameTemplate) const noexcept;

private:
  [[gnu::always_inline]]
  inline void reclaimCompleted() noexcept {
    __u32 idx, rcvd;
    rcvd = xsk_ring_cons__peek(m_compRing, m_txFrameCount, &idx);
    for (__u32 i = 0; i < rcvd; ++i)
      m_freeStack[m_freeTop++] = *xsk_ring_cons__comp_addr(m_compRing, idx++);
    if (rcvd > 0)
      xsk_ring_cons__release(m_compRing, rcvd);
  }

  // ── Hot members ──────────────────────────────────────────────────────
  std::uint8_t*   m_umem{};
  xsk_ring_prod*  m_txRing{};
  xsk_ring_cons*  m_compRing{};
  int             m_fd{-1};
  std::uint64_t*  m_freeStack{};
  std::uint32_t   m_freeTop{};
  std::uint64_t   m_pendingAddr{};
  std::uint32_t   m_pendingLen{};
  bool            m_needWakeup{};
  std::uint32_t   m_frameSize{};
  std::uint32_t   m_txFrameCount{};

  std::unique_ptr<std::uint64_t[]> m_freeStackStorage;
};

#endif // ABTRDA3_AFXDPTX_H
