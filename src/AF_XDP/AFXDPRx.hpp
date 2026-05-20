//
// Created by asherjil on 3/3/26.
// Refactored 5/2026 — libxdp ring operations.
//

#ifndef ABTRDA3_AFXDPRX_H
#define ABTRDA3_AFXDPRX_H

#include "AFXDPSocket.hpp"
#include "RxFrame.hpp"

#include <fmt/core.h>
#include <sys/socket.h>

class AFXDPRx {
public:
  explicit AFXDPRx(AFXDPSocket& sock);
  AFXDPRx(const AFXDPRx&) = delete;
  AFXDPRx& operator=(const AFXDPRx&) = delete;
  AFXDPRx(AFXDPRx&& other) noexcept;
  AFXDPRx& operator=(AFXDPRx&& other) noexcept;
  ~AFXDPRx() = default;

  // ── Hot path: non-blocking receive ───────────────────────────────────
  [[nodiscard, gnu::always_inline]]
  inline RxFrame tryReceive() noexcept {
    __u32 idx, rcvd;
    rcvd = xsk_ring_cons__peek(m_rxRing, 1, &idx);
    if (rcvd == 0) [[likely]] {
      int r = ::recvfrom(m_fd, nullptr, 0, MSG_DONTWAIT, nullptr, nullptr);
      static int dbg = 0;
      if (dbg < 5 && r < 0) { fmt::print(stderr, "[Rx] recvfrom errno={}\n", errno); ++dbg; }
      return {};
    }

    const struct xdp_desc* desc = xsk_ring_cons__rx_desc(m_rxRing, idx);
    m_pendingAddr = desc->addr;

    return {
      .data   = {m_umem + desc->addr, desc->len},
      .sec    = 0,
      .nsec   = 0,
      .status = 1   // non-zero = frame present
    };
  }

  [[gnu::always_inline]]
  inline void release() noexcept {
    // Release processed RX descriptor
    xsk_ring_cons__release(m_rxRing, 1);

    // Recycle buffer back to fill ring
    __u32 idx;
    if (xsk_ring_prod__reserve(m_fillRing, 1, &idx) == 0) {
      if (xsk_ring_prod__needs_wakeup(m_fillRing) || !m_needWakeup)
        ::recvfrom(m_fd, nullptr, 0, MSG_DONTWAIT, nullptr, nullptr);
      return;
    }

    *xsk_ring_prod__fill_addr(m_fillRing, idx) = m_pendingAddr;
    xsk_ring_prod__submit(m_fillRing, 1);

    // Wake kernel to consume fill ring if it's running low
    if (xsk_ring_prod__needs_wakeup(m_fillRing) || !m_needWakeup)
      ::recvfrom(m_fd, nullptr, 0, MSG_DONTWAIT, nullptr, nullptr);
  }

private:
  std::uint8_t*   m_umem{};
  xsk_ring_cons*  m_rxRing{};
  xsk_ring_prod*  m_fillRing{};
  int             m_fd{-1};
  bool            m_needWakeup{};
  std::uint64_t   m_pendingAddr{};
};

#endif // ABTRDA3_AFXDPRX_H
