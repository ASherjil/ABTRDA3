# ABTRDA3

Ultra-low-latency Ethernet timing library with two transport backends:
**packet_mmap** and **AF_XDP**.

## Build

### packet_mmap only (default)

```bash
cmake -B build/x86_64-release -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-x86_64.cmake \
  -DCMAKE_BUILD_TYPE=Release

cmake --build build/x86_64-release
```

### With AF_XDP enabled

Requires `clang` (for BPF compilation) and `libelf`/`zlib` on the build host.
`libbpf` is fetched automatically via CMake FetchContent.

```bash
cmake -B build/x86_64-release -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-x86_64.cmake \
  -DCMAKE_BUILD_TYPE=Release \
  -DABTRDA3_ENABLE_AF_XDP=ON

cmake --build build/x86_64-release
```

## Configuration

Edit `test/abtrda3_test.toml`:

```toml
[general]
transport    = "packet_mmap"   # "packet_mmap" or "af_xdp"
ether_type   = 0x88B5
frame_size   = 64
watchdog_sec = 30

# packet_mmap settings
block_size   = 4096
block_number = 64

[server]
interface = "eno2"
mac       = "d4:f5:27:2a:a9:59"
cpu_core  = 4

[client]
interface = "eno2"
mac       = "20:87:56:b6:33:67"
cpu_core  = 4

[af_xdp]
queue_id        = 0
umem_frame_size = 4096
frame_count     = 64
zero_copy       = false
need_wakeup     = true
```

## Run

The test binary is at `build/x86_64-release/test/abtrda3_test`.
Copy both the binary and the TOML config to each target machine.

### Server (reflector)

```bash
sudo taskset -c 4 ./abtrda3_test --server --config abtrda3_test.toml
```

### Client (latency measurement)

```bash
sudo taskset -c 4 ./abtrda3_test --client --count 1000 --config abtrda3_test.toml
```

Start the server first, then the client. The client sends `--count` packets
and prints min/median/max/avg RTT in microseconds.

## Switching transports

Change `transport` in the TOML file:

- `"packet_mmap"` — classic packet_mmap rings (works everywhere, no BPF)
- `"af_xdp"` — AF_XDP with XDP EtherType filter (requires `ABTRDA3_ENABLE_AF_XDP=ON` at build time and `sudo`)

No recompilation needed to switch — same binary, different config value.

## Safety (AF_XDP on NFS boot port)

The XDP filter only redirects EtherType `0x88B5` to userspace.
All other traffic (NFS, SSH, DHCP) passes through to the kernel untouched.
If the process exits or crashes, the XDP program is detached automatically.

Manual detach if needed:

```bash
sudo ip link set <interface> xdp off
```
