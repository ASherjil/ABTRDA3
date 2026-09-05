// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Areeb Sherjil

#ifndef ABTRDA3_AFXDP_HPP
#define ABTRDA3_AFXDP_HPP

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

#include <bpf/bpf.h>
#include <bpf/libbpf.h>
#include <fmt/core.h>
#include <xdp/libxdp.h>
#include <xdp/xsk.h>

#include "../common/NapiConfig.hpp"

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

enum class AFXDPMode : std::uint8_t {
    RxOnly,
    TxOnly,
    RxTx
};

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

template <AFXDPMode M, std::uint32_t NumRxFrames = 2048, std::uint32_t NumTxFrames = 2048,
          std::uint32_t FrameSize = 4096, bool NeedWakeup = true>
class AFXDP {
    static_assert(M == AFXDPMode::TxOnly || (NumRxFrames >= 8 && (NumRxFrames & (NumRxFrames - 1)) == 0),
                  "NumRxFrames must be a power of 2 and >= 8");
    static_assert(M == AFXDPMode::RxOnly || (NumTxFrames >= 8 && (NumTxFrames & (NumTxFrames - 1)) == 0),
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

    // NAPI deferral / irq-suspend VALUES. WHICH of them is applied is chosen at
    // runtime by init(enableDeferral, enableIrqSuspend) -> resolved into
    // m_napiDefer/m_napiGro/m_napiSusp (both flags off => all 0 = deferral OFF).
    // defer/gro are the canonical shared values (napi::) so the DPDK net_af_xdp
    // path applies the identical regime.
    static constexpr std::uint32_t NAPI_DEFER_HARD_IRQS = napi::kDeferHardIrqs;
    static constexpr std::uint64_t NAPI_GRO_FLUSH_NS    = napi::kGroFlushTimeoutNs;
    static constexpr std::uint64_t NAPI_IRQ_SUSPEND_NS  = 20'000'000;

public:
    explicit AFXDP(const char* interface, std::uint32_t queueId = 0,
                   const char* bpfProgPath = "af_xdp_kern.o") noexcept;
    ~AFXDP();

    AFXDP(const AFXDP&)            = delete;
    AFXDP& operator=(const AFXDP&) = delete;
    AFXDP(AFXDP&& other) noexcept;
    AFXDP& operator=(AFXDP&& other) noexcept;

    // enableDeferral   — classic NAPI deferral (napi_defer_hard_irqs + gro_flush_timeout).
    // enableIrqSuspend — kernel-6.13 irq-suspend-timeout regime (implies defer+gro on).
    // BOTH false => deferral AND irq-suspend are explicitly ZEROED (defer=gro=suspend=0),
    //   clearing any netdev-wide value a previous run may have left enabled.
    [[nodiscard]] bool init(bool enableDeferral = true, bool enableIrqSuspend = false);
    void               shutdown() noexcept;

    [[nodiscard]] int  fd() const noexcept;
    [[nodiscard]] bool isCopyMode() const noexcept;

    [[nodiscard, gnu::always_inline, gnu::hot]]
    inline std::span<const std::uint8_t> tryReceive() noexcept
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
    void applyNapiDeferralSysfs() const noexcept;

    void clearXdpProgram() noexcept;
    void loadXdpProgram(const char* path);
    void xskConfigureUmem();
    void xskPopulateFillRing();
    void xskConfigureSocket();
    void applySetSockOpt() const;
    void enterXsksIntoMap();
    void verifyConfig();

    [[gnu::always_inline, gnu::hot]]
    inline void completeTx() noexcept
        requires (M != AFXDPMode::RxOnly);

    [[gnu::always_inline, gnu::hot]]
    inline void kickTx() noexcept
        requires (M != AFXDPMode::RxOnly);

    void moveFrom(AFXDP& o) noexcept;

    const char*   m_interface{nullptr};
    std::uint32_t m_queueId{0};
    const char*   m_bpfProgPath{nullptr};
    std::string   m_bindMode;

    xsk_umem_info   m_umem{};
    xsk_socket_info m_xsk{};

    xdp_program* m_xdpProg{nullptr};
    int          m_ifindex{0};
    int          m_fd{-1};

    bool m_copyMode{false};
    bool m_customBpf{false};

    // NAPI regime: the two init() flags, resolved once into the values pushed to
    // sysfs (netdev-wide) and netlink (per-NAPI). All 0 == deferral OFF.
    bool          m_enableDeferral{true};
    bool          m_enableIrqSuspend{false};
    std::uint32_t m_napiDefer{0};
    std::uint64_t m_napiGro{0};
    std::uint64_t m_napiSusp{0};

    std::uint64_t m_pendingRxAddr{0};
    __u32         m_pendingFqIdx{0};   // FILL slot reserved in tryReceive(), filled in release()
    std::uint64_t m_pendingTxAddr{0};
    std::uint32_t m_pendingTxLen{0};
    std::uint32_t m_txFrameNb{0};
};

// =============================================================================

template <AFXDPMode M, std::uint32_t NumRxFrames, std::uint32_t NumTxFrames, std::uint32_t FrameSize,
          bool NeedWakeup>
AFXDP<M, NumRxFrames, NumTxFrames, FrameSize, NeedWakeup>::AFXDP(const char* interface, std::uint32_t queueId,
                                                                 const char* bpfProgPath) noexcept
    : m_interface{interface},
      m_queueId{queueId},
      m_bpfProgPath{bpfProgPath} {
}

template <AFXDPMode M, std::uint32_t NumRxFrames, std::uint32_t NumTxFrames, std::uint32_t FrameSize,
          bool NeedWakeup>
AFXDP<M, NumRxFrames, NumTxFrames, FrameSize, NeedWakeup>::~AFXDP() {
    shutdown();
}

template <AFXDPMode M, std::uint32_t NumRxFrames, std::uint32_t NumTxFrames, std::uint32_t FrameSize,
          bool NeedWakeup>
AFXDP<M, NumRxFrames, NumTxFrames, FrameSize, NeedWakeup>::AFXDP(AFXDP&& other) noexcept {
    moveFrom(other);
}

template <AFXDPMode M, std::uint32_t NumRxFrames, std::uint32_t NumTxFrames, std::uint32_t FrameSize,
          bool NeedWakeup>
AFXDP<M, NumRxFrames, NumTxFrames, FrameSize, NeedWakeup>&
AFXDP<M, NumRxFrames, NumTxFrames, FrameSize, NeedWakeup>::operator=(AFXDP&& other) noexcept {
    if (this != &other) {
        shutdown();
        moveFrom(other);
    }
    return *this;
}

template <AFXDPMode M, std::uint32_t NumRxFrames, std::uint32_t NumTxFrames, std::uint32_t FrameSize,
          bool NeedWakeup>
bool AFXDP<M, NumRxFrames, NumTxFrames, FrameSize, NeedWakeup>::init(bool enableDeferral,
                                                                     bool enableIrqSuspend) {
    m_enableDeferral   = enableDeferral;
    m_enableIrqSuspend = enableIrqSuspend;
    // Resolve the NAPI regime once from the two flags. irq-suspend implies defer+gro
    // (it cannot take effect without them) plus the suspend timeout; deferral is
    // defer+gro only; both-off ZEROES everything so a netdev-wide value a prior run
    // enabled is cleared. These feed both the sysfs writes (applyNapiDeferralSysfs)
    // and the per-NAPI netlink set in verifyConfig().
    if (m_enableIrqSuspend) {
        m_napiDefer = NAPI_DEFER_HARD_IRQS;
        m_napiGro   = NAPI_GRO_FLUSH_NS;
        m_napiSusp  = NAPI_IRQ_SUSPEND_NS;
    } else if (m_enableDeferral) {
        m_napiDefer = NAPI_DEFER_HARD_IRQS;
        m_napiGro   = NAPI_GRO_FLUSH_NS;
        m_napiSusp  = 0;
    } else {
        m_napiDefer = 0;
        m_napiGro   = 0;
        m_napiSusp  = 0;
    }

    {
        struct rlimit const r{.rlim_cur = RLIM_INFINITY, .rlim_max = RLIM_INFINITY};
        if (::setrlimit(RLIMIT_MEMLOCK, &r) != 0) {
            fmt::print(stderr, "[AFXDP] WARN: setrlimit(RLIMIT_MEMLOCK): {}\n", std::strerror(errno));
        }
    }

    m_ifindex = static_cast<int>(::if_nametoindex(m_interface));
    if (m_ifindex == 0) {
        fmt::print(stderr, "[AFXDP] bad interface: {}\n", m_interface);
        return false;
    }

    // Strip any XDP program a prior (possibly crashed) run left attached on this
    // interface before we attach ours — equivalent to `ip link set dev <iface> xdp off`.
    clearXdpProgram();

    // Enable (or clear) the netdev-wide busy-poll deferral for the chosen regime.
    // ALL modes: busy-poll drives the driver from BOTH recvmsg (RX) and sendto (TX),
    // so a TX-only socket needs it too. Done before the steer/bind recreates the NAPI.
    applyNapiDeferralSysfs();

    if constexpr (HAS_RX) {
        steerAllTrafficToQueue();
    }

    if constexpr (HAS_RX) {
        if (m_bpfProgPath && m_bpfProgPath[0] != '\0') {
            loadXdpProgram(m_bpfProgPath);
        }
    }

    xskConfigureUmem();
    if constexpr (HAS_RX) {
        xskPopulateFillRing();
    }
    xskConfigureSocket();
    applySetSockOpt();

    if constexpr (HAS_RX) {
        if (m_customBpf && m_xdpProg) {
            enterXsksIntoMap();
        }
    }

    verifyConfig();

    return true;
}

template <AFXDPMode M, std::uint32_t NumRxFrames, std::uint32_t NumTxFrames, std::uint32_t FrameSize,
          bool NeedWakeup>
void AFXDP<M, NumRxFrames, NumTxFrames, FrameSize, NeedWakeup>::applyNapiDeferralSysfs() const noexcept {
    // The netdev-wide NAPI deferral knobs PERSIST across runs and are inherited
    // by newly-created NAPIs — so a prior run that enabled deferral leaves them
    // set. (Re)write them on every init (napi::applyDeferralSysfs), BEFORE the
    // steer/bind recreates the NAPI, so the chosen regime is deterministic and a
    // stale enable cannot silently survive. Both flags off => 0/0 (deferral OFF).
    // The irq-suspend-timeout has no netdev-wide sysfs (netlink/per-NAPI only)
    // and is applied in the NAPI_SET block.
    napi::applyDeferralSysfs(m_interface, m_napiDefer, m_napiGro, "AFXDP");

    fmt::print(stderr, "[AFXDP] {}: NAPI sysfs napi_defer_hard_irqs={} gro_flush_timeout={}ns ({})\n",
               m_interface, m_napiDefer, m_napiGro,
               m_enableIrqSuspend ? "irq-suspend regime" : (m_enableDeferral ? "deferral" : "deferral OFF"));
}

template <AFXDPMode M, std::uint32_t NumRxFrames, std::uint32_t NumTxFrames, std::uint32_t FrameSize,
          bool NeedWakeup>
void AFXDP<M, NumRxFrames, NumTxFrames, FrameSize, NeedWakeup>::shutdown() noexcept {
    if (m_xsk.xsk) {
        xdp_statistics stats{};
        socklen_t      slen = sizeof(stats);
        if (::getsockopt(m_fd, SOL_XDP, XDP_STATISTICS, &stats, &slen) == 0) {
            fmt::print(stderr,
                       "[XSK stats] rx_dropped={} rx_invalid_descs={} rx_ring_full={} "
                       "rx_fill_empty={} tx_invalid={} tx_ring_empty={}\n",
                       stats.rx_dropped, stats.rx_invalid_descs, stats.rx_ring_full,
                       stats.rx_fill_ring_empty_descs, stats.tx_invalid_descs, stats.tx_ring_empty_descs);
        }
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

template <AFXDPMode M, std::uint32_t NumRxFrames, std::uint32_t NumTxFrames, std::uint32_t FrameSize,
          bool NeedWakeup>
int AFXDP<M, NumRxFrames, NumTxFrames, FrameSize, NeedWakeup>::fd() const noexcept {
    return m_fd;
}

template <AFXDPMode M, std::uint32_t NumRxFrames, std::uint32_t NumTxFrames, std::uint32_t FrameSize,
          bool NeedWakeup>
bool AFXDP<M, NumRxFrames, NumTxFrames, FrameSize, NeedWakeup>::isCopyMode() const noexcept {
    return m_copyMode;
}

template <AFXDPMode M, std::uint32_t NumRxFrames, std::uint32_t NumTxFrames, std::uint32_t FrameSize,
          bool NeedWakeup>
inline std::span<const std::uint8_t>
AFXDP<M, NumRxFrames, NumTxFrames, FrameSize, NeedWakeup>::tryReceive() noexcept
    requires (M != AFXDPMode::TxOnly)
{
    __u32              idx  = 0;
    const unsigned int rcvd = xsk_ring_cons__peek(&m_xsk.rx, 1, &idx);
    if (!rcvd) {
        ::recvfrom(m_fd, nullptr, 0, MSG_DONTWAIT, nullptr, nullptr);
        return {};
    }
    if (xsk_ring_prod__reserve(&m_umem.fq, 1, &m_pendingFqIdx) != rcvd) [[unlikely]] {
        fmt::print(stderr, "[AFXDP] FILL reserve failed at one packet in flight — "
                           "kernel ring desync/deadlock; aborting\n");
        std::abort();
    }

    const xdp_desc* desc = xsk_ring_cons__rx_desc(&m_xsk.rx, idx);
    m_pendingRxAddr      = desc->addr;
    return {static_cast<const std::uint8_t*>(xsk_umem__get_data(m_umem.buffer, m_pendingRxAddr)), desc->len};
}

template <AFXDPMode M, std::uint32_t NumRxFrames, std::uint32_t NumTxFrames, std::uint32_t FrameSize,
          bool NeedWakeup>
inline void AFXDP<M, NumRxFrames, NumTxFrames, FrameSize, NeedWakeup>::release() noexcept
    requires (M != AFXDPMode::TxOnly)
{
    // Tail of xdpsock rx_drop(): recycle the consumed frame into the FILL slot
    // reserved in tryReceive(), submit it, then release the RX slot.
    *xsk_ring_prod__fill_addr(&m_umem.fq, m_pendingFqIdx) = m_pendingRxAddr;
    xsk_ring_prod__submit(&m_umem.fq, 1);
    xsk_ring_cons__release(&m_xsk.rx, 1);
}

template <AFXDPMode M, std::uint32_t NumRxFrames, std::uint32_t NumTxFrames, std::uint32_t FrameSize,
          bool NeedWakeup>
inline std::uint8_t* AFXDP<M, NumRxFrames, NumTxFrames, FrameSize, NeedWakeup>::acquire(
    std::uint32_t frameLen) noexcept
    requires (M != AFXDPMode::RxOnly)
{
    m_pendingTxAddr = static_cast<std::uint64_t>(m_txFrameNb) * FrameSize;
    m_pendingTxLen  = frameLen;
    return static_cast<std::uint8_t*>(xsk_umem__get_data(m_umem.buffer, m_pendingTxAddr));
}

template <AFXDPMode M, std::uint32_t NumRxFrames, std::uint32_t NumTxFrames, std::uint32_t FrameSize,
          bool NeedWakeup>
inline void AFXDP<M, NumRxFrames, NumTxFrames, FrameSize, NeedWakeup>::commit() noexcept
    requires (M != AFXDPMode::RxOnly)
{
    __u32 idx = 0;
    while (xsk_ring_prod__reserve(&m_xsk.tx, 1, &idx) != 1) [[unlikely]] {
        completeTx();
    }
    xdp_desc* d = xsk_ring_prod__tx_desc(&m_xsk.tx, idx);
    d->addr     = m_pendingTxAddr;
    d->len      = m_pendingTxLen;
    d->options  = 0;
    xsk_ring_prod__submit(&m_xsk.tx, 1);
    m_xsk.outstanding_tx++;
    m_txFrameNb = (m_txFrameNb + 1) & TX_POOL_MASK;

    completeTx();
}

template <AFXDPMode M, std::uint32_t NumRxFrames, std::uint32_t NumTxFrames, std::uint32_t FrameSize,
          bool NeedWakeup>
inline void AFXDP<M, NumRxFrames, NumTxFrames, FrameSize, NeedWakeup>::completeTx() noexcept
    requires (M != AFXDPMode::RxOnly)
{
    if (!m_xsk.outstanding_tx) {
        return;
    }

    if constexpr (NeedWakeup) {
        if (xsk_ring_prod__needs_wakeup(&m_xsk.tx)) {
            kickTx();
        }
    } else {
        kickTx();
    }

    __u32 idx = 0;
    if (const unsigned int rcvd = xsk_ring_cons__peek(&m_umem.cq, BATCH_SIZE, &idx)) {
        xsk_ring_cons__release(&m_umem.cq, rcvd);
        m_xsk.outstanding_tx -= rcvd;
    }
}

template <AFXDPMode M, std::uint32_t NumRxFrames, std::uint32_t NumTxFrames, std::uint32_t FrameSize,
          bool NeedWakeup>
inline bool AFXDP<M, NumRxFrames, NumTxFrames, FrameSize, NeedWakeup>::send(
    std::span<const std::uint8_t> frame) noexcept
    requires (M != AFXDPMode::RxOnly)
{
    auto* dst = acquire(static_cast<std::uint32_t>(frame.size()));
    if (!dst) [[unlikely]] {
        return false;
    }
    std::memcpy(dst, frame.data(), frame.size());
    commit();
    return true;
}

template <AFXDPMode M, std::uint32_t NumRxFrames, std::uint32_t NumTxFrames, std::uint32_t FrameSize,
          bool NeedWakeup>
void AFXDP<M, NumRxFrames, NumTxFrames, FrameSize, NeedWakeup>::prefillRing(
    std::span<const std::uint8_t> frameTemplate) noexcept
    requires (M != AFXDPMode::RxOnly)
{
    // A TX frame owns exactly [frameNb * FrameSize, +FrameSize) — the same chunk
    // acquire() hands out. A template larger than the chunk would run into the next
    // frame and corrupt every one of them, so refuse it here rather than transmit
    // garbage (same treatment as a mis-sized FILL ring in xskPopulateFillRing).
    if (frameTemplate.size() > FrameSize) {
        fmt::print(stderr, "[AFXDP] prefillRing: template {}B exceeds frame size {}B — aborting\n",
                   frameTemplate.size(), FrameSize);
        std::abort();
    }
    for (std::uint32_t i = 0; i < TX_POOL_FRAMES; i++) {
        const std::uint64_t addr = static_cast<std::uint64_t>(i) * FrameSize;
        std::memcpy(m_umem.buffer + addr, frameTemplate.data(), frameTemplate.size());
    }
}

template <AFXDPMode M, std::uint32_t NumRxFrames, std::uint32_t NumTxFrames, std::uint32_t FrameSize,
          bool NeedWakeup>
void AFXDP<M, NumRxFrames, NumTxFrames, FrameSize, NeedWakeup>::steerAllTrafficToQueue() noexcept {
    if (m_queueId != 0) {
        fmt::print(stderr,
                   "[AFXDP] WARN: queue {} != 0 — skipping combined=1 steering "
                   "(it routes all RX to queue 0); steer RSS to queue {} manually\n",
                   m_queueId, m_queueId);
        return;
    }
    if (napi::ensureCombinedOne(m_interface, "AFXDP") == napi::CombinedResult::Changed) {
        waitForCarrier();
    }
}

template <AFXDPMode M, std::uint32_t NumRxFrames, std::uint32_t NumTxFrames, std::uint32_t FrameSize,
          bool NeedWakeup>
void AFXDP<M, NumRxFrames, NumTxFrames, FrameSize, NeedWakeup>::waitForCarrier() noexcept {
    const timespec settle{.tv_sec = 0, .tv_nsec = 500'000'000};
    ::nanosleep(&settle, nullptr);

    char path[64];
    std::snprintf(path, sizeof(path), "/sys/class/net/%s/carrier", m_interface);

    const timespec tick{.tv_sec = 0, .tv_nsec = 100'000'000};
    for (int i = 0; i < 60; i++) {
        const int cfd = ::open(path, O_RDONLY);
        if (cfd >= 0) {
            char          c = 0;
            const ssize_t n = ::read(cfd, &c, 1);
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

template <AFXDPMode M, std::uint32_t NumRxFrames, std::uint32_t NumTxFrames, std::uint32_t FrameSize,
          bool NeedWakeup>
void AFXDP<M, NumRxFrames, NumTxFrames, FrameSize, NeedWakeup>::xskConfigureUmem() {
    const std::size_t umemSize = static_cast<std::size_t>(NUM_FRAMES) * FrameSize;
    m_umem.buffer              = static_cast<std::uint8_t*>(
        ::mmap(nullptr, umemSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    if (m_umem.buffer == MAP_FAILED) {
        fmt::print(stderr, "[AFXDP] mmap failed: {}\n", std::strerror(errno));
        m_umem.buffer = nullptr;
        std::abort();
    }

    xsk_umem_config umem_cfg{};
    umem_cfg.fill_size      = FILL_SIZE;
    umem_cfg.comp_size      = COMP_SIZE;
    umem_cfg.frame_size     = FrameSize;
    umem_cfg.frame_headroom = XSK_UMEM__DEFAULT_FRAME_HEADROOM;

    if (const int ret = xsk_umem__create(&m_umem.umem, m_umem.buffer, umemSize, &m_umem.fq, &m_umem.cq,
                                         &umem_cfg)) {
        fmt::print(stderr, "[AFXDP] xsk_umem__create failed: {}\n", std::strerror(-ret));
        std::abort();
    }
}

template <AFXDPMode M, std::uint32_t NumRxFrames, std::uint32_t NumTxFrames, std::uint32_t FrameSize,
          bool NeedWakeup>
void AFXDP<M, NumRxFrames, NumTxFrames, FrameSize, NeedWakeup>::xskPopulateFillRing() {
    __u32        idx = 0;
    unsigned int got = xsk_ring_prod__reserve(&m_umem.fq, RX_POOL_FRAMES, &idx);
    if (got != RX_POOL_FRAMES) {
        // Short FILL reserve at init = mis-sized ring, a fatal setup error (xdpsock
        // xsk_populate_fill_ring exit_with_error). Abort rather than run half-armed.
        fmt::print(stderr, "[AFXDP] FILL reserve {} != {} — aborting\n", got, RX_POOL_FRAMES);
        std::abort();
    }
    for (std::uint32_t i = 0; i < got; i++) {
        *xsk_ring_prod__fill_addr(&m_umem.fq, idx++) = static_cast<std::uint64_t>(TX_POOL_FRAMES + i) *
                                                       FrameSize;
    }
    xsk_ring_prod__submit(&m_umem.fq, got);
}

template <AFXDPMode M, std::uint32_t NumRxFrames, std::uint32_t NumTxFrames, std::uint32_t FrameSize,
          bool NeedWakeup>
void AFXDP<M, NumRxFrames, NumTxFrames, FrameSize, NeedWakeup>::xskConfigureSocket() {
    std::uint32_t bindFlags = XDP_USE_NEED_WAKEUP;
    if constexpr (!NeedWakeup) {
        bindFlags &= ~static_cast<std::uint32_t>(XDP_USE_NEED_WAKEUP);
    }

    xsk_socket_config sock_cfg{};
    sock_cfg.rx_size      = RX_SIZE;
    sock_cfg.tx_size      = TX_SIZE;
    sock_cfg.libxdp_flags = m_customBpf ? XSK_LIBXDP_FLAGS__INHIBIT_PROG_LOAD : 0;

    xsk_ring_cons* rxp = HAS_RX ? &m_xsk.rx : nullptr;
    xsk_ring_prod* txp = HAS_TX ? &m_xsk.tx : nullptr;

    m_bindMode = "generic";
    for (auto [xdpFlags, zc, label] : {std::tuple{XDP_FLAGS_DRV_MODE, XDP_ZEROCOPY, "zero-copy"},
                                       std::tuple{XDP_FLAGS_DRV_MODE, XDP_COPY, "native-copy"},
                                       std::tuple{XDP_FLAGS_SKB_MODE, XDP_COPY, "generic"}}) {
        sock_cfg.xdp_flags  = xdpFlags;
        sock_cfg.bind_flags = static_cast<__u16>(bindFlags | zc);

        if (xsk_socket__create(&m_xsk.xsk, m_interface, m_queueId, m_umem.umem, rxp, txp, &sock_cfg) == 0) {
            m_bindMode = label;
            m_copyMode = (zc == XDP_COPY);
            break;
        }
    }

    if (!m_xsk.xsk) {
        fmt::print(stderr, "[AFXDP] all socket modes failed\n");
        std::abort();
    }
    m_fd = xsk_socket__fd(m_xsk.xsk);   // needed by applySetSockOpt/verifyConfig/map insert

    fmt::print(stderr, "[AFXDP] {} | queue={} frames={} (tx={} rx={})\n", m_bindMode, m_queueId, NUM_FRAMES,
               TX_POOL_FRAMES, RX_POOL_FRAMES);
}

template <AFXDPMode M, std::uint32_t NumRxFrames, std::uint32_t NumTxFrames, std::uint32_t FrameSize,
          bool NeedWakeup>
void AFXDP<M, NumRxFrames, NumTxFrames, FrameSize, NeedWakeup>::applySetSockOpt() const {
    int on = 1;
    if (::setsockopt(m_fd, SOL_SOCKET, SO_PREFER_BUSY_POLL, &on, sizeof(on)) < 0) {
        fmt::print(stderr, "[AFXDP] WARN: SO_PREFER_BUSY_POLL: {}\n", std::strerror(errno));
    }
    int usecs = 20;
    if (::setsockopt(m_fd, SOL_SOCKET, SO_BUSY_POLL, &usecs, sizeof(usecs)) < 0) {
        fmt::print(stderr, "[AFXDP] WARN: SO_BUSY_POLL: {}\n", std::strerror(errno));
    }
    int budget = static_cast<int>(BATCH_SIZE);
    if (::setsockopt(m_fd, SOL_SOCKET, SO_BUSY_POLL_BUDGET, &budget, sizeof(budget)) < 0) {
        fmt::print(stderr, "[AFXDP] WARN: SO_BUSY_POLL_BUDGET: {}\n", std::strerror(errno));
    }

    // READBACK: a clean setsockopt only means the kernel parsed the option;
    // ask what values the socket is actually holding (napi::readXskModes;
    // SO_BUSY_POLL_BUDGET is SET-ONLY in the kernel and cannot be read back).
    const napi::XskModes rb = napi::readXskModes(m_fd);
    if (rb.preferBusyPoll == on && rb.busyPollUs == usecs) {
        fmt::print(stderr,
                   "[AFXDP] kernel confirms: busy-poll ACTIVE "
                   "(prefer={} usecs={}; budget={} write-only, "
                   "set call returned clean)\n",
                   rb.preferBusyPoll, rb.busyPollUs, budget);
    } else {
        fmt::print(stderr,
                   "[AFXDP] WARN: busy-poll readback MISMATCH — wrote "
                   "prefer={} usecs={}, kernel holds prefer={} usecs={}\n",
                   on, usecs, rb.preferBusyPoll, rb.busyPollUs);
    }
}

template <AFXDPMode M, std::uint32_t NumRxFrames, std::uint32_t NumTxFrames, std::uint32_t FrameSize,
          bool NeedWakeup>
void AFXDP<M, NumRxFrames, NumTxFrames, FrameSize, NeedWakeup>::enterXsksIntoMap() {
    const bpf_object* bpf_obj = xdp_program__bpf_obj(m_xdpProg);
    const bpf_map*    map     = bpf_object__find_map_by_name(bpf_obj, "xsks_map");
    if (map) {
        const int xsks_map_fd = bpf_map__fd(map);
        int       key         = static_cast<int>(m_queueId);
        if (bpf_map_update_elem(xsks_map_fd, &key, &m_fd, 0) != 0) {
            fmt::print(stderr, "[AFXDP] WARN: XSKMAP insert failed: {}\n", std::strerror(errno));
        } else {
            fmt::print(stderr, "[AFXDP] XSKMAP populated (fd={} key={})\n", m_fd, key);
        }
    } else {
        fmt::print(stderr, "[AFXDP] WARN: xsks_map not found in BPF\n");
    }
}

template <AFXDPMode M, std::uint32_t NumRxFrames, std::uint32_t NumTxFrames, std::uint32_t FrameSize,
          bool NeedWakeup>
void AFXDP<M, NumRxFrames, NumTxFrames, FrameSize, NeedWakeup>::verifyConfig() {
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

    const bool           ladderZc = (std::strcmp(m_bindMode.c_str(), "zero-copy") == 0);
    bool                 zc       = !m_copyMode;
    const napi::XskModes modes    = napi::readXskModes(m_fd);
    if (modes.valid) {
        zc         = modes.zeroCopy;
        m_copyMode = !zc;   // the kernel's answer overrides the ladder's label
    } else {
        fmt::print(stderr,
                   "[AFXDP] WARN: getsockopt(XDP_OPTIONS): {} — cannot "
                   "verify zero-copy, trusting bind label '{}'\n",
                   std::strerror(errno), m_bindMode);
    }
    if (zc != ladderZc) {
        fmt::print(stderr,
                   "[AFXDP] WARN: bind accepted '{}' but kernel grant "
                   "says {} — readback wins\n",
                   m_bindMode, zc ? "ZERO-COPY" : "COPY");
    }

    const char* attach = "no XDP prog (TxOnly)";
    if constexpr (HAS_RX) {
        LIBBPF_OPTS(bpf_xdp_query_opts, qopts);
        if (bpf_xdp_query(m_ifindex, 0, &qopts) == 0) {
            switch (qopts.attach_mode) {
                case XDP_ATTACHED_DRV:
                    attach = "native (DRV)";
                    break;
                case XDP_ATTACHED_SKB:
                    attach = "generic (SKB)";
                    break;
                case XDP_ATTACHED_HW:
                    attach = "hw-offload";
                    break;
                case XDP_ATTACHED_MULTI:
                    attach = "multi";
                    break;
                case XDP_ATTACHED_NONE:
                    attach = "NONE (no prog?!)";
                    break;
                default:
                    attach = "unknown";
                    break;
            }
        } else {
            attach = "query FAILED";
        }
    }
    fmt::print(stderr,
               "[AFXDP] kernel confirms: {} | XDP attach: {} "
               "(XDP_OPTIONS=0x{:x})\n",
               zc ? "ZERO-COPY" : "COPY-mode", attach, modes.xdpOptionsFlags);

    if constexpr (NeedWakeup) {
        xdp_mmap_offsets moff{};
        socklen_t        mlen = sizeof(moff);
        if (::getsockopt(m_fd, SOL_XDP, XDP_MMAP_OFFSETS, &moff, &mlen) == 0 && mlen == sizeof(moff)) {
            bool fillNw = false;
            bool txNw   = false;
            if constexpr (HAS_RX) {
                fillNw = xsk_ring_prod__needs_wakeup(&m_umem.fq) != 0;
            }
            if constexpr (HAS_TX) {
                txNw = xsk_ring_prod__needs_wakeup(&m_xsk.tx) != 0;
            }
            fmt::print(stderr,
                       "[AFXDP] kernel confirms: NEED_WAKEUP live "
                       "(ring flags now: fill={} tx={})\n",
                       fillNw, txNw);
        } else {
            fmt::print(stderr,
                       "[AFXDP] WARN: NEED_WAKEUP NOT confirmed — kernel "
                       "returned {}B of {}B xdp_mmap_offsets (pre-5.4 ring "
                       "layout, no flags words)\n",
                       static_cast<std::size_t>(mlen), sizeof(moff));
        }
    }

    // Per-NAPI deferral / irq-suspend via netlink. ALL modes: busy-poll TX needs it
    // too, and irq-suspend-timeout has no sysfs (netlink-only). On a TX-only socket
    // SO_INCOMING_NAPI_ID is the bound queue's NAPI (shared RX/TX pair on i40e/igc);
    // if it reads 0 we skip and rely on the netdev-wide sysfs write above.
    {
        std::uint32_t napiId = 0;
        socklen_t     idLen  = sizeof(napiId);
        if (::getsockopt(m_fd, SOL_SOCKET, SO_INCOMING_NAPI_ID, &napiId, &idLen) == 0 && napiId != 0) {
            const std::uint32_t defer = m_napiDefer;
            const std::uint64_t gro   = m_napiGro;
            const std::uint64_t susp  = m_napiSusp;
            const bool          ok    = napi::setBusyPoll(napiId, defer, gro, susp);
            fmt::print(stderr,
                       "[AFXDP] NAPI {}: defer-hard-irqs={} gro-flush={}ns irq-suspend={}ns ({}) => {}\n",
                       napiId, defer, gro, susp,
                       m_enableIrqSuspend ? "irq-suspend regime"
                                          : (m_enableDeferral ? "deferral busy-poll" : "deferral OFF"),
                       ok ? "applied" : "FAILED");
            // READBACK via NETDEV_CMD_NAPI_GET — the netlink ACK above only says the
            // kernel accepted the message; this asks what the NAPI actually runs with.
            if (ok) {
                const napi::NapiParams rb = napi::getBusyPoll(napiId);
                if (rb.valid && rb.deferHardIrqs == defer && rb.groFlushNs == gro &&
                    rb.irqSuspendNs == susp) {
                    fmt::print(stderr, "[AFXDP] kernel confirms: NAPI {} params VERIFIED\n", napiId);
                } else if (rb.valid) {
                    fmt::print(stderr,
                               "[AFXDP] WARN: NAPI {} readback MISMATCH — kernel "
                               "holds defer={} gro={}ns irq-suspend={}ns\n",
                               napiId, rb.deferHardIrqs, rb.groFlushNs, rb.irqSuspendNs);
                } else {
                    fmt::print(stderr,
                               "[AFXDP] WARN: NAPI {} readback unavailable — set "
                               "not independently confirmed\n",
                               napiId);
                }
            }
        } else {
            fmt::print(stderr,
                       "[AFXDP] WARN: SO_INCOMING_NAPI_ID unavailable "
                       "(napi_id={}) — NAPI config not applied\n",
                       napiId);
        }
    }
}

template <AFXDPMode M, std::uint32_t NumRxFrames, std::uint32_t NumTxFrames, std::uint32_t FrameSize,
          bool NeedWakeup>
inline void AFXDP<M, NumRxFrames, NumTxFrames, FrameSize, NeedWakeup>::kickTx() noexcept
    requires (M != AFXDPMode::RxOnly)
{
    // Faithful xdpsock kick_tx(): one sendto wakes the kernel TX NAPI. Success or
    // a benign "busy/again" errno is fine; anything else is a real fault.
    const ssize_t ret = ::sendto(m_fd, nullptr, 0, MSG_DONTWAIT, nullptr, 0);
    if (ret >= 0 || errno == ENOBUFS || errno == EAGAIN || errno == EBUSY || errno == ENETDOWN) {
        return;
    }
    fmt::println(stderr, "[AFXDP] kickTx sendto failed: {}", std::strerror(errno));
    std::abort();
}

template <AFXDPMode M, std::uint32_t NumRxFrames, std::uint32_t NumTxFrames, std::uint32_t FrameSize,
          bool NeedWakeup>
void AFXDP<M, NumRxFrames, NumTxFrames, FrameSize, NeedWakeup>::clearXdpProgram() noexcept {
    // Detach whatever XDP program is on the interface so a leftover
    // dispatcher/prog can't block our attach or mis-route RX. Shared rtnetlink
    // implementation (napi::) — same sweep the DPDK net_af_xdp path runs.
    napi::detachXdpProgram(m_interface, "AFXDP");
}

template <AFXDPMode M, std::uint32_t NumRxFrames, std::uint32_t NumTxFrames, std::uint32_t FrameSize,
          bool NeedWakeup>
void AFXDP<M, NumRxFrames, NumTxFrames, FrameSize, NeedWakeup>::loadXdpProgram(const char* path) {
    m_xdpProg = xdp_program__open_file(path, nullptr, nullptr);
    if (const long err = libxdp_get_error(m_xdpProg)) {   // libxdp returns a long errno
        fmt::print(stderr, "[AFXDP] BPF load failed ({}): {}\n", path, std::strerror(static_cast<int>(-err)));
        m_xdpProg = nullptr;
        return;
    }
    if (const int err = xdp_program__attach(m_xdpProg, m_ifindex, XDP_MODE_NATIVE, 0)) {
        fmt::print(stderr, "[AFXDP] BPF attach failed: {}\n", std::strerror(-err));
        xdp_program__close(m_xdpProg);
        m_xdpProg = nullptr;
        return;
    }
    m_customBpf = true;
    fmt::print(stderr, "[AFXDP] custom BPF loaded + attached: {}\n", path);
}

template <AFXDPMode M, std::uint32_t NumRxFrames, std::uint32_t NumTxFrames, std::uint32_t FrameSize,
          bool NeedWakeup>
void AFXDP<M, NumRxFrames, NumTxFrames, FrameSize, NeedWakeup>::moveFrom(AFXDP& o) noexcept {
    // EVERY member must be carried across. m_pendingFqIdx in particular: it is the
    // FILL slot tryReceive() reserved and release() writes to, so dropping it makes
    // a move between those two calls recycle the frame into slot 0 instead — silent
    // ring corruption. The NAPI regime fields must come too or a moved-from socket
    // misreports the deferral it is actually running.
    m_interface        = o.m_interface;
    m_queueId          = o.m_queueId;
    m_bpfProgPath      = o.m_bpfProgPath;
    m_bindMode         = std::move(o.m_bindMode);
    m_umem             = o.m_umem;
    m_xsk              = o.m_xsk;
    m_xdpProg          = std::exchange(o.m_xdpProg, nullptr);
    m_ifindex          = std::exchange(o.m_ifindex, 0);
    m_fd               = std::exchange(o.m_fd, -1);
    m_copyMode         = o.m_copyMode;
    m_customBpf        = o.m_customBpf;
    m_enableDeferral   = o.m_enableDeferral;
    m_enableIrqSuspend = o.m_enableIrqSuspend;
    m_napiDefer        = o.m_napiDefer;
    m_napiGro          = o.m_napiGro;
    m_napiSusp         = o.m_napiSusp;
    m_pendingRxAddr    = o.m_pendingRxAddr;
    m_pendingFqIdx     = o.m_pendingFqIdx;
    m_pendingTxAddr    = o.m_pendingTxAddr;
    m_pendingTxLen     = o.m_pendingTxLen;
    m_txFrameNb        = o.m_txFrameNb;
    o.m_umem.umem      = nullptr;
    o.m_umem.buffer    = nullptr;
    o.m_xsk.xsk        = nullptr;
}

#endif   // ABTRDA3_AFXDP_HPP
