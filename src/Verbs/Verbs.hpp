//
// Verbs — raw-verbs (libibverbs RAW_PACKET QP) ultra-low-latency transport.
//
// Rationale: the DPDK/mlx5 path measures ~1.80us one-way at its architectural
// floor; perf shows the host cost concentrated in the ethdev/mbuf layers around
// the PMD. This transport talks to the SAME mlx5 hardware through the verbs
// stack directly (the path behind vendor "sub-microsecond" latency numbers):
//   TX: ibv_post_send + IBV_SEND_INLINE — the provider builds the WQE and, for
//       small inline WQEs, writes it through the BlueFlame doorbell itself
//       (rdma-core providers/mlx5/qp.c post_send_db: nreq==1 && inl && size <=
//       bf->buf_size/16 => mlx5_bf_copy) — zero NIC reads, same mechanism that
//       won -520ns on the PMD, minus rte_ethdev/rte_mbuf on top.
//   RX: pre-posted recv WRs into a flat slot array + ibv_poll_cq busy-poll;
//       steering by destination MAC via ibv_create_flow (same NIC unicast
//       filter semantics as the DPDK path — no promiscuous).
//
// mlx5-ONLY by construction: RAW_PACKET QPs need an RDMA-capable NIC + verbs
// provider. The i40e/igc ports have none (Intel irdma covers E810/X722 only) —
// they keep the DPDK transport; the dispatch selects per-port via the TOML
// (transport = "verbs"). Every tunable is a compile-time template parameter
// (queue depths, signal pacing, inline ceiling); only the interface name is
// runtime config. Satisfies the TxRing/RxRing concepts (tryReceive/release +
// acquire/commit/send/prefillRing) like every other transport.
//
// Send-CQ discipline (deviation from perftest's blocking cq_mod drain, which
// would stall 1-in-N rounds and fatten the tail): WQEs are SIGNALED every
// SignalEvery-th send; completions are reaped LAZILY and NON-BLOCKINGLY one
// pacing period later — by then the CQE has been resident for ~SignalEvery
// round-trips, so the poll returns instantly and nothing on the hot path ever
// waits. The SQ can hold 2*SignalEvery in-flight WQEs max, sized well under
// SqDepth by static_assert.
//

#ifndef ABTRDA3_VERBS_HPP
#define ABTRDA3_VERBS_HPP

#include <infiniband/verbs.h>

#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <span>
#include <string>
#include <string_view>

#include <fmt/core.h>

#include "../common/RxFrame.hpp"

enum class VerbsMode : std::uint8_t { RxOnly, TxOnly, RxTx };

template<VerbsMode M,
         std::uint8_t  PortNum     = 1,     // HCA port (each PCIe fn = 1-port HCA)
         std::uint16_t SqDepth     = 256,   // send WQEs the SQ can hold
         std::uint16_t RqDepth     = 256,   // pre-posted recv WRs / RX slots
         std::uint16_t SignalEvery = 64,    // signaled-CQE pacing (power of two)
         std::uint16_t MaxInline   = 128,   // requested max_inline_data
         std::uint16_t MaxFrame    = 2048>  // RX slot stride
class Verbs {
  static constexpr bool HAS_RX = (M == VerbsMode::RxOnly || M == VerbsMode::RxTx);
  static constexpr bool HAS_TX = (M == VerbsMode::TxOnly || M == VerbsMode::RxTx);

  static_assert((SignalEvery & (SignalEvery - 1)) == 0, "SignalEvery must be a power of two");
  static_assert(SignalEvery * 4 <= SqDepth, "SQ must hold several signal periods");

public:
  explicit Verbs(std::string_view ifname) noexcept : m_ifname{ifname} {}

  Verbs(const Verbs&)            = delete;
  Verbs& operator=(const Verbs&) = delete;
  Verbs(Verbs&&)                 = delete;
  Verbs& operator=(Verbs&&)      = delete;

  ~Verbs() { shutdown(); }

  // ── Cold path: device open -> PD/CQ/QP -> RTS -> MR -> recv ring -> flow ───
  [[nodiscard]] bool init() noexcept {
    // netdev -> RDMA device name: /sys/class/net/<if>/device/infiniband/<dev>
    const std::string ibdev = ibdevFromNetdev(m_ifname);
    if (ibdev.empty()) {
      fmt::println(stderr, "[Verbs] {}: no RDMA device (mlx5 only — check "
                           "/sys/class/net/{}/device/infiniband)", m_ifname, m_ifname);
      return false;
    }
    if (!readMac()) return false;

    int num = 0;
    ibv_device** list = ibv_get_device_list(&num);
    for (int i = 0; i < num; ++i)
      if (ibdev == ibv_get_device_name(list[i])) { m_ctx = ibv_open_device(list[i]); break; }
    ibv_free_device_list(list);
    if (!m_ctx) { fmt::println(stderr, "[Verbs] {}: open '{}' failed", m_ifname, ibdev); return false; }

    m_pd = ibv_alloc_pd(m_ctx);
    if (!m_pd) { fmt::println(stderr, "[Verbs] {}: alloc_pd failed", m_ifname); return false; }

    // Both CQs always exist (QP creation wants them); the unused side stays empty.
    m_sendCq = ibv_create_cq(m_ctx, SqDepth, nullptr, nullptr, 0);
    m_recvCq = ibv_create_cq(m_ctx, RqDepth, nullptr, nullptr, 0);
    if (!m_sendCq || !m_recvCq) { fmt::println(stderr, "[Verbs] {}: create_cq failed", m_ifname); return false; }

    // One flat registered region: [TX slot][RX slot 0..RqDepth-1].
    const std::size_t bufSize = static_cast<std::size_t>(MaxFrame) * (1 + RqDepth);
    if (posix_memalign(reinterpret_cast<void**>(&m_buf), 4096, bufSize) != 0) return false;
    std::memset(m_buf, 0, bufSize);
    m_mr = ibv_reg_mr(m_pd, m_buf, bufSize, IBV_ACCESS_LOCAL_WRITE);
    if (!m_mr) { fmt::println(stderr, "[Verbs] {}: reg_mr failed (RLIMIT_MEMLOCK?)", m_ifname); return false; }

    ibv_qp_init_attr qa{};
    qa.qp_type             = IBV_QPT_RAW_PACKET;
    qa.send_cq             = m_sendCq;
    qa.recv_cq             = m_recvCq;
    qa.cap.max_send_wr     = HAS_TX ? SqDepth : 1;
    qa.cap.max_recv_wr     = HAS_RX ? RqDepth : 1;
    qa.cap.max_send_sge    = 1;
    qa.cap.max_recv_sge    = 1;
    qa.cap.max_inline_data = HAS_TX ? MaxInline : 0;
    m_qp = ibv_create_qp(m_pd, &qa);
    if (!m_qp) { fmt::println(stderr, "[Verbs] {}: create_qp(RAW_PACKET) failed (root/CAP_NET_RAW?)", m_ifname); return false; }
    m_maxInline = qa.cap.max_inline_data;   // provider may round up

    // RAW_PACKET state machine needs no peer info: INIT(+port) -> RTR -> RTS.
    ibv_qp_attr at{};
    at.qp_state = IBV_QPS_INIT;
    at.port_num = PortNum;
    if (ibv_modify_qp(m_qp, &at, IBV_QP_STATE | IBV_QP_PORT) != 0) { qpErr("INIT"); return false; }
    at = {}; at.qp_state = IBV_QPS_RTR;
    if (ibv_modify_qp(m_qp, &at, IBV_QP_STATE) != 0) { qpErr("RTR"); return false; }
    if constexpr (HAS_TX) {
      at = {}; at.qp_state = IBV_QPS_RTS;
      if (ibv_modify_qp(m_qp, &at, IBV_QP_STATE) != 0) { qpErr("RTS"); return false; }
    }

    if constexpr (HAS_RX) {
      for (std::uint16_t s = 0; s < RqDepth; ++s)
        if (!postRecv(s)) { fmt::println(stderr, "[Verbs] {}: post_recv({}) failed", m_ifname, s); return false; }

      // Steer frames addressed to this port's MAC to the QP (unicast filter
      // semantics — without a flow rule a RAW_PACKET QP receives nothing).
      struct { ibv_flow_attr attr; ibv_flow_spec_eth eth; } fr{};
      fr.attr.type         = IBV_FLOW_ATTR_NORMAL;
      fr.attr.size         = sizeof(fr);
      fr.attr.priority     = 0;
      fr.attr.num_of_specs = 1;
      fr.attr.port         = PortNum;
      fr.eth.type          = IBV_FLOW_SPEC_ETH;
      fr.eth.size          = sizeof(ibv_flow_spec_eth);
      std::memcpy(fr.eth.val.dst_mac, m_mac.data(), 6);
      std::memset(fr.eth.mask.dst_mac, 0xFF, 6);
      m_flow = ibv_create_flow(m_qp, &fr.attr);
      if (!m_flow) { fmt::println(stderr, "[Verbs] {}: create_flow failed", m_ifname); return false; }
    }

    fmt::println(stderr, "[Verbs] {} ({}) port {} ready — inline {}B, sq {} rq {} signal 1/{}",
                 m_ifname, ibdev, PortNum, m_maxInline, SqDepth, RqDepth, SignalEvery);
    return true;
  }

  [[nodiscard]] std::array<std::uint8_t, 6> macAddress() const noexcept { return m_mac; }

  // ── TX ──────────────────────────────────────────────────────────────────────
  // One frame slot, reused forever: IBV_SEND_INLINE copies the frame into the
  // WQE inside ibv_post_send, so the buffer is free again the moment it returns
  // (same invariant as the DPDK reusable-mbuf path).
  void prefillRing(std::span<const std::uint8_t> frameTemplate) noexcept requires (HAS_TX) {
    std::memcpy(m_buf, frameTemplate.data(),
                std::min<std::size_t>(frameTemplate.size(), MaxFrame));
  }

  [[nodiscard, gnu::always_inline, gnu::hot]]
  inline std::uint8_t* acquire(std::uint32_t frameLen) noexcept requires (HAS_TX) {
    m_txLen = frameLen;
    return m_buf;                       // slot 0 = the TX frame
  }

  [[gnu::always_inline, gnu::hot]]
  inline void commit() noexcept requires (HAS_TX) {
    ibv_sge sge{ .addr = reinterpret_cast<std::uintptr_t>(m_buf),
                 .length = m_txLen, .lkey = m_mr->lkey };
    ibv_send_wr wr{};
    wr.sg_list    = &sge;
    wr.num_sge    = 1;
    wr.opcode     = IBV_WR_SEND;
    wr.send_flags = IBV_SEND_INLINE;
    if ((++m_txCount & (SignalEvery - 1)) == 0) [[unlikely]] {
      wr.send_flags |= IBV_SEND_SIGNALED;
      ++m_unreaped;
    }
    ibv_send_wr* bad = nullptr;
    if (ibv_post_send(m_qp, &wr, &bad) != 0) [[unlikely]]
      fmt::println(stderr, "[Verbs] {}: post_send failed", m_ifname);

    // Lazy reap: the CQE we poll for was signaled a full pacing period ago
    // (~SignalEvery round-trips resident) — this poll never blocks in practice.
    if (m_unreaped >= 2) [[unlikely]] {
      ibv_wc wc;
      if (ibv_poll_cq(m_sendCq, 1, &wc) > 0) --m_unreaped;
      // Backstop (never taken in steady state): refuse to let the SQ overflow.
      while (m_unreaped >= SqDepth / SignalEvery - 1)
        if (ibv_poll_cq(m_sendCq, 1, &wc) > 0) --m_unreaped;
    }
  }

  [[nodiscard, gnu::always_inline, gnu::hot]]
  inline bool send(std::span<const std::uint8_t> frame) noexcept requires (HAS_TX) {
    auto* dst = acquire(static_cast<std::uint32_t>(frame.size()));
    std::memcpy(dst, frame.data(), frame.size());
    commit();
    return true;
  }

  // ── RX ──────────────────────────────────────────────────────────────────────
  // Busy-poll the recv CQ; a completion's wr_id is the slot index, byte_len the
  // full L2 frame length (FCS stripped by HW). release() re-posts the slot.
  [[nodiscard, gnu::always_inline, gnu::hot]]
  inline RxFrame tryReceive() noexcept requires (HAS_RX) {
    ibv_wc wc;
    if (ibv_poll_cq(m_recvCq, 1, &wc) <= 0) [[likely]] return {};
    if (wc.status != IBV_WC_SUCCESS) [[unlikely]] {
      // re-post the slot and keep going; surfaces as a dropped frame
      postRecv(static_cast<std::uint16_t>(wc.wr_id));
      return {};
    }
    m_lastSlot = static_cast<std::uint16_t>(wc.wr_id);
    return { .data   = { rxSlot(m_lastSlot), wc.byte_len },
             .sec    = 0,
             .nsec   = 0,
             .status = 1 };
  }

  [[gnu::always_inline, gnu::hot]]
  inline void release() noexcept requires (HAS_RX) {
    postRecv(m_lastSlot);
  }

  void shutdown() noexcept {
    if (m_flow)   { ibv_destroy_flow(m_flow);   m_flow = nullptr; }
    if (m_qp)     { ibv_destroy_qp(m_qp);       m_qp = nullptr; }
    if (m_sendCq) { ibv_destroy_cq(m_sendCq);   m_sendCq = nullptr; }
    if (m_recvCq) { ibv_destroy_cq(m_recvCq);   m_recvCq = nullptr; }
    if (m_mr)     { ibv_dereg_mr(m_mr);         m_mr = nullptr; }
    if (m_buf)    { std::free(m_buf);           m_buf = nullptr; }
    if (m_pd)     { ibv_dealloc_pd(m_pd);       m_pd = nullptr; }
    if (m_ctx)    { ibv_close_device(m_ctx);    m_ctx = nullptr; }
  }

private:
  [[gnu::always_inline]]
  std::uint8_t* rxSlot(std::uint16_t s) const noexcept {
    return m_buf + static_cast<std::size_t>(MaxFrame) * (1 + s);
  }

  [[gnu::always_inline]]
  bool postRecv(std::uint16_t s) noexcept {
    ibv_sge sge{ .addr = reinterpret_cast<std::uintptr_t>(rxSlot(s)),
                 .length = MaxFrame, .lkey = m_mr->lkey };
    ibv_recv_wr wr{};
    wr.wr_id   = s;
    wr.sg_list = &sge;
    wr.num_sge = 1;
    ibv_recv_wr* bad = nullptr;
    return ibv_post_recv(m_qp, &wr, &bad) == 0;
  }

  void qpErr(const char* st) const noexcept {
    fmt::println(stderr, "[Verbs] {}: modify_qp -> {} failed", m_ifname, st);
  }

  static std::string ibdevFromNetdev(const std::string& ifname) noexcept {
    const std::string dir = "/sys/class/net/" + ifname + "/device/infiniband";
    std::string out;
    if (DIR* d = opendir(dir.c_str())) {
      while (dirent* e = readdir(d))
        if (e->d_name[0] != '.') { out = e->d_name; break; }
      closedir(d);
    }
    return out;
  }

  bool readMac() noexcept {
    const std::string p = "/sys/class/net/" + m_ifname + "/address";
    FILE* f = std::fopen(p.c_str(), "r");
    if (!f) { fmt::println(stderr, "[Verbs] {}: cannot read MAC", m_ifname); return false; }
    unsigned b[6]{};
    const int n = std::fscanf(f, "%x:%x:%x:%x:%x:%x", &b[0],&b[1],&b[2],&b[3],&b[4],&b[5]);
    std::fclose(f);
    if (n != 6) return false;
    for (int i = 0; i < 6; ++i) m_mac[i] = static_cast<std::uint8_t>(b[i]);
    return true;
  }

  std::string                 m_ifname;
  std::array<std::uint8_t, 6> m_mac{};

  ibv_context*  m_ctx{nullptr};
  ibv_pd*       m_pd{nullptr};
  ibv_cq*       m_sendCq{nullptr};
  ibv_cq*       m_recvCq{nullptr};
  ibv_qp*       m_qp{nullptr};
  ibv_mr*       m_mr{nullptr};
  ibv_flow*     m_flow{nullptr};
  std::uint8_t* m_buf{nullptr};

  std::uint32_t m_maxInline{0};
  std::uint32_t m_txLen{0};
  std::uint64_t m_txCount{0};
  std::uint32_t m_unreaped{0};
  std::uint16_t m_lastSlot{0};
};

#endif // ABTRDA3_VERBS_HPP
