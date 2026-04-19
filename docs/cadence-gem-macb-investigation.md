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
| PHY | Microchip LAN8841, RGMII-ID mode (internal delay TX+RX) |
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
