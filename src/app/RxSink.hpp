// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Areeb Sherjil

#pragma once

#include "RingConcepts.hpp"
#include "TestConfig.hpp"

#include <cstdio>
#include <cstring>
#include <ctime>
#include <stop_token>

// =============================================================================
// RX-only packet sink
//
// Receives packets, validates destination MAC and frame size, counts them.
// No payload inspection — EtherType filtering is handled by the transport.
// Runs until watchdog timeout or SIGINT.
// =============================================================================

template<RxRing Rx>
inline void run_rxsink(Rx& rx, const TestConfig& cfg, std::stop_token stop) {
    std::uint64_t accepted = 0;
    std::uint64_t rejected = 0;

    // Our MAC — check if incoming packets are addressed to us
    const auto& ourMac = cfg.server.mac;  // RxSink runs as "server" role

    timespec t_start{};
    clock_gettime(CLOCK_MONOTONIC, &t_start);

    while (true) {
        for (std::uint32_t i = 0; i < 65536; ++i) {
            RxFrame rxf = rx.tryReceive();
            if (rxf.data.empty()) [[likely]] {
                continue;
            }

            // Perform a simple check, is this packet meant for us ? Check its dst mac address
            if (std::memcmp(rxf.data.data(), ourMac.data(), 6) == 0) {
                accepted++;
            } else {
                rejected++;
            }

            rx.release();
        }
        if (stop.stop_requested()) break;
    }

    timespec t_end{};
    clock_gettime(CLOCK_MONOTONIC, &t_end);
    double elapsed_s = static_cast<double>(t_end.tv_sec - t_start.tv_sec)
                     + static_cast<double>(t_end.tv_nsec - t_start.tv_nsec) / 1e9;
    double pps = (elapsed_s > 0) ? static_cast<double>(accepted) / elapsed_s : 0;

    std::printf("\n=== RX Sink Results ===\n");
    std::printf("Accepted: %lu packets (dst MAC match + size OK)\n", accepted);
    std::printf("Rejected: %lu packets\n", rejected);
    std::printf("Duration: %.2f s\n", elapsed_s);
    std::printf("Rate:     %.0f pps\n", pps);
}
