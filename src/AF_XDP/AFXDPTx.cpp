//
// Created by asherjil on 3/3/26.
// Refactored 5/2026 — libxdp ring accessors.
//

#include "AFXDPTx.hpp"
#include <utility>

AFXDPTx::AFXDPTx(AFXDPSocket& sock)
  : m_umem{sock.umemArea()},
    m_txRing{&sock.txRing()},
    m_compRing{&sock.compRing()},
    m_fd{sock.fd()},
    m_needWakeup{sock.needWakeup()},
    m_frameSize{sock.frameSize()},
    m_txFrameCount{sock.txFrames()} {

  // Pre-fill free stack with TX frame addresses
  m_freeStackStorage = std::make_unique<std::uint64_t[]>(m_txFrameCount);
  m_freeStack = m_freeStackStorage.get();
  m_freeTop   = 0;

  for (std::uint32_t i = 0; i < m_txFrameCount; ++i)
    m_freeStack[m_freeTop++] = static_cast<std::uint64_t>(i) * m_frameSize;
}

AFXDPTx::AFXDPTx(AFXDPTx&& other) noexcept
  : m_umem{std::exchange(other.m_umem, nullptr)},
    m_txRing{std::exchange(other.m_txRing, nullptr)},
    m_compRing{std::exchange(other.m_compRing, nullptr)},
    m_fd{std::exchange(other.m_fd, -1)},
    m_freeStack{std::exchange(other.m_freeStack, nullptr)},
    m_freeTop{std::exchange(other.m_freeTop, 0)},
    m_pendingAddr{other.m_pendingAddr},
    m_pendingLen{other.m_pendingLen},
    m_needWakeup{other.m_needWakeup},
    m_frameSize{std::exchange(other.m_frameSize, 0)},
    m_txFrameCount{std::exchange(other.m_txFrameCount, 0)},
    m_freeStackStorage{std::move(other.m_freeStackStorage)} {}

AFXDPTx& AFXDPTx::operator=(AFXDPTx&& other) noexcept {
  if (this != &other) {
    m_umem        = std::exchange(other.m_umem, nullptr);
    m_txRing      = std::exchange(other.m_txRing, nullptr);
    m_compRing    = std::exchange(other.m_compRing, nullptr);
    m_fd          = std::exchange(other.m_fd, -1);
    m_freeStack   = std::exchange(other.m_freeStack, nullptr);
    m_freeTop     = std::exchange(other.m_freeTop, 0);
    m_pendingAddr = other.m_pendingAddr;
    m_pendingLen  = other.m_pendingLen;
    m_needWakeup  = other.m_needWakeup;
    m_frameSize   = std::exchange(other.m_frameSize, 0);
    m_txFrameCount = std::exchange(other.m_txFrameCount, 0);
    m_freeStackStorage = std::move(other.m_freeStackStorage);
  }
  return *this;
}

void AFXDPTx::prefillRing(std::span<const std::uint8_t> frameTemplate) const noexcept {
  for (std::uint32_t i = 0; i < m_txFrameCount; ++i) {
    auto addr = static_cast<std::uint64_t>(i) * m_frameSize;
    std::memcpy(m_umem + addr, frameTemplate.data(), frameTemplate.size());
  }
}
