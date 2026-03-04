//
// Created by asherjil on 3/3/26.
//

#ifndef ABTRDA3_AFXDPRX_H
#define ABTRDA3_AFXDPRX_H
#include "AFXDPSocket.hpp"
#include "RxFrame.hpp"

#include <atomic>
#include <sys/socket.h>

/*
 * We use the RX/FILL rings here. The main rule is always "whoever puts the data in bumps the producer, who takes the data out
 * bumps the consumer".
 * First we give chunks from FILL ring to the kernel to receive data. Then the kernel will put the data in the RX ring.
 * FILL ring: User -> increment producer, kernel -> increment consumer
 * RX ring : kernel -> increment producer, User -> increment consumer 
 */
class AFXDPRx {
public:
    explicit AFXDPRx(const AFXDPSocket& sock);

    AFXDPRx(const AFXDPRx&) = delete;
    AFXDPRx& operator=(const AFXDPRx&) = delete;
    AFXDPRx(AFXDPRx&& other) noexcept;
    AFXDPRx& operator=(AFXDPRx&& other) noexcept;
    ~AFXDPRx() = default;

    // ── Hot path: non-blocking receive ───────────────────────────────────
    [[nodiscard, gnu::always_inline]]
    inline RxFrame tryReceive() noexcept {
        std::atomic_ref<std::uint32_t> prod(*m_rxProdPtr);
        std::uint32_t produced = prod.load(std::memory_order_acquire);

        if (produced == m_rxCons) [[likely]]
            return {};   // nothing new — empty data signals "no frame"

        // Kernel placed an xdp_desc in the RX ring: {addr, len, options}
        const xdp_desc& desc = m_rxDescs[m_rxCons & m_rxMask];
        m_pendingAddr = desc.addr;

        return {
            .data   = {m_umem + desc.addr, desc.len},
            .sec    = 0,
            .nsec   = 0,
            .status = 1   // non-zero = frame present
        };
    }


    [[gnu::always_inline]]
    inline void release() noexcept {
        // Advance RX consumer
        ++m_rxCons;
        std::atomic_ref<std::uint32_t> cons(*m_rxConsPtr);
        cons.store(m_rxCons, std::memory_order_release);

        // Return frame address to Fill ring (recycle the buffer for kernel)
        std::uint32_t idx = m_fillProd & m_fillMask;
        m_fillDescs[idx] = m_pendingAddr;
        ++m_fillProd;

        std::atomic_ref<std::uint32_t> fprod(*m_fillProdPtr);
        fprod.store(m_fillProd, std::memory_order_release);

        // Wake the kernel to refill RX buffers. With NEED_WAKEUP driver support,
        // only kick when idle. Without it, always kick.
        if (!m_needWakeup ||
            (std::atomic_ref<std::uint32_t>(*m_fillFlags).load(std::memory_order_relaxed)
             & XDP_RING_NEED_WAKEUP)) [[unlikely]]
          ::recvfrom(m_fd, nullptr, 0, MSG_DONTWAIT, nullptr, nullptr);
    }
private:
  // ── Hot members ──────────────────────────────────────────────────────
  std::uint8_t*  m_umem{};           // UMEM base pointer
  xdp_desc*      m_rxDescs{};        // RX ring descriptor array
  std::uint32_t* m_rxProdPtr{};      // RX ring producer (kernel writes)
  std::uint32_t* m_rxConsPtr{};      // RX ring consumer (we write)
  std::uint32_t  m_rxCons{};         // cached RX consumer index
  std::uint32_t  m_rxMask{};         // RX ring index mask

  std::uint64_t* m_fillDescs{};      // Fill ring descriptor array
  std::uint32_t* m_fillProdPtr{};    // Fill ring producer (we write)
  std::uint32_t  m_fillProd{};       // cached Fill producer index
  std::uint32_t  m_fillMask{};       // Fill ring index mask
  std::uint32_t* m_fillFlags{};      // Fill ring flags (kernel sets XDP_RING_NEED_WAKEUP)
  int            m_fd{-1};           // socket fd (for recvfrom wakeup)
  bool           m_needWakeup{};     // driver supports NEED_WAKEUP optimisation

  std::uint64_t  m_pendingAddr{};    // frame addr to recycle on release()
};

#endif // ABTRDA3_AFXDPRX_H
