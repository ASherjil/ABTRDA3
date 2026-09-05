// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Areeb Sherjil

#pragma once
// =============================================================================
// Profiling.hpp — opt-in, ZERO-release-cost hot-path instrumentation.
//
// Everything gates on prof::kDebugProfiling. With it false (the PRODUCTION
// default) every call site lives inside `if constexpr (prof::kDebugProfiling)`
// and is discarded at compile time, so the hot path is byte-identical to the
// un-instrumented build (verify with objdump if paranoid). Build with
// -DABT_DEBUG_PROFILING to flip it on for a debug/measurement build.
//
// Modular by design: drop a `prof::CycleStats` next to any hot-path stage,
// `record()` the rdtsc delta, `report()` at teardown. Reused by the RTT client
// (tx / rx-process cost), the server (reflect cost), and — once wired — the
// mlx5 RX HW-timestamp decomposition. Debug numbers are NOT production numbers:
// the rdtscp brackets add a few ns to each measured RTT; that is the point (we
// are measuring CPU cost, not reporting the production latency).
// =============================================================================
#include <algorithm>
#include <cstdint>
#include <ctime>

#include <fmt/core.h>
#include <x86intrin.h>   // __rdtscp

namespace prof {

#ifdef ABT_DEBUG_PROFILING
inline constexpr bool kDebugProfiling = true;
#else
inline constexpr bool kDebugProfiling = false;
#endif

// rdtscp: serializes against PRIOR loads so a bracket can't be reordered out of
// the stage it measures. Only ever called inside if constexpr(kDebugProfiling).
[[gnu::always_inline]] inline std::uint64_t cycles() noexcept {
    unsigned aux = 0;
    return __rdtscp(&aux);
}

// One-shot TSC-Hz estimate (CLOCK_MONOTONIC vs rdtsc over ~20 ms). Debug-only,
// so the busy-wait cost is irrelevant; lets report() convert cycles -> ns. The
// RTT client already has a calibrated sh.tscHz to pass instead.
inline double tscHz() {
    timespec a{};
    timespec b{};
    clock_gettime(CLOCK_MONOTONIC, &a);
    const std::uint64_t c0 = cycles();
    do {
        clock_gettime(CLOCK_MONOTONIC, &b);
    } while ((b.tv_sec - a.tv_sec) * 1'000'000'000LL + (b.tv_nsec - a.tv_nsec) < 20'000'000LL);
    const std::uint64_t c1 = cycles();
    const double ns = static_cast<double>((b.tv_sec - a.tv_sec) * 1'000'000'000LL + (b.tv_nsec - a.tv_nsec));
    return static_cast<double>(c1 - c0) * 1e9 / ns;
}

// Streaming min / mean / max over cycle deltas. Tiny + branchless record().
struct CycleStats {
    std::uint64_t count = 0;
    std::uint64_t sum   = 0;
    std::uint64_t min   = UINT64_MAX;
    std::uint64_t max   = 0;

    [[gnu::always_inline]] inline void record(std::uint64_t cyc) noexcept {
        ++count;
        sum += cyc;
        min = cyc < min ? cyc : min;
        max = cyc > max ? cyc : max;
    }

    void report(const char* name, double hz) const {
        if (count == 0) {
            fmt::print(stderr, "[prof] {:<24} no samples\n", name);
            return;
        }
        const double nsPer = 1e9 / hz;
        fmt::print(stderr, "[prof] {:<24} n={:<11} min={:6.0f}ns  mean={:7.1f}ns  max={:9.0f}ns\n", name,
                   count, static_cast<double>(min) * nsPer,
                   (static_cast<double>(sum) / static_cast<double>(count)) * nsPer,
                   static_cast<double>(max) * nsPer);
    }
};

}   // namespace prof
