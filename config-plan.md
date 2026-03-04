# AF_XDP Implementation Plan for ABTRDA3

## Context

ABTRDA3 currently uses `packet_mmap` for ultra-low-latency timing event TX/RX. We're adding AF_XDP as an alternative transport to potentially achieve even lower latency. The XDP eBPF filter is **safety-critical** — the target machines boot from NFS over the same NIC, so the filter must only redirect our custom EtherType (0x88B5) to userspace and pass everything else to the kernel stack.

**Target:** cfc-865-mkdev16/mkdev30, Debian 12 FECOS, kernel 5.10-rt, eno2 = Intel I350 (`igb` driver, native XDP, 4 queues).

## Architecture

- **Separate classes, same hot-path API** — `AfXdpTx` / `AfXdpRx` satisfy the same `TxRing` / `RxRing` C++20 concepts as `PacketMmapTx` / `PacketMmapRx`
- **Shared socket** — AF_XDP requires TX and RX to share one socket + UMEM on the same queue. `AfXdpSocket` handles setup; `AfXdpTx(sock)` and `AfXdpRx(sock)` attach to its rings
- **Embedded BPF** — XDP filter compiled at build time with clang, embedded as C byte array
- **FetchContent for libbpf** — built as static lib from source, no system package needed
- **Compile-time selection** — `#ifdef ABTRDA3_HAS_AF_XDP`, templates constrained by concepts, zero virtual dispatch

## File Layout

### New files

| File | Purpose | ~Lines |
|------|---------|--------|
| `src/RxFrame.hpp` | `RxFrame` struct (extracted from PacketMmapRx.hpp) | 15 |
| `src/RingConcepts.hpp` | `TxRing`, `RxRing` C++20 concepts | 40 |
| `src/AfXdpSocket.hpp` | `XdpConfig` struct + `AfXdpSocket` RAII class decl | 100 |
| `src/AfXdpSocket.cpp` | Socket/UMEM/ring-mmap/BPF-load/attach/bind | 250 |
| `src/AfXdpTx.hpp` | `AfXdpTx` class (inline hot path) | 120 |
| `src/AfXdpTx.cpp` | Constructor (init free stack, wire pointers), move ops | 80 |
| `src/AfXdpRx.hpp` | `AfXdpRx` class (inline hot path) | 90 |
| `src/AfXdpRx.cpp` | Constructor (pre-fill Fill ring), move ops | 70 |
| `src/bpf/xdp_filter.bpf.c` | XDP EtherType filter eBPF program | 50 |
| `cmake/BpfCompile.cmake` | CMake function: `.bpf.c` → `.bpf.o` → `.h` embed | 50 |
| `cmake/BinToHeader.cmake` | Binary-to-C-header conversion script | 40 |
| `cmake/BuildLibbpf.cmake` | Build libbpf as CMake static library | 40 |

### Files to modify

| File | Change |
|------|--------|
| `src/PacketMmapRx.hpp` | Move `RxFrame` out, include `RxFrame.hpp` |
| `src/PacketMmapTx.hpp` | Add `static_assert(TxRing<PacketMmapTx>)` |
| `src/CMakeLists.txt` | Add AF_XDP sources, libbpf FetchContent, BPF compile |
| `CMakeLists.txt` | Add `ABTRDA3_ENABLE_AF_XDP` option, `enable_language(C)` |
| `test/TestConfig.hpp` | Add `Transport` enum, `AfXdpTestConfig`, parse `[af_xdp]` |
| `test/Client.hpp` | Template on `TxRing`/`RxRing`, take TX/RX by reference |
| `test/Server.hpp` | Template on `TxRing`/`RxRing`, take TX/RX by reference |
| `test/main.cpp` | Construct TX/RX in main, dispatch on transport |
| `test/abtrda3_test.toml` | Add `transport` field and `[af_xdp]` section |

## XDP eBPF Filter — Safety-Critical

```c
// src/bpf/xdp_filter.bpf.c — DEFAULT IS XDP_PASS (safe)
SEC("xdp")
int xdp_filter_ethertype(struct xdp_md *ctx) {
    struct ethhdr *eth = (void *)(long)ctx->data;
    if ((void *)(eth + 1) > (void *)(long)ctx->data_end)
        return XDP_PASS;                    // too short → kernel

    if (eth->h_proto != bpf_htons(target_ethertype))
        return XDP_PASS;                    // not ours → kernel (NFS/SSH safe)

    return bpf_redirect_map(&xsks_map, ctx->rx_queue_index, XDP_PASS);
    //                                     fallback: XDP_PASS ─────────^
}
```

**Safety guarantees:**
- Default action is always `XDP_PASS` — packets go to normal kernel stack
- Only `EtherType == 0x88B5` gets redirected to AF_XDP socket
- If XSK socket not ready → `XDP_PASS` fallback, no drops
- Never uses `XDP_DROP` — we never discard any packet
- `target_ethertype` configurable via libbpf global variable rewrite at load time

## AF_XDP Ring Architecture

```
                    ┌─────────────┐
                    │   UMEM      │  Contiguous mmap'd memory
                    │ (N frames)  │  Split: first half TX, second half RX
                    └──────┬──────┘
                           │
          ┌────────────────┼────────────────┐
          │                │                │
    ┌─────▼─────┐   ┌─────▼─────┐   ┌─────▼─────┐
    │  TX Ring   │   │Completion │   │  RX Ring   │   ┌──────────┐
    │ user→kern  │   │ kern→user │   │ kern→user  │   │Fill Ring │
    │ (AfXdpTx)  │   │ (AfXdpTx) │   │ (AfXdpRx)  │   │user→kern │
    └────────────┘   └───────────┘   └────────────┘   │(AfXdpRx) │
                                                       └──────────┘
```

## Key Class APIs

### AfXdpSocket (cold path — construction only)

```cpp
struct XdpConfig {
    const char* interface;
    uint32_t queueId     = 0;
    uint32_t frameSize   = 4096;  // UMEM frame size
    uint32_t frameCount  = 64;    // total frames (split TX/RX)
    uint16_t etherType   = 0x88B5;
    bool     zeroCopy    = false;
    bool     needWakeup  = true;
};

class AfXdpSocket {
public:
    explicit AfXdpSocket(const XdpConfig& cfg);
    ~AfXdpSocket();  // detaches XDP program, unmaps, closes
    // Move-only
    // Exposes: fd, umemBase, ring pointers, offsets, frameSize, etc.
};
```

**Constructor steps:**
1. `socket(AF_XDP, SOCK_RAW, 0)`
2. `mmap(MAP_ANONYMOUS | MAP_POPULATE)` for UMEM
3. `setsockopt(XDP_UMEM_REG)` — register UMEM
4. `setsockopt(XDP_TX_RING, XDP_RX_RING, XDP_UMEM_FILL_RING, XDP_UMEM_COMPLETION_RING)`
5. `getsockopt(XDP_MMAP_OFFSETS)` → mmap each ring
6. Load embedded BPF object via libbpf, set `target_ethertype`
7. `bpf_xdp_attach()` (idempotent — detect if already attached)
8. `bpf_map_update_elem(xsks_map, queueId, fd)`
9. `bind(fd, sockaddr_xdp{AF_XDP, ifindex, queueId})`

### AfXdpTx (hot path — inline in header)

```cpp
class AfXdpTx {
public:
    explicit AfXdpTx(AfXdpSocket& sock);  // wires TX + Completion ring pointers
    uint8_t* acquire(uint32_t frameLen) noexcept;  // pop from free stack
    void commit() noexcept;                         // push to TX ring, sendto()
    bool send(std::span<const uint8_t>) noexcept;   // acquire + memcpy + commit
    void prefillRing(std::span<const uint8_t>) noexcept; // stamp all UMEM TX frames

private:
    void reclaimCompleted() noexcept;  // drain Completion ring → free stack
    // Hot members: umemBase, txDescs, txProducer, freeStack, fd, ringMask (~64 bytes)
};
```

**Key difference from PacketMmapTx:** Uses a **free-stack** instead of a simple ring cursor, because the Completion ring returns frames out-of-order. `acquire()` calls `reclaimCompleted()` first to refill the free stack.

### AfXdpRx (hot path — inline in header)

```cpp
class AfXdpRx {
public:
    explicit AfXdpRx(AfXdpSocket& sock);  // wires RX + Fill ring, pre-fills Fill ring
    RxFrame tryReceive() noexcept;        // check RX ring producer
    void release() noexcept;              // return frame addr to Fill ring

private:
    // Hot members: umemBase, rxDescs, rxProducer, fillDescs, fillProducer (~60 bytes)
};
```

**Key difference from PacketMmapRx:** `release()` puts the frame address back on the **Fill ring** (so kernel can reuse it), instead of writing `TP_STATUS_KERNEL`. If the Fill ring empties, the kernel drops packets.

## C++20 Concepts — `src/RingConcepts.hpp`

```cpp
template<typename T>
concept TxRing = requires(T t, std::span<const uint8_t> f, uint32_t len) {
    { t.send(f) } noexcept -> std::same_as<bool>;
    { t.acquire(len) } noexcept -> std::same_as<uint8_t*>;
    { t.commit() } noexcept;
    { t.prefillRing(f) } noexcept;
};

template<typename T>
concept RxRing = requires(T t) {
    { t.tryReceive() } noexcept -> std::same_as<RxFrame>;
    { t.release() } noexcept;
};

// Verified at compile time:
// static_assert(TxRing<PacketMmapTx> && TxRing<AfXdpTx>);
// static_assert(RxRing<PacketMmapRx> && RxRing<AfXdpRx>);
```

## Test Code Changes

### TOML config addition
```toml
[general]
transport    = "packet_mmap"    # or "af_xdp"
# ... existing fields ...

[af_xdp]
queue_id     = 0
zero_copy    = false
need_wakeup  = true
```

### Templated Server/Client
```cpp
// Server.hpp — templates on concept, TX/RX passed by reference
template<TxRing Tx, RxRing Rx>
void run_server(Tx& tx, Rx& rx, const TestConfig& cfg, std::stop_token stop);

// Client.hpp — same pattern
template<TxRing Tx, RxRing Rx>
void run_client(Tx& tx, Rx& rx, const TestConfig& cfg, uint32_t count, std::stop_token stop);
```

### main.cpp dispatch
```cpp
if (cfg.transport == Transport::PacketMmap) {
    PacketMmapTx tx(tx_cfg);  PacketMmapRx rx(rx_cfg);
    if (is_server) run_server(tx, rx, cfg, stop);
    else           run_client(tx, rx, cfg, count, stop);
}
#ifdef ABTRDA3_HAS_AF_XDP
else {
    AfXdpSocket sock(xdp_cfg);
    AfXdpTx tx(sock);  AfXdpRx rx(sock);
    if (is_server) run_server(tx, rx, cfg, stop);
    else           run_client(tx, rx, cfg, count, stop);
}
#endif
```

## Implementation Phases

### Phase 1: Infrastructure (no functional change)
1. Extract `RxFrame` → `src/RxFrame.hpp`, update includes
2. Create `src/RingConcepts.hpp` with concepts + static_asserts
3. Template `test/Server.hpp` and `test/Client.hpp`
4. Update `test/main.cpp` to construct TX/RX and pass to templates
5. **Verify:** existing packet_mmap test still builds and works

### Phase 2: CMake + BPF pipeline
1. Create `cmake/BpfCompile.cmake`, `cmake/BinToHeader.cmake`, `cmake/BuildLibbpf.cmake`
2. Add `ABTRDA3_ENABLE_AF_XDP` option to root `CMakeLists.txt`
3. Add FetchContent(libbpf v1.5.0) + BPF compile step to `src/CMakeLists.txt`
4. Create `src/bpf/xdp_filter.bpf.c`
5. **Verify:** BPF program compiles and embeds (`xdp_filter_embed.h` generated)

### Phase 3: AF_XDP core classes
1. Create `AfXdpSocket.hpp/.cpp` — UMEM + socket + BPF lifecycle
2. Create `AfXdpTx.hpp/.cpp` — TX ring + Completion ring + free stack
3. Create `AfXdpRx.hpp/.cpp` — RX ring + Fill ring
4. Add sources + libbpf link to `src/CMakeLists.txt`
5. **Verify:** library builds with `ABTRDA3_ENABLE_AF_XDP=ON`

### Phase 4: Test integration
1. Update `test/TestConfig.hpp` — Transport enum, AfXdpTestConfig
2. Update `test/abtrda3_test.toml` — add transport + [af_xdp] section
3. Update `test/main.cpp` — runtime dispatch
4. **Verify:** `transport = "packet_mmap"` still works (regression)
5. **Verify:** `transport = "af_xdp"` on eno2 between mkdev16/mkdev30

## Build Dependencies

| Dependency | Source | Available on build machine? |
|-----------|--------|---------------------------|
| libbpf v1.5.0 | FetchContent (GitHub) | N/A — fetched at build time |
| libelf | System (`/usr/lib64/libelf.so`) | Yes |
| zlib | System (`/usr/lib64/libz.so`) | Yes |
| clang (BPF target) | System (`/bin/clang` v19.1.7) | Yes |
| linux/if_xdp.h | System (`/usr/include/linux/`) | Yes |

## Risks and Mitigations

1. **NFS crash from bad XDP filter** — Mitigated by default `XDP_PASS`, no `XDP_DROP`, EtherType-only match, `XDP_PASS` fallback on redirect failure
2. **libbpf CMake build fragility** — Pin to v1.5.0 tag, list exact source files
3. **Cross-compile BPF** — BPF bytecode is arch-independent; always compiled with host clang
4. **Kernel 5.10 features** — Avoid newer AF_XDP features (multi-buffer, tx metadata). `XDP_USE_NEED_WAKEUP` supported since 5.4
5. **igb driver XDP support** — Intel igb supports native XDP since kernel 5.3; verified on target
