//
// Created by asherjil on 3/3/26.
//

#include "AFXDPTx.hpp"

#include <utility>

AFXDPTx::AFXDPTx(const AFXDPSocket& sock)
  : m_umem{sock.umemArea()},
    m_txDescs{sock.txRing.descs},
    m_txProdPtr{sock.txRing.producer},
    m_txMask{sock.txRing.mask},
    m_fd{sock.fd()},
    m_txFlags{sock.txRing.flags},
    m_needWakeup{sock.needWakeup()},
    m_compDescs{sock.compRing.descs},
    m_compProdPtr{sock.compRing.producer},
    m_compConsPtr{sock.compRing.consumer},
    m_compMask{sock.compRing.mask},
    m_frameSize{sock.frameSize()},
    m_txFrameCount{sock.txFrames()} {

  // Allocate free stack and pre-fill with TX frame addresses.
  // TX frames use the first half of UMEM: addresses [0, frameSize, 2*frameSize, ...]
  m_freeStackStorage = std::make_unique<std::uint64_t[]>(m_txFrameCount);
  m_freeStack = m_freeStackStorage.get();
  m_freeTop   = 0;

  for (std::uint32_t i = 0; i < m_txFrameCount; ++i) {
      m_freeStack[m_freeTop++] = static_cast<std::uint64_t>(i) * m_frameSize;
  }
}


AFXDPTx::AFXDPTx(AFXDPTx&& other) noexcept
  : m_umem{std::exchange(other.m_umem, nullptr)},
    m_txDescs{std::exchange(other.m_txDescs, nullptr)},
    m_txProdPtr{std::exchange(other.m_txProdPtr, nullptr)},
    m_freeStack{std::exchange(other.m_freeStack, nullptr)},
    m_txProd{std::exchange(other.m_txProd, 0)},
    m_txMask{std::exchange(other.m_txMask, 0)},
    m_freeTop{std::exchange(other.m_freeTop, 0)},
    m_fd{std::exchange(other.m_fd, -1)},
    m_txFlags{std::exchange(other.m_txFlags, nullptr)},
    m_needWakeup{other.m_needWakeup},
    m_pendingAddr{other.m_pendingAddr},
    m_pendingLen{other.m_pendingLen},
    m_compDescs{std::exchange(other.m_compDescs, nullptr)},
    m_compProdPtr{std::exchange(other.m_compProdPtr, nullptr)},
    m_compConsPtr{std::exchange(other.m_compConsPtr, nullptr)},
    m_compCons{std::exchange(other.m_compCons, 0)},
    m_compMask{std::exchange(other.m_compMask, 0)},
    m_frameSize{std::exchange(other.m_frameSize, 0)},
    m_txFrameCount{std::exchange(other.m_txFrameCount, 0)},
    m_freeStackStorage{std::move(other.m_freeStackStorage)} {}

AFXDPTx& AFXDPTx::operator=(AFXDPTx&& other) noexcept {
  if (this != &other) {
      m_umem          = std::exchange(other.m_umem, nullptr);
      m_txDescs       = std::exchange(other.m_txDescs, nullptr);
      m_txProdPtr     = std::exchange(other.m_txProdPtr, nullptr);
      m_freeStack     = std::exchange(other.m_freeStack, nullptr);
      m_txProd        = std::exchange(other.m_txProd, 0);
      m_txMask        = std::exchange(other.m_txMask, 0);
      m_freeTop       = std::exchange(other.m_freeTop, 0);
      m_fd            = std::exchange(other.m_fd, -1);
      m_txFlags       = std::exchange(other.m_txFlags, nullptr);
      m_needWakeup    = other.m_needWakeup;
      m_pendingAddr   = other.m_pendingAddr;
      m_pendingLen    = other.m_pendingLen;
      m_compDescs     = std::exchange(other.m_compDescs, nullptr);
      m_compProdPtr   = std::exchange(other.m_compProdPtr, nullptr);
      m_compConsPtr   = std::exchange(other.m_compConsPtr, nullptr);
      m_compCons      = std::exchange(other.m_compCons, 0);
      m_compMask      = std::exchange(other.m_compMask, 0);
      m_frameSize     = std::exchange(other.m_frameSize, 0);
      m_txFrameCount  = std::exchange(other.m_txFrameCount, 0);
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
