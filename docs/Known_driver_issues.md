# Known Driver Issues

Driver-level problems found while building ultra-low-latency transports on the `rtserver`
bench (kernel `7.0.7-hz100`). Each is stated as **symptom → mechanism → evidence → fix**,
and every claim below is backed by a measurement or a source line. Where a fix is a
register write, the register is read back and printed at startup.

The recurring theme for AF_XDP: it keeps the in-tree kernel driver in the datapath, so its
latency and reliability are inherited from that driver's NAPI and zero-copy maturity. Every
AF_XDP anomaly here was reproduced with **xdpsock**, the kernel's own reference AF_XDP app —
if xdpsock fails too, the fault is in the driver, not in our code.

## Contents

- [1. Intel XXV710-DA2 — `i40e`](#1-intel-xxv710-da2--i40e)
  - [1.1 AF_XDP: busy-poll is a silent no-op on the stock driver](#11-af_xdp-busy-poll-is-a-silent-no-op-on-the-stock-driver)
  - [1.2 AF_XDP: a gapless busy-poll strands RX](#12-af_xdp-a-gapless-busy-poll-strands-rx)
  - [1.3 DPDK: RX write-back is ITR-gated — fixed with NoITR](#13-dpdk-rx-write-back-is-itr-gated--fixed-with-noitr)
- [2. Intel I225-V — `igc`](#2-intel-i225-v--igc)
  - [2.1 AF_XDP: zero-copy TX has no pump — disqualified](#21-af_xdp-zero-copy-tx-has-no-pump--disqualified)
  - [2.2 DPDK: `nb_txq = 0` silently breaks RX](#22-dpdk-nb_txq--0-silently-breaks-rx)
  - [2.3 DPDK: RX `WTHRESH = 0` stops write-back, and `imissed` lies](#23-dpdk-rx-wthresh--0-stops-write-back-and-imissed-lies)
  - [2.4 DPDK: the 2.5G PHY is slower than 1G — run the link at 1 GbE](#24-dpdk-the-25g-phy-is-slower-than-1g--run-the-link-at-1-gbe)
- [3. DPDK EAL — affects every PMD](#3-dpdk-eal--affects-every-pmd)
- [4. Solarflare X2522 — `sfc` / ef_vi](#4-solarflare-x2522--sfc--ef_vi)
  - [4.1 CTPIO: the default writer is a fallback lottery — use in_order](#41-ctpio-the-default-writer-is-a-fallback-lottery--use-in_order)
  - [4.2 The driver's PTP subsystem causes rare µs-scale outliers — compile it out](#42-the-drivers-ptp-subsystem-causes-rare-µs-scale-outliers--compile-it-out)

---

## 1. Intel XXV710-DA2 — `i40e`

### 1.1 AF_XDP: busy-poll is a silent no-op on the stock driver

**Symptom.** `setsockopt(SO_BUSY_POLL / SO_PREFER_BUSY_POLL)` succeeds and reads back as
enabled, but the socket never busy-polls — RX runs at interrupt latency. Nothing errors.

**Mechanism.** The kernel busy-poll path (`sk_busy_loop`, `net/xdp/xsk.c`) needs a **nonzero
`napi_id`** on the bound RX queue to know which NAPI to spin on. Stock i40e never calls
`netif_queue_set_napi()` for its RX queues, so the XSK socket gets `napi_id = 0`,
`sk_can_busy_loop()` is false, and busy-poll is inert. igc and mlx5 do this association
in-tree and need no patch.

**Verify it — `getsockopt` is NOT proof.** It only echoes the socket option back; it never looks
at `napi_id`. (Our own `busy-poll ACTIVE` log line has the same weakness.) Check the queue:

```bash
# stock i40e.ko: 0 hits. Patched: 1.
nm -D /lib/modules/$(uname -r)/kernel/drivers/net/ethernet/intel/i40e/i40e.ko \
  | grep -c netif_queue_set_napi

# every RX queue must show a NONZERO napi-id (TX queues have none — the patch is RX-only)
sudo ynl --family netdev --do queue-get --json '{"ifindex":'$(cat /sys/class/net/xxv0/ifindex)'}'
```

On the patched driver every `xxv0`/`xxv1` RX queue reports a nonzero `napi-id` (8210–8217 /
8218–8225). On stock, all zero.

**Fix.** `patches/i40e-afxdp-busypoll.patch` — wire each RX ring to its q_vector's NAPI in
`i40e_napi_enable_all()` (and clear it in disable), mirroring `igc_set_queue_napi()`:

```c
/* in i40e_napi_enable_all() */
i40e_for_each_ring(ring, q_vector->rx)
        netif_queue_set_napi(vsi->netdev, ring->queue_index,
                             NETDEV_QUEUE_TYPE_RX, &q_vector->napi);
```

All published i40e AF_XDP numbers were taken on this patched driver, so they are real
busy-poll and not interrupt-driven.

### 1.2 AF_XDP: a gapless busy-poll strands RX

**Symptom.** With NAPI deferral on (`napi_defer_hard_irqs=2`, `gro_flush_timeout` nonzero), a
**SCHED_FIFO** busy-poll loop on a cleanly isolated core never receives sparse / request-reply
traffic. The frame reaches the NIC — `port.rx_unicast` increments — but `rx_packets` stays 0
and the app never sees it.

**Mechanism.** Under deferral the hardirq stays masked during busy-poll and re-arms only when
the NAPI *completes* (`napi_complete_done`, once the defer counter decays). Completion needs a
**gap in the polling**: an interval where the app is not calling `recvfrom`, so the gro timer
can fire. Every `recvfrom` runs `busy_poll_stop`, which **resets** the counter. A never-yielding
FIFO loop on a dedicated core resets it forever → NAPI never completes → the hardirq never
re-arms → the packet is stranded. It is a property of the **gap**, not of the poll rate.

**Reproduce it with xdpsock.** The whole trick is `-Q`: it suppresses xdpsock's own per-second
stats thread. **That thread is the bug's camouflage** — it preempts xdpsock's poll loop
~5×/10 s, and that incidental gap is the only reason stock xdpsock re-arms the IRQ and appears
healthy.

```bash
# 1. deferral ON  (the regime under test)
echo 2      | sudo tee /sys/class/net/xxv1/napi_defer_hard_irqs
echo 200000 | sudo tee /sys/class/net/xxv1/gro_flush_timeout

# 2. RX: zero-copy, busy-poll, gapless FIFO on an isolated core.
#    -Q = no stats thread  => a TRULY gapless loop. Drop -Q and the bug hides.
sudo taskset -c 6 ./xdpsock -i xxv1 -q 0 -r -z -B -b 1 -W FIFO -U 49 -Q &

# 3. TX: inject exactly ONE frame from the other port  (-C 1)
sudo taskset -c 5 ./xdpsock -i xxv0 -q 0 -t -z -B -b 1 -C 1 \
     -G 40:a6:b7:02:b3:11 -H 40:a6:b7:02:b3:10

# 4. did the hardirq ever re-arm?
grep i40e-xxv1 /proc/interrupts     # frozen => stranded
```

| xdpsock RX | Result |
|---|---|
| `-W FIFO -U 49` **without** `-Q` | catches — its stats thread supplies the gap |
| `-W FIFO -U 49` **with** `-Q` | **strands.** IRQ count frozen, `rx_packets` = 0 |
| SCHED_OTHER (drop `-W/-U`) | catches — the scheduler supplies the gap |

Confirmed with `perf record -e sched:sched_switch` (the preemptions are **xdpsock→xdpsock**,
its own stats thread) and `/proc/<pid>/status` (both loops show **0 voluntary** context
switches; only xdpsock shows involuntary ones). `recvfrom` rates were equal (~1.46–1.48 M/s),
so this is not over-polling.

**A stray IRQ supplies the gap too.** A stale IRQ-pinning mask left WiFi interrupts on a
supposedly isolated core, which masked the strand entirely. With all device IRQs moved off, the
strand **reproduces at both `gro_flush_timeout=2000` and `200000`** — the gro value is
irrelevant; the gap is the variable. Residual IPIs (`LOC`/`RES`/`CAL`, ~35/s) are *not* enough;
NET_RX device activity is. **Check before trusting any FIFO busy-poll result:**

```bash
grep -E 'iwlwifi|mlx5' /proc/interrupts   # isolated-core columns MUST be 0
```

**Fix — two escapes, both verified:**

```bash
# A. SCHED_OTHER (what we publish). Scheduler preemption supplies the gap.
#    Measured statistically tied with FIFO:49 on this bench, so nothing is lost.

# B. Kill deferral. busy_poll_stop() then calls napi_complete_done() on every empty
#    poll, so no gap is needed. Costs ~34k IRQ/s — on housekeeping cores only.
echo 0 | sudo tee /sys/class/net/xxv1/napi_defer_hard_irqs
echo 0 | sudo tee /sys/class/net/xxv1/gro_flush_timeout
```

Under a gapless FIFO loop there is no middle ground: `defer > 0` strands, `defer = 0` storms.

### 1.3 DPDK: RX write-back is ITR-gated — fixed with NoITR

**Symptom.** Stock DPDK on the XXV710 gives a **~30 µs** median RTT at one frame in flight —
roughly 3× what the silicon can do — with no dropped packets and no obvious cause.

**Mechanism.** The 700-series does **not** write an RX descriptor back per packet. Datasheet
332464 rev4.1 §8.3.3.1.4.2: write-back happens only when (a) a full 128 B descriptor line
completes (4 × 32 B — which never happens with one frame in flight), (b) the queue context is
evicted, or (c) **the interrupt logic initiates it**. With one frame in flight only (c) is
available, so the descriptor is delivered on the interrupt-moderation timer — the ITR. DPDK
binds the data path with `I40E_ITR_INDEX_DEFAULT` (= 0, a real ITR) at `i40e_ethdev.c:2473`,
and `i40e_calc_itr_interval()` programs that ITR to **32 µs** (`i40e_ethdev.h:1521`).

The datasheet names the escape hatch itself, in the same section:

> *"The receive descriptors could be reported instantly for each packet by setting the ITR_INDX
> to NoITR."*

DPDK never does this on the data path. It defines `I40E_ITR_INDEX_NONE` (= 3) and uses it
**only for the FDIR queue** (`i40e_fdir.c:233`) — exactly where it wants prompt write-back.

**Fix — set `QINT_RQCTL.ITR_INDX = 3` (NoITR) by a direct BAR0 MMIO write.** No DPDK patch:
the install stays stock. `pci::i40eSetNoItr()` (`PciHelpers.hpp`) maps
`/sys/bus/pci/devices/<bdf>/resource0`, scans `QINT_RQCTL(q) = 0x0003A000 + 4q`, and
read-modify-writes **bits 12:11 only** on each bound queue, preserving `MSIX_INDX`,
`NEXTQ_INDX` and `NEXTQ_TYPE` so the interrupt linked-list stays intact. Applied after
`dev_start` and read back:

```
[PCI] xxv0 (0000:02:00.0): i40e NoITR on 1 RX queue(s) (q1: 0x47ff0001 -> 0x47ff1801)
```

`XOR = 0x1800` — bits 11 and 12 exactly, nothing else touched. Enabled by `kI40eNoItr` in
`src/app/TransportTraits.hpp`; applied identically to the DPDK vfio PMD, DPDK-over-AF_XDP and
native AF_XDP, since it is a property of the silicon rather than of the stack.

**Evidence — a 2 × 2 over the two knobs** (XXV710, one frame in flight, RTT median):

| ITR interval | `ITR_INDX` | Median RTT |
|---|---|---:|
| 32 µs (PMD default) | 0 — stock DPDK | ~30 µs |
| 0 (interval cleared) | 0 | 9.028 µs |
| 32 µs (PMD default) | **3 — NoITR** | **9.029 µs** |

The third row is the point: **NoITR alone reaches the floor with the 32 µs interval left fully
in place**, so it genuinely detaches the queue from the ITR. Clearing the interval to zero
reaches the same floor by a different route — the two are equivalent, and neither goes below
~9 µs. That residual is the silicon (two serialised PCIe reads per TX plus the MAC/PHY), not
write-back gating.

**Positive control — proof the hardware honours a post-`dev_start` write.** A register readback
only proves the *register* took the value; it cannot prove the queue's internal context is
using it. So we point `ITR_INDX` at an otherwise-unused ITR index loaded with a deliberately
large interval, where a working write must be *visible*:

| ITR index 1 loaded with | `ITR_INDX` | Median RTT |
|---|---|---:|
| 20 µs | 1 | **20.653 µs** (P99 32.4, max 32.8) |

The RTT more than doubles, so post-start `QINT_RQCTL` writes are honoured. (This also disproves
a competing theory — that under DPDK's polling mode, with `INTENA` cleared and `WB_ON_ITR`
never set, the ITR-expiry write-back path is structurally dead. A 20 µs interval demonstrably
slows write-back, so that path is alive.) Re-runnable via `kI40eItrProbe`.

**Cleanest demonstration — DPDK-over-AF_XDP.** On that path an `ethtool -C rx-usecs 50`
*survives* into the run, so the throttle is verifiably live in the register at measurement
time — and the latency is unaffected:

```
[DPDK-ITR] xxv0: PFINT_ITRN(data-RX)=0x00000019 (50us) => RX path THROTTLED (ITRN nonzero!)
=== Round-Trip Latency Results ===  Median: 10.750 us
```

A 50 µs RX throttle that the hardware was obeying would put this at 30–60 µs. It sits on the
10.753 µs baseline measured with the ITR cleared. NoITR is doing all of the work, with the
counterfactual printed in the same log.

**Three traps if you re-implement this.**

- **Unimplemented i40e registers read back `0xDEADBEEF`.** That value has bit 30 (`CAUSE_ENA`)
  set *and* bits 12:11 (`ITR_INDX`) already equal to 3 — so a naive scan "finds" hundreds of
  bound queues and reports every write as successful. Validate with reserved bit 31 == 0.
- **The bound queue is not at index 0.** On this card it is at absolute index **1** (queue 0
  belongs to the FDIR VSI). A hardcoded index would write the wrong register and still succeed.
  Locate queues by their `CAUSE_ENA` bit instead of assuming.
- **`NEXTQ_TYPE` differs by driver.** DPDK writes `0` (`I40E_QUEUE_TYPE_RX`); the kernel driver
  chains RX→TX and writes `1`. A validator keyed on `NEXTQ_TYPE == 0` silently matches nothing
  on the AF_XDP paths.

---

## 2. Intel I225-V — `igc`

### 2.1 AF_XDP: zero-copy TX has no pump — disqualified

**Symptom.** A one-frame-in-flight ping-pong over AF_XDP zero-copy deadlocks after the first
hop. Not slow — **unmeasurable**.

**Mechanism.** `igc_xdp_xmit_zc()` — the only function that moves XSK TX descriptors to the
hardware ring — is called *exclusively* from `igc_clean_tx_irq()`, i.e. from the **TX**
queue-vector's NAPI handler. `need_wakeup` is likewise only ever set there. Two facts make
that fatal:

- With `combined=1` the i225 runs **unpaired** `rx-0` / `tx-0` MSI-X vectors —
  `igc_set_flag_queue_pairs()` only pairs RX and TX onto one vector when more than half of the
  4 max queues are in use. (i40e always uses paired `TxRx` vectors, so its RX poll services TX
  completions in the same NAPI pass — which is why i40e honours the AF_XDP contract.)
- Busy-polling and `sendto()` kicks drive the **RX** NAPI only. On igc a kick returns having
  done nothing.

So a zero-copy application **cannot cause its own TX descriptors to be transmitted**. Frames
leave only when unrelated kernel traffic (IPv6 housekeeping, ARP, anything addressed to the
port) happens to run the TX vector's NAPI. Streaming tests hide this — interrupts are always in
flight. A ping-pong exposes it: after the first hop there is no such traffic, and it deadlocks.

**Reproduce it with xdpsock + mausezahn.** Run `xdpsock -l` (l2fwd = MAC-swap echo) on **both**
ports of a back-to-back pair, then inject **one** seed frame. l2fwd swaps src↔dst, so that
single frame bounces A→B→A→B forever: exactly one frame in flight, self-paced at the real RTT.
No injector loop, no external pacing.

```bash
# xdpsock binds queue 0 ONLY — RSS on >1 queue steers frames elsewhere and fakes a "hang"
sudo ethtool -L enp5s0 combined 1
sudo ethtool -L enp6s0 combined 1

# l2fwd on BOTH ports: zero-copy (-z), busy-poll (-B), batch 1, no stats thread (-Q)
sudo taskset -c 5 ./xdpsock -i enp5s0 -q 0 -l -z -B -b 1 -a -Q &
sudo taskset -c 6 ./xdpsock -i enp6s0 -q 0 -l -z -B -b 1 -a -Q &

# inject exactly ONE frame: dst = enp6s0's MAC, src = enp5s0's MAC, ethertype 0x88b5, 46B pad
sudo mausezahn enp5s0 -c 1 \
  "f0 2f 74 b1 41 97  f0 2f 74 b1 41 96  88 b5  $(printf '00 %.0s' $(seq 46))"

# watch it bounce (or not)
watch -n1 'ethtool -S enp5s0 | grep -w rx_packets; ethtool -S enp6s0 | grep -w rx_packets'
```

Run the identical procedure on each NIC — same binary, same flags, one variable (the driver):

| NIC | Result after one injected frame |
|---|---|
| ConnectX-4 Lx (mlx5) | bounces indefinitely |
| XXV710 (i40e) | **1,035,345 hops/side** (≈12.3 µs/RTT) |
| **I225-V (igc)** | **freezes at `rx=1 / tx=1`** |

On igc the counters stop dead after the seed frame is received once. Meanwhile a
*unidirectional* `xdpsock -t` → `xdpsock -r` on the very same ports runs fine **at the same
time** — the RX path is healthy; only the TX pump is missing. `-m` (kick TX unconditionally,
ignoring `need_wakeup`) changes nothing, which rules out "the driver just forgot to set the
wakeup flag": the kick itself is a no-op because it drives the RX NAPI.

> **Trap:** `xdpsock -t -C 1` (send one packet *total*) always succeeds on igc, because
> `igc_configure()` pre-arms ~2047 RX descriptors at setup. That is a primed ring, not a
> healthy driver. Only a **sustained** send→wait→send sequence exposes the stall.

**Implication.** igc AF_XDP-ZC is not a viable low-latency transport, and no userspace
workaround exists — the defect is in the driver's NAPI wiring. The DPDK-over-AF_XDP vdev does
**not** escape it either, since it rides the same `igc.ko` zero-copy path. The only viable path
for this NIC is the **native DPDK `net_igc` PMD over vfio-pci**, which takes `igc.ko` out of
the loop entirely. An upstream report to intel-wired-lan is planned.

### 2.2 DPDK: `nb_txq = 0` silently breaks RX

**Symptom.** An RX-only port configures cleanly and starts, the NIC receives every frame
(`ipackets` climbs), but `rte_eth_rx_burst()` returns 0 forever — and `imissed` stays 0.

**Mechanism.** Unlike mlx5 and i40e, the igc PMD does not tolerate an asymmetric queue count:
configuring 0 TX queues succeeds but leaves RX descriptor write-back dead.

**Fix.** Configure ≥ 1 queue in *both* directions on igc, even for unidirectional roles —
`setSymmetricQueues(true)` in `src/app/TransportTraits.hpp`.

### 2.3 DPDK: RX `WTHRESH = 0` stops write-back, and `imissed` lies

**Symptom.** Setting `rxconf.rx_thresh.wthresh = 0` — attempting immediate per-descriptor
write-back, a valid idiom on other Intel parts — gives: TX port `opackets = 300000`, RX port
`ipackets = 300000`, app `recorded = 0`, `imissed = 0`. Every frame arrives at the NIC and none
reaches the application, and the stats report no loss.

**Mechanism.** On i225 the write-back threshold must be non-zero; with 0 the write-back engine
has no trigger and never posts a DD bit. The PMD's `imissed` is never populated (the MPC
counter isn't wired up), so the loss is invisible to the standard stats.

**Fix.** Keep the PMD default (4). It costs nothing at one frame in flight, because EITR sits
at its post-reset default of 0 — there is no moderation timer delaying the flush. **And never
trust `imissed` on igc:** cross-check `opackets` (TX port) against `ipackets` (RX port) against
the application's own count.

### 2.4 DPDK: the 2.5G PHY is slower than 1G — run the link at 1 GbE

**Symptom.** The I225-V is **faster at 1 GbE than at its native 2.5 GbE**: 17.143 µs median RTT
at 2.5 GbE against **13.397 µs** at 1 GbE. Dropping the link rate is worth **3.75 µs**.

**Mechanism.** The PHY, and Intel documents it in its own driver. The igc PTP
timestamp-correction constants (`igc.h`, applied per link speed in `igc_ptp.c`) give the i225's
*fixed* MAC+PHY pipeline latency:

| Link speed | TX (ns) | RX (ns) | Per wire traversal |
|---|---:|---:|---:|
| 2500 | 1325 | 1485 | **2.81 µs** |
| 1000 | 80 | 300 | **0.38 µs** |

An RTT crosses the wire twice, so 2.5GBASE-T carries ~5.6 µs of unavoidable PHY latency where
1000BASE-T carries ~0.76 µs. The slower link's extra serialisation of a 64-byte frame (~0.7 µs
RTT) does not come close to paying that back. This is inherent to 802.3bz — 2.5GBASE-T is a
quarter-clocked 10GBASE-T, so the LDPC codeword that occupies 320 ns at 10 G takes 1.28 µs to
fill at 2.5 G and must be buffered whole before it can be decoded.

**Fix.** Restrict the autoneg **advertisement** to 1 G, before `dev_configure`:

```cpp
// src/app/TransportTraits.hpp — kIgcAdvertise1GOnly
if (role.driver == "igc")
    nic.setLinkSpeeds(RTE_ETH_LINK_SPEED_1G);   // -> conf.link_speeds
```

The PMD **rejects** `RTE_ETH_LINK_SPEED_FIXED`, so restricting the advertisement is the only
supported route — autoneg still runs, which is fine for BASE-T master/slave negotiation. Both
ends are the two ports of the same card, so both resolve to 1 G.

**Nothing else on this PMD is a latency lever.** A register-level audit of DPDK 25.11 and the
kernel driver found: EITR is never written by the PMD and the init-time `CTRL.RST` leaves it at
0 (moderation already off); EEE/LPI is already disabled by the PMD's own base init, and the
i225 has no 802.3az support at all (that is i226-only); DMA coalescing is never enabled; the TX
doorbell is written immediately per burst with RS on every descriptor; PCIe ASPM was verified
disabled on both ports. **The igc PMD has no devargs at all.**

---

## 3. DPDK EAL — affects every PMD

**Symptom.** RTT tail spikes on the isolated hot core, with no NIC or application cause. The
`dpdk-intr` thread — EAL's control thread, which services the i40e 50 ms self-re-arming
link/stat alarm, VFIO config-space interrupts and the multiprocess socket — ping-pongs with our
busy-poll loop ~80×/s (proven with `perf record -e sched:sched_switch`).

**Mechanism.** EAL derives its control-thread cpuset from **the caller's affinity at
`rte_eal_init`, minus the dataplane lcores**. Our RTT roles pin the caller to a single isolated
lcore *before* init, so that difference is **empty** — and EAL falls back to pinning the control
thread onto our poll core.

**Fix** (`DPDKEal::ealInit`). Narrow the caller's affinity to the housekeeping core *across* the
`rte_eal_init` call, then restore the caller's own pin:

```cpp
cpu_set_t saved, only;
pthread_getaffinity_np(pthread_self(), sizeof(saved), &saved);
CPU_ZERO(&only);
CPU_SET(kControlThreadCore, &only);                      // core 0 = housekeeping/IRQ core
pthread_setaffinity_np(pthread_self(), sizeof(only), &only);
rte_eal_init(argc, argv);                                // control cpuset = {0}
pthread_setaffinity_np(pthread_self(), sizeof(saved), &saved);
```

Affects every PCI/vfio PMD. The AF_XDP PMD is immune (no PCI interrupt source), but the fix is
harmless there.

**Process-global EAL — why `prepare()` and `init()` are separate.** EAL initialises **once per
process**, and its `-a` allowlist fixes which PCI devices exist for the whole process. The
single-recorder owns both loopback ports in one process, so both BDFs (or both `--vdev` strings)
must be registered *before* the first `init()` triggers that one EAL init. Hence the split, and
hence the non-template `DPDKEal` base holding the lists — a static inside the class *template*
would be duplicated per specialisation (`DPDK<TxOnly>` vs `DPDK<RxOnly>`) and double-init EAL.
Two allowlist rules: entries may carry devargs (`<bdf>,key=val`) and dedup compares only the BDF
part (**first registration wins**); and the AF_XDP vdev path needs `--no-pci --in-memory`, so
two AF_XDP primaries (RTT server + client) can run on one host without colliding on the lock
file.

**SIMD width — Intel's "scalar is a big latency win" guidance did NOT reproduce here.**
`--force-max-simd-bitwidth` selects the i40e PMD's RX/TX path; A/B on XXV710 RTT gave
**AVX-512 ≡ scalar ≈ 9.15 µs median**. The floor is the hardware pipeline, not the vector path.
We ship **256** (AVX2): same latency as 512, keeps throughput headroom, avoids AVX-512's
frequency side effects. Verify which path the PMD actually picked — the cheap way to confirm any
devarg took:

```
[DPDK] xxv0: tx burst mode: Vector AVX2      # rte_eth_{rx,tx}_burst_mode_get()
[DPDK] xxv0: rx burst mode: Vector AVX2
```

**Teardown: `dev_stop` only, never `dev_close`.** `dev_stop` disables queues/DMA/MSI-X and
cancels the periodic alarm, so a device on vfio cannot keep running unattended and contend on
PCIe with the next run — an earlier version without it left the **igc DMA-ing during a
subsequent XXV710 run**, producing a fat tail. We deliberately do not `set_link_up` or
`dev_close`: `dev_stop` parks the PHY, so the next `dev_start` re-syncs from parked (~4.7 ms),
absorbed by the warmup. Also **bind-once**: `shutdown()` does not restore the kernel driver —
doing it in the destructor **hangs**, because the sysfs unbind blocks until vfio releases the
device while EAL still holds it (we never call `rte_eal_cleanup`). Recovery if needed:
`dpdk-devbind --bind=i40e <bdf>`.

---

## 4. Solarflare X2522 — `sfc` / ef_vi

The ef_vi transport uses **CTPIO** (cut-through PIO): the CPU write-combines each frame into a
NIC MMIO aperture and the NIC starts emitting before the frame is fully written. When a CTPIO
write goes wrong the NIC poisons the frame and the send silently falls back to the DMA
descriptor path — the frame still arrives, but ~1 µs later. Both issues below were found
chasing that tail; both fixes are in the published §6 Benchmarks numbers.

### 4.1 CTPIO: the default writer is a fallback lottery — use in_order

**Symptom.** Same binary, same config, back-to-back runs: the server-side CTPIO fallback rate
swings **0.02% → 16%** between runs. Each fallback costs ~+1 µs, so the fallback fraction *is*
the P99+ shelf. (App counters were verified against hardware to the packet:
`ctpio_wins`/`fallbacks` == ethtool `ctpio_success`/`ctpio_poison` per port.)

**Mechanism.** libciul has three CTPIO copy routines, selected by `EF_VI_CTPIO_MODE`:

- **paced** (default) — inserts timing gaps between 64 B write-combining-buffer flushes so the
  NIC's cut-through engine is never starved. But the gap length is calibrated for a
  **hard-coded 10G link** (`ctpio.c`: *"TODO: Supporting 10Gbit link only"*) from a
  **per-process TSC calibration** — wrong pacing on a 25G link, and a different draw every
  process start. That is the run-to-run lottery.
- **fast** — no WC discipline at all. Any adjacent store or coherence activity can evict two
  half-filled WC buffers out of order; the NIC sees a torn (non-contiguous) frame and poisons
  it. Median is the best of the three on a lucky run, with an unbounded fallback tail on an
  unlucky one.
- **in_order** — an `sfence` per 64 B block. WC eviction order becomes **architectural** rather
  than probabilistic.

**Fix.** `EF_VI_CTPIO_MODE=in_order`, plus a CTPIO cut-through threshold ≥ frame length (the
NIC then buffers the whole frame before emitting — no underrun poison either). Cost vs a lucky
fast run: ~15 ns of median. At 64 B a frame crosses a single WC-buffer boundary, so the
vendor's throughput caveat about in_order (written for 1500 B frames) does not apply.
`diagnostic_scripts/rtt_run.sh` exports it by default for ef_vi configs.

**Evidence.** The §6.1 24 h soak: **44.3 billion sends, `ctpio_poison = 0` on both ports.**
No run at any other mode setting ever achieved zero.

### 4.2 The driver's PTP subsystem causes rare µs-scale outliers — compile it out

**Symptom.** A 24 h ef_vi soak on the stock sfc driver leaves a discrete class of **eight
isolated outliers between 3.2 and 7.7 µs** over 44.2 B samples, on a host proven clean.
The identical soak on the same driver minus PTP: **max 3.306 µs, class gone.**

**Mechanism.** The sfc driver runs its PTP machinery even with **no PTP consumer** —
`/dev/ptp*` never opened, no PHC user: a dedicated PTP channel occupies its own MSI-X vector
(9 vectors/port instead of 8) and the management controller delivers 4 Hz time-sync events per
port plus periodic driver work. The discriminator that convicted the kernel driver: the same
outlier band is **absent under DPDK on the same ports** (vfio — no kernel driver attached).

A second, independent lever in the same driver: the **hardware monitor** (`efx_monitor`) polls
sensors every 200 ms by default and put outliers on a strict 3 s grid. It is runtime-writable —
parked via `/etc/modprobe.d`: `options sfc monitor_interval_ms=3600000` (module params reset on
reload, so the modprobe.d pin is mandatory, and never write 0 — that busy-loops).

**Fix — what exactly was removed.** No source patch: Onload's out-of-tree sfc supports building
without PTP. `ptp.h` carries static-inline stubs for the whole PTP API; the Makefile hardcodes
`export CONFIG_SFC_PTP := y`, but a command-line override wins and `config.h` regenerates:

```bash
cd ~/onload/build/x86_64_linux-$(uname -r)       # the mmake build-tree top
PATH=~/onload/scripts:$PATH make CONFIG_SFC_PTP= -j8
sudo cp ...drivers/net/ethernet/sfc/sfc.ko /lib/modules/$(uname -r)/extra/sfc.ko
sudo depmod -a && sudo onload_tool reload
```

That compiles `ptp.c` out entirely — the PTP channel, its MSI-X vector, the 4 Hz MC time-sync
subscription and the periodic work all cease to exist. **Verify which build is loaded:**
`cat /sys/module/sfc/srcversion` (this bench: stock `9B0DECB84BC3A9B0940A929`, PTP-less
`E1C5DF64F9375059E04EE84`); 8 MSI-X vectors/port instead of 9; zero `ptp` rows in
`/proc/interrupts`.

**Result** (24 h A/B, Benchmarks §6.1/§6.2): max **7.679 → 3.306 µs**, median −23 ns, and a
~75–105 ns wider P99–P99.999 (the PTP-less build runs a broad second mode near 2.1–2.2 µs).
Trade a 100 ns shelf for a 4.4 µs cut in worst case — for a bounded-latency claim, the
PTP-less driver wins.
