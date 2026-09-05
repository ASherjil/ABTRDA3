// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Areeb Sherjil

#pragma once

// run_client — cross-host RTT ping-pong ORIGINATOR, DURATION-bounded.
//
// The peer (run_server) reflects every frame (MAC-swap echo). This client fires
// ONE frame at a time, spin-waits for the echo carrying the matching sequence
// number, and records the round-trip. RTT is SERIAL (one frame in flight: send
// -> spin -> measure), so it captures the true closed-loop latency — distinct
// from SingleRecorder's pipelined one-way stream.
//
// Like --single it is bounded by TIME (client.duration_sec), not a packet count,
// so a 60 s smoke test and a 24 h determinism soak differ by one TOML field. The
// histogram is the SAME machinery as --single: each RTT (a raw TSC cycle delta)
// is pushed to an SPSC queue and drained on a SEPARATE pinned core by HistThread
// into an HdrHistogram. A 24 h run at line rate would overflow any std::vector;
// the HdrHistogram is O(1) memory and keeps full sub-ns resolution.
//
// WARMUP: the first kWarmupDiscard (500k) successful round-trips are dropped
// ("warm up the old diesel engine" — cold caches, link/IRQ ramp) so they never
// skew the percentiles. Timeouts during warmup are not counted as loss.
//
// TSC timing: t1 (send) and t2 (echo) are read on the SAME pinned core, so the
// rdtscp delta is the true RTT — no cross-core skew, no clock_gettime overhead.

#include "RingConcepts.hpp"
#include "TestConfig.hpp"
#include "HistThread.hpp"       // Shared, HistThread, kEndSentinel — the recorder side
#include "SingleRecorder.hpp"   // pinThread, kWarmupDiscard, frame offsets, storeU/loadU
#include "Profiling.hpp"

#include <fmt/core.h>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <stop_token>
#include <thread>
#include <vector>

template<TxRing Tx, RxRing Rx>
inline void run_client(Tx& tx, Rx& rx, const TestConfig& cfg, std::stop_token stop) {
    if (cfg.frameSize < kFrameMinBytes) {
        fmt::print(stderr, "[Client] frame_size {} < {} — aborting\n", cfg.frameSize, kFrameMinBytes);
        return;
    }

    // Prefill the TX ring's ethernet header (hot path then writes only the seq).
    std::vector<std::uint8_t> tmpl(cfg.frameSize, 0);
    std::memcpy(&tmpl[kOffDstMac], cfg.server.mac.data(), 6);   // dst = reflector
    std::memcpy(&tmpl[kOffSrcMac], cfg.client.mac.data(), 6);   // src = us
    tmpl[kOffEtherType]     = static_cast<std::uint8_t>(cfg.etherType >> 8);
    tmpl[kOffEtherType + 1] = static_cast<std::uint8_t>(cfg.etherType & 0xFF);
    tx.prefillRing(tmpl);

    // Shared SPSC surface + histogram thread on its OWN isolated core. The hot
    // loop is the sole producer; HistThread is the sole consumer (txDone unused).
    constexpr bool kHwTs = requires (Tx& t) {
        t.hwRoundTrip();
    };
    const double tscHz = tsc::calibrateHz();
    Shared sh(cfg);
    sh.tscHz = kHwTs ? 4.0e9 : tscHz;

    HistThread hist(sh, cfg.outputPath, cfg.clientDurationSec,
                    kHwTs ? "Round-Trip (NIC hardware timestamps)" : "Round-Trip",
                    /*reportOneWayHalf=*/true);
    std::jthread tHist([&] {
        pinThread(cfg.clientRecorderCore);
        hist.run(stop);
    });

    // Pacing (send_interval_us). 0 => fire as fast as each round-trip closes.
    const std::uint64_t intervalNs = static_cast<std::uint64_t>(cfg.sendIntervalUs) * 1000ULL;
    const long          paceSec    = static_cast<long>(intervalNs / 1'000'000'000ULL);
    const long          paceNsec   = static_cast<long>(intervalNs % 1'000'000'000ULL);
    timespec nextSend{};
    if (intervalNs > 0)
        clock_gettime(CLOCK_MONOTONIC, &nextSend);

    const std::uint64_t durationCyc = static_cast<std::uint64_t>(tscHz * static_cast<double>(cfg.clientDurationSec));
    const std::uint64_t timeoutCyc  = static_cast<std::uint64_t>(tscHz * 1.0);   // 1s echo timeout

    std::uint32_t       seq      = 0;
    std::uint64_t       sent     = 0;
    std::uint64_t       recorded = 0;
    std::uint64_t       timeouts = 0;
    std::uint64_t       warmup   = 0;   // successful round-trips discarded (caps at kWarmupDiscard)
    const std::uint64_t start    = tsc::now();
    [[maybe_unused]] prof::CycleStats txStats;   // debug-only; gated, elided in release
    [[maybe_unused]] prof::CycleStats rxStats;   // debug-only; gated, elided in release


    [[maybe_unused]] double outlierUs = 2.5;
    if constexpr (prof::kDebugProfiling) {
        if (const char* e = std::getenv("ABTRDA3_OUTLIER_US")) {
            outlierUs = std::atof(e);
        }
    }
    [[maybe_unused]] const std::uint64_t outlierThreshCyc = static_cast<std::uint64_t>(sh.tscHz * outlierUs * 1e-6);
    [[maybe_unused]] int outlierLogged = 0;

    for (;;) {
        const std::uint64_t loopTsc = tsc::now();
        if (loopTsc - start >= durationCyc || stop.stop_requested()) {
            break;
        }
        // Optional drift-free pacing.
        if (intervalNs > 0) {
            nextSend.tv_sec  += paceSec;
            nextSend.tv_nsec += paceNsec;
            if (nextSend.tv_nsec >= 1'000'000'000L) {   // both addends < 1e9 => at most one carry
                nextSend.tv_sec  += 1;
                nextSend.tv_nsec -= 1'000'000'000L;
            }
            clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &nextSend, nullptr);
        }

        [[maybe_unused]] std::uint64_t txStart = 0;
        if constexpr (prof::kDebugProfiling) txStart = prof::cycles();
        std::uint8_t* dst = tx.acquire(cfg.frameSize);
        if (!dst) [[unlikely]] {
            continue;
        }

        const std::uint32_t seqNet = htonl(seq);
        storeU<std::uint32_t>(dst + kOffSeq, seqNet);
        std::uint64_t t1 = loopTsc;
        if constexpr (!kHwTs) {
            t1 = tsc::now();                          // send stamp (same core as t2)
        }
        tx.commit();
        if constexpr (prof::kDebugProfiling) {
            txStats.record(prof::cycles() - txStart);
        }
        ++sent;

        // Spin for the echo carrying OUR seq. Discard stale echoes / TX copies
        // (packet_mmap RX sees our own outgoing frames).
        bool          received = false;
        std::uint32_t spins    = 0;
        while (true) {
            if (RxFrame rxf = rx.tryReceive(); !rxf.data.empty()) [[unlikely]] {
                [[maybe_unused]] std::uint64_t t2 = 0;
                if constexpr (!kHwTs) {
                    t2 = tsc::now();                  // take the timestamp at the earliest point
                }
                [[maybe_unused]] std::uint64_t rxStart = 0;
                if constexpr (prof::kDebugProfiling) {
                    rxStart = prof::cycles();
                }
                if (rxf.data.size() >= kFrameMinBytes &&
                    std::memcmp(rxf.data.data(), cfg.client.mac.data(), 6) == 0) [[likely]] {
                    const std::uint32_t rxSeq = loadU<std::uint32_t>(rxf.data.data() + kOffSeq);
                    if (rxSeq == seqNet) [[likely]] {     // our echo
                        rx.release();
                        if (warmup < kWarmupDiscard) {
                            ++warmup;                      // cold-start: measured but not recorded
                        } else {
                            std::uint64_t sample = t2 - t1;
                            if constexpr (kHwTs) {
                                sample = tx.hwRoundTrip();
                            }
                            if (!sh.toHist.try_push(sample)) [[unlikely]] {
                                sh.pushFailures.fetch_add(1, std::memory_order_relaxed);
                            }
                            ++recorded;
                            if constexpr (prof::kDebugProfiling) {
                                if (sample > outlierThreshCyc && outlierLogged < 64) {
                                    fmt::print(stderr,
                                        "[outlier] #{:<11} t={:8.1f}ms  rtt={:7.2f}us\n",
                                        recorded,
                                        static_cast<double>(loopTsc - start) * 1e3 / tscHz,
                                        static_cast<double>(sample)         * 1e6 / sh.tscHz);
                                    ++outlierLogged;
                                }
                            }
                        }
                        if constexpr (prof::kDebugProfiling) rxStats.record(prof::cycles() - rxStart);
                        received = true;
                        break;
                    }
                }
                rx.release();                              // not ours — drop, keep spinning
            }
            if ((++spins & 0x7FF) == 0) {
                if (stop.stop_requested() || (tsc::now() - t1) >= timeoutCyc) {
                    break;
                }
            }
        }

        if (!received && warmup >= kWarmupDiscard) {
            ++timeouts;
        }
        ++seq;
    }

    // Stop the histogram thread: in-band sentinel (FIFO drains every sample first).
    while (!sh.toHist.try_push(kEndSentinel));
    tHist.join();

    if constexpr (prof::kDebugProfiling) {
        txStats.report("client tx (acq->commit)", tscHz);
        rxStats.report("client rx-process",       tscHz);
    }

    fmt::print(stderr,
               "[Client] sent={} recorded={} timeouts={} warmup_discarded={} push_fail={}\n",
               sent, recorded, timeouts, warmup,
               sh.pushFailures.load(std::memory_order_relaxed));

}
