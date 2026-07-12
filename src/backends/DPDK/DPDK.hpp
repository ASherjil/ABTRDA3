#pragma once

#include "DPDKEal.hpp"
#include "RxFrame.hpp"
#include "PciHelpers.hpp"
#include "NapiConfig.hpp"
#include <rte_cycles.h>
#include <rte_errno.h>
#include <rte_ethdev.h>
#include <rte_mbuf.h>
#include <rte_mempool.h>
#include <rte_ether.h>
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <utility>

// =============================================================================
// DPDK — one PMD-AGNOSTIC transport for every NIC, through rte_ethdev. Satisfies
// the TxRing/RxRing concepts (tryReceive/release + acquire/commit/send); the hot
// path is identical across PMDs. Everything fixable at compile time is a template
// param; only ifname/lcore/driver are runtime (from the TOML).
//
// NO DRIVER-SPECIFIC LOGIC LIVES HERE. Which NIC needs which knob (link speed,
// symmetric queues, devargs, TX inline reuse) and the post-start BAR0 register
// fixes (i40e ITR clear, igc EEE) are decided in TransportDispatch, which knows
// the TOML driver name. Driver traps: docs/Known_driver_issues.md.
//
// Binding models, chosen by EXPLICIT per-role TOML flags (never by sniffing the
// driver name), both false = default:
//   default          PCI pass-through — prepare() unbinds the kernel driver onto
//                    vfio-pci (needs intel_iommu=on iommu=pt). BIND-ONCE: shutdown()
//                    does NOT restore the kernel driver (it would hang; KDI §4.3).
//   bifurcated=true  mlx5-class — stays on the kernel driver, DPDK drives alongside.
//   afxdpPMD=true    DPDK-over-AF_XDP via a net_af_xdp vdev; the port is preconditioned
//                    and its granted modes (ZC / busy-poll / deferral) read back.
//
// prepare()/init() are SPLIT because EAL is process-global: every port must be
// registered before the first init() triggers the one rte_eal_init (KDI §4.1).
// init(doWaitLink=false) starts a port without waiting — a loopback link only comes
// up once BOTH PHYs are up, so dispatch starts both, then waitLink()s both.
// RING SIZES: 256 (not 1024) — at one frame in flight the rings are pure cache
// footprint; 256 keeps CQ+SQ hot in L1/L2. adjust_nb_rx_tx_desc rounds up if a PMD
// needs more. Promiscuous mode is NOT enabled (both ends know the peer MAC).
// =============================================================================

enum class DpdkMode : std::uint8_t {
  RxOnly,
  TxOnly,
  RxTx
};

template<DpdkMode M, std::uint16_t QueueId = 0, std::uint16_t NbRxDesc = 256, std::uint16_t NbTxDesc = 256, std::uint16_t BurstSize = 32, std::uint32_t NumMbufs = 8191, std::uint16_t MaxFrame = RTE_MBUF_DEFAULT_DATAROOM>
class DPDK : private DPDKEal {
  static_assert(BurstSize >= 1, "BurstSize must be >= 1");
  static_assert(NumMbufs > BurstSize, "NumMbufs must exceed BurstSize");

  static constexpr bool          HAS_RX          = (M == DpdkMode::RxOnly || M == DpdkMode::RxTx);
  static constexpr bool          HAS_TX          = (M == DpdkMode::TxOnly || M == DpdkMode::RxTx);
  static constexpr std::uint32_t MBUF_CACHE      = 250;
  // TX inline-reuse (setTxInlineReuse): the PMD still decrements the reused mbuf's
  // refcnt once per send when it reaps completions, so top it back up whenever it dips
  // below REFCNT_LOW (decrements lag sends by <= NbTxDesc << REFCNT_LOW => never 0).
  // Gated to frames <= INLINE_SAFE_LEN (mlx5 default inlen_send = 290B; 128 = margin).
  static constexpr std::uint32_t INLINE_SAFE_LEN = 128;
  static constexpr std::uint16_t REFCNT_LOW      = 0x2000;
  static constexpr std::uint16_t REFCNT_TOPUP    = 0x4000;

public:
  DPDK(std::string_view ifname, int lcore, std::string_view driver) noexcept;
  ~DPDK();

  DPDK(const DPDK&)            = delete;
  DPDK& operator=(const DPDK&) = delete;
  DPDK(DPDK&& o) noexcept;
  DPDK& operator=(DPDK&& o) noexcept;

  [[nodiscard]] bool prepare(bool bifurcated = false, bool afxdpPMD = false) noexcept;
  [[nodiscard]] bool init(bool doWaitLink = true, bool bifurcated = false, bool afxdpPMD = false, bool applyDeferral = false) noexcept;
  [[nodiscard]] bool waitLink() noexcept;
  void shutdown() noexcept;

  // Generic pre-init knobs — this class is PMD-agnostic; WHICH driver needs which
  // knob is decided in TransportDispatch (it knows the TOML driver name).
  // All four must be called BEFORE prepare()/init().
  void setLinkSpeeds(std::uint32_t speedsMask) noexcept;   // RTE_ETH_LINK_SPEED_* mask; 0 = full autoneg
  void setSymmetricQueues(bool on) noexcept;               // force nb_rxq == nb_txq == 1
  void setNbRxDesc(std::uint16_t n) noexcept;              // override NbRxDesc (shrinks the PMD refill burst)
  void setDevargs(std::string_view devargs) noexcept;      // "key=val,..." appended to the -a entry
  void setTxInlineReuse(bool on) noexcept;                 // single-mbuf full-inline TX fast path

  [[nodiscard]] std::array<std::uint8_t, 6> macAddress() const noexcept;

  void prefillRing(std::span<const std::uint8_t> frameTemplate) noexcept
      requires (M == DpdkMode::TxOnly || M == DpdkMode::RxTx);

  [[nodiscard, gnu::always_inline, gnu::hot]]
  inline RxFrame tryReceive() noexcept
      requires (M == DpdkMode::RxOnly || M == DpdkMode::RxTx);

  [[gnu::always_inline, gnu::hot]]
  inline void release() noexcept
      requires (M == DpdkMode::RxOnly || M == DpdkMode::RxTx);

  [[nodiscard, gnu::always_inline, gnu::hot]]
  inline std::uint8_t* acquire(std::uint32_t frameLen) noexcept
      requires (M == DpdkMode::TxOnly || M == DpdkMode::RxTx);

  [[gnu::always_inline, gnu::hot]]
  inline void commit() noexcept
      requires (M == DpdkMode::TxOnly || M == DpdkMode::RxTx);

  [[nodiscard, gnu::always_inline, gnu::hot]]
  inline bool send(std::span<const std::uint8_t> frame) noexcept
      requires (M == DpdkMode::TxOnly || M == DpdkMode::RxTx);

private:
  std::string                        m_ifname;
  std::string                        m_driver;
  int                                m_lcore{};
  std::string                        m_pci;

  std::uint16_t                      m_port{0};
  rte_mempool*                       m_pool{nullptr};
  std::array<std::uint8_t, 6>        m_mac{};

  std::array<std::uint8_t, MaxFrame> m_txTemplate{};
  std::uint16_t                      m_txTemplateLen{0};

  rte_mbuf*                          m_rxBurst[BurstSize]{};
  std::uint16_t                      m_rxCount{0};
  std::uint16_t                      m_rxIdx{0};
  rte_mbuf*                          m_pendingRx{nullptr};
  rte_mbuf*                          m_pendingTx{nullptr};

  bool                               m_prepared{false};
  bool                               m_started{false};

  bool                               m_bifurcated{false};
  bool                               m_afxdpPmd{false};
  bool                               m_applyDeferral{false};
  std::uint32_t                      m_linkSpeeds{0};
  bool                               m_symmetricQueues{false};
  std::uint16_t                      m_nbRxDesc{NbRxDesc};
  std::string                        m_devargs;

  rte_mbuf*                          m_txReuse{nullptr};
  bool                               m_txReuseInline{false};
};

// =============================================================================

template<DpdkMode M, std::uint16_t QueueId, std::uint16_t NbRxDesc, std::uint16_t NbTxDesc, std::uint16_t BurstSize, std::uint32_t NumMbufs, std::uint16_t MaxFrame>
DPDK<M, QueueId, NbRxDesc, NbTxDesc, BurstSize, NumMbufs, MaxFrame>::DPDK(std::string_view ifname, int lcore, std::string_view driver) noexcept
  : m_ifname{ifname}, m_driver{driver}, m_lcore{lcore} {}

template<DpdkMode M, std::uint16_t QueueId, std::uint16_t NbRxDesc, std::uint16_t NbTxDesc, std::uint16_t BurstSize, std::uint32_t NumMbufs, std::uint16_t MaxFrame>
DPDK<M, QueueId, NbRxDesc, NbTxDesc, BurstSize, NumMbufs, MaxFrame>::~DPDK() {
  shutdown();
}

template<DpdkMode M, std::uint16_t QueueId, std::uint16_t NbRxDesc, std::uint16_t NbTxDesc, std::uint16_t BurstSize, std::uint32_t NumMbufs, std::uint16_t MaxFrame>
DPDK<M, QueueId, NbRxDesc, NbTxDesc, BurstSize, NumMbufs, MaxFrame>::DPDK(DPDK&& o) noexcept
  : m_ifname{std::move(o.m_ifname)}, m_driver{std::move(o.m_driver)},
    m_lcore{o.m_lcore}, m_pci{std::move(o.m_pci)},
    m_port{o.m_port}, m_pool{std::exchange(o.m_pool, nullptr)}, m_mac{o.m_mac},
    m_txTemplate{o.m_txTemplate}, m_txTemplateLen{o.m_txTemplateLen},
    m_prepared{o.m_prepared}, m_started{std::exchange(o.m_started, false)},
    m_bifurcated{o.m_bifurcated}, m_afxdpPmd{o.m_afxdpPmd},
    m_applyDeferral{o.m_applyDeferral},
    m_linkSpeeds{o.m_linkSpeeds}, m_symmetricQueues{o.m_symmetricQueues}, m_nbRxDesc{o.m_nbRxDesc},
    m_devargs{std::move(o.m_devargs)},
    m_txReuse{std::exchange(o.m_txReuse, nullptr)},
    m_txReuseInline{o.m_txReuseInline} {}

template<DpdkMode M, std::uint16_t QueueId, std::uint16_t NbRxDesc, std::uint16_t NbTxDesc, std::uint16_t BurstSize, std::uint32_t NumMbufs, std::uint16_t MaxFrame>
DPDK<M, QueueId, NbRxDesc, NbTxDesc, BurstSize, NumMbufs, MaxFrame>& DPDK<M, QueueId, NbRxDesc, NbTxDesc, BurstSize, NumMbufs, MaxFrame>::operator=(DPDK&& o) noexcept {
  if (this != &o) {
    shutdown();
    m_ifname        = std::move(o.m_ifname);
    m_driver        = std::move(o.m_driver);
    m_lcore         = o.m_lcore;
    m_pci           = std::move(o.m_pci);
    m_port          = o.m_port;
    m_pool          = std::exchange(o.m_pool, nullptr);
    m_mac           = o.m_mac;
    m_txTemplate    = o.m_txTemplate;
    m_txTemplateLen = o.m_txTemplateLen;
    m_prepared      = o.m_prepared;
    m_started       = std::exchange(o.m_started, false);
    m_bifurcated      = o.m_bifurcated;
    m_afxdpPmd        = o.m_afxdpPmd;
    m_applyDeferral   = o.m_applyDeferral;
    m_linkSpeeds      = o.m_linkSpeeds;
    m_symmetricQueues = o.m_symmetricQueues;
    m_nbRxDesc        = o.m_nbRxDesc;
    m_devargs         = std::move(o.m_devargs);
    m_txReuse         = std::exchange(o.m_txReuse, nullptr);
    m_txReuseInline   = o.m_txReuseInline;
  }
  return *this;
}

template<DpdkMode M, std::uint16_t QueueId, std::uint16_t NbRxDesc, std::uint16_t NbTxDesc, std::uint16_t BurstSize, std::uint32_t NumMbufs, std::uint16_t MaxFrame>
bool DPDK<M, QueueId, NbRxDesc, NbTxDesc, BurstSize, NumMbufs, MaxFrame>::prepare(bool bifurcated, bool afxdpPMD) noexcept {
  if (m_prepared) return true;
  m_bifurcated = bifurcated;
  m_afxdpPmd   = afxdpPMD;

  // DPDK-over-AF_XDP: a net_af_xdp vdev on the kernel netdev — no BDF, no vfio bind.
  // Precondition the port exactly like the native AFXDP backend (a stale XDP program
  // blocks the PMD's attach; the PMD binds queue 0 only). mode=drv + busy_budget, no
  // force_copy => zero-copy negotiated; all of it VERIFIED by readback in init().
  if (m_afxdpPmd) {
    napi::detachXdpProgram(m_ifname.c_str(), "DPDK");
    napi::ensureCombinedOne(m_ifname.c_str(), "DPDK");   // link bounce absorbed by waitLink
    const std::string vname = "net_af_xdp" + std::to_string(vdevCount());
    addVdev(vname + ",iface=" + m_ifname +
            ",start_queue=0,queue_count=1,mode=drv,busy_budget=64");
    m_pci           = vname;   // init() resolves the port by this name
    m_txReuseInline = false;   // full-inline reuse is N/A here
    m_prepared      = true;
    std::fprintf(stderr, "[DPDK] %s: AF_XDP PMD via vdev %s (busy-poll, zero-copy)\n",
                 m_ifname.c_str(), vname.c_str());
    return true;
  }

  m_pci = pci::resolveBdf(m_ifname);
  if (m_pci.empty()) m_pci = pci::bdfFromName(m_ifname);
  if (m_pci.empty()) {
    std::fprintf(stderr, "[DPDK] %s: cannot resolve PCI BDF (no netdev, and name is "
                         "not an enp<bus>s<dev>f<func> form)\n", m_ifname.c_str());
    return false;
  }

  if (m_bifurcated) {
    std::fprintf(stderr, "[DPDK] %s (%s): bifurcated — kept on kernel driver\n",
                 m_ifname.c_str(), m_pci.c_str());
  } else {
    if (!bindToVfio(m_pci)) {
      std::fprintf(stderr, "[DPDK] %s (%s): vfio-pci bind failed — IOMMU enabled "
                           "(intel_iommu=on iommu=pt) and vfio-pci loaded?\n",
                   m_ifname.c_str(), m_pci.c_str());
      return false;
    }
    std::fprintf(stderr, "[DPDK] %s (%s): unbound '%s' -> vfio-pci\n",
                 m_ifname.c_str(), m_pci.c_str(), m_driver.empty() ? "?" : m_driver.c_str());
  }

  // Any PMD devargs come from dispatch (setDevargs) — e.g. the mlx5 latency set.
  addAllowedBdf(m_devargs.empty() ? m_pci : m_pci + "," + m_devargs);
  m_prepared = true;
  return true;
}

template<DpdkMode M, std::uint16_t QueueId, std::uint16_t NbRxDesc, std::uint16_t NbTxDesc, std::uint16_t BurstSize, std::uint32_t NumMbufs, std::uint16_t MaxFrame>
bool DPDK<M, QueueId, NbRxDesc, NbTxDesc, BurstSize, NumMbufs, MaxFrame>::init(bool doWaitLink, bool bifurcated, bool afxdpPMD, bool applyDeferral) noexcept {
  m_applyDeferral = applyDeferral;
  if (!prepare(bifurcated, afxdpPMD)) return false;

  if (!ealInit(m_pci, m_lcore)) return false;

  if (rte_eth_dev_get_port_by_name(m_pci.c_str(), &m_port) != 0) {
    std::fprintf(stderr, "[DPDK] no DPDK port for %s (%s)\n", m_ifname.c_str(), m_pci.c_str());
    return false;
  }

  rte_eth_dev_info info{};
  if (rte_eth_dev_info_get(m_port, &info) != 0) {
    std::fprintf(stderr, "[DPDK] dev_info_get failed (port %u)\n", m_port);
    return false;
  }

  int sid = rte_eth_dev_socket_id(m_port);
  if (sid < 0) sid = static_cast<int>(rte_socket_id());

  char poolName[32];
  std::snprintf(poolName, sizeof(poolName), "mbufpool_%u", m_port);
  m_pool = rte_pktmbuf_pool_create(poolName, NumMbufs, MBUF_CACHE, 0,
                                   RTE_MBUF_DEFAULT_BUF_SIZE, sid);
  if (!m_pool) {
    std::fprintf(stderr, "[DPDK] mbuf pool create failed: %s\n", rte_strerror(rte_errno));
    return false;
  }

  rte_eth_conf conf{};
  conf.rxmode.mq_mode = RTE_ETH_MQ_RX_NONE;
  conf.txmode.mq_mode = RTE_ETH_MQ_TX_NONE;
  if (info.tx_offload_capa & RTE_ETH_TX_OFFLOAD_MBUF_FAST_FREE)
    conf.txmode.offloads |= RTE_ETH_TX_OFFLOAD_MBUF_FAST_FREE;
  if (m_linkSpeeds != 0) {
    conf.link_speeds = m_linkSpeeds;
    std::fprintf(stderr, "[DPDK] %s: autoneg advertisement restricted "
                         "(link_speeds mask 0x%x)\n", m_ifname.c_str(), m_linkSpeeds);
  }

  // The unused direction is configured only to satisfy the driver (AF_XDP PMD pairs
  // rx+tx per queue; setSymmetricQueues covers PMDs that break otherwise). The hot
  // path still drives one direction; m_pool above always backs the unused RX queue.
  const bool bothQueues = m_afxdpPmd || m_symmetricQueues;
  const std::uint16_t nb_rxq = (HAS_RX || bothQueues) ? 1 : 0;
  const std::uint16_t nb_txq = (HAS_TX || bothQueues) ? 1 : 0;
  if (rte_eth_dev_configure(m_port, nb_rxq, nb_txq, &conf) != 0) {
    std::fprintf(stderr, "[DPDK] dev_configure failed (port %u)\n", m_port);
    return false;
  }

  std::uint16_t nb_rxd = m_nbRxDesc, nb_txd = NbTxDesc;
  if (rte_eth_dev_adjust_nb_rx_tx_desc(m_port, &nb_rxd, &nb_txd) != 0) {
    std::fprintf(stderr, "[DPDK] adjust_nb_rx_tx_desc failed (port %u)\n", m_port);
    return false;
  }
  // Report what the PMD ACTUALLY took: adjust_nb_rx_tx_desc silently clamps to the PMD's
  // min/max, so a requested ring is not a granted one. RX size drives the in-poll mbuf refill
  // burst (mlx5: min(64, nb_rxd>>2)), which is a P99 lever — do not leave it unobservable.
  std::fprintf(stderr, "[DPDK] %s: rx_desc=%u (asked %u) tx_desc=%u\n",
               m_ifname.c_str(), nb_rxd, m_nbRxDesc, nb_txd);

  if (HAS_RX || bothQueues) {
    // PMD-default thresholds: audited optimal at one-in-flight on every campaign NIC
    // (never override rx wthresh to 0 on igc — it kills write-back; KDI §3.2).
    rte_eth_rxconf rxconf = info.default_rxconf;
    rxconf.offloads = conf.rxmode.offloads;
    if (rte_eth_rx_queue_setup(m_port, QueueId, nb_rxd, sid, &rxconf, m_pool) < 0) {
      std::fprintf(stderr, "[DPDK] rx_queue_setup failed\n");
      return false;
    }
  }
  if (HAS_TX || bothQueues) {
    rte_eth_txconf txconf = info.default_txconf;
    txconf.offloads = conf.txmode.offloads;
    if (rte_eth_tx_queue_setup(m_port, QueueId, nb_txd, sid, &txconf) < 0) {
      std::fprintf(stderr, "[DPDK] tx_queue_setup failed\n");
      return false;
    }
  }

  if (rte_eth_dev_start(m_port) < 0) {
    std::fprintf(stderr, "[DPDK] dev_start failed (port %u)\n", m_port);
    return false;
  }
  m_started = true;

  // NB: driver-specific post-start register fixes (i40e ITR clear, igc EEE) live in
  // TransportDispatch — pci::i40eClearItr / pci::igcDisableEee, called after init().

  // Apply (or deterministically ZERO) the NAPI deferral regime, then read back what
  // the kernel actually granted the PMD's XSK — zero-copy and busy-poll are negotiated
  // SILENTLY, so verify like a register write. Same napi:: helpers as native AF_XDP.
  if (m_afxdpPmd) {
    if (m_applyDeferral)
      napi::applyDeferralSysfs(m_ifname.c_str(), napi::kDeferHardIrqs,
                               napi::kGroFlushTimeoutNs, "DPDK");
    else
      napi::applyDeferralSysfs(m_ifname.c_str(), 0, 0, "DPDK");
    napi::printXskProcessValidation(m_ifname.c_str(), m_applyDeferral, "DPDK");
  }

  rte_eth_burst_mode bm{};
  if constexpr (M != DpdkMode::RxOnly)
    if (rte_eth_tx_burst_mode_get(m_port, QueueId, &bm) == 0)
      std::fprintf(stderr, "[DPDK] %s: tx burst mode: %s\n", m_ifname.c_str(), bm.info);
  if constexpr (M != DpdkMode::TxOnly)
    if (rte_eth_rx_burst_mode_get(m_port, QueueId, &bm) == 0)
      std::fprintf(stderr, "[DPDK] %s: rx burst mode: %s\n", m_ifname.c_str(), bm.info);

  rte_ether_addr mac{};
  rte_eth_macaddr_get(m_port, &mac);
  std::memcpy(m_mac.data(), mac.addr_bytes, 6);

  if (doWaitLink) return waitLink();
  return true;
}

template<DpdkMode M, std::uint16_t QueueId, std::uint16_t NbRxDesc, std::uint16_t NbTxDesc, std::uint16_t BurstSize, std::uint32_t NumMbufs, std::uint16_t MaxFrame>
bool DPDK<M, QueueId, NbRxDesc, NbTxDesc, BurstSize, NumMbufs, MaxFrame>::waitLink() noexcept {
  rte_eth_link link{};
  for (int i = 0; i < 200; ++i) {
    // A failed query leaves `link` stale — only trust link_status when the call succeeded.
    if (rte_eth_link_get_nowait(m_port, &link) == 0 && link.link_status == RTE_ETH_LINK_UP) break;
    if (i == 20 || (i > 20 && (i % 30) == 0)) {
      int e = rte_eth_dev_set_link_up(m_port);
      if (e != 0 && e != -ENOTSUP)
        std::fprintf(stderr, "[DPDK] %s: set_link_up returned %d\n", m_ifname.c_str(), e);
    }
    rte_delay_us_block(100'000);
  }
  std::fprintf(stderr,
      "[DPDK] %s port %u %s %u Mbps  MAC %02x:%02x:%02x:%02x:%02x:%02x\n",
      m_ifname.c_str(), m_port,
      link.link_status == RTE_ETH_LINK_UP ? "UP" : "DOWN", link.link_speed,
      m_mac[0], m_mac[1], m_mac[2], m_mac[3], m_mac[4], m_mac[5]);
  if (link.link_status != RTE_ETH_LINK_UP)
    std::fprintf(stderr, "[DPDK] %s: WARN link still DOWN after ~20s — "
                         "initial packets may be lost\n", m_ifname.c_str());
  return true;
}

template<DpdkMode M, std::uint16_t QueueId, std::uint16_t NbRxDesc, std::uint16_t NbTxDesc, std::uint16_t BurstSize, std::uint32_t NumMbufs, std::uint16_t MaxFrame>
void DPDK<M, QueueId, NbRxDesc, NbTxDesc, BurstSize, NumMbufs, MaxFrame>::shutdown() noexcept {
  if (!m_started) return;

  // Wire truth before teardown, independent of our own bookkeeping: TX opackets vs RX
  // ipackets vs the app's recorded count. (ipackets ~= recorded >> sent => the PMD
  // duplicates; ipackets ~= sent but recorded >> sent => we re-read buffers. imissed =
  // RX-ring overrun; rx_nombuf = mempool exhausted. On igc imissed always reads 0.)
  {
    rte_eth_stats st{};
    if (rte_eth_stats_get(m_port, &st) == 0) {
      std::fprintf(stderr,
          "[DPDK] %s STATS: opackets=%llu ipackets=%llu obytes=%llu ibytes=%llu "
          "imissed=%llu ierrors=%llu oerrors=%llu rx_nombuf=%llu\n",
          m_ifname.c_str(),
          static_cast<unsigned long long>(st.opackets),
          static_cast<unsigned long long>(st.ipackets),
          static_cast<unsigned long long>(st.obytes),
          static_cast<unsigned long long>(st.ibytes),
          static_cast<unsigned long long>(st.imissed),
          static_cast<unsigned long long>(st.ierrors),
          static_cast<unsigned long long>(st.oerrors),
          static_cast<unsigned long long>(st.rx_nombuf));
    }
  }

  if (m_pendingRx) {
    rte_pktmbuf_free(m_pendingRx);
    m_pendingRx = nullptr;
  }
  // dev_stop ONLY — no set_link_up, no dev_close, no kernel-driver restore (KDI §4.3).
  rte_eth_dev_stop(m_port);
  if constexpr (HAS_TX) {
    if (m_txReuse) {
      rte_mbuf_refcnt_set(m_txReuse, 1);   // PMD decrements are done; let the free land
      rte_pktmbuf_free(m_txReuse);
      m_txReuse = nullptr;
    }
  }
  m_started = false;
}

template<DpdkMode M, std::uint16_t QueueId, std::uint16_t NbRxDesc, std::uint16_t NbTxDesc, std::uint16_t BurstSize, std::uint32_t NumMbufs, std::uint16_t MaxFrame>
std::array<std::uint8_t, 6> DPDK<M, QueueId, NbRxDesc, NbTxDesc, BurstSize, NumMbufs, MaxFrame>::macAddress() const noexcept {
  return m_mac;
}

template<DpdkMode M, std::uint16_t QueueId, std::uint16_t NbRxDesc, std::uint16_t NbTxDesc, std::uint16_t BurstSize, std::uint32_t NumMbufs, std::uint16_t MaxFrame>
void DPDK<M, QueueId, NbRxDesc, NbTxDesc, BurstSize, NumMbufs, MaxFrame>::prefillRing(std::span<const std::uint8_t> frameTemplate) noexcept requires (M == DpdkMode::TxOnly || M == DpdkMode::RxTx) {
  m_txTemplateLen = static_cast<std::uint16_t>(std::min<std::size_t>(frameTemplate.size(), MaxFrame));
  std::memcpy(m_txTemplate.data(), frameTemplate.data(), m_txTemplateLen);
}

template<DpdkMode M, std::uint16_t QueueId, std::uint16_t NbRxDesc, std::uint16_t NbTxDesc, std::uint16_t BurstSize, std::uint32_t NumMbufs, std::uint16_t MaxFrame>
inline RxFrame DPDK<M, QueueId, NbRxDesc, NbTxDesc, BurstSize, NumMbufs, MaxFrame>::tryReceive() noexcept requires (M == DpdkMode::RxOnly || M == DpdkMode::RxTx) {
  if (m_rxIdx >= m_rxCount) [[unlikely]] {
    m_rxCount = rte_eth_rx_burst(m_port, QueueId, m_rxBurst, BurstSize);
    m_rxIdx   = 0;
    if (m_rxCount == 0) [[likely]] return {};
  }
  m_pendingRx = m_rxBurst[m_rxIdx];
  return { .data   = {rte_pktmbuf_mtod(m_pendingRx, const std::uint8_t*),
                      rte_pktmbuf_pkt_len(m_pendingRx)},
           .sec    = 0,
           .nsec   = 0,
           .status = 1 };
}

template<DpdkMode M, std::uint16_t QueueId, std::uint16_t NbRxDesc, std::uint16_t NbTxDesc, std::uint16_t BurstSize, std::uint32_t NumMbufs, std::uint16_t MaxFrame>
inline void DPDK<M, QueueId, NbRxDesc, NbTxDesc, BurstSize, NumMbufs, MaxFrame>::release() noexcept requires (M == DpdkMode::RxOnly || M == DpdkMode::RxTx) {
  rte_pktmbuf_free(m_pendingRx);
  m_pendingRx = nullptr;
  ++m_rxIdx;
}

template<DpdkMode M, std::uint16_t QueueId, std::uint16_t NbRxDesc, std::uint16_t NbTxDesc, std::uint16_t BurstSize, std::uint32_t NumMbufs, std::uint16_t MaxFrame>
inline std::uint8_t* DPDK<M, QueueId, NbRxDesc, NbTxDesc, BurstSize, NumMbufs, MaxFrame>::acquire(std::uint32_t frameLen) noexcept requires (M == DpdkMode::TxOnly || M == DpdkMode::RxTx) {
  if (m_txReuseInline && frameLen <= INLINE_SAFE_LEN) {
    if (!m_txReuse) [[unlikely]] {
      m_txReuse = rte_pktmbuf_alloc(m_pool);
      if (!m_txReuse) return nullptr;
      rte_mbuf_refcnt_set(m_txReuse, REFCNT_TOPUP);
      if (m_txTemplateLen)
        std::memcpy(rte_pktmbuf_mtod(m_txReuse, std::uint8_t*),
                    m_txTemplate.data(), m_txTemplateLen);
    }
    if (rte_mbuf_refcnt_read(m_txReuse) < REFCNT_LOW) [[unlikely]]
      rte_mbuf_refcnt_update(m_txReuse, REFCNT_TOPUP);
    m_pendingTx = m_txReuse;
    m_pendingTx->data_len = static_cast<std::uint16_t>(frameLen);
    m_pendingTx->pkt_len  = frameLen;
    return rte_pktmbuf_mtod(m_pendingTx, std::uint8_t*);
  }
  m_pendingTx = rte_pktmbuf_alloc(m_pool);
  if (!m_pendingTx) [[unlikely]] return nullptr;
  m_pendingTx->data_len = static_cast<std::uint16_t>(frameLen);
  m_pendingTx->pkt_len  = frameLen;
  auto* p = rte_pktmbuf_mtod(m_pendingTx, std::uint8_t*);
  if (m_txTemplateLen) [[likely]]
    std::memcpy(p, m_txTemplate.data(), std::min<std::uint32_t>(frameLen, m_txTemplateLen));
  return p;
}

template<DpdkMode M, std::uint16_t QueueId, std::uint16_t NbRxDesc, std::uint16_t NbTxDesc, std::uint16_t BurstSize, std::uint32_t NumMbufs, std::uint16_t MaxFrame>
inline void DPDK<M, QueueId, NbRxDesc, NbTxDesc, BurstSize, NumMbufs, MaxFrame>::commit() noexcept requires (M == DpdkMode::TxOnly || M == DpdkMode::RxTx) {
  while (rte_eth_tx_burst(m_port, QueueId, &m_pendingTx, 1) == 0) [[unlikely]] {}
  m_pendingTx = nullptr;
}

template<DpdkMode M, std::uint16_t QueueId, std::uint16_t NbRxDesc, std::uint16_t NbTxDesc, std::uint16_t BurstSize, std::uint32_t NumMbufs, std::uint16_t MaxFrame>
inline bool DPDK<M, QueueId, NbRxDesc, NbTxDesc, BurstSize, NumMbufs, MaxFrame>::send(std::span<const std::uint8_t> frame) noexcept requires (M == DpdkMode::TxOnly || M == DpdkMode::RxTx) {
  auto* buf = acquire(static_cast<std::uint32_t>(frame.size()));
  if (!buf) [[unlikely]] return false;
  std::memcpy(buf, frame.data(), frame.size());
  commit();
  return true;
}

// PMDs may reject RTE_ETH_LINK_SPEED_FIXED; a bare mask restricts the autoneg
// ADVERTISEMENT instead (both looped ends share the conf, so the link resolves there).
template<DpdkMode M, std::uint16_t QueueId, std::uint16_t NbRxDesc, std::uint16_t NbTxDesc, std::uint16_t BurstSize, std::uint32_t NumMbufs, std::uint16_t MaxFrame>
void DPDK<M, QueueId, NbRxDesc, NbTxDesc, BurstSize, NumMbufs, MaxFrame>::setLinkSpeeds(std::uint32_t speedsMask) noexcept {
  m_linkSpeeds = speedsMask;
}

// For PMDs that break on an asymmetric queue config (igc: nb_txq=0 silently kills RX
// — docs/Known_driver_issues.md §3.1). The AF_XDP PMD forces this internally.
template<DpdkMode M, std::uint16_t QueueId, std::uint16_t NbRxDesc, std::uint16_t NbTxDesc, std::uint16_t BurstSize, std::uint32_t NumMbufs, std::uint16_t MaxFrame>
void DPDK<M, QueueId, NbRxDesc, NbTxDesc, BurstSize, NumMbufs, MaxFrame>::setSymmetricQueues(bool on) noexcept {
  m_symmetricQueues = on;
}

// Both PMDs bulk-refill mbufs at the TOP of rx_burst, so a large ring pays a big refill
// INSIDE a poll while the echo is in flight. mlx5 refills min(64, NbRxDesc>>2): 256 -> 64
// mbufs, 64 -> 16. Measured on CX4 (30s A/B): P99.9 3.793 -> 3.648us, median UNMOVED.
// Per-PMD, not global — i40e's rearm threshold is a compile-time 64 and a 64-entry ring
// would leave no headroom. rte_eth_dev_adjust_nb_rx_tx_desc still clamps to PMD limits.
template<DpdkMode M, std::uint16_t QueueId, std::uint16_t NbRxDesc, std::uint16_t NbTxDesc, std::uint16_t BurstSize, std::uint32_t NumMbufs, std::uint16_t MaxFrame>
void DPDK<M, QueueId, NbRxDesc, NbTxDesc, BurstSize, NumMbufs, MaxFrame>::setNbRxDesc(std::uint16_t n) noexcept {
  m_nbRxDesc = n;
}

// Appended to this port's -a allowlist entry, so it must land before prepare()
// registers the BDF (first registration wins).
template<DpdkMode M, std::uint16_t QueueId, std::uint16_t NbRxDesc, std::uint16_t NbTxDesc, std::uint16_t BurstSize, std::uint32_t NumMbufs, std::uint16_t MaxFrame>
void DPDK<M, QueueId, NbRxDesc, NbTxDesc, BurstSize, NumMbufs, MaxFrame>::setDevargs(std::string_view devargs) noexcept {
  m_devargs = devargs;
}

// ONLY valid when the PMD copies the whole frame into the descriptor inside tx_burst
// (mlx5 + txqs_min_inline=0): the buffer is then reusable the moment commit() returns,
// so one mbuf is re-sent forever — no per-round alloc/free. See acquire().
template<DpdkMode M, std::uint16_t QueueId, std::uint16_t NbRxDesc, std::uint16_t NbTxDesc, std::uint16_t BurstSize, std::uint32_t NumMbufs, std::uint16_t MaxFrame>
void DPDK<M, QueueId, NbRxDesc, NbTxDesc, BurstSize, NumMbufs, MaxFrame>::setTxInlineReuse(bool on) noexcept {
  m_txReuseInline = on;
}

