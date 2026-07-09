# Ultra-Low-Latency NIC Benchmarks — DPDK vs AF_XDP vs Verbs

Closed-loop round-trip latency on identical silicon across three NICs:
**ConnectX-4 Lx** (mlx5), **Intel XXV710-DA2** (i40e), and **Intel I225-V** (igc).
The goal is the comparison asked for constantly online and answered only with
theory: *DPDK vs AF_XDP (vs verbs) on the same NIC, with real tail percentiles.*

> **Status:** ConnectX-4 Lx (§3) and Intel XXV710-DA2 (§4) are **complete** — all six
> 24 h soaks populated (verbs, DPDK, AF_XDP, and DPDK-over-AF_XDP on each NIC). Intel
> I225-V (§5) and the Solarflare X2522 ef_vi comparison are still pending. Sections 1–2
> (system/config/methodology) are final.

### Results at a glance — 24 h RTT (µs), one frame in flight

| NIC | Transport | Median | P99.999 | Max | Samples |
|---|---|---:|---:|---:|---:|
| **ConnectX-4 Lx** (mlx5) | Verbs (RAW_PACKET QP) | **2.426** | 3.272 | 6.080 | 34.0 B |
| **ConnectX-4 Lx** (mlx5) | DPDK (mlx5 PMD) | **3.587** | 4.322 | 8.691 | 23.3 B |
| **ConnectX-4 Lx** (mlx5) | AF_XDP (ZC + busy-poll) | **6.231** | 8.001 | 12.943 | 13.5 B |
| **ConnectX-4 Lx** (mlx5) | DPDK-over-AF_XDP PMD | **6.615** | 8.147 | 13.361 | 13.0 B |
| **Intel XXV710-DA2** (i40e) | DPDK (i40e PMD) | **9.028** | 9.601 | 12.882 | 9.47 B |
| **Intel XXV710-DA2** (i40e) | AF_XDP (ZC + busy-poll) | **10.529** | 11.910 | 13.569 | 8.20 B |
| **Intel XXV710-DA2** (i40e) | DPDK-over-AF_XDP PMD | **10.753** | 11.857 | 13.566 | 8.05 B |

_One-way ≈ RTT / 2. The bypass ladder is monotone on each NIC: the more of the kernel
a transport replaces, the lower and tighter the latency. On mlx5, verbs < DPDK <
AF_XDP < DPDK-over-AF_XDP; on i40e, native DPDK sits at the silicon floor with AF_XDP
~1.5 µs above it._

---

## 1. System

| Component | Specification |
|---|---|
| CPU | Intel Core i9-11900K (Rocket Lake, 8C/16T) — **SMT disabled → 8 active cores** |
| RAM | 16 GB DDR4-3200 _(channel config: TBD)_ |
| Motherboard | ASUS ROG Strix Z590-E Gaming WiFi (BIOS 2405) |
| Cooler | Cooler Master Atmos 360 mm AIO (≈300 W) |
| Disk | 128 GB NVMe M.2 SSD |
| OS | Ubuntu 26.04 |
| Kernel | `7.0.7-hz100` — custom build, `CONFIG_HZ=100`, full `nohz_full` |
| TSC | 3.504 GHz, invariant (`tsc=reliable`) → **0.285 ns/tick** measurement resolution |

### NICs under test

| NIC | Driver | Ports | Link | Notes |
|---|---|---|---|---|
| Mellanox ConnectX-4 Lx | `mlx5_core` / `mlx5_ib` | cx0, cx1 | 2 × 25 GbE | RDMA-capable (verbs) |
| Intel XXV710-DA2 | `i40e` | xxv0, xxv1 | 2 × 25 GbE | DPDK via `vfio-pci` |
| Intel I225-V | `igc` | enp6s0, enp7s0 | 2.5 GbE | DPDK via `vfio-pci` |

> **PCIe topology (Z590-E):** the two 25G add-in NICs run on the CPU PEG bifurcated
> **×8/×8** — ConnectX-4 Lx (×8) and XXV710-DA2 (×8), each at full bandwidth. The
> NVMe SSD sits on a **chipset (PCH) M.2** slot, off the CPU PEG, so it does not
> share the NICs' root complex (no cross-device PCIe contention).

### Hardware

> Photos of the loopback cabling and the NIC cooling are in
> **[Hardware photos](#hardware-photos)** at the end of this document.

---

## 2. Configuration & Methodology

### 2.1 BIOS
- ASUS Multi-Core Enhancement → **Enforce All Limits**
- Power limits: **PL1 = PL2 = 200 W**
- Core voltage: **−0.05 V offset undervolt** (thermal headroom)
- Core ratio: **all-core sync ×50 → 5.00 GHz pinned**
- **C-states disabled** and **SpeedStep (EIST) disabled** — fixed frequency, no idle transitions
- **All PCIe power-saving (ASPM / L-states) disabled**
- **AVX-512 disabled** (frequency/thermal stability — DPDK is built AVX2)
- Hyper-Threading (SMT): **disabled**
- **SATA controller disabled**, **legacy USB support disabled** (trim unused IRQ/SMI sources)

### 2.2 Kernel command line (`7.0.7-hz100`)
```
clocksource=tsc tsc=reliable nowatchdog nmi_watchdog=0 mce=ignore_ce pcie_aspm=off
isolcpus=domain,managed_irq,5-7 irqaffinity=0,1 rcu_nocbs=5-7 nohz_full=5-7 nosmt
cpufreq.default_governor=performance intel_idle.max_cstate=1 processor.max_cstate=1
intel_iommu=on iommu=pt default_hugepagesz=1G hugepagesz=1G hugepages=4
transparent_hugepage=never slub_debug=- module_blacklist=i915,xe
sysctl.vm.stat_interval=86400
```
- **Core isolation:** cores **5–7** isolated (`isolcpus` + `nohz_full` + `rcu_nocbs`);
  housekeeping on 0–4; device IRQs pinned to 0,1. RTT roles: client hot core 5,
  reflector hot core 6, histogram/recorder thread core 7.
- **iGPU disabled** (`module_blacklist=i915,xe`) — its vmap/vunmap churn triggered
  kernel-VA TLB-shootdown IPIs that reached the isolated cores.
- **`vm.stat_interval=86400`** — silences the periodic per-CPU vmstat kworker.
- 1 GiB hugepages, IOMMU passthrough, C-state 1, `performance` governor, no watchdog.

### 2.3 System tuning
- Hot path: one busy-poll thread per isolated core, **SCHED_OTHER** (measured
  statistically equal to SCHED_FIFO:49 on this rig — see §2.5).
- DPDK control/interrupt thread parked on housekeeping core 0 (off the poll cores).
- NIC completion IRQs kept off the poll cores where the queue count allows.

### 2.4 Measurement methodology
- **Loopback:** the two ports of each dual-port NIC are DAC-linked; one port is the
  client (originator), the other reflects every frame (MAC-swap echo).
- **Closed-loop RTT:** exactly one frame in flight — send → spin for the matching
  echo → record. One-way ≈ RTT / 2.
- **Timing:** `rdtscp` taken on a single pinned core (no cross-core skew); raw TSC
  cycle deltas, converted to µs once at report time.
- **Aggregation:** HdrHistogram, 5 significant figures over raw cycles →
  **~0.285 ns resolution** up to ~75 µs; O(1) memory, exact 64-bit counts.
- **Duration:** 24 h per transport; first 500k samples discarded as warm-up.
  Percentiles stabilise while the max ratchets — headline metric is **P99.999**.
- **Outputs:** `<name>.csv` (percentile) + `<name>.hist.csv` (per-bucket) under
  `test/latency_analysis/`, plotted with `plot_latency_hist.py`.

### 2.5 Notes on determinism (validated, not assumed)
- SCHED_OTHER vs SCHED_FIFO:49 — statistically tied on both NICs on a quiet box.
- The residual tail is the OS/observer, not the stack: the campaign's tightest
  distribution (XXV710 DPDK) holds **P99.999 within 0.57 µs of the median over
  9.5 B packets**, and the observer effect traced entirely to device IRQs landing on
  the isolated cores — steering WiFi/completion IRQs onto housekeeping cores 0–1 was
  what removed it (SSH-in + live `sensors` monitoring then had no measurable effect).
- On i40e the SCHED_FIFO busy-poll + NAPI-deferral strand is real but avoided by
  running SCHED_OTHER; see `Known_driver_issues.md` §1.2.

---

## 3. ConnectX-4 Lx (mlx5)

### 3.1 Verbs & DPDK

#### Verbs — RAW_PACKET QP, mlx5dv direct CQE poll

| Metric | RTT (µs) |
|---|---|
| Min | 2.112 |
| Median | 2.426 |
| P99 | 2.781 |
| P99.9 | 2.864 |
| P99.99 | 3.030 |
| P99.999 | 3.272 |
| Max | 6.080 |
| Mean | 2.460 |
| Samples | 34,024,754,354 |

![CX4 verbs histogram](../test/latency_analysis/ConnectX_4_Lx/connectx_4_lx_verbs_rtt_hist.png)

The lowest-latency path on this NIC — median **2.426 µs RTT (~1.21 µs one-way)**, and
the tail stays inside **3.272 µs at P99.999** across 34 billion packets.

#### DPDK — mlx5 PMD (bifurcated, kernel keeps the netdev)

| Metric | RTT (µs) |
|---|---|
| Min | 3.174 |
| Median | 3.587 |
| P99 | 3.990 |
| P99.9 | 4.060 |
| P99.99 | 4.282 |
| P99.999 | 4.322 |
| Max | 8.691 |
| Mean | 3.628 |
| Samples | 23,279,093,850 |

![CX4 DPDK histogram](../test/latency_analysis/ConnectX_4_Lx/connectx_4_lx_dpdk_rtt_hist.png)

~1.16 µs slower than verbs at the median (the PMD's generic mbuf datapath vs the
hand-tuned direct CQE poll), but the **tightest tail of any transport here**:
P99.999 4.322 µs, only 0.74 µs above the median.

### 3.2 AF_XDP — zero-copy + busy-poll

> mlx5 is the one NIC where one-in-flight busy-poll achieves low latency (the CQE is
> written back promptly), so AF_XDP-ZC is viable here.

To compete with DPDK, AF_XDP must run **zero-copy** (frames DMA'd straight into the
user-space umem via the native driver — no kernel copy) and **busy-poll** (the app
drives the NAPI inline through the socket syscall, polling the rings instead of
waiting on interrupts). Both are **on by default** in our harness. Two tunables vary:

- **NAPI hard-IRQ deferral** — `napi_defer_hard_irqs` keeps the queue serviced by
  busy-poll for N rounds before re-arming the hardware IRQ; `gro_flush_timeout` is the
  backstop timer (ns) that re-arms the IRQ if the app stops polling. Together they let
  busy-poll suppress interrupts (lower jitter) — the regime the canonical AF_XDP
  README recommends:
  ```
  echo 2 | sudo tee /sys/class/net/<interface>/napi_defer_hard_irqs
  echo 200000 | sudo tee /sys/class/net/<interface>/gro_flush_timeout
  ```
  Reference: <https://github.com/xdp-project/bpf-examples/blob/main/AF_XDP-example/README.org>
- **`XDP_USE_NEED_WAKEUP`** — bind flag: when set, the app kicks the kernel
  (`sendto`/`poll`) only when the ring raises `need_wakeup`; when unset, the app
  always kicks, driving the NAPI inline every cycle.

#### Config sweep (5-min, to select the 24 h config)
All four keep zero-copy + busy-poll on; they vary `XDP_USE_NEED_WAKEUP` × deferral:

| Config | Min     | Median  | P99     | P99.9    | P99.99   | P99.999  | Max       |
|---|---------|---------|---------|----------|----------|----------|-----------|
| A — need_wakeup + deferral | _5.472_ | _6.298_ | _7.248_ | _7.550_  | _7.882_  | _8.031_  | _10.547_  |
| B — need_wakeup, no deferral | _6.231_ | _8.039_ | _9.779_ | _10.574_ | _10.862_ | _12.732_ | _181.301_ |
| C — no need_wakeup + deferral | _5.528_ | _6.232_ | _7.176_ | _7.520_  | _7.822_  | _8.009_  | _11.602_  |
| D — no need_wakeup, no deferral | _6.284_ | _7.944_ | _9.651_ | _10.465_ | _10.831_ | _15.101_ | _175.629_ |

_(all µs, RTT; 5-min runs.)_ **Chosen for 24 h: _Config C_.**

#### 24 h soak (Config C)

| Metric | RTT (µs) |
|---|---|
| Min | 5.303 |
| Median | 6.231 |
| P99 | 7.146 |
| P99.9 | 7.370 |
| P99.99 | 7.780 |
| P99.999 | 8.001 |
| Max | 12.943 |
| Mean | 6.389 |
| Samples | 13,460,287,612 |

![CX4 AF_XDP histogram](../test/latency_analysis/ConnectX_4_Lx/connectx_4_lx_af_xdp_rtt_hist.png)

The 24 h soak lands right on the Config C sweep (median 6.231 vs 6.232) — busy-poll
zero-copy holds its shape over 13.5 billion packets. **~2.6 µs slower than DPDK**: the
cost of driving the NAPI through the AF_XDP socket layer rather than owning the ring
in userspace.

### 3.3 DPDK over AF_XDP (AF_XDP PMD)

> DPDK's `net_af_xdp` vdev PMD — the DPDK API layered on top of the kernel AF_XDP
> socket (bifurcated, no vfio unbind). Answers "what does the general framework cost
> on top of raw AF_XDP?" Same Config C (zero-copy + busy-poll + deferral) as §3.2.

| Metric | RTT (µs) |
|---|---|
| Min | 5.382 |
| Median | 6.615 |
| P99 | 7.265 |
| P99.9 | 7.793 |
| P99.99 | 7.881 |
| P99.999 | 8.147 |
| Max | 13.361 |
| Mean | 6.592 |
| Samples | 13,027,086,306 |

![CX4 DPDK-AF_XDP histogram](../test/latency_analysis/ConnectX_4_Lx/connectx_4_lx_dpdk_af_xdp_rtt_hist.png)

**+0.38 µs over native AF_XDP** at the median (6.615 vs 6.231) — the PMD's extra mbuf
alloc/copy on each side of the socket. The tail tracks native AF_XDP closely (P99.999
8.147 vs 8.001). So on mlx5 the ladder is complete and monotone:
**verbs 2.43 < DPDK 3.59 < AF_XDP 6.23 < DPDK-over-AF_XDP 6.62 µs.**

---

## 4. Intel XXV710-DA2 (i40e)

### 4.1 DPDK — i40e PMD (vfio-pci)

> Sits at the i40e silicon floor (~9 µs RTT); application software is ~180 ns of the
> path, the rest is NIC + PCIe + 25G wire.

| Metric | RTT (µs) |
|---|---|
| Min | 8.581 |
| Median | 9.028 |
| P99 | 9.449 |
| P99.9 | 9.508 |
| P99.99 | 9.547 |
| P99.999 | 9.601 |
| Max | 12.882 |
| Mean | 9.038 |
| Samples | 9,472,423,727 |

![XXV710 DPDK histogram](../test/latency_analysis/XXV710_DA2/xxv710_da2_dpdk_rtt_hist.png)

The determinism headline of the whole campaign: median **9.028 µs** and **P99.999
9.601 µs** — a **0.57 µs spread across five nines over 9.5 billion packets.** This is
the "software *is* real-time" data point; the entire distribution is silicon + wire,
not the stack. Software is ~180 ns of the path.

### 4.2 AF_XDP — zero-copy + busy-poll

> On i40e, one-in-flight RX descriptor write-back is ITR-gated (~50 µs adaptive), so
> our AF_XDP path clears the ITR register directly via a BAR0 mmap to reach ~10.5 µs
> RTT, and runs **SCHED_OTHER** to avoid the FIFO busy-poll / NAPI-deferral strand
> (see `Known_driver_issues.md` §1.2). Kernel 7.0 i40e has no in-driver busy-poll —
> the `netif_napi_add_config` path exists only in `ice` (E810), not `i40e`.

Same 4-config sweep as **§3.2** (zero-copy + busy-poll baseline; the deferral knobs
and `XDP_USE_NEED_WAKEUP` flag are defined there), run on `xxv0`/`xxv1`.

#### Config sweep (5-min, to select the 24 h config)

| Config | Min     | Median   | P99      | P99.9    | P99.99   | P99.999   | Max       |
|---|---------|----------|----------|----------|----------|-----------|-----------|
| A — need_wakeup + deferral | _9.563_ | _10.480_ | _11.125_ | _11.391_ | _11.555_ | _11.702_  | _12.538_  |
| B — need_wakeup, no deferral | _9.357_ | _10.156_ | _10.766_ | _10.985_ | _11.164_ | _13.949_  | _202.22_  |
| C — no need_wakeup + deferral | _9.430_ | _10.517_ | _11.034_ | _11.208_ | _11.346_ | _11.498_  | _12.008_  |
| D — no need_wakeup, no deferral | _9.365_ | _10.103_ | _10.766_ | _11.012_ | _11.233_ | _101.938_ | _302.762_ |

_(all µs, RTT; 5-min runs.)_ **Chosen for 24 h: _Config C_.**

#### 24 h soak (Config C)

| Metric | RTT (µs) |
|---|---|
| Min | 9.391 |
| Median | 10.529 |
| P99 | 11.101 |
| P99.9 | 11.348 |
| P99.99 | 11.646 |
| P99.999 | 11.910 |
| Max | 13.569 |
| Mean | 10.502 |
| Samples | 8,203,692,200 |

![XXV710 AF_XDP histogram](../test/latency_analysis/XXV710_DA2/xxv710_da2_af_xdp_rtt_hist.png)

**~1.5 µs above native DPDK** (10.529 vs 9.028) — on i40e the AF_XDP socket path can't
reach the vfio PMD's floor even with the ITR cleared. The tail is well-behaved
(P99.999 11.910, max 13.569 over 8.2 billion packets).

### 4.3 DPDK over AF_XDP (AF_XDP PMD)

> DPDK's `net_af_xdp` PMD on i40e. **The stock PMD does not clear the i40e ITR**, so
> out of the box it runs at **~51 µs median RTT** (adaptive-ITR RX-writeback
> throttling). We added the same BAR0 ITR-clear the native paths use — resolving the
> BDF from the ifname when the `af_xdp` vdev is bound to i40e — and with it the PMD
> converges to the native AF_XDP floor. Both numbers below are with the fix.

| Metric | RTT (µs) |
|---|---|
| Min | 9.489 |
| Median | 10.753 |
| P99 | 11.333 |
| P99.9 | 11.637 |
| P99.99 | 11.758 |
| P99.999 | 11.857 |
| Max | 13.566 |
| Mean | 10.641 |
| Samples | 8,053,389,198 |

![XXV710 DPDK-AF_XDP histogram](../test/latency_analysis/XXV710_DA2/xxv710_da2_dpdk_af_xdp_rtt_hist.png)

**Stock ~51 µs → ITR-cleared 10.753 µs.** With the fix it sits **+0.22 µs over native
AF_XDP** (10.753 vs 10.529) — the same small PMD mbuf tax seen on mlx5 (+0.38 µs),
confirming the 51 µs was pure ITR throttling and not the PMD itself.

---

## 5. Intel I225-V (igc)

### 5.1 DPDK — igc PMD (vfio-pci)
_[24 h soak pending]_

| Metric | RTT (µs) |
|---|---|
| Min | _TBD_ |
| Median | _TBD_ |
| P99 | _TBD_ |
| P99.9 | _TBD_ |
| P99.99 | _TBD_ |
| P99.999 | _TBD_ |
| Max | _TBD_ |
| Mean | _TBD_ |
| Samples | _TBD_ |

![I225-V DPDK histogram](../test/latency_analysis/TBD.png)

### 5.2 AF_XDP — zero-copy + busy-poll
_[24 h soak pending]_

> igc busy-poll works for throughput but stalls the one-in-flight RTT (Intel-wide
> RX write-back starvation, same class as i40e). _Confirm latency-mode behaviour._

| Metric | RTT (µs) |
|---|---|
| Min | _TBD_ |
| Median | _TBD_ |
| P99 | _TBD_ |
| P99.9 | _TBD_ |
| P99.99 | _TBD_ |
| P99.999 | _TBD_ |
| Max | _TBD_ |
| Mean | _TBD_ |
| Samples | _TBD_ |

![I225-V AF_XDP histogram](../test/latency_analysis/TBD.png)

---

## Appendix — reproduction

- Toolchain / build: `diagnostic_scripts/build.sh` (x86_64 Release).
- Run: `diagnostic_scripts/rtt_run.sh <config.toml>` per NIC/transport.
- Plot: `python3 test/latency_analysis/plot_latency_hist.py <name>.hist.csv --title "..."`.
- Raw data: `test/latency_analysis/*.csv` (+ `*.hist.csv`).

---

## Hardware photos

![Dual-port NICs DAC-looped port-to-port](DAC_loopback.jpg)
*Both 25 G NICs — ConnectX-4 Lx and Intel XXV710-DA2 — DAC-looped port 0 ↔ port 1
(blue pull-tab DAC cables). Each frame the client port sends is reflected by the
card's second port, giving the exactly-one-frame-in-flight closed loop.*

![Auxiliary cooling fans ducted across the NIC heatsinks](NIC_with_fans.jpg)
*Add-in 12 V fans over the passively-cooled NIC heatsinks. Forced airflow dropped the
ConnectX-4 Lx from 80 °C+ to under 50 °C and keeps the Solarflare X2522 inside its
thermal envelope for sustained 24 h soaks — the cards ship with no fan of their own.*
