# WREN Sidecar Spy Poller — Technical Reference

## WREN PCI Identification
- Vendor: `0x10DC` (CERN)
- Device: `0x0455`
- sysfs BAR1: `/sys/bus/pci/devices/<BDF>/resource1`

## BAR1 Memory Map

### Host Registers (BAR1 + 0x0000)
| Offset | Register | Description |
|--------|----------|-------------|
| 0x00 | IDENT | `0x5745524E` ("WREN") — use to verify mapping |
| 0x10 | WR_STATE | bit 0: link_up, bit 1: time_valid |
| 0x14 | TM_TAI_LO | Current TAI seconds (low 32b) |
| 0x18 | TM_TAI_HI | Current TAI seconds (high bits) |
| 0x1C | TM_CYCLES | 28-bit cycle counter @ 62.5MHz (16ns/tick) |
| 0x20 | TM_COMPACT | Compact time: cycles[27:0] + tai_sec[31:28] |
| 0x28 | ISR | Interrupt status (masked), bit 4 = ASYNC |
| 0x2C | ISR_RAW | Raw interrupt status (read-only, safe) |
| 0x30 | IMR | Interrupt mask |
| 0x34 | IACK | Interrupt ack (write-only, DO NOT touch) |

### Mailbox (BAR1 + 0x10000)
| Offset | Region | Size |
|--------|--------|------|
| 0x0000 | B2H (board-to-host sync reply) | 4KB |
| 0x1000 | H2B (host-to-board sync command) | 4KB |
| 0x2000 | async_data[0..2047] (ring buffer) | 8KB |
| 0x4000 | async_board_off (write pointer) | 4B |
| 0x4004 | async_host_off (read pointer) | 4B |

**Absolute offsets from BAR1 base:**
```
ASYNC_DATA_BASE  = BAR1 + 0x12000
ASYNC_BOARD_OFF  = BAR1 + 0x14000
ASYNC_HOST_OFF   = BAR1 + 0x14004  (never write this)
```

## Ring Buffer Protocol

- 2048 x 32-bit words, indices 0-2047
- Producer: R5F firmware writes data, advances async_board_off
- Consumer: kernel driver reads data, advances async_host_off
- Sidecar: reads data, never modifies async_host_off (non-destructive peek)
- Empty when: `async_board_off == async_host_off`
- Wrap mask: `(offset + len) & 2047`

## Capsule Header Format (1 word)

```
bits [7:0]   = typ          (message type)
bits [15:8]  = source_idx   (RX source index)
bits [31:16] = len          (total length in 32-bit words, including header)
```

### Message Types
| typ | Constant | Meaning |
|-----|----------|---------|
| 0x01 | CMD_ASYNC_CONTEXT | CTIM context received |
| 0x02 | CMD_ASYNC_EVENT | LTIM/CTIM event received |
| 0x03 | CMD_ASYNC_CONFIG | Comparator loaded (action triggered) |
| 0x04 | CMD_ASYNC_PULSE | Pulse generated (with timestamp) |
| 0x06 | CMD_ASYNC_PROMISC | Raw packet (debug mode) |
| 0x07 | CMD_ASYNC_CONT | Continuation of large payload |
| 0x08 | CMD_ASYNC_REL_ACT | Action released |

## Event Capsule Layout (typ=0x02)

```
word 0: header        { typ=0x02, source_idx, len }
word 1: IDs           { ev_id[15:0], ctxt_id[31:16] }
word 2: ts.nsec       (nanoseconds, TAI)
word 3: ts.sec        (seconds, TAI)
word 4+: parameters   (TLV-encoded, optional)
```

- `ev_id`: uint16_t, range 0-511
- `ctxt_id`: uint16_t, 0-127 valid, 0xFF = no context
- `ts`: the event's **due time** (scheduled action time, not reception time)

## Event ID Examples (from timing-domain-cern)

| Name | Domain | Numeric ID |
|------|--------|------------|
| PX.SCY-CT | CPS | 156 |
| BX.SCY-CT | PSB | 351 |
| PX.FCY600-CT | CPS | 151 |

Event IDs are 32-bit in FESA (`EventDescriptor::getId()`) but 16-bit on the wire.

## Busy-Poll Detection Algorithm

```cpp
// One-time setup
uint32_t shadow_off = read32(ASYNC_BOARD_OFF);  // sync to current position

// Hot loop (pinned core, SCHED_FIFO, mlockall)
while (running) {
    uint32_t board_off = read32(ASYNC_BOARD_OFF);  // ~2.6us PCIe read

    if (board_off == shadow_off)
        continue;  // no new data

    // Process all new messages
    while (shadow_off != board_off) {
        uint32_t hdr = read32(ASYNC_DATA_BASE + shadow_off * 4);
        uint8_t  typ = hdr & 0xFF;
        uint16_t len = (hdr >> 16) & 0xFFFF;

        if (typ == 0x02) {  // CMD_ASYNC_EVENT
            uint32_t ids = read32(ASYNC_DATA_BASE + ((shadow_off + 1) & 2047) * 4);
            uint16_t ev_id = ids & 0xFFFF;

            if (ev_id == target_event_id) {
                // Read timestamp if needed
                // uint32_t nsec = read32(ASYNC_DATA_BASE + ((shadow_off+2)&2047)*4);
                // uint32_t sec  = read32(ASYNC_DATA_BASE + ((shadow_off+3)&2047)*4);
                fire_trigger_packet();
            }
        }

        shadow_off = (shadow_off + len) & 2047;  // advance with wrap
    }
}
```

### Per-iteration costs
| Step | Latency |
|------|---------|
| Poll async_board_off (1 PCIe read) | ~2.6us |
| Read header (1 PCIe read, only on new data) | ~2.6us |
| Read ev_id (1 PCIe read, only on event) | ~2.6us |
| Parse + match | <100ns |

Average detection: ~1.3us (half polling interval) + ~5.2us (2 reads) = **~6.5us**

## Forwarding Strategy

**Events arrive seconds in advance.** The `ts` field is the **due time** (future TAI time
when the accelerator event occurs), not the reception time. The sidecar catches events
seconds before they're due.

**Strategy: forward immediately, always.** Catch event from ring buffer, send via
packet_mmap to remote FEC with the due time embedded in the payload. No timers, no
holding. The remote FEC decides:
- `due_time > now + threshold` → schedule action at due_time (normal, seconds of margin)
- `due_time <= now` → act immediately (emergency/zero-lead event)

This handles both normal events (seconds of lead time) and emergency events (~17-32us
forwarding latency) with identical sidecar logic.

## Expected Event Load

Typical subscription: 1-2 CTIMs + ~5 LTIMs = ~7 events per cycle.
Multiple events often arrive in the same WRT frame (firmware writes them back-to-back).

| Scenario | Sidecar Processing Time |
|----------|------------------------|
| 1 event | ~6.5us detect + ~3us TX = ~10us |
| 7 events (batch) | ~6.5us detect + 7 x (~5.2us read + ~3us TX) = ~64us |

With seconds of lead time, even 64us for 7 events is negligible.
Batching all events into one Ethernet frame (single sendto) would reduce to ~43us,
but individual packets are simpler and perfectly adequate.

## End-to-End Latency Estimate (per event)

| Stage | Latency |
|-------|---------|
| WREN R5F firmware (parse + ring write) | ~5-10us |
| Sidecar detection (poll + peek) | ~4-8us |
| packet_mmap TX (sendto + kernel + NIC DMA) | ~3-5us |
| Wire transit (1GbE direct cable) | ~1-2us |
| packet_mmap RX (NIC + NAPI + ring) | ~4-7us |
| **Total** | **~17-32us** |

For normal events this latency is irrelevant (seconds of lead time available).
For zero-lead emergency events this is the worst-case forwarding delay.

## Optimizations
- TX side: `tc qdisc replace dev ethX root noqueue` (bypass qdisc)
- RX side: `setsockopt(fd, SOL_SOCKET, SO_BUSY_POLL, &1, 4)` + `sysctl net.core.busy_poll=1`
- RX side: `ethtool -C ethX rx-usecs 0 rx-frames 1` (disable interrupt coalescing)
- Both: isolated CPU core, SCHED_FIFO, mlockall(MCL_CURRENT|MCL_FUTURE)

## Safety Notes
- NEVER write to IACK (0x34) -- that's the kernel driver's job
- NEVER write to async_host_off -- the kernel driver manages consumption
- ISR/ISR_RAW are read-only -- safe to read for diagnostics
- The sidecar is purely observational; kernel driver operates normally alongside it

## Timing Registers for Benchmarking
```
current_tai_sec  = read32(BAR1 + 0x14)  // TM_TAI_LO
current_cycles   = read32(BAR1 + 0x1C)  // TM_CYCLES (62.5MHz)
current_ns       = current_cycles * 16  // 16ns per tick
detection_lead   = event.ts - current_time  // how early we caught it
```
No hardware reception timestamp exists in the ring buffer. For benchmarking, use `clock_gettime(CLOCK_TAI)` on host or WREN's own timing registers.

## Benchmark: FESA/EDGE Baseline (to beat)

Existing approach measured in test lab (both FECs in adjacent racks):
1. WREN-installed FEC receives timing event
2. FESA publishes via RDA/CMW subscription (TCP/IP, CERN technical network)
3. Remote FEC (no timing card) receives event via subscription
4. FESA RT action writes pulse (1→0) to output patch panel using EDGE
5. LEMO cable carries pulse back to WREN-installed FEC
6. WREN hardware timestamps the input signal arrival

**Result: median 1.28ms, P100 3+ms**

Our target with sidecar + packet_mmap + ABTEdge: **~17-32us** (40-75x faster).
Same LEMO loopback setup can be used for apples-to-apples comparison.

## Reference Source Files
- Ring buffer struct: `wren-gw/sw/hw-include/mb_map.h`
- Capsule format: `wren-gw/sw/api/include/wren/wren-packet.h`
- Async message types: `wren-gw/sw/hw-include/wren-mb-defs.h`
- Host register map: `wren-gw/sw/hw-include/host_map.h`
- Kernel driver: `wren-gw/sw/drivers/wren-core.c`
- Userspace API: `wren-gw/sw/api/include/wren/wrenrx.h`
- Event ID catalog: `timing-wrt/timing-domain-cern/src/timing-domain-cern/*.cpp`

## packet_mmap Forwarding Layer (ABTRDA3)

### Design Decisions

| Decision | Choice | Rationale |
|----------|--------|-----------|
| TPACKET version | **V2** for both RX and TX | Deterministic frame-by-frame processing. V3 block batching introduced jitter/timeouts for single events. |
| Ring sizing | `frame_size = 4096` | Page-aligned frames. One frame per slot. No block aggregation. |
| TX send mechanism | `send(fd, NULL, 0, MSG_DONTWAIT)` | Immediate transmission of marked frame. |
| Qdisc bypass | `PACKET_QDISC_BYPASS` on TX | Skips kernel traffic control layer, saves ~1-2us per packet. |
| HW timestamps | RX: `SOF_TIMESTAMPING_RAW_HARDWARE` | Zero cost in `tpacket2_hdr`. |
| Safety | 30s Watchdog Thread | Prevents system lockup when running `SCHED_FIFO` at 100% CPU on isolated cores. |

### TPACKET_V2 Ring Setup (Deterministic Low-Latency)

```cpp
struct tpacket_req req{};
req.tp_block_size = 4096;   // Page size
req.tp_frame_size = 4096;   // One frame per block/slot
req.tp_block_nr   = 64;     // Ring depth
req.tp_frame_nr   = 64;     // Total frames

setsockopt(fd, SOL_PACKET, PACKET_RX_RING, &req, sizeof(req)); // or TX_RING
```

### Measured Performance (Feb 2026)
Hardware: CERN FECs (cfc-865-mkdev30 <-> cfc-865-mkdev16)
Optimization: `SCHED_FIFO` (99), `isolcpus`, `mlockall`

| Metric | Value |
|--------|-------|
| Min RTT | ~43 µs |
| Avg RTT | ~57 µs |
| **One-Way Latency** | **~28.5 µs** |

### RX Busy-Poll Hot Loop (V2)

```cpp
while (running) {
    auto* hdr = (struct tpacket2_hdr*)next_frame_ptr;

    if ((hdr->tp_status & TP_STATUS_USER) == 0)
        continue;  // spin

    // Data ready at: (uint8_t*)hdr + hdr->tp_mac
    process_packet((uint8_t*)hdr + hdr->tp_mac, hdr->tp_snaplen);

    // Release frame
    hdr->tp_status = TP_STATUS_KERNEL;
    advance_ring_pointer();
}
```

### TX Hot Path (V2)

```cpp
bool send_packet(const uint8_t* payload, size_t len) {
    auto* hdr = (struct tpacket2_hdr*)next_frame_ptr;

    if (hdr->tp_status != TP_STATUS_AVAILABLE)
        return false;

    // Copy payload
    memcpy((uint8_t*)hdr + TPACKET_ALIGN(sizeof(tpacket2_hdr)), payload, len);
    hdr->tp_len = len;
    hdr->tp_snaplen = len;

    // Mark and flush
    hdr->tp_status = TP_STATUS_SEND_REQUEST;
    send(fd, NULL, 0, MSG_DONTWAIT);

    advance_ring_pointer();
    return true;
}
```

### System Tuning Checklist

```bash
# 1. Enable Busy Polling (Critical for RX)
sysctl -w net.core.busy_poll=1
sysctl -w net.core.busy_read=1

# 2. Run with Real-Time Priority and CPU Pinning
# (Test app now accepts --cpu <N> to handle pinning internally)
sudo ./abtrda3_test --client --count 1000 --cpu 2

# 3. Disable Interrupt Coalescing
ethtool -C eth0 rx-usecs 0 rx-frames 1

# 4. Watchdog Safety
# App automatically terminates after 30s to allow recovery if SSH locks up.
```
