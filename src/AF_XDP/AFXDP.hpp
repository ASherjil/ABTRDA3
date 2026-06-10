//
// AFXDP — unified xdpsock-derived AF_XDP transport.
//
// One class template owns the whole stack: UMEM mmap, FILL/COMP/RX/TX rings,
// the XDP program, the socket, AND the hot-path TX/RX. Mirrors the structure of
// Intel_I210 / Cadence_GEM:
//   * cheap constructor stores config (interface, queueId, bpf path),
//   * init() does the cold-path bring-up and returns bool,
//   * shutdown() (called by the destructor and move-assign) tears down,
//   * tryReceive/release + acquire/commit/send/prefillRing are the hot path.
//
// The ring/UMEM state is held in the SAME two structs xdpsock.c uses
// (xsk_umem_info / xsk_socket_info) so the Rx/Tx code reads 1:1 with the
// example. Everything latency-relevant is a compile-time constant so the
// optimiser can strength-reduce it: FrameSize -> shift, TX-pool wrap -> mask
// (no division), NeedWakeup -> the kickTx() wakeup branch is compiled out.
//

#ifndef ABTRDA3_AFXDP_HPP
#define ABTRDA3_AFXDP_HPP

#include "../common/RxFrame.hpp"
#include "../common/NapiConfig.hpp"   // per-NAPI irq-suspend-timeout busy-poll knobs

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
#ifndef SO_INCOMING_NAPI_ID
#define SO_INCOMING_NAPI_ID 56
#endif
#ifndef SOL_XDP
#define SOL_XDP 283
#endif

enum class AFXDPMode : std::uint8_t { RxOnly, TxOnly, RxTx };

// Mirror of xdpsock.c's xsk_umem_info / xsk_socket_info so the Rx/Tx hot path
// reads 1:1 with the example. The stats sub-structs (ring/app/driver) are
// omitted — we read the kernel's XDP_STATISTICS at teardown instead. In xdpsock
// these are heap structs with a umem POINTER (so a shared UMEM can back many
// sockets); we are single-socket, so we embed both by value.
struct xsk_umem_info {
  xsk_ring_prod fq{};            // FILL ring (xdpsock: umem->fq)
  xsk_ring_cons cq{};            // COMPLETION ring (xdpsock: umem->cq)
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

  // UMEM frame partition (compile-time). TX owns [0 .. TX_POOL_FRAMES),
  // RX/FILL owns [TX_POOL_FRAMES .. NUM_FRAMES). The pools are DISJOINT: our
  // API copies into a dedicated TX frame (unlike xdpsock l2fwd's in-place
  // reflect), so a TX frame is never also an RX/FILL frame.
  static constexpr std::uint32_t TX_POOL_FRAMES = HAS_TX ? NumTxFrames : 0;
  static constexpr std::uint32_t RX_POOL_FRAMES = HAS_RX ? NumRxFrames : 0;
  static constexpr std::uint32_t NUM_FRAMES     = TX_POOL_FRAMES + RX_POOL_FRAMES;

  // TX pool is a power of 2, so the producer index wraps with a mask instead of
  // an integer division on the hot path.
  static_assert(!HAS_TX || (TX_POOL_FRAMES & (TX_POOL_FRAMES - 1)) == 0,
                "TX_POOL_FRAMES must be a power of 2");
  static constexpr std::uint32_t TX_POOL_MASK = TX_POOL_FRAMES - 1;

  // Ring capacities (all powers of 2 — libxsk requirement). FILL carries the
  // xdpsock-style 2x headroom; the rest match their pools.
  static constexpr std::uint32_t FILL_SIZE = 2 * NumRxFrames;
  static constexpr std::uint32_t COMP_SIZE = NumTxFrames;
  static constexpr std::uint32_t RX_SIZE   = NumRxFrames;
  static constexpr std::uint32_t TX_SIZE   = NumTxFrames;

  static constexpr std::uint32_t BATCH_SIZE = 64;

  // Busy-poll regime. CRITICAL kernel fact (net/core/dev.c busy_poll_stop): the
  // kernel only RE-ARMS NAPI deferral if (defer_hard_irqs_count && gro_flush_timeout)
  // are BOTH nonzero. If both are 0, after each busy-poll the HARDWARE IRQ fires
  // immediately -> NAPI runs in softirq from the IRQ, NOT inline from our recvfrom
  // -> we fall back to the INTERRUPT path (measured: 79us on igc). So busy-poll
  // REQUIRES nonzero defer + gro-flush. Two working regimes:
  //  * CLASSIC BUSY-POLL (default): small defer + small gro-flush, NO irq-suspend.
  //    Keeps busy-poll owning the NAPI but WITHOUT the kernel-6.13 irq-suspend
  //    hrtimer/APIC machinery that perf showed wastes ~12% of the hot loop
  //    (native_write_msr 4.25%, lapic_next_deadline, hrtimer_start) on this
  //    always-hot one-in-flight workload (no idle phase for irq-suspend to exploit).
  //  * IRQ-SUSPEND (kernel 6.13+): USE_IRQ_SUSPEND=true -> add irq-suspend-timeout
  //    (good for bursty/idle-cycling server loads; not a saturated ping-pong).
  // We do NOT care about CPU on this 8-core box; busy-poll burns 100% by design.
  static constexpr bool          USE_IRQ_SUSPEND      = false;
  static constexpr std::uint32_t NAPI_DEFER_HARD_IRQS = 2;            // must be >0 for busy-poll
  static constexpr std::uint64_t NAPI_GRO_FLUSH_NS    = 2'000;        // 2us: small timer, NOT a floor while busy-polling
  static constexpr std::uint64_t NAPI_IRQ_SUSPEND_NS  = 20'000'000;   // 20ms (only used if USE_IRQ_SUSPEND)

public:
  explicit AFXDP(const char*   interface,
                 std::uint32_t queueId     = 0,
                 const char*   bpfProgPath = "af_xdp_kern.o") noexcept
    : m_interface{interface}, m_queueId{queueId}, m_bpfProgPath{bpfProgPath} {}

  AFXDP(const AFXDP&)            = delete;
  AFXDP& operator=(const AFXDP&) = delete;

  AFXDP(AFXDP&& other) noexcept { moveFrom(other); }
  AFXDP& operator=(AFXDP&& other) noexcept {
    if (this != &other) { shutdown(); moveFrom(other); }
    return *this;
  }
  ~AFXDP() { shutdown(); }

  // ── Cold path ─────────────────────────────────────────────────────────────
  // Bring up the whole AF_XDP stack. Returns false (no abort) on any failure so
  // the caller can fall back, matching Intel_I210 / Cadence_GEM::init().
  [[nodiscard]] bool init() {
    // 0. Raise RLIMIT_MEMLOCK to infinity — xdpsock main()'s first step. The
    //    UMEM pins its pages against this limit, and BPF map/prog memory was
    //    memlock-accounted pre-5.11. Best-effort (WARN, not fatal): modern
    //    memcg-accounted kernels don't need it; xdpsock hard-exits here.
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

    // Driver sniff for the TX-kick policy (see kickTx). mlx5's needs_wakeup flag
    // dynamics differ from igc/i40e: ~100 rounds in (when its NAPI busy window
    // first expires) a TxOnly gated kick races the NAPI-idle transition — the
    // submit lands after the driver's final ring check but the flag read still
    // said "no kick needed" -> that descriptor is never transmitted -> permanent
    // hang (proven 2026-06-10 on cx4: two runs froze at ~85/~103 rounds with
    // tx_xsk_xmit == tx_xsk_cqes == app-posted-minus-one). Unconditional kick is
    // the same remedy the RxTx mode already needs on igc/i40e.
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

    // 1. Clean any stale XDP programs from a previous run.
    bpf_xdp_detach(m_ifindex, XDP_FLAGS_DRV_MODE, nullptr);
    bpf_xdp_detach(m_ifindex, XDP_FLAGS_SKB_MODE, nullptr);

    // 1.5 Steer ALL RX to the bound queue (ethtool -L combined 1). MUST happen
    //     here — before the XDP attach and the socket bind — see the method
    //     comment: it is a NIC queue reprogram (PF reset on i40e) and would
    //     wreck a live ZC bind / drained FILL ring if done afterwards.
    if constexpr (HAS_RX)
      steerAllTrafficToQueue();

    // 2. Load our custom redirect-all BPF (xdpsock: load_xdp_program). RX modes
    //    ONLY — the program's sole job is to redirect inbound frames into the
    //    XSKMAP, which a TxOnly socket never uses (it has no RX ring and is not
    //    inserted into the map). Attaching a redirect prog on a TX-only port is
    //    pointless and steals inbound frames into a dead map, so skip it.
    if constexpr (HAS_RX) {
      if (m_bpfProgPath && m_bpfProgPath[0] != '\0')
        loadCustomBpf(m_bpfProgPath);
    }

    // 3. Allocate the UMEM packet area (xdpsock: mmap).
    const std::size_t umemSize = static_cast<std::size_t>(NUM_FRAMES) * FrameSize;
    m_umem.buffer = static_cast<std::uint8_t*>(
      ::mmap(nullptr, umemSize, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    if (m_umem.buffer == MAP_FAILED) {
      fmt::print(stderr, "[AFXDP] mmap failed: {}\n", std::strerror(errno));
      m_umem.buffer = nullptr;
      return false;
    }

    // 4. Create the UMEM + FILL/COMP rings (xdpsock: xsk_configure_umem).
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

    // 4.5 Pre-load the FILL ring BEFORE the bind (CRITICAL on i40e ZC).
    // xsk_socket__create() makes the driver arm its HW RX descriptor ring from
    // FILL at bind time (i40e_alloc_rx_buffers_zc). An empty FILL at bind leaves
    // the HW RX ring without DMA targets — silent drops + "Failed to allocate
    // some buffers on AF_XDP ZC ... Rx ring 0" in dmesg. xdpsock populates FILL
    // before xsk_configure_socket() for exactly this reason.
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

    // 5. Bind flags (xdpsock: opt_xdp_bind_flags).
    std::uint32_t bindFlags = XDP_USE_NEED_WAKEUP;
    if constexpr (!NeedWakeup)
      bindFlags &= ~static_cast<std::uint32_t>(XDP_USE_NEED_WAKEUP);

    // 6. Create the socket with the xdpsock fallback chain (ZC → native-copy →
    //    generic). RX/TX rings are bound here.
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

    // Authoritative zero-copy check: ask the kernel what it actually granted
    // (the label above only reflects which bind flags were accepted).
    {
      xdp_options xopts{};
      socklen_t   olen = sizeof(xopts);
      if (::getsockopt(m_fd, SOL_XDP, XDP_OPTIONS, &xopts, &olen) == 0)
        fmt::print(stderr, "[AFXDP] kernel reports: {} (XDP_OPTIONS=0x{:x})\n",
                   (xopts.flags & XDP_OPTIONS_ZEROCOPY) ? "ZERO-COPY" : "COPY-mode",
                   xopts.flags);
      else
        fmt::print(stderr, "[AFXDP] WARN: getsockopt(XDP_OPTIONS): {}\n",
                   std::strerror(errno));
    }

    // 7. NAPI busy-polling (SO_PREFER_BUSY_POLL) — applied to ALL modes, RxTx
    // INCLUDED. This is the ULTRA-LOW-LATENCY polling path (~17us median on i40e):
    // recvfrom()/sendto() run the NAPI poll INLINE so RX drains without waiting on
    // a softirq/IRQ round-trip, instead of the ksoftirqd path (~96us on igc).
    //
    // KNOWN TRADEOFF (deliberately re-enabled for low-latency testing on the
    // XXV710): on an interleaved TX+RX (RxTx) socket the busy-poll recvfrom() owns
    // the NAPI and under-submits the ZC TX ring, so RxTx loses packets — intermittent
    // on i40e (e.g. 956/44), fully broken on igc (0 round-trips). The unconditional
    // kickTx() below mitigates this (every committed TX is poked), but does not fully
    // cure it on igc. The CORRECT-but-slower regime is to gate this back to
    // `if constexpr (!(HAS_RX && HAS_TX))` so RxTx omits busy-poll and TX is driven
    // by ksoftirqd. See [[project-afxdp-pingpong]] for the full A/B.
    // Busy-poll is an RX mechanism (it busy-loops the NAPI to drain RX inline);
    // it is meaningless on a TX-only socket. WORSE on igc: SO_PREFER_BUSY_POLL on
    // a TX-only socket makes igc_xsk_wakeup(XDP_WAKEUP_TX) a NOP when the shared
    // queue-pair NAPI is "already running", so the TX kick is swallowed and TX
    // stalls after a few packets (observed: tx frozen at ~4, hot thread camped in
    // the RX recvfrom). So apply busy-poll for RX modes ONLY.
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
    }

    // 7.5 NAPI per-vector busy-poll config (RX modes only). Find THIS socket's
    // NAPI (xsk_bind marks sk_napi_id at bind, so SO_INCOMING_NAPI_ID is valid).
    //   USE_IRQ_SUSPEND=true : set defer-hard-irqs + gro-flush + irq-suspend (the
    //     kernel-6.13 regime; good for bursty/idle server loads).
    //   USE_IRQ_SUSPEND=false (default, classic busy-poll): set defer + gro-flush
    //     (BOTH must be nonzero or busy-poll falls back to the IRQ path -> 79us!)
    //     but irq-suspend=0 -> removes the kernel-6.13 hrtimer/APIC storm (~12% of
    //     the hot loop here) while keeping busy-poll owning the NAPI.
    // Best-effort: WARN, never fatal.
    if constexpr (HAS_RX) {
      std::uint32_t napiId  = 0;
      socklen_t     idLen   = sizeof(napiId);
      if (::getsockopt(m_fd, SOL_SOCKET, SO_INCOMING_NAPI_ID, &napiId, &idLen) == 0
          && napiId != 0) {
        const std::uint32_t defer = NAPI_DEFER_HARD_IRQS;             // nonzero either regime
        const std::uint64_t gro   = NAPI_GRO_FLUSH_NS;                // nonzero either regime
        const std::uint64_t susp  = USE_IRQ_SUSPEND ? NAPI_IRQ_SUSPEND_NS : 0;
        const bool ok = napi::setBusyPoll(napiId, defer, gro, susp);
        fmt::print(stderr,
          "[AFXDP] NAPI {}: defer-hard-irqs={} gro-flush={}ns irq-suspend={}ns ({}) => {}\n",
          napiId, defer, gro, susp,
          USE_IRQ_SUSPEND ? "irq-suspend regime" : "brute-force busy-poll",
          ok ? "applied" : "FAILED");
      } else {
        fmt::print(stderr, "[AFXDP] WARN: SO_INCOMING_NAPI_ID unavailable "
                           "(napi_id={}) — NAPI config not applied\n", napiId);
      }
    }

    // 8. Insert the socket fd into the XSKMAP so the BPF can redirect to us
    //    (xdpsock: enter_xsks_into_map, gated on !BENCH_TXONLY — a TX-only
    //    socket has no RX to redirect). Key = queue id, matching rx_queue_index.
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

    // FILL is armed and the driver's HW RX ring was loaded at bind. Like
    // xdpsock, we do NOT prime here — the first empty-RX poll in tryReceive()
    // drives NAPI exactly as xdpsock's rx_drop/l2fwd loop does.
    return true;
  }

  // Idempotent teardown. Dumps the cumulative kernel-side XSK counters first:
  // rx_fill_empty > 0 means the driver ran out of FILL buffers (the i40e ZC
  // "Failed to allocate" symptom); rx_ring_full means userspace didn't drain RX
  // fast enough; tx_ring_empty means we kicked TX with nothing queued.
  void shutdown() noexcept {
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

  // ── Accessors ─────────────────────────────────────────────────────────────
  [[nodiscard]] int  fd()         const noexcept { return m_fd; }
  [[nodiscard]] bool isCopyMode() const noexcept { return m_copyMode; }

  // ── RX hot path ───────────────────────────────────────────────────────────
  // Peek EXACTLY one descriptor. xsk_ring_cons__peek advances cached_cons by the
  // number returned and release() frees exactly one — peeking more than we
  // release would desync cached_cons and silently drop frames 2..N.
  [[nodiscard, gnu::always_inline, gnu::hot]]
  inline RxFrame tryReceive() noexcept requires (HAS_RX) {
    __u32 idx;
    if (xsk_ring_cons__peek(&m_xsk.rx, 1, &idx) == 0) [[likely]] {
      // Empty peek. The recvfrom that drives the NAPI poll is needed ONLY when
      // bound with XDP_USE_NEED_WAKEUP (NeedWakeup=true). When NeedWakeup=false
      // (the arxiv "Cluster 0" lowest-latency config), the kernel never gates the
      // rings on a wakeup, so busy-poll drives the NAPI WITHOUT any syscall — we
      // just spin on the ring. Then RE-PEEK so a frame the poll just delivered is
      // returned in THIS call (A/B-proven tail win on igc: skipping the re-peek
      // worsened P99.99 25->48us, P99.999 40->94us, Max ~4x).
      if constexpr (NeedWakeup) {
        ::recvfrom(m_fd, nullptr, 0, MSG_DONTWAIT, nullptr, nullptr);
        if (xsk_ring_cons__peek(&m_xsk.rx, 1, &idx) == 0)
          return {};
      } else {
        return {};   // no syscall: next spin re-peeks the ring
      }
    }
    const struct xdp_desc* desc = xsk_ring_cons__rx_desc(&m_xsk.rx, idx);
    m_pendingRxAddr = desc->addr;
    return { .data = {m_umem.buffer + desc->addr, desc->len}, .sec = 0, .nsec = 0, .status = 1 };
  }

  [[gnu::always_inline, gnu::hot]]
  inline void release() noexcept requires (HAS_RX) {
    // xdpsock rx_drop order: return the buffer to the FILL ring BEFORE releasing
    // the RX slot. FILL has 2x headroom and we hold only RX_POOL_FRAMES buffers,
    // so the reserve always succeeds for our 1-in-flight ping-pong; the recvfrom
    // retry mirrors rx_drop's stall handling defensively.
    __u32 idx_fq;
    if (xsk_ring_prod__reserve(&m_umem.fq, 1, &idx_fq) != 1) [[unlikely]] {
      ::recvfrom(m_fd, nullptr, 0, MSG_DONTWAIT, nullptr, nullptr);
      if (xsk_ring_prod__reserve(&m_umem.fq, 1, &idx_fq) != 1) {
        xsk_ring_cons__release(&m_xsk.rx, 1);  // still free the RX slot — never leak it
        return;
      }
    }
    *xsk_ring_prod__fill_addr(&m_umem.fq, idx_fq) = m_pendingRxAddr;
    xsk_ring_prod__submit(&m_umem.fq, 1);
    xsk_ring_cons__release(&m_xsk.rx, 1);
  }

  // ── TX hot path ───────────────────────────────────────────────────────────
  // TX frames live in their own pool [0 .. TX_POOL_FRAMES) and never overlap the
  // RX/FILL pool. m_txFrameNb cycles within that pool; completed frames return
  // implicitly as the counter wraps (xdpsock txonly model).
  [[nodiscard, gnu::always_inline, gnu::hot]]
  inline std::uint8_t* acquire(std::uint32_t frameLen) noexcept requires (HAS_TX) {
    reapTx();  // reclaim completed TX frames — pure userspace, NO syscall
    m_pendingTxAddr = static_cast<std::uint64_t>(m_txFrameNb) * FrameSize;
    m_pendingTxLen  = frameLen;
    return m_umem.buffer + m_pendingTxAddr;
  }

  [[gnu::always_inline, gnu::hot]]
  inline void commit() noexcept requires (HAS_TX) {
    __u32 idx;
    // TX ring full — kick + reap until a slot frees (rare for ping-pong).
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
    m_txFrameNb = (m_txFrameNb + 1) & TX_POOL_MASK;  // power-of-2 wrap (no div)

    kickTx();  // exactly ONE syscall to start TX of the packet just submitted
  }

  [[nodiscard, gnu::always_inline, gnu::hot]]
  inline bool send(std::span<const std::uint8_t> frame) noexcept requires (HAS_TX) {
    auto* dst = acquire(static_cast<std::uint32_t>(frame.size()));
    if (!dst) [[unlikely]] return false;
    std::memcpy(dst, frame.data(), frame.size());
    commit();
    return true;
  }

  // Prefill ONLY the TX pool. RX-pool frames are already owned by the kernel
  // (submitted to FILL in init()), so touching them here would race inbound DMA.
  void prefillRing(std::span<const std::uint8_t> frameTemplate) noexcept requires (HAS_TX) {
    for (std::uint32_t i = 0; i < TX_POOL_FRAMES; i++) {
      std::uint64_t addr = static_cast<std::uint64_t>(i) * FrameSize;
      std::memcpy(m_umem.buffer + addr, frameTemplate.data(), frameTemplate.size());
    }
  }

private:
  // Collapse the NIC to ONE combined channel so ALL RX lands on queue 0 — the
  // queue the XSK binds to and the only key in the XSKMAP (== `ethtool -L <if>
  // combined 1`). MUST run BEFORE the XDP attach and the socket bind: changing
  // the channel count reprograms the NIC's queues (a PF reset on i40e), which
  // would tear down a live ZC bind, re-arm the HW RX ring from an already-
  // drained FILL ring (the "Failed to allocate" bug), and can drop the attached
  // XDP program. Single-queue steering targets queue 0 only.
  void steerAllTrafficToQueue() noexcept {
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

    // Already single-queue: do NOT re-set it. ethtool -L is a PF reset that
    // bounces the link DOWN for ~2.5-3 s on igc/i40e (confirmed via dmesg "NIC
    // Link is Down"); re-applying an unchanged value would bounce it for nothing
    // and drop the start of the run.
    if (ch.combined_count == 1) {
      fmt::print(stderr, "[AFXDP] OK: {} already at 1 RX queue (combined=1) — no change, no link bounce\n",
                 m_interface);
      ::close(fd);
      return;
    }

    // Collapse to a single combined channel. This reprograms the NIC and bounces
    // the link, so we MUST wait for carrier to return before any traffic starts.
    ch.cmd            = ETHTOOL_SCHANNELS;
    ch.combined_count = 1;                   // leave other/rx/tx counts as-is
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

  // After ethtool -L bounces the link (PF reset), wait for it to come back UP
  // before init() proceeds — otherwise the first packets are transmitted into a
  // dead link and lost (on igc the carrier is down ~2.5-3 s). Settle first so we
  // don't read a stale carrier=1 before the DOWN event registers, then poll.
  void waitForCarrier() noexcept {
    const timespec settle{0, 500'000'000};   // 500 ms — let the DOWN event land
    ::nanosleep(&settle, nullptr);

    char path[64];
    std::snprintf(path, sizeof(path), "/sys/class/net/%s/carrier", m_interface);

    const timespec tick{0, 100'000'000};      // 100 ms
    for (int i = 0; i < 60; i++) {            // up to ~6 s total
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

  // Reap completed TX descriptors back to the pool. Pure userspace, NO syscall.
  // Completed frames return implicitly as m_txFrameNb wraps; they must NOT be
  // donated to the FILL ring (that is the in-place l2fwd model, which would
  // merge the disjoint TX and RX pools).
  [[gnu::always_inline, gnu::hot]]
  inline void reapTx() noexcept requires (HAS_TX) {
    if (!m_xsk.outstanding_tx) return;
    __u32 idx;
    unsigned int rcvd = xsk_ring_cons__peek(&m_umem.cq, BATCH_SIZE, &idx);
    if (rcvd > 0) {
      xsk_ring_cons__release(&m_umem.cq, rcvd);
      m_xsk.outstanding_tx -= rcvd;
    }
  }

  // Kick the kernel to start TX (one sendto -> ndo_xsk_wakeup(TX)). Mirrors
  // xdpsock complete_tx_only: `if (!need_wakeup || needs_wakeup(tx)) kick`.
  //
  // RxTx (interleaved TX+RX, e.g. client/server): kick UNCONDITIONALLY. The
  // XDP_USE_NEED_WAKEUP skip-the-syscall hint is UNSAFE here — after a busy-poll
  // recvfrom() the TX needs_wakeup flag reads CLEAR, so a gated kick is skipped
  // and the just-submitted descriptor is NEVER transmitted (the recvfrom-driven
  // NAPI services RX but does not pull the ZC TX ring on igc/i40e). PROVEN by
  // cross-transport A/B: AF_XDP client/server TX delivered 0 packets while a
  // packet_mmap peer's TX delivered fine.
  //
  // TxOnly (txgen, AND the single-recorder's split-port Tx): needs_wakeup-gated
  // kick (xdpsock-faithful). Verified on igc that the gated kick drives the
  // single-recorder Tx cleanly (separate Tx/Rx ports = separate NAPIs, so the
  // shared-NAPI igc_xsk_wakeup TX-NOP that bit RxTx does not apply here). The
  // earlier AlwaysKick override was removed once this was proven (same latency).
  //
  // XDP_USE_NEED_WAKEUP stays set at bind for RxTx (required on i40e ZC; false
  // hangs the kernel) — we just choose when to use its hint.
  [[gnu::always_inline, gnu::hot]]
  inline void kickTx() noexcept requires (HAS_TX) {
    if constexpr (HAS_RX) {                       // RxTx: must always kick
      ::sendto(m_fd, nullptr, 0, MSG_DONTWAIT, nullptr, 0);
    } else if constexpr (NeedWakeup) {            // TxOnly + need_wakeup: gated (xdpsock)
      // mlx5 exception (m_txKickAlways, set by the init() driver sniff): the
      // gated kick races mlx5's NAPI-idle transition and strands a descriptor
      // ~100 rounds in — kick unconditionally there. igc/i40e keep the gated
      // path (verified clean on igc; same latency, fewer syscalls).
      if (m_txKickAlways || xsk_ring_prod__needs_wakeup(&m_xsk.tx))
        ::sendto(m_fd, nullptr, 0, MSG_DONTWAIT, nullptr, 0);
    } else {                                       // TxOnly, no need_wakeup: always kick
      ::sendto(m_fd, nullptr, 0, MSG_DONTWAIT, nullptr, 0);
    }
  }

  void loadCustomBpf(const char* path) {
    // xdpsock: load_xdp_program. xdp_program__open_file may return an error
    // pointer; check it with libxdp_get_error before use.
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

  // Transfer ownership of the kernel handles and copy the rest. The ring structs
  // (inside m_umem / m_xsk) hold pointers into the transferred mmap'd ring
  // memory, so copying them by value keeps them valid; we then null the source's
  // owning handles so its shutdown() is a no-op.
  void moveFrom(AFXDP& o) noexcept {
    m_interface     = o.m_interface;
    m_queueId       = o.m_queueId;
    m_bpfProgPath   = o.m_bpfProgPath;
    m_umem          = o.m_umem;        // fq, cq, umem*, buffer
    m_xsk           = o.m_xsk;         // rx, tx, xsk*, outstanding_tx
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

  // ── Members ───────────────────────────────────────────────────────────────
  // Config (set in ctor)
  const char*     m_interface{nullptr};
  std::uint32_t   m_queueId{0};
  const char*     m_bpfProgPath{nullptr};

  // UMEM + socket state, grouped exactly like xdpsock.c.
  xsk_umem_info   m_umem{};   // fq, cq, umem*, buffer
  xsk_socket_info m_xsk{};    // rx, tx, xsk*, outstanding_tx

  // Owning handles not part of the xdpsock structs.
  xdp_program*    m_xdpProg{nullptr};
  int             m_ifindex{0};
  int             m_fd{-1};   // cached xsk_socket__fd(m_xsk.xsk) for the hot path

  // Cold-path flags
  bool            m_copyMode{false};
  bool            m_customBpf{false};
  bool            m_txKickAlways{false};  // mlx5: gated TX kick races NAPI-idle (see init/kickTx)

  // Hot-path cursors
  std::uint64_t   m_pendingRxAddr{0};
  std::uint64_t   m_pendingTxAddr{0};
  std::uint32_t   m_pendingTxLen{0};
  std::uint32_t   m_txFrameNb{0};
};

#endif // ABTRDA3_AFXDP_HPP
