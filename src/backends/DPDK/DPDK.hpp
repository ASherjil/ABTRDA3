#pragma once

#include "RxFrame.hpp"
#include "PciHelpers.hpp"
#include "BackendBase.hpp"

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
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// =============================================================================
// DPDK — one DPDK transport for every supported NIC (Intel i40e XXV710, Intel
// igc I225-V via the e1000 PMD, Mellanox mlx5 ConnectX-4) through the unified
// rte_ethdev API. Hot path (rte_eth_rx_burst / rte_eth_tx_burst) is identical
// across PMDs. Target: DPDK 25.11 LTS. Satisfies the TxRing/RxRing concepts
// (tryReceive/release + acquire/commit/send/prefillRing); every knob that can
// be fixed at compile time is a template param — only interface name, lcore
// and driver are runtime (from the test config).
//
// BINDING MODELS (selected by the kernel driver name from the TOML):
//   * Intel pass-through (i40e/igc/...): prepare() unbinds the kernel driver and
//     binds vfio-pci (driver_override + drivers_probe, as dpdk-devbind does;
//     needs IOMMU: intel_iommu=on iommu=pt). BIND-ONCE: shutdown() does NOT
//     restore the kernel driver — restoring it in the dtor HANGS (the sysfs
//     unbind blocks until vfio releases the device, but EAL still holds it; no
//     rte_eal_cleanup). Re-runs resolve the BDF from the predictable name
//     (pci::bdfFromName) since the netdev is gone. Recovery if ever needed:
//     dpdk-devbind --bind=i40e <bdf>, or reboot.
//   * Mellanox mlx5 ("mlx5" in driver name): BIFURCATED — stays on the kernel
//     driver (needs rdma-core); DPDK drives the port alongside the kernel.
//
// PROCESS-GLOBAL EAL (namespace dpdk below): EAL initialises ONCE per process
// and its -a allowlist fixes which PCI devices exist for the WHOLE process. The
// single-recorder owns BOTH loopback ports in one process, so prepare() must
// run on (and register) BOTH BDFs BEFORE the first init() triggers the one EAL
// init — hence the prepare()/init() split. The guard + BDF set live at
// namespace scope (a template-class static would give each DPDK<Mode> its own
// copy and double-init EAL). addAllowedBdf() entries may carry devargs
// ("<bdf>,key=val"); dedup compares the BDF part only and the FIRST
// registration wins, so prepare()'s devargs-bearing entry isn't shadowed by
// ealInit's bare fallback. The EAL file-prefix is derived from the first BDF
// (devargs stripped), so server+client can run as two DPDK primaries on the
// same looped-DAC host. The runtime SIMD cap handed to EAL is dpdk::kMaxSimdBitwidth
// (--force-max-simd-bitwidth) — see its definition for how it picks the i40e PMD's
// vector vs scalar RX/TX path and why scalar wins for one-packet-in-flight latency.
//
// mlx5 DEVARGS (all A/B-tested 2026-06-10 on ConnectX-4 Lx):
//   txq_mem_algn=0    — disable the consecutive-TxQ-umem feature (DPDK >= 25.x,
//     a cache-alignment optimisation for MANY-queue workloads): with two
//     single-queue ports in one process its chunk allocation fails on the
//     second port ("Failed to allocate consecutive memory for TxQs").
//     0 = legacy per-queue allocation + own MRs.
//   txqs_min_inline=0 — enable FULL Tx data inlining at 1 queue. By default
//     inlining needs >= 8 Tx queues (mlx5_txq.c txq_set_params); below that
//     CX4Lx inlines only the forced 18B L2 minimum and the NIC gather-DMAs the
//     rest of the frame from host memory on EVERY send. With 0 the whole 64B
//     frame rides inside the WQE — no payload DMA read. A/B: -520ns UNIFORM
//     shift (median 2.325 -> 1.806us). Verify via the tx burst mode print
//     ("INLINE" must appear).
//   rxq_cqe_comp_en=0 — plain 64B CQEs, no decompression in the RX poll path
//     (compression saves PCIe bandwidth under load — pointless at one packet
//     in flight). A/B: neutral; kept, zero cost.
//   sq_db_nc=1 REJECTED — non-cached doorbell made the whole band +10-20ns
//     worse (NC serializes every store of the ~3-WQEBB BlueFlame write; the
//     default WC mapping batches it).
//
// i40e ITR FIX (applied after dev_start): the i40e silicon honors the
// per-vector Interrupt Throttle Rate even in pure polling mode — ITR governs
// when completed RX descriptors are written back to host RAM. DPDK 25.11
// leaves it at the firmware default and exposes NO public API to change it
// (no devarg, nothing in rte_pmd_i40e.h). Proof it is the lever (2026-05-26,
// packet_mmap A/B with ethtool -C): rx-usecs 50 -> median 49.81us; rx-usecs 0
// -> 11.88us. Fix: a SECOND mmap of the same physical BAR0 via sysfs resource0
// (ABTEdge BackendBase; DPDK's vfio mapping untouched), writing 0 to
// I40E_PFINT_ITR0(0) = 0x00038000 and I40E_PFINT_ITRN(0,0) = 0x00030000
// (offsets from drivers/net/intel/i40e/base/i40e_register.h — either vector
// could be tied to the queue).
//
// TX PATH: acquire() returns a fresh mbuf's data pointer (header pre-stamped
// from prefillRing — DPDK has no fixed pre-stamped ring); commit() bursts it;
// on success the PMD owns and later frees the mbuf; on ring-full the mbuf is
// freed (drop, never leak). mlx5 FULL-INLINE FAST PATH (m_txReuseInline, set in
// prepare() for bifurcated only): with txqs_min_inline=0 the PMD COPIES the
// whole small frame into the WQE inside the tx_burst call itself, so the
// buffer is reusable the moment commit() returns — ONE mbuf re-sent forever,
// no per-round alloc/free, no mempool cache traffic. The PMD still decrements
// refcnt once per send when it processes completions (inside our own tx_burst
// calls — single-threaded, so the plain refcnt read is race-free); the 16-bit
// refcnt is topped back up whenever it dips below REFCNT_LOW (decrements lag
// sends by at most NbTxDesc << REFCNT_LOW, so it never reaches zero). Gated to
// frames <= INLINE_SAFE_LEN (default inlen_send = 290B; 128 keeps wide margin).
// shutdown() resets the refcnt to 1 after dev_stop (PMD decrements done) so
// the free actually returns the mbuf to the pool.
//
// RX PATH: peek-one over an internal burst cache; when drained, busy-poll a
// fresh rte_eth_rx_burst. No syscalls. Promiscuous mode is NOT enabled — both
// ends know the peer MAC (TOML), so the NIC's unicast filter does the work
// (promisc forces every unicast through the full RX pipeline — a small but
// measurable NIC-internal cost).
//
// RING SIZES (NbRx/TxDesc default 256, was 1024): with one packet in flight
// the rings are pure cache footprint — 1024 RX CQEs alone is 64KB cycling
// through L2; 256 keeps CQ+SQ hot in L1/L2. PMD minimums are far below;
// adjust_nb_rx_tx_desc still rounds up if a PMD ever needs more.
//
// LINK MANAGEMENT: init(doWaitLink=false) starts the port WITHOUT waiting —
// the single-recorder owns both ports of a loopback pair and a loopback link
// only comes up once BOTH PHYs are up, so dispatch starts both then calls
// waitLink() on both. waitLink polls ~20s; after a 2s grace (don't tear down a
// mid-negotiation link) it actively restarts autoneg via set_link_up every
// ~3s; -ENOTSUP (bifurcated mlx5, link already up) is expected/harmless.
// shutdown() = rte_eth_dev_stop ONLY: dev_stop disables queues/DMA/MSI-X and
// cancels the periodic alarm so the device cannot run unattended on vfio and
// contend on PCIe with the next run's NIC (an earlier no-dev_stop version left
// the igc DMA-ing during a subsequent XXV710 run -> fat tail). No set_link_up,
// no dev_close: dev_stop parks the PHY (i40e), so the next run's dev_start
// re-syncs from parked (~4.7ms, ~85ms in) — absorbed by the recorder's
// 500k-packet warmup. The next process re-probes; i40e_pf_reset() cleans the
// stopped state.
//
// BURST-MODE PRINTS (after dev_start): rte_eth_tx/rx_burst_mode_get — the
// cheap way to VERIFY a devarg took effect (mlx5 Tx must report "INLINE";
// i40e reports its AVX-512 vector paths).
// =============================================================================

namespace dpdk {

// --force-max-simd-bitwidth cap handed to EAL, which selects the i40e PMD RX/TX
// path: 512 = AVX-512, 256 = AVX2, 128 = SSE, 64 = SCALAR. A/B on XXV710 RTT
// (2026-06-23): AVX-512 and 64 (Scalar Simple/Bulk) gave the SAME ~9.15us median
// — the floor is the i40e HARDWARE pipeline, not the PMD vector path, so Intel's
// "scalar = big latency win" did NOT reproduce here. 256 (AVX2) is the default:
// same latency as 512, keeps vector throughput headroom, and avoids AVX-512's
// frequency/power side effects. Cold EAL startup arg (no hot-path codegen
// impact); the startup burst-mode print verifies which path the PMD picked.
inline constexpr unsigned kMaxSimdBitwidth = 256;

inline std::vector<std::string>& allowedBdfs() {
  static std::vector<std::string> v;
  return v;
}

inline void addAllowedBdf(const std::string& bdf) {
  auto& v = allowedBdfs();
  const auto key = bdf.substr(0, bdf.find(','));
  for (const auto& e : v)
    if (e.substr(0, e.find(',')) == key) return;
  v.push_back(bdf);
}

// DPDK-over-AF_XDP: each entry is a full --vdev string
// ("net_af_xdpN,iface=cx0,start_queue=0,queue_count=1,mode=drv,busy_budget=64").
// Same prepare()-before-init() registration model as the BDF allowlist so both
// loopback ports are known before the single EAL init. Dedup on the vdev name.
inline std::vector<std::string>& vdevs() {
  static std::vector<std::string> v;
  return v;
}

inline void addVdev(const std::string& arg) {
  auto& v = vdevs();
  const auto key = arg.substr(0, arg.find(','));
  for (const auto& e : v)
    if (e.substr(0, e.find(',')) == key) return;
  v.push_back(arg);
}

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
inline constexpr int kControlThreadCore = 0;

inline bool ealInit(const std::string& bdf, int lcore) noexcept {
  static bool inited = false;
  if (inited) return true;

  const auto& vds = vdevs();
  if (vds.empty()) addAllowedBdf(bdf);   // BDF mode only; AF_XDP regs in prepare()
  const auto& bdfs = allowedBdfs();

  const std::string& base = !vds.empty() ? vds.front() : bdfs.front();
  std::string prefix = "abtrda3_" + base.substr(0, base.find(','));
  for (char& c : prefix) if (c == ':' || c == '.' || c == ',') c = '_';
  std::string lcoreStr = std::to_string(lcore);

  std::vector<std::string> args = {
    "abtrda3", "-l", lcoreStr, "--main-lcore", lcoreStr,
    "-n", "4", "--file-prefix", prefix,
    "--force-max-simd-bitwidth=" + std::to_string(kMaxSimdBitwidth)
  };
  if (!vds.empty()) {
    args.emplace_back("--no-pci");            // AF_XDP PMD is a vdev; no PCI probe
    args.emplace_back("--in-memory");         // no /var/run/dpdk/<prefix> files => two
                                              // af_xdp primaries (RTT server+client) on
                                              // one host don't collide on the lock
    for (const auto& v : vds) {
      args.emplace_back("--vdev");
      args.emplace_back(v);
    }
  } else {
    for (const auto& b : bdfs) {
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
  inited = true;
  return true;
}

}  // namespace dpdk

enum class DpdkMode : std::uint8_t { RxOnly, TxOnly, RxTx };

template<DpdkMode M, std::uint16_t QueueId = 0, std::uint16_t NbRxDesc = 256, std::uint16_t NbTxDesc = 256, std::uint16_t BurstSize = 32, std::uint32_t NumMbufs = 8191, std::uint16_t MaxFrame = RTE_MBUF_DEFAULT_DATAROOM>
class DPDK {
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

  [[nodiscard]] bool prepare() noexcept;
  [[nodiscard]] bool init(bool doWaitLink = true) noexcept;
  [[nodiscard]] bool waitLink() noexcept;
  void shutdown() noexcept;

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
  static bool ealInitOnce(const std::string& pci, int lcore) noexcept;
  static bool writeSysfs(const std::string& path, std::string_view val) noexcept;
  static bool bindToVfio(const std::string& bdf) noexcept;

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
    m_txReuse       = std::exchange(o.m_txReuse, nullptr);
    m_txReuseInline = o.m_txReuseInline;
  }
  return *this;
}

template<DpdkMode M, std::uint16_t QueueId, std::uint16_t NbRxDesc, std::uint16_t NbTxDesc, std::uint16_t BurstSize, std::uint32_t NumMbufs, std::uint16_t MaxFrame>
bool DPDK<M, QueueId, NbRxDesc, NbTxDesc, BurstSize, NumMbufs, MaxFrame>::prepare() noexcept {
  if (m_prepared) return true;

  // DPDK-over-AF_XDP: drive a kernel AF_XDP socket through the net_af_xdp vdev
  // PMD — no PCI BDF, no vfio rebind (the PMD uses the kernel netdev directly).
  // Our DPDK API drives it via the SAME rte_ethdev hot path as every other PMD;
  // only the control plane differs. mode=drv (native XDP), busy_budget enables
  // preferred busy-poll, force_copy omitted => zero-copy negotiated. m_pci holds
  // the vdev name so init()'s get_port_by_name() resolves it unchanged.
  if (m_driver.find("af_xdp") != std::string::npos) {
    const std::string vname = "net_af_xdp" + std::to_string(dpdk::vdevs().size());
    dpdk::addVdev(vname + ",iface=" + m_ifname +
                  ",start_queue=0,queue_count=1,mode=drv,busy_budget=64");
    m_pci           = vname;
    m_txReuseInline = false;   // mlx5 full-inline reuse is N/A for the AF_XDP PMD
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

  const bool bifurcated = (m_driver.find("mlx5") != std::string::npos);
  if (bifurcated) {
    std::fprintf(stderr, "[DPDK] %s (%s): mlx5 bifurcated — kept on kernel driver\n",
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

  dpdk::addAllowedBdf(bifurcated
      ? m_pci + ",txq_mem_algn=0,txqs_min_inline=0,rxq_cqe_comp_en=0"
      : m_pci);
  m_txReuseInline = bifurcated;
  m_prepared = true;
  return true;
}

template<DpdkMode M, std::uint16_t QueueId, std::uint16_t NbRxDesc, std::uint16_t NbTxDesc, std::uint16_t BurstSize, std::uint32_t NumMbufs, std::uint16_t MaxFrame>
bool DPDK<M, QueueId, NbRxDesc, NbTxDesc, BurstSize, NumMbufs, MaxFrame>::init(bool doWaitLink) noexcept {
  if (!prepare()) return false;

  if (!ealInitOnce(m_pci, m_lcore)) return false;

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

  // The net_af_xdp PMD requires nb_rx_queues == nb_tx_queues (it pairs an XSK
  // rx+tx per queue). Our single-recorder uses asymmetric TxOnly/RxOnly ports,
  // which mlx5/i40e accept but af_xdp rejects with -EINVAL. So in af_xdp mode
  // configure BOTH directions (1 each) regardless of mode — the unused side is
  // set up only to satisfy the pairing; the hot path still drives one direction.
  const bool afxdp = (m_driver.find("af_xdp") != std::string::npos);
  const std::uint16_t nb_rxq = (HAS_RX || afxdp) ? 1 : 0;
  const std::uint16_t nb_txq = (HAS_TX || afxdp) ? 1 : 0;
  if (rte_eth_dev_configure(m_port, nb_rxq, nb_txq, &conf) != 0) {
    std::fprintf(stderr, "[DPDK] dev_configure failed (port %u)\n", m_port);
    return false;
  }

  std::uint16_t nb_rxd = NbRxDesc, nb_txd = NbTxDesc;
  if (rte_eth_dev_adjust_nb_rx_tx_desc(m_port, &nb_rxd, &nb_txd) != 0) {
    std::fprintf(stderr, "[DPDK] adjust_nb_rx_tx_desc failed (port %u)\n", m_port);
    return false;
  }

  if (HAS_RX || afxdp) {
    rte_eth_rxconf rxconf = info.default_rxconf;
    rxconf.offloads = conf.rxmode.offloads;
    if (rte_eth_rx_queue_setup(m_port, QueueId, nb_rxd, sid, &rxconf, m_pool) < 0) {
      std::fprintf(stderr, "[DPDK] rx_queue_setup failed\n");
      return false;
    }
  }
  if (HAS_TX || afxdp) {
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

  if (m_driver == "i40e") {
    const std::size_t bar0Size = pci::barSize(m_pci, 0);
    const std::string resPath  = "/sys/bus/pci/devices/" + m_pci + "/resource0";
    BackendBase bar0;
    if (bar0Size > 0 && bar0.open(resPath.c_str(), 0, bar0Size)) {
      *bar0.registerPtr<std::uint32_t>(0x00038000) = 0;
      *bar0.registerPtr<std::uint32_t>(0x00030000) = 0;
      std::fprintf(stderr, "[DPDK] %s: i40e ITR cleared (RX writeback throttling off)\n",
                   m_ifname.c_str());
    } else {
      std::fprintf(stderr, "[DPDK] %s: WARN: %s mmap failed — ITR not cleared, "
                           "expect ~30us median RTT\n", m_ifname.c_str(), resPath.c_str());
    }
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

template<DpdkMode M, std::uint16_t QueueId, std::uint16_t NbRxDesc, std::uint16_t NbTxDesc, std::uint16_t BurstSize, std::uint32_t NumMbufs, std::uint16_t MaxFrame>
bool DPDK<M, QueueId, NbRxDesc, NbTxDesc, BurstSize, NumMbufs, MaxFrame>::ealInitOnce(const std::string& pci, int lcore) noexcept {
  return dpdk::ealInit(pci, lcore);
}

template<DpdkMode M, std::uint16_t QueueId, std::uint16_t NbRxDesc, std::uint16_t NbTxDesc, std::uint16_t BurstSize, std::uint32_t NumMbufs, std::uint16_t MaxFrame>
bool DPDK<M, QueueId, NbRxDesc, NbTxDesc, BurstSize, NumMbufs, MaxFrame>::writeSysfs(const std::string& path, std::string_view val) noexcept {
  const int fd = ::open(path.c_str(), O_WRONLY | O_CLOEXEC);
  if (fd < 0) return false;
  const ssize_t n = ::write(fd, val.data(), val.size());
  ::close(fd);
  return n == static_cast<ssize_t>(val.size());
}

template<DpdkMode M, std::uint16_t QueueId, std::uint16_t NbRxDesc, std::uint16_t NbTxDesc, std::uint16_t BurstSize, std::uint32_t NumMbufs, std::uint16_t MaxFrame>
bool DPDK<M, QueueId, NbRxDesc, NbTxDesc, BurstSize, NumMbufs, MaxFrame>::bindToVfio(const std::string& bdf) noexcept {
  if (::access("/sys/bus/pci/drivers/vfio-pci", F_OK) != 0)
    (void)std::system("modprobe vfio-pci");
  if (pci::currentDriver(bdf) == "vfio-pci") return true;
  const std::string dev = "/sys/bus/pci/devices/" + bdf;
  writeSysfs(dev + "/driver_override", "vfio-pci");
  writeSysfs(dev + "/driver/unbind", bdf);
  return writeSysfs("/sys/bus/pci/drivers_probe", bdf);
}
