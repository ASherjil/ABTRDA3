#pragma once

#include "RxFrame.hpp"
#include "PciHelpers.hpp"
#include "NapiConfig.hpp"
#include <rte_eal.h>
#include <rte_cycles.h>
#include <rte_errno.h>
#include <rte_ethdev.h>
#include <rte_mbuf.h>
#include <rte_mempool.h>
#include <rte_ether.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <unistd.h>
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

enum class DpdkMode : std::uint8_t {
  RxOnly,
  TxOnly,
  RxTx
};

// Process-global EAL state + cold-path helpers, shared by EVERY DPDK<Mode, ...>
// specialization through this NON-TEMPLATE base class. EAL initialises once per
// process and its -a allowlist fixes which PCI devices exist for the WHOLE
// process, so this state must have exactly ONE copy — a static inside the class
// template would be duplicated per specialization and double-init EAL. Private
// inheritance keeps it an implementation detail of DPDK<>; everything is inline
// static (no virtuals, empty base, zero cost), and hoisting the non-dependent
// helpers here also stops them being stamped out once per specialization.
class DpdkEal {
protected:
  DpdkEal() noexcept = default;

  static constexpr unsigned kMaxSimdBitwidth = 256;

  // All DPDK control threads (the eal-intr-thread shown as "dpdk-intr" — it services
  // the i40e 50ms self-re-arming link/stat alarm and VFIO config-space interrupts —
  // plus the multiprocess socket handler) are parked on kControlThreadCore. DPDK
  // derives its control-thread cpuset from the CALLER's affinity at rte_eal_init
  // MINUS the dataplane lcores; the RTT server/client path pins the caller to the
  // single isolated lcore BEFORE init, so that difference is EMPTY and EAL falls
  // back to pinning the intr thread onto our poll core, where it ping-pongs with our
  // SCHED_OTHER busy-poll ~80x/s (proven via sched:sched_switch) -> RTT tail spikes.
  // ealInit narrows the caller's affinity to {kControlThreadCore} across rte_eal_init
  // so the control cpuset is exactly that core, then restores the caller's pin. Core
  // 0 is the kernel's housekeeping/IRQ core (irqaffinity=0,1) — where periodic work
  // already lives. (AF_XDP is immune: no PCI/VFIO interrupt source.)
  static constexpr int kControlThreadCore = 0;

  // PCI -a allowlist, registered by prepare() BEFORE the single EAL init (the
  // single-recorder owns BOTH loopback ports in one process, so both BDFs must
  // be known first — hence the prepare()/init() split). Entries may carry
  // devargs ("<bdf>,key=val"); dedup compares the BDF part only and the FIRST
  // registration wins, so a devargs-bearing entry isn't shadowed by ealInit's
  // bare fallback.
  inline static std::vector<std::string> s_allowedBdfs{};

  // DPDK-over-AF_XDP: each entry is a full --vdev string
  // ("net_af_xdpN,iface=cx0,start_queue=0,queue_count=1,mode=drv,busy_budget=64").
  // Same prepare()-before-init() registration model as the BDF allowlist so both
  // loopback ports are known before the single EAL init. Dedup on the vdev name.
  inline static std::vector<std::string> s_vdevs{};

  inline static bool s_ealInited{false};

  static void addAllowedBdf(const std::string& bdf) {
    const auto key = bdf.substr(0, bdf.find(','));
    for (const auto& e : s_allowedBdfs)
      if (e.substr(0, e.find(',')) == key) return;
    s_allowedBdfs.push_back(bdf);
  }

  static void addVdev(const std::string& arg) {
    const auto key = arg.substr(0, arg.find(','));
    for (const auto& e : s_vdevs)
      if (e.substr(0, e.find(',')) == key) return;
    s_vdevs.push_back(arg);
  }

  static bool ealInit(const std::string& bdf, int lcore) noexcept {
    if (s_ealInited) return true;

    if (s_vdevs.empty()) addAllowedBdf(bdf);   // BDF mode only; AF_XDP regs in prepare()

    const std::string& base = !s_vdevs.empty() ? s_vdevs.front() : s_allowedBdfs.front();
    std::string prefix = "abtrda3_" + base.substr(0, base.find(','));
    for (char& c : prefix) if (c == ':' || c == '.' || c == ',') c = '_';
    std::string lcoreStr = std::to_string(lcore);

    std::vector<std::string> args = {
      "abtrda3", "-l", lcoreStr, "--main-lcore", lcoreStr,
      "-n", "4", "--file-prefix", prefix,
      "--force-max-simd-bitwidth=" + std::to_string(kMaxSimdBitwidth)
    };
    if (!s_vdevs.empty()) {
      args.emplace_back("--no-pci");            // AF_XDP PMD is a vdev; no PCI probe
      args.emplace_back("--in-memory");         // no /var/run/dpdk/<prefix> files => two
                                                // af_xdp primaries (RTT server+client) on
                                                // one host don't collide on the lock
      for (const auto& v : s_vdevs) {
        args.emplace_back("--vdev");
        args.emplace_back(v);
      }
    } else {
      for (const auto& b : s_allowedBdfs) {
        args.emplace_back("-a");
        args.emplace_back(b);
      }
    }

    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (auto& a : args) argv.push_back(a.data());
    argv.push_back(nullptr);

    // Park DPDK's control threads on kControlThreadCore (see its definition): narrow
    // our affinity to that core so EAL's control cpuset = {kControlThreadCore}, run
    // init, then restore our own pin. Skipped if the control core IS our lcore.
    const bool park = (kControlThreadCore != lcore);
    cpu_set_t savedAff;
    CPU_ZERO(&savedAff);
    bool haveSaved = false;
    if (park) {
      haveSaved = (pthread_getaffinity_np(pthread_self(), sizeof(savedAff), &savedAff) == 0);
      cpu_set_t ctrlSet;
      CPU_ZERO(&ctrlSet);
      CPU_SET(kControlThreadCore, &ctrlSet);
      pthread_setaffinity_np(pthread_self(), sizeof(ctrlSet), &ctrlSet);
    }

    const int rc = rte_eal_init(static_cast<int>(args.size()), argv.data());

    if (park && haveSaved)
      pthread_setaffinity_np(pthread_self(), sizeof(savedAff), &savedAff);

    if (rc < 0) {
      std::fprintf(stderr, "[DPDK] rte_eal_init failed: %s\n", rte_strerror(rte_errno));
      return false;
    }
    s_ealInited = true;
    return true;
  }

  static bool writeSysfs(const std::string& path, std::string_view val) noexcept {
    const int fd = ::open(path.c_str(), O_WRONLY | O_CLOEXEC);
    if (fd < 0) return false;
    const ssize_t n = ::write(fd, val.data(), val.size());
    ::close(fd);
    return n == static_cast<ssize_t>(val.size());
  }

  static bool bindToVfio(const std::string& bdf) noexcept {
    if (::access("/sys/bus/pci/drivers/vfio-pci", F_OK) != 0)
      (void)std::system("modprobe vfio-pci");
    if (pci::currentDriver(bdf) == "vfio-pci") return true;
    const std::string dev = "/sys/bus/pci/devices/" + bdf;
    writeSysfs(dev + "/driver_override", "vfio-pci");
    writeSysfs(dev + "/driver/unbind", bdf);
    return writeSysfs("/sys/bus/pci/drivers_probe", bdf);
  }
};

template<DpdkMode M, std::uint16_t QueueId = 0, std::uint16_t NbRxDesc = 256, std::uint16_t NbTxDesc = 256, std::uint16_t BurstSize = 32, std::uint32_t NumMbufs = 8191, std::uint16_t MaxFrame = RTE_MBUF_DEFAULT_DATAROOM>
class DPDK : private DpdkEal {
  static_assert(BurstSize >= 1, "BurstSize must be >= 1");
  static_assert(NumMbufs > BurstSize, "NumMbufs must exceed BurstSize");

  static constexpr bool          HAS_RX          = (M == DpdkMode::RxOnly || M == DpdkMode::RxTx);
  static constexpr bool          HAS_TX          = (M == DpdkMode::TxOnly || M == DpdkMode::RxTx);
  static constexpr std::uint32_t MBUF_CACHE      = 250;
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

  // Generic pre-init knobs. This class is PMD-agnostic — all DRIVER-SPECIFIC
  // policy (which NIC needs which knob, plus post-start BAR0 register fixes)
  // lives in TransportDispatch, which knows the TOML driver name.
  //   setLinkSpeeds — RTE_ETH_LINK_SPEED_* mask for dev_configure (0 = full
  //     autoneg). PMDs may reject RTE_ETH_LINK_SPEED_FIXED; a bare mask
  //     restricts the autoneg ADVERTISEMENT (both looped ends get the same
  //     conf, so the link resolves at the restricted speed).
  //   setSymmetricQueues — force nb_rxq == nb_txq == 1 even for unidirectional
  //     roles (some PMDs break asymmetric configs; the AF_XDP PMD requires
  //     pairing and forces this internally).
  //   setDevargs — extra PMD devargs appended to this port's -a allowlist entry
  //     ("key=val,key=val"); must be called BEFORE prepare() registers the BDF.
  //   setTxInlineReuse — enable the single-mbuf full-inline TX fast path. ONLY
  //     valid when the PMD copies the whole frame inside tx_burst itself (e.g.
  //     mlx5 with txqs_min_inline=0), making the buffer reusable on return.
  void setLinkSpeeds(std::uint32_t speedsMask) noexcept { m_linkSpeeds = speedsMask; }
  void setSymmetricQueues(bool on) noexcept { m_symmetricQueues = on; }
  void setDevargs(std::string_view devargs) noexcept { m_devargs = devargs; }
  void setTxInlineReuse(bool on) noexcept { m_txReuseInline = on; }

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
    m_linkSpeeds{o.m_linkSpeeds}, m_symmetricQueues{o.m_symmetricQueues},
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

  if (m_afxdpPmd) {
    napi::detachXdpProgram(m_ifname.c_str(), "DPDK");
    napi::ensureCombinedOne(m_ifname.c_str(), "DPDK");   // bounce absorbed by waitLink
    const std::string vname = "net_af_xdp" + std::to_string(s_vdevs.size());
    addVdev(vname + ",iface=" + m_ifname +
            ",start_queue=0,queue_count=1,mode=drv,busy_budget=64");
    m_pci           = vname;
    m_txReuseInline = false;   // full-inline reuse is N/A for the AF_XDP PMD
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

  // The AF_XDP PMD requires nb_rx_queues == nb_tx_queues (it pairs an XSK rx+tx
  // per queue); dispatch requests the same via setSymmetricQueues for PMDs that
  // break asymmetric configs (igc: docs/Known_driver_issues.md §3.1). The unused
  // side only satisfies the driver; the hot path still drives one direction.
  // m_pool is created unconditionally above, so a TxOnly port always has a pool
  // for its unused RX queue.
  const bool bothQueues = m_afxdpPmd || m_symmetricQueues;
  const std::uint16_t nb_rxq = (HAS_RX || bothQueues) ? 1 : 0;
  const std::uint16_t nb_txq = (HAS_TX || bothQueues) ? 1 : 0;
  if (rte_eth_dev_configure(m_port, nb_rxq, nb_txq, &conf) != 0) {
    std::fprintf(stderr, "[DPDK] dev_configure failed (port %u)\n", m_port);
    return false;
  }

  std::uint16_t nb_rxd = NbRxDesc, nb_txd = NbTxDesc;
  if (rte_eth_dev_adjust_nb_rx_tx_desc(m_port, &nb_rxd, &nb_txd) != 0) {
    std::fprintf(stderr, "[DPDK] adjust_nb_rx_tx_desc failed (port %u)\n", m_port);
    return false;
  }

  if (HAS_RX || bothQueues) {
    // PMD-default thresholds throughout — audited optimal for one-in-flight on
    // every campaign NIC (and on igc, never write rx wthresh=0: it kills
    // descriptor write-back outright — docs/Known_driver_issues.md §3.2).
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

  // Driver-specific post-start register fixes (i40e ITR clear, igc EEE clear)
  // are NOT done here — this transport is PMD-agnostic. TransportDispatch
  // applies them via pci::i40eClearItr / pci::igcDisableEee after init().

  // DPDK-over-AF_XDP: apply (or deterministically CLEAR) the NAPI deferral
  // regime, then read back what the kernel actually granted the PMD's XSK —
  // zero-copy and busy-poll are negotiated silently, so verify like a register
  // write. Shared napi:: helpers, identical to the native AFXDP backend.
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
    rte_eth_link_get_nowait(m_port, &link);
    if (link.link_status == RTE_ETH_LINK_UP) break;
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

  // DIAGNOSTIC: dump the NIC's OWN packet counters before teardown. This is the
  // wire truth, independent of our hot-path bookkeeping — the discriminator for the
  // "recorded >> sent" DPDK bug: compare TX opackets vs RX ipackets vs the app's
  // recorded. ipackets ~= recorded (>> sent) => wire/PMD genuinely duplicates;
  // ipackets ~= sent but recorded >> sent => our tryReceive/release re-reads buffers.
  // imissed = RX-ring overrun (no descriptor); rx_nombuf = mempool exhausted.
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
  rte_eth_dev_stop(m_port);
  if constexpr (HAS_TX) {
    if (m_txReuse) {
      rte_mbuf_refcnt_set(m_txReuse, 1);
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

