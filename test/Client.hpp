#pragma once

#include "RingConcepts.hpp"
#include "TestConfig.hpp"
#include "fmt/ostream.h"

#include <algorithm>
#include <arpa/inet.h>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <stop_token>
#include <vector>

[[gnu::always_inline]]
inline std::uint64_t now_ns() noexcept {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<std::uint64_t>(ts.tv_sec) * 1'000'000'000ULL
         + static_cast<std::uint64_t>(ts.tv_nsec);
}

template<TxRing Tx, RxRing Rx>
inline void run_client(Tx& tx, Rx& rx, const TestConfig& cfg, std::uint32_t count, std::stop_token stop) {
    // Pre-fill ethernet header into every TX ring slot (zero-copy: hot path only writes payload)
    std::vector<std::uint8_t> frameTemplate(cfg.frameSize, 0);
    std::memcpy(&frameTemplate[0], cfg.server.mac.data(), 6);  // Dst = server
    std::memcpy(&frameTemplate[6], cfg.client.mac.data(), 6);  // Src = client
    frameTemplate[12] = static_cast<std::uint8_t>(cfg.etherType >> 8);
    frameTemplate[13] = static_cast<std::uint8_t>(cfg.etherType & 0xFF);
    tx.prefillRing(frameTemplate);

    std::vector<std::uint64_t> latencies_ns;
    latencies_ns.reserve(count);

    constexpr std::uint64_t kTimeoutNs = 5'000'000; // 5ms

    // Pacing: use CLOCK_MONOTONIC absolute sleeps for drift-free timing
    const std::uint64_t intervalNs = static_cast<std::uint64_t>(cfg.sendIntervalUs) * 1000ULL;
    timespec nextSend{};
    if (intervalNs > 0) {
        clock_gettime(CLOCK_MONOTONIC, &nextSend);
    }

    for (std::uint32_t i = 0; i < count && !stop.stop_requested(); ++i) {
        // Pace sends if configured (e.g. 1ms to simulate CERN WR timing signals)
        if (intervalNs > 0) {
            // Advance next send time
            nextSend.tv_nsec += static_cast<long>(intervalNs);
            while (nextSend.tv_nsec >= 1'000'000'000L) {
                nextSend.tv_sec  += 1;
                nextSend.tv_nsec -= 1'000'000'000L;
            }
            clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &nextSend, nullptr);
        }

        std::uint64_t t1 = now_ns();

        // Zero-copy: write seq number directly into TX ring slot
        auto* dst = tx.acquire(cfg.frameSize);
        if (!dst)[[unlikely]] {
            std::fprintf(stderr, "TX ring full, skipping packet %u\n", i);
            continue;
        }

        // Ethernet header already in slot from prefillRing() — only write payload
        std::uint32_t seq_net = htonl(i);
        std::memcpy(dst + 14, &seq_net, sizeof(seq_net));
        tx.commit(); // send the frame

        // Spin-wait for echo with matching sequence number.
        // Discard stale echoes and outgoing copies (packet_mmap RX sees TX copies).
        bool received = false;
        std::uint32_t spins = 0;

        while (true) {
          if (RxFrame rxf = rx.tryReceive(); !rxf.data.empty()) [[unlikely]]{
                // Check 1: dst MAC — is this packet for us?
                if (std::memcmp(rxf.data.data(), cfg.client.mac.data(), 6) == 0)[[likely]]{
                    std::uint32_t rx_seq;
                    std::memcpy(&rx_seq, rxf.data.data() + 14, sizeof(rx_seq));

                    // Check 2: sequence number — prevent stale echoes being recorded
                    if (rx_seq == seq_net)[[likely]] {
                        rx.release();
                        std::uint64_t t2 = now_ns();
                        latencies_ns.push_back(t2 - t1);
                        received = true;
                        break;
                    }
                }
                rx.release();
            }
            if ((++spins & 0x7FF) == 0) {
                if (stop.stop_requested() || (now_ns() - t1) >= kTimeoutNs) break;
            }
        }

        if (!received) {
            fmt::println(stderr,"Packet {} timed out ({}ms), packet loss occurred!", i, (kTimeoutNs - intervalNs)/1'000'000);
        }
    }

    if (latencies_ns.empty()) return;

    // Stats
    std::sort(latencies_ns.begin(), latencies_ns.end());
    std::uint64_t sum = 0;
    for (auto v : latencies_ns) sum += v;

    auto to_us = [](std::uint64_t ns) { return static_cast<double>(ns) / 1000.0; };
    std::size_t n = latencies_ns.size();
    auto percentile = [&](double p) -> std::uint64_t {
        std::size_t idx = static_cast<std::size_t>(p / 100.0 * static_cast<double>(n - 1));
        return latencies_ns[idx];
    };

    std::uint64_t p50  = percentile(50.0);
    std::uint64_t p99  = percentile(99.0);
    std::uint64_t p999 = percentile(99.9);

    std::printf("\n=== Latency Results (%zu samples) ===\n", n);
    std::printf("Min RTT:    %.2f us\n", to_us(latencies_ns.front()));
    std::printf("Median RTT: %.2f us\n", to_us(p50));
    std::printf("P99 RTT:    %.2f us\n", to_us(p99));
    std::printf("P99.9 RTT:  %.2f us\n", to_us(p999));
    std::printf("Max RTT:    %.2f us\n", to_us(latencies_ns.back()));
    std::printf("-----------------------------\n");
    std::printf("Worst case latency(1-way): %.3f us\n", to_us(latencies_ns.back())/2.0);

    // Write latencies to file if configured
    const char* output_path = cfg.outputPath.empty() ? nullptr : cfg.outputPath.c_str();
    if (output_path) {
        FILE* f = std::fopen(output_path, "w");
        if (f) {
            for (auto ns : latencies_ns)
                std::fprintf(f, "%lu\n", ns);
            std::fclose(f);
            std::printf("[Client] Wrote %zu latencies to %s\n", n, output_path);
        } else {
            std::fprintf(stderr, "[Client] Failed to open %s: %s\n", output_path, std::strerror(errno));
        }
    }
}
