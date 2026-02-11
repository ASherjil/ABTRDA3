//
// Created by asherjil on 2/10/26.
//

#ifndef ABTRDA3_PACKETMMAPRX_H
#define ABTRDA3_PACKETMMAPRX_H

#include "SocketOps.hpp"
#include <cstdint>
#include <span>
#include <atomic>
#include <linux/if_packet.h>


struct RxFrame {
  std::span<const std::uint8_t> data;
  std::uint32_t sec;
  std::uint32_t nsec;
  std::uint32_t status;
};

class PacketMmapRx {
public:
  explicit PacketMmapRx(const RingConfig& cfg);
  PacketMmapRx(const PacketMmapRx&) = delete;
  PacketMmapRx(PacketMmapRx&& other) noexcept;

  PacketMmapRx& operator=(const PacketMmapRx&) = delete;
  PacketMmapRx& operator=(PacketMmapRx&& other) noexcept;

  ~PacketMmapRx() = default;

  // HOT PATH - non-blocking, returns empty span if no frame ready.
  // The returned RxFrame::data points into the ring. Valid until release().
  // A block may contain multiple packets. tryReceive() returns them one at a
  // time; release() advances to the next packet, releasing the block back to
  // the kernel only after the last packet is consumed.
  [[nodiscard, gnu::always_inline]]
  RxFrame tryReceive() noexcept {
    // If we're already inside a block, return the current packet
    tpacket2_hdr* hdr = reinterpret_cast<tpacket2_hdr*>(m_nextFrame);

    if ((hdr->tp_status & TP_STATUS_USER) == 0) [[likely]] {
      return {};
    }

    // Data is ready! Return pointer to payload
    return {
      .data = {reinterpret_cast<const std::uint8_t*>(hdr) + hdr->tp_mac, hdr->tp_snaplen},
      .sec  = hdr->tp_sec,
      .nsec = hdr->tp_nsec,
      .status = hdr->tp_status
    };
  }

  // HOT PATH - advance to next packet in block, or release block if last.
  [[gnu::always_inline]]
  void release() noexcept {
    tpacket2_hdr* hdr = reinterpret_cast<tpacket2_hdr*>(m_nextFrame);

    // 1. Mark frame as free for the Kernal to use again
    std::atomic_thread_fence(std::memory_order_release);
    hdr->tp_status = TP_STATUS_KERNEL;

    // Move along to the next frame in the buffer
    m_nextFrame += m_frameSize;
    if (m_nextFrame >= m_ringEnd) {
      m_nextFrame = m_ringBase;
    }
  }

private:
  SocketOps m_socketHandler;
  std::uint8_t* m_nextFrame;
  std::uint8_t* m_ringBase;
  std::uint8_t* m_ringEnd;
  std::uint32_t m_frameSize;
};

#endif // ABTRDA3_PACKETMMAPRX_H
