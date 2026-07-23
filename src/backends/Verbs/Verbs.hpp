// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Areeb Sherjil

#ifndef ABTRDA3_VERBS_HPP
#define ABTRDA3_VERBS_HPP

#include <infiniband/verbs.h>
#include <infiniband/mlx5dv.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <endian.h>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <fmt/core.h>

#include "../common/RxFrame.hpp"

// =============================================================================
// Verbs — raw-verbs (libibverbs RAW_PACKET QP) ultra-low-latency transport.
//
// WHY: the DPDK/mlx5 path bottoms out at ~1.80us one-way; perf shows its host
// cost concentrated in the ethdev/mbuf layers around the PMD. This transport
// drives the SAME mlx5 hardware through the verbs stack directly (the path
// behind vendor "sub-microsecond" latency numbers). Measured 2026-06-10 on
// ConnectX-4 Lx (230.7M samples): median 1.266us / P99.999 1.813us / Max 2.42us
// — -30% vs the tuned DPDK path.
//
// SCOPE: mlx5/RDMA-capable NICs ONLY, by construction — RAW_PACKET QPs need a
// verbs provider; Intel i40e/igc have none (irdma covers E810/X722 only) and
// keep the DPDK transport. The dispatch selects per-port via the TOML
// (transport = "verbs"). Kernel driver keeps the port (bifurcated, like
// DPDK/mlx5); ports must be admin-UP. Requires RLIMIT_MEMLOCK unlimited
// (ibv_reg_mr charges it — /etc/security/limits.d/99-rdma-memlock.conf).
//
// DESIGN:
//   * One IBV_QPT_RAW_PACKET QP per port object. State machine for raw packet
//     QPs needs no peer info: INIT(+port) -> RTR -> RTS (RTS only if TX).
//   * TX = ONE reused frame slot + IBV_SEND_INLINE: the provider copies the
//     frame into the WQE inside ibv_post_send, so the buffer is free again the
//     moment it returns; for small inline WQEs the provider writes the WQE
//     through the BlueFlame doorbell itself (rdma-core providers/mlx5/qp.c
//     post_send_db: nreq==1 && inl && small => mlx5_bf_copy) — zero NIC reads
//     per send, the same mechanism that won -520ns on the PMD (txqs_min_inline).
//   * RX = RqDepth pre-posted recv WRs into a flat slot array + ibv_poll_cq
//     busy-poll. wr_id = slot index; release() re-posts the slot. Steering by
//     destination MAC via ibv_create_flow (unicast-filter semantics, no promisc
//     — without a flow rule a RAW_PACKET QP receives nothing).
//   * One flat registered MR: [TX slot][RX slot 0..RqDepth-1], 4K-aligned.
//
// SEND-CQ DISCIPLINE (deviation from perftest's blocking cq_mod drain, which
// would stall 1-in-N rounds and fatten the tail): WQEs are SIGNALED every
// SignalEvery-th send; completions are reaped LAZILY and NON-BLOCKINGLY one
// pacing period later — by then the CQE has been resident for ~SignalEvery
// round-trips, so the poll returns instantly and the hot path never waits.
// In-flight WQEs are bounded at 2*SignalEvery, sized well under SqDepth by
// static_assert; a backstop loop (never taken in steady state) refuses to let
// the SQ overflow.
//
// TUNABLES (all compile-time template params; only the interface name is
// runtime config): PortNum (each PCIe fn = a 1-port HCA), SqDepth, RqDepth,
// SignalEvery (power of two), MaxInline (provider may grant more — CX4Lx
// grants 140B for a 128B request; must cover the frame), MaxFrame (RX slot
// stride). Satisfies the TxRing/RxRing concepts (tryReceive/release +
// acquire/commit/send/prefillRing) like every other transport.
//
// PROVIDER ENV TUNING (set by init() via setenv(overwrite=0) BEFORE the device
// open — libmlx5 reads them at context allocation; a shell export still wins):
//   * MLX5_SINGLE_THREADED=1 — compiles the provider's spinlocks to no-ops
//     (rdma-core mlx5.h:1360). Default libmlx5 takes an uncontended spinlock on
//     EVERY post_send and EVERY poll_cq (~20-40 cycles each) — including every
//     empty poll of the spin loop. Safe: only the hot-loop thread touches the
//     verbs objects; the provider aborts if multi-threaded use is detected.
//   * MLX5_STALL_CQ_POLL=0 — disable the provider's adaptive CQ-poll stall
//     (deliberate wait cycles to reduce PCIe pressure; a throughput feature,
//     auto-detected per device — pure added latency for a ping-pong).
//   * MLX5_CQE_SIZE=128 was A/B-TESTED AND REJECTED (2026-06-10): 64 vs 128
//     identical on every percentile — scatter-to-CQE does not engage for
//     RAW_PACKET RQs on this device, so the larger CQE buys nothing. Default
//     64B CQEs kept (single cacheline per completion).
//
// DIRECT RX POLL (mlx5dv): tryReceive() does NOT call ibv_poll_cq. The recv
// CQ's raw CQE ring, doorbell record and the QP's RQ depth are exported once at
// init via mlx5dv_init_obj; the hot poll is then a single volatile read of the
// current CQE's op_own byte + the owner-bit check (get_sw_cqe logic from
// rdma-core cq.c: valid when opcode != MLX5_CQE_INVALID and owner ==
// !!(ci & cqe_cnt)) — ~5 instructions per empty poll vs ~60-100 cycles through
// the provider (spinlock + stall logic + ibv_wc translation), and the poll runs
// ~100x per round.
//
// The RQ is ALSO direct — REQUIRED, not optional: the provider's ibv_post_recv
// tracks fullness as head - tail, and tail only advances inside the provider's
// own poll_cq, which we bypass — so after RqDepth receives every repost fails
// silently with ENOMEM and the RQ runs dry (first flight froze at exactly
// ci == RqDepth). Design: recv WQEs are fixed 16B data segs {byte_count, lkey,
// addr}; ALL rq.wqe_cnt positions are pre-written ONCE at init with the
// identity mapping slot = position mod RqDepth (head - consumed == RqDepth is
// invariant, so position and slot stay in lockstep across wraps — wqe_counter
// & (RqDepth-1) IS the slot, no table). "Reposting" in release() is then just
// ++rqHead + one doorbell-record store (dbrec[0] = htobe32(head & 0xffff)),
// plus ++ci + the CQ dbrec store (htobe32(ci & 0xffffff)) so the NIC never
// sees the CQ as full. Acquire fence between the owner check and the CQE field
// reads (free on x86, required for correctness). Unexpected/error opcodes
// consume the CQE, bump both counters and return empty. The SEND CQ stays on
// legacy ibv_poll_cq (touched once per SignalEvery rounds — irrelevant).
// ibv_poll_cq / ibv_post_recv must never be called on the recv CQ / RQ once
// the direct path owns their indices.
//
// GOTCHAS:
//   * Never set MLX5_SHUT_UP_BF=1 — disabling BlueFlame reintroduces the WQE
//     DMA-read that inline+BF eliminates.
//   * init() resolves the RDMA device from the netdev name via
//     /sys/class/net/<if>/device/infiniband (slot-move-proof, no BDFs).
//   * RX wc.byte_len is the full L2 frame (FCS stripped by HW); frame layout in
//     the slot starts at the ethernet header (dst MAC at offset 0).
//   * On wc error the slot is re-posted and the frame surfaces as a drop.
//   * shutdown() order: flow -> QP -> CQs -> MR -> buffer -> PD -> device.
// =============================================================================

enum class VerbsMode : std::uint8_t { RxOnly, TxOnly, RxTx };

template<VerbsMode M, std::uint8_t PortNum = 1, std::uint16_t SqDepth = 256, std::uint16_t RqDepth = 8192, std::uint16_t SignalEvery = 64, std::uint16_t MaxInline = 128, std::uint16_t MaxFrame = 2048>
class Verbs {
  static constexpr bool HAS_RX = (M == VerbsMode::RxOnly || M == VerbsMode::RxTx);
  static constexpr bool HAS_TX = (M == VerbsMode::TxOnly || M == VerbsMode::RxTx);

  static_assert((SignalEvery & (SignalEvery - 1)) == 0, "SignalEvery must be a power of two");
  static_assert(SignalEvery * 4 <= SqDepth, "SQ must hold several signal periods");

public:
  explicit Verbs(std::string_view ifname) noexcept;
  ~Verbs();

  Verbs(const Verbs&)            = delete;
  Verbs& operator=(const Verbs&) = delete;
  Verbs(Verbs&&)                 = delete;
  Verbs& operator=(Verbs&&)      = delete;

  [[nodiscard]] bool init() noexcept;
  void shutdown() noexcept;

  [[nodiscard]] std::array<std::uint8_t, 6> macAddress() const noexcept;

  void prefillRing(std::span<const std::uint8_t> frameTemplate) noexcept
      requires (M == VerbsMode::TxOnly || M == VerbsMode::RxTx);

  [[nodiscard, gnu::always_inline, gnu::hot]]
  inline std::uint8_t* acquire(std::uint32_t frameLen) noexcept
      requires (M == VerbsMode::TxOnly || M == VerbsMode::RxTx);

  [[gnu::always_inline, gnu::hot]]
  inline void commit() noexcept
      requires (M == VerbsMode::TxOnly || M == VerbsMode::RxTx);

  [[nodiscard, gnu::always_inline, gnu::hot]]
  inline bool send(std::span<const std::uint8_t> frame) noexcept
      requires (M == VerbsMode::TxOnly || M == VerbsMode::RxTx);

  [[nodiscard, gnu::always_inline, gnu::hot]]
  inline RxFrame tryReceive() noexcept
      requires (M == VerbsMode::RxOnly || M == VerbsMode::RxTx);

  [[gnu::always_inline, gnu::hot]]
  inline void release() noexcept
      requires (M == VerbsMode::RxOnly || M == VerbsMode::RxTx);

private:
  [[gnu::always_inline]] std::uint8_t* rxSlot(std::uint16_t s) const noexcept;
  void prewriteRq() noexcept;
  void qpErr(const char* st) const noexcept;
  [[nodiscard]] static std::string ibdevFromNetdev(const std::string& ifname) noexcept;
  [[nodiscard]] bool readMac() noexcept;

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

  std::uint8_t*               m_cqBuf{nullptr};
  volatile std::uint32_t*     m_cqDbrec{nullptr};
  std::uint32_t               m_cqeCnt{0};
  std::uint32_t               m_cqeSz{0};
  std::uint32_t               m_cqCi{0};
  std::uint8_t*               m_rqBuf{nullptr};
  volatile std::uint32_t*     m_rqDbrec{nullptr};
  std::uint32_t               m_rqWqeCnt{0};
  std::uint32_t               m_rqStride{0};
  std::uint32_t               m_rqHead{0};
};

// =============================================================================

template<VerbsMode M, std::uint8_t PortNum, std::uint16_t SqDepth, std::uint16_t RqDepth, std::uint16_t SignalEvery, std::uint16_t MaxInline, std::uint16_t MaxFrame>
Verbs<M, PortNum, SqDepth, RqDepth, SignalEvery, MaxInline, MaxFrame>::Verbs(std::string_view ifname) noexcept
  : m_ifname{ifname} {}

template<VerbsMode M, std::uint8_t PortNum, std::uint16_t SqDepth, std::uint16_t RqDepth, std::uint16_t SignalEvery, std::uint16_t MaxInline, std::uint16_t MaxFrame>
Verbs<M, PortNum, SqDepth, RqDepth, SignalEvery, MaxInline, MaxFrame>::~Verbs() {
  shutdown();
}

template<VerbsMode M, std::uint8_t PortNum, std::uint16_t SqDepth, std::uint16_t RqDepth, std::uint16_t SignalEvery, std::uint16_t MaxInline, std::uint16_t MaxFrame>
bool Verbs<M, PortNum, SqDepth, RqDepth, SignalEvery, MaxInline, MaxFrame>::init() noexcept {
  ::setenv("MLX5_SINGLE_THREADED", "1", 0);
  ::setenv("MLX5_STALL_CQ_POLL",   "0", 0);

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
    if (ibdev == ibv_get_device_name(list[i])) {
      m_ctx = ibv_open_device(list[i]);
      break;
    }
  ibv_free_device_list(list);
  if (!m_ctx) {
    fmt::println(stderr, "[Verbs] {}: open '{}' failed", m_ifname, ibdev);
    return false;
  }

  m_pd = ibv_alloc_pd(m_ctx);
  if (!m_pd) {
    fmt::println(stderr, "[Verbs] {}: alloc_pd failed", m_ifname);
    return false;
  }

  m_sendCq = ibv_create_cq(m_ctx, SqDepth, nullptr, nullptr, 0);
  m_recvCq = ibv_create_cq(m_ctx, RqDepth, nullptr, nullptr, 0);
  if (!m_sendCq || !m_recvCq) {
    fmt::println(stderr, "[Verbs] {}: create_cq failed", m_ifname);
    return false;
  }

  const std::size_t bufSize = static_cast<std::size_t>(MaxFrame) * (1 + RqDepth);
  if (posix_memalign(reinterpret_cast<void**>(&m_buf), 4096, bufSize) != 0) return false;
  std::memset(m_buf, 0, bufSize);
  m_mr = ibv_reg_mr(m_pd, m_buf, bufSize, IBV_ACCESS_LOCAL_WRITE);
  if (!m_mr) {
    fmt::println(stderr, "[Verbs] {}: reg_mr failed (RLIMIT_MEMLOCK?)", m_ifname);
    return false;
  }

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
  if (!m_qp) {
    fmt::println(stderr, "[Verbs] {}: create_qp(RAW_PACKET) failed (root/CAP_NET_RAW?)", m_ifname);
    return false;
  }
  m_maxInline = qa.cap.max_inline_data;

  ibv_qp_attr at{};
  at.qp_state = IBV_QPS_INIT;
  at.port_num = PortNum;
  if (ibv_modify_qp(m_qp, &at, IBV_QP_STATE | IBV_QP_PORT) != 0) {
    qpErr("INIT");
    return false;
  }
  at = {}; at.qp_state = IBV_QPS_RTR;
  if (ibv_modify_qp(m_qp, &at, IBV_QP_STATE) != 0) {
    qpErr("RTR");
    return false;
  }
  if constexpr (HAS_TX) {
    at = {}; at.qp_state = IBV_QPS_RTS;
    if (ibv_modify_qp(m_qp, &at, IBV_QP_STATE) != 0) {
      qpErr("RTS");
      return false;
    }
  }

  if constexpr (HAS_RX) {
    mlx5dv_obj obj{};
    mlx5dv_cq  dvcq{};
    mlx5dv_qp  dvqp{};
    obj.cq.in  = m_recvCq; obj.cq.out = &dvcq;
    obj.qp.in  = m_qp;     obj.qp.out = &dvqp;
    if (mlx5dv_init_obj(&obj, MLX5DV_OBJ_CQ | MLX5DV_OBJ_QP) != 0) {
      fmt::println(stderr, "[Verbs] {}: mlx5dv_init_obj failed (mlx5 provider required)", m_ifname);
      return false;
    }
    m_cqBuf    = static_cast<std::uint8_t*>(dvcq.buf);
    m_cqDbrec  = reinterpret_cast<volatile std::uint32_t*>(dvcq.dbrec);
    m_cqeCnt   = dvcq.cqe_cnt;
    m_cqeSz    = dvcq.cqe_size;
    m_rqBuf    = static_cast<std::uint8_t*>(dvqp.rq.buf);
    m_rqDbrec  = reinterpret_cast<volatile std::uint32_t*>(dvqp.dbrec);
    m_rqWqeCnt = dvqp.rq.wqe_cnt;
    m_rqStride = dvqp.rq.stride;
    if (m_rqWqeCnt < RqDepth || (m_rqWqeCnt & (m_rqWqeCnt - 1)) != 0 ||
        (m_cqeCnt & (m_cqeCnt - 1)) != 0 || m_rqStride < 16) {
      fmt::println(stderr, "[Verbs] {}: unexpected ring geometry (rq {} stride {} cq {})",
                   m_ifname, m_rqWqeCnt, m_rqStride, m_cqeCnt);
      return false;
    }

    prewriteRq();
    m_rqHead = RqDepth;
    std::atomic_thread_fence(std::memory_order_release);
    m_rqDbrec[0] = htobe32(m_rqHead & 0xffff);

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
    if (!m_flow) {
      fmt::println(stderr, "[Verbs] {}: create_flow failed", m_ifname);
      return false;
    }
  }

  fmt::println(stderr, "[Verbs] {} ({}) port {} ready — inline {}B, sq {} rq {} signal 1/{}",
               m_ifname, ibdev, PortNum, m_maxInline, SqDepth, RqDepth, SignalEvery);
  return true;
}

template<VerbsMode M, std::uint8_t PortNum, std::uint16_t SqDepth, std::uint16_t RqDepth, std::uint16_t SignalEvery, std::uint16_t MaxInline, std::uint16_t MaxFrame>
void Verbs<M, PortNum, SqDepth, RqDepth, SignalEvery, MaxInline, MaxFrame>::shutdown() noexcept {
  if (m_flow)   {
    ibv_destroy_flow(m_flow);
    m_flow = nullptr;
  }
  if (m_qp)     {
    ibv_destroy_qp(m_qp);
    m_qp = nullptr;
  }
  if (m_sendCq) {
    ibv_destroy_cq(m_sendCq);
    m_sendCq = nullptr;
  }
  if (m_recvCq) {
    ibv_destroy_cq(m_recvCq);
    m_recvCq = nullptr;
  }
  if (m_mr)     {
    ibv_dereg_mr(m_mr);
    m_mr = nullptr;
  }
  if (m_buf)    {
    std::free(m_buf);
    m_buf = nullptr;
  }
  if (m_pd)     {
    ibv_dealloc_pd(m_pd);
    m_pd = nullptr;
  }
  if (m_ctx)    {
    ibv_close_device(m_ctx);
    m_ctx = nullptr;
  }
}

template<VerbsMode M, std::uint8_t PortNum, std::uint16_t SqDepth, std::uint16_t RqDepth, std::uint16_t SignalEvery, std::uint16_t MaxInline, std::uint16_t MaxFrame>
std::array<std::uint8_t, 6> Verbs<M, PortNum, SqDepth, RqDepth, SignalEvery, MaxInline, MaxFrame>::macAddress() const noexcept {
  return m_mac;
}

template<VerbsMode M, std::uint8_t PortNum, std::uint16_t SqDepth, std::uint16_t RqDepth, std::uint16_t SignalEvery, std::uint16_t MaxInline, std::uint16_t MaxFrame>
void Verbs<M, PortNum, SqDepth, RqDepth, SignalEvery, MaxInline, MaxFrame>::prefillRing(std::span<const std::uint8_t> frameTemplate) noexcept requires (M == VerbsMode::TxOnly || M == VerbsMode::RxTx) {
  std::memcpy(m_buf, frameTemplate.data(),
              std::min<std::size_t>(frameTemplate.size(), MaxFrame));
}

template<VerbsMode M, std::uint8_t PortNum, std::uint16_t SqDepth, std::uint16_t RqDepth, std::uint16_t SignalEvery, std::uint16_t MaxInline, std::uint16_t MaxFrame>
inline std::uint8_t* Verbs<M, PortNum, SqDepth, RqDepth, SignalEvery, MaxInline, MaxFrame>::acquire(std::uint32_t frameLen) noexcept requires (M == VerbsMode::TxOnly || M == VerbsMode::RxTx) {
  m_txLen = frameLen;
  return m_buf;
}

template<VerbsMode M, std::uint8_t PortNum, std::uint16_t SqDepth, std::uint16_t RqDepth, std::uint16_t SignalEvery, std::uint16_t MaxInline, std::uint16_t MaxFrame>
inline void Verbs<M, PortNum, SqDepth, RqDepth, SignalEvery, MaxInline, MaxFrame>::commit() noexcept requires (M == VerbsMode::TxOnly || M == VerbsMode::RxTx) {
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

  if (m_unreaped >= 2) [[unlikely]] {
    ibv_wc wc;
    if (ibv_poll_cq(m_sendCq, 1, &wc) > 0) --m_unreaped;
    while (m_unreaped >= SqDepth / SignalEvery - 1)
      if (ibv_poll_cq(m_sendCq, 1, &wc) > 0) --m_unreaped;
  }
}

template<VerbsMode M, std::uint8_t PortNum, std::uint16_t SqDepth, std::uint16_t RqDepth, std::uint16_t SignalEvery, std::uint16_t MaxInline, std::uint16_t MaxFrame>
inline bool Verbs<M, PortNum, SqDepth, RqDepth, SignalEvery, MaxInline, MaxFrame>::send(std::span<const std::uint8_t> frame) noexcept requires (M == VerbsMode::TxOnly || M == VerbsMode::RxTx) {
  auto* dst = acquire(static_cast<std::uint32_t>(frame.size()));
  std::memcpy(dst, frame.data(), frame.size());
  commit();
  return true;
}

template<VerbsMode M, std::uint8_t PortNum, std::uint16_t SqDepth, std::uint16_t RqDepth, std::uint16_t SignalEvery, std::uint16_t MaxInline, std::uint16_t MaxFrame>
inline RxFrame Verbs<M, PortNum, SqDepth, RqDepth, SignalEvery, MaxInline, MaxFrame>::tryReceive() noexcept requires (M == VerbsMode::RxOnly || M == VerbsMode::RxTx) {
  std::uint8_t* entry = m_cqBuf
      + static_cast<std::size_t>(m_cqCi & (m_cqeCnt - 1)) * m_cqeSz;
  auto* cqe = reinterpret_cast<mlx5_cqe64*>(m_cqeSz == 64 ? entry : entry + 64);
  const std::uint8_t opOwn  = *reinterpret_cast<volatile std::uint8_t*>(&cqe->op_own);
  const std::uint8_t opcode = opOwn >> 4;
  const std::uint8_t owner  = ((m_cqCi & m_cqeCnt) != 0) ? 1 : 0;
  if (opcode == MLX5_CQE_INVALID ||
      (opOwn & MLX5_CQE_OWNER_MASK) != owner) [[likely]] return {};
  std::atomic_thread_fence(std::memory_order_acquire);
  const std::uint16_t slot = be16toh(cqe->wqe_counter) & (RqDepth - 1);
  if (opcode != MLX5_CQE_RESP_SEND) [[unlikely]] {
    ++m_rqHead;
    m_rqDbrec[0] = htobe32(m_rqHead & 0xffff);
    ++m_cqCi;
    m_cqDbrec[0] = htobe32(m_cqCi & 0xffffff);
    return {};
  }
  m_lastSlot = slot;
  return { .data   = { rxSlot(m_lastSlot), be32toh(cqe->byte_cnt) },
           .sec    = 0,
           .nsec   = 0,
           .status = 1 };
}

template<VerbsMode M, std::uint8_t PortNum, std::uint16_t SqDepth, std::uint16_t RqDepth, std::uint16_t SignalEvery, std::uint16_t MaxInline, std::uint16_t MaxFrame>
inline void Verbs<M, PortNum, SqDepth, RqDepth, SignalEvery, MaxInline, MaxFrame>::release() noexcept requires (M == VerbsMode::RxOnly || M == VerbsMode::RxTx) {
  ++m_rqHead;
  m_rqDbrec[0] = htobe32(m_rqHead & 0xffff);
  ++m_cqCi;
  m_cqDbrec[0] = htobe32(m_cqCi & 0xffffff);
}

template<VerbsMode M, std::uint8_t PortNum, std::uint16_t SqDepth, std::uint16_t RqDepth, std::uint16_t SignalEvery, std::uint16_t MaxInline, std::uint16_t MaxFrame>
inline std::uint8_t* Verbs<M, PortNum, SqDepth, RqDepth, SignalEvery, MaxInline, MaxFrame>::rxSlot(std::uint16_t s) const noexcept {
  return m_buf + static_cast<std::size_t>(MaxFrame) * (1 + s);
}

template<VerbsMode M, std::uint8_t PortNum, std::uint16_t SqDepth, std::uint16_t RqDepth, std::uint16_t SignalEvery, std::uint16_t MaxInline, std::uint16_t MaxFrame>
void Verbs<M, PortNum, SqDepth, RqDepth, SignalEvery, MaxInline, MaxFrame>::prewriteRq() noexcept {
  struct DataSeg { std::uint32_t byteCount; std::uint32_t lkey; std::uint64_t addr; };
  for (std::uint32_t p = 0; p < m_rqWqeCnt; ++p) {
    const std::uint16_t s = static_cast<std::uint16_t>(p & (RqDepth - 1));
    auto* seg = reinterpret_cast<DataSeg*>(m_rqBuf + static_cast<std::size_t>(p) * m_rqStride);
    seg->byteCount = htobe32(MaxFrame);
    seg->lkey      = htobe32(m_mr->lkey);
    seg->addr      = htobe64(reinterpret_cast<std::uintptr_t>(rxSlot(s)));
  }
}

template<VerbsMode M, std::uint8_t PortNum, std::uint16_t SqDepth, std::uint16_t RqDepth, std::uint16_t SignalEvery, std::uint16_t MaxInline, std::uint16_t MaxFrame>
void Verbs<M, PortNum, SqDepth, RqDepth, SignalEvery, MaxInline, MaxFrame>::qpErr(const char* st) const noexcept {
  fmt::println(stderr, "[Verbs] {}: modify_qp -> {} failed", m_ifname, st);
}

template<VerbsMode M, std::uint8_t PortNum, std::uint16_t SqDepth, std::uint16_t RqDepth, std::uint16_t SignalEvery, std::uint16_t MaxInline, std::uint16_t MaxFrame>
std::string Verbs<M, PortNum, SqDepth, RqDepth, SignalEvery, MaxInline, MaxFrame>::ibdevFromNetdev(const std::string& ifname) noexcept {
  const std::string dir = "/sys/class/net/" + ifname + "/device/infiniband";
  std::string out;
  if (DIR* d = opendir(dir.c_str())) {
    while (dirent* e = readdir(d))
      if (e->d_name[0] != '.') {
        out = e->d_name;
        break;
      }
    closedir(d);
  }
  return out;
}

template<VerbsMode M, std::uint8_t PortNum, std::uint16_t SqDepth, std::uint16_t RqDepth, std::uint16_t SignalEvery, std::uint16_t MaxInline, std::uint16_t MaxFrame>
bool Verbs<M, PortNum, SqDepth, RqDepth, SignalEvery, MaxInline, MaxFrame>::readMac() noexcept {
  const std::string p = "/sys/class/net/" + m_ifname + "/address";
  FILE* f = std::fopen(p.c_str(), "r");
  if (!f) {
    fmt::println(stderr, "[Verbs] {}: cannot read MAC", m_ifname);
    return false;
  }
  unsigned b[6]{};
  const int n = std::fscanf(f, "%x:%x:%x:%x:%x:%x", &b[0],&b[1],&b[2],&b[3],&b[4],&b[5]);
  std::fclose(f);
  if (n != 6) return false;
  for (int i = 0; i < 6; ++i) m_mac[i] = static_cast<std::uint8_t>(b[i]);
  return true;
}

#endif // ABTRDA3_VERBS_HPP
