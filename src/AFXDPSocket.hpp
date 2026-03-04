//
// Created by asherjil on 3/3/26.
//

#ifndef ABTRDA3_AFXDPSOCKET_H
#define ABTRDA3_AFXDPSOCKET_H

#include <cstddef>
#include <cstdint>
#include <sys/types.h>     // off_t
#include <linux/if_xdp.h>  // xdp_desc, sockaddr_xdp, XDP_* constants

struct bpf_object; // libbpf forward declaration (definition only needed in .cpp)

// ── Configuration ────────────────────────────────────────────────────────
struct XdpConfig {
  const char*    interface;
  std::uint32_t  queueId     = 0;
  std::uint32_t  frameSize   = 4096;   // UMEM frame size (must be 2048 or 4096)
  std::uint32_t  frameCount  = 64;     // total UMEM frames (split half TX, half RX)
  std::uint16_t  etherType   = 0x88B5;
  bool           zeroCopy    = false;
  bool           needWakeup  = true;   // skip sendto()/recvfrom() when kernel is actively polling
};

// ── Ring pointer bundles ─────────────────────────────────────────────────
//
// Each AF_XDP ring is a shared-memory region with:
//   producer  — index written by the producing side (atomic)
//   consumer  — index written by the consuming side (atomic)
//   descs[]   — ring buffer of descriptors (power-of-2, indexed by idx & mask)
//   flags     — kernel sets XDP_RING_NEED_WAKEUP here when it goes idle

// Fill + Completion rings: descriptors are uint64_t UMEM frame addresses
struct XdpAddrRing {
  std::uint32_t* producer{};
  std::uint32_t* consumer{};
  std::uint64_t* descs{};
  std::uint32_t* flags{};
  std::uint32_t  mask{};       // size - 1 (for power-of-2 indexing)
  std::uint32_t  size{};
};

// TX + RX rings: descriptors are xdp_desc { addr, len, options }
struct XdpDescRing {
  std::uint32_t* producer{};
  std::uint32_t* consumer{};
  xdp_desc*      descs{};
  std::uint32_t* flags{};
  std::uint32_t  mask{};
  std::uint32_t  size{};
};

class AFXDPSocket {
public:
  explicit AFXDPSocket(const XdpConfig& cfg);

  AFXDPSocket(const AFXDPSocket&) = delete;
  AFXDPSocket& operator=(const AFXDPSocket&) = delete;
  AFXDPSocket(AFXDPSocket&& other) noexcept;
  AFXDPSocket& operator=(AFXDPSocket&& other) noexcept;
  ~AFXDPSocket();

  // ── Ring state (wired during construction, used by AfXdpTx / AfXdpRx) ──
  XdpDescRing  txRing{};       // user → kernel (TX submit)
  XdpAddrRing  compRing{};     // kernel → user (TX completion)
  XdpDescRing  rxRing{};       // kernel → user (RX delivery)
  XdpAddrRing  fillRing{};     // user → kernel (RX buffer refill)

  // ── Accessors ─────────────────────────────────────────────────────────
  int            fd() const noexcept;
  std::uint8_t*  umemArea() const noexcept;
  std::uint32_t  frameSize() const noexcept;
  std::uint32_t  txFrames() const noexcept;
  std::uint32_t  rxFrames() const noexcept;
  bool           needWakeup() const noexcept;

  // Helper functions - static
  static void* mapRing(int fd, std::size_t size, off_t pgoff);
  static void  wireAddrRing(void* base, const xdp_ring_offset& off, std::uint32_t size, XdpAddrRing& ring);
  static void  wireDescRing(void* base, const xdp_ring_offset& off, std::uint32_t size, XdpDescRing& ring);
private:
  void cleanup() noexcept;    // shared teardown for destructor + move-assign
  // Socket + UMEM
  int            m_fd{-1};
  std::uint8_t*  m_umem{nullptr};
  std::size_t    m_umemSize{0};
  std::uint32_t  m_frameSize{0};
  std::uint32_t  m_txFrameCount{0};
  std::uint32_t  m_rxFrameCount{0};
  bool           m_needWakeup{false};

  // BPF program lifecycle
  bpf_object*    m_bpfObj{nullptr};
  int            m_ifindex{0};
  std::uint32_t  m_xdpFlags{0};    // DRV_MODE or SKB_MODE (for detach)
  bool           m_xdpAttached{false};

  // Ring mmap tracking (for cleanup)
  struct MmapRegion {
    void* ptr{nullptr};
    std::size_t len{0};
  };

  MmapRegion     m_ringMaps[4]{};  // [0]=fill, [1]=comp, [2]=tx, [3]=rx
};

#endif // ABTRDA3_AFXDPSOCKET_H
