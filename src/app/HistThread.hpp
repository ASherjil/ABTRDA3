// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Areeb Sherjil

#pragma once

// HistThread — the latency RECORDER, shared by every benchmark mode.
//
// It owns nothing on the hot path: producers (SingleRecorder's RxThread, or the
// RTT client's ping-pong loop) push raw TSC CYCLE deltas into an SPSC queue, and
// this thread — on its own isolated core — drains them into an HdrHistogram and
// writes the report + CSVs. Recording cycles (not truncated ns) keeps the full
// hardware resolution (~0.4 ns/cycle); the cycles->us conversion happens once per
// statistic at report time.
//
// Termination: producers push kEndSentinel when the stream ends. The SPSC is FIFO,
// so every real sample is drained before the sentinel pops — no lost tail, no
// second flag. A stop_token is the SIGINT backstop.
//
// `Shared` (below) is the ONLY cross-thread surface: the queue, the txDone flag,
// the overflow tally, and the once-calibrated TSC rate.

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>
#include <stop_token>
#include <string>
#include <utility>

#include <fmt/core.h>
#include <hdr/hdr_histogram.h>
#include <rigtorp/SPSCQueue.h>

#include "TestConfig.hpp"
#include "TscClock.hpp"

// RX -> Hist end marker. A real cycle-delta of ~2^64 cycles is physically
// impossible (~years), so this can never collide with a sample.
inline constexpr std::uint64_t kEndSentinel = ~0ULL;

// ── Shared cross-core surface — the ONLY state touched by more than one thread ──
struct Shared {
    rigtorp::SPSCQueue<std::uint64_t> toHist;            // producer writes, Hist reads (cycle deltas)
    std::atomic<bool>                 txDone{false};     // TX writes, RX reads (unused by the RTT client)
    std::atomic<std::uint64_t>        pushFailures{0};   // producer -> SPSC overflow tally
    const TestConfig&                 cfg;               // read-only after construction
    double                            tscHz{0.0};        // calibrated ONCE before threads start
    std::optional<std::uint64_t> (*decode)(std::uint64_t, std::uint32_t){nullptr};   // raw sample -> value
    std::uint32_t decodeArg{0};

    explicit Shared(const TestConfig& c)
        : toHist(c.recQueueCapacity),
          cfg(c) {
    }
};

// ── Hist: drain the SPSC into an HdrHistogram, then report ───────────────────
class HistThread {
public:
    // `title` labels the report ("One-Way", "Round-Trip"). When `reportOneWayHalf`
    // is set (RTT mode) the report also prints the RTT/2 one-way estimate.
    HistThread(Shared& sh, std::string outputPath, std::uint64_t durationSec, std::string title = "One-Way",
               bool reportOneWayHalf = false);

    void run(const std::stop_token& stop);

private:
    void report(hdr_histogram* h, std::uint64_t recorded, double tscHz);

    Shared&       m_sh;
    std::string   m_outputPath;
    std::uint64_t m_durationSec;
    std::string   m_title;
    bool          m_reportOneWayHalf;
};

// =============================================================================

inline HistThread::HistThread(Shared& sh, std::string outputPath, std::uint64_t durationSec,
                              std::string title, bool reportOneWayHalf)
    : m_sh(sh),
      m_outputPath(std::move(outputPath)),
      m_durationSec(durationSec),
      m_title(std::move(title)),
      m_reportOneWayHalf(reportOneWayHalf) {
}

inline void HistThread::run(const std::stop_token& stop) {
    const double tscHz = m_sh.tscHz;   // calibrated once by the coordinator, read-only here

    // Range = 1 cycle .. 60 s of cycles, 5 significant figures (HdrHistogram's max:
    // 0.001% relative error).
    hdr_histogram*     h      = nullptr;
    const std::int64_t maxCyc = static_cast<std::int64_t>(tscHz * 60.0);
    if (hdr_init(1, maxCyc, 5, &h) != 0 || !h) {
        fmt::print(stderr, "[SingleRec/Hist] hdr_init failed\n");
        return;
    }

    std::uint64_t       recorded = 0;
    std::uint64_t       dropped  = 0;
    const std::uint64_t startTsc = tsc::now();
    const std::uint64_t hbCyc    = static_cast<std::uint64_t>(tscHz) * 600;   // 10 min
    std::uint64_t       nextHb   = hbCyc;

    while (true) {
        // 10-minute heartbeat (off the hot path).
        const std::uint64_t el = tsc::now() - startTsc;
        if (el >= nextHb) [[unlikely]] {
            const double elapsedMin = static_cast<double>(el) / tscHz / 60.0;
            const double remainMin  = static_cast<double>(m_durationSec) / 60.0 - elapsedMin;
            fmt::println(
                stderr, "[SingleRec/Hist] heartbeat: ~{:.0f} min elapsed, ~{:.0f} min remaining, recorded={}",
                elapsedMin, remainMin < 0.0 ? 0.0 : remainMin, recorded);
            nextHb += hbCyc;
        }

        const std::uint64_t* cyclePtr = m_sh.toHist.front();
        if (!cyclePtr) {
            if (stop.stop_requested()) {
                break;   // SIGINT backstop
            }
            continue;
        }
        const std::uint64_t cyc = *cyclePtr;
        m_sh.toHist.pop();
        if (cyc == kEndSentinel) {
            break;   // producer signalled end-of-stream
        }

        // Warmup discard happens at the PRODUCER (it never pushes cold-start frames),
        // so every sample delivered here is already steady-state.
        std::uint64_t sample = cyc;
        if (m_sh.decode != nullptr) {
            const std::optional<std::uint64_t> decoded = m_sh.decode(cyc, m_sh.decodeArg);
            if (!decoded) {
                ++dropped;
                continue;
            }
            sample = *decoded;
        }
        hdr_record_value(h, static_cast<std::int64_t>(sample));
        ++recorded;
    }

    report(h, recorded, tscHz);
    if (dropped != 0) {
        fmt::println(stderr, "[SingleRec/Hist] dropped={} (rejected by the sample decoder)", dropped);
    }
    hdr_close(h);
}

inline void HistThread::report(hdr_histogram* h, std::uint64_t recorded, double tscHz) {
    // Histogram values are TSC cycles; convert to MICROSECONDS for display. 3 decimals
    // (ns) preserves the full hardware resolution (1 cycle ~0.4 ns).
    auto us = [tscHz](std::int64_t cyc) {
        return tsc::cyclesToNs(static_cast<std::uint64_t>(cyc), tscHz) / 1000.0;
    };

    fmt::print("\n=== {} Latency Results ({} samples) ===\n", m_title, recorded);
    fmt::print("Min:     {:.3f} us\n", us(hdr_min(h)));
    fmt::print("Median:  {:.3f} us\n", us(hdr_value_at_percentile(h, 50.0)));
    fmt::print("P99:     {:.3f} us\n", us(hdr_value_at_percentile(h, 99.0)));
    fmt::print("P99.9:   {:.3f} us\n", us(hdr_value_at_percentile(h, 99.9)));
    fmt::print("P99.99:  {:.3f} us\n", us(hdr_value_at_percentile(h, 99.99)));
    fmt::print("P99.999: {:.3f} us\n", us(hdr_value_at_percentile(h, 99.999)));
    fmt::print("Max:     {:.3f} us\n", us(hdr_max(h)));
    fmt::print("Mean:    {:.3f} us\n", hdr_mean(h) * 1e9 / tscHz / 1000.0);   // double cycles -> us
    if (m_reportOneWayHalf) {
        // RTT -> one-way estimate: on a symmetric link one-way ~ RTT/2.
        fmt::print("-- one-way estimate (RTT/2) --\n");
        fmt::print("Median 1-way: {:.3f} us\n", us(hdr_value_at_percentile(h, 50.0)) / 2.0);
        fmt::print("P99.9 1-way:  {:.3f} us\n", us(hdr_value_at_percentile(h, 99.9)) / 2.0);
        fmt::print("Max 1-way:    {:.3f} us\n", us(hdr_max(h)) / 2.0);
    }
    fmt::print("-----------------------------\n");

    if (m_outputPath.empty()) {
        return;
    }

    // Percentile-distribution CSV (hdrhistogram.github.io plotFiles / gnuplot) — dense
    // in the TAIL. Values are cycles; scale by cycles-per-us so Value comes out in us.
    std::FILE* f = std::fopen(m_outputPath.c_str(), "w");
    if (!f) {
        fmt::print(stderr, "[SingleRec/Hist] cannot open {}: {}\n", m_outputPath, std::strerror(errno));
        return;
    }
    const double cyclesPerUs = tscHz / 1e6;
    hdr_percentiles_print(h, f, 5, cyclesPerUs, CSV);   // 5 ticks/half, cycles -> us
    std::fclose(f);
    fmt::print("[SingleRec/Hist] wrote percentile CSV to {}\n", m_outputPath);

    // Per-bucket histogram (value_us,count) for a latency-vs-frequency plot — dense in
    // the BODY, which the percentile export cannot reconstruct. NB: on multi-billion-
    // sample soaks the percentile CSV's count column wraps; THIS file is the truth.
    // Strip one trailing ".csv" so "foo.csv" -> "foo.hist.csv", not "foo.csv.hist.csv".
    std::string histPath = m_outputPath;
    if (histPath.size() >= 4 && histPath.compare(histPath.size() - 4, 4, ".csv") == 0) {
        histPath.resize(histPath.size() - 4);
    }
    histPath += ".hist.csv";
    if (std::FILE* hf = std::fopen(histPath.c_str(), "w")) {
        std::fprintf(hf, "value_us,count\n");
        hdr_iter it{};
        hdr_iter_recorded_init(&it, h);
        while (hdr_iter_next(&it)) {
            std::fprintf(hf, "%.4f,%lld\n",
                         tsc::cyclesToNs(static_cast<std::uint64_t>(it.value), tscHz) / 1000.0,
                         static_cast<long long>(it.count));
        }
        std::fclose(hf);
        fmt::print("[SingleRec/Hist] wrote per-bucket histogram to {}\n", histPath);
    }
}
