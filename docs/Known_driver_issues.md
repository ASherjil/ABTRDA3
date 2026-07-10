# Known Driver Issues (AF_XDP & DPDK PMDs)

Driver-level problems found while building ultra-low-latency transports on the
`rtserver` test bench (kernel `7.0.7-hz100`). Each issue is stated as
**symptom → mechanism → evidence → fix/status**. The recurring theme for AF_XDP: it
keeps the in-tree kernel driver in the datapath, so its latency and reliability are
inherited from each driver's NAPI / zero-copy maturity (i40e, mlx5 sound; igc
broken). Every AF_XDP anomaly below was localised against **xdpsock** (the kernel's
own reference AF_XDP app) — if xdpsock fails too, the fault is in the driver, not our
code.

Coverage: §1 i40e AF_XDP, §2 igc AF_XDP (the disqualification), §3 igc **DPDK PMD**
quirks.

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

**Reconfirmed via IRQ isolation — the gap can be a stray IRQ, not just a thread.** A stale
`pin-irqs.sh` mask (`HOUSE="0-1,6-7"`, from a prior 2–5 isolation) was mis-routing WiFi IRQs
onto the now-isolated cores 6,7 after isolation moved to 5–7 — that traffic silently supplied
the re-arm gap, so the socket *caught* (making the strand look "fixed"). Moving all WiFi/mlx5
IRQs off the isolated cores made the strand **reproduce at both `gro=2000` and `gro=200000`** —
proving the gro value is irrelevant; the **gap, not the timer, is the variable**. Residual IPIs
(`LOC`/`RES`/`CAL`, ~35/s) still hit the core yet are **not** enough — specifically **NET_RX
device-IRQ** activity is what nudges the NAPI. **Trap:** before trusting any FIFO busy-poll
result, confirm the isolated cores carry no device IRQs
(`grep -E 'iwlwifi|mlx5' /proc/interrupts` → the isolated-core columns must be 0).

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

## 2. Intel I225-V — `igc` (netdevs `enp5s0` / `enp6s0`)

**Final verdict (2026-07-10, after a multi-day investigation):** the igc driver has
**no self-service pump for the XSK zero-copy TX ring**. Everything below — the
"wedges", the "modprobe fixes", the works-then-doesn't behaviour — turned out to be
one root cause wearing different disguises. AF_XDP on igc is disqualified for
one-frame-in-flight latency measurement; the NIC is DPDK-only in this campaign.

### 2.1 Root cause: ZC TX is only serviced from a NAPI path that nothing drives

In the igc source, `igc_xdp_xmit_zc()` — the only function that moves XSK TX
descriptors to the hardware ring — is invoked *exclusively* from `igc_clean_tx_irq()`,
i.e. from the **TX queue-vector's interrupt/NAPI handler**. The `need_wakeup` flag is
likewise only ever set inside that same handler. Two facts make this fatal:

- With `combined=1` (mandatory for xdpsock-style single-queue tests) the i225 runs
  **unpaired** `rx-0` / `tx-0` MSI-X vectors — `igc_set_flag_queue_pairs()` only pairs
  RX and TX onto one vector when more than half the max queues (4) are in use.
  i40e, by contrast, always uses paired `TxRx` vectors: its RX poll services TX
  completions in the same NAPI pass, which is why i40e honours the AF_XDP contract.
- Both busy-polling and `sendto()` kicks drive the **RX** NAPI only. On igc a kick
  returns having done nothing; `need_wakeup` on/off is behaviourally identical
  (verified: unconditional-kick builds change nothing).

So a userspace ZC application **cannot cause its own TX descriptors to be
transmitted**. Frames leave only when *unrelated kernel traffic* (IPv6 housekeeping,
ARP, anything addressed to the port) runs the TX vector's NAPI — a **donor pump**.
Streaming tests mask this (interrupts are always in flight); a ping-pong exposes it:
after the first hop there is no donor, and the exchange deadlocks. The kernel's own
`xdpsock -l` (l2fwd) cannot ping-pong on igc — in zero-copy mode it deliberately
issues no kicks (the source comments "Tx is driven by the NAPI loop"; the kick is
gated on `XDP_COPY`).

**The controlled proof (3-NIC manual A/B, chatter deliberately left ON):** l2fwd
reflectors on both ports of each NIC, one seeded frame injected. mlx5: bounces
indefinitely. i40e: **1,035,345 hops/side** (≈12.3 µs/RTT). igc: **freezes at
rx=1 / tx=1** — while a unidirectional xdpsock Tx→Rx on the very same ports ran fine
*simultaneously* (RX path healthy; TX pump absent). Same binaries, same procedure,
one variable: the driver.

### 2.2 The false trails this produced (kept as a methodology warning)

Before the root cause was isolated, the symptom set supported several wrong theories,
each briefly convincing:

- **"ZC TX engine wedges under XDP attach/detach churn."** The freeze at exactly ring
  depth (2048), zero completions, and millions of futile `sendto` kicks/sec looked
  like a wedged DMA engine. It is simply the unserviced ring filling up.
- **"`modprobe -r igc && modprobe igc` fixes it."** The reload never repaired
  anything — it **restored the donor**: a fresh driver bind brings the ports up,
  IPv6/DAD/MLD chatter fires, and that traffic services the TX queue for a while.
- **"Control-plane ops (addr flush, flag changes) wedge TX."** They *silence the
  donor* (a port with no addresses generates no kernel traffic), which kills ZC TX at
  the next quiet moment. i40e survives identical ops because it never needed a donor.
- **"It works when I test it, fails in the app."** Tests run right after
  setup — while link-up chatter is still flowing. The app runs on a quiesced port.

**Rule that survives:** on igc, any ZC TX result is only valid if you can account for
*what serviced the TX queue*. Cross-check `ethtool -S` on **both** ports against the
app's own counters; igc's `imissed`/MPC is never populated and hides drops.

**Implication.** igc/i225 AF_XDP-ZC is not a measurable low-latency transport, with
or without workarounds — the defect is architectural in the driver's NAPI wiring, not
a tuning issue. The driver-quirk-immune path for igc is a **native DPDK PMD over
vfio-pci** (`net_igc`), which takes `igc.ko` out of the loop entirely. (A `net_af_xdp`
DPDK vdev does **not** count — it rides the same `igc.ko` ZC path.) An upstream
report to intel-wired-lan is planned.

### 2.3 Link/PHY warmup loss on cold bursts

**Symptom.** At the *front* of a transmit burst, the first frames are lost.

**Evidence.** Healthy NIC, `xdpsock` TX 100 k `enp5s0` → `enp6s0` hardware
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
the missing drain; on igc completion waits for the donor pump (§2.1), so an undrained
teardown is near-guaranteed to strand frames. The correct idiom on *any* driver is to
**drive the doorbell yourself and drain until `outstanding_tx == 0` before teardown**
— teardown-only, zero hot-path cost. (On i40e the RX busy-poll drives the same paired
NAPI, so RTT is unaffected; on igc nothing drives it — see §2.1.)

---

## 3. Intel I225-V — DPDK `igc` PMD quirks

Not defects on the scale of §2 — the igc PMD is sound and is the transport we publish
for this NIC — but three behaviours cost real debugging time and one of them silently
voids results.

### 3.1 `dev_configure(nb_txq=0)` silently breaks RX

**Symptom.** An RX-only port (our SingleRecorder receiver) configures cleanly and
starts, the NIC receives every frame (`ipackets` climbs), but `rte_eth_rx_burst()`
returns 0 forever — and `imissed` stays 0.
**Mechanism.** Unlike mlx5/i40e, the igc PMD does not tolerate an asymmetric
queue count: configuring 0 TX queues succeeds but leaves RX descriptor write-back
dead.
**Fix.** Always configure ≥1 queue in *both* directions on igc, even for
unidirectional roles (dispatch calls `setSymmetricQueues(true)` for igc — driver policy lives in `TransportDispatch.hpp`).

### 3.2 RX `WTHRESH=0` stops descriptor write-back entirely — and `imissed` lies

**Symptom.** Setting `rxconf.rx_thresh.wthresh = 0` (attempting immediate
per-descriptor write-back, a valid idiom on other Intel parts): TX port
`opackets=300000`, RX port `ipackets=300000`, app `recorded=0`, `imissed=0`.
**Mechanism.** On i225 the write-back threshold must be non-zero; with 0 the
write-back engine has no trigger and never posts a DD bit. The PMD's `imissed` is
never populated (the MPC counter isn't wired up), so the loss is invisible to the
standard stats.
**Rule.** Keep the PMD default (4) — a full audit showed it costs nothing at one
frame in flight because EITR sits at its post-reset default of 0 (no moderation
timer delaying partial write-back flushes). And on igc, never trust `imissed`:
cross-check `opackets` (TX port) vs `ipackets` (RX port) vs the app's own count.

### 3.3 Nothing else to tune — the audit trail

A register-level audit of the PMD (DPDK 25.11) and the kernel driver, done before
committing to the 24 h soak, so nobody repeats it: EITR is never written by the PMD
and the init-time `CTRL.RST` returns it to 0 (moderation off — optimal); EEE/LPI is
actively disabled by the PMD's own base init (and i225 has no 802.3az support anyway
— that's i226-only); DMA coalescing is never enabled; the TX doorbell is written
immediately per burst with RS on every descriptor; flow control defaults to
`fc_full` (pause advertised — harmless at one frame in flight); PCIe ASPM was
verified disabled on both ports. The igc PMD has **no devargs at all**. The only
latency lever that exists is the link speed (Benchmarks.md §5.2): the 2.5GBASE-T PHY
carries ~2.4 µs/traversal more fixed pipeline latency than 1000BASE-T, so the
campaign runs this NIC at 1 GbE via autoneg-advertisement restriction
(`kIgcAdvertise1GOnly` in `src/app/TransportDispatch.hpp` -> `setLinkSpeeds`; the PMD rejects forced speed).
