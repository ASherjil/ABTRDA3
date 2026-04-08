//
// Created by asherjil on 2/10/26.
//

#ifndef ABTRDA3_PACKETMMAPRX_H
#define ABTRDA3_PACKETMMAPRX_H

#include "SocketOps.hpp"
#include "RxFrame.hpp"
#include <atomic>
#include <linux/if_packet.h>

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

    // Be VERY CAREFULL. Its either reinterpret_cast<volatile T*> or std::atomic_ref
    std::atomic_ref<std::uint32_t> currentStatus(hdr->tp_status);
    const std::uint32_t loadedStatus = currentStatus.load(std::memory_order_acquire);
    if ((loadedStatus & TP_STATUS_USER) == 0) [[likely]] {
      return {};
    }

    // Data is ready! Return pointer to payload
    return {
      .data = {reinterpret_cast<const std::uint8_t*>(hdr) + hdr->tp_mac, hdr->tp_snaplen},
      .sec  = hdr->tp_sec,
      .nsec = hdr->tp_nsec,
      .status = loadedStatus // we reuse the previosuly loaded value
    };
  }

  // HOT PATH - advance to next packet in block, or release block if last.
  [[gnu::always_inline]]
  void release() noexcept {
    tpacket2_hdr* hdr = reinterpret_cast<tpacket2_hdr*>(m_nextFrame);

    // Be VERY CAREFULL. Its either reinterpret_cast<volatile T*> or std::atomic_ref
    std::atomic_ref<std::uint32_t> currentStatus(hdr->tp_status);

    // Hand it back to the kernel
    currentStatus.store(TP_STATUS_KERNEL,std::memory_order_release);

    // Move along to the next frame in the buffer
    m_nextFrame += m_frameSize;
    if (m_nextFrame >= m_ringEnd) {
      m_nextFrame = m_ringBase;
    }
    __builtin_prefetch(m_nextFrame, 0, 3);  // read, highest temporal locality(ONLY FOR HIGH THROUGHPUT)
  }

private:
  // HOT: accessed every tryReceive()/release(), packed into one cache line (28 bytes)
  std::uint8_t* m_nextFrame;
  std::uint8_t* m_ringBase;
  std::uint8_t* m_ringEnd;
  std::uint32_t m_frameSize;
  // COLD: constructor/destructor only
  SocketOps     m_socketHandler;
};

#endif // ABTRDA3_PACKETMMAPRX_H
