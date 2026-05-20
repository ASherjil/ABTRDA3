//
// AFXDPSocket — xdpsock-style UMEM + socket + XDP initialisation.
//

#include "AFXDPSocket.hpp"

#include <bpf/bpf.h>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fmt/core.h>
#include <linux/if_link.h>
#include <net/if.h>
#include <netpacket/packet.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>
#include <utility>

// Busy-poll socket options (may be missing from older headers).
#ifndef SO_BUSY_POLL
#define SO_BUSY_POLL 46
#endif
#ifndef SO_PREFER_BUSY_POLL
#define SO_PREFER_BUSY_POLL 69
#endif
#ifndef SO_BUSY_POLL_BUDGET
#define SO_BUSY_POLL_BUDGET 70
#endif
#ifndef SOL_PACKET
#define SOL_PACKET 263
#endif

AFXDPSocket::AFXDPSocket(const XdpConfig& cfg)
  : m_frameSize{cfg.frameSize},
    m_frameCount{cfg.frameCount},
    m_needWakeup{cfg.needWakeup},
    m_ifindex{static_cast<int>(::if_nametoindex(cfg.interface))}
{
  if (m_ifindex == 0) {
    fmt::print(stderr, "[AFXDPSocket] bad interface: {}\n", cfg.interface);
    std::abort();
  }

  // ── 1. Clean old XDP programs ────────────────────────────────────────
  bpf_xdp_detach(m_ifindex, XDP_FLAGS_DRV_MODE, nullptr);
  bpf_xdp_detach(m_ifindex, XDP_FLAGS_SKB_MODE, nullptr);

  // ── 1.5. Load custom BPF if requested (xdpsock: load_xdp_program) ───
  if (cfg.bpfProgPath && cfg.bpfProgPath[0] != '\0')
    loadCustomBpf(cfg.bpfProgPath);

  // ── 2. Allocate UMEM buffer (xdpsock: mmap) ─────────────────────────
  std::size_t umemSize = static_cast<std::size_t>(m_frameCount) * m_frameSize;
  m_umemBuffer = static_cast<std::uint8_t*>(
    ::mmap(nullptr, umemSize,
           PROT_READ | PROT_WRITE,
           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
  if (m_umemBuffer == MAP_FAILED) {
    fmt::print(stderr, "[AFXDPSocket] mmap failed: {}\n", std::strerror(errno));
    std::abort();
  }

  // ── 3. Create UMEM (xdpsock: xsk_configure_umem) ────────────────────
  struct xsk_umem_config umem_cfg{};
  umem_cfg.fill_size     = XSK_RING_PROD__DEFAULT_NUM_DESCS * 2;  // 4096
  umem_cfg.comp_size     = XSK_RING_CONS__DEFAULT_NUM_DESCS;       // 2048
  umem_cfg.frame_size    = m_frameSize;
  umem_cfg.frame_headroom = XSK_UMEM__DEFAULT_FRAME_HEADROOM;

  int ret = xsk_umem__create(&m_umem, m_umemBuffer, umemSize,
                             &m_fillRing, &m_compRing, &umem_cfg);
  if (ret) {
    fmt::print(stderr, "[AFXDPSocket] xsk_umem__create failed: {}\n", std::strerror(-ret));
    std::abort();
  }

  // ── 4. Build bind flags (xdpsock: opt_xdp_bind_flags) ───────────────
  m_bindFlags = XDP_USE_NEED_WAKEUP;
  if (!m_needWakeup)
    m_bindFlags &= ~static_cast<std::uint32_t>(XDP_USE_NEED_WAKEUP);

  // ── 5. Create socket with fallback chain (xdpsock: xsk_configure_socket)
  struct xsk_socket_config sock_cfg{};
  sock_cfg.rx_size      = XSK_RING_CONS__DEFAULT_NUM_DESCS;
  sock_cfg.tx_size      = XSK_RING_PROD__DEFAULT_NUM_DESCS;
  // If we loaded a custom BPF, inhibit libxdp's built-in dispatcher.
  sock_cfg.libxdp_flags = m_customBpf ? XSK_LIBXDP_FLAGS__INHIBIT_PROG_LOAD : 0;

  const char* bindMode = "generic";

  for (auto [xdpFlags, zc, label] : {
         std::tuple{XDP_FLAGS_DRV_MODE, XDP_ZEROCOPY, "zero-copy"},
         std::tuple{XDP_FLAGS_DRV_MODE, XDP_COPY,    "native-copy"},
         std::tuple{XDP_FLAGS_SKB_MODE, XDP_COPY,     "generic"}
       }) {
    sock_cfg.xdp_flags  = xdpFlags;
    sock_cfg.bind_flags = static_cast<__u16>(m_bindFlags | zc);

    if (xsk_socket__create(&m_xsk, cfg.interface, cfg.queueId, m_umem,
                           &m_rxRing, &m_txRing, &sock_cfg) == 0) {
      bindMode = label;
      m_copyMode = (zc == XDP_COPY);
      if (zc == XDP_ZEROCOPY) m_bindFlags |= XDP_ZEROCOPY;
      else                    m_bindFlags |= XDP_COPY;
      break;
    }
  }

  if (!m_xsk) {
    fmt::print(stderr, "[AFXDPSocket] all socket modes failed\n");
    std::abort();
  }

  fmt::print(stderr, "[AFXDPSocket] {} | fd={} queue={} frames={}\n",
             bindMode, fd(), cfg.queueId, m_frameCount);

  // ── 5.5 Enable NAPI busy-polling (xdpsock --busy-poll) ──────────────
  // Drive RX *inline* from recvfrom() on the app core, in the app's own
  // timeslice. Without this, the RT setup (SCHED_FIFO app pinned to the NIC's
  // core with RT throttling disabled) starves the NAPI poll and AF_XDP RX
  // stalls. Requires net.core.busy_poll > 0 (NicTuner sets it).
  {
    int sfd = fd();
    int on = 1;
    if (::setsockopt(sfd, SOL_SOCKET, SO_PREFER_BUSY_POLL, &on, sizeof(on)) < 0)
      fmt::print(stderr, "[AFXDPSocket] WARN: SO_PREFER_BUSY_POLL: {}\n", std::strerror(errno));
    int usecs = 20;
    if (::setsockopt(sfd, SOL_SOCKET, SO_BUSY_POLL, &usecs, sizeof(usecs)) < 0)
      fmt::print(stderr, "[AFXDPSocket] WARN: SO_BUSY_POLL: {}\n", std::strerror(errno));
    int budget = static_cast<int>(BATCH_SIZE);
    if (::setsockopt(sfd, SOL_SOCKET, SO_BUSY_POLL_BUDGET, &budget, sizeof(budget)) < 0)
      fmt::print(stderr, "[AFXDPSocket] WARN: SO_BUSY_POLL_BUDGET: {}\n", std::strerror(errno));
  }

  // ── 6. If custom BPF loaded, insert socket fd into XSKMAP ────────────
  if (m_customBpf && m_xdpProg) {
    // xdpsock: enter_xsks_into_map
    int xsks_map_fd = -1;
    struct bpf_object* bpf_obj = xdp_program__bpf_obj(m_xdpProg);
    struct bpf_map* map = bpf_object__find_map_by_name(bpf_obj, "xsks_map");
    if (map) {
      xsks_map_fd = bpf_map__fd(map);
      int fd_val = xsk_socket__fd(m_xsk);
      int key = cfg.queueId;  // use queue id as key (matches rx_queue_index in BPF)
      if (bpf_map_update_elem(xsks_map_fd, &key, &fd_val, 0) != 0)
        fmt::print(stderr, "[AFXDPSocket] WARN: XSKMAP insert failed: {}\n",
                   std::strerror(errno));
      else
        fmt::print(stderr, "[AFXDPSocket] XSKMAP populated (fd={} key={})\n",
                   fd_val, key);
    } else {
      fmt::print(stderr, "[AFXDPSocket] WARN: xsks_map not found in BPF\n");
    }
  }

  // ── 7. Promiscuous mode ─────────────────────────────────────────────
  // Required for the TX-then-RX (client) pattern on i40e zero-copy: the first
  // sendto() on a freshly-bound XSK leaves the RX queue's unicast filter in a
  // state where frames to the port's OWN MAC are not delivered to the socket.
  // Promisc forces the VSI re-arm that fixes it. RX-first sockets don't need
  // it, but enabling it unconditionally is harmless (the BPF still only
  // redirects our ethertype; everything else is XDP_PASSed to the kernel).
  enablePromisc();
}

void AFXDPSocket::enablePromisc() {
  // A protocol-0 AF_PACKET socket buffers no packets, but lets us add a
  // refcounted PACKET_MR_PROMISC membership. The kernel auto-clears promisc
  // when this fd closes (even on crash) and won't clobber other promisc users.
  m_promiscFd = ::socket(AF_PACKET, SOCK_RAW, 0);
  if (m_promiscFd < 0) {
    fmt::print(stderr, "[AFXDPSocket] WARN: promisc socket: {}\n", std::strerror(errno));
    return;
  }
  struct packet_mreq mreq{};
  mreq.mr_ifindex = m_ifindex;
  mreq.mr_type    = PACKET_MR_PROMISC;
  if (::setsockopt(m_promiscFd, SOL_PACKET, PACKET_ADD_MEMBERSHIP,
                   &mreq, sizeof(mreq)) < 0) {
    fmt::print(stderr, "[AFXDPSocket] WARN: PACKET_MR_PROMISC: {}\n", std::strerror(errno));
    ::close(m_promiscFd);
    m_promiscFd = -1;
    return;
  }
  fmt::print(stderr, "[AFXDPSocket] promiscuous mode enabled (ifindex {})\n", m_ifindex);
}

void AFXDPSocket::loadCustomBpf(const char* path) {
  // xdpsock: load_xdp_program — note: xdp_program__open_file returns a
  // pointer that can be an error value; use libxdp_get_error to check.
  m_xdpProg = xdp_program__open_file(path, nullptr, nullptr);
  int err = libxdp_get_error(m_xdpProg);
  if (err) {
    fmt::print(stderr, "[AFXDPSocket] BPF load failed ({}): {}\n",
               path, std::strerror(-err));
    m_xdpProg = nullptr;
    return;
  }

  err = xdp_program__attach(m_xdpProg, m_ifindex, XDP_MODE_NATIVE, 0);
  if (err) {
    fmt::print(stderr, "[AFXDPSocket] BPF attach failed: {}\n",
               std::strerror(-err));
    xdp_program__close(m_xdpProg);
    m_xdpProg = nullptr;
    return;
  }

  m_customBpf = true;
  fmt::print(stderr, "[AFXDPSocket] custom BPF loaded + attached: {}\n", path);
}

void AFXDPSocket::populateFillRing() {
  // xdpsock: xsk_populate_fill_ring — stuff ALL frames into fill ring
  __u32 idx;
  int ret = xsk_ring_prod__reserve(&m_fillRing,
                                   XSK_RING_PROD__DEFAULT_NUM_DESCS * 2,
                                   &idx);
  if (ret != static_cast<int>(XSK_RING_PROD__DEFAULT_NUM_DESCS * 2)) {
    fmt::print(stderr, "[AFXDPSocket] fill ring reserve failed: {} != {}\n",
               ret, XSK_RING_PROD__DEFAULT_NUM_DESCS * 2);
    std::abort();
  }

  for (std::uint32_t i = 0; i < XSK_RING_PROD__DEFAULT_NUM_DESCS * 2; i++)
    *xsk_ring_prod__fill_addr(&m_fillRing, idx++) =
      static_cast<std::uint64_t>(i) * m_frameSize;

  xsk_ring_prod__submit(&m_fillRing, XSK_RING_PROD__DEFAULT_NUM_DESCS * 2);

  // Wake kernel to notice the fill ring
  if (xsk_ring_prod__needs_wakeup(&m_fillRing) || !m_needWakeup)
    ::recvfrom(fd(), nullptr, 0, MSG_DONTWAIT, nullptr, nullptr);
}

// ── Move constructor / assignment ───────────────────────────────────────

AFXDPSocket::AFXDPSocket(AFXDPSocket&& other) noexcept
  : m_umem{std::exchange(other.m_umem, nullptr)},
    m_xsk{std::exchange(other.m_xsk, nullptr)},
    m_umemBuffer{std::exchange(other.m_umemBuffer, nullptr)},
    m_xdpProg{std::exchange(other.m_xdpProg, nullptr)},
    m_promiscFd{std::exchange(other.m_promiscFd, -1)},
    m_txRing{other.m_txRing},
    m_rxRing{other.m_rxRing},
    m_fillRing{other.m_fillRing},
    m_compRing{other.m_compRing},
    m_frameSize{other.m_frameSize},
    m_frameCount{other.m_frameCount},
    m_needWakeup{other.m_needWakeup},
    m_copyMode{other.m_copyMode},
    m_customBpf{other.m_customBpf},
    m_bindFlags{other.m_bindFlags},
    m_ifindex{other.m_ifindex}
{}

AFXDPSocket& AFXDPSocket::operator=(AFXDPSocket&& other) noexcept {
  if (this != &other) {
    cleanup();
    m_umem        = std::exchange(other.m_umem, nullptr);
    m_xsk         = std::exchange(other.m_xsk, nullptr);
    m_umemBuffer  = std::exchange(other.m_umemBuffer, nullptr);
    m_xdpProg     = std::exchange(other.m_xdpProg, nullptr);
    m_promiscFd   = std::exchange(other.m_promiscFd, -1);
    m_txRing      = other.m_txRing;
    m_rxRing      = other.m_rxRing;
    m_fillRing    = other.m_fillRing;
    m_compRing    = other.m_compRing;
    m_frameSize   = other.m_frameSize;
    m_frameCount  = other.m_frameCount;
    m_needWakeup  = other.m_needWakeup;
    m_copyMode    = other.m_copyMode;
    m_customBpf   = other.m_customBpf;
    m_bindFlags   = other.m_bindFlags;
    m_ifindex     = other.m_ifindex;
  }
  return *this;
}

// ── Destructor / cleanup ────────────────────────────────────────────────

AFXDPSocket::~AFXDPSocket() { cleanup(); }

void AFXDPSocket::cleanup() noexcept {
  if (m_promiscFd >= 0) {
    ::close(m_promiscFd);  // drops the PACKET_MR_PROMISC membership
    m_promiscFd = -1;
  }
  if (m_xsk) {
    xsk_socket__delete(m_xsk);
    m_xsk = nullptr;
  }
  if (m_umem) {
    (void)xsk_umem__delete(m_umem);
    m_umem = nullptr;
  }
  if (m_umemBuffer && m_umemBuffer != MAP_FAILED) {
    ::munmap(m_umemBuffer,
             static_cast<std::size_t>(m_frameCount) * m_frameSize);
    m_umemBuffer = nullptr;
  }
  if (m_xdpProg) {
    xdp_program__detach(m_xdpProg, m_ifindex, XDP_MODE_NATIVE, 0);
    xdp_program__close(m_xdpProg);
    m_xdpProg = nullptr;
  }
  if (m_ifindex > 0) {
    bpf_xdp_detach(m_ifindex, XDP_FLAGS_DRV_MODE, nullptr);
    bpf_xdp_detach(m_ifindex, XDP_FLAGS_SKB_MODE, nullptr);
  }
}
