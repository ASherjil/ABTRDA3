//
// Created by asherjil on 3/3/26.
//

#include "AFXDPRx.hpp"

#include <utility>

AFXDPRx::AFXDPRx(const AFXDPSocket& sock)
  : m_umem{sock.umemArea()},
    m_rxDescs{sock.rxRing.descs},
    m_rxProdPtr{sock.rxRing.producer},
    m_rxConsPtr{sock.rxRing.consumer},
    m_rxMask{sock.rxRing.mask},
    m_fillDescs{sock.fillRing.descs},
    m_fillProdPtr{sock.fillRing.producer},
    m_fillMask{sock.fillRing.mask},
    m_fillFlags{sock.fillRing.flags},
    m_fd{sock.fd()} {

  // Pre-fill the Fill ring with RX frame addresses.
  // RX frames use the second half of UMEM.
  // This tells the kernel: "here are empty buffers, place received packets here."
  std::uint32_t rxFrames = sock.rxFrames();
  std::uint32_t txFrames = sock.txFrames();  // offset: RX starts after TX
  for (std::uint32_t i = 0; i < rxFrames; ++i) {
    std::uint64_t addr = static_cast<std::uint64_t>(txFrames + i) * sock.frameSize();
    m_fillDescs[i & m_fillMask] = addr;
  }

  // Publish all Fill ring entries to the kernel
  m_fillProd = rxFrames;
  std::atomic_ref<std::uint32_t> fprod(*m_fillProdPtr);
  fprod.store(m_fillProd, std::memory_order_release);
}

AFXDPRx::AFXDPRx(AFXDPRx&& other) noexcept
    : m_umem{std::exchange(other.m_umem, nullptr)},
      m_rxDescs{std::exchange(other.m_rxDescs, nullptr)},
      m_rxProdPtr{std::exchange(other.m_rxProdPtr, nullptr)},
      m_rxConsPtr{std::exchange(other.m_rxConsPtr, nullptr)},
      m_rxCons{std::exchange(other.m_rxCons, 0)},
      m_rxMask{std::exchange(other.m_rxMask, 0)},
      m_fillDescs{std::exchange(other.m_fillDescs, nullptr)},
      m_fillProdPtr{std::exchange(other.m_fillProdPtr, nullptr)},
      m_fillProd{std::exchange(other.m_fillProd, 0)},
      m_fillMask{std::exchange(other.m_fillMask, 0)},
      m_fillFlags{std::exchange(other.m_fillFlags, nullptr)},
      m_fd{std::exchange(other.m_fd, -1)},
      m_pendingAddr{other.m_pendingAddr} {}

AFXDPRx& AFXDPRx::operator=(AFXDPRx&& other) noexcept {
  if (this != &other) {
    m_umem        = std::exchange(other.m_umem, nullptr);
    m_rxDescs     = std::exchange(other.m_rxDescs, nullptr);
    m_rxProdPtr   = std::exchange(other.m_rxProdPtr, nullptr);
    m_rxConsPtr   = std::exchange(other.m_rxConsPtr, nullptr);
    m_rxCons      = std::exchange(other.m_rxCons, 0);
    m_rxMask      = std::exchange(other.m_rxMask, 0);
    m_fillDescs   = std::exchange(other.m_fillDescs, nullptr);
    m_fillProdPtr = std::exchange(other.m_fillProdPtr, nullptr);
    m_fillProd    = std::exchange(other.m_fillProd, 0);
    m_fillMask    = std::exchange(other.m_fillMask, 0);
    m_fillFlags   = std::exchange(other.m_fillFlags, nullptr);
    m_fd          = std::exchange(other.m_fd, -1);
    m_pendingAddr = other.m_pendingAddr;
  }
  return *this;
}
