// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Areeb Sherjil

#pragma once

#include <atomic>
#include <cstdio>
#include <cstring>
#include <optional>
#include <stop_token>
#include <thread>
#include <vector>

#include <fmt/core.h>

#include "HistThread.hpp"
#include "Profiling.hpp"
#include "RingConcepts.hpp"
#include "SingleRecorder.hpp"
#include "TestConfig.hpp"

inline constexpr bool kapplyCTPIOFence = true;

template <TxRing Tx, RxRing Rx>
inline void run_server(Tx& tx, Rx& rx, const TestConfig& cfg, const std::stop_token& stop) {
    std::uint64_t                     packet_count = 0;
    [[maybe_unused]] prof::CycleStats reflectStats;   // debug-only; gated, elided in release

    std::vector<std::uint8_t> tmpl(cfg.frameSize, 0);
    std::memcpy(&tmpl[0], cfg.client.mac.data(), 6);   // reply dst = the originator
    std::memcpy(&tmpl[6], cfg.server.mac.data(), 6);   // reply src = us
    tmpl[12] = static_cast<std::uint8_t>(cfg.etherType >> 8);
    tmpl[13] = static_cast<std::uint8_t>(cfg.etherType & 0xFF);
    tx.prefillRing(tmpl);

    constexpr bool kHwTs = requires (Tx& t, Rx& r) {
        r.hwRxTimestamp();
        t.pollTxTimestamp();
    };
    std::optional<Shared>       sh;
    std::optional<HistThread>   hist;
    std::optional<std::jthread> tHist;
    if constexpr (kHwTs) {
        sh.emplace(cfg);
        sh->tscHz     = 4.0e9;
        sh->decode    = &Tx::hwTurnaround;
        sh->decodeArg = rx.hwRxTimestampCorrection();
        hist.emplace(*sh, cfg.serverHwTimestampOutputPath, cfg.clientDurationSec,
                     "Tick-to-Trade (NIC hardware timestamps: RX stamp -> TX stamp)",
                     /*reportOneWayHalf=*/false);
        tHist.emplace([&] {
            pinThread(cfg.serverHwTimestampRecorderCore);
            hist->run(stop);
        });
        fmt::print(stderr, "[Server] tick-to-trade: NIC hardware timestamps, recorder on core {}\n",
                   cfg.serverHwTimestampRecorderCore);
    }

    [[maybe_unused]] std::uint64_t hwWarmup      = 0;
    [[maybe_unused]] std::uint32_t rxInFlight    = 0;
    [[maybe_unused]] std::uint32_t txSeqInFlight = 0;

    // Outer loop checks stop token; inner loop spins tight without atomic reads
    bool stopping = false;
    while (!stopping) {
        for (std::uint32_t i = 0; i < 65536; ++i) {
            const std::span<const std::uint8_t> rxf = rx.tryReceive();
            if (rxf.empty()) {
                if constexpr (kHwTs) {
                    if (const auto txTs = tx.pollTxTimestamp(); txTs && txTs->seq == txSeqInFlight)
                        [[unlikely]] {
                        if (hwWarmup < kWarmupDiscard) {
                            ++hwWarmup;
                        } else if (!sh->toHist.try_push((static_cast<std::uint64_t>(rxInFlight) << 32) |
                                                        txTs->minor)) [[unlikely]] {
                            sh->pushFailures.fetch_add(1, std::memory_order_relaxed);
                        }
                    }
                }
                continue;
            }
            if constexpr (kHwTs) {
                rxInFlight    = rx.hwRxTimestamp();
                txSeqInFlight = static_cast<std::uint32_t>(packet_count);
            }

            // CPU-cost bracket: packet in hand -> reply posted (excludes the poll wait).
            [[maybe_unused]] std::uint64_t reflectStart = 0;
            if constexpr (prof::kDebugProfiling) {
                reflectStart = prof::cycles();
            }

            // Zero-copy: write directly into TX ring slot
            std::uint8_t* dst = tx.acquire(cfg.frameSize);
            while (!dst) {
                if (stop.stop_requested()) [[unlikely]] {
                    rx.release();
                    stopping = true;
                    break;
                }
                dst = tx.acquire(cfg.frameSize);
            }
            if (stopping) [[unlikely]] {
                break;
            }

            std::memcpy(dst + 14, rxf.data() + 14, 12);

            if constexpr (kapplyCTPIOFence) {
                std::atomic_thread_fence(std::memory_order_seq_cst);
            }

            rx.release();
            tx.commit();

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
        while (!sh->toHist.try_push(kEndSentinel))
            ;
        tHist->join();
    }

    std::printf("\n[Server] Reflected %lu packets.\n", packet_count);
    if constexpr (prof::kDebugProfiling) {
        reflectStats.report("server reflect (rx->tx)", prof::tscHz());
    }
    if constexpr (kHwTs) {
        fmt::print(stderr, "[Server] hw_ts warmup_discarded={} push_fail={}\n", hwWarmup,
                   sh->pushFailures.load(std::memory_order_relaxed));
    }
}
