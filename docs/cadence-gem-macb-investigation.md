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

## Reference Documentation

- Xilinx UG1085: Zynq UltraScale+ TRM, Chapter 16 (GEM)
- Cadence GEM Technical Reference Manual
- Linux kernel: `drivers/net/ethernet/cadence/macb_main.c`, `macb.h`
- DPDK has NO macb driver — Linux kernel source is the primary reference
