#pragma once

#include "RingConcepts.hpp"
#include "TestConfig.hpp"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <stop_token>
#include <thread>
#include <vector>
#include <arpa/inet.h>

// =============================================================================
// TX-only traffic generator
//
// Sends packets at a configurable rate with a sequence number in the payload.
// No receive, no echo — pure unidirectional transmission.
// Use with RxSink on the other end for one-way latency / loss measurement.
// =============================================================================

[[gnu::always_inline]]
inline std::uint64_t txgen_now_ns() noexcept {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<std::uint64_t>(ts.tv_sec) * 1'000'000'000ULL
         + static_cast<std::uint64_t>(ts.tv_nsec);
}

template<TxRing Tx>
inline void run_txgen(Tx& tx, const TestConfig& cfg, std::uint32_t count, std::stop_token stop) {
    // Pre-fill ethernet header into every TX ring slot
    std::vector<std::uint8_t> frameTemplate(cfg.frameSize, 0);
    std::memcpy(&frameTemplate[0], cfg.server.mac.data(), 6);  // Dst = server
    std::memcpy(&frameTemplate[6], cfg.client.mac.data(), 6);  // Src = client
    frameTemplate[12] = static_cast<std::uint8_t>(cfg.etherType >> 8);
    frameTemplate[13] = static_cast<std::uint8_t>(cfg.etherType & 0xFF);
    tx.prefillRing(frameTemplate);

    const std::uint64_t intervalNs = static_cast<std::uint64_t>(cfg.sendIntervalUs) * 1000ULL;
    timespec nextSend{};
    if (intervalNs > 0)
        clock_gettime(CLOCK_MONOTONIC, &nextSend);

    std::uint64_t t_start = txgen_now_ns();
    std::uint32_t sent = 0;
    std::uint32_t failed = 0;

    for (std::uint32_t i = 0; i < count && !stop.stop_requested(); ++i) {
        // Pace sends if configured
        if (intervalNs > 0) {
            nextSend.tv_nsec += static_cast<long>(intervalNs);
            while (nextSend.tv_nsec >= 1'000'000'000L) {
                nextSend.tv_sec  += 1;
                nextSend.tv_nsec -= 1'000'000'000L;
            }
            clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &nextSend, nullptr);
        }

        auto* dst = tx.acquire(cfg.frameSize);
        if (!dst) [[unlikely]] {
            failed++;
            continue;
        }

        // Write sequence number at offset 14 (after Ethernet header)
        std::uint32_t seq_net = htonl(i);
        std::memcpy(dst + 14, &seq_net, sizeof(seq_net));

        // Write TX timestamp at offset 18 (for one-way latency with RxSink)
        std::uint64_t ts_ns = txgen_now_ns();
        std::memcpy(dst + 18, &ts_ns, sizeof(ts_ns));

        tx.commit();
        sent++;
    }
    std::uint64_t t_end = txgen_now_ns();

    // Allow the NIC to drain its internal Tx FIFO before we return
    // and the destructor disables the transmitter.
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    double elapsed_s = static_cast<double>(t_end - t_start) / 1e9;
    double pps = (elapsed_s > 0) ? static_cast<double>(sent) / elapsed_s : 0;

    std::printf("\n=== TX Generator Results ===\n");
    std::printf("Sent:     %u packets\n", sent);
    std::printf("Failed:   %u packets (TX ring full)\n", failed);
    std::printf("Duration: %.2f s\n", elapsed_s);
    std::printf("Rate:     %.0f pps\n", pps);
}
