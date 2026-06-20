# AF_XDP Known Driver Issues

Driver-level problems found while building ultra-low-latency AF_XDP-ZC transports on
the `rtserver` test bench (kernel `7.0.7-hz100`). Each issue is stated as
**symptom → mechanism → evidence → fix/status**. The recurring theme: AF_XDP keeps
the in-tree kernel driver in the datapath, so its latency and reliability are
inherited from each driver's NAPI / zero-copy maturity (i40e < mlx5 sound; igc
buggy). Every anomaly below was localised against **xdpsock** (the kernel's own
reference AF_XDP app) — if xdpsock fails too, the fault is in the driver, not our code.

NICs covered: Intel XXV710 (i40e), Intel I225-V (igc). ConnectX-4 Lx (mlx5) — **TBD,
added later**.

---

## 1. Intel XXV710-DA2 — `i40e` (netdevs `xxv0` / `xxv1`)

### 1.1 Busy-poll is silently disabled on the stock driver — requires a patch

**Symptom.** `setsockopt(SO_BUSY_POLL / SO_PREFER_BUSY_POLL)` succeeds, but the XSK
socket never actually busy-polls; RX falls back to interrupt latency. No error is
reported.

**Mechanism.** The kernel busy-poll path (`sk_busy_loop`, `net/xdp/xsk.c`) needs a
**nonzero `napi_id`** on the bound RX queue to know which NAPI instance to spin on.
Stock i40e never calls `netif_queue_set_napi()` for its RX queues, so the XSK socket
gets `napi_id = 0` → `sk_can_busy_loop()` is false → busy-poll is inert. (igc and
mlx5 do this association in-tree, so they don't need a patch.)

**Patch** (`patches/i40e-afxdp-busypoll.patch`) — wire each RX queue to its
q_vector's NAPI in `i40e_napi_enable_all()` (cleared in disable), mirroring
`igc_set_queue_napi()`:

```c
/* in i40e_napi_enable_all() */
i40e_for_each_ring(ring, q_vector->rx)
        netif_queue_set_napi(vsi->netdev, ring->queue_index,
                             NETDEV_QUEUE_TYPE_RX, &q_vector->napi);
```

**Evidence.**
- Symbol: loaded `i40e.ko` references `netif_queue_set_napi` (`nm … | grep -c` → 1);
  the stock backup `i40e.ko.jun10bak` → 0.
- Runtime: `ynl queue-get` shows `xxv0`/`xxv1` RX queues 0–7 each with a nonzero
  `napi-id` (8210–8217 / 8218–8225); TX queues have none (patch is RX-only — correct).
- `getsockopt` is **not** proof: our `[AFXDP] busy-poll ACTIVE` log only reads the
  socket option back, not `napi_id`. Verify engagement with the `ynl` napi-id check.

**Status.** Patched driver built and **loaded/active** on `rtserver`
(`/sys/module/i40e/srcversion` == on-disk `i40e.ko`). All i40e busy-poll/RTT numbers
were taken on the patched driver — they are real busy-poll, not interrupt-driven.

### 1.2 NAPI deferral strands the hardirq under a gapless FIFO busy-poll

**Symptom.** With NAPI deferral on (`napi_defer_hard_irqs=2`,
`gro_flush_timeout=2000ns`) a SCHED_FIFO busy-poll loop pinned to an isolated core
**drops sparse / idle / request-reply RX entirely** — the frame reaches the NIC
(`port.rx_unicast` increments) but `rx_packets` stays 0 and the app never sees it.

**Mechanism.** Under deferral the i40e hardirq stays masked during busy-poll and
re-arms only when the NAPI *completes* (`napi_complete_done`, after the defer counter
decays to 0). Completion requires a **gap in the polling** — a ≥ ~4 µs (2× gro)
pause where the app isn't calling `recvfrom`, so the gro-timer softirq can decay the
counter. Every `recvfrom` runs `busy_poll_stop`, which **resets** the counter. A
never-yielding FIFO loop on a dedicated isolated core resets it forever → NAPI never
completes → hardirq never re-arms → the packet is stranded. It is a **timing/gap**
property, not a poll-rate property.

**How we broke xdpsock** (proof it's the driver/scheduling, not our code). Under the
*identical* stack (isolated cmdline, deferral 2/2000, `-B`, FIFO:49, `taskset -c 2`),
xdpsock *caught* the lone packets while our rxsink stranded — at first glance "our
code is worse." It is not:
- `recvfrom` rate was equal (ours ~1.481 M/s vs xdpsock ~1.455 M/s) → not over-polling.
- `/proc/<pid>/status`: both had **0 voluntary** context switches; xdpsock had ~5
  *involuntary* preemptions / 10 s, ours had **0**.
- `perf record -e sched:sched_switch -C 2`: the preemptions were **xdpsock→xdpsock** —
  its main thread being preempted by its own **SCHED_OTHER stats-poller thread** (run
  via RT-throttling, `sched_rt_runtime_us=950000/1000000`, ~1/s). That incidental gap
  is the *only* reason xdpsock re-armed the IRQ.
- Negative control: `xdpsock -Q` (gates out the poller) → xdpsock **strands** exactly
  like us. Positive control: a co-located FIFO:50 dummy ("GAP_MAKER") or a hog on the
  core → our app **catches**. (`bpftrace` was unusable here — malformed `btf_vmlinux`;
  used `/proc/status` + `perf` instead.)

So xdpsock was never a better receive loop — just a noisier roommate on the core.

**Evidence (numbers).**
- Clean A/B: `ABTRDA3_RT_BYPASS=1` (SCHED_OTHER) → caught 5/5, IRQ 189 climbed;
  default FIFO+pin → 0 caught, IRQ 189 frozen. Same binary.
- GAP_MAKER + deferral: first working deferred RTT, but median **1011 µs ≈ the
  1.01 ms gap period** — RTT is gated by the re-arm cadence (replies land in ~10 µs,
  the min, then wait for the next gap). Shrinking the gap to ~10 µs burns ~50% of the
  core. The deferred path is structurally unfit for low latency.

**Fix.** Run **without deferral**: `napi_defer_hard_irqs=0`, `gro_flush_timeout=0`
(`AFXDP.hpp` `NAPI_DEFER_HARD_IRQS=0` / `NAPI_GRO_FLUSH_NS=0`). `busy_poll_stop` then
calls `napi_complete_done` after every empty poll → the inline poll catches the
descriptor each iteration and the hardirq stays armed as a backstop, **no gap
required**. Result (i40e, `xxv0↔xxv1`, 10 k RTT): median **10.38 µs** (5.19 µs
one-way), P99 10.97 µs. Cost: continuous ~34 k IRQ/s, but only on housekeeping cores
(0–1,6–7), never the hot core → no jitter, CPU/power only. Under pure FIFO busy-poll
there is no middle ground — `defer>0` stalls, `defer=0` storms; `0` is the only
workable value. (Open: ~0.1% tail to ~200 µs at P99.9 — separate tail-cliff work.)

### 1.3 `needs_wakeup` flag-timing quirk — since fixed

There was historically a `needs_wakeup`-related fragility (driver-dependent timing of
the wakeup flag affecting the ZC wakeup path); it has since been addressed and is no
longer a blocker on this kernel. Noted for completeness only.

---

## 2. Intel I225-V — `igc` (netdevs `enp6s0` / `enp7s0`)

igc's zero-copy support is newer and noticeably less robust than i40e's. On igc the
**TX** side is the fragile one, and the driver breaks *readily* under xdpsock — the
opposite asymmetry from i40e, and the clearest sign the faults are driver-level.

### 2.1 Zero-copy TX engine wedges under XDP attach/detach churn

**Symptom.** After repeated XDP attach/detach cycles and/or runs killed mid-TX, the
igc ZC TX engine hangs: `tx` count freezes at **exactly the TX ring depth (2048)**,
**zero completions** ever return, and the app spins **millions of futile `sendto`
kicks/sec** (`tx wakeup sendtos` in the millions). The ring fills, `outstanding_tx`
never drops, no slot frees, and it spins forever.

**Evidence it's the driver, not us.** **xdpsock hangs identically** — same freeze at
2048, same kick storm, RX side sees nothing. The kernel's reference app cannot get a
frame onto the wire, so the fault is below userspace.

**Fix.** Full driver reset (re-inits rings + DMA on both ports; `ip link down/up`
alone does **not** clear it):

```
sudo pkill -9 xdpsock; sudo pkill -9 abtrda3_test
sudo ip link set dev enp6s0 xdp off; sudo ip link set dev enp7s0 xdp off
sudo modprobe -r igc && sudo modprobe igc
```

**Rule.** Any data collected while the NIC is wedging is **void**. Re-baseline after a
reset with `xdpsock` TX 100 k → `ethtool -S enp7s0 | grep rx_packets:` before trusting
any TX result.

### 2.2 Zero-copy datapath fragility (the i40e contrast)

**Observation.** On i40e, reproducing an AF_XDP anomaly in xdpsock took deliberate
effort (strip the incidental scheduling gap, §1.2) and even then it was a *scheduling*
interaction, not a datapath defect — the driver was sound. On igc, xdpsock breaks with
**no special effort** (TX wedge, delivery loss). This qualitative asymmetry localises
the igc faults in the i225 ZC datapath itself.

**Implication.** igc/i225 AF_XDP-ZC is **not** a publication-grade low-latency
transport without driver-side workarounds. The driver-quirk-immune path for igc is a
**native DPDK PMD over vfio-pci** (`net_igc`), which takes the kernel driver out of the
loop entirely. (Note: a `net_af_xdp` DPDK vdev does **not** count — it rides the same
`igc.ko` ZC path and shares these quirks.)

### 2.3 Link/PHY warmup loss on cold bursts

**Symptom.** At the *front* of a transmit burst, the first frames are lost.

**Evidence.** Healthy NIC, `xdpsock` TX 100 k `enp6s0` → `enp7s0` hardware
`rx_packets` delta = **99 767** (≈233 lost); a separate clean run lost 218. The loss
is at the front (xdpsock drains its tail), ~220–230 frames at ~3 M pps ≈ **~75 µs** —
consistent with i225 PHY/LPI wake latency after idle.

**Consequence.** A tiny cold burst (e.g. `--count 2`) sent right after bind falls
*entirely* inside this window and appears to "vanish." This is a measurement-priming
issue, **not** a TX defect — **warm the link** (continuous traffic) before small-burst
or one-way latency tests, or exclude the warmup frames.

### 2.4 Asynchronous-TX teardown sensitivity (drain before close)

**Mechanism.** AF_XDP TX is asynchronous: `commit()` only *enqueues* a descriptor and
kicks; the frame leaves the NIC when the kernel later runs the driver's NAPI ZC-xmit.
A TX-only generator that finishes its loop in microseconds and tears the socket down
(blind `sleep`) can close **before** NAPI pushes the last descriptors — they die in
the ring. xdpsock avoids this with `complete_tx_only_all()` (loop kick+reap until
`outstanding_tx == 0`); DPDK does an equivalent drain on port-stop.

**Driver interaction.** i40e completes promptly enough that a one-shot path tolerated
the missing drain; igc's later completion makes it fatal for small bursts. The correct
idiom on *any* driver is to **drive the doorbell yourself and drain until
`outstanding_tx == 0` before teardown** — teardown-only, zero hot-path cost, and
irrelevant to RTT (where the RX busy-poll drives the same NAPI).

---

## ConnectX-4 Lx — `mlx5` (netdevs `cx0` / `cx1`)

TBD — to be documented after the CX4 campaign.
