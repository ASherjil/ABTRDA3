# ABTRDA3 — Current State & Next Steps

## Overview

ABTRDA3 is an ultra-low-latency timing library using `packet_mmap` (TPACKET_V2) and `AF_XDP` for raw Ethernet TX/RX. The XDP eBPF filter is safety-critical — target machines boot from NFS over the same NIC, so the filter only redirects EtherType 0x88B5 to userspace and passes everything else to the kernel stack.

## Target Hardware

### x86_64 — cfc-865-mkdev16 / mkdev30

| Property | eno1 (NFS boot) | eno2 (dedicated LAN) |
|----------|-----------------|---------------------|
| Driver | e1000e (Intel I219) | igb (Intel I350) |
| Queues | 1 RX, 1 TX | 4 RX, 4 TX |
| XDP mode | Generic/SKB only | Generic/SKB only (igb native broken on 5.14-rt) |
| Zero-copy AF_XDP | No (`ndo_xsk_wakeup` not implemented) | No |
| ntuple filters | Not supported (`off [fixed]`) | Supported but not needed (dedicated) |
| RSS | Not supported (single queue) | Supported (we steer all to queue 0) |
| Kernel | 5.14.0-570.52.1.el9_6.x86_64 PREEMPT_RT | Same |

### ARM64 — cfd-865-mkdev50

| Property | end0 (NFS boot) |
|----------|----------------|
| Driver | macb (Cadence/Xilinx GEM on Zynq UltraScale+) |
| Queues | **2 RX, 2 TX** |
| XDP mode | **Native/DRV supported** (macb implements `ndo_bpf`) |
| Zero-copy AF_XDP | No (`ndo_xsk_wakeup` not implemented) |
| ntuple filters | **Supported** (`off` but can be enabled) |
| RSS | Not supported (`receive-hashing: off [fixed]`) |
| Kernel | 6.6.40-xlnxLTS20242 |
| Cores | 4 (cores 0-3) |
| NIC IRQ | IRQ 51, primarily on core 0 |

#### ARM64 ntuple flow steering — verified working

```bash
# Enable ntuple filters
$ sudo ethtool -K end0 ntuple on

# Steer ethertype 0x88B5 to RX queue 1
$ sudo ethtool -N end0 flow-type ether proto 0x88b5 action 1
Added rule with ID 0

# Verify
$ ethtool -n end0
2 RX rings available
Total 1 rules

Filter: 0
    Flow Type: Raw Ethernet
    Src MAC addr: 00:00:00:00:00:00 mask: FF:FF:FF:FF:FF:FF
    Dest MAC addr: 00:00:00:00:00:00 mask: FF:FF:FF:FF:FF:FF
    Ethertype: 0x88B5 mask: 0x0
    Action: Direct to queue 1
```

#### ARM64 driver/queue verification

```bash
$ ethtool -i end0
driver: macb
version: 6.6.40-xlnxLTS20242-fecos03-1-g
bus-info: ff0b0000.ethernet

$ ls /sys/class/net/end0/queues/
rx-0  rx-1  tx-0  tx-1

$ cat /proc/interrupts | grep end0
 51:    3487283          0          0      81145     GICv2  89 Level     end0, end0
```

## Benchmark Results

### x86_64 — eno2 direct LAN (mkdev30 → mkdev16), 50M packets, 1ms interval

| Metric | packet_mmap | AF_XDP (SKB/copy) |
|--------|------------|-------------------|
| Min RTT | 43.31 us | 43.76 us |
| Median | 79.40 us | 79.70 us |
| P99 | 80.80 us | 81.00 us |
| P99.9 | 82.54 us | 87.68 us |
| P100 | 136.83 us | 139.39 us |
| Packet loss | 0 | 0 |
| Duration | ~13.9 hrs | ~13.9 hrs |

**One-way P100: ~70us on dedicated interface.** Both transports are equivalent at SKB/copy mode on igb.

### ARM64 — end0 NFS boot (30s test, 20K packets, no NIC tuning, core 1)

| Metric | packet_mmap | AF_XDP (SKB/copy) |
|--------|------------|-------------------|
| Min RTT | 75.26 us | 74.68 us |
| Median | 93.06 us | 93.46 us |
| P99 | 113.00 us | 105.86 us |
| P100 | 146.26 us | 153.12 us |

AF_XDP in SKB/copy mode provides no advantage on ARM64 NFS boot — same kernel path.

## Completed Work

- AF_XDP transport (SKB/copy mode) — fully implemented and tested
- packet_mmap transport with EAGAIN/EBUSY retry loops
- Sequence number validation in client (eliminates ghost frames from AF_PACKET TX copies)
- NicTuner: irqbalance stop, kernel thread migration to core 0, workqueue migration, cpu_dma_latency, performance governor, busy_poll, vm.stat_interval, interrupt coalescing, GRO/GSO/TSO disable, RSS steering, RT throttle disable, ksoftirqd FIFO:50, IRQ pinning
- CPU pinning via sched_setaffinity (no more taskset -c)
- TOML config: separate [packet_mmap] and [af_xdp] sections, client count, output path, send_interval_us
- Python plot script with auto-save PNG
- ARM64 cross-compilation (with AF_XDP when sysroot has libelf/zlib)

## NIC Tuning — What We Tried

### Runtime tuning (implemented in NicTuner)
All applied, all effective on dedicated eno2:
- Stop irqbalance, migrate kernel threads + workqueues to core 0
- cpu_dma_latency = 0, performance governor, vm.stat_interval = 120
- Interrupt coalescing = 0, GRO/GSO/TSO off, RSS all→queue 0
- sched_rt_runtime_us = -1, ksoftirqd FIFO:50
- NIC IRQs pinned to app core, other IRQs moved off

### Not applicable on target hardware
- **THP, NUMA balancing, KSM**: compiled out of the kernel
- **busy_poll/busy_read**: `/proc/sys/net/core/busy_poll` doesn't exist (CONFIG_NET_RX_BUSY_POLL not enabled)
- **RPS**: redundant (IRQs already pinned to app core)
- **RFS**: AF_PACKET doesn't call recv(), no flow tracking
- **Accelerated RFS**: neither e1000e nor igb implements ndo_rx_flow_steer
- **ntuple flow steering on x86**: e1000e doesn't support it, igb doesn't need it (dedicated interface)
- **RSS on eno1**: single queue, impossible
- **Adding queues to eno1**: e1000e is single-queue silicon, hardware limitation

### Requires kernel boot parameters (not possible on diskless NFS boot)
- `isolcpus`, `nohz_full`, `rcu_nocbs`: would eliminate scheduler ticks and kernel threads on app cores
- `mitigations=off`: 5-30% perf gain from disabling Spectre/Meltdown mitigations
- `transparent_hugepage=never`: only if THP were enabled

## Next Steps — AF_XDP Native Mode on ARM64

The ARM64 mkdev50 (macb driver, kernel 6.6) is the most promising target for significant latency improvement. It uniquely supports:

1. **Native XDP** (DRV mode) — BPF runs in the driver's NAPI handler, before skb allocation
2. **ntuple flow steering** — hardware-level ethertype→queue steering
3. **2 RX queues** — enables full NFS/timing traffic separation

### Architecture on ARM64 (end0)

```
                NIC (macb, 2 queues)
                      │
         ┌────────────┴────────────┐
         │                         │
    RX Queue 0                RX Queue 1
    (NFS/SSH/ARP)             (0x88B5 timing)
    ntuple: default           ntuple: ether proto 0x88B5 action 1
         │                         │
    IRQ → Core 0              IRQ → Core 1
    ksoftirqd/0               XDP native redirect
    kernel stack              AF_XDP socket (copy mode)
         │                         │
    NFS client                Our app (SCHED_FIFO:49)
    (SCHED_OTHER)             pinned to core 1
```

### Implementation tasks

#### 1. Separate XDP attach mode from AF_XDP bind mode

Currently `zero_copy = true` sets both `XDP_FLAGS_DRV_MODE` (BPF attach) and `XDP_ZEROCOPY` (socket bind). We need:
- `XDP_FLAGS_DRV_MODE` attach (native XDP) + `XDP_COPY` bind (copy-mode AF_XDP)
- New config option: `xdp_native = true` (controls BPF attach mode, independent of zero_copy)
- `zero_copy` only controls the socket bind flags

```toml
[af_xdp]
queue_id        = 1          # bind to queue 1 (timing traffic steered here)
xdp_native      = true       # attach BPF in native/DRV mode (requires driver support)
zero_copy       = false      # copy-mode bind (macb doesn't support zero-copy)
```

#### 2. Add ntuple flow steering to NicTuner

When the NIC supports ntuple and has multiple queues:
- Enable ntuple: `ethtool -K <iface> ntuple on`
- Add rule: `ethtool -N <iface> flow-type ether proto 0x88b5 action <queue>`
- Pin the timing queue's IRQ to the app core
- Leave NFS traffic on queue 0 / core 0

#### 3. Test on ARM64

- Run AF_XDP native mode + ntuple steering on mkdev50
- Compare against packet_mmap and AF_XDP SKB mode
- Overnight test to validate P100 stability

### Expected benefit

On ARM64 with native XDP + ntuple steering:
- Timing packets bypass the entire kernel network stack (no skb allocation)
- NFS traffic on a completely separate queue/core — zero contention
- Should approach the dedicated-LAN baseline (~70us one-way P100)
