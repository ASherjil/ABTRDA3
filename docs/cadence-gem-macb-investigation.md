# Cadence GEM/macb PMD Investigation — cfd-865-mkdev50

Investigation date: 2026-04-16

## Hardware Summary

| Property | Value |
|----------|-------|
| SoC | Xilinx Zynq UltraScale+ (ZynqMP) |
| Board | diot_v4 |
| CPU | 4x ARM Cortex-A53 (aarch64), BogoMIPS 100.00 |
| RAM | ~8 GB |
| Kernel | 6.6.40-xlnxLTS20242 |
| Interface | `end0` (renamed from eth0) |
| MAC | fc:0f:e7:1b:8b:55 |
| Speed | 1 Gbps, Full duplex |
| PHY | Microchip LAN8841, RGMII-ID mode (internal delay TX+RX), MDIO addr **31** (0x1f) |
| PHY ID | 0x00221652 |
| Driver | macb (built-in, CONFIG_MACB=y) |
| GEM revision | 0x50070106 |

## Bus Architecture — Platform Device (NOT PCIe)

The GEM is a platform device on the AXI bus, memory-mapped at a fixed address.
There is NO PCIe bus on this device (`lspci` fails — no `/proc/bus/pci`).

| Property | Value |
|----------|-------|
| MMIO base | `0xff0b0000` |
| MMIO size | 4 KB (0x1000) |
| Device tree path | `/axi/ethernet@ff0b0000` |
| Compatible | `xlnx,zynqmp-gem`, `cdns,gem` |
| Sysfs path | `/sys/devices/platform/axi/ff0b0000.ethernet` |
| DMA coherent | NO — explicit cache flush/invalidate needed on ARM64 |
| IOMMU | None |
| UIO devices | 15 already present (`/dev/uio0` – `/dev/uio14`) |

### Unbinding the kernel driver

Since macb is built-in (`CONFIG_MACB=y`), cannot use `rmmod`. Must use sysfs:

```bash
# Unbind
echo ff0b0000.ethernet > /sys/bus/platform/drivers/macb/unbind

# Re-bind
echo ff0b0000.ethernet > /sys/bus/platform/drivers/macb/bind
```

### MMIO access for PMD

No PCIeBackend — need a PlatformBackend that does:
```cpp
int fd = open("/dev/mem", O_RDWR | O_SYNC);
void* base = mmap(nullptr, 0x1000, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0xff0b0000);
```

Or use an existing UIO device if one covers the GEM region.

## Queue Architecture — 2 Independent Hardware Queues

**Confirmed via DCFG6 register (0x294) = 0x02D10002.**

The GEM has 2 truly independent DMA queues, each with separate descriptor rings.

### Descriptor ring base address registers (live values from hardware)

| Queue | TX Base Register | TX Ring Address | RX Base Register | RX Ring Address |
|-------|-----------------|----------------|-----------------|----------------|
| Q0 | `0x01C` (TBQP) | `0x5FF00618` | `0x018` (RBQP) | `0x5FF05E60` |
| Q1 | `0x440` | `0x5FF0A160` | `0x480` | `0x5FF0C000` |

Four unique, non-zero addresses — each queue owns its own descriptor ring in memory.

### Per-queue interrupt registers

| Register | Q0 Offset | Q1 Offset |
|----------|-----------|-----------|
| ISR (status) | `0x024` | `0x400` |
| IER (enable) | `0x02C` | `0x600` |
| IDR (disable) | `0x030` | `0x620` |
| IMR (mask) | `0x028` | `0x640` |

For a poll-mode driver, disable all interrupts and poll descriptor rings directly.

### TX trigger (TSTART) is global but harmless

`TSTART` is bit 9 of the Network Control Register (`0x000`). It is a global signal that tells
ALL queue DMA engines to check their descriptor rings. It is edge-triggered — if a queue
has no new descriptors, its DMA engine does nothing. This does NOT cause contention
between queues.

### RX packet steering via hardware screening registers

The GEM has hardware screeners that steer incoming packets to specific queues:

- **4 Type 1 screeners** at `0x500–0x50C` (DSCP/priority based)
- **4 Type 2 screeners** at `0x540–0x54C` (EtherType + compare based)
- **4 EtherType compare registers** at `0x6E0–0x6EC`
- **4 Compare A registers** at `0x700–0x71C`

Type 2 screener register format:
```
Bits [3:0]  = target queue number (GEM_QUEUE)
Bits [11:9] = EtherType compare register index (GEM_ETHT2IDX)
Bit  [12]   = EtherType compare enable (GEM_ETHTEN)
```

Example configuration for PMD:
- Write `0x88B5` to EtherType compare register 0 (`0x6E0`)
- Write screener 0 (`0x540`): queue=0, etht2idx=0, ethten=1 → `0x1000`
- All EtherType 0x88B5 packets → Queue 0 (hot path)
- Everything else → Queue 1 (kernel/NFS/SSH via TAP bridge)

### Confirmed hardware register values

| Register | Offset | Value | Meaning |
|----------|--------|-------|---------|
| Net Control | `0x000` | `0x0010001C` | TX+RX enabled, MDIO enabled |
| Net Config | `0x004` | `0x012E044A` | GigE, full duplex, FCS remove, 64-bit bus |
| DMA Config | `0x010` | `0x70180F10` | 1536B RX buffer, burst 16, TX csum offload |
| DCFG5 | `0x290` | — | Design config (queue count) |
| DCFG6 | `0x294` | `0x02D10002` | Confirms 2 priority queues |
| DCFG8 | `0x29C` | `0x04040404` | 4 screeners, 4 ethertype, 4 compare A/B |

### ethtool queue information

```
$ ethtool -l end0
Channel parameters for end0:
Pre-set maximums:
RX:     1
TX:     1
Other:  0
Combined:   1
Current hardware settings:
RX:     1
TX:     1
Other:  0
Combined:   1
```

Note: ethtool reports 1 combined channel because the Linux macb driver exposes
queues as priority queues, not separate channels. The hardware still has 2
independent DMA queues as confirmed by the register dump.

## Proposed PMD Architecture — Two-Thread Design

```
                    Single wire (1 Gbps RGMII)
                           |
                    +------+------+
                    |  MAC + PHY  |
                    | (LAN8841)   |
                    +------+------+
                           |
                    +------+------+
              RX:   | HW Screener |  EtherType 0x88B5 -> Q0
                    |             |  everything else  -> Q1
                    +--+------+---+
                       |      |
              TX:   +--+--++--+--+   TX arbiter: Q1 priority > Q0
                    |     ||     |
               +----+---+ +-----+---+
               |Queue 0 | |Queue 1  |
               |TX ring | |TX ring  |  <- separate DMA descriptors
               |RX ring | |RX ring  |  <- separate DMA descriptors
               |ISR/IMR | |ISR/IMR  |  <- separate interrupt status
               +--------+ +---------+
                  |              |
            Thread 1          Thread 2
            (hot path,        (TAP bridge,
             polling,          usleep(200),
             SCHED_FIFO)       SCHED_OTHER)
```

### Thread 1 — Hot path (pinned core, SCHED_FIFO)
- Polls Queue 0 RX descriptors for test traffic (EtherType 0x88B5)
- Sends replies on Queue 0 TX descriptors
- Never touches TAP fd, never does syscalls
- Zero contention with Thread 2

### Thread 2 — TAP bridge (unpinned, SCHED_OTHER, ~5000 polls/sec)
- Polls Queue 1 RX descriptors for kernel traffic (NFS, SSH, ARP, DHCP)
- Writes received packets to TAP fd → kernel networking stack
- Reads TAP fd for kernel TX → writes to Queue 1 TX descriptors
- `usleep(200)` between polls to avoid 100% CPU (~200us extra latency on SSH, invisible)

### No SPSC queues needed

Unlike the I210 (which would need SPSC queues because it can't steer RX packets
to queues by EtherType in hardware), the GEM's hardware screening gives
packet-level separation at the DMA layer. Each thread owns its hardware queue
end-to-end — no shared data structures.

### TAP device setup sequence

Critical: must set up TAP with end0's IP/routes BEFORE unbinding macb.

1. Create TAP device, assign end0's IP address and routes to it
2. Unbind macb via sysfs
3. PMD takes over hardware, starts both threads
4. On shutdown: stop threads, re-bind macb, restore IP/routes to end0

## Key Differences from I210 PMD

| Aspect | I210 (x86_64) | GEM/macb (ARM64) |
|--------|---------------|-------------------|
| Bus | PCIe Gen1 x1 | AXI (platform device) |
| MMIO access | PCIeBackend (BAR0) | mmap /dev/mem at 0xff0b0000 |
| Driver unbind | rmmod igb | sysfs unbind (built-in) |
| DMA coherent | Yes (x86 snooped) | No — cache flush/invalidate needed |
| RX screening | Software EtherType check | Hardware screener registers |
| Queues | 4 TX / 4 RX | 2 TX / 2 RX |
| TAP bridge needed | No (spare ports) | Yes (single port, PXE/NFS/SSH) |
| Register space | 512 KB | 4 KB |
| PHY | Internal GS40G | External LAN8841 via MDIO |
| PTP | SYSTIML/SYSTIMH | TSU (gem-ptp-timer) |

## Why Kernel-Mode Queue Separation Fails (packet_mmap / AF_PACKET)

Even though the hardware has 2 independent DMA queues with screening registers,
**four independent problems** prevent the kernel networking stack from using them
for traffic separation:

### Problem 1: Single interrupt wire (hardware — unfixable)

The Zynq UltraScale+ SoC routes ONE GIC SPI per GEM instance, shared by all queues.
This is hardwired in silicon — not a driver or device tree bug.

```
/proc/interrupts:
51:  6572540  0  0  0  GICv2  89 Level  end0, end0
                                         Q0     Q1 — same IRQ!

/proc/device-tree/.../interrupts:
  Both entries = GIC SPI 57 (IRQ 89)
```

Consequence: cannot affinitize queue interrupts to different CPUs.

### Problem 2: macb driver doesn't tag packets with queue number

`gem_rx()` in macb_main.c calls `napi_gro_receive()` but never calls
`skb_record_rx_queue()`. The networking stack cannot identify which hardware
queue received a given packet.

### Problem 3: No way to bind packet_mmap to a hardware queue

`AF_PACKET` / `packet_mmap` has no API to say "give me only packets from
hardware queue N." `PACKET_FANOUT_QM` mode exists for this purpose, but it
relies on `skb_get_rx_queue()` — which returns 0 for every packet because
the driver doesn't tag them (Problem 2).

### Problem 4: Single interrupt = no per-queue CPU affinity

With one IRQ for both queues, all NAPI processing fires from the same IRQ
on the same CPU. Cannot pin queue 0's processing to an isolated core while
letting queue 1 run on a shared core.

### The stack of failures

```
Hardware:     1 GIC SPI ──────────► can't separate IRQ affinity
                  │
Device Tree:  both entries = SPI 57 ► platform_get_irq() returns same IRQ
                  │
macb driver:  no skb_record_rx_queue() ► kernel can't tell queues apart
                  │
AF_PACKET:    no queue binding API ──► packet_mmap gets merged stream
                  │
              ════╧════════════════
              Everything arrives as one undifferentiated
              stream at the socket layer
```

### Why the PMD bypasses all of this

With a custom PMD (unbind macb, mmap registers directly):
1. Don't care about interrupts — poll descriptor rings directly
2. Don't care about SKB tagging — read from Q0 or Q1 ring, you know which is which
3. Don't need packet_mmap queue binding — you own the hardware
4. Can pin each queue's poll loop to a different CPU core

The hardware screening registers work perfectly at the DMA level — it's only
the kernel's interrupt/socket abstraction that collapses the queue separation.
The PMD operates below that layer.

### Note on driver architecture

The macb driver is actually properly designed for multi-queue internally:
- Two independent NAPI instances (one per queue)
- `macb_interrupt()` reads ISR only for its own queue
- `macb_rx_poll()` drains only its own queue's descriptor ring

The problem is upstream of the driver — the SoC only provides one interrupt
wire, and the kernel socket layer has no queue-aware delivery mechanism.

### ethtool -N flow steering

The driver DOES support programming screening registers via `ethtool -N`
(gem_add_flow_filter → programs Type 2 SCRT2 registers). However, ethtool
is not installed on cfd-865-mkdev50. Even if it were, steering packets to
queue 1's DMA ring doesn't help because the socket layer merges them anyway.

## Linux Kernel Source Analysis — What to Port

Source path: `/nfs/cs-ccr-nfshome/user/asherjil/ABTTiming/linux-xlnx/drivers/net/ethernet/cadence/`

### File breakdown

| File | Lines | Purpose | What to port |
|------|-------|---------|-------------|
| `macb.h` | 1486 | Register defs, bit fields, descriptor structs | **~950 lines near-verbatim** → `GEMRegisters.hpp` |
| `macb_main.c` | 6040 | Driver logic | **~12% copy, ~8% rewrite, ~80% skip** |
| `macb_ptp.c` | 466 | Hardware timestamping (TSU) | **~70 lines** for future PTP support |
| `macb_pci.c` | 133 | PCIe wrapper for Cadence eval boards | **Skip entirely** (we're a platform device) |

### macb.h — near-verbatim copy to GEMRegisters.hpp

**Keep (lines 17-970)**: All register offsets, bit field defines (`MACB_RE_OFFSET` etc.),
bit-manipulation macros (`MACB_BIT`, `MACB_BF`, `MACB_BFEXT`, `MACB_BFINS` + GEM_ variants),
descriptor structs (`macb_dma_desc`, `macb_dma_desc_64`, `macb_dma_desc_ptp`), constants.
These are pure hardware definitions — portable C.

**Delete**:
- Lines 10-16: Linux kernel `#include`s (clk.h, phylink.h, ptp_clock_kernel.h, net_tstamp.h,
  interrupt.h, phy/phy.h, workqueue.h)
- Lines ~820-852: register access macros that reference `struct macb` (`macb_readl`,
  `gem_readl`, `queue_readl`, etc.) — we use our own `AXIBackend::readReg/writeReg`
- Lines 972-1486: All kernel driver structs (`macb_tx_skb`, `macb_queue`, `macb`) —
  they use `sk_buff`, `napi_struct`, `spinlock_t`, `phylink`, `clk`, `ptp_clock` etc.

**Add at top**:
```cpp
#include <cstdint>
using u8  = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
```

### macb_main.c — Tier 1: Copy verbatim (~700 lines)

Just substitute `m_bus.writeReg()` for `gem_writel()` / `macb_writel()` / `queue_writel()`:

| Function | Lines | Purpose |
|----------|-------|---------|
| `hw_readl`, `hw_writel`, native variants | 223-241 | Register primitives (uses `relaxed` — zero barrier overhead) |
| `macb_set_addr` / `macb_get_addr` | 1107-1142 | 64-bit descriptor address + `dma_wmb()` (preserve barrier!) |
| `macb_tx_dma`, `macb_rx_ring_wrap`, `macb_rx_desc`, `macb_rx_buffer` | 194-220 | Ring index arithmetic |
| `macb_reset_hw` | 2746-2776 | Full reset: disable RX/TX, clear stats, disable IRQs per queue |
| `macb_init_hw` | 2897-2930 | Orchestrates NCFGR/PBUFRXCUT/DMA config |
| `macb_configure_dma` | 2851-2895 | DMA engine (burst, RX buffer size, 64b, PTP) |
| `macb_init_buffers` | 489-512 | Program RBQPH/TBQPH and per-queue ring bases |
| `macb_set_hwaddr` / `macb_get_hwaddr` | 272-324 | MAC address via SA1B/SA1T |
| `macb_mdc_clk_div` / `gem_mdc_clk_div` / `macb_dbw` | 2778-2842 | Capability probing |
| `gem_init_rings` / `macb_init_rings` / `macb_init_rx_ring` | 1631-2744 | Descriptor ring init |
| `macb_tx_complete_pending`, `macb_rx_pending` | 1728-1822 | Polling checks (drop spin_lock) |
| `macb_halt_tx`, `macb_tx_restart` | 1076-1805 | Stall recovery |
| **`gem_enable_flow_filters`, `gem_prog_cmp_regs`** | 3712-3844 | **Screener programming for our Q0/Q1 steering** |
| `hash_get_index`, `hash_bit_value`, `macb_sethashtable` | 2965-3008 | Multicast hash |
| `macb_mdio_wait_for_idle`, `macb_mdio_read_c22`, `macb_mdio_write_c22` | 325-441 | MDIO (strip `pm_runtime_*`) |

### macb_main.c — Tier 2: Algorithmic gold, rewrite around kernel types (~235 lines)

The descriptor-handling algorithm is what we want. Replace kernel types with our own:

| Function | Lines | What to preserve | What to replace |
|----------|-------|------------------|-----------------|
| `gem_rx` | 1458-1628 | Descriptor polling, USED-bit check, `rmb()`/`dma_rmb()` placement (~35 lines) | `sk_buff` alloc → our buffer pool |
| `gem_rx_refill` | 1374-1435 | Descriptor filling, barrier order (~20 lines) | `netdev_alloc_skb` → `HugepageBuffer` |
| `macb_tx_map` | 2103-2275 | TX descriptor build in reverse, `wmb()/dma_wmb()` (~80 lines, **load-bearing on ARM64**) | skb fragment walking |
| `macb_start_xmit` | 2381-2484 | TSTART write (line 2474), ring slot check (~30 lines) | netif queueing |
| `macb_tx_complete` | 1296-1372 | TX_USED polling, tx_tail advance (~40 lines) | sk_buff stats/release; drop spin_lock |
| `macb_alloc_consistent` | 2626-2686 | Ring assignment arithmetic (~30 lines) | `dma_alloc_coherent` → `HugepageBuffer` |

### macb_main.c — Tier 3: Skip entirely (~4800 lines, 80%)

- All interrupt handlers (`macb_interrupt`, `gem_wol_interrupt`, `macb_wol_interrupt`,
  `macb_poll_controller`) — 195 lines
- NAPI poll (`macb_rx_poll`, `macb_tx_poll`) — ~50 lines
- Stats collection, ethtool strings — ~300 lines
- ethtool ops (regs dump, ring settings, channel count, WoL, coalescing) — ~500 lines
- Suspend/resume/runtime_pm — ~200 lines
- Phylink state machine integration — ~400 lines
- `macb_open`/`macb_close` (networking-stack lifecycle) — ~100 lines
- `macb_tx_error_task` (kernel workqueue error recovery) — 120 lines
- VLAN, `macb_pad_and_fcs`, `macb_clear_csum` — ~100 lines
- XDP/AF_XDP hooks, SRIOV/VF, netpoll
- `macb_configure_caps` device-tree probing (we hard-code for Zynq UltraScale+)

### Critical memory barriers — NEVER remove on ARM64

ARM64 is NOT DMA-coherent. These barriers in the kernel source are load-bearing:

```c
// Before hardware reads descriptors we wrote:
dma_wmb()   // macb_set_addr:1119, gem_rx_refill:1417, macb_tx_map:2257

// Before we read descriptors hardware wrote:
dma_rmb()   // gem_rx:1477, macb_tx_complete:1317

// Between writing address and ctrl (LAST step before HW sees the frame):
wmb()       // macb_tx_map:2243
```

C++ equivalents:
- `dma_wmb` ≈ `std::atomic_thread_fence(std::memory_order_release)` or `asm volatile("dmb oshst")`
- `dma_rmb` ≈ `std::atomic_thread_fence(std::memory_order_acquire)` or `asm volatile("dmb oshld")`
- `wmb` ≈ `asm volatile("dsb sy")` for strict ordering

### macb_ptp.c — TSU/PTP copy (~70 lines, future work)

Not needed for initial bring-up. Useful when we want hardware timestamps:

| Function | Lines | Action |
|----------|-------|--------|
| `gem_hw_timestamp` | 246-272 | Copy — extract 64-bit TS from descriptor PTP extension |
| `gem_tsu_set_time` | 77-100 | Copy — TN → TSL → TSH register order is critical |
| `gem_tsu_get_time` | 41-75 | Copy — rollover detection logic |
| `gem_tsu_incr_set` | 102-120 | Copy — clock rate programming |
| `gem_ptp_init_timer` | 205-218 | Copy — increment arithmetic from rate |
| `macb_ptp_desc` | 28-39 | Copy — PTP extension pointer math |
| `gem_ptp_set_ts_mode` | 367-375 | Copy — enable HW timestamping in TXBDCTRL/RXBDCTRL |

Skip all `ptp_clock_info` callbacks (Linux PTP subsystem integration).

### Top 10 functions to port first (by value)

1. `macb_reset_hw` (2746-2776, 30 lines) — clean reset, 100% copy
2. `macb_init_hw` (2897-2930, 33 lines) — full init, 100% copy
3. `macb_configure_dma` (2851-2895, 44 lines) — DMA config, 100% copy
4. `macb_set_addr` + `macb_get_addr` (1107-1142, 33 lines) — descriptor addr, **dma_wmb() critical**
5. `macb_tx_complete` (1296-1372, 76 lines) — TX polling, drop spin_lock, ~50 lines preserved
6. `gem_rx` (1458-1628, 170 lines) — RX polling, ~35 lines preserved descriptor logic
7. `macb_tx_map` (2103-2275, 172 lines) — TX descriptor fill, **barriers critical**, ~80 lines preserved
8. `macb_init_rings` + `gem_init_rings` (2701-2744, 41 lines) — ring init, mostly copy
9. `macb_init_buffers` (489-512, 23 lines) — ring base programming, 100% copy
10. `gem_prog_cmp_regs` + `gem_enable_flow_filters` (3758-3844 + 3712-3756, 130 lines) — **screener setup, core to our routing**

### Recommended porting order (7-day estimate)

| Day | Phase | Functions |
|-----|-------|-----------|
| 1 | Cold path | `macb_reset_hw`, `macb_init_hw`, `macb_configure_dma`, `macb_init_buffers` |
| 2 | Descriptors | `macb_set_addr`/`get_addr`, ring init, `HugepageBuffer` allocation |
| 3 | PHY/MDIO | `macb_mdio_read_c22`/`write_c22` (strip PM), LAN8841 init |
| 4-5 | TX hot path | `macb_tx_map` rewrite, `macb_start_xmit`, `macb_tx_complete` |
| 6-7 | RX hot path | `gem_rx` rewrite, `gem_rx_refill` with our buffer pool |
| 8 | Screeners & multi-queue | `gem_prog_cmp_regs`, `gem_enable_flow_filters`, Q1 hot path, Q0 TAP |
| 9-10 | Test + tune | Latency benchmark, barrier audit |

### Summary numbers

| File | Total lines | Copy verbatim | Rewrite preserving logic | Skip |
|------|-------------|---------------|--------------------------|------|
| macb_pci.c | 133 | 0 | 0 | 133 |
| macb_ptp.c | 466 | ~70 | 0 | ~400 |
| macb_main.c | 6040 | ~700 | ~235 | ~5100 |
| **Total** | **6639** | **~770** | **~235** | **~5633** |

~15% of the Linux code transfers by line count. Algorithmic value is higher: ~60-70% of
critical-path logic (descriptor handling, init sequence, memory barrier placement) is
preserved. The other 85% of lines is kernel plumbing we replace with our C++ infrastructure
(AXIBackend, HugepageBuffer, TapBridge, direct polling).

Expected final PMD size: 1500-2000 lines (1000-1200 ported from macb + 500-800 new C++).

## PHY MDIO Address — 31 (0x1f), NOT 0

Investigation date: 2026-04-20

The device tree declares the PHY at MDIO address 31:

```
/sys/firmware/devicetree/base/axi/ethernet@ff0b0000/mdio/phy@1f/reg = 0x0000001f
```

Kernel dmesg confirms:
```
macb ff0b0000.ethernet end0: PHY [ff0b0000.ethernet-ffffffff:1f] driver [Microchip LAN8841 Gigabit PHY] (irq=POLL)
```

The only device on the MDIO bus:
```
/sys/bus/mdio_bus/devices/ff0b0000.ethernet-ffffffff:1f
```

Our PMD's MDIO scan (addresses 0→31) finds a valid PHY ID (0x00221652) at address 0
**and** at address 31. The PHY may respond on address 0 as a broadcast/alias, but the
canonical address is 31. Writes to address 0 may silently fail (reads work because the
PHY echoes its registers, but the write target may not latch).

**Impact:** If `initPhy()` picks address 0, BMCR writes (power-down clear, isolate clear)
and `isLinkUp()` status reads may be hitting the wrong address. The fix is to scan in
reverse (31→0) or hardcode 31 for this platform.

## CRL_APB Clock Gating After macb Unbind

Investigation date: 2026-04-20

When macb is unbound, `macb_remove()` → `clk_disable_unprepare()` drops the clock refcount
to zero. The Linux clock framework then gates the GEM reference clocks via CRL_APB:

| Register | Address | Before unbind | After unbind | Bits cleared |
|----------|---------|---------------|--------------|--------------|
| GEM0_REF_CTRL | 0xFF5E0050 | 0x06010C00 | 0x00010C00 | 25 (CLKACT0), 26 (CLKACT1) |
| GEM_TSU_REF | 0xFF5E0100 | 0x01010600 | 0x00010600 | 24 (CLKACT) |

**Symptoms:** MAC APB registers still read/write (APB bus always on), MDIO works (separate
clock), TXGO latches at 1 but TXCNT stays at 0 forever. No error flags. No traffic on wire.

**Fix:** After unbind, mmap CRL_APB at 0xFF5E0000 and OR-set the CLKACT bits back on.
Implemented in `Cadence_GEM::restoreGemClocks()`.

This also fixes the rebind hang — `macb_probe()` → phylink init was stalling because the
PHY/MAC had no reference clock.

## Network and NFS Recovery After Rebind

Investigation date: 2026-04-20

### Boot-time service order (from SSH investigation of healthy mkdev50)

1. **macb_probe** creates `eth0`, renamed to `end0` by udev (~7.7s into boot)
2. **systemd-networkd** starts (PID 736), applies `/etc/systemd/network/fec.network`:
   - `Match: Name=en*`, `Type=ether`
   - `DHCP=ipv4`, `ConfigureWithoutCarrier=yes`
3. **PHY link-up** at ~14s (Microchip LAN8841, 1Gbps/Full)
4. **DHCP** assigns 10.11.33.71 (via 137.138.17.9)
5. **fec-setup-filesystem.service** runs `/usr/sbin/fec-filesystem-mangling`:
   - Mounts `/usr/local` via NFSv3: `mount -overs=3,noatime,nodev,ro cs-ccr-felab:/data/dsc/lab/debian/12/aarch64 /usr/local`
   - Creates overlay at `/usr/local/lib/modules/$(uname -r)` with upperdir in `/run/fec/overlayfs/`
   - Appends autofs entries to `/etc/fstab` (these are NOT static — generated at boot)
   - Runs `systemctl daemon-reload && systemctl restart remote-fs.target`
6. **Autofs NFS mounts** come up: `/nfs/cs-ccr-nfshome`, `/nfs/cs-ccr-felab`, etc.

### What breaks after unbind/rebind

| What | Why it breaks | How to fix |
|------|---------------|------------|
| `end0` disappears | macb_remove destroys netdev | macb_probe recreates it on rebind |
| DHCP/IPv4 gone | No interface, no address | **Manual IP assignment** (see below) — networkd is dead |
| `/usr/local` hangs | Hard NFS mount retries forever (timeo=600 = 60s) | Auto-recovers within 1–60s once IP path is restored |
| `/usr/local` "device busy" on unmount | Overlay at `/usr/local/lib/modules/$(uname -r)` holds it | Stop overlay first: `systemctl stop usr-local-lib-modules-*.mount` |
| Autofs mounts dead | NFS client state stale | `systemctl restart remote-fs.target` after IP is back |
| `systemctl` commands hang | If run under SCHED_FIFO (from PMD via fork), starves systemd/D-Bus | Drop to SCHED_OTHER before forking |
| `systemd-networkd` | Threads stuck on NFS after losing the interface — reload/reconfigure/restart all fail | **Do NOT rely on networkd.** Stop it before unbind, use manual `ip` commands after rebind |
| `ip link show` hangs | Netlink needs RTNL mutex, held by kernel during deferred NFS teardown | Use sysfs (`[ -d /sys/class/net/end0 ]`) between unbind and rebind |
| collectd | Tries to stat NFS paths | Stop BEFORE unbind while NFS is still healthy |
| Dynamic linker hang | `ld.so.cache` entries under `/usr/local/lib` cause library loads to block | Set `LD_LIBRARY_PATH=/lib/aarch64-linux-gnu:/lib:/usr/lib/aarch64-linux-gnu:/usr/lib` |

### Recovery approaches tried and failed

1. **`networkctl reconfigure end0`** — returns OK but `Network File: n/a` (networkd doesn't
   re-match fec.network to the new ifindex after rebind)
2. **`networkctl reload` + `reconfigure`** — both fail silently when networkd is hung
3. **`systemctl restart systemd-networkd`** — hangs (daemon has threads stuck on dead NFS)
4. **`ip link show`** — blocks on RTNL mutex during NFS teardown (caused full device hang
   on first attempt before switching to sysfs checks)

### Working recovery sequence (verified 2026-04-20)

**Key insight: bypass networkd, use manual IP assignment.** Total recovery time: ~20 seconds.

**Before unbind** (while everything works):
```bash
# 1. Capture network config
SAVED_ADDR=$(ip -4 -o addr show end0 | grep -oP 'inet \K[\d./]+')  # e.g. 10.11.33.71/24
SAVED_GW=$(ip -4 route show default dev end0 | grep -oP 'via \K[\d.]+')  # e.g. 10.11.33.1

# 2. Stop services that will hang on dead NFS
systemctl stop collectd
systemctl stop systemd-networkd   # CRITICAL: stop while it can still cleanly shutdown

# 3. Pre-warm binaries (loads shared libs into page cache)
ip link show end0 > /dev/null 2>&1 || true
# ... etc for all binaries needed after unbind
```

**After rebind:**
```bash
export PATH=/usr/sbin:/usr/bin:/sbin:/bin
export LD_LIBRARY_PATH=/lib/aarch64-linux-gnu:/lib:/usr/lib/aarch64-linux-gnu:/usr/lib

# 4. Wait for interface via sysfs (NOT netlink)
# [ -d /sys/class/net/end0 ]   — pure VFS stat, no RTNL mutex

# 5. Manual network config (bypass networkd entirely)
ip link set end0 up
ip addr add $SAVED_ADDR dev end0
ip route add default via $SAVED_GW dev end0

# 6. Wait for PHY link-up (~5s for LAN8841 autoneg)
# cat /sys/class/net/end0/carrier  — poll until "1"

# 7. Verify: ping $SAVED_GW

# 8. NFS auto-recovers (hard mount retries on its own)
# /usr/local recovered after just 1s in testing

# 9. Restart autofs NFS mounts
systemctl restart remote-fs.target

# 10. Restart networkd last (for long-term DHCP renewal)
systemctl restart systemd-networkd   # may timeout — non-critical, IP works
```

### Timing from successful test run

| Step | Time | Duration |
|------|------|----------|
| Unbind | 20:57:46.928 | instant |
| Rebind | 20:57:51.095 | instant (3s pause between) |
| end0 appears | 20:57:52 | 1s after rebind |
| IP assigned | 20:57:51.238 | instant (manual) |
| PHY link-up | 20:57:56 | 5s (autoneg) |
| Gateway ping | 20:57:56.578 | instant |
| /usr/local NFS | 20:57:56.578 | 1s (auto-recover) |
| remote-fs.target | 20:57:57 | ~1s |
| All NFS OK | 20:58:07 | ~20s total |

### Key insight: NFS hard mount auto-recovers fast

The `/usr/local` mount is `hard,proto=tcp,timeo=600,retrans=2`. In practice, once the IP
path is restored, NFS reconnects within 1–2 seconds (not 60s). The 60s `timeo` is the
*maximum* retry interval — the kernel NFS client retries much sooner when the TCP SYN
succeeds immediately. Previous failures were caused by never reaching this point because
networkd was hung and no IP was assigned.

### For the C++ PMD destructor

The `restoreNetworkAndNfs()` method should follow the same pattern:
1. Save IP/gateway in `init()` (before unbind) using `getifaddrs()` (no fork needed)
2. Stop networkd before unbind (while it's healthy): `system("systemctl stop systemd-networkd")`
3. After rebind: use raw syscalls via netlink `AF_NETLINK/NETLINK_ROUTE` to set IP and route
   (avoids forking `ip` which may hang on RTNL), or fork `ip` with `timeout`
4. Poll carrier via `/sys/class/net/end0/carrier` (no fork)
5. NFS auto-recovers; `system("systemctl restart remote-fs.target")` for autofs mounts

## Coherent DMA Descriptor Rings — the cache-line writeback race

Investigation date: 2026-04-30

### Symptom

At line rate (~1.488 Mpps for 64-byte L2 frames on 1 GbE), bursts of 200–1000
packets showed catastrophic loss in *both* directions, while the silicon reported
clean operation:

| Direction | HW counter | SW saw | Loss |
|-----------|-----------|--------|------|
| GEM as RxSink, 200 sent at gap=0 | 200 unicasts received, `resErr=0` | `Accepted=9`, `rx_tail=9` | 95.5 % |
| GEM as RxSink, 400 sent at gap=0 | 400 received, `resErr=0` | `Accepted=13`, `rx_tail=13` | 96.7 % |
| GEM as TxGen, 1000 sent at gap=0 | `TX=448`, all received by I210 RxSink | `Sent=447, Failed=553`, `tx_inflight=256` | 55 % "ring full" |

End-state pattern was always the same shape: SW's tail/inflight tracker stuck mid-burst,
DMA registers (RBQP / TBQP) pointing past it, the descriptor immediately under SW's tail
read as `USED=0`, the next descriptor `USED=1`, and the rest of the ring untouched.

### Root cause

The Linux macb driver allocates descriptor rings with `dma_alloc_coherent`, which on
ARM with non-coherent DMA returns Normal Non-Cacheable memory. Our PMD originally
backed the rings with cached hugepages and bracketed every descriptor access with
`dc cvac` / `dc civac` + `dsb sy`.

```
GEMDescriptor64  =  16 bytes
ARM cache line   =  64 bytes
                ↓
4 descriptors share each cache line.
```

The race plays out over ~hundreds of nanoseconds during a line-rate burst:

```
T1: HW DMA writes desc[N]   USED=1 to DRAM
T2: SW tryReceive(N) → reads USED=1, processes packet
T3: SW release(N) modifies desc[N] fields. The CPU does
    READ-FOR-OWNERSHIP of cache line covering descs[N..N+3].
    Snapshot at T3:  desc[N]   USED=1 (HW just wrote)
                     desc[N+1] USED=0 (HW hasn't written yet)
                     desc[N+2] USED=0
                     desc[N+3] USED=0
T4: HW writes desc[N+1] USED=1 to DRAM.       ← key moment
T5: SW modifies desc[N] in cache → USED=0.
T6: dc cvac writes the entire 64-byte line back to DRAM,
    INCLUDING desc[N+1]'s stale USED=0  ← STOMPS HW's T4 write.
T7: HW writes desc[N+2] USED=1 (after our writeback — survives).
T8: HW writes desc[N+3] USED=1 (survives).
…
T∞: SW polls desc[N+1], sees USED=0 forever, m_rxTail frozen.
```

The TX direction has the mirror image: SW's `txAcquire` modifies desc[N], the read-for-ownership picks
up a stale `TX_USED=0` for desc[N+1] that HW has just transitioned to `TX_USED=1`, the writeback stomps
it, `txReclaim` can never advance past it, the SW thinks the ring is full, and `Failed` shoots up.

### Diagnostic that confirmed it

A two-stage diagnostic was added during the investigation and **kept in the repository**
even though the bug is fixed — it is generally useful and zero-overhead unless invoked:

1. `dumpRxDescriptorCacheCoherency<Q>()` — at end-of-test, reads each descriptor twice:
   once via the cached PMD view (with the same `dc civac + dsb` the hot path performs)
   and once via a fresh `mmap("/dev/mem", O_SYNC)` of the same physical address (uncached
   straight-from-DRAM read). Reports any disagreement.

2. `RxWatcher` — a side-thread that, while the test runs, polls the first 32 descriptors
   through the uncached `/dev/mem` mapping and records every `RX_USED`-bit transition with
   a `CLOCK_MONOTONIC` timestamp. The "smoking gun" is a `1 → 0` transition on a descriptor
   the SW never released.

The first run of the cached-vs-uncached diagnostic showed *agreement* between the two
views, which is misleading — the cache and DRAM agreed because the writeback had already
corrupted DRAM in the past. The end-state pattern (USED=0 sandwiched between processed and
unfilled USED=1 ranges, in different positions on every run) is itself the smoking-gun
signature of partial-cache-line writeback. The watcher trace was muddled by long L2-writeback
visibility delays on the uncached observer, but corroborated the pattern.

### Fix — `gem_uio` kernel module + coherent DMA pool

A new kernel module `src/Cadence_GEM/gem_uio/` does what the kernel macb driver does
internally: `dma_alloc_coherent()` for the descriptor region. The module exposes the
buffer through UIO, so userspace can `mmap` it directly:

```
mem[0]  GEM register window           (UIO_MEM_PHYS, kernel maps non-cached)
mem[1]  64 KiB coherent DMA pool      (UIO_MEM_PHYS over dma_alloc_coherent
                                       — guaranteed non-cached on this platform)
```

64 KiB easily fits 4 rings × 256 descriptors × 16 B = 16 KiB, with headroom.

The module also holds the GEM clocks alive via the kernel CCF (`clk_prepare_enable`).
That's a side benefit — the firmware's clock/power-domain state stays consistent
during the PMD run, which lets us drop the old direct-`CRL_APB`-MMIO-poke that was
in `restoreGemClocks()` (and the corresponding gate-off poke in `resetForRebind()`).

### Userspace integration

```
+------------------------------------------------------+
| init() flow                                          |
|                                                      |
|   unbind macb                                        |
|   bindGemUio()                  ← new                |
|     ↓ writes driver_override = "gem_uio"             |
|     ↓ writes gem_uio/bind                            |
|   m_descPool.open(deviceName)   ← new                |
|     ↓ scans /sys/.../uio/uio<N>                      |
|     ↓ reads /sys/class/uio/uio<N>/maps/map1/{addr,size} |
|     ↓ mmap /dev/uio<N> at offset = page_size         |
|   initRings() partitions pool: RX[0] RX[1] TX[0] TX[1] |
|   …                                                  |
|                                                      |
| destructor flow (mirrors)                            |
|   m_descPool.close()                                 |
|   unbindGemUio()                ← new                |
|   modprobe wrapper + bind macb  (existing)           |
+------------------------------------------------------+
```

Two new userspace types:

- **`CoherentDmaPool`** — RAII wrapper around `/dev/uio<N>`. Locates the UIO instance
  for our platform device, mmaps `mem[1]`, exposes `virtAt(off)` / `physAt(off)`.
- **`DescRingSlice`** — POD `{ GEMDescriptor64* virt; uint64_t phys; }`. Each
  ring is a slice of the pool. Same API surface as the prior `DMARing<>` so the
  rest of the PMD didn't change.

### Cache maintenance — what stays, what goes

| Boundary                                          | Before          | After          |
|---------------------------------------------------|-----------------|----------------|
| SW writes descriptor field                        | `dc cvac`       | (none)         |
| SW reads descriptor field                         | `dc civac`      | (none)         |
| Between USED-bit probe and ctrl/len read          | (relied on `dc civac`) | `dmb ishld` ✱ |
| SW writes descriptor → kicks `TSTART` MMIO        | `dsb sy`        | `dsb sy` (kept) |
| SW init descriptors → HW enabled                  | `dsb sy`        | `dsb sy` (kept) |
| SW writes packet payload (cached) → HW reads      | `dc cvac` range + `dsb sy` | unchanged — payload is still cached |
| HW writes packet payload (cached) → SW reads      | `dc civac` range + `dsb sy` | unchanged |

✱ ARM is weakly ordered. The branch on `addr & RX_USED` is a control dependency, which
on AArch64 is *not* sufficient to keep a subsequent load of `ctrl` from being speculatively
executed before the addr load resolves. A `dmb ishld` (load-load barrier in inner-shareable
domain, ~few cycles) closes that window.

Cache maintenance ops on **packet buffers** stay because the buffers are still backed by
cached hugepages — locality on payload bytes is real; spending those cycles is worth it.
Only the descriptor *rings* moved to coherent memory.

### Result

End-to-end at line-rate burst, 1000 packets, `send_interval_us = 0`:

| Direction                              | Sent | Delivered | Notes |
|----------------------------------------|------|-----------|-------|
| GEM TxGen → Intel I210 RxSink          | 1000 | 1000      | `Failed: 0`, HW `TX=1000` |
| Intel I210 TxGen → GEM RxSink          | 1000 | 1000      | `Accepted: 1000, Rejected: 0`, HW `RX=1257` (incl. 257 broadcasts) |

Cache stomp — gone in both directions.

### Performance footnote — does uncached cost us latency?

Negligibly, possibly imperceptibly, for our access pattern:

- Every cached descriptor access on the old path was *already* a DRAM round-trip,
  because we explicitly invalidated before every read and flushed after every write.
  The cache never amortised anything for descriptors — we paid full DRAM latency *plus*
  the cache-maintenance overhead.
- Uncached descriptor accesses pay the same DRAM latency (~80–100 ns per access on
  Cortex-A53) without the cache ops or barriers.
- Net difference: a few tens of ns per packet, swamped by 670 ns inter-frame time at
  line rate. Theoretically faster, indistinguishable in practice.

This is also why the kernel macb driver does it this way — `dma_alloc_coherent` for
descriptors, regular cached pages for skb payload.

## Building gem_uio.ko against a kernel whose source we don't have

Investigation date: 2026-04-30

The FECOS kernel running on `cfd-865-mkdev50` is built from a private git tree at a
specific commit:

```
$ uname -r
6.6.40-xlnxLTS20242-fecos03-1-g8f9a3dc
                          └─┬──────┘
                            git short-hash, 1 commit ahead of the fecos03 tag
```

The available cross-compile headers (`/acc/sys/cdk/linux-headers/`) only carry the
base `fecos03` headers — not the `-1-g8f9a3dc` topspot. So a module built normally
gets a `vermagic` of `6.6.40-xlnxLTS20242-fecos03 SMP …` and `insmod` rejects it
with `Invalid module format`. `CONFIG_MODULE_FORCE_LOAD` is not set on this kernel,
so `insmod -f` doesn't help. We don't have permission to rebuild the kernel.

### Constraints

| Path | Status |
|------|--------|
| `/lib/modules/$(uname -r)/build` | does **not exist** on the device |
| Modules tree at `/usr/local/lib/modules/<ver>/` | exists, but `modules.dep` etc. are empty (this kernel ships with everything built-in) |
| Native build | impossible — no kernel build tree on the box |
| Kernel source for `-1-g8f9a3dc` | not available to us |
| `CONFIG_MODULE_FORCE_LOAD` | not set → `insmod -f` rejected |

### What works — patch vermagic in the built `.ko`

`gem_uio` only references stable kernel APIs (`clk_*`, `devm_*`, `dma_alloc_coherent`,
`platform_driver_*`, `uio_*`, `_printk`). None of them is touched by a typical 1-commit
delta on a maintenance branch. Cross-compile against the closest available headers,
then **rewrite the vermagic string** in the `.modinfo` ELF section to match the running
kernel exactly:

```bash
# 1. Cross-compile against fecos03 headers
cd src/Cadence_GEM/gem_uio
make KDIR=/acc/sys/cdk/linux-headers/6.6.40-xlnxLTS20242-fecos03-aarch64 \
     ARCH=arm64 \
     CROSS_COMPILE=/acc/sys/cdk/debian/12/aarch64/sysroots/host/usr/bin/aarch64-linux-gnu-

# 2. Extract, patch, write back the .modinfo section
CROSS=/acc/sys/cdk/debian/12/aarch64/sysroots/host/usr/bin/aarch64-linux-gnu-
$CROSS objcopy -O binary --only-section=.modinfo gem_uio.ko /tmp/modinfo.bin
python3 - <<'PY'
data = open('/tmp/modinfo.bin','rb').read()
old  = b'6.6.40-xlnxLTS20242-fecos03 SMP'
new  = b'6.6.40-xlnxLTS20242-fecos03-1-g8f9a3dc SMP'
assert old in data
open('/tmp/modinfo.bin','wb').write(data.replace(old, new, 1))
PY
$CROSS objcopy --update-section .modinfo=/tmp/modinfo.bin gem_uio.ko

# 3. Verify
modinfo gem_uio.ko | grep vermagic
# vermagic: 6.6.40-xlnxLTS20242-fecos03-1-g8f9a3dc SMP mod_unload modversions aarch64
```

The new vermagic is longer than the old one. `objcopy --update-section` rewrites the
section size in the ELF header automatically — no manual ELF surgery needed.

### Sanity-check the symbol set before deploying

```bash
$ aarch64-linux-gnu-nm gem_uio.ko | grep ' U '
                 U clk_disable
                 U clk_enable
                 U clk_prepare
                 U clk_unprepare
                 U devm_clk_get
                 U devm_clk_get_optional
                 U devm_kmalloc
                 U devm_platform_get_and_ioremap_resource
                 U dma_alloc_attrs
                 U dma_free_attrs
                 U dma_set_coherent_mask
                 U dma_set_mask
                 U __platform_driver_register
                 U platform_driver_unregister
                 U _printk
                 U __stack_chk_fail
                 U __uio_register_device
                 U uio_unregister_device
```

All EXPORT_SYMBOL_GPL/EXPORT_SYMBOL APIs that have been stable since at least 5.x.
If a future module references something more volatile (e.g. `phylink_*`,
`netdev_*`, anything from `drivers/net/phy/`), this technique gets riskier — those
APIs change between kernel patch versions.

### When this technique fails

If the running kernel was built with `CONFIG_MODVERSIONS=y` AND any of the symbols
you import had its CRC change between the headers' tag and the running kernel,
`insmod` rejects the module per-symbol with `disagrees about version of symbol …`.
Hit during the very first attempt with an early version that used `dev_info`,
`dev_err`, `dev_err_probe` — fixed by switching those to `pr_info` / `pr_err`,
which use `_printk` (CRC didn't change).

### Operational note for teammates

`insmod` once per boot and the module persists. There is no `modprobe` hook —
binding only happens when the PMD invokes `bindGemUio()` at runtime via
`driver_override`. Loading the module by itself doesn't disturb macb.

```bash
sudo insmod /dev/shm/gem_uio.ko
lsmod | grep gem_uio                           # should show "gem_uio  12288  0"
ls /sys/bus/platform/drivers/gem_uio/          # should show bind/unbind/uevent/module
```

## Cleanup TODO

Tracked items, in approximate priority order. None of these affect correctness of
the data path — they're polish for the public release.

### 1. Suppress broadcast `resErr` saturation on RX

Symptom: `resErr=262143` (0x3FFFF, 18-bit counter saturated) at end of every
RxSink test, even though the data path delivers 100 % of the unicasts we care
about.

Root cause: the GEM has 5 hardware queues (`DCFG6` confirms this). We program
descriptor rings only for Q0 and Q1. Network broadcasts that the EtherType
screener doesn't classify get routed by a default policy to one of the unused
queues (Q2/Q3/Q4), which has no `RBQP` programmed → BNA → `resErr++`.

Three options, pick one:

1. Set `NCFGR.NBC=1` to drop broadcasts at the SA filter (we don't need them
   for our 0x88B5 traffic — kernel sees broadcasts via the TAP bridge anyway,
   *if* it's enabled).
2. Allocate empty descriptor rings for Q2–Q4 so broadcasts have somewhere to
   land and get ignored cleanly.
3. Configure a fall-through screener entry that routes everything not matching
   our EtherType to Q0.

Option 1 is the simplest. Option 3 matches what the kernel macb driver does.

### 2. Fix the trailing `tx_inflight=256` cosmetic

Symptom: at end of TxGen, even when every packet was actually transmitted,
`tx_inflight` shows 256 because no `txReclaim` is called during the 10 ms
drain sleep.

Fix: in `run_txgen` after the loop, before the sleep, call `txReclaim` once
(or repeatedly until `inflight == 0`). One-line change.

### 3. Initialise `m_rxBuffers`/`m_txBuffers` only for queues we actually use

Currently `initRings` allocates `NumRxDesc * BuffSize = 512 KiB` per queue
and `NumTxDesc * BuffSize = 512 KiB` per queue, for both Q0 and Q1. The
slow-path Q0 only carries TAP traffic — could use a smaller ring (64 entries)
to save ~3.5 MiB of hugepages.

Low value; defer until hugepages become tight.

### 4. README.md and one-page architecture diagram

For the GitHub release. Should cover:
- What this PMD is and isn't (low-latency timing, not a NIC replacement)
- Hardware support matrix (Intel I210, Cadence GEM)
- Build, deploy, run instructions
- The `gem_uio.ko` requirement on Zynq UltraScale+
- Performance envelope (e.g. "100 % delivery up to ~200 K pps measured;
  256-descriptor ring depth caps line-rate burst at ~256 packets")
- Contributing guide

### 5. Optional: silence `RxWatcher` and the cached-vs-uncached dump unless asked

Right now the cache-coherency diagnostic and the watcher fire on every run.
Now that the bug is fixed and unlikely to return, gate them behind an env var
(`ABTRDA3_GEM_DESC_DEBUG=1`) so normal runs skip them and have less log spam.
The watcher already supports this idea — just need to also gate the
`dumpRxDescriptorCacheCoherency` call.

(Strictly speaking these have already been removed from the live build —
keep this entry only if you want to preserve them as opt-in diagnostics
in some branch.)

### 6. Verify the threaded-rebind diagnostic still reports correctly

The hung-probe `/proc/<tid>/stack` capture in `rebindKernelDriver` was
load-bearing during development. It should still work — but worth a smoke
test on a kernel where the modprobe wrapper *isn't* applied, just to confirm
the safety net is intact for future regressions.

### 7. Document the per-queue rings on a schematic

For the README. The four-slice partition of `gem_uio` `mem[1]` is the heart
of the architecture and not obvious from code alone:

```
gem_uio mem[1]: 64 KiB coherent DMA at paddr=0x5ff00000
+------------------+
| RX ring Q0       |  256 desc × 16 B = 4 KiB    @ 0x5ff00000
+------------------+
| RX ring Q1       |  256 desc × 16 B = 4 KiB    @ 0x5ff01000
+------------------+
| TX ring Q0       |  256 desc × 16 B = 4 KiB    @ 0x5ff02000
+------------------+
| TX ring Q1       |  256 desc × 16 B = 4 KiB    @ 0x5ff03000
+------------------+
|  unused (48 KiB) |
+------------------+
```

## Reference Documentation

- Xilinx UG1085: Zynq UltraScale+ TRM, Chapter 16 (GEM)
- Cadence GEM Technical Reference Manual
- Linux kernel (cloned locally): `/nfs/cs-ccr-nfshome/user/asherjil/ABTTiming/linux-xlnx/drivers/net/ethernet/cadence/`
  - `macb.h` — register definitions (source of `GEMRegisters.hpp`)
  - `macb_main.c` — driver logic reference
  - `macb_ptp.c` — TSU/PTP hardware timestamping
  - `macb_pci.c` — PCIe wrapper (not applicable to us)
- DPDK has NO official macb driver. The Phytium patch v1 (Oct 2024, rejected due to build
  errors and style issues) contained register-level work that may be useful for cross-reference
- Existing infrastructure in `/user/asherjil/ABTTiming/ABTEdge/src/backends/`:
  - `AXIBackend.hpp` — AXI register access (mmap at 0xff0b0000)
  - `InterfaceDiscovery.hpp` — driver unbind/rebind via sysfs
  - `HugepageBuffer.hpp` — replaces `dma_alloc_coherent`
  - `DMARing.hpp` — ring management helpers
