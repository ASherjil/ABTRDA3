// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Areeb Sherjil

#pragma once

#include "RingConcepts.hpp"
#include "TestConfig.hpp"
#include "HistThread.hpp"
#include "SingleRecorder.hpp"
#include "Profiling.hpp"

#include <fmt/core.h>
#include <cstdio>
#include <cstring>
#include <atomic>
#include <optional>
#include <stop_token>
#include <thread>
#include <vector>

inline constexpr bool kapplyCTPIOFence = true;

template<TxRing Tx, RxRing Rx>
inline void run_server(Tx& tx, Rx& rx, const TestConfig& cfg, std::stop_token stop) {
    std::uint64_t packet_count = 0;
    [[maybe_unused]] prof::CycleStats reflectStats;   // debug-only; gated, elided in release

    std::vector<std::uint8_t> tmpl(cfg.frameSize, 0);
    std::memcpy(&tmpl[0], cfg.client.mac.data(), 6);   // reply dst = the originator
    std::memcpy(&tmpl[6], cfg.server.mac.data(), 6);   // reply src = us
    tmpl[12] = static_cast<std::uint8_t>(cfg.etherType >> 8);
    tmpl[13] = static_cast<std::uint8_t>(cfg.etherType & 0xFF);
    tx.prefillRing(tmpl);

    constexpr bool kHwTs = requires (Tx& t) {
        t.hwTurnaround();
    };
    std::optional<Shared>       sh;
    std::optional<HistThread>   hist;
    std::optional<std::jthread> tHist;
    if constexpr (kHwTs) {
        sh.emplace(cfg);
        sh->tscHz = 4.0e9;
        hist.emplace(*sh, cfg.serverOutputPath, cfg.clientDurationSec,
                     "Tick-to-Trade (NIC hardware timestamps: RX stamp -> TX stamp)",
                     /*reportOneWayHalf=*/false);
        tHist.emplace([&] {
            pinThread(cfg.serverRecorderCore);
            hist->run(stop);
        });
        fmt::print(stderr, "[Server] tick-to-trade: NIC hardware timestamps, recorder on core {}\n",
                   cfg.serverRecorderCore);
    }

    [[maybe_unused]] std::uint64_t hwWarmup = 0;

    // Outer loop checks stop token; inner loop spins tight without atomic reads
    bool stopping = false;
    while (!stopping) {
        for (std::uint32_t i = 0; i < 65536; ++i) {
            RxFrame rxf = rx.tryReceive();
            if (rxf.data.empty()){
                continue;
            }

            // CPU-cost bracket: packet in hand -> reply posted (excludes the poll wait).
            [[maybe_unused]] std::uint64_t reflectStart = 0;
            if constexpr (prof::kDebugProfiling) {
                reflectStart = prof::cycles();
            }

            // Zero-copy: write directly into TX ring slot
            std::uint8_t* dst = tx.acquire(cfg.frameSize);
            while (!dst) {
                if (stop.stop_requested())[[unlikely]]{
                    rx.release();
                    stopping = true;
                    break;
                }
                dst = tx.acquire(cfg.frameSize);
            }
            if (stopping) [[unlikely]] {
                break;
            }

            std::memcpy(dst + 14, &rxf.data[14], 12);

            if constexpr(kapplyCTPIOFence) {
                std::atomic_thread_fence(std::memory_order_seq_cst);
            }

            rx.release();
            tx.commit();

            if constexpr (kHwTs) {
                if (hwWarmup < kWarmupDiscard) {
                    ++hwWarmup;
                } else if (!sh->toHist.try_push(tx.hwTurnaround())) [[unlikely]] {
                    sh->pushFailures.fetch_add(1, std::memory_order_relaxed);
                }
            }

            if constexpr (prof::kDebugProfiling) {
                reflectStats.record(prof::cycles() - reflectStart);
            }
            packet_count++;
        }

        if (stop.stop_requested()) {
            break;
        }
    }

    if constexpr (kHwTs) {
        while (!sh->toHist.try_push(kEndSentinel));
        tHist->join();
    }

    std::printf("\n[Server] Reflected %lu packets.\n", packet_count);
    if constexpr (prof::kDebugProfiling) {
        reflectStats.report("server reflect (rx->tx)", prof::tscHz());
    }
    if constexpr (kHwTs) {
        fmt::print(stderr, "[Server] hw_ts warmup_discarded={} push_fail={}\n",
                   hwWarmup, sh->pushFailures.load(std::memory_order_relaxed));
    }
}
