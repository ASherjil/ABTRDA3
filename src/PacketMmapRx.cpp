//
// Created by asherjil on 2/10/26.
//

#include "PacketMmapRx.hpp"
#include <utility>

PacketMmapRx::PacketMmapRx(const RingConfig& cfg)
  :m_socketHandler{cfg}, m_blockSize{cfg.blockSize}{

  m_ringBase  = static_cast<std::uint8_t*>(m_socketHandler.m_ringAddress);
  m_ringEnd   = m_ringBase + m_socketHandler.m_ringSize;
  m_nextBlock = m_ringBase;
}


PacketMmapRx::PacketMmapRx(PacketMmapRx&& other) noexcept
  :m_socketHandler{std::move(other.m_socketHandler)},
   m_nextBlock{std::exchange(other.m_nextBlock, nullptr)},
   m_ringBase{std::exchange(other.m_ringBase, nullptr)},
   m_ringEnd{std::exchange(other.m_ringEnd, nullptr)},
   m_blockSize{std::exchange(other.m_blockSize, 0)}{
}


PacketMmapRx& PacketMmapRx::operator=(PacketMmapRx&& other) noexcept {
  if (this != &other) {
    m_socketHandler = std::move(other.m_socketHandler);
    m_nextBlock     = std::exchange(other.m_nextBlock, nullptr);
    m_ringBase      = std::exchange(other.m_ringBase, nullptr);
    m_ringEnd       = std::exchange(other.m_ringEnd, nullptr);
    m_blockSize     = std::exchange(other.m_blockSize, 0);
  }
  return *this;
}