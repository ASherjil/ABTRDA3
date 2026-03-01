# WREN (White Rabbit Event Node) — Technical Reference

## What It Is

WREN is a timing distribution board for CERN's accelerator complex. It replaces multiple
legacy timing networks (GMT, BST, RF distribution, SMP) with a single White Rabbit Ethernet
network providing sub-nanosecond synchronization.

**SoC**: Xilinx Ultrascale+ XCZU4CG (Zynq MPSoC)
- **FPGA gateware**: Time-critical — trigger generation, RF frame parsing (1ns precision)
- **ARM Cortex-R5**: Firmware (bare-metal) — event parsing, configuration, mailbox IPC
- **ARM Cortex-A53**: Reserved for future use

**Form factors**: PXIe (primary), VME64x, PCIe — target ~1000 CHF/board
**Connection**: Single-mode single-fiber to a WR Switch (1 Gbps Ethernet + PTP sync)
**Connectors**: 6, 14, or 38 I/Os depending on form factor, some bidirectional

As of summer 2025, pilot installations deployed on a few dozen FECs.
Reference: ICALEPCS'25 paper WEBR003 (Moscardi et al.)

---

## The Timing Data Model

### Domains
Each accelerator has its own timing domain with named fields, contexts, and events:
- **P** = CPS, **B** = PSB, **E** = LEIR, **S** = SPS, **H** = LHC, **D** = ADE(PS), **A** = LNA(LINAC), **C** = SCT

Domains are isolated: fields, contexts, and events are constrained to the same domain.
Equipment typically listens to a single domain (the accelerator it's installed in).

### Fields
Named typed variables belonging to a domain. Types: INT32, INT64, FLOAT, BOOL, ENUM.
```
BEAM_ID:          int32   (e.g. 24526)
PARTICLE:         enum    (PROTON, PB82, ...)
BASIC_PERIOD_NB:  int32   (e.g. 2)
```
Most fields go in contexts (stable per cycle). Only fields that change within a cycle
are transmitted with individual events.

### Contexts
Snapshots of field values valid for a time interval. Identified by an 8-bit rolling counter.
Only one context is "in force" at any given moment per domain.
Up to 4 cached per WREN.
```
Context ID: 0x1 | Valid from: 08:00:10.000 | Valid to: 08:00:12.400
Fields: {BEAM_ID=24526, PARTICLE=PROTON, BASIC_PERIOD_NB=2}
```

### Events
Named occurrences with a nanosecond-precision **due-time** (TAI timestamp).
Each event references a context (inherits all its field values) and may carry additional
event-specific field values.
```
Event: PX.SBP-CT | Due-time: 08:00:11.200 | Context: 0x1 | BASIC_PERIOD_NB: 2
Effective fields: BEAM_ID=24526, PARTICLE=PROTON (inherited from context)
```

Two types of events in cycling accelerators:
- **Scheduled events**: Predetermined in the supercycle, sent ~3 basic periods in advance
- **Asynchronous events**: Unpredictable (e.g. Beam Dump), sent with due-time in near future
  accounting only for network propagation time

---

## CTIM vs LTIM

### CTIM (Central TIMing)
Events **centrally generated and broadcast** over the WR network to all receivers.
These are the "global announcements" of accelerator cycle milestones.

Defined in `timing-domain-cern` (shared library on NFS, auto-generated from timing database):
```cpp
// CpsDomain.cpp
PX_FCY600_CT(151, "PX.FCY600-CT", "Forewarning Cycle -600ms", domain),
PX_WCY200_CT(159, "PX.WCY200-CT", "Warning start cycle -200ms", domain),
PX_SCY_CT(156,    "PX.SCY-CT",    "Start Cycle", domain),
PX_ECY_CT(148,    "PX.ECY-CT",    "End Cycle", domain),
PX_SBP_CT(155,    "PX.SBP-CT",    "Start Basic Period", domain),
```

**Naming convention**: `<Domain><Sub>.<Name>-CT`
- Domain prefix: P=CPS, B=PSB, S=SPS, E=LEIR, H=LHC
- Sub-prefix: I=Injection, E=Ejection, I2=2nd batch, X=general
- `-CT` suffix = Central Timing (i.e. it's a CTIM)
- Example: `PIX.F900-CT` = CPS Injection forewarning -900ms

### LTIM (Local TIMing)
**Locally generated actions** on a WREN receiver card. Highly configurable trains of pulses
accompanied by optional software interrupts. Two loading modes:

- **Event-bound**: Loaded upon reception of a matching CTIM event (with optional field conditions)
- **Immediate**: Loaded immediately upon configuration (no event trigger needed)

```
CTIM arrives (network) → matches LTIM condition → LTIM loads → start/delay → pulses fire
```

Once loaded, LTIMs are handled entirely by the FPGA gateware — guaranteeing timely
load, start, and stop operations independent of software.

| Feature | CTIM | LTIM |
|---------|------|------|
| Origin | Central, broadcast on network | Local, on receiver card |
| Trigger | Timing system generates it | Programmed, triggered by CTIM arrival or immediately |
| Delay | N/A | Configurable delay from CTIM |
| Hardware | N/A | Has a triggerer channel (1-32), produces pulses + IRQs |
| Managed by | Transmitter software | WrenReceiver API (not in base Receiver interface) |

---

## Wire Protocol: WRT Frames

Two frame types, both L2 Ethernet (max 1500 bytes payload):

### Packet Header (v1 — 3 words / 12 bytes)
```
word 0: [version:8 | domain_id:8 | xmit_id:8 | next_pkt_delay:8]
word 1: [frame_len:16 | seq_id:16]
word 2: [send_timestamp:24 (compressed) | tai_epoch:2 | reserved:6]
```
- version = 0x01 (active)
- seq_id: 0xFFFC = SYNC (resync), 0xFFF8 = INVALID
- send_timestamp: 22 bits @ 0.256us resolution

### Capsule Header (1 word / 4 bytes)
```
byte 0: type      (0x01=context, 0x02=event)
byte 1: padding
bytes 2-3: length (total capsule length in 32-bit words)
```

### Context Capsule (4+ words)
```
word 0: capsule header  {type=0x01, pad, len}
word 1: [context_id:16 | padding:16]      context_id 0-127 valid, 0xFF=none
word 2: valid_from.nsec  (nanoseconds, TAI)
word 3: valid_from.sec   (seconds, TAI)
word 4+: parameters (TLV-encoded)
```

### Event Capsule (4+ words)
```
word 0: capsule header  {type=0x02, pad, len}
word 1: [event_id:16 | context_id:16]     event_id 0-65535, context_id 0xFF=none
word 2: due_time.nsec   (nanoseconds, TAI)
word 3: due_time.sec    (seconds, TAI)
word 4+: parameters (TLV-encoded, optional)
```

### Parameter TLV Encoding (per parameter, 1+ words)
```
word 0: [length:10 | datatype:6 | param_id:16]
word 1+: data (length-1 words)

Datatypes: S32=1, U32=2, S64=3, U64=4, F32=5
```

### Event-type frames (max 1 kHz, no-loss)
```
[Ethernet/UDP Header] [Packet Header (3w)] [Capsules...] [CRC]
```
Payload contains context capsules followed by event capsules.
Context capsules must precede the event capsules that reference them.

### RF-type frames (max 50 kHz, no-loss)
```
[Streamer/RoE Header] [Tx Timestamp | Source ID | Sequence ID | FTW data] [CRC]
```
Carry Frequency Tuning Words for local RF clock regeneration (Bunch clock, Frev).

---

## How Events Are Sent: Legacy vs New

### Legacy GMT: Events fire at the moment they occur

GMT runs on RS-485 serial at 500 kbit/s. Eight 32-bit frames per millisecond.
Events are sent **at the instant** they happen. Equipment needing lead time relies on
a cascade of **forewarning/warning** events at fixed offsets:

```
CPS cycle example (legacy GMT, real-time firing):
  t-900ms:  PIX.F900-CT    fires -> "injection coming in 900ms"
  t-600ms:  PX.FCY600-CT   fires -> "cycle starting in 600ms"
  t-200ms:  PX.WCY200-CT   fires -> "cycle starting in 200ms"
  t-50ms:   PX.WCY50-CT    fires -> "cycle starting in 50ms"
  t=0:      PX.SCY-CT      fires -> "cycle starts NOW"
```

Legacy limitations: 500kbit/s bandwidth, 1.2km max link, 32 nodes/segment,
manual timing calibration needed per FEC, frames too small for contextual info.

### New WRT/WREN: Events sent seconds in advance with future due-times

In the new system, **all scheduled events** carry a **future TAI timestamp** (the due-time).
Events are sent out roughly **3 basic periods in advance** (~3.6 seconds for cycling
accelerators where 1 basic period = 1.2 seconds).

```
WRT/WREN (advance delivery):
  t-3.6s:  Event-type frame arrives containing:
           Context: {BEAM_ID=24526, PARTICLE=PROTON, ...}
           Event: PX.SCY-CT, due-time = t=0 (TAI)

  WREN FPGA loads triggerer config (<23us), then waits on WR-synchronized TAI clock...

  t=0 (nanosecond precision):  Triggerer fires hardware output
```

**Event Delivery Offset** (subscriber-side):
```
PX.SBP-CT example:
  Due time:                08:00:11.200
  Delivery offset:         -160 ms
  Delivery time to app:    08:00:11.040    (app gets notified 160ms early)
```
Range: -5s to +5s, microsecond precision. Only affects software delivery,
NOT trigger load time (use loadOffset in TriggerSettings for that).

**Asynchronous events** (e.g. Beam Dump) are sent with due-time in the near future,
accounting only for network propagation time (~500us max).

**Key consequences**:
1. No more forewarning cascades needed (one event + delivery offset replaces all)
2. Sub-nanosecond trigger precision (FPGA counts TAI clock edges)
3. Scheduled events arrive ~3.6s early (cycling accelerators)
4. Forewarning events still defined in domain-cern for backward compatibility

**LHC exception**: Timing is NOT based on supercycles. Operators initiate phase transitions
(injection, acceleration, collision) using **event tables** — pre-loaded sequences of events
with relative due-times and WAIT entries.

---

## The Trigger Generator (LTIM Hardware)

32 LTIM channels organized in **4 groups of 8**. Each channel can host one or more LTIMs.
This is the core action engine, implemented entirely in FPGA gateware.

### State Machine
```
unconfigured → idle → [event+condition] → loaded → [start] → delay → pulsing
                                                                  ↓
                                                            interrupts + pulses
```
Many LTIMs use simpler config: auto-start (skip loaded state), single pulse or interrupt.

### Configuration Parameters
1. **Loading condition**: Event-ID + up to 3 field predicates (&&, ||, operators: > < >= <= == & bitmask)
2. **Loading offset**: +/- delay from event timestamp
3. **Clock input**: PPS-aligned 1Hz, 1kHz, 1MHz, 10MHz, 40MHz, 1GHz, Fbunch, Frev, or external
4. **Trigger delay**: N clock cycles after start
5. **Start input** (opt): External pin, another LTIM, PPS, or AUTO (immediate)
6. **Start type/polarity** (opt): edge/level, active-high/low
7. **Stop input** (opt): External pin or another LTIM
8. **Stop type/polarity** (opt): edge/level, active-high/low
9. **Pulse width**: 32-bit, 1ns resolution, min 16ns (0 = level output)
10. **Pulse period**: 32-bit, in clock multiples, min 32ns
11. **Number of pulses**: 16-bit (0 = infinite)
12. **Output**: External pin and/or IRQ and/or another LTIM's input

### Group Output Logic
Each group's 8 LTIM outputs can be combined through **AND, NAND, OR, or NOR** gates
before reaching the physical output pin. Output polarity is configurable at the pin.

### Performance
- Processing latency (frame reception -> triggerer loaded): **< 23 us**
- Configuration latency (host -> triggerer): < 10 ms
- Max interrupt rate: ~1100/sec
- Max interrupt latency: 100 us

### Examples
**50MHz clock on 450GeV extraction:**
- Condition: `extraction AND Energy == 450GeV`
- Clock: WR TAI, Width: 20ns, Period: 40ns, Pulses: infinite

**5us pulse 100ns before flat-top:**
- Condition: `flat-top`, Offset: -100ns
- Width: 5000ns, Pulses: 1

**RF-synchronous pulse 3 revolutions after injection:**
- Condition: `injection AND Particle == protons`
- Clock: Frev, Delay: 3, Start: External (edge, active-high)
- Width: 1 Frev period, Pulses: 1

---

## Software Stack

```
┌──────────────────────────────────────────────────────────────┐
│                  FESA / EPICS / CLI (wrentest)                │
├──────────────┬───────────────┬────────────┬──────────────────┤
│ receiver-wren│ receiver-ctr  │receiver-soft│ receiver-mock   │  transmitter-wren
│ (WREN HW)   │ (legacy GMT)  │(TCP/PTP)   │ (testing)       │  (TX impl)
├──────────────┴───────────────┴────────────┴──────────────────┤
│              receiver-cern (ReceiverProvider factory)         │
├──────────────────────────────────────────────────────────────┤
│    Receiver (abstract interface)                              │
│    └── ContextAwareReceiver (context caching base)           │
├──────────────────────────────────────────────────────────────┤
│    domain (Domain, Field, Context, Event, FieldDescriptor)   │
│    domain-cern (CPS, PSB, SPS, LHC, LEIR, ADE, LNA, SCT)   │
├──────────────────────────────────────────────────────────────┤
│    libwrenrx.so / libwrentx.a  (low-level C API)            │
├──────────────────────────────────────────────────────────────┤
│    Linux kernel driver (wren-core.ko)                        │
│    64 clients, 256 buffers, mailbox IPC                      │
├──────────────────────────────────────────────────────────────┤
│    ARM R5 Firmware (bare-metal)                              │
├──────────────────────────────────────────────────────────────┤
│    FPGA Gateware (VHDL)  — triggerers, NIC, RF, NCO, clocks │
└──────────────────────────────────────────────────────────────┘
```

**Key design decisions**:
- LTIM management deliberately NOT in base Receiver interface (keeps it generic/reusable)
- receiver-ctr wraps legacy CTR library for transparent GMT-to-WRT migration
- domain-cern is a shared library on NFS — add events/fields without recompiling clients

### receiver-soft (relevant to sidecar project)
Official solution for platforms without WREN hardware (SoC boards, etc.).
Connects via **TCP to a Timing Distributor server**. Contexts and events transmitted
in advance. Accuracy limited by host system clock (improvable with PTP).
Expected to grow in importance as SoC platforms multiply in the accelerator complex.

**This is what the sidecar project competes with** — but via raw Ethernet (~30us)
instead of TCP through a server (~1.28ms via FESA/RDA path).

---

## Receiver C++ API

### Core Interface (Receiver)
```cpp
class Receiver {
    Time utc() const;                                  // Current UTC time
    void subscribe(const std::string& eventName);      // Subscribe to CTIM/LTIM
    void subscribe(const std::string& eventName, Duration offset);  // With delivery offset
    void unsubscribe(const std::string& eventName);
    void subscribeContext(const Domain& domain);
    void unsubscribeContext(const Domain& domain);
    Context getContext(const Domain& domain) const;     // Latest context
    Context getContext(const Domain& domain, const Time& time) const;
    Event waitEvent();                                  // BLOCKING — wait for next event
};
```

### Context Caching (ContextAwareReceiver)
Extends Receiver with boost::multi_index_container cache:
- Lookup by (domain, context_id) — hash index
- Lookup by (domain, validFrom timestamp) — ordered index
- Default retention: 300 seconds
- Auto-eviction of old contexts on new context arrival

### WrenReceiver Extensions (LTIM Management)
```cpp
class WrenReceiver : public ContextAwareReceiver {
    // LTIM creation
    std::shared_ptr<WrenEventBoundTriggerCondition>
        newEventBoundTriggerCondition(const EventDescriptor&);
    std::shared_ptr<WrenImmediateTriggerCondition>
        newImmediateTriggerCondition(const Domain&);

    std::vector<uint16_t> createTrigger(
        const std::string& name,
        uint8_t channel,    // 1-32
        const std::vector<std::tuple<TriggerSettings, std::shared_ptr<TriggerCondition>>>& configs
    );

    // LTIM management
    void updateTrigger(const std::string& name, uint16_t index, const TriggerSettings&);
    void deleteTrigger(const std::string& name);
    void forceTrigger(const std::string& name, uint16_t index);
    TriggerSettings getTriggerSettings(const std::string& name, uint16_t index);
    std::vector<TriggerInfo> getAllTriggers() const;
};
```

### TriggerSettings
```cpp
struct TriggerSettings {
    bool enabled, interruptEnabled, outputEnabled, repeat;
    TriggerStart start;       // Pin, LTIM chain, PPS, AUTO
    TriggerStop stop;         // Pin, LTIM chain, NONE
    TriggerClock clock;       // Pin, LTIM chain, 1kHz/1MHz/10MHz/40MHz/1GHz/FBUNCH/FREV
    uint64_t initialDelay;    // Clock cycles
    Duration pulseWidth;      // Nanoseconds
    uint32_t pulsePeriod;     // Clock cycles (0=level)
    uint32_t nPulses;         // 0=infinite
    Duration loadOffset;      // Offset on event due-time (negative=early)
};
```

### TriggerCondition
```cpp
class TriggerCondition {
    virtual bool isEventBound() const = 0;
    virtual const EventDescriptor& getEventDescriptor() const;  // throws if not event-bound
    virtual void setExpression(std::unique_ptr<condition::Node> expr) = 0;
};
// Expression: up to 3 field predicates combined with &&/||
// Operators: EQ, NE, LT, LE, GT, GE, BITMASK
```

### Usage Example (from demo)
```cpp
auto receiver = std::make_shared<timing::receiver::WrenReceiver>(lun);
receiver->subscribe("PX.SBP-CT");
receiver->subscribeContext(Domain::of("CPS"));

while (true) {
    Event event = receiver->waitEvent();  // blocking
    auto beam_id = event.getFieldValue<int32_t>("BEAM_ID");
    auto trigger_time = event.getTriggerTime();
    auto& context = event.getContext();
}
```

---

## Kernel Driver: Subscription & Event Delivery Path

### Subscription Setup (IOCTL path)

**1. Add RX Source** (`WREN_IOC_RX_ADD_SOURCE`):
- Allocates source slot, stores protocol info + holdoff
- Sends `CMD_RX_SET_SOURCE` to firmware via mailbox
- Returns source index to userspace

**2. Add Config = Condition + Action** (`WREN_IOC_RX_ADD_CONFIG`):
- Allocates/reuses condition (conditions shared across actions if identical)
- Sends `CMD_RX_SET_COND` to firmware (comparator logic)
- Allocates action slot, links to condition
- Sends `CMD_RX_SET_ACTION` to firmware (pulser config)
- Returns action index

**3. Subscribe to Events** (`WREN_IOC_RX_SUBSCRIBE_EVENT`):
- Sets bit in per-client bitmap: `client->sources[src_idx].evsubs[event_id]`
- Increments global counter: `wren->sources[src_idx].evsubs[event_id]`
- **On first global subscription**: sends `CMD_RX_SUBSCRIBE` to firmware
- Firmware only writes events to async ring if global subscription count > 0

**4. Subscribe to Contexts** (`WREN_IOC_RX_SUBSCRIBE_CONTEXT`):
- Sets bit in device bitmap: `wren->client_sources[src_idx]`
- Immediately sends cached contexts to client ring

**5. Subscribe to Configs/Pulses** (`WREN_IOC_RX_SUBSCRIBE_CONFIG`):
- Sets bit in device bitmap: `wren->client_configs[act_idx]`
- Implicitly subscribes to parent source's contexts

### Event Flow: Firmware -> Driver -> Userspace

```
WR Network -> FPGA -> Firmware R5
  |
  | Firmware checks: evsubs[event_id] > 0?
  | If NO: discard (no subscribers)
  | If YES:
  |   Write capsule to async_data[] ring buffer (2048 words)
  |   Update async_board_off (firmware write pointer)
  |   Assert ASYNC interrupt
  |
  v
Hardware IRQ -> wren_irq_handler()
  |   Read & acknowledge ISR
  |   Return IRQ_WAKE_THREAD (deferred to threaded handler)
  |
  v
Threaded handler -> wren_irq_thread()
  |   Read async_board_off (firmware) vs async_host_off (driver)
  |   For each new capsule between pointers:
  |
  |   CMD_ASYNC_CONTEXT (0x01):
  |     Allocate buffer(s) from 256-buffer pool (lock-free CAS)
  |     Copy capsule data from async_data[] to buffer
  |     For each client in client_sources[src_idx] bitmap:
  |       Put msg in client->ring[ring_tail] (128 entries per client)
  |       Increment buffer refcnt
  |       wake_up(&client->wqh)
  |     Store in cached_contexts[ctxt_id % 4]
  |
  |   CMD_ASYNC_EVENT (0x02):
  |     Allocate buffer, copy data
  |     For each client with context subscription to this source:
  |       Check: test_bit(event_id, client->sources[src_idx].evsubs)
  |       If subscribed: put msg in client ring, wake_up
  |     Store as last_evt[src_idx] for pulse association
  |
  |   CMD_ASYNC_PULSE (0x04):
  |     Lookup config_id from comparator table
  |     For each client in client_configs[config_id] bitmap:
  |       Put msg (with timestamp + config_id) in client ring
  |       Attach associated event buffer if available
  |       wake_up
  |
  |   CMD_ASYNC_CONFIG (0x03):
  |     Configuration successfully applied notification
  |
  |   CMD_ASYNC_CONT (0x07):
  |     Continuation for oversized capsules spanning multiple entries
  |
  |   Update async_host_off (driver read pointer)
  |
  v
Userspace -> read(client_fd) -> wren_usr_read()
  |   wait_event_interruptible(&client->wqh, ring not empty)
  |   Read msg from ring[ring_head]
  |   Extract cmd + buf_idx from atomic buf_cmd field
  |   Copy buffer data to userspace
  |   Advance ring_head
  |   wren_put_buffer() -> decrement refcnt -> return to pool if 0
```

### Buffer Management (256-buffer pool)
- Lock-free allocation via atomic CAS on free-list head
- Reference counting: +1 per client receiving, +1 for context cache
- Auto-release when all consumers have read
- Each buffer: 128 x 32-bit words max (512 bytes)

### 64-Client Model
- Each `open()` on the WREN device creates a client (max 64)
- Per-client: 128-entry ring buffer, per-source subscription bitmaps (4096 event IDs)
- Device-level bitmaps track which clients subscribe to which sources/configs
- Lock-free ring: head advanced by reader, tail by IRQ handler

### Key Filtering Points
| Where | What's filtered |
|-------|----------------|
| Firmware | Events with zero subscribers globally -> discarded |
| Firmware | Source holdoff (inter-packet minimum interval) |
| Driver (IRQ thread) | Events delivered only to clients subscribed to that event_id |
| Driver (IRQ thread) | Pulses delivered only to clients subscribed to that config_id |
| Driver (IRQ thread) | Contexts delivered to all source-subscribed clients (no filtering) |

---

## Async Ring Buffer Detail (what the sidecar reads)

```
BAR1 + 0x12000: async_data[0..2047]  (2048 x 32-bit words, circular)
BAR1 + 0x14000: async_board_off      (firmware write pointer, word index)
BAR1 + 0x14004: async_host_off       (driver read pointer — NEVER touch)
```

### Capsule Format in Ring (same as wire format)

Capsule header (1 word):
```
bits [7:0]   = typ          (0x01=context, 0x02=event, 0x04=pulse, 0x07=continuation)
bits [15:8]  = source_idx   (RX source index)
bits [31:16] = len          (total words including header)
```

Event capsule (typ=0x02):
```
word 0: header     {typ=0x02, source_idx, len}
word 1: IDs        {ev_id[15:0], ctxt_id[31:16]}
word 2: ts.nsec    (due-time nanoseconds, TAI)
word 3: ts.sec     (due-time seconds, TAI)
word 4+: params    (TLV-encoded, optional)
```

Context capsule (typ=0x01):
```
word 0: header     {typ=0x01, source_idx, len}
word 1: IDs        {ctxt_id[15:0], padding[31:16]}
word 2: valid_from.nsec
word 3: valid_from.sec
word 4+: params    (TLV-encoded fields)
```

Pulse capsule (typ=0x04):
```
word 0: header     {typ=0x04, source_idx, len}
word 1: comp_idx   (comparator/action index)
word 2: ts.nsec    (pulse execution timestamp)
word 3: ts.sec
```

**The ring buffer only contains events the kernel driver has subscribed to.**
Firmware checks `evsubs[event_id] > 0` before writing to the ring.

**Wire and async formats are identical for capsules** — only the delivery mechanism
differs (Ethernet frame vs shared-memory ring buffer).

---

## Physical I/O

- **Front panel**: 2-8 LEMO00 (bidirectional) + high-density coax (32+ I/Os) + SFP + JTAG + microUSB
- **Backplane**: Up to 32 outputs (VME/PXIe), duplicates of front panel signals
- **Bidirectional I/Os**: Configurable as triggerer output, external start/stop/clock input
- **Buffers**: 3V on 50 Ohms, LED per I/O
- **Input timestamping**: TAI timescale, ns precision

Group output logic: each group's 8 triggerer outputs combinable via AND/NAND/OR/NOR gates
with configurable polarity.

---

## CERN Deployment

- All timing domains on a **single WR network**, separated by VLANs
- Tree topology, 5 layers deep, ~200 WR switches, ~2000 receivers
- Traffic flows toward leaves only (precaution for determinism)
- Max propagation latency: ~500 us (estimated, to be measured)
- Grandmaster clock provides common time reference to all VLANs
- Migration: SPS/LHC by 2029 (LS3), remaining by 2033 (LS4)
- Parallel operation with legacy GMT during transition
- As of summer 2025: few dozen pilot FECs deployed

---

## Diagnostics: wrentest CLI

Command-line diagnostic tool with hierarchical sub-commands:
- `status` — network link, synchronization, time
- `events` / `fields` — CTIM event and field descriptions
- `ltim` — LTIM configuration and management
- `io` — I/O pin configuration
- `wait` — monitor CTIM events and LTIM interrupts as they occur
- `history` — past CTIMs, LTIMs, rising/falling edges on I/O pins
- `troubleshooting` — diagnose LTIM misbehavior, suggest fixes

---

## Key Source Files

| What | Path |
|------|------|
| **Ring buffer layout** | `wren-gw/sw/hw-include/mb_map.h` |
| **Capsule/packet format** | `wren-gw/sw/api/include/wren/wren-packet.h` |
| **Async message types** | `wren-gw/sw/hw-include/wren-mb-defs.h` |
| **Host register map** | `wren-gw/sw/hw-include/host_map.h` |
| **Mailbox commands** | `wren-gw/sw/hw-include/wren-mb-cmds.def` |
| **Kernel driver** | `wren-gw/sw/drivers/wren-core.c` |
| **Kernel structures** | `wren-gw/sw/drivers/wren-core.h` |
| **IOCTL definitions** | `wren-gw/sw/drivers/wren-ioctl.h` |
| **R5 firmware main** | `wren-gw/sw/firmware/main.c` |
| **RX firmware core** | `wren-gw/sw/firmware/wrenrx-core.c` |
| **Userspace RX API** | `wren-gw/sw/api/include/wren/wrenrx.h` |
| **VHDL top-level** | `wren-gw/hdl/rtl/wren_core.vhd` |
| **Triggerer VHDL** | `wren-gw/hdl/rtl/pulser_group.vhd` |
| **Receiver interface** | `timing-receiver/src/timing-receiver/Receiver.h` |
| **Context cache** | `timing-receiver/src/timing-receiver/ContextCache.h` |
| **WrenReceiver** | `timing-receiver-wren/src/timing-receiver-wren/WrenReceiver.h` |
| **Trigger conditions** | `timing-receiver-wren/src/timing-receiver-wren/TriggerCondition.h` |
| **WREN message types** | `timing-receiver-wren/src/timing-receiver-wren/WrenMessage.h` |
| **Domain classes** | `timing-domain/src/timing-domain/Domain.h` |
| **CPS events catalog** | `timing-domain-cern/src/timing-domain-cern/CpsDomain.cpp` |
| **PSB events catalog** | `timing-domain-cern/src/timing-domain-cern/PsbDomain.cpp` |
| **SPS events catalog** | `timing-domain-cern/src/timing-domain-cern/SpsDomain.cpp` |
| **LHC events catalog** | `timing-domain-cern/src/timing-domain-cern/LhcDomain.cpp` |
| **Demo receiver app** | `timing-receiver-wren/src/demo/receiver-wren.cpp` |
| **WREN functional spec** | `WREN_LTIM/WREN_Specs_20230209.pdf` |
| **WREN study document** | `WREN_LTIM/WREN_STUDY.pdf` |
| **ICALEPCS'25 paper** | WEBR003 (Moscardi et al.) |

All source paths under `/opt/public/FESA_core_repos/` prefixed with `WREN_LTIM/` or `timing/timing-wrt/`.
