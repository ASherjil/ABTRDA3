#pragma once

#include "PacketMmapTx.hpp"
#include "PacketMmapRx.hpp"
#include "TestConfig.hpp"

#include <cstdio>
#include <cstring>
#include <cstdint>
#include <vector>
#include <algorithm>
#include <stop_token>
#include <time.h>
#include <arpa/inet.h>

[[gnu::always_inline]]
inline std::uint64_t now_ns() noexcept {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<std::uint64_t>(ts.tv_sec) * 1'000'000'000ULL
         + static_cast<std::uint64_t>(ts.tv_nsec);
}

inline void run_client(const TestConfig& cfg, std::uint32_t count, std::stop_token stop) {
    RingConfig tx_cfg{};
    tx_cfg.interface     = cfg.client.interface.c_str();
    tx_cfg.direction     = RingDirection::TX;
    tx_cfg.blockSize     = cfg.blockSize;
    tx_cfg.blockNumber   = cfg.blockNumber;
    tx_cfg.protocol      = cfg.etherType;
    tx_cfg.packetVersion = TPACKET_V2;
    tx_cfg.qdiscBypass   = true;

    RingConfig rx_cfg{};
    rx_cfg.interface     = cfg.client.interface.c_str();
    rx_cfg.direction     = RingDirection::RX;
    rx_cfg.blockSize     = cfg.blockSize;
    rx_cfg.blockNumber   = cfg.blockNumber;
    rx_cfg.protocol      = cfg.etherType;
    rx_cfg.packetVersion = TPACKET_V2;
    rx_cfg.hwTimeStamp   = false;

    PacketMmapTx tx(tx_cfg);
    PacketMmapRx rx(rx_cfg);

    // Pre-fill ethernet header into every TX ring slot (zero-copy: hot path only writes payload)
    std::vector<std::uint8_t> frameTemplate(cfg.frameSize, 0);
    std::memcpy(&frameTemplate[0], cfg.server.mac.data(), 6);  // Dst = server
    std::memcpy(&frameTemplate[6], cfg.client.mac.data(), 6);  // Src = client
    frameTemplate[12] = static_cast<std::uint8_t>(cfg.etherType >> 8);
    frameTemplate[13] = static_cast<std::uint8_t>(cfg.etherType & 0xFF);
    tx.prefillRing(frameTemplate);

    std::vector<std::uint64_t> latencies_ns;
    latencies_ns.reserve(count);

    constexpr std::uint64_t kTimeoutNs = 1'000'000'000; // 1 second

    for (std::uint32_t i = 0; i < count && !stop.stop_requested(); ++i) {
        std::uint64_t t1 = now_ns();

        // Zero-copy: write seq number directly into TX ring slot
        auto* dst = tx.acquire(cfg.frameSize);
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
                if (rxf.data.size() >= cfg.frameSize) {
                    std::uint64_t t2 = now_ns();
                    latencies_ns.push_back(t2 - t1);
                    received = true;
                    rx.release();
                    break;
                }
                rx.release();
            }
            if ((++spins & 0xFFFF) == 0) {
                if (stop.stop_requested() || (now_ns() - t1) >= kTimeoutNs) break;
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
