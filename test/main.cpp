// ABTRDA3 - Ultra-Low Latency Ping-Pong Test
//
// Usage:
//   Server (Reflector): sudo taskset -c 4 ./abtrda3_test --server
//   Client (Measurer):  sudo taskset -c 4 ./abtrda3_test --client --count 1000

#include "PacketMmapTx.hpp"
#include "PacketMmapRx.hpp"
#include "NicTuner.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <cstdint>
#include <array>
#include <vector>
#include <algorithm>
#include <csignal>
#include <time.h>
#include <sched.h>
#include <pthread.h>
#include <sys/mman.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <thread>
#include <chrono>

// =============================================================================
// Configuration
// =============================================================================

constexpr const char*   kInterface  = "eno2";
constexpr int           kCpuCore    = 4;
constexpr std::uint16_t kEtherType  = 0x88B5;   // IEEE local experimental

// FEC-A: cfc-865-mkdev30 (eno2)
constexpr std::array<std::uint8_t, 6> kFecA_MAC = {0xd4, 0xf5, 0x27, 0x2a, 0xa9, 0x59};
// FEC-B: cfc-865-mkdev16 (eno2)
constexpr std::array<std::uint8_t, 6> kFecB_MAC = {0x20, 0x87, 0x56, 0xb6, 0x33, 0x67};

constexpr std::uint32_t kBlockSize   = 4096;
constexpr std::uint32_t kBlockNumber = 64;
constexpr std::size_t   kFrameSize   = 64;   // Minimum ethernet frame

// =============================================================================
// Globals
// =============================================================================

static volatile std::sig_atomic_t g_running = 1;

static void sigint_handler(int) { g_running = 0; }

[[gnu::always_inline]]
inline std::uint64_t now_ns() noexcept {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<std::uint64_t>(ts.tv_sec) * 1'000'000'000ULL
         + static_cast<std::uint64_t>(ts.tv_nsec);
}

// =============================================================================
// Watchdog — safety net for RT lockups / unreachable SSH
//
// Runs on core 0 at SCHED_OTHER (normal priority) so it can always fire
// even when the hot path busy-spins at SCHED_FIFO on another core.
// After 30s it sets g_running = 0, allowing the program to exit cleanly.
// =============================================================================

static void spawn_watchdog() {
    std::thread([](){
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET(0, &cpuset);
        pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);

        sched_param sp{};
        sp.sched_priority = 0;
        sched_setscheduler(0, SCHED_OTHER, &sp);

        std::this_thread::sleep_for(std::chrono::seconds(30));
        if (g_running) {
            std::fprintf(stderr, "\n[Watchdog] 30s timeout. Stopping...\n");
            g_running = 0;
        }
    }).detach();
}

// =============================================================================
// Server (Reflector)
// =============================================================================

static void run_server() {
    std::printf("[Server] Listening on %s (Reflector)\n", kInterface);
    std::printf("[Server] Watchdog: auto-shutdown in 30 seconds\n");
    spawn_watchdog();

    RingConfig rx_cfg{};
    rx_cfg.interface   = kInterface;
    rx_cfg.direction   = RingDirection::RX;
    rx_cfg.blockSize   = kBlockSize;
    rx_cfg.blockNumber = kBlockNumber;
    rx_cfg.protocol    = kEtherType;
    rx_cfg.hwTimeStamp = false;

    RingConfig tx_cfg{};
    tx_cfg.interface     = kInterface;
    tx_cfg.direction     = RingDirection::TX;
    tx_cfg.blockSize     = kBlockSize;
    tx_cfg.blockNumber   = kBlockNumber;
    tx_cfg.protocol      = kEtherType;
    tx_cfg.packetVersion = TPACKET_V2;
    tx_cfg.qdiscBypass   = true;

    PacketMmapRx rx(rx_cfg);
    PacketMmapTx tx(tx_cfg);

    std::uint64_t packet_count = 0;

    // Outer loop checks g_running; inner loop spins tight without volatile reads
    while (true) {
        for (std::uint32_t i = 0; i < 65536; ++i) {
            RxFrame rxf = rx.tryReceive();
            if (rxf.data.empty()) continue;

            if (rxf.data.size() >= kFrameSize) {
                // Zero-copy: write directly into TX ring slot
                auto* dst = tx.acquire(kFrameSize);
                while (!dst) {
                    if (!g_running) { rx.release(); return; }
                    dst = tx.acquire(kFrameSize);
                }

                // Swap MACs from RX ring directly into TX slot, copy payload
                std::memcpy(dst,      &rxf.data[6],  6);   // dst MAC = RX src
                std::memcpy(dst + 6,  &rxf.data[0],  6);   // src MAC = RX dst
                std::memcpy(dst + 12, &rxf.data[12], kFrameSize - 12);

                rx.release();  // free RX slot before TX syscall
                tx.commit();
                packet_count++;
            } else {
                rx.release();
            }
        }
        if (!g_running) break;
    }

    std::printf("\n[Server] Reflected %lu packets.\n", packet_count);
}

// =============================================================================
// Client (Measurer)
// =============================================================================

static void run_client(std::uint32_t count) {
    std::printf("[Client] Sending %u packets to measure RTT\n", count);
    std::printf("[Client] Watchdog: auto-shutdown in 30 seconds\n");
    spawn_watchdog();

    RingConfig tx_cfg{};
    tx_cfg.interface     = kInterface;
    tx_cfg.direction     = RingDirection::TX;
    tx_cfg.blockSize     = kBlockSize;
    tx_cfg.blockNumber   = kBlockNumber;
    tx_cfg.protocol      = kEtherType;
    tx_cfg.packetVersion = TPACKET_V2;
    tx_cfg.qdiscBypass   = true;

    RingConfig rx_cfg{};
    rx_cfg.interface     = kInterface;
    rx_cfg.direction     = RingDirection::RX;
    rx_cfg.blockSize     = kBlockSize;
    rx_cfg.blockNumber   = kBlockNumber;
    rx_cfg.protocol      = kEtherType;
    rx_cfg.packetVersion = TPACKET_V2;
    rx_cfg.hwTimeStamp   = false;

    PacketMmapTx tx(tx_cfg);
    PacketMmapRx rx(rx_cfg);

    // Pre-fill ethernet header into every TX ring slot (zero-copy: hot path only writes payload)
    std::array<std::uint8_t, kFrameSize> frameTemplate{};
    std::memcpy(&frameTemplate[0], kFecB_MAC.data(), 6);  // Dst
    std::memcpy(&frameTemplate[6], kFecA_MAC.data(), 6);  // Src
    frameTemplate[12] = static_cast<std::uint8_t>(kEtherType >> 8);
    frameTemplate[13] = static_cast<std::uint8_t>(kEtherType & 0xFF);
    tx.prefillRing(frameTemplate);

    std::vector<std::uint64_t> latencies_ns;
    latencies_ns.reserve(count);

    constexpr std::uint64_t kTimeoutNs = 1'000'000'000; // 1 second

    for (std::uint32_t i = 0; i < count && g_running; ++i) {
        std::uint64_t t1 = now_ns();

        // Zero-copy: write seq number directly into TX ring slot
        auto* dst = tx.acquire(kFrameSize);
        if (!dst) {
            std::fprintf(stderr, "TX ring full, skipping packet %u\n", i);
            continue;
        }

        // Ethernet header already in slot from prefillRing() — only write payload
        std::uint32_t seq_net = htonl(i);
        std::memcpy(dst + 14, &seq_net, sizeof(seq_net));
        tx.commit();

        // Spin-wait for echo. Check timeout every 65536 spins (~300us).
        bool received = false;
        std::uint32_t spins = 0;

        while (true) {
            RxFrame rxf = rx.tryReceive();
            if (!rxf.data.empty()) {
                if (rxf.data.size() >= kFrameSize) {
                    std::uint64_t t2 = now_ns();
                    latencies_ns.push_back(t2 - t1);
                    received = true;
                    rx.release();
                    break;
                }
                rx.release();
            }
            if ((++spins & 0xFFFF) == 0) {
                if (!g_running || (now_ns() - t1) >= kTimeoutNs) break;
            }
        }

        if (!received) {
            std::printf("Packet %u timed out!\n", i);
        }
    }

    if (latencies_ns.empty()) return;

    // Stats
    std::sort(latencies_ns.begin(), latencies_ns.end());
    std::uint64_t sum = 0;
    for (auto v : latencies_ns) sum += v;

    auto to_us = [](std::uint64_t ns) { return static_cast<double>(ns) / 1000.0; };
    std::size_t n = latencies_ns.size();

    std::printf("\n=== Latency Results (%zu samples) ===\n", n);
    std::printf("Min RTT:    %.2f us\n", to_us(latencies_ns.front()));
    std::printf("Median RTT: %.2f us\n", to_us(latencies_ns[n / 2]));
    std::printf("Max RTT:    %.2f us\n", to_us(latencies_ns.back()));
    std::printf("Avg RTT:    %.2f us\n", to_us(sum / n));
    std::printf("-----------------------------\n");
    std::printf("Est. One-Way Latency: %.2f us\n", to_us(latencies_ns[n / 2]) / 2.0);
}

// =============================================================================
// Main
// =============================================================================

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::printf("Usage:\n  %s --server\n  %s --client --count <N>\n", argv[0], argv[0]);
        return 1;
    }

    // NicTuner applies all system tuning (NAPI polling, interrupt coalescing,
    // NIC offloads, RT throttling, ksoftirqd priority, IRQ affinity) and
    // restores originals on destruction.
    NicTuner tuner(kInterface, kCpuCore);

    if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0) {
        std::fprintf(stderr, "[Warn] mlockall failed\n");
    }

    // SCHED_FIFO:49 — below ksoftirqd (FIFO:50, set by NicTuner) so NAPI
    // can still deliver packets to the mmap ring on this RT kernel.
    {
        sched_param sp{};
        sp.sched_priority = 49;
        if (sched_setscheduler(0, SCHED_FIFO, &sp) != 0) {
            std::fprintf(stderr, "[Warn] SCHED_FIFO:49 failed: %s\n", std::strerror(errno));
        }
    }

    std::signal(SIGINT, sigint_handler);

    if (std::strcmp(argv[1], "--server") == 0) {
        run_server();
    } else if (std::strcmp(argv[1], "--client") == 0) {
        std::uint32_t count = 10;
        if (argc >= 4 && std::strcmp(argv[2], "--count") == 0) {
            count = static_cast<std::uint32_t>(std::atoi(argv[3]));
        }
        run_client(count);
    } else {
        std::printf("Unknown mode.\n");
        return 1;
    }

    return 0;
}
