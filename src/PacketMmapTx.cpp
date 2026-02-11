//
// Created by asherjil on 2/10/26.
//

#include "PacketMmapTx.hpp"
#include <utility>


PacketMmapTx::PacketMmapTx(const RingConfig& cfg)
  :m_frameSize{cfg.blockSize}, m_socketHandler{cfg} {

  if (cfg.packetVersion != TPACKET_V2) {
    throw std::invalid_argument("PacketMmapTx requires the TPACKET_V2 ring configuration");
  }

  m_fd        = m_socketHandler.m_fd;
  m_ringBase  = static_cast<uint8_t*>(m_socketHandler.m_ringAddress);
  m_ringEnd   = m_ringBase + m_socketHandler.m_ringSize;
  m_nextSlot  = m_ringBase;
}


PacketMmapTx::PacketMmapTx(PacketMmapTx&& other) noexcept
  :m_nextSlot{std::exchange(other.m_nextSlot, nullptr)},
   m_ringBase{std::exchange(other.m_ringBase, nullptr)},
   m_ringEnd{std::exchange(other.m_ringEnd, nullptr)},
   m_fd{std::exchange(other.m_fd, -1)},
   m_frameSize{std::exchange(other.m_frameSize, 0)},
   m_socketHandler{std::move(other.m_socketHandler)}{
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

