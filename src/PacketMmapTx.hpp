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

  // ── Copy-based send (convenience API) ────────────────────────────
  [[nodiscard, gnu::always_inline]]
  bool send(std::span<const std::uint8_t> frame) noexcept {
    auto* dst = acquire(static_cast<std::uint32_t>(frame.size()));
    if (!dst) [[unlikely]]
      return false;
    std::memcpy(dst, frame.data(), frame.size());
    commit();
    return true;
  }

  // ── Zero-copy acquire/commit API ───────────────────────────────
  // acquire() returns a pointer directly into the TX ring slot.
  // Write the full Ethernet frame (dst+src+ethertype+payload) there.
  // Call commit() when done to trigger kernel transmission.
  // Returns nullptr if the ring slot is not available.
  [[nodiscard, gnu::always_inline]]
  std::uint8_t* acquire(std::uint32_t frameLen) noexcept {
    auto* hdr = reinterpret_cast<tpacket2_hdr*>(m_nextSlot);

    if (hdr->tp_status != TP_STATUS_AVAILABLE) [[unlikely]]
      return nullptr;
    if (frameLen > m_frameSize - kDataOffset) [[unlikely]]
      return nullptr;

    hdr->tp_len     = frameLen;
    hdr->tp_snaplen = frameLen;

    return m_nextSlot + kDataOffset;
  }

  [[gnu::always_inline]]
  void commit() noexcept {
    auto* hdr = reinterpret_cast<tpacket2_hdr*>(m_nextSlot);

    std::atomic_thread_fence(std::memory_order_release);
    hdr->tp_status = TP_STATUS_SEND_REQUEST;

    ::send(m_fd, nullptr, 0, MSG_DONTWAIT);

    m_nextSlot += m_frameSize;
    if (m_nextSlot >= m_ringEnd)
      m_nextSlot = m_ringBase;
    __builtin_prefetch(m_nextSlot, 1, 3);  // WRITE prefetch: RFO into M-state
  }

  // ── Ring pre-fill (call once at startup) ─────────────────────────
  // Stamps a fixed frame template into every AVAILABLE ring slot.
  // The kernel never touches the data area on recycle (only tp_status),
  // so the template persists across sends — hot path only writes the
  // bytes that actually change.
  void prefillRing(std::span<const std::uint8_t> frameTemplate) noexcept {
    for (auto* slot = m_ringBase; slot < m_ringEnd; slot += m_frameSize) {
      auto* hdr = reinterpret_cast<tpacket2_hdr*>(slot);
      if (hdr->tp_status == TP_STATUS_AVAILABLE) {
        std::memcpy(slot + kDataOffset, frameTemplate.data(), frameTemplate.size());
      }
    }
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
