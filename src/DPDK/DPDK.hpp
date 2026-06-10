//
// DPDK — one DPDK transport for every supported NIC (Intel i40e XXV710, Intel
// igc I225-V via the e1000 PMD, Mellanox mlx5 ConnectX-4) through the unified
// rte_ethdev API. The hot path (rte_eth_rx_burst / rte_eth_tx_burst) is identical
// across all PMDs.
//
// Configured by interface NAME + kernel DRIVER name (like the other transports),
// not a raw PCI id. init() resolves the name -> PCI BDF while the netdev still
// exists, then prepares the device per its binding model:
//   * Intel pass-through (i40e/igc/...): unbind the kernel driver, bind vfio-pci
//     (needs IOMMU). shutdown() rebinds the kernel driver so the netdev reappears
//     and the next run resolves the name again — mirrors Intel_I210/Cadence_GEM.
//   * Mellanox mlx5 (driver name contains "mlx5"): BIFURCATED — left on the kernel
//     driver (needs rdma-core); DPDK drives it alongside the kernel.
//
// Satisfies the TxRing/RxRing concepts (tryReceive/release + acquire/commit/send/
// prefillRing). Every knob that can be fixed at compile time is a template param —
// only the interface name, lcore and driver are runtime (from the test config).
//
// Target: DPDK 25.11 LTS.
//
#pragma once

#include "RxFrame.hpp"
#include "PciHelpers.hpp"
#include "BackendBase.hpp"   // ABTEdge — sysfs-resource0 MMIO (for i40e ITR fix)

#include <rte_eal.h>
#include <rte_cycles.h>   // rte_delay_us_block (link-up wait)
#include <rte_errno.h>
#include <rte_ethdev.h>
#include <rte_mbuf.h>
#include <rte_mempool.h>
#include <rte_ether.h>

#include <fcntl.h>
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

// ── Process-global EAL init ─────────────────────────────────────────────────
// DPDK's EAL is initialised ONCE per process, and its -a allowlist fixes which
// PCI devices exist for the WHOLE process. A single-port run allowlists one BDF;
// the single-recorder (Tx port + Rx port in one process) needs BOTH allowlisted
// on that one init. So the guard + BDF set live at namespace scope (NOT as a
// template-class static, which would give each DPDK<Mode> its own copy and
// double-init EAL). Register every port's BDF up front, then ealInit() emits one
// -a per BDF. Idempotent: the first ealInit() call wins; later calls are no-ops.
namespace dpdk {

inline std::vector<std::string>& allowedBdfs() {
  static std::vector<std::string> v;
  return v;
}

// Register a BDF to be allowlisted at EAL init. Must be called BEFORE ealInit()
// (i.e. before any port's DPDK::init() triggers it). The entry may carry devargs
// ("<bdf>,key=val"); dedup compares the BDF part only, first registration wins —
// so prepare()'s devargs-bearing entry isn't shadowed by ealInit's bare fallback.
inline void addAllowedBdf(const std::string& bdf) {
  auto& v = allowedBdfs();
  const auto key = bdf.substr(0, bdf.find(','));
  for (const auto& e : v)
    if (e.substr(0, e.find(',')) == key) return;
  v.push_back(bdf);
}

// Initialise EAL once, allowlisting every registered BDF (plus `bdf` if not yet
// registered — covers the single-port path that calls ealInit directly). lcore
// is the EAL main lcore (must equal the process's pinned core). Thread-unsafe by
// design: all registration + the first init happen on the main thread at startup.
inline bool ealInit(const std::string& bdf, int lcore) noexcept {
  static bool inited = false;
  if (inited) return true;

  addAllowedBdf(bdf);
  const auto& bdfs = allowedBdfs();

  // file-prefix derived from the first BDF (unique per process; strip any devargs).
  std::string prefix = "abtrda3_" + bdfs.front().substr(0, bdfs.front().find(','));
  for (char& c : prefix) if (c == ':' || c == '.') c = '_';
  std::string lcoreStr = std::to_string(lcore);

  // Fixed args + two ("-a", <bdf>) tokens per allowlisted device.
  std::vector<std::string> args = {
    "abtrda3", "-l", lcoreStr, "--main-lcore", lcoreStr,
    "-n", "4", "--file-prefix", prefix, "--force-max-simd-bitwidth=512"
  };
  for (const auto& b : bdfs) { args.emplace_back("-a"); args.emplace_back(b); }

  std::vector<char*> argv;
  argv.reserve(args.size() + 1);
  for (auto& a : args) argv.push_back(a.data());
  argv.push_back(nullptr);

  if (rte_eal_init(static_cast<int>(args.size()), argv.data()) < 0) {
    std::fprintf(stderr, "[DPDK] rte_eal_init failed: %s\n", rte_strerror(rte_errno));
    return false;
  }
  inited = true;
  return true;
}

}  // namespace dpdk

enum class DpdkMode : std::uint8_t { RxOnly, TxOnly, RxTx };

// NbRx/TxDesc 256 (was 1024): with one packet in flight the rings are pure cache
// footprint — 1024 RX CQEs alone is 64KB cycling through L2; 256 keeps CQ+SQ hot
// in L1/L2. PMD minimums (mlx5, i40e: 64) are far below; adjust_nb_rx_tx_desc
// still rounds up if a PMD ever needs more.
template<DpdkMode M,
         std::uint16_t QueueId   = 0,
         std::uint16_t NbRxDesc  = 256,
         std::uint16_t NbTxDesc  = 256,
         std::uint16_t BurstSize = 32,
         std::uint32_t NumMbufs  = 8191,
         std::uint16_t MaxFrame  = RTE_MBUF_DEFAULT_DATAROOM>
class DPDK {
  static_assert(BurstSize >= 1, "BurstSize must be >= 1");
  static_assert(NumMbufs > BurstSize, "NumMbufs must exceed BurstSize");

  static constexpr bool          HAS_RX     = (M == DpdkMode::RxOnly || M == DpdkMode::RxTx);
  static constexpr bool          HAS_TX     = (M == DpdkMode::TxOnly || M == DpdkMode::RxTx);
  static constexpr std::uint32_t MBUF_CACHE = 250;   // per-lcore mempool cache

  // mlx5 reusable-TX-mbuf path (see acquire()): only frames guaranteed to be
  // fully inlined into the WQE may reuse the buffer (default inlen_send = 290B,
  // txq_inline_max unset; 128 keeps a wide margin). The PMD's completion-time
  // refcnt decrements lag sends by at most NbTxDesc, so topping up whenever the
  // count dips below REFCNT_LOW (>> NbTxDesc) guarantees it never reaches zero.
  static constexpr std::uint32_t INLINE_SAFE_LEN = 128;
  static constexpr std::uint16_t REFCNT_LOW      = 0x2000;
  static constexpr std::uint16_t REFCNT_TOPUP    = 0x4000;

public:
  // ifname: kernel interface name ("enp1s0f1np1"). lcore: EAL main lcore — must
  // equal the core RuntimeSetup pins this process to (role.cpuCore). driver: the
  // kernel driver name (role.driver); "mlx5*" => bifurcated (leave on kernel),
  // anything else => Intel pass-through (unbind -> vfio-pci).
  DPDK(std::string_view ifname, int lcore, std::string_view driver) noexcept
    : m_ifname{ifname}, m_driver{driver}, m_lcore{lcore} {}

  DPDK(const DPDK&)            = delete;
  DPDK& operator=(const DPDK&) = delete;

  DPDK(DPDK&& o) noexcept
    : m_ifname{std::move(o.m_ifname)}, m_driver{std::move(o.m_driver)},
      m_lcore{o.m_lcore}, m_pci{std::move(o.m_pci)},
      m_port{o.m_port}, m_pool{std::exchange(o.m_pool, nullptr)}, m_mac{o.m_mac},
      m_txTemplate{o.m_txTemplate}, m_txTemplateLen{o.m_txTemplateLen},
      m_prepared{o.m_prepared}, m_started{std::exchange(o.m_started, false)},
      m_txReuse{std::exchange(o.m_txReuse, nullptr)},
      m_txReuseInline{o.m_txReuseInline} {}

  DPDK& operator=(DPDK&& o) noexcept {
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

  ~DPDK() { shutdown(); }

  // ── Cold path phase 1: resolve BDF + bind to vfio + register for EAL ────────
  // Split out of init() so a multi-port single process (the latency recorder)
  // can bind BOTH ports and register BOTH BDFs BEFORE the one EAL init runs —
  // EAL's -a allowlist probes vfio devices at init time, so every port must be
  // bound and registered first. Idempotent: safe to call once here and again
  // (no-op) from init(). For the single-port path init() calls this itself, so
  // behaviour is unchanged (`-a <one bdf>`).
  [[nodiscard]] bool prepare() noexcept {
    if (m_prepared) return true;

    // Resolve name -> BDF. First via the netdev (when still on the kernel
    // driver); if that's gone (already on vfio-pci from a previous run), derive
    // it from the firmware/BIOS-assigned predictable name (enp<bus>s<dev>f<func>).
    m_pci = pci::resolveBdf(m_ifname);
    if (m_pci.empty()) m_pci = pci::bdfFromName(m_ifname);
    if (m_pci.empty()) {
      std::fprintf(stderr, "[DPDK] %s: cannot resolve PCI BDF (no netdev, and name is "
                           "not an enp<bus>s<dev>f<func> form)\n", m_ifname.c_str());
      return false;
    }

    // Prepare the device per its binding model.
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

    // Register this BDF so the one process-wide EAL init allowlists it (and any
    // sibling port that also registered before EAL came up). mlx5 devargs:
    //  txq_mem_algn=0    — disable the consecutive-TxQ-umem feature (DPDK >= 25.x,
    //    a cache-alignment optimisation for MANY-queue workloads) — with two
    //    single-queue ports in one process its chunk allocation fails on the
    //    second port ("Failed to allocate consecutive memory for TxQs");
    //    0 = legacy per-queue allocation + own MRs.
    //  txqs_min_inline=0 — enable FULL Tx data inlining at 1 queue. By default
    //    inlining needs >= 8 Tx queues (mlx5_txq.c txq_set_params: "If there are
    //    few Tx queues it is prioritized to save CPU cycles and disable data
    //    inlining at all"); below that, ConnectX-4 Lx inlines only the forced
    //    18B L2 minimum and the NIC gather-DMAs the rest of the frame from host
    //    memory on EVERY send. With 0 the whole 64B frame rides inside the WQE —
    //    no payload DMA read. Verify via the tx burst mode print ("INLINE").
    //    A/B 2026-06-10: -520ns UNIFORM shift (median 2.325 -> 1.806us).
    //  rxq_cqe_comp_en=0 — disable CQE compression: completions arrive as plain
    //    64B CQEs, no decompression step in the RX poll path (compression saves
    //    PCIe bandwidth under load — pointless at one packet in flight).
    //    A/B 2026-06-10: neutral (median identical) — kept, zero cost.
    //  sq_db_nc=1 REJECTED (A/B 2026-06-10): non-cached doorbell made the whole
    //    band +10-20ns worse (NC serializes every store of the ~3-WQEBB BlueFlame
    //    write; the default WC mapping batches it). Default (0, WC) is optimal here.
    dpdk::addAllowedBdf(bifurcated
        ? m_pci + ",txq_mem_algn=0,txqs_min_inline=0,rxq_cqe_comp_en=0"
        : m_pci);
    // Full-inline TX => the reusable-mbuf fast path is safe (see acquire()).
    m_txReuseInline = bifurcated;
    m_prepared = true;
    return true;
  }

  // ── Cold path phase 2: EAL + port bring-up ─────────────────────────────────
  // doWaitLink=true (default): start the port AND block until link is up — used by
  // single-port-per-process modes (client/server, txgen). doWaitLink=false: start
  // only, do NOT wait — used by the single-recorder, which owns BOTH ports of a
  // loopback pair in one process and must start BOTH before waiting EITHER (a
  // loopback link only comes up once both PHYs are up). It then calls waitLink()
  // on each. See waitLink().
  [[nodiscard]] bool init(bool doWaitLink = true) noexcept {
    if (!prepare()) return false;   // no-op if the recorder already prepared us

    // EAL + port. ealInitOnce allowlists every BDF registered by prepare() so
    // far, so a multi-port process sees all its ports after the single init.
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

    // Single queue per direction, no RSS — all traffic on one queue (the DPDK
    // analog of AF_XDP's `ethtool -L combined 1`).
    rte_eth_conf conf{};
    conf.rxmode.mq_mode = RTE_ETH_MQ_RX_NONE;
    conf.txmode.mq_mode = RTE_ETH_MQ_TX_NONE;
    // Faster TX completion: bulk-free mbufs without per-mbuf checks (valid because
    // every TX mbuf comes from one pool with refcnt 1).
    if (info.tx_offload_capa & RTE_ETH_TX_OFFLOAD_MBUF_FAST_FREE)
      conf.txmode.offloads |= RTE_ETH_TX_OFFLOAD_MBUF_FAST_FREE;

    constexpr std::uint16_t nb_rxq = HAS_RX ? 1 : 0;
    constexpr std::uint16_t nb_txq = HAS_TX ? 1 : 0;
    if (rte_eth_dev_configure(m_port, nb_rxq, nb_txq, &conf) != 0) {
      std::fprintf(stderr, "[DPDK] dev_configure failed (port %u)\n", m_port);
      return false;
    }

    std::uint16_t nb_rxd = NbRxDesc, nb_txd = NbTxDesc;
    if (rte_eth_dev_adjust_nb_rx_tx_desc(m_port, &nb_rxd, &nb_txd) != 0) {
      std::fprintf(stderr, "[DPDK] adjust_nb_rx_tx_desc failed (port %u)\n", m_port);
      return false;
    }

    if constexpr (HAS_RX) {
      rte_eth_rxconf rxconf = info.default_rxconf;
      rxconf.offloads = conf.rxmode.offloads;
      if (rte_eth_rx_queue_setup(m_port, QueueId, nb_rxd, sid, &rxconf, m_pool) < 0) {
        std::fprintf(stderr, "[DPDK] rx_queue_setup failed\n");
        return false;
      }
    }
    if constexpr (HAS_TX) {
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

    // ── i40e: kill NIC-internal RX-writeback throttling ───────────────────────
    // The i40e silicon honors the per-vector Interrupt Throttle Rate even in
    // pure polling mode — ITR governs when completed RX descriptors are DMA'd
    // to host RAM. In DPDK 25.11 the PMD leaves ITR at I40E_QUEUE_ITR_INTERVAL_DEFAULT
    // (32us) and exposes no public API to change it (no rte_eth_dev_set_reg,
    // no devarg, nothing in rte_pmd_i40e.h). The kernel i40e driver hides this
    // via adaptive coalescing; we have to write the register ourselves.
    //
    // Proof this is the lever (2026-05-26, packet_mmap A/B with ethtool -C):
    //   adaptive off, rx-usecs 50 -> median 49.81us
    //   adaptive off, rx-usecs 0  -> median 11.88us
    //
    // Access: a second mmap of /sys/bus/pci/devices/<bdf>/resource0 via
    // ABTEdge's BackendBase. DPDK's vfio mapping is untouched; we get a
    // separate VMA pointing at the same physical BAR0.
    if (m_driver == "i40e") {
      const std::size_t bar0Size = pci::barSize(m_pci, 0);
      const std::string resPath  = "/sys/bus/pci/devices/" + m_pci + "/resource0";
      BackendBase bar0;
      if (bar0Size > 0 && bar0.open(resPath.c_str(), 0, bar0Size)) {
        // Register offsets from drivers/net/intel/i40e/base/i40e_register.h:
        //   I40E_PFINT_ITR0(_i)         = 0x00038000 + (_i) * 128
        //   I40E_PFINT_ITRN(_i, _INTPF) = 0x00030000 + (_i) * 2048 + (_INTPF) * 4
        // _i = ITR index (0 is the default RX index). Write 0 to both the misc-
        // vector ITR and the per-RX-vector ITR — either could be tied to our
        // queue depending on whether the PMD allocated an MSI-X for it.
        *bar0.registerPtr<std::uint32_t>(0x00038000) = 0;   // I40E_PFINT_ITR0(0)
        *bar0.registerPtr<std::uint32_t>(0x00030000) = 0;   // I40E_PFINT_ITRN(0, 0)
        std::fprintf(stderr, "[DPDK] %s: i40e ITR cleared (RX writeback throttling off)\n",
                     m_ifname.c_str());
      } else {
        std::fprintf(stderr, "[DPDK] %s: WARN: %s mmap failed — ITR not cleared, "
                             "expect ~30us median RTT\n", m_ifname.c_str(), resPath.c_str());
      }
    }

    // Report the PMD-selected burst modes — the cheap way to VERIFY a devarg took
    // effect (e.g. mlx5 Tx must report "INLINE" once txqs_min_inline=0 applies;
    // i40e reports its AVX512 vector paths here too).
    rte_eth_burst_mode bm{};
    if constexpr (M != DpdkMode::RxOnly)
      if (rte_eth_tx_burst_mode_get(m_port, QueueId, &bm) == 0)
        std::fprintf(stderr, "[DPDK] %s: tx burst mode: %s\n", m_ifname.c_str(), bm.info);
    if constexpr (M != DpdkMode::TxOnly)
      if (rte_eth_rx_burst_mode_get(m_port, QueueId, &bm) == 0)
        std::fprintf(stderr, "[DPDK] %s: rx burst mode: %s\n", m_ifname.c_str(), bm.info);

    // Promiscuous mode is NOT enabled: both ends of the test know the peer MAC
    // (toml carries client.mac + server.mac), so the NIC's MAC unicast filter does
    // the work. Promisc forces every received unicast through the full RX pipeline
    // regardless of dst MAC — a small but measurable NIC-internal cost we don't need.

    rte_ether_addr mac{};
    rte_eth_macaddr_get(m_port, &mac);
    std::memcpy(m_mac.data(), mac.addr_bytes, 6);

    if (doWaitLink) return waitLink();
    return true;
  }

  // Wait for link UP. SEPARATED from init() so a two-port loopback pair (the
  // single-recorder: port0 TX <-> port1 RX on ONE card via the DAC) can START
  // BOTH ports before waiting EITHER. A loopback link only comes up once BOTH
  // PHYs are up, so the old per-port "init+wait in sequence" left the first port
  // waiting for a peer that wasn't started yet (it would time out, then the second
  // port's start brought the pair up). Dispatch now calls init(false) on both,
  // then waitLink() on both.
  //
  // Re-run robustness: a prior run's dev_close parks the PHY DOWN. We give passive
  // autoneg a 2 s grace (don't tear down a link that's mid-negotiation), then
  // actively restart autoneg via rte_eth_dev_set_link_up() and re-kick every ~3 s
  // to recover the cold PHY — so re-runs come up without a manual driver rebind.
  // -ENOTSUP (bifurcated PMDs like mlx5, link already up) is expected/harmless.
  [[nodiscard]] bool waitLink() noexcept {
    rte_eth_link link{};
    for (int i = 0; i < 200; ++i) {                // up to ~20 s, 100 ms steps
      rte_eth_link_get_nowait(m_port, &link);
      if (link.link_status == RTE_ETH_LINK_UP) break;
      if (i == 20 || (i > 20 && (i % 30) == 0)) {  // after 2 s grace, then every ~3 s
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
    return true;   // non-fatal: the recorder's warmup retries tolerate a slow link
  }

  void shutdown() noexcept {
    if (!m_started) return;
    if (m_pendingRx) { rte_pktmbuf_free(m_pendingRx); m_pendingRx = nullptr; }
    // Clean stop ONLY. rte_eth_dev_stop() disables the queues/DMA/MSI-X and cancels
    // the periodic alarm, so the device CANNOT run unattended on vfio and contend on
    // PCIe with the next run's NIC. (An earlier no-dev_stop version left the igc
    // DMA-ing during a subsequent XXV710 run -> fat tail; this prevents that.)
    // We deliberately do NOT set_link_up here and do NOT dev_close: dev_stop parks
    // the PHY (i40e_dev_stop -> i40e_dev_set_link_down), so the next run's dev_start
    // brings the link up FROM PARKED = a one-time ~4.7ms autoneg re-sync ~85ms in —
    // which the recorder's 500k-packet (~2.5s) warmup ABSORBS before timing starts.
    // Device stays on vfio-pci (bind-once); the next process re-probes and
    // i40e_pf_reset() cleans the stopped state. Restore the kernel driver if ever
    // needed: sudo dpdk-devbind.py --bind=i40e <bdf> (or reboot).
    rte_eth_dev_stop(m_port);
    // After dev_stop the PMD has flushed the TX ring (its refcnt decrements are
    // done) — reset our topped-up count to 1 so the free actually reaches zero
    // and the mbuf returns to the pool.
    if constexpr (HAS_TX) {
      if (m_txReuse) {
        rte_mbuf_refcnt_set(m_txReuse, 1);
        rte_pktmbuf_free(m_txReuse);
        m_txReuse = nullptr;
      }
    }
    m_started = false;
  }

  [[nodiscard]] std::array<std::uint8_t, 6> macAddress() const noexcept { return m_mac; }

  // Store the header template so acquire() can stamp it into each fresh TX mbuf
  // (DPDK allocates a new mbuf per send — there is no fixed pre-stamped ring).
  // Client/txgen call this once; the reflector overwrites the whole frame.
  void prefillRing(std::span<const std::uint8_t> frameTemplate) noexcept requires (HAS_TX) {
    m_txTemplateLen = static_cast<std::uint16_t>(std::min<std::size_t>(frameTemplate.size(), MaxFrame));
    std::memcpy(m_txTemplate.data(), frameTemplate.data(), m_txTemplateLen);
  }

  // ── RX hot path ────────────────────────────────────────────────────────────
  // Peek-one over an internal burst cache; when drained, busy-poll a fresh
  // rte_eth_rx_burst. No syscalls — pure kernel bypass.
  [[nodiscard, gnu::always_inline, gnu::hot]]
  inline RxFrame tryReceive() noexcept requires (HAS_RX) {
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

  [[gnu::always_inline, gnu::hot]]
  inline void release() noexcept requires (HAS_RX) {
    rte_pktmbuf_free(m_pendingRx);
    m_pendingRx = nullptr;
    ++m_rxIdx;
  }

  // ── TX hot path ────────────────────────────────────────────────────────────
  // acquire() returns a fresh mbuf's data pointer (header pre-stamped from
  // prefillRing); caller writes payload; commit() bursts it. On success the PMD
  // owns and later frees the mbuf.
  //
  // mlx5 full-inline fast path (m_txReuseInline): with txqs_min_inline=0 the PMD
  // COPIES the whole small frame into the WQE inside the tx_burst call itself, so
  // the buffer is reusable the moment commit() returns — ONE mbuf re-sent forever,
  // no per-round alloc/free, no mempool cache traffic. The PMD still decrements
  // refcnt once per send when it processes completions (which happens inside our
  // own tx_burst calls — single-threaded, so the plain refcnt read is race-free);
  // we top the 16-bit refcnt back up before it can drain (decrements lag sends by
  // at most the TX ring depth, far less than REFCNT_LOW). Gated to frames small
  // enough to be guaranteed inlined (default inlen_send = 290B; we stay well under).
  [[nodiscard, gnu::always_inline, gnu::hot]]
  inline std::uint8_t* acquire(std::uint32_t frameLen) noexcept requires (HAS_TX) {
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

  [[gnu::always_inline, gnu::hot]]
  inline void commit() noexcept requires (HAS_TX) {
    if (rte_eth_tx_burst(m_port, QueueId, &m_pendingTx, 1) == 0) [[unlikely]]
      rte_pktmbuf_free(m_pendingTx);   // ring full — drop, never leak
    m_pendingTx = nullptr;
  }

  [[nodiscard, gnu::always_inline, gnu::hot]]
  inline bool send(std::span<const std::uint8_t> frame) noexcept requires (HAS_TX) {
    auto* buf = acquire(static_cast<std::uint32_t>(frame.size()));
    if (!buf) [[unlikely]] return false;
    std::memcpy(buf, frame.data(), frame.size());
    commit();
    return true;
  }

private:
  // EAL is process-global: init exactly once. Each process allowlists only its own
  // device (-a) on its own lcore (-l) with a unique --file-prefix, so the server
  // and client can run as two DPDK primaries on the same looped-DAC host.
  // Delegates to the process-global dpdk::ealInit (see top of file). The single-
  // port path calls this with its own BDF and gets `-a <bdf>` exactly as before;
  // the single-recorder pre-registers both ports' BDFs via dpdk::addAllowedBdf()
  // so the one EAL init allowlists both. --force-max-simd-bitwidth=512 (set in
  // ealInit) raises DPDK's runtime SIMD cap so the i40e PMD picks the AVX-512
  // RX/TX path; on Rocket Lake there is a small sustained-AVX-512 clock penalty
  // but no transition jitter for a continuous busy-poll workload.
  static bool ealInitOnce(const std::string& pci, int lcore) noexcept {
    return dpdk::ealInit(pci, lcore);
  }

  static bool writeSysfs(const std::string& path, std::string_view val) noexcept {
    const int fd = ::open(path.c_str(), O_WRONLY | O_CLOEXEC);
    if (fd < 0) return false;
    const ssize_t n = ::write(fd, val.data(), val.size());
    ::close(fd);
    return n == static_cast<ssize_t>(val.size());
  }

  // Unbind whatever kernel driver owns the device and bind vfio-pci (driver_override
  // + drivers_probe, as dpdk-devbind does). Idempotent if already on vfio-pci.
  static bool bindToVfio(const std::string& bdf) noexcept {
    if (::access("/sys/bus/pci/drivers/vfio-pci", F_OK) != 0)
      (void)std::system("modprobe vfio-pci");
    if (pci::currentDriver(bdf) == "vfio-pci") return true;
    const std::string dev = "/sys/bus/pci/devices/" + bdf;
    writeSysfs(dev + "/driver_override", "vfio-pci");
    writeSysfs(dev + "/driver/unbind", bdf);          // best-effort if bound
    return writeSysfs("/sys/bus/pci/drivers_probe", bdf);
  }

  std::string                        m_ifname;
  std::string                        m_driver;
  int                                m_lcore{};
  std::string                        m_pci;             // resolved BDF

  std::uint16_t                      m_port{0};
  rte_mempool*                       m_pool{nullptr};
  std::array<std::uint8_t, 6>        m_mac{};

  std::array<std::uint8_t, MaxFrame> m_txTemplate{};    // header, stamped per mbuf
  std::uint16_t                      m_txTemplateLen{0};

  rte_mbuf*                          m_rxBurst[BurstSize]{};   // RX burst cache
  std::uint16_t                      m_rxCount{0};
  std::uint16_t                      m_rxIdx{0};
  rte_mbuf*                          m_pendingRx{nullptr};
  rte_mbuf*                          m_pendingTx{nullptr};

  bool                               m_prepared{false};
  bool                               m_started{false};

  rte_mbuf*                          m_txReuse{nullptr};       // mlx5 full-inline: the one reusable TX mbuf
  bool                               m_txReuseInline{false};   // set in prepare(): mlx5/bifurcated only
};
