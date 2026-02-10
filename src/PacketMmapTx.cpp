//
// Created by asherjil on 2/10/26.
//

#include "PacketMmapTx.hpp"
#include <utility>


PacketMmapTx::PacketMmapTx(const RingConfig& cfg)
  :m_socketHandler{cfg}, m_frameSize{cfg.blockSize} {

  m_fd        = m_socketHandler.m_fd;
  m_ringBase  = static_cast<uint8_t*>(m_socketHandler.m_ringAddress);
  m_ringEnd   = m_ringBase + m_socketHandler.m_ringSize;
  m_nextSlot  = m_ringBase;
}


PacketMmapTx::PacketMmapTx(PacketMmapTx&& other) noexcept
  :m_socketHandler{std::move(other.m_socketHandler)},
   m_fd{std::exchange(other.m_fd, -1)},
   m_nextSlot{std::exchange(other.m_nextSlot, nullptr)},
   m_ringBase{std::exchange(other.m_ringBase, nullptr)},
   m_ringEnd{std::exchange(other.m_ringEnd, nullptr)},
   m_frameSize{std::exchange(other.m_frameSize, 0)}{
}

PacketMmapTx& PacketMmapTx::operator=(PacketMmapTx&& other) noexcept{
  if (this != &other) {
    m_socketHandler = std::move(other.m_socketHandler);
    m_frameSize     = std::exchange(other.m_frameSize, 0);
    m_fd            = std::exchange(other.m_fd, -1);
    m_nextSlot      = std::exchange(other.m_nextSlot, nullptr);
    m_ringBase      = std::exchange(other.m_ringBase, nullptr);
    m_ringEnd       = std::exchange(other.m_ringEnd, nullptr);
  }
  return *this;
}

