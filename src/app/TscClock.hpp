// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Areeb Sherjil

#pragma once

// TscClock — lowest-overhead software timestamp source for the single-process
// latency recorder. Reads the CPU's invariant TSC directly via rdtscp (~10ns,
// no syscall, no VDSO frame) instead of clock_gettime, and converts cycles->ns
// only at analysis time so the hot path is a single instruction.
//
// Why this is safe on this box (verified): constant_tsc + nonstop_tsc means the
// TSC ticks at a FIXED rate tied to the base frequency (~2.496 GHz on the
// i7-11700F), INDEPENDENT of the core P-state — so a core boosting 0.8->4.9 GHz
// does NOT change the TSC rate. The divisor is therefore constant and knowable.
// We do NOT hardcode it: calibrate() measures it once at startup against
// CLOCK_MONOTONIC, which is robust across machines and needs no root/dmesg.
//
// rdtscp (not bare rdtsc) is deliberate: it has a built-in load-fence so it
// only retires after prior instructions, bounding the measured interval
// correctly. Both timestamps are read on the SAME pinned core, so cross-core
// TSC skew does not apply.

#include <cstdint>
#include <ctime>

#include <x86intrin.h>

namespace tsc {

[[gnu::always_inline, gnu::hot]]
inline std::uint64_t now() noexcept {
    unsigned aux = 0;
    return __rdtscp(&aux);   // serializing read of the invariant TSC
}

[[gnu::always_inline]]
inline std::uint64_t mono_ns() noexcept {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<std::uint64_t>(ts.tv_sec) * 1'000'000'000ULL + static_cast<std::uint64_t>(ts.tv_nsec);
}

// Measure the TSC tick rate (Hz) over a fixed wall-clock window. Called once at
// startup on the recorder thread. ~`windowMs` of one-time cost. The returned
// value is the cycles->ns divisor used for the whole run.
[[nodiscard]] inline double calibrateHz(int windowMs = 200) noexcept {
    const std::uint64_t ns0 = mono_ns();
    const std::uint64_t c0  = now();

    const timespec sleep{.tv_sec = 0, .tv_nsec = static_cast<long>(windowMs) * 1'000'000L};
    ::nanosleep(&sleep, nullptr);

    const std::uint64_t c1  = now();
    const std::uint64_t ns1 = mono_ns();

    const double dCycles = static_cast<double>(c1 - c0);
    const double dSec    = static_cast<double>(ns1 - ns0) / 1e9;
    return dSec > 0.0 ? dCycles / dSec : 0.0;
}

// Convert a cycle delta to nanoseconds using a pre-computed tsc_hz.
[[nodiscard, gnu::always_inline]]
inline double cyclesToNs(std::uint64_t cycles, double tscHz) noexcept {
    return static_cast<double>(cycles) * 1e9 / tscHz;
}

}   // namespace tsc
