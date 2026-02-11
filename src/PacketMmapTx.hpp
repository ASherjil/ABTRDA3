//
// Created by asherjil on 2/10/26.
//

#ifndef ABTRDA3_PACKETMMAPTX_H
#define ABTRDA3_PACKETMMAPTX_H

#include "SocketOps.hpp"
#include <cstdint>
#include <cstring>
#include <span>
#include <atomic>
#include <linux/if_packet.h>
#include <sys/socket.h>


static constexpr std::uint32_t kDataOffset = TPACKET_ALIGN(sizeof(tpacket2_hdr));

class PacketMmapTx {
public:
  explicit PacketMmapTx(const RingConfig& cfg);
  PacketMmapTx(const PacketMmapTx&) = delete;
  PacketMmapTx(PacketMmapTx&& other) noexcept;

  PacketMmapTx& operator=(const PacketMmapTx&) = delete;
  PacketMmapTx& operator=(PacketMmapTx&& other) noexcept;

  ~PacketMmapTx() = default;

  // HOT PATH function set to inline
  [[nodiscard, gnu::always_inline]]
  bool send(std::span<const std::uint8_t> frame) noexcept {
    tpacket2_hdr* hdr = reinterpret_cast<tpacket2_hdr*>(m_nextSlot);

    // This is less frequently to trigger to let the compiler optimise it away
    if (hdr->tp_status != TP_STATUS_AVAILABLE)[[unlikely]] {
      return false;
    }

    std::size_t currentFrameSize = frame.size();
    // Let the compiler also optimise it away
    if (currentFrameSize > m_frameSize - kDataOffset)[[unlikely]]{
      return false;
    }
    std::memcpy(m_nextSlot + kDataOffset, frame.data(), currentFrameSize);
    hdr->tp_len     = static_cast<std::uint32_t>(currentFrameSize);
    hdr->tp_snaplen = static_cast<std::uint32_t>(currentFrameSize);

    std::atomic_thread_fence(std::memory_order_release);
    hdr->tp_status = TP_STATUS_SEND_REQUEST;

    ::send(m_fd, nullptr, 0, MSG_DONTWAIT);

    // Advance: one add + one branch. No multiply and no modulo.
    m_nextSlot += m_frameSize;
    if (m_nextSlot >= m_ringEnd) {// if the next slot reaches the end
      m_nextSlot = m_ringBase;// reset it back to the start
    }
    __builtin_prefetch(m_nextSlot, 0, 3);  // read, highest temporal locality

    return true;
  }
private:
  // HOT: accessed every send(), packed into one cache line (32 bytes)
  std::uint8_t* m_nextSlot;    // points directly into the ring
  std::uint8_t* m_ringBase;    // ring start
  std::uint8_t* m_ringEnd;     // ring start + ring_size
  int           m_fd;          // copied from SocketOps, avoids indirection
  std::uint32_t m_frameSize;
  // COLD: constructor/destructor only
  SocketOps     m_socketHandler;
};

#endif // ABTRDA3_PACKETMMAPTX_H
