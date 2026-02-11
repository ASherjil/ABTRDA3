//
// Created by asherjil on 2/10/26.
//

#include "PacketMmapRx.hpp"
#include <utility>
#include <stdexcept>

PacketMmapRx::PacketMmapRx(const RingConfig& cfg)
  :m_frameSize{cfg.blockSize}, m_socketHandler{cfg}{

  // Safety: Ensure user didn't ask for V3
    if (cfg.packetVersion != TPACKET_V2) {
      throw std::invalid_argument("PacketMmapRx optimized for TPACKET_V2 only");
    }

  m_ringBase  = static_cast<std::uint8_t*>(m_socketHandler.m_ringAddress);
  m_ringEnd   = m_ringBase + m_socketHandler.m_ringSize;
  m_nextFrame = m_ringBase;
}


PacketMmapRx::PacketMmapRx(PacketMmapRx&& other) noexcept
  :m_nextFrame{std::exchange(other.m_nextFrame, nullptr)},
   m_ringBase{std::exchange(other.m_ringBase, nullptr)},
   m_ringEnd{std::exchange(other.m_ringEnd, nullptr)},
   m_frameSize{std::exchange(other.m_frameSize, 0)},
   m_socketHandler{std::move(other.m_socketHandler)}{
}


PacketMmapRx& PacketMmapRx::operator=(PacketMmapRx&& other) noexcept {
  if (this != &other) {
    m_socketHandler = std::move(other.m_socketHandler);
    m_nextFrame     = std::exchange(other.m_nextFrame, nullptr);
    m_ringBase      = std::exchange(other.m_ringBase, nullptr);
    m_ringEnd       = std::exchange(other.m_ringEnd, nullptr);
    m_frameSize     = std::exchange(other.m_frameSize, 0);
  }
  return *this;
}