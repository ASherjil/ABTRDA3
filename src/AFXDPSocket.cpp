//
// Created by asherjil on 3/3/26.
//

#include "AFXDPSocket.hpp"
#include "xdp_filter_embed.h"   // generated Phase 2: xdp_filter_bpf[], xdp_filter_bpf_len

#include <bpf/libbpf.h>        // bpf_object__open_mem, bpf_object__load, etc.
#include <bpf/bpf.h>           // bpf_map_update_elem (low-level syscall wrapper)
#include <cstring>
#include <linux/if_link.h>  // XDP_FLAGS_DRV_MODE, XDP_FLAGS_SKB_MODE
#include <net/if.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <system_error>
#include <unistd.h>
#include <utility>

AFXDPSocket::AFXDPSocket(const XdpConfig& cfg)
  :m_frameSize{cfg.frameSize}, m_txFrameCount{cfg.frameCount / 2}, m_rxFrameCount{cfg.frameCount - m_txFrameCount}, m_needWakeup {cfg.needWakeup} {

  // 1. Create the AF_XDP socket
  m_fd = ::socket(AF_XDP, SOCK_RAW, 0);
  if (m_fd < 0) {
    throw std::system_error(errno, std::generic_category(), "socket(AF_XDP)");
  }

  // 2. Allocate the UMEM
  // MAP_ANONYMOUS -> not backed by a file(pure RAM)
  // MAP_POPULATE -> pre-fault all pages(avoid page faults on hot paths)
  // MAP_PRIVATE -> our copy no other process uses this
  m_umemSize = static_cast<std::size_t>(cfg.frameCount) * cfg.frameSize;
  m_umem = static_cast<std::uint8_t*>(
    ::mmap(
      nullptr,
      m_umemSize,
      PROT_READ | PROT_WRITE,
      MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE,
      -1,
      0)
    );
  if (m_umem == MAP_FAILED) {
    m_umem = nullptr;
    throw std::system_error(errno, std::generic_category(), "mmap UMEM");
  }

  // 3. Register the UMEM with the socket
  xdp_umem_reg umem_reg{};
  umem_reg.addr = reinterpret_cast<std::uint64_t>(m_umem);
  umem_reg.len = m_umemSize;
  umem_reg.chunk_size = cfg.frameSize;
  umem_reg.headroom = 0;
  umem_reg.flags = 0;

  if (::setsockopt(m_fd, SOL_XDP, XDP_UMEM_REG, &umem_reg, sizeof(umem_reg)) < 0) {
    throw std::system_error(errno, std::generic_category(), "XDP_UMEM_REG");
  }

  // 4. Set the ring sizes
  // Fill ring: rxFrameCount
  // Completion ring: txFrameCount
  // TX ring: txFrameCount
  // RX ring: rxFrameCount
  std::uint32_t fillSize = m_rxFrameCount;
  std::uint32_t compSize = m_txFrameCount;
  std::uint32_t txSize   = m_txFrameCount;
  std::uint32_t rxSize   = m_rxFrameCount;

  if (::setsockopt(m_fd, SOL_XDP, XDP_UMEM_FILL_RING,       &fillSize, sizeof(fillSize)) < 0)
    throw std::system_error(errno, std::generic_category(), "XDP_UMEM_FILL_RING");
  if (::setsockopt(m_fd, SOL_XDP, XDP_UMEM_COMPLETION_RING, &compSize, sizeof(compSize)) < 0)
    throw std::system_error(errno, std::generic_category(), "XDP_UMEM_COMPLETION_RING");
  if (::setsockopt(m_fd, SOL_XDP, XDP_TX_RING,              &txSize,   sizeof(txSize)) < 0)
    throw std::system_error(errno, std::generic_category(), "XDP_TX_RING");
  if (::setsockopt(m_fd, SOL_XDP, XDP_RX_RING,              &rxSize,   sizeof(rxSize)) < 0)
    throw std::system_error(errno, std::generic_category(), "XDP_RX_RING");

  // 5. Get ring memory layout and mmap each ring
  // The mmap page offsets are magic constants defined in if_xdp.h:
  //   XDP_UMEM_PGOFF_FILL_RING       = 0x100000000
  //   XDP_UMEM_PGOFF_COMPLETION_RING = 0x180000000
  //   XDP_PGOFF_TX_RING              = 0x80000000
  //   XDP_PGOFF_RX_RING              = 0x00000000
  xdp_mmap_offsets off{};
  socklen_t optlen = sizeof(off);
  if (::getsockopt(m_fd, SOL_XDP, XDP_MMAP_OFFSETS, &off, &optlen) < 0) {
    throw std::system_error(errno, std::generic_category(), "XDP_MMAP_OFFSETS");
  }

  // Fill ring (user → kernel: "here are empty buffers for RX")
  m_ringMaps[0].len = off.fr.desc + fillSize * sizeof(std::uint64_t);
  m_ringMaps[0].ptr = mapRing(m_fd, m_ringMaps[0].len, XDP_UMEM_PGOFF_FILL_RING);
  wireAddrRing(m_ringMaps[0].ptr, off.fr, fillSize, fillRing);

  // Completion ring (kernel → user: "these TX buffers are done, you can reuse them")
  m_ringMaps[1].len = off.cr.desc + compSize * sizeof(std::uint64_t);
  m_ringMaps[1].ptr = mapRing(m_fd, m_ringMaps[1].len, XDP_UMEM_PGOFF_COMPLETION_RING);
  wireAddrRing(m_ringMaps[1].ptr, off.cr, compSize, compRing);

  // TX ring (user → kernel: "send this frame")
  m_ringMaps[2].len = off.tx.desc + txSize * sizeof(xdp_desc);
  m_ringMaps[2].ptr = mapRing(m_fd, m_ringMaps[2].len, XDP_PGOFF_TX_RING);
  wireDescRing(m_ringMaps[2].ptr, off.tx, txSize, txRing);

  // RX ring (kernel → user: "a frame arrived")
  m_ringMaps[3].len = off.rx.desc + rxSize * sizeof(xdp_desc);
  m_ringMaps[3].ptr = mapRing(m_fd, m_ringMaps[3].len, XDP_PGOFF_RX_RING);
  wireDescRing(m_ringMaps[3].ptr, off.rx, rxSize, rxRing);

  // 6. Load the BPF program from embedded bytes
  LIBBPF_OPTS(bpf_object_open_opts, open_opts, .object_name = "xdp");
  m_bpfObj = bpf_object__open_mem(xdp_filter_bpf, xdp_filter_bpf_len, &open_opts);
  if (!m_bpfObj) {
    throw std::runtime_error("bpf_object__open_mem failed");
  }

  // 7. Rewrite target_ethertype before loading
  struct bpf_map* rodata = bpf_object__find_map_by_name(m_bpfObj, "xdp.rodata");
  if (!rodata) {
      // Fallback: search all maps for .rodata (name may vary by libbpf version)
      struct bpf_map *m;
      bpf_object__for_each_map(m, m_bpfObj) {
          if (std::strstr(bpf_map__name(m), ".rodata")) {
              rodata = m;
              break;
          }
      }
  }
  if (rodata) {
      std::size_t sz = 0;
      void* init = bpf_map__initial_value(rodata, &sz);
      if (init && sz >= sizeof(std::uint16_t)) {
          // .rodata layout: offset 0 = __u16 target_ethertype
          // (only one global, so offset is always 0)
          std::memcpy(init, &cfg.etherType, sizeof(cfg.etherType));
      }
  }

  // 8. Load the BPF program into the kernel
  if (bpf_object__load(m_bpfObj) < 0 ) {
    throw std::runtime_error("bpf_object__load failed (verifier rejected?)");
  }

  // Get the program fd (handle to the loaded BPF in kernel)
  struct bpf_program *prog = bpf_object__find_program_by_name(m_bpfObj, "xdp_filter_ethertype");
  if (!prog)
    throw std::runtime_error("BPF program 'xdp_filter_ethertype' not found");
  int prog_fd = bpf_program__fd(prog);

  // Get the xsks_map fd (the XSKMAP our XDP program uses for bpf_redirect_map)
  struct bpf_map *xsks = bpf_object__find_map_by_name(m_bpfObj, "xsks_map");
  if (!xsks)
    throw std::runtime_error("BPF map 'xsks_map' not found");
  int xsks_map_fd = bpf_map__fd(xsks);


  // 9. Attach the XDP program to the network interface
  //
  // Auto-detect the best mode by trying from best to worst:
  //   1. Native/DRV mode — BPF runs in driver NAPI handler (fastest)
  //   2. Generic/SKB mode — BPF runs in kernel net stack (universal fallback)
  m_ifindex = static_cast<int>(::if_nametoindex(cfg.interface));
  if (m_ifindex == 0)
    throw std::system_error(errno, std::generic_category(), "if_nametoindex");

  auto tryAttach = [&](std::uint32_t flags) -> int {
    int e = bpf_xdp_attach(m_ifindex, prog_fd, flags, nullptr);
    if (e == -EEXIST) {
      bpf_xdp_detach(m_ifindex, flags, nullptr);
      e = bpf_xdp_attach(m_ifindex, prog_fd, flags, nullptr);
    }
    return e;
  };

  // Try native XDP first, fall back to generic/SKB
  m_xdpFlags = XDP_FLAGS_DRV_MODE;
  if (tryAttach(m_xdpFlags) != 0) {
    m_xdpFlags = XDP_FLAGS_SKB_MODE;
    if (tryAttach(m_xdpFlags) != 0)
      throw std::runtime_error("bpf_xdp_attach failed in both native and generic mode");
  }
  m_xdpAttached = true;
  bool nativeXdp = (m_xdpFlags == XDP_FLAGS_DRV_MODE);

  // In generic/SKB mode, always bind to queue 0 — the kernel ignores
  // hardware queue assignments and only delivers to queue-0-bound sockets.
  // ntuple steering still provides interrupt isolation at the hardware level.
  std::uint32_t bindQueue = nativeXdp ? cfg.queueId : 0;

  // 10. Register our socket in XSKMAP at the queue we will bind to.
  {
    int val = m_fd;
    std::uint32_t key = bindQueue;
    if (bpf_map_update_elem(xsks_map_fd, &key, &val, 0) < 0)
      throw std::system_error(errno, std::generic_category(), "bpf_map_update_elem(xsks_map)");
  }

  // 11. Bind socket to interface & queue
  //
  // Auto-detect bind mode (best to worst):
  //   1. XDP_ZEROCOPY — zero-copy DMA (requires driver ndo_xsk_wakeup, native XDP only)
  //   2. XDP_COPY     — kernel copies to/from UMEM (universal)

  sockaddr_xdp sxdp{};
  sxdp.sxdp_family   = AF_XDP;
  sxdp.sxdp_ifindex  = static_cast<std::uint32_t>(m_ifindex);
  sxdp.sxdp_queue_id = bindQueue;

  const char* bindMode = "copy";
  bool bound = false;

  // Only try zero-copy if we got native XDP (zero-copy requires DRV mode)
  if (nativeXdp) {
    sxdp.sxdp_flags = XDP_ZEROCOPY;
    if (cfg.needWakeup) sxdp.sxdp_flags |= XDP_USE_NEED_WAKEUP;
    if (::bind(m_fd, reinterpret_cast<sockaddr*>(&sxdp), sizeof(sxdp)) == 0) {
      bindMode = "zero-copy";
      bound = true;
    }
  }

  // Fall back to copy mode
  if (!bound) {
    sxdp.sxdp_flags = XDP_COPY;
    if (cfg.needWakeup) sxdp.sxdp_flags |= XDP_USE_NEED_WAKEUP;
    if (::bind(m_fd, reinterpret_cast<sockaddr*>(&sxdp), sizeof(sxdp)) < 0)
      throw std::system_error(errno, std::generic_category(), "bind(AF_XDP)");
  }

  std::fprintf(stderr, "[AFXDPSocket] %s XDP, %s | fd=%d queue=%u frames=%u+%u\n",
               nativeXdp ? "native" : "generic",
               bindMode,
               m_fd, bindQueue, m_txFrameCount, m_rxFrameCount);
}

AFXDPSocket::AFXDPSocket(AFXDPSocket&& other) noexcept
      : txRing{other.txRing}, compRing{other.compRing},
        rxRing{other.rxRing}, fillRing{other.fillRing},
        m_fd{std::exchange(other.m_fd, -1)},
        m_umem{std::exchange(other.m_umem, nullptr)},
        m_umemSize{std::exchange(other.m_umemSize, 0)},
        m_frameSize{std::exchange(other.m_frameSize, 0)},
        m_txFrameCount{std::exchange(other.m_txFrameCount, 0)},
        m_rxFrameCount{std::exchange(other.m_rxFrameCount, 0)},
        m_needWakeup{other.m_needWakeup},
        m_bpfObj{std::exchange(other.m_bpfObj, nullptr)},
        m_ifindex{std::exchange(other.m_ifindex, 0)},
        m_xdpFlags{other.m_xdpFlags},
        m_xdpAttached{std::exchange(other.m_xdpAttached, false)} {

  std::copy(std::begin(other.m_ringMaps), std::end(other.m_ringMaps), m_ringMaps);
  for (auto& rm : other.m_ringMaps) {
    rm = {};
  }
}

AFXDPSocket& AFXDPSocket::operator=(AFXDPSocket&& other) noexcept {
  if (this != &other) {
    cleanup();

    txRing   = other.txRing;   compRing = other.compRing;
    rxRing   = other.rxRing;   fillRing = other.fillRing;

    m_fd            = std::exchange(other.m_fd, -1);
    m_umem          = std::exchange(other.m_umem, nullptr);
    m_umemSize      = std::exchange(other.m_umemSize, 0);
    m_frameSize     = std::exchange(other.m_frameSize, 0);
    m_txFrameCount  = std::exchange(other.m_txFrameCount, 0);
    m_rxFrameCount  = std::exchange(other.m_rxFrameCount, 0);
    m_needWakeup    = other.m_needWakeup;
    m_bpfObj        = std::exchange(other.m_bpfObj, nullptr);
    m_ifindex       = std::exchange(other.m_ifindex, 0);
    m_xdpFlags      = other.m_xdpFlags;
    m_xdpAttached   = std::exchange(other.m_xdpAttached, false);

    std::copy(std::begin(other.m_ringMaps), std::end(other.m_ringMaps), m_ringMaps);
    for (auto& rm : other.m_ringMaps) {
      rm = {};
    }
  }
  return *this;
}

AFXDPSocket::~AFXDPSocket() {
  cleanup();
}

void AFXDPSocket::detachXdp() noexcept {
  if (m_xdpAttached && m_ifindex > 0) {
    bpf_xdp_detach(m_ifindex, m_xdpFlags, nullptr);
    m_xdpAttached = false;
  }
}

void AFXDPSocket::cleanup() noexcept {
  if (m_xdpAttached && m_ifindex > 0)
    bpf_xdp_detach(m_ifindex, m_xdpFlags, nullptr);

  if (m_bpfObj)
    bpf_object__close(m_bpfObj);

  for (auto& rm : m_ringMaps) {
    if (rm.ptr && rm.ptr != MAP_FAILED)
      ::munmap(rm.ptr, rm.len);
  }

  if (m_umem && m_umem != MAP_FAILED)
    ::munmap(m_umem, m_umemSize);

  if (m_fd >= 0)
    ::close(m_fd);
}


void* AFXDPSocket::mapRing(int fd, std::size_t size, off_t pgoff) {
    void* ptr = ::mmap(nullptr, size,
                       PROT_READ | PROT_WRITE,
                       MAP_SHARED | MAP_POPULATE,
                       fd, pgoff);
    if (ptr == MAP_FAILED) {
      throw std::system_error(errno, std::generic_category(), "mmap ring");
    }
    return ptr;
}

// Wire a XdpAddrRing (Fill / Completion) from mmap base + kernel offsets
void AFXDPSocket::wireAddrRing(void* base, const xdp_ring_offset& off, std::uint32_t size, XdpAddrRing& ring) {
    auto* b = static_cast<std::uint8_t*>(base);
    ring.producer = reinterpret_cast<std::uint32_t*>(b + off.producer);
    ring.consumer = reinterpret_cast<std::uint32_t*>(b + off.consumer);
    ring.descs    = reinterpret_cast<std::uint64_t*>(b + off.desc);
    ring.flags    = reinterpret_cast<std::uint32_t*>(b + off.flags);
    ring.mask     = size - 1;
    ring.size     = size;
}

// Wire a XdpDescRing (TX / RX) from mmap base + kernel offsets
void AFXDPSocket::wireDescRing(void* base, const xdp_ring_offset& off, std::uint32_t size, XdpDescRing& ring) {
    auto* b = static_cast<std::uint8_t*>(base);
    ring.producer = reinterpret_cast<std::uint32_t*>(b + off.producer);
    ring.consumer = reinterpret_cast<std::uint32_t*>(b + off.consumer);
    ring.descs    = reinterpret_cast<xdp_desc*>(b + off.desc);
    ring.flags    = reinterpret_cast<std::uint32_t*>(b + off.flags);
    ring.mask     = size - 1;
    ring.size     = size;
}


int AFXDPSocket::fd() const noexcept {
  return m_fd;
}

std::uint8_t*  AFXDPSocket::umemArea() const noexcept {
  return m_umem;
}

std::uint32_t  AFXDPSocket::frameSize() const noexcept {
  return m_frameSize;
}

std::uint32_t  AFXDPSocket::txFrames() const noexcept {
  return m_txFrameCount;
}

std::uint32_t  AFXDPSocket::rxFrames() const noexcept {
  return m_rxFrameCount;
}

bool AFXDPSocket::needWakeup() const noexcept {
  return m_needWakeup;
}