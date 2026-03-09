//
// Created by asherjil on 3/3/26.
//

#ifndef ABTRDA3_AFXDPTX_H
#define ABTRDA3_AFXDPTX_H

#include "AFXDPSocket.hpp"

#include <atomic>
#include <cstring>
#include <memory>
#include <span>
#include <sys/socket.h>

// ── Zero-copy acquire/commit API ─────────────────────────────────────
//
// acquire(): Pop a free UMEM frame address from the free stack.
//            Returns pointer directly into UMEM — write your frame there.
//            Unlike packet_mmap, there's NO header before the data.
//            The pointer IS the start of the Ethernet frame.
//
// commit():  Push the frame into the TX ring and kick the kernel.

/*
 * We use the TX/COMPLETION rings here. The main rule is always "whoever puts the data in bumps the producer, who takes the data out
 * bumps the consumer".
 * TX Ring: User -> increment producer, kernel -> increment the consumer
 * COMPLETION Ring: kernel -> increments the producer, User -> increments the consumer  
 */

class AFXDPTx {
public:
  explicit AFXDPTx(const AFXDPSocket& sock);
  AFXDPTx(const AFXDPTx&) = delete;
  AFXDPTx& operator=(const AFXDPTx&) = delete;
  AFXDPTx(AFXDPTx&& other) noexcept;
  AFXDPTx& operator=(AFXDPTx&& other) noexcept;

  // ── Copy-based send (convenience API) ────────────────────────────────
  [[nodiscard, gnu::always_inline]]
  inline bool send(std::span<const std::uint8_t> frame) noexcept {
    auto* dst = acquire(static_cast<std::uint32_t>(frame.size()));
    if (!dst) [[unlikely]]
        return false;
    std::memcpy(dst, frame.data(), frame.size());
    commit();
    return true;
  }


  [[nodiscard, gnu::always_inline]]
  inline std::uint8_t* acquire(std::uint32_t frameLen) noexcept {
      // Try to reclaim completed TX frames first if free stack is empty
      if (m_freeTop == 0) [[unlikely]] {
          reclaimCompleted();
          if (m_freeTop == 0) [[unlikely]]
              return nullptr;  // all frames still in-flight
      }

      m_pendingAddr = m_freeStack[--m_freeTop];
      m_pendingLen  = frameLen;

      return m_umem + m_pendingAddr;
  }

  [[gnu::always_inline]]
  inline void commit() noexcept {
    // Write descriptor into TX ring: {UMEM address, frame length}
    std::uint32_t idx = m_txProd & m_txMask;
    m_txDescs[idx].addr    = m_pendingAddr;
    m_txDescs[idx].len     = m_pendingLen;
    m_txDescs[idx].options = 0;

    // Release-store producer index so kernel sees the new descriptor.
    // This is the AF_XDP equivalent of setting TP_STATUS_SEND_REQUEST.
    ++m_txProd;
    std::atomic_ref<std::uint32_t> prod(*m_txProdPtr);
    prod.store(m_txProd, std::memory_order_release);

    // Kick the kernel to transmit. If NEED_WAKEUP is supported by the driver,
    // only kick when the kernel signals it has gone idle (~300ns saved per packet).
    // Without driver support, always kick.
    if (!m_needWakeup ||
        (std::atomic_ref<std::uint32_t>(*m_txFlags).load(std::memory_order_relaxed)
         & XDP_RING_NEED_WAKEUP)) [[unlikely]] {
      // Retry on EAGAIN/EBUSY — kernel TX queue momentarily full.
      // Reclaim completed frames between retries to free ring space.
      while (::sendto(m_fd, nullptr, 0, MSG_DONTWAIT, nullptr, 0) < 0) {
        if (errno != EAGAIN && errno != EBUSY) [[unlikely]] break;
        reclaimCompleted();
      }
    }
  }

  void prefillRing(std::span<const std::uint8_t> frameTemplate) const noexcept {
    for (std::uint32_t i = 0; i < m_txFrameCount; ++i) {
      auto addr = static_cast<std::uint64_t>(i) * m_frameSize;
      std::memcpy(m_umem + addr, frameTemplate.data(), frameTemplate.size());
    }
  }
private:

  [[gnu::always_inline]]
  inline void reclaimCompleted() noexcept {
      std::atomic_ref<std::uint32_t> prod(*m_compProdPtr);
      std::uint32_t produced = prod.load(std::memory_order_acquire);

      while (m_compCons != produced) {
          std::uint64_t addr = m_compDescs[m_compCons & m_compMask];
          m_freeStack[m_freeTop++] = addr;
          ++m_compCons;
      }

      // Release-store consumer so kernel knows we've consumed these entries
      // and can reuse the Completion ring slots.
      std::atomic_ref<std::uint32_t> cons(*m_compConsPtr);
      cons.store(m_compCons, std::memory_order_release);
  }

  // ── Hot members (fits in ~64 bytes cache line) ───────────────────────
  std::uint8_t*  m_umem{};           // UMEM base pointer
  xdp_desc*      m_txDescs{};        // TX ring descriptor array
  std::uint32_t* m_txProdPtr{};      // TX ring producer (shared with kernel)
  std::uint64_t* m_freeStack{};      // free-address stack (raw ptr to owned array)
  std::uint32_t  m_txProd{};         // cached TX producer index
  std::uint32_t  m_txMask{};         // TX ring index mask (size - 1)
  std::uint32_t  m_freeTop{};        // free stack pointer (next pop position)
  int            m_fd{-1};           // socket fd (for sendto kick)
  std::uint32_t* m_txFlags{};        // TX ring flags (kernel sets XDP_RING_NEED_WAKEUP here)
  bool           m_needWakeup{};     // driver supports NEED_WAKEUP optimisation
  std::uint64_t  m_pendingAddr{};    // frame addr from last acquire()
  std::uint32_t  m_pendingLen{};     // frame len from last acquire()

  // ── Cold members (Completion ring — only touched in reclaimCompleted) ─
  std::uint64_t* m_compDescs{};      // Completion ring descriptor array
  std::uint32_t* m_compProdPtr{};    // Completion ring producer (kernel writes)
  std::uint32_t* m_compConsPtr{};    // Completion ring consumer (we write)
  std::uint32_t  m_compCons{};       // cached Completion consumer index
  std::uint32_t  m_compMask{};       // Completion ring index mask

  std::uint32_t  m_frameSize{};
  std::uint32_t  m_txFrameCount{};

  // Ownership of free stack memory (cold — only used for deallocation)
  std::unique_ptr<std::uint64_t[]> m_freeStackStorage;
};

#endif // ABTRDA3_AFXDPTX_H
