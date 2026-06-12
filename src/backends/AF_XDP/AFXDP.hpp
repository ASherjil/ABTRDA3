#ifndef ABTRDA3_AFXDP_HPP
#define ABTRDA3_AFXDP_HPP

#include "../common/RxFrame.hpp"
#include "../common/NapiConfig.hpp"

#include <bpf/bpf.h>
#include <bpf/libbpf.h>
#include <xdp/xsk.h>
#include <xdp/libxdp.h>

#include <fmt/core.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <span>
#include <string>
#include <tuple>
#include <utility>

#include <fcntl.h>
#include <linux/ethtool.h>
#include <linux/if_link.h>
#include <linux/if_xdp.h>
#include <linux/sockios.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <unistd.h>

#ifndef SO_BUSY_POLL
#define SO_BUSY_POLL 46
#endif
#ifndef SO_PREFER_BUSY_POLL
#define SO_PREFER_BUSY_POLL 69
#endif
#ifndef SO_BUSY_POLL_BUDGET
#define SO_BUSY_POLL_BUDGET 70
#endif
#ifndef SO_INCOMING_NAPI_ID
#define SO_INCOMING_NAPI_ID 56
#endif
#ifndef SOL_XDP
#define SOL_XDP 283
#endif

// =============================================================================
// AFXDP — unified xdpsock-derived AF_XDP transport.
//
// One class template owns the whole stack: UMEM mmap, FILL/COMP/RX/TX rings,
// the XDP program, the socket, AND the hot-path TX/RX. Cheap constructor stores
// config; init() does the cold bring-up and returns bool; shutdown() (dtor and
// move-assign) tears down. Satisfies the TxRing/RxRing concepts (tryReceive/
// release + acquire/commit/send/prefillRing). The ring/UMEM state lives in the
// SAME two structs xdpsock.c uses (xsk_umem_info / xsk_socket_info) so the hot
// path reads 1:1 with the example. Everything latency-relevant is a
// compile-time constant: FrameSize -> shift, TX-pool wrap -> mask (no
// division), NeedWakeup -> kickTx() branches compiled out.
//
// UMEM PARTITION: TX owns frames [0 .. NumTxFrames), RX/FILL owns the rest.
// The pools are DISJOINT — our API copies into a dedicated TX frame (unlike
// xdpsock l2fwd's in-place reflect), so a TX frame is never an RX/FILL frame.
// Completed TX frames return implicitly as m_txFrameNb wraps (xdpsock txonly
// model); reapTx() must NOT donate them to FILL (that would merge the pools).
// prefillRing() stamps the TX pool only — RX frames are kernel-owned (in FILL)
// and touching them would race inbound DMA. FILL carries xdpsock's 2x headroom.
//
// INIT ORDER (each step earned the hard way):
//   0. RLIMIT_MEMLOCK -> infinity (xdpsock main()'s first step; UMEM pins pages
//      against it). Best-effort on modern memcg-accounted kernels.
//   1. Detach stale XDP programs from previous runs.
//   1.5 steerAllTrafficToQueue() — collapse the NIC to ONE combined channel so
//      all RX lands on queue 0 (== `ethtool -L <if> combined 1`). MUST run
//      BEFORE the XDP attach and the socket bind: changing the channel count is
//      a NIC queue reprogram (PF reset on i40e, link down ~2.5-3s) that would
//      tear down a live ZC bind, re-arm the HW RX ring from a drained FILL, and
//      can drop the attached program. If already combined=1 it is NOT re-set
//      (re-applying an unchanged value still bounces the link). After a real
//      change, waitForCarrier() blocks until the link returns.
//   2. Load + attach our redirect-all BPF (af_xdp_kern.o), RX modes only — a
//      TxOnly socket is not in the XSKMAP and a redirect prog on a TX-only port
//      would steal inbound frames into a dead map.
//   3-4. UMEM mmap + xsk_umem__create (FILL/COMP rings).
//   4.5 Pre-load FILL BEFORE the bind (CRITICAL on i40e ZC): the driver arms
//      its HW RX ring from FILL at bind time (i40e_alloc_rx_buffers_zc); an
//      empty FILL leaves the HW ring without DMA targets — silent drops +
//      "Failed to allocate some buffers on AF_XDP ZC" in dmesg.
//   5-6. Bind with the xdpsock fallback chain (ZC -> native-copy -> generic),
//      XDP_USE_NEED_WAKEUP per the NeedWakeup template param. The bind label
//      only reflects accepted flags — the AUTHORITATIVE zero-copy check is
//      getsockopt(XDP_OPTIONS) afterwards (kernel reports what it granted).
//   7. SO_PREFER_BUSY_POLL + SO_BUSY_POLL + budget, RX modes ONLY. Busy-poll is
//      an RX mechanism (recvfrom runs the NAPI poll INLINE instead of the
//      ksoftirqd path — ~17us vs ~96us median on igc). On a TX-only socket it
//      is meaningless and on igc actively harmful: it makes
//      igc_xsk_wakeup(XDP_WAKEUP_TX) a NOP when the shared queue-pair NAPI is
//      "already running", so TX stalls after a few packets. RxTx keeps it
//      (low-latency choice) with the always-kick mitigation below.
//   7.5 Per-NAPI knobs via netdev-genl (SO_INCOMING_NAPI_ID is valid after
//      bind). CRITICAL kernel fact (net/core/dev.c busy_poll_stop): NAPI
//      deferral is only RE-ARMED if defer_hard_irqs AND gro_flush_timeout are
//      BOTH nonzero — if both are 0, the hardware IRQ fires after each
//      busy-poll and NAPI falls back to the softirq/IRQ path (measured: 79us
//      on igc). Two regimes:
//        * CLASSIC BUSY-POLL (default, USE_IRQ_SUSPEND=false): small defer +
//          small gro-flush, irq-suspend=0. Keeps busy-poll owning the NAPI
//          WITHOUT the kernel-6.13 irq-suspend hrtimer/APIC machinery that
//          perf showed wastes ~12% of the hot loop (native_write_msr,
//          lapic_next_deadline, hrtimer_start) on this always-hot
//          one-in-flight workload.
//        * IRQ-SUSPEND (USE_IRQ_SUSPEND=true, kernel 6.13+): adds
//          irq-suspend-timeout — good for bursty/idle-cycling server loads,
//          not a saturated ping-pong.
//   8. Insert the socket fd into the XSKMAP (key = queue id, matching
//      rx_queue_index), RX modes with our custom BPF only.
//   EVERY applied mode is READ BACK like a register write (XDP_OPTIONS,
//   bpf_xdp_query, XDP_MMAP_OFFSETS, getsockopt busy-poll trio, NAPI_GET) —
//   the bind ladder keeps the transport generic, the readback prints what the
//   kernel ACTUALLY granted ("kernel confirms:" lines; grep before a soak).
//
// RX HOT PATH: peek EXACTLY one descriptor (peeking more than release() frees
// would desync cached_cons and silently drop frames 2..N). On an empty peek
// with NeedWakeup, recvfrom() drives the NAPI poll inline and then RE-PEEKS so
// a frame the poll just delivered is returned in THIS call — A/B-proven tail
// win on igc (skipping the re-peek worsened P99.99 25->48us, P99.999 40->94us,
// Max ~4x). release() returns the buffer to FILL BEFORE releasing the RX slot
// (xdpsock rx_drop order); on the rare FILL-full it kicks and retries, and
// always frees the RX slot (never leaks it).
//
// TX KICK POLICY (kickTx — one sendto -> ndo_xsk_wakeup(TX)):
//   * RxTx: kick UNCONDITIONALLY. After a busy-poll recvfrom() the TX
//     needs_wakeup flag reads CLEAR, so a gated kick is skipped and the
//     just-submitted descriptor is NEVER transmitted (the recvfrom-driven NAPI
//     services RX but does not pull the ZC TX ring on igc/i40e). Proven by
//     cross-transport A/B: AF_XDP client/server TX delivered 0 packets while a
//     packet_mmap peer delivered fine.
//   * TxOnly on igc/i40e: needs_wakeup-GATED kick (xdpsock-faithful) — verified
//     clean on igc (separate Tx/Rx ports = separate NAPIs; same latency, fewer
//     syscalls).
//   * TxOnly on mlx5 (m_txKickAlways, set by an init() driver sniff): kick
//     UNCONDITIONALLY. mlx5's flag dynamics differ: ~100 rounds in (when its
//     NAPI busy window first expires) the gated kick races the NAPI-idle
//     transition — the submit lands after the driver's final ring check but
//     the flag read still said "no kick needed" -> that descriptor is never
//     transmitted -> permanent hang. Proven 2026-06-10 on ConnectX-4 Lx: two
//     runs froze at ~85/~103 rounds with tx_xsk_xmit == tx_xsk_cqes ==
//     app-posted-minus-one; always-kick fixed it outright.
//   XDP_USE_NEED_WAKEUP stays set at bind regardless (required on i40e ZC —
//   NeedWakeup=false hangs the kernel there); the policy chooses when to USE
//   the hint, not whether it exists.
//
// TEARDOWN: shutdown() first dumps the kernel's cumulative XDP_STATISTICS —
// rx_fill_empty > 0 means the driver ran out of FILL buffers (the i40e ZC
// "Failed to allocate" symptom); rx_ring_full means userspace didn't drain;
// tx_ring_empty means a kick found nothing queued. Then socket -> umem ->
// munmap -> program detach.
//
// MOVE SEMANTICS: the ring structs hold pointers into the transferred mmap'd
// ring memory, so copying them by value keeps them valid; the source's owning
// handles are nulled so its shutdown() is a no-op.
// =============================================================================

enum class AFXDPMode : std::uint8_t { RxOnly, TxOnly, RxTx };

struct xsk_umem_info {
  xsk_ring_prod fq{};
  xsk_ring_cons cq{};
  xsk_umem*     umem{nullptr};
  std::uint8_t* buffer{nullptr};
};

struct xsk_socket_info {
  xsk_ring_cons rx{};
  xsk_ring_prod tx{};
  xsk_socket*   xsk{nullptr};
  std::uint32_t outstanding_tx{0};
};

template<AFXDPMode     M,
         std::uint32_t NumRxFrames = 2048,
         std::uint32_t NumTxFrames = 2048,
         std::uint32_t FrameSize   = 4096,
         bool          NeedWakeup  = true>
class AFXDP {
  static_assert(M == AFXDPMode::TxOnly ||
                (NumRxFrames >= 8 && (NumRxFrames & (NumRxFrames - 1)) == 0),
                "NumRxFrames must be a power of 2 and >= 8");
  static_assert(M == AFXDPMode::RxOnly ||
                (NumTxFrames >= 8 && (NumTxFrames & (NumTxFrames - 1)) == 0),
                "NumTxFrames must be a power of 2 and >= 8");
  static_assert(FrameSize == 2048 || FrameSize == 4096,
                "FrameSize must be 2048 or 4096 (XSK aligned-mode chunk)");

  static constexpr bool HAS_RX = (M != AFXDPMode::TxOnly);
  static constexpr bool HAS_TX = (M != AFXDPMode::RxOnly);

  static constexpr std::uint32_t TX_POOL_FRAMES = HAS_TX ? NumTxFrames : 0;
  static constexpr std::uint32_t RX_POOL_FRAMES = HAS_RX ? NumRxFrames : 0;
  static constexpr std::uint32_t NUM_FRAMES     = TX_POOL_FRAMES + RX_POOL_FRAMES;

  static_assert(!HAS_TX || (TX_POOL_FRAMES & (TX_POOL_FRAMES - 1)) == 0,
                "TX_POOL_FRAMES must be a power of 2");
  static constexpr std::uint32_t TX_POOL_MASK = TX_POOL_FRAMES - 1;

  static constexpr std::uint32_t FILL_SIZE = 2 * NumRxFrames;
  static constexpr std::uint32_t COMP_SIZE = NumTxFrames;
  static constexpr std::uint32_t RX_SIZE   = NumRxFrames;
  static constexpr std::uint32_t TX_SIZE   = NumTxFrames;

  static constexpr std::uint32_t BATCH_SIZE = 64;

  static constexpr bool          USE_IRQ_SUSPEND      = false;
  static constexpr std::uint32_t NAPI_DEFER_HARD_IRQS = 2;
  static constexpr std::uint64_t NAPI_GRO_FLUSH_NS    = 2'000;
  static constexpr std::uint64_t NAPI_IRQ_SUSPEND_NS  = 20'000'000;

public:
  explicit AFXDP(const char*   interface,
                 std::uint32_t queueId     = 0,
                 const char*   bpfProgPath = "af_xdp_kern.o") noexcept;
  ~AFXDP();

  AFXDP(const AFXDP&)            = delete;
  AFXDP& operator=(const AFXDP&) = delete;
  AFXDP(AFXDP&& other) noexcept;
  AFXDP& operator=(AFXDP&& other) noexcept;

  [[nodiscard]] bool init();
  void shutdown() noexcept;

  [[nodiscard]] int  fd()         const noexcept;
  [[nodiscard]] bool isCopyMode() const noexcept;

  [[nodiscard, gnu::always_inline, gnu::hot]]
  inline RxFrame tryReceive() noexcept
      requires (M != AFXDPMode::TxOnly);

  [[gnu::always_inline, gnu::hot]]
  inline void release() noexcept
      requires (M != AFXDPMode::TxOnly);

  [[nodiscard, gnu::always_inline, gnu::hot]]
  inline std::uint8_t* acquire(std::uint32_t frameLen) noexcept
      requires (M != AFXDPMode::RxOnly);

  [[gnu::always_inline, gnu::hot]]
  inline void commit() noexcept
      requires (M != AFXDPMode::RxOnly);

  [[nodiscard, gnu::always_inline, gnu::hot]]
  inline bool send(std::span<const std::uint8_t> frame) noexcept
      requires (M != AFXDPMode::RxOnly);

  void prefillRing(std::span<const std::uint8_t> frameTemplate) noexcept
      requires (M != AFXDPMode::RxOnly);

private:
  void steerAllTrafficToQueue() noexcept;
  void waitForCarrier() noexcept;

  [[gnu::always_inline, gnu::hot]]
  inline void reapTx() noexcept
      requires (M != AFXDPMode::RxOnly);

  [[gnu::always_inline, gnu::hot]]
  inline void kickTx() noexcept
      requires (M != AFXDPMode::RxOnly);

  void loadCustomBpf(const char* path);
  void moveFrom(AFXDP& o) noexcept;

  const char*     m_interface{nullptr};
  std::uint32_t   m_queueId{0};
  const char*     m_bpfProgPath{nullptr};

  xsk_umem_info   m_umem{};
  xsk_socket_info m_xsk{};

  xdp_program*    m_xdpProg{nullptr};
  int             m_ifindex{0};
  int             m_fd{-1};

  bool            m_copyMode{false};
  bool            m_customBpf{false};
  bool            m_txKickAlways{false};

  std::uint64_t   m_pendingRxAddr{0};
  std::uint64_t   m_pendingTxAddr{0};
  std::uint32_t   m_pendingTxLen{0};
  std::uint32_t   m_txFrameNb{0};
};

// =============================================================================

template<AFXDPMode M, std::uint32_t NumRxFrames, std::uint32_t NumTxFrames,
         std::uint32_t FrameSize, bool NeedWakeup>
AFXDP<M, NumRxFrames, NumTxFrames, FrameSize, NeedWakeup>::
AFXDP(const char* interface, std::uint32_t queueId, const char* bpfProgPath) noexcept
  : m_interface{interface}, m_queueId{queueId}, m_bpfProgPath{bpfProgPath} {}

template<AFXDPMode M, std::uint32_t NumRxFrames, std::uint32_t NumTxFrames,
         std::uint32_t FrameSize, bool NeedWakeup>
AFXDP<M, NumRxFrames, NumTxFrames, FrameSize, NeedWakeup>::
~AFXDP() { shutdown(); }

template<AFXDPMode M, std::uint32_t NumRxFrames, std::uint32_t NumTxFrames,
         std::uint32_t FrameSize, bool NeedWakeup>
AFXDP<M, NumRxFrames, NumTxFrames, FrameSize, NeedWakeup>::
AFXDP(AFXDP&& other) noexcept { moveFrom(other); }

template<AFXDPMode M, std::uint32_t NumRxFrames, std::uint32_t NumTxFrames,
         std::uint32_t FrameSize, bool NeedWakeup>
AFXDP<M, NumRxFrames, NumTxFrames, FrameSize, NeedWakeup>&
AFXDP<M, NumRxFrames, NumTxFrames, FrameSize, NeedWakeup>::
operator=(AFXDP&& other) noexcept {
  if (this != &other) { shutdown(); moveFrom(other); }
  return *this;
}

template<AFXDPMode M, std::uint32_t NumRxFrames, std::uint32_t NumTxFrames,
         std::uint32_t FrameSize, bool NeedWakeup>
bool AFXDP<M, NumRxFrames, NumTxFrames, FrameSize, NeedWakeup>::
init() {
  {
    struct rlimit r{ RLIM_INFINITY, RLIM_INFINITY };
    if (::setrlimit(RLIMIT_MEMLOCK, &r) != 0)
      fmt::print(stderr, "[AFXDP] WARN: setrlimit(RLIMIT_MEMLOCK): {}\n", std::strerror(errno));
  }

  m_ifindex = static_cast<int>(::if_nametoindex(m_interface));
  if (m_ifindex == 0) {
    fmt::print(stderr, "[AFXDP] bad interface: {}\n", m_interface);
    return false;
  }

  if constexpr (HAS_TX) {
    char lnk[256]{};
    const std::string drvPath =
        std::string("/sys/class/net/") + m_interface + "/device/driver";
    const ssize_t n = ::readlink(drvPath.c_str(), lnk, sizeof(lnk) - 1);
    if (n > 0) {
      const char* base = std::strrchr(lnk, '/');
      m_txKickAlways = base && std::strstr(base, "mlx5");
      if (m_txKickAlways)
        fmt::print(stderr, "[AFXDP] {}: mlx5 — TX kick policy: ALWAYS "
                           "(gated kick races NAPI-idle on this driver)\n", m_interface);
    }
  }

  bpf_xdp_detach(m_ifindex, XDP_FLAGS_DRV_MODE, nullptr);
  bpf_xdp_detach(m_ifindex, XDP_FLAGS_SKB_MODE, nullptr);

  if constexpr (HAS_RX)
    steerAllTrafficToQueue();

  if constexpr (HAS_RX) {
    if (m_bpfProgPath && m_bpfProgPath[0] != '\0')
      loadCustomBpf(m_bpfProgPath);
  }

  const std::size_t umemSize = static_cast<std::size_t>(NUM_FRAMES) * FrameSize;
  m_umem.buffer = static_cast<std::uint8_t*>(
    ::mmap(nullptr, umemSize, PROT_READ | PROT_WRITE,
           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
  if (m_umem.buffer == MAP_FAILED) {
    fmt::print(stderr, "[AFXDP] mmap failed: {}\n", std::strerror(errno));
    m_umem.buffer = nullptr;
    return false;
  }

  xsk_umem_config umem_cfg{};
  umem_cfg.fill_size      = FILL_SIZE;
  umem_cfg.comp_size      = COMP_SIZE;
  umem_cfg.frame_size     = FrameSize;
  umem_cfg.frame_headroom = XSK_UMEM__DEFAULT_FRAME_HEADROOM;

  if (int ret = xsk_umem__create(&m_umem.umem, m_umem.buffer, umemSize,
                                 &m_umem.fq, &m_umem.cq, &umem_cfg)) {
    fmt::print(stderr, "[AFXDP] xsk_umem__create failed: {}\n", std::strerror(-ret));
    return false;
  }

  if constexpr (HAS_RX) {
    __u32 idx;
    unsigned int got = xsk_ring_prod__reserve(&m_umem.fq, RX_POOL_FRAMES, &idx);
    if (got != RX_POOL_FRAMES)
      fmt::print(stderr, "[AFXDP] WARN: FILL reserve {} != {}\n", got, RX_POOL_FRAMES);
    for (std::uint32_t i = 0; i < got; i++)
      *xsk_ring_prod__fill_addr(&m_umem.fq, idx++) =
        static_cast<std::uint64_t>(TX_POOL_FRAMES + i) * FrameSize;
    xsk_ring_prod__submit(&m_umem.fq, got);
  }

  std::uint32_t bindFlags = XDP_USE_NEED_WAKEUP;
  if constexpr (!NeedWakeup)
    bindFlags &= ~static_cast<std::uint32_t>(XDP_USE_NEED_WAKEUP);

  xsk_socket_config sock_cfg{};
  sock_cfg.rx_size      = RX_SIZE;
  sock_cfg.tx_size      = TX_SIZE;
  sock_cfg.libxdp_flags = m_customBpf ? XSK_LIBXDP_FLAGS__INHIBIT_PROG_LOAD : 0;

  xsk_ring_cons* rxp = HAS_RX ? &m_xsk.rx : nullptr;
  xsk_ring_prod* txp = HAS_TX ? &m_xsk.tx : nullptr;

  const char* bindMode = "generic";
  for (auto [xdpFlags, zc, label] : {
         std::tuple{XDP_FLAGS_DRV_MODE, XDP_ZEROCOPY, "zero-copy"},
         std::tuple{XDP_FLAGS_DRV_MODE, XDP_COPY,     "native-copy"},
         std::tuple{XDP_FLAGS_SKB_MODE, XDP_COPY,     "generic"}
       }) {
    sock_cfg.xdp_flags  = xdpFlags;
    sock_cfg.bind_flags = static_cast<__u16>(bindFlags | zc);
    if (xsk_socket__create(&m_xsk.xsk, m_interface, m_queueId, m_umem.umem,
                           rxp, txp, &sock_cfg) == 0) {
      bindMode   = label;
      m_copyMode = (zc == XDP_COPY);
      break;
    }
  }
  if (!m_xsk.xsk) {
    fmt::print(stderr, "[AFXDP] all socket modes failed\n");
    return false;
  }
  m_fd = xsk_socket__fd(m_xsk.xsk);

  fmt::print(stderr, "[AFXDP] {} | fd={} queue={} frames={} (tx={} rx={})\n",
             bindMode, m_fd, m_queueId, NUM_FRAMES, TX_POOL_FRAMES, RX_POOL_FRAMES);

  // ---- VERIFY (write-then-read-back): the bind ladder records what the
  // kernel ACCEPTED; only a query says what is ACTUALLY in force. Three
  // readbacks, all informational — the ladder keeps this transport generic
  // (office-desktop NICs without ZC or native XDP still run), the readback
  // makes the mode it landed in 100% explicit:
  //   1. XDP_OPTIONS      — zero-copy grant (authoritative; sets m_copyMode)
  //   2. bpf_xdp_query    — XDP program attach mode: native (DRV) vs SKB
  //   3. XDP_MMAP_OFFSETS — NEED_WAKEUP: the flags-bearing ring layout is only
  //      returned by kernels where ring need-wakeup flags are live (bind would
  //      have EINVAL'd on an unknown flag; this confirms positively).
  {
    const bool ladderZc = (std::strcmp(bindMode, "zero-copy") == 0);
    bool zc = !m_copyMode;
    xdp_options xopts{};
    socklen_t olen = sizeof(xopts);
    if (::getsockopt(m_fd, SOL_XDP, XDP_OPTIONS, &xopts, &olen) == 0) {
      zc         = (xopts.flags & XDP_OPTIONS_ZEROCOPY) != 0;
      m_copyMode = !zc;   // the kernel's answer overrides the ladder's label
    } else {
      fmt::print(stderr, "[AFXDP] WARN: getsockopt(XDP_OPTIONS): {} — cannot "
                         "verify zero-copy, trusting bind label '{}'\n",
                 std::strerror(errno), bindMode);
    }
    if (zc != ladderZc)
      fmt::print(stderr, "[AFXDP] WARN: bind accepted '{}' but kernel grant "
                         "says {} — readback wins\n",
                 bindMode, zc ? "ZERO-COPY" : "COPY");

    const char* attach = "no XDP prog (TxOnly)";
    if constexpr (HAS_RX) {
      LIBBPF_OPTS(bpf_xdp_query_opts, qopts);
      if (bpf_xdp_query(m_ifindex, 0, &qopts) == 0) {
        switch (qopts.attach_mode) {
          case XDP_ATTACHED_DRV:   attach = "native (DRV)";     break;
          case XDP_ATTACHED_SKB:   attach = "generic (SKB)";    break;
          case XDP_ATTACHED_HW:    attach = "hw-offload";       break;
          case XDP_ATTACHED_MULTI: attach = "multi";            break;
          case XDP_ATTACHED_NONE:  attach = "NONE (no prog?!)"; break;
          default:                 attach = "unknown";          break;
        }
      } else {
        attach = "query FAILED";
      }
    }
    fmt::print(stderr, "[AFXDP] kernel confirms: {} | XDP attach: {} "
                       "(XDP_OPTIONS=0x{:x})\n",
               zc ? "ZERO-COPY" : "COPY-mode", attach, xopts.flags);

    if constexpr (NeedWakeup) {
      xdp_mmap_offsets moff{};
      socklen_t mlen = sizeof(moff);
      if (::getsockopt(m_fd, SOL_XDP, XDP_MMAP_OFFSETS, &moff, &mlen) == 0
          && mlen == sizeof(moff)) {
        bool fillNw = false, txNw = false;
        if constexpr (HAS_RX) fillNw = xsk_ring_prod__needs_wakeup(&m_umem.fq) != 0;
        if constexpr (HAS_TX) txNw   = xsk_ring_prod__needs_wakeup(&m_xsk.tx) != 0;
        fmt::print(stderr, "[AFXDP] kernel confirms: NEED_WAKEUP live "
                           "(ring flags now: fill={} tx={})\n", fillNw, txNw);
      } else {
        fmt::print(stderr, "[AFXDP] WARN: NEED_WAKEUP NOT confirmed — kernel "
                           "returned {}B of {}B xdp_mmap_offsets (pre-5.4 ring "
                           "layout, no flags words)\n",
                   static_cast<std::size_t>(mlen), sizeof(moff));
      }
    }
  }

  if constexpr (HAS_RX) {
    int on = 1;
    if (::setsockopt(m_fd, SOL_SOCKET, SO_PREFER_BUSY_POLL, &on, sizeof(on)) < 0)
      fmt::print(stderr, "[AFXDP] WARN: SO_PREFER_BUSY_POLL: {}\n", std::strerror(errno));
    int usecs = 20;
    if (::setsockopt(m_fd, SOL_SOCKET, SO_BUSY_POLL, &usecs, sizeof(usecs)) < 0)
      fmt::print(stderr, "[AFXDP] WARN: SO_BUSY_POLL: {}\n", std::strerror(errno));
    int budget = static_cast<int>(BATCH_SIZE);
    if (::setsockopt(m_fd, SOL_SOCKET, SO_BUSY_POLL_BUDGET, &budget, sizeof(budget)) < 0)
      fmt::print(stderr, "[AFXDP] WARN: SO_BUSY_POLL_BUDGET: {}\n", std::strerror(errno));

    // READBACK: a clean setsockopt only means the kernel parsed the option;
    // ask what values the socket is actually holding.
    int rPrefer = -1, rUsecs = -1, rBudget = -1;
    socklen_t rl = sizeof(rPrefer);
    (void)::getsockopt(m_fd, SOL_SOCKET, SO_PREFER_BUSY_POLL, &rPrefer, &rl);
    rl = sizeof(rUsecs);
    (void)::getsockopt(m_fd, SOL_SOCKET, SO_BUSY_POLL, &rUsecs, &rl);
    rl = sizeof(rBudget);
    (void)::getsockopt(m_fd, SOL_SOCKET, SO_BUSY_POLL_BUDGET, &rBudget, &rl);
    if (rPrefer == on && rUsecs == usecs && rBudget == budget)
      fmt::print(stderr, "[AFXDP] kernel confirms: busy-poll ACTIVE "
                         "(prefer={} usecs={} budget={})\n",
                 rPrefer, rUsecs, rBudget);
    else
      fmt::print(stderr, "[AFXDP] WARN: busy-poll readback MISMATCH — wrote "
                         "prefer={} usecs={} budget={}, kernel holds "
                         "prefer={} usecs={} budget={}\n",
                 on, usecs, budget, rPrefer, rUsecs, rBudget);
  }

  if constexpr (HAS_RX) {
    std::uint32_t napiId  = 0;
    socklen_t     idLen   = sizeof(napiId);
    if (::getsockopt(m_fd, SOL_SOCKET, SO_INCOMING_NAPI_ID, &napiId, &idLen) == 0
        && napiId != 0) {
      const std::uint32_t defer = NAPI_DEFER_HARD_IRQS;
      const std::uint64_t gro   = NAPI_GRO_FLUSH_NS;
      const std::uint64_t susp  = USE_IRQ_SUSPEND ? NAPI_IRQ_SUSPEND_NS : 0;
      const bool ok = napi::setBusyPoll(napiId, defer, gro, susp);
      fmt::print(stderr,
        "[AFXDP] NAPI {}: defer-hard-irqs={} gro-flush={}ns irq-suspend={}ns ({}) => {}\n",
        napiId, defer, gro, susp,
        USE_IRQ_SUSPEND ? "irq-suspend regime" : "brute-force busy-poll",
        ok ? "applied" : "FAILED");
      // READBACK via NETDEV_CMD_NAPI_GET — the netlink ACK above only says the
      // kernel accepted the message; this asks what the NAPI actually runs with.
      if (ok) {
        const napi::NapiParams rb = napi::getBusyPoll(napiId);
        if (rb.valid && rb.deferHardIrqs == defer && rb.groFlushNs == gro
            && rb.irqSuspendNs == susp)
          fmt::print(stderr, "[AFXDP] kernel confirms: NAPI {} params VERIFIED\n",
                     napiId);
        else if (rb.valid)
          fmt::print(stderr, "[AFXDP] WARN: NAPI {} readback MISMATCH — kernel "
                             "holds defer={} gro={}ns irq-suspend={}ns\n",
                     napiId, rb.deferHardIrqs, rb.groFlushNs, rb.irqSuspendNs);
        else
          fmt::print(stderr, "[AFXDP] WARN: NAPI {} readback unavailable — set "
                             "not independently confirmed\n", napiId);
      }
    } else {
      fmt::print(stderr, "[AFXDP] WARN: SO_INCOMING_NAPI_ID unavailable "
                         "(napi_id={}) — NAPI config not applied\n", napiId);
    }
  }

  if (HAS_RX && m_customBpf && m_xdpProg) {
    bpf_object* bpf_obj = xdp_program__bpf_obj(m_xdpProg);
    bpf_map*    map     = bpf_object__find_map_by_name(bpf_obj, "xsks_map");
    if (map) {
      int xsks_map_fd = bpf_map__fd(map);
      int fd_val      = m_fd;
      int key         = static_cast<int>(m_queueId);
      if (bpf_map_update_elem(xsks_map_fd, &key, &fd_val, 0) != 0)
        fmt::print(stderr, "[AFXDP] WARN: XSKMAP insert failed: {}\n", std::strerror(errno));
      else
        fmt::print(stderr, "[AFXDP] XSKMAP populated (fd={} key={})\n", fd_val, key);
    } else {
      fmt::print(stderr, "[AFXDP] WARN: xsks_map not found in BPF\n");
    }
  }

  return true;
}

template<AFXDPMode M, std::uint32_t NumRxFrames, std::uint32_t NumTxFrames,
         std::uint32_t FrameSize, bool NeedWakeup>
void AFXDP<M, NumRxFrames, NumTxFrames, FrameSize, NeedWakeup>::
shutdown() noexcept {
  if (m_xsk.xsk) {
    xdp_statistics stats{};
    socklen_t      slen = sizeof(stats);
    if (::getsockopt(m_fd, SOL_XDP, XDP_STATISTICS, &stats, &slen) == 0)
      fmt::print(stderr,
                 "[XSK stats] rx_dropped={} rx_invalid_descs={} rx_ring_full={} "
                 "rx_fill_empty={} tx_invalid={} tx_ring_empty={}\n",
                 stats.rx_dropped, stats.rx_invalid_descs, stats.rx_ring_full,
                 stats.rx_fill_ring_empty_descs, stats.tx_invalid_descs,
                 stats.tx_ring_empty_descs);
    xsk_socket__delete(m_xsk.xsk);
    m_xsk.xsk = nullptr;
    m_fd      = -1;
  }
  if (m_umem.umem) {
    (void)xsk_umem__delete(m_umem.umem);
    m_umem.umem = nullptr;
  }
  if (m_umem.buffer && m_umem.buffer != MAP_FAILED) {
    ::munmap(m_umem.buffer, static_cast<std::size_t>(NUM_FRAMES) * FrameSize);
    m_umem.buffer = nullptr;
  }
  if (m_xdpProg) {
    xdp_program__detach(m_xdpProg, m_ifindex, XDP_MODE_NATIVE, 0);
    xdp_program__close(m_xdpProg);
    m_xdpProg = nullptr;
  }
  if (m_ifindex > 0) {
    bpf_xdp_detach(m_ifindex, XDP_FLAGS_DRV_MODE, nullptr);
    bpf_xdp_detach(m_ifindex, XDP_FLAGS_SKB_MODE, nullptr);
    m_ifindex = 0;
  }
}

template<AFXDPMode M, std::uint32_t NumRxFrames, std::uint32_t NumTxFrames,
         std::uint32_t FrameSize, bool NeedWakeup>
int AFXDP<M, NumRxFrames, NumTxFrames, FrameSize, NeedWakeup>::
fd() const noexcept { return m_fd; }

template<AFXDPMode M, std::uint32_t NumRxFrames, std::uint32_t NumTxFrames,
         std::uint32_t FrameSize, bool NeedWakeup>
bool AFXDP<M, NumRxFrames, NumTxFrames, FrameSize, NeedWakeup>::
isCopyMode() const noexcept { return m_copyMode; }

template<AFXDPMode M, std::uint32_t NumRxFrames, std::uint32_t NumTxFrames,
         std::uint32_t FrameSize, bool NeedWakeup>
inline RxFrame AFXDP<M, NumRxFrames, NumTxFrames, FrameSize, NeedWakeup>::
tryReceive() noexcept
    requires (M != AFXDPMode::TxOnly) {
  __u32 idx;
  if (xsk_ring_cons__peek(&m_xsk.rx, 1, &idx) == 0) [[likely]] {
    if constexpr (NeedWakeup) {
      ::recvfrom(m_fd, nullptr, 0, MSG_DONTWAIT, nullptr, nullptr);
      if (xsk_ring_cons__peek(&m_xsk.rx, 1, &idx) == 0)
        return {};
    } else {
      return {};
    }
  }
  const struct xdp_desc* desc = xsk_ring_cons__rx_desc(&m_xsk.rx, idx);
  m_pendingRxAddr = desc->addr;
  return { .data = {m_umem.buffer + desc->addr, desc->len}, .sec = 0, .nsec = 0, .status = 1 };
}

template<AFXDPMode M, std::uint32_t NumRxFrames, std::uint32_t NumTxFrames,
         std::uint32_t FrameSize, bool NeedWakeup>
inline void AFXDP<M, NumRxFrames, NumTxFrames, FrameSize, NeedWakeup>::
release() noexcept
    requires (M != AFXDPMode::TxOnly) {
  __u32 idx_fq;
  if (xsk_ring_prod__reserve(&m_umem.fq, 1, &idx_fq) != 1) [[unlikely]] {
    ::recvfrom(m_fd, nullptr, 0, MSG_DONTWAIT, nullptr, nullptr);
    if (xsk_ring_prod__reserve(&m_umem.fq, 1, &idx_fq) != 1) {
      xsk_ring_cons__release(&m_xsk.rx, 1);
      return;
    }
  }
  *xsk_ring_prod__fill_addr(&m_umem.fq, idx_fq) = m_pendingRxAddr;
  xsk_ring_prod__submit(&m_umem.fq, 1);
  xsk_ring_cons__release(&m_xsk.rx, 1);
}

template<AFXDPMode M, std::uint32_t NumRxFrames, std::uint32_t NumTxFrames,
         std::uint32_t FrameSize, bool NeedWakeup>
inline std::uint8_t* AFXDP<M, NumRxFrames, NumTxFrames, FrameSize, NeedWakeup>::
acquire(std::uint32_t frameLen) noexcept
    requires (M != AFXDPMode::RxOnly) {
  reapTx();
  m_pendingTxAddr = static_cast<std::uint64_t>(m_txFrameNb) * FrameSize;
  m_pendingTxLen  = frameLen;
  return m_umem.buffer + m_pendingTxAddr;
}

template<AFXDPMode M, std::uint32_t NumRxFrames, std::uint32_t NumTxFrames,
         std::uint32_t FrameSize, bool NeedWakeup>
inline void AFXDP<M, NumRxFrames, NumTxFrames, FrameSize, NeedWakeup>::
commit() noexcept
    requires (M != AFXDPMode::RxOnly) {
  __u32 idx;
  while (xsk_ring_prod__reserve(&m_xsk.tx, 1, &idx) != 1) {
    kickTx();
    reapTx();
  }
  struct xdp_desc* d = xsk_ring_prod__tx_desc(&m_xsk.tx, idx);
  d->addr    = m_pendingTxAddr;
  d->len     = m_pendingTxLen;
  d->options = 0;
  xsk_ring_prod__submit(&m_xsk.tx, 1);
  m_xsk.outstanding_tx++;
  m_txFrameNb = (m_txFrameNb + 1) & TX_POOL_MASK;

  kickTx();
}

template<AFXDPMode M, std::uint32_t NumRxFrames, std::uint32_t NumTxFrames,
         std::uint32_t FrameSize, bool NeedWakeup>
inline bool AFXDP<M, NumRxFrames, NumTxFrames, FrameSize, NeedWakeup>::
send(std::span<const std::uint8_t> frame) noexcept
    requires (M != AFXDPMode::RxOnly) {
  auto* dst = acquire(static_cast<std::uint32_t>(frame.size()));
  if (!dst) [[unlikely]] return false;
  std::memcpy(dst, frame.data(), frame.size());
  commit();
  return true;
}

template<AFXDPMode M, std::uint32_t NumRxFrames, std::uint32_t NumTxFrames,
         std::uint32_t FrameSize, bool NeedWakeup>
void AFXDP<M, NumRxFrames, NumTxFrames, FrameSize, NeedWakeup>::
prefillRing(std::span<const std::uint8_t> frameTemplate) noexcept
    requires (M != AFXDPMode::RxOnly) {
  for (std::uint32_t i = 0; i < TX_POOL_FRAMES; i++) {
    std::uint64_t addr = static_cast<std::uint64_t>(i) * FrameSize;
    std::memcpy(m_umem.buffer + addr, frameTemplate.data(), frameTemplate.size());
  }
}

template<AFXDPMode M, std::uint32_t NumRxFrames, std::uint32_t NumTxFrames,
         std::uint32_t FrameSize, bool NeedWakeup>
void AFXDP<M, NumRxFrames, NumTxFrames, FrameSize, NeedWakeup>::
steerAllTrafficToQueue() noexcept {
  if (m_queueId != 0) {
    fmt::print(stderr, "[AFXDP] WARN: queue {} != 0 — skipping combined=1 steering "
                       "(it routes all RX to queue 0); steer RSS to queue {} manually\n",
               m_queueId, m_queueId);
    return;
  }
  int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) {
    fmt::print(stderr, "[AFXDP] WARN: steering socket: {}\n", std::strerror(errno));
    return;
  }
  auto ethtoolIoctl = [&](void* cmd) {
    ifreq ifr{};
    std::strncpy(ifr.ifr_name, m_interface, IFNAMSIZ - 1);
    ifr.ifr_data = static_cast<char*>(cmd);
    return ::ioctl(fd, SIOCETHTOOL, &ifr) == 0;
  };

  ethtool_channels ch{};
  ch.cmd = ETHTOOL_GCHANNELS;
  if (!ethtoolIoctl(&ch) || ch.max_combined == 0) {
    fmt::print(stderr, "[AFXDP] WARN: {} has no combined channels (driver lacks `ethtool -L`); "
                       "cannot guarantee single-queue steering\n", m_interface);
    ::close(fd);
    return;
  }

  if (ch.combined_count == 1) {
    fmt::print(stderr, "[AFXDP] OK: {} already at 1 RX queue (combined=1) — no change, no link bounce\n",
               m_interface);
    ::close(fd);
    return;
  }

  ch.cmd            = ETHTOOL_SCHANNELS;
  ch.combined_count = 1;
  ethtoolIoctl(&ch);

  ethtool_channels rb{};
  rb.cmd = ETHTOOL_GCHANNELS;
  const bool ok = ethtoolIoctl(&rb) && rb.combined_count == 1;
  ::close(fd);

  if (!ok) {
    fmt::print(stderr, "[AFXDP] FAIL: {} combined={} (want 1) — RX may not reach the XSK; "
                       "run: sudo ethtool -L {} combined 1\n",
               m_interface, rb.combined_count, m_interface);
    return;
  }
  fmt::print(stderr, "[AFXDP] {} steered to 1 RX queue (combined=1) — link bounced, waiting for carrier...\n",
             m_interface);
  waitForCarrier();
}

template<AFXDPMode M, std::uint32_t NumRxFrames, std::uint32_t NumTxFrames,
         std::uint32_t FrameSize, bool NeedWakeup>
void AFXDP<M, NumRxFrames, NumTxFrames, FrameSize, NeedWakeup>::
waitForCarrier() noexcept {
  const timespec settle{0, 500'000'000};
  ::nanosleep(&settle, nullptr);

  char path[64];
  std::snprintf(path, sizeof(path), "/sys/class/net/%s/carrier", m_interface);

  const timespec tick{0, 100'000'000};
  for (int i = 0; i < 60; i++) {
    int cfd = ::open(path, O_RDONLY);
    if (cfd >= 0) {
      char c = 0;
      ssize_t n = ::read(cfd, &c, 1);
      ::close(cfd);
      if (n == 1 && c == '1') {
        fmt::print(stderr, "[AFXDP] {} link up after ~{} ms\n", m_interface, 500 + i * 100);
        return;
      }
    }
    ::nanosleep(&tick, nullptr);
  }
  fmt::print(stderr, "[AFXDP] WARN: {} carrier still down after ~6 s — initial packets may drop\n",
             m_interface);
}

template<AFXDPMode M, std::uint32_t NumRxFrames, std::uint32_t NumTxFrames,
         std::uint32_t FrameSize, bool NeedWakeup>
inline void AFXDP<M, NumRxFrames, NumTxFrames, FrameSize, NeedWakeup>::
reapTx() noexcept
    requires (M != AFXDPMode::RxOnly) {
  if (!m_xsk.outstanding_tx) return;
  __u32 idx;
  unsigned int rcvd = xsk_ring_cons__peek(&m_umem.cq, BATCH_SIZE, &idx);
  if (rcvd > 0) {
    xsk_ring_cons__release(&m_umem.cq, rcvd);
    m_xsk.outstanding_tx -= rcvd;
  }
}

template<AFXDPMode M, std::uint32_t NumRxFrames, std::uint32_t NumTxFrames,
         std::uint32_t FrameSize, bool NeedWakeup>
inline void AFXDP<M, NumRxFrames, NumTxFrames, FrameSize, NeedWakeup>::
kickTx() noexcept
    requires (M != AFXDPMode::RxOnly) {
  if constexpr (HAS_RX) {
    ::sendto(m_fd, nullptr, 0, MSG_DONTWAIT, nullptr, 0);
  } else if constexpr (NeedWakeup) {
    if (m_txKickAlways || xsk_ring_prod__needs_wakeup(&m_xsk.tx))
      ::sendto(m_fd, nullptr, 0, MSG_DONTWAIT, nullptr, 0);
  } else {
    ::sendto(m_fd, nullptr, 0, MSG_DONTWAIT, nullptr, 0);
  }
}

template<AFXDPMode M, std::uint32_t NumRxFrames, std::uint32_t NumTxFrames,
         std::uint32_t FrameSize, bool NeedWakeup>
void AFXDP<M, NumRxFrames, NumTxFrames, FrameSize, NeedWakeup>::
loadCustomBpf(const char* path) {
  m_xdpProg = xdp_program__open_file(path, nullptr, nullptr);
  if (int err = libxdp_get_error(m_xdpProg)) {
    fmt::print(stderr, "[AFXDP] BPF load failed ({}): {}\n", path, std::strerror(-err));
    m_xdpProg = nullptr;
    return;
  }
  if (int err = xdp_program__attach(m_xdpProg, m_ifindex, XDP_MODE_NATIVE, 0)) {
    fmt::print(stderr, "[AFXDP] BPF attach failed: {}\n", std::strerror(-err));
    xdp_program__close(m_xdpProg);
    m_xdpProg = nullptr;
    return;
  }
  m_customBpf = true;
  fmt::print(stderr, "[AFXDP] custom BPF loaded + attached: {}\n", path);
}

template<AFXDPMode M, std::uint32_t NumRxFrames, std::uint32_t NumTxFrames,
         std::uint32_t FrameSize, bool NeedWakeup>
void AFXDP<M, NumRxFrames, NumTxFrames, FrameSize, NeedWakeup>::
moveFrom(AFXDP& o) noexcept {
  m_interface     = o.m_interface;
  m_queueId       = o.m_queueId;
  m_bpfProgPath   = o.m_bpfProgPath;
  m_umem          = o.m_umem;
  m_xsk           = o.m_xsk;
  m_xdpProg       = std::exchange(o.m_xdpProg, nullptr);
  m_ifindex       = std::exchange(o.m_ifindex, 0);
  m_fd            = std::exchange(o.m_fd, -1);
  m_copyMode      = o.m_copyMode;
  m_customBpf     = o.m_customBpf;
  m_txKickAlways  = o.m_txKickAlways;
  m_pendingRxAddr = o.m_pendingRxAddr;
  m_pendingTxAddr = o.m_pendingTxAddr;
  m_pendingTxLen  = o.m_pendingTxLen;
  m_txFrameNb     = o.m_txFrameNb;
  o.m_umem.umem   = nullptr;
  o.m_umem.buffer = nullptr;
  o.m_xsk.xsk     = nullptr;
}

#endif // ABTRDA3_AFXDP_HPP
