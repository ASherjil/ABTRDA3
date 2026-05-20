//
// Created by asherjil on 3/3/26.
// Refactored 5/2026 — libxdp ring access.
//

#include "AFXDPRx.hpp"
#include <cstdio>
#include <cstdlib>
#include <utility>

AFXDPRx::AFXDPRx(AFXDPSocket& sock)
  : m_umem{sock.umemArea()},
    m_rxRing{&sock.rxRing()},
    m_fillRing{&sock.fillRing()},
    m_fd{sock.fd()},
    m_needWakeup{sock.needWakeup()} {

  // Pre-fill the fill ring with RX buffer addresses.
  // RX frames occupy the second half of UMEM.
  std::uint32_t rxFrames = sock.rxFrames();
  std::uint32_t txFrames = sock.txFrames();

  // Reserve and fill all RX frames
  __u32 idx;
  if (xsk_ring_prod__reserve(m_fillRing, rxFrames, &idx) < rxFrames) {
    std::fprintf(stderr, "AFXDPRx: fill ring reserve failed\n");
    std::abort();
  }

  for (std::uint32_t i = 0; i < rxFrames; ++i)
    *xsk_ring_prod__fill_addr(m_fillRing, idx++) =
      static_cast<std::uint64_t>(txFrames + i) * sock.frameSize();

  xsk_ring_prod__submit(m_fillRing, rxFrames);

  // Kick the kernel to notice the fill ring buffers
  if (xsk_ring_prod__needs_wakeup(m_fillRing) || !m_needWakeup)
    ::recvfrom(m_fd, nullptr, 0, MSG_DONTWAIT, nullptr, nullptr);
}

AFXDPRx::AFXDPRx(AFXDPRx&& other) noexcept
  : m_umem{std::exchange(other.m_umem, nullptr)},
    m_rxRing{std::exchange(other.m_rxRing, nullptr)},
    m_fillRing{std::exchange(other.m_fillRing, nullptr)},
    m_fd{std::exchange(other.m_fd, -1)},
    m_needWakeup{other.m_needWakeup},
    m_pendingAddr{other.m_pendingAddr} {}

AFXDPRx& AFXDPRx::operator=(AFXDPRx&& other) noexcept {
  if (this != &other) {
    m_umem        = std::exchange(other.m_umem, nullptr);
    m_rxRing      = std::exchange(other.m_rxRing, nullptr);
    m_fillRing    = std::exchange(other.m_fillRing, nullptr);
    m_fd          = std::exchange(other.m_fd, -1);
    m_needWakeup  = other.m_needWakeup;
    m_pendingAddr = other.m_pendingAddr;
  }
  return *this;
}
