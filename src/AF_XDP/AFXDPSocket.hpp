//
// Created by asherjil on 3/3/26.
// Refactored 5/2026 — xdpsock-style UMEM + socket initialisation.
//

#ifndef ABTRDA3_AFXDPSOCKET_H
#define ABTRDA3_AFXDPSOCKET_H

#include <cstddef>
#include <cstdint>
#include <memory>

#include <xdp/xsk.h>
#include <xdp/libxdp.h>

struct XdpConfig {
  const char*    interface;
  std::uint32_t  queueId     = 0;
  std::uint32_t  frameSize   = XSK_UMEM__DEFAULT_FRAME_SIZE;
  std::uint32_t  frameCount  = 4096;
  std::uint16_t  etherType   = 0x88B5;
  bool           needWakeup  = true;
  const char*    bpfProgPath = nullptr;  // xdpsock-style custom BPF .o
};

class AFXDPSocket {
public:
  static constexpr std::uint32_t BATCH_SIZE = 64;

  explicit AFXDPSocket(const XdpConfig& cfg);

  AFXDPSocket(const AFXDPSocket&) = delete;
  AFXDPSocket& operator=(const AFXDPSocket&) = delete;
  AFXDPSocket(AFXDPSocket&& other) noexcept;
  AFXDPSocket& operator=(AFXDPSocket&& other) noexcept;
  ~AFXDPSocket();

  // ── Ring accessors ──────────────────────────────────────────────────
  xsk_ring_prod&  txRing()       noexcept { return m_txRing; }
  xsk_ring_cons&  compRing()     noexcept { return m_compRing; }
  xsk_ring_cons&  rxRing()       noexcept { return m_rxRing; }
  xsk_ring_prod&  fillRing()     noexcept { return m_fillRing; }

  // ── Accessors ───────────────────────────────────────────────────────
  int             fd()           const noexcept { return xsk_socket__fd(m_xsk); }
  std::uint8_t*   umemArea()     const noexcept { return m_umemBuffer; }
  std::uint32_t   frameSize()    const noexcept { return m_frameSize; }
  std::uint32_t   numFrames()    const noexcept { return m_frameCount; }
  bool            needWakeup()   const noexcept { return m_needWakeup; }
  bool            isCopyMode()   const noexcept { return m_copyMode; }
  std::uint32_t   bindFlags()    const noexcept { return m_bindFlags; }

  // ── Fill ring initial population (xdpsock: xsk_populate_fill_ring) ─
  void populateFillRing();

private:
  void cleanup() noexcept;
  void loadCustomBpf(const char* path);
  void enablePromisc();

  xsk_umem*       m_umem{nullptr};
  xsk_socket*     m_xsk{nullptr};
  std::uint8_t*   m_umemBuffer{nullptr};
  xdp_program*    m_xdpProg{nullptr};
  int             m_promiscFd{-1};

  xsk_ring_prod   m_txRing{};
  xsk_ring_cons   m_rxRing{};
  xsk_ring_prod   m_fillRing{};
  xsk_ring_cons   m_compRing{};

  std::uint32_t   m_frameSize{0};
  std::uint32_t   m_frameCount{0};
  bool            m_needWakeup{false};
  bool            m_copyMode{false};
  bool            m_customBpf{false};
  std::uint32_t   m_bindFlags{0};

  int             m_ifindex{0};
};

#endif // ABTRDA3_AFXDPSOCKET_H
