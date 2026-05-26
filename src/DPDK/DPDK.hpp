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

enum class DpdkMode : std::uint8_t { RxOnly, TxOnly, RxTx };

template<DpdkMode M,
         std::uint16_t QueueId   = 0,
         std::uint16_t NbRxDesc  = 1024,
         std::uint16_t NbTxDesc  = 1024,
         std::uint16_t BurstSize = 32,
         std::uint32_t NumMbufs  = 8191,
         std::uint16_t MaxFrame  = RTE_MBUF_DEFAULT_DATAROOM>
class DPDK {
  static_assert(BurstSize >= 1, "BurstSize must be >= 1");
  static_assert(NumMbufs > BurstSize, "NumMbufs must exceed BurstSize");

  static constexpr bool          HAS_RX     = (M == DpdkMode::RxOnly || M == DpdkMode::RxTx);
  static constexpr bool          HAS_TX     = (M == DpdkMode::TxOnly || M == DpdkMode::RxTx);
  static constexpr std::uint32_t MBUF_CACHE = 250;   // per-lcore mempool cache

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
      m_started{std::exchange(o.m_started, false)} {}

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
      m_started       = std::exchange(o.m_started, false);
    }
    return *this;
  }

  ~DPDK() { shutdown(); }

  // ── Cold path: resolve + bind + EAL + port bring-up ────────────────────────
  [[nodiscard]] bool init() noexcept {
    // 1. Resolve name -> BDF. First via the netdev (when still on the kernel
    //    driver); if that's gone (already on vfio-pci from a previous run), derive
    //    it from the firmware/BIOS-assigned predictable name (enp<bus>s<dev>f<func>).
    m_pci = pci::resolveBdf(m_ifname);
    if (m_pci.empty()) m_pci = pci::bdfFromName(m_ifname);
    if (m_pci.empty()) {
      std::fprintf(stderr, "[DPDK] %s: cannot resolve PCI BDF (no netdev, and name is "
                           "not an enp<bus>s<dev>f<func> form)\n", m_ifname.c_str());
      return false;
    }

    // 2. Prepare the device per its binding model.
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

    // 3. EAL + port.
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

    // Promiscuous mode is NOT enabled: both ends of the test know the peer MAC
    // (toml carries client.mac + server.mac), so the NIC's MAC unicast filter does
    // the work. Promisc forces every received unicast through the full RX pipeline
    // regardless of dst MAC — a small but measurable NIC-internal cost we don't need.

    rte_ether_addr mac{};
    rte_eth_macaddr_get(m_port, &mac);
    std::memcpy(m_mac.data(), mac.addr_bytes, 6);

    rte_eth_link link{};
    rte_eth_link_get_nowait(m_port, &link);
    std::fprintf(stderr,
        "[DPDK] %s port %u (%s) %s %u Mbps  MAC %02x:%02x:%02x:%02x:%02x:%02x\n",
        m_ifname.c_str(), m_port, info.driver_name,
        link.link_status == RTE_ETH_LINK_UP ? "UP" : "DOWN", link.link_speed,
        m_mac[0], m_mac[1], m_mac[2], m_mac[3], m_mac[4], m_mac[5]);
    return true;
  }

  void shutdown() noexcept {
    if (!m_started) return;
    if (m_pendingRx) { rte_pktmbuf_free(m_pendingRx); m_pendingRx = nullptr; }
    rte_eth_dev_stop(m_port);
    rte_eth_dev_close(m_port);
    m_started = false;
    // Deliberately LEAVE the device on vfio-pci (standard DPDK "bind once" model).
    // Rebinding the kernel driver here HANGS the destructor: the sysfs driver/unbind
    // blocks until vfio releases the device, but EAL still holds it open (we don't
    // call rte_eal_cleanup), so the write never returns. Re-runs resolve the BDF from
    // the predictable name (pci::bdfFromName) since the netdev is gone. Restore the
    // kernel driver manually if needed:  sudo dpdk-devbind.py --bind=i40e <bdf>  (or reboot).
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
  [[nodiscard, gnu::always_inline, gnu::hot]]
  inline std::uint8_t* acquire(std::uint32_t frameLen) noexcept requires (HAS_TX) {
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
  static bool ealInitOnce(const std::string& pci, int lcore) noexcept {
    static bool inited = false;
    if (inited) return true;

    std::string prefix = "abtrda3_" + pci;
    for (char& c : prefix) if (c == ':' || c == '.') c = '_';
    std::string lcoreStr = std::to_string(lcore);

    // --force-max-simd-bitwidth=512 raises DPDK's runtime SIMD cap
    // (RTE_VECT_DEFAULT_SIMD_BITWIDTH = 256 by default, i.e. AVX2). With this set,
    // i40e PMD's ci_get_x86_max_simd_bitwidth() picks the AVX-512 RX/TX path when
    // the CPU supports it. CAVEAT: Rocket Lake (i7-11700F) has a small AVX-512
    // frequency penalty — under sustained AVX-512 use, the core may steady-state
    // 100-400 MHz below the AVX2 turbo bin. For our busy-poll workload that runs
    // AVX-512 continuously there's no transition jitter; whether the per-burst
    // SIMD win outweighs the clock loss is workload-specific. Measure both.
    std::array<std::string, 12> args = {
      "abtrda3", "-l", lcoreStr, "--main-lcore", lcoreStr,
      "-n", "4", "-a", pci, "--file-prefix", prefix,
      "--force-max-simd-bitwidth=512"
    };
    std::array<char*, args.size() + 1> argv{};
    for (std::size_t i = 0; i < args.size(); ++i) argv[i] = args[i].data();
    argv[args.size()] = nullptr;

    if (rte_eal_init(static_cast<int>(args.size()), argv.data()) < 0) {
      std::fprintf(stderr, "[DPDK] rte_eal_init failed: %s\n", rte_strerror(rte_errno));
      return false;
    }
    inited = true;
    return true;
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

  bool                               m_started{false};
};
