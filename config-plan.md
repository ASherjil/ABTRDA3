# ABTRDA3 & ABTWREN — TOML Config File Plan

## Design Principle

**Libraries define structs. Applications own config files.**

The core ABTRDA3 library (`src/`) has **zero dependency on TOML++**. It only defines plain C++ structs (`RingConfig`, and in the future `TransportConfig` when AF_XDP lands). Applications that use ABTRDA3 — whether ABTWREN, the ping-pong test tool, or anything else — each parse their own TOML config file and populate those structs.

```
┌─────────────────────────────────────────────┐
│ ABTRDA3 core library (src/)                 │
│                                             │
│  RingConfig {                               │
│    interface, direction, blockSize,         │
│    blockNumber, protocol, packetVersion,    │
│    hwTimeStamp, qdiscBypass                 │
│  }                                          │
│                                             │
│  PacketMmapTx, PacketMmapRx, NicTuner,     │
│  SocketOps                                  │
│                                             │
│  *** NO TOML dependency ***                 │
└───────────────┬─────────────────────────────┘
                │ populates struct
        ┌───────┴───────┐
        │               │
        ▼               ▼
┌──────────────┐  ┌──────────────────┐
│ test tool    │  │ ABTWREN          │
│ (test/)      │  │ (separate repo)  │
│              │  │                  │
│ Parses its   │  │ Parses its own   │
│ own TOML     │  │ abtwren.toml     │
│              │  │                  │
│ toml++ dep   │  │ toml++ dep       │
│ lives HERE   │  │ lives HERE       │
└──────────────┘  └──────────────────┘
```

## What Changes in ABTRDA3

### Core library (`src/`) — NO changes needed

`RingConfig` already exists in `SocketOps.hpp` and is a plain struct. Applications already create it by hand. Nothing to change here.

### Test tool (`test/main.cpp`) — Add TOML config

The test tool currently has all values hardcoded:

```cpp
// Current hardcoded values in test/main.cpp:
constexpr const char*   kInterface   = "eno2";
constexpr int           kCpuCore     = 4;
constexpr std::uint16_t kEtherType   = 0x88B5;
constexpr std::array<std::uint8_t, 6> kFecA_MAC = {0xd4, 0xf5, 0x27, 0x2a, 0xa9, 0x59};
constexpr std::array<std::uint8_t, 6> kFecB_MAC = {0x20, 0x87, 0x56, 0xb6, 0x33, 0x67};
constexpr std::uint32_t kBlockSize   = 4096;
constexpr std::uint32_t kBlockNumber = 64;
```

These move into `test/abtrda3_test.toml`:

```toml
[network]
interface    = "eno2"
ether_type   = 0x88B5
block_size   = 4096
block_number = 64
frame_size   = 64
cpu_core     = 4

[network.fec_a]
mac = "d4:f5:27:2a:a9:59"

[network.fec_b]
mac = "20:87:56:b6:33:67"
```

### CMake changes

toml++ is added only to the **test target**, not to the core library:

```cmake
# In test/CMakeLists.txt (not src/CMakeLists.txt!)
FetchContent_Declare(
    tomlplusplus
    GIT_REPOSITORY https://github.com/marzer/tomlplusplus.git
    GIT_TAG        v3.4.0
)
FetchContent_MakeAvailable(tomlplusplus)

add_executable(abtrda3_test main.cpp)
target_link_libraries(abtrda3_test PRIVATE abtrda3 abtrda3_flags tomlplusplus::tomlplusplus)
```

The `abtrda3` static library target in `src/CMakeLists.txt` remains untouched — no TOML link.

### Test tool config parsing

A small `loadConfig()` function in `test/main.cpp` (or a separate `test/Config.hpp` if preferred) parses the TOML and populates `RingConfig`:

```cpp
#include <toml++/toml.hpp>

struct TestConfig {
    std::string interface;
    int         cpuCore;
    std::uint16_t etherType;
    std::array<std::uint8_t, 6> fecA_MAC;
    std::array<std::uint8_t, 6> fecB_MAC;
    std::uint32_t blockSize;
    std::uint32_t blockNumber;
    std::size_t   frameSize;
};

TestConfig loadConfig(const char* path) {
    auto tbl = toml::parse_file(path);
    TestConfig cfg{};
    cfg.interface   = tbl["network"]["interface"].value_or("eno2"s);
    cfg.cpuCore     = tbl["network"]["cpu_core"].value_or(4);
    cfg.etherType   = static_cast<uint16_t>(tbl["network"]["ether_type"].value_or(0x88B5));
    cfg.blockSize   = tbl["network"]["block_size"].value_or(4096u);
    cfg.blockNumber = tbl["network"]["block_number"].value_or(64u);
    cfg.frameSize   = tbl["network"]["frame_size"].value_or(64u);
    cfg.fecA_MAC    = parseMac(tbl["network"]["fec_a"]["mac"].value_or(""s));
    cfg.fecB_MAC    = parseMac(tbl["network"]["fec_b"]["mac"].value_or(""s));
    return cfg;
}
```

Then `RingConfig` is populated from `TestConfig`:

```cpp
RingConfig rx_cfg{};
rx_cfg.interface   = cfg.interface.c_str();
rx_cfg.direction   = RingDirection::RX;
rx_cfg.blockSize   = cfg.blockSize;
rx_cfg.blockNumber = cfg.blockNumber;
rx_cfg.protocol    = cfg.etherType;
// ... etc
```

## What Changes in ABTWREN

ABTWREN does the same pattern but with a richer config file. It parses its own `abtwren.toml` and populates `RingConfig` (from ABTRDA3) plus its own application-specific structs.

### `abtwren.toml` layout

```toml
[general]
watchdog_sec = 30

[network]
interface    = "eno2"
ether_type   = 0x88B5
block_size   = 4096
block_number = 64

[network.tx_mac]
mac = "d4:f5:27:2a:a9:59"    # mkdev30 (transmitter)

[network.rx_mac]
mac = "20:87:56:b6:33:67"    # mkdev16 (receiver)

[transmitter]
cpu_core = 4

[transmitter.pcie]
vendor_id = 0x10DC
device_id = 0x0455
bar       = 1

# CTIM event subscriptions: event_id + pulser channel for 0-offset actions
[[transmitter.ctim]]
event_id   = 142
pulser_idx = 24    # PIX.AMCLO-CT

[[transmitter.ctim]]
event_id   = 143
pulser_idx = 25    # PIX.F900-CT

[[transmitter.ctim]]
event_id   = 138
pulser_idx = 26    # PI2X.F900-CT

[[transmitter.ctim]]
event_id   = 156
pulser_idx = 27    # PX.SCY-CT (Start Cycle)

# LTIM targets: event_id + slot + delay for enriched FIRE metadata
[[transmitter.ltim]]
event_id = 142
slot     = 23
delay_ns = 50_000_000     # 50ms

[[transmitter.ltim]]
event_id = 143
slot     = 20
delay_ns = 10_000_000     # 10ms

[[transmitter.ltim]]
event_id = 143
slot     = 21
delay_ns = 100_000_000    # 100ms

[[transmitter.ltim]]
event_id = 143
slot     = 22
delay_ns = 900_000_000    # 900ms

[[transmitter.ltim]]
event_id = 138
slot     = 21
delay_ns = 100_000_000    # 100ms

[[transmitter.ltim]]
event_id = 138
slot     = 22
delay_ns = 900_000_000    # 900ms

[receiver]
poller_core    = 4
processor_core = 5
queue_capacity = 4096
shm_name       = "/abtwren_events"
```

### ABTWREN `AppConfig` struct

```cpp
struct AppConfig {
    // [general]
    int watchdogSec = 30;

    // [network]
    std::string interface = "eno2";
    std::uint16_t etherType = 0x88B5;
    std::uint32_t blockSize = 4096;
    std::uint32_t blockNumber = 64;
    std::array<std::uint8_t, 6> txMac{};
    std::array<std::uint8_t, 6> rxMac{};

    // [transmitter]
    int txCore = 4;
    std::uint16_t pcieVendor = 0x10DC;
    std::uint16_t pcieDevice = 0x0455;
    int pcieBar = 1;
    std::vector<CtimTarget> ctimTargets;
    std::vector<LtimTarget> ltimTargets;

    // [receiver]
    int pollerCore = 4;
    int processorCore = 5;
    std::size_t queueCapacity = 4096;
    std::string shmName = "/abtwren_events";
};
```

### ABTWREN CMake

toml++ links to `abtwren` executable only (not to `wren_transmitter` or `wren_receiver` static libs):

```cmake
FetchContent_Declare(
    tomlplusplus
    GIT_REPOSITORY https://github.com/marzer/tomlplusplus.git
    GIT_TAG        v3.4.0
)
FetchContent_MakeAvailable(... tomlplusplus)

target_link_libraries(abtwren
    PRIVATE wren_transmitter wren_receiver abtwren_flags tomlplusplus::tomlplusplus
)
```

### CLI change

```
# Before:
sudo ./abtwren --tx
sudo ./abtwren --rx

# After:
sudo ./abtwren --tx --config /path/to/abtwren.toml
sudo ./abtwren --rx --config /path/to/abtwren.toml

# Falls back to ./abtwren.toml in CWD if --config is not given
```

## Summary

| Component | TOML dependency? | Config file | Who parses? |
|-----------|-----------------|-------------|-------------|
| ABTRDA3 core (`src/`) | No | None | N/A — just defines `RingConfig` |
| ABTRDA3 test (`test/`) | Yes | `abtrda3_test.toml` | `test/main.cpp` |
| ABTWREN executable | Yes | `abtwren.toml` | `src/main.cpp` via `AppConfig` |

The key insight: **ABTRDA3 core never touches TOML**. It exposes `RingConfig` as a plain struct. Every application that uses ABTRDA3 is responsible for filling that struct however it wants — from a TOML file, from command-line args, or from hardcoded values.
