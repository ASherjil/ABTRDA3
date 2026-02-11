// ABTRDA3 - Ultra Low Latency Latency Test
//
// Usage:
//   Server (Reflector): sudo ./abtrda3_test --server
//   Client (Measurer):  sudo ./abtrda3_test --client --count 100
//

#include "PacketMmapTx.hpp"
#include "PacketMmapRx.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <array>
#include <vector>
#include <algorithm>
#include <csignal>
#include <time.h>
#include <sched.h>
#include <sys/mman.h>
#include <arpa/inet.h>
#include <unistd.h>

// =============================================================================
// Configuration
// =============================================================================

constexpr const char*   kInterface  = "eno2";
constexpr std::uint16_t kEtherType  = 0x88B5;   // IEEE local experimental

// FEC-A: cfc-865-mkdev30 (eno2: d4:f5:27:2a:a9:59)
constexpr std::array<std::uint8_t, 6> kFecA_MAC = {0xd4, 0xf5, 0x27, 0x2a, 0xa9, 0x59};
// FEC-B: cfc-865-mkdev16 (eno2: 20:87:56:b6:33:67)
constexpr std::array<std::uint8_t, 6> kFecB_MAC = {0x20, 0x87, 0x56, 0xb6, 0x33, 0x67};

constexpr std::uint32_t kBlockSize    = 4096;
constexpr std::uint32_t kBlockNumber  = 64;   // Keep ring small/fast
constexpr std::size_t   kFrameSize    = 64;   // Minimum ethernet frame

// =============================================================================
// Globals
// =============================================================================

static volatile std::sig_atomic_t g_running = 1;

static void sigint_handler(int) {
    g_running = 0;
}

[[gnu::always_inline]]
inline std::uint64_t now_ns() noexcept {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<std::uint64_t>(ts.tv_sec) * 1'000'000'000ULL
         + static_cast<std::uint64_t>(ts.tv_nsec);
}

#include <thread>
#include <chrono>

// ... (existing includes)

// =============================================================================
// Server (Reflector) - The "Echo" side
// =============================================================================

static void run_server() {
    std::printf("[Server] Listening on %s (Reflector Mode)\n", kInterface);
    std::printf("[Server] Auto-shutdown in 30 seconds (Safety Watchdog)...\n");

    // Watchdog thread: kills server after 30s to prevent RT lockups
    std::thread watchdog([](){
        std::this_thread::sleep_for(std::chrono::seconds(30));
        if (g_running) {
            std::fprintf(stderr, "\n[Watchdog] Timeout reached. Stopping...\n");
            g_running = 0;
        }
    });
    // Detach so we don't have to join it if we exit early (clean OS cleanup)
    watchdog.detach();

    // RX: TPACKET_V3 (Efficient polling)
    RingConfig rx_cfg{};
    rx_cfg.interface   = kInterface;
    rx_cfg.direction   = RingDirection::RX;
    rx_cfg.blockSize   = kBlockSize;
    rx_cfg.blockNumber = kBlockNumber;
    rx_cfg.protocol    = kEtherType;
    rx_cfg.hwTimeStamp = false; // Not needed for reflector

    // TX: TPACKET_V2 (Low latency send)
    RingConfig tx_cfg{};
    tx_cfg.interface   = kInterface;
    tx_cfg.direction   = RingDirection::TX;
    tx_cfg.blockSize   = kBlockSize;
    tx_cfg.blockNumber = kBlockNumber;
    tx_cfg.protocol    = kEtherType;
    tx_cfg.packetVersion = TPACKET_V2; 
    tx_cfg.qdiscBypass = true;

    PacketMmapRx rx(rx_cfg);
    PacketMmapTx tx(tx_cfg);

    std::array<std::uint8_t, kFrameSize> buffer{};
    std::uint64_t packet_count = 0;

    while (g_running) {
        RxFrame rxf = rx.tryReceive();
        
        if (!rxf.data.empty()) {
            // DEBUG: Print reception
            // std::printf("[Server] Rx packet len=%zu\n", rxf.data.size());

            // Only reflect our experimental EtherType packets of correct size
            if (rxf.data.size() >= kFrameSize) {
                // Zero-copy-ish reflection:
                // We copy from RX ring -> Stack buffer -> TX ring
                // (Direct RX->TX ring copy would be faster but requires pointer arithmetic safety)
                
                std::memcpy(buffer.data(), rxf.data.data(), kFrameSize);

                // Swap MAC addresses
                // Dst [0-5] becomes Src [6-11]
                // Src [6-11] becomes Dst [0-5]
                std::memcpy(&buffer[0], &rxf.data[6], 6);
                std::memcpy(&buffer[6], &rxf.data[0], 6);

                // Send it back immediately
                while (!tx.send(buffer) && g_running) {
                    // Spin if TX ring is full (unlikely in ping-pong)
                }
                
                packet_count++;
            }
            rx.release();
        } else {
            // Busy loop - no sleep for lowest latency
            // std::this_thread::yield(); // Optional: Uncomment to save CPU at cost of latency
        }
    }
    std::printf("\n[Server] Reflected %lu packets. Exiting.\n", packet_count);
}

// =============================================================================
// Client (Measurer) - The "Ping" side
// =============================================================================

static void run_client(std::uint32_t count) {
    std::printf("[Client] Sending %u packets to measure RTT\n", count);
    std::printf("[Client] Auto-shutdown in 30 seconds (Safety Watchdog)...\n");

    // Watchdog thread: kills client after 30s to prevent RT lockups
    std::thread watchdog([](){
        std::this_thread::sleep_for(std::chrono::seconds(30));
        if (g_running) {
            std::fprintf(stderr, "\n[Watchdog] Timeout reached. Stopping...\n");
            g_running = 0;
        }
    });
    watchdog.detach();

    // TX: TPACKET_V2
    RingConfig tx_cfg{};
    tx_cfg.interface   = kInterface;
    tx_cfg.direction   = RingDirection::TX;
    tx_cfg.blockSize   = kBlockSize;
    tx_cfg.blockNumber = kBlockNumber;
    tx_cfg.protocol    = kEtherType;
    tx_cfg.packetVersion = TPACKET_V2;
    tx_cfg.qdiscBypass = true;

    // RX: TPACKET_V2
    RingConfig rx_cfg{};
    rx_cfg.interface   = kInterface;
    rx_cfg.direction   = RingDirection::RX;
    rx_cfg.blockSize   = kBlockSize;
    rx_cfg.blockNumber = kBlockNumber;
    rx_cfg.protocol    = kEtherType;
    rx_cfg.packetVersion = TPACKET_V2;
    rx_cfg.hwTimeStamp = false; // Use software (userspace) timestamps for RTT

    PacketMmapTx tx(tx_cfg);
    PacketMmapRx rx(rx_cfg);

    std::array<std::uint8_t, kFrameSize> frame{};
    // Pre-fill invariant parts
    std::memcpy(&frame[0], kFecB_MAC.data(), 6); // Dest
    std::memcpy(&frame[6], kFecA_MAC.data(), 6); // Src
    frame[12] = static_cast<std::uint8_t>(kEtherType >> 8);
    frame[13] = static_cast<std::uint8_t>(kEtherType & 0xFF);
    // Payload can be zeros or sequence number
    
    std::vector<double> latencies_us;
    latencies_us.reserve(count);

    for (std::uint32_t i = 0; i < count && g_running; ++i) {
        // 1. Mark Start Time
        std::uint64_t t1 = now_ns();

        // 2. Send Packet
        // Embed sequence number in payload for verification (optional, bytes 14-17)
        std::uint32_t seq_net = htonl(i);
        std::memcpy(&frame[14], &seq_net, sizeof(seq_net));

        if (!tx.send(frame)) {
            std::fprintf(stderr, "TX Ring full! Skipping packet %u\n", i);
            continue;
        }

        // 3. Wait for Echo (Blocking/Polling with Timeout)
        bool received = false;
        std::uint64_t timeout_ns = 1'000'000'000; // 1 second timeout
        std::uint64_t start_wait = now_ns();

        while ((now_ns() - start_wait) < timeout_ns && g_running) {
            RxFrame rxf = rx.tryReceive();
            if (!rxf.data.empty()) {
                // Verify it's our packet (check length and/or sequence)
                if (rxf.data.size() >= kFrameSize) {
                     // 4. Mark End Time immediately
                    std::uint64_t t2 = now_ns();
                    
                    std::uint64_t rtt_ns = t2 - t1;
                    latencies_us.push_back(static_cast<double>(rtt_ns) / 1000.0);
                    received = true;
                    rx.release();
                    break; 
                }
                rx.release();
            }
        }

        if (!received) {
            std::printf("Packet %u timed out!\n", i);
        }

        // Small gap between packets to let things settle (optional)
        // usleep(1000); 
    }

    if (latencies_us.empty()) return;

    // Stats
    std::sort(latencies_us.begin(), latencies_us.end());
    double sum = 0;
    for (auto v : latencies_us) sum += v;
    
    std::printf("\n=== Latency Results (%zu samples) ===\n", latencies_us.size());
    std::printf("Min RTT:    %.2f us\n", latencies_us.front());
    std::printf("Median RTT: %.2f us\n", latencies_us[latencies_us.size()/2]);
    std::printf("Max RTT:    %.2f us\n", latencies_us.back());
    std::printf("Avg RTT:    %.2f us\n", sum / latencies_us.size());
    std::printf("-----------------------------\n");
    std::printf("Est. One-Way Latency: %.2f us\n", latencies_us[latencies_us.size()/2] / 2.0);
}

// =============================================================================
// Main
// =============================================================================

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::printf("Usage:\n  %s --server\n  %s --client --count <N>\n", argv[0], argv[0]);
        return 1;
    }

    // Set RT priority (needs root)
    if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0) {
        std::fprintf(stderr, "[Warn] mlockall failed\n");
    }
    // Optional: Set thread priority to SCHED_FIFO here if desired

    std::signal(SIGINT, sigint_handler);

    if (std::strcmp(argv[1], "--server") == 0) {
        run_server();
    } 
    else if (std::strcmp(argv[1], "--client") == 0) {
        std::uint32_t count = 10; // default
        if (argc >= 4 && std::strcmp(argv[2], "--count") == 0) {
            count = std::atoi(argv[3]);
        }
        run_client(count);
    } 
    else {
        std::printf("Unknown mode.\n");
        return 1;
    }

    return 0;
}