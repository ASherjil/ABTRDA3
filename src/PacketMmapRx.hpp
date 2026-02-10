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

  // HOT PATH - non-blocking, returns nullptr-span if no frame ready
  // The returned RxFrame::data point into ring. Valid release().
  [[nodiscard, gnu::always_inline]]
  RxFrame tryReceive() noexcept {
    tpacket_block_desc* block = reinterpret_cast<tpacket_block_desc*>(m_nextBlock);
    auto& bh = block->hdr.bh1;

    if ((bh.block_status & TP_STATUS_USER) == 0) [[likely]]{
      return {.data = {}, .sec = 0, .nsec = 0, .status = 0};
    }

    tpacket3_hdr* pkt = reinterpret_cast<tpacket3_hdr*>(m_nextBlock + bh.offset_to_first_pkt);

    return {
      .data = {reinterpret_cast<const std::uint8_t*>(pkt) + pkt->tp_mac, pkt->tp_snaplen},
      .sec  = pkt->tp_sec,
      .nsec = pkt->tp_nsec,
      .status = pkt->tp_status
    };
  }

  // HOT PATH - Release is seperated fromt he tryReceive()
  [[gnu::always_inline]]
  void release() noexcept {
    tpacket_block_desc* block = reinterpret_cast<tpacket_block_desc*>(m_nextBlock);

    std::atomic_thread_fence(std::memory_order_release);
    block->hdr.bh1.block_status = TP_STATUS_KERNEL; // hand the space back to the kernel

    m_nextBlock += m_blockSize;
    if (m_nextBlock >= m_ringEnd) {
      m_nextBlock = m_ringBase;
    }
  }
private:
  SocketOps m_socketHandler;
  std::uint8_t* m_nextBlock;
  std::uint8_t* m_ringBase;
  std::uint8_t* m_ringEnd;
  std::uint32_t m_blockSize;
};

#endif // ABTRDA3_PACKETMMAPRX_H
