//
// DPDK — one DPDK transport for every supported NIC (Intel i40e XXV710, Intel
// igc I225-V, Mellanox mlx5 ConnectX-4) through the unified rte_ethdev API.
//
// The hot path (rte_eth_rx_burst / rte_eth_tx_burst) is identical across all
// PMDs. The ONLY per-vendor difference is device binding, which is an ops step
// OUTSIDE this code:
//   * Intel i40e/igc : unbind kernel driver -> bind vfio-pci (needs IOMMU).
//   * Mellanox mlx5  : stays on kernel mlx5_core (bifurcated); needs rdma-core.
// The device is addressed by PCI id (e.g. "0000:01:00.1") — Intel ports lose
// their netdev name once on vfio-pci, so a name won't do.
//
// Satisfies the TxRing/RxRing concepts (tryReceive/release + acquire/commit/
// send/prefillRing), so it drops into TransportDispatch as transport = "dpdk".
//
// Every knob that can be fixed at compile time is a template parameter — this is
// the ultra-low-latency path, so the hot loop carries no runtime configuration.
// Only the PCI id and the EAL lcore are runtime (they come from the test config).
//
// Target: DPDK 25.11 LTS. Compiled only when ABTRDA3_HAS_DPDK is defined
// (cmake -DABTRDA3_ENABLE_DPDK=ON).
//
#pragma once

#include "RxFrame.hpp"

#include <rte_eal.h>
#include <rte_errno.h>
#include <rte_ethdev.h>
#include <rte_mbuf.h>
#include <rte_mempool.h>
#include <rte_ether.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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

  static constexpr bool          HAS_RX      = (M == DpdkMode::RxOnly || M == DpdkMode::RxTx);
  static constexpr bool          HAS_TX      = (M == DpdkMode::TxOnly || M == DpdkMode::RxTx);
  static constexpr std::uint32_t MBUF_CACHE  = 250;   // per-lcore mempool cache

public:
  // pciAddr: DPDK device id ("0000:01:00.1"). lcore: EAL main lcore — must equal
  // the core RuntimeSetup pins this process to (role.cpuCore) so EAL keeps us put.
  DPDK(std::string_view pciAddr, int lcore) noexcept
    : m_pci{pciAddr}, m_lcore{lcore} {}

  DPDK(const DPDK&)            = delete;
  DPDK& operator=(const DPDK&) = delete;

  DPDK(DPDK&& o) noexcept
    : m_pci{std::move(o.m_pci)}, m_lcore{o.m_lcore}, m_port{o.m_port},
      m_pool{std::exchange(o.m_pool, nullptr)}, m_mac{o.m_mac},
      m_txTemplate{o.m_txTemplate}, m_txTemplateLen{o.m_txTemplateLen},
      m_started{std::exchange(o.m_started, false)} {}

  DPDK& operator=(DPDK&& o) noexcept {
    if (this != &o) {
      shutdown();
      m_pci           = std::move(o.m_pci);
      m_lcore         = o.m_lcore;
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

  // ── Cold path: EAL + port bring-up ─────────────────────────────────────────
  [[nodiscard]] bool init() noexcept {
    if (!ealInitOnce(m_pci, m_lcore)) return false;

    if (rte_eth_dev_get_port_by_name(m_pci.c_str(), &m_port) != 0) {
      std::fprintf(stderr, "[DPDK] no port for %s (bound to vfio-pci / mlx5_core?)\n",
                   m_pci.c_str());
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
    // equivalent of AF_XDP's `ethtool -L combined 1`).
    rte_eth_conf conf{};
    conf.rxmode.mq_mode = RTE_ETH_MQ_RX_NONE;
    conf.txmode.mq_mode = RTE_ETH_MQ_TX_NONE;
    // Faster TX completion: lets the PMD bulk-free mbufs without per-mbuf checks
    // (valid because every TX mbuf comes from one pool with refcnt 1).
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

    rte_eth_promiscuous_enable(m_port);   // defensive on a dedicated test link

    rte_ether_addr mac{};
    rte_eth_macaddr_get(m_port, &mac);
    std::memcpy(m_mac.data(), mac.addr_bytes, 6);

    rte_eth_link link{};
    rte_eth_link_get_nowait(m_port, &link);
    std::fprintf(stderr,
        "[DPDK] port %u (%s) %s %u Mbps  MAC %02x:%02x:%02x:%02x:%02x:%02x\n",
        m_port, info.driver_name,
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
  }

  [[nodiscard]] std::array<std::uint8_t, 6> macAddress() const noexcept { return m_mac; }

  // Store the header template so acquire() can stamp it into each fresh TX mbuf
  // (DPDK allocates a new mbuf per send — there is no fixed pre-stamped ring).
  // Client/txgen call this once; the reflector overwrites the whole frame and
  // never does.
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
  // EAL is process-global: init exactly once. Each process allowlists only its
  // own device (-a) on its own lcore (-l) with a unique --file-prefix, so the
  // server and client can run as two DPDK primaries on the same looped-DAC host.
  static bool ealInitOnce(const std::string& pci, int lcore) noexcept {
    static bool inited = false;
    if (inited) return true;

    std::string prefix = "abtrda3_" + pci;
    for (char& c : prefix) if (c == ':' || c == '.') c = '_';
    std::string lcoreStr = std::to_string(lcore);

    std::array<std::string, 11> args = {
      "abtrda3", "-l", lcoreStr, "--main-lcore", lcoreStr,
      "-n", "4", "-a", pci, "--file-prefix", prefix
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

  std::string                        m_pci;
  int                                m_lcore{};
  std::uint16_t                      m_port{0};
  rte_mempool*                       m_pool{nullptr};
  std::array<std::uint8_t, 6>        m_mac{};

  std::array<std::uint8_t, MaxFrame> m_txTemplate{};   // header, stamped per mbuf
  std::uint16_t                      m_txTemplateLen{0};

  rte_mbuf*                          m_rxBurst[BurstSize]{};   // RX burst cache
  std::uint16_t                      m_rxCount{0};
  std::uint16_t                      m_rxIdx{0};
  rte_mbuf*                          m_pendingRx{nullptr};
  rte_mbuf*                          m_pendingTx{nullptr};

  bool                               m_started{false};
};
