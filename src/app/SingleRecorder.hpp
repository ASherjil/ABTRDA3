#pragma once

// SingleRecorder — single-process, OPEN-ENDED one-way latency benchmark.
//
// Port 1 (client) transmits, Port 2 (server) receives, in ONE process, so a
// single host's invariant TSC brackets every packet — no cross-machine clock
// sync. Unlike the old serial loopback (one frame in flight: send -> spin ->
// measure), this is a PIPELINED stream: an independent Tx thread fires frames
// each carrying their own send-timestamp, and an independent Rx thread timestamps
// arrival and computes latency = t_arrive - t_send from the in-band stamp. Many
// frames in flight, so the distribution reflects the offered load.
//
// THREE threads, each pinned to its OWN isolated core, all SCHED_OTHER. One
// busy-poll thread per isolated core needs no RT priority: nothing else is
// scheduled there, so SCHED_OTHER already runs ~100% — while FIFO would risk
// starving per-CPU kernel threads (vmstat/kworker) into a lockup and incurs the
// 95% RT-throttle. (Low-latency tuning guidance: prefer SCHED_OTHER + busy-poll
// on isolated cores over real-time priorities.)
//   * TxThread   (hot_path_core): acquire -> stamp seq + tsc::now() -> commit.
//   * RxThread   (rx_core):       verify MAC+ethertype, stamp arrival, anti-replay
//                                 seq gate, push (t_arrive - t_send) into the SPSC.
//   * HistThread (benchmark_recorder_core): pop SPSC, cycles->ns, record into the
//                                 HdrHistogram, dump CSV. Stops on an in-band
//                                 end-sentinel pushed by RxThread.
//
// CROSS-CORE TSC: t_send is read on the Tx core, t_arrive on the Rx core. Valid
// here because the box is single-socket with constant_tsc + nonstop_tsc — every
// core's TSC derives from one crystal and is reset-synchronised (sub-ns), so the
// cross-core delta is the true one-way latency. A MULTI-socket host would break
// this and need a PHC.
//
// COORDINATION (minimal): ONE atomic `txDone` (Tx -> Rx) plus an in-band queue
// SENTINEL (Rx -> Hist). The SPSC is FIFO, so Hist drains every real sample before
// it pops the sentinel — no second flag, no lost tail.
//
// The run is bounded by DURATION (TSC cycles), so the same binary does a 60 s
// smoke test or a 24 h soak by changing one TOML field.

#include "RingConcepts.hpp"
#include "TestConfig.hpp"
#include "TscClock.hpp"

#include <rigtorp/SPSCQueue.h>
#include <hdr/hdr_histogram.h>

#include <fmt/core.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <arpa/inet.h>
#include <pthread.h>
#include <sched.h>

// RX -> Hist end marker. A real cycle-delta of ~2^64 cycles is physically
// impossible (~years), so this value can never collide with a sample.
inline constexpr std::uint64_t kEndSentinel = ~0ULL;

// Frames discarded at the START by RxThread (NIC/link cold-start, before the Rx is
// at speed) — never pushed to the histogram, so they never skew the percentiles. At
// the boundary RxThread prints the warmup-window stats and RESETS the loss counters
// so the final report covers only the steady-state window. Each discarded frame is
// one fewer measured — add a little margin to duration_sec (e.g. 310s for ~5 min).
inline constexpr std::uint64_t kWarmupDiscard = 500'000;

// ── Frame payload layout ─────────────────────────────────────────────────────
//   [0..5] dst MAC | [6..11] src MAC | [12..13] ethertype | [14..17] seq (htonl)
//   | [18..25] tx TSC stamp (native u64). frame_size must be >= kFrameMinBytes.
inline constexpr std::size_t kOffDstMac     = 0;
inline constexpr std::size_t kOffSrcMac     = 6;
inline constexpr std::size_t kOffEtherType  = 12;
inline constexpr std::size_t kOffSeq        = 14;   // uint32, network order
inline constexpr std::size_t kOffTxStamp    = 18;   // uint64, native (same host)
inline constexpr std::size_t kFrameMinBytes = kOffTxStamp + sizeof(std::uint64_t);  // 26

// Unaligned store/load helpers. memcpy with a COMPILE-TIME-CONSTANT size is NOT a
// function call — GCC/Clang lower it to a single (un)aligned mov, i.e. the exact
// store/load you would hand-write. This is the correct, optimal idiom; a
// reinterpret_cast<T*> access is the same speed but UB (strict aliasing + unaligned).
template<typename T>
[[gnu::always_inline]] inline void storeU(std::uint8_t* p, T v) noexcept {
    std::memcpy(p, &v, sizeof(T));
}

template<typename T>
[[gnu::always_inline]] inline T loadU(const std::uint8_t* p) noexcept {
    T v;
    std::memcpy(&v, p, sizeof(T));
    return v;
}

// Pin the calling thread to `core` and set its scheduling policy. All three
// recorder threads use SCHED_OTHER (prio 0): on an isolated core a single
// busy-poll thread runs ~100% under SCHED_OTHER, while avoiding RT throttling and
// the FIFO lockup risk. Affinity is set regardless of policy.
inline void pinThread(int core, int policy = SCHED_OTHER, int prio = 0) noexcept {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(core, &set);
    pthread_setaffinity_np(pthread_self(), sizeof(set), &set);

    sched_param sp{};
    sp.sched_priority = prio;
    pthread_setschedparam(pthread_self(), policy, &sp);
}

// ── Shared cross-core surface — the ONLY state touched by more than one thread ──
struct Shared {
    rigtorp::SPSCQueue<std::uint64_t> toHist;          // producer writes, Hist reads (cycle deltas)
    std::atomic<bool>                 txDone{false};   // TX writes, RX reads (unused by the RTT client)
    std::atomic<std::uint64_t>        pushFailures{0}; // producer -> SPSC overflow tally
    const TestConfig&                 cfg;             // read-only after construction
    double                            tscHz{0.0};      // calibrated ONCE before threads start

    explicit Shared(const TestConfig& c) : toHist(c.recQueueCapacity), cfg(c) {}
};

// ── Sequence gate ────────────────────────────────────────────────────────────
// Each frame carries a monotonically increasing seq. Per frame:
//   diff == 1  -> in order                       (record)
//   diff  > 1  -> a gap: diff-1 frames were lost  (record this one, count the loss)
//   diff <= 0  -> at-or-behind the last accepted  (DROP, do not record):
//                 stale echo / duplicate / reordered frame
// `diff` is the SIGNED distance seq-last. The uint32 seq WRAPS at line rate
// (~every 2 min at 25G), so a plain `seq > last` test would misread a post-wrap gap
// as "behind" and stall the stream. The subtraction wraps in uint32, and the int32
// cast (two's-complement, well-defined in C++20) maps slightly-ahead -> small +ve
// and behind -> -ve — wrap-safe with a single comparison, no magic constant.
struct AntiReplay {
    std::uint32_t last{0};
    bool          have{false};

    bool accept(std::uint32_t seq, std::uint64_t& lost, std::uint64_t& dropped) noexcept {
        if (!have) {
            have = true;
            last = seq;
            return true;                                // first frame sets the baseline
        }
        const std::int32_t diff = static_cast<std::int32_t>(seq - last);
        if (diff <= 0) {
            ++dropped;                                  // stale echo / duplicate / reorder
            return false;
        }
        lost += static_cast<std::uint64_t>(diff - 1);   // 0 if in order, else the gap size
        last = seq;
        return true;
    }
};

// ── Tx: stream frames, each carrying seq + its own send timestamp ────────────
template<TxRing Tx>
class TxThread {
public:
    TxThread(Shared& sh, Tx& tx) : m_sh(sh), m_tx(tx) {}

    void run(std::uint64_t durationCyc, std::stop_token stop) {
        const TestConfig& cfg = m_sh.cfg;

        // Guard a sane frame size (>= 14B hdr + 4B seq + 8B stamp). Also gives GCC
        // the range fact that silences a -Wstringop-overflow false positive on the
        // &tmpl[kOffSrcMac] write below (it otherwise assumes frameSize may be 0).
        if (cfg.frameSize < kFrameMinBytes) {
            fmt::print(stderr, "[SingleRec/Tx] frame_size {} < {} — aborting\n",
                       cfg.frameSize, kFrameMinBytes);
            m_sh.txDone.store(true, std::memory_order_release);
            return;
        }

        // Prefill the TX ring's ethernet header so the hot path only writes the
        // sequence number and the timestamp.
        std::vector<std::uint8_t> tmpl(cfg.frameSize, 0);
        std::memcpy(&tmpl[kOffDstMac], cfg.server.mac.data(), 6);   // dst = Rx port
        std::memcpy(&tmpl[kOffSrcMac], cfg.client.mac.data(), 6);   // src = Tx port
        tmpl[kOffEtherType]     = static_cast<std::uint8_t>(cfg.etherType >> 8);
        tmpl[kOffEtherType + 1] = static_cast<std::uint8_t>(cfg.etherType & 0xFF);
        m_tx.prefillRing(tmpl);

        const std::uint64_t intervalNs = static_cast<std::uint64_t>(cfg.sendIntervalUs) * 1000ULL;
        // Split the (constant) interval into whole seconds + remainder once, so the
        // per-send timespec carry is O(1) and correct for intervals >= 1s.
        const long          paceSec    = static_cast<long>(intervalNs / 1'000'000'000ULL);
        const long          paceNsec   = static_cast<long>(intervalNs % 1'000'000'000ULL);
        timespec nextSend{};
        if (intervalNs > 0)
            clock_gettime(CLOCK_MONOTONIC, &nextSend);

        std::uint32_t       seq   = 0;
        const std::uint64_t start = tsc::now();

        while (tsc::now() - start < durationCyc && !stop.stop_requested()) {
            // Optional pacing (send_interval_us). 0 => max rate (back-to-back).
            if (intervalNs > 0) {
                nextSend.tv_sec  += paceSec;
                nextSend.tv_nsec += paceNsec;
                if (nextSend.tv_nsec >= 1'000'000'000L) {   // both addends < 1e9 => at most one carry
                    nextSend.tv_sec  += 1;
                    nextSend.tv_nsec -= 1'000'000'000L;
                }
                clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &nextSend, nullptr);
            }

            std::uint8_t* dst = m_tx.acquire(cfg.frameSize);
            if (!dst) [[unlikely]]
                continue;

            storeU<std::uint32_t>(dst + kOffSeq, htonl(seq));   // seq, network order

            const std::uint64_t t0 = tsc::now();               // send timestamp (in-band)
            storeU<std::uint64_t>(dst + kOffTxStamp, t0);       // native order (same host)
            m_tx.commit();
            ++seq;
        }

        m_sh.txDone.store(true, std::memory_order_release);
        fmt::print(stderr, "[SingleRec/Tx] sent={}\n", seq);
    }

private:
    Shared& m_sh;
    Tx&     m_tx;
};

// ── Rx: verify, stamp arrival, compute latency, push to the SPSC ────────────
template<RxRing Rx>
class RxThread {
public:
    RxThread(Shared& sh, Rx& rx) : m_sh(sh), m_rx(rx) {}

    void run() {
        const TestConfig& cfg = m_sh.cfg;

        // Expected 14-byte header (dst MAC + src MAC + ethertype), verified IN FULL
        // per frame via two OVERLAPPING 8-byte loads: offset 0 covers bytes 0..7,
        // offset 6 covers bytes 6..13 — together the whole header (bytes 6..7 twice,
        // harmless). Branchless, two GPR loads, no memcmp, no vector unit. Pad to 16
        // so the offset-6 load reads 8 valid bytes.
        std::array<std::uint8_t, 16> hdr = {0};
        std::memcpy(&hdr[kOffDstMac], cfg.server.mac.data(), 6);
        std::memcpy(&hdr[kOffSrcMac], cfg.client.mac.data(), 6);
        hdr[kOffEtherType]     = static_cast<std::uint8_t>(cfg.etherType >> 8);
        hdr[kOffEtherType + 1] = static_cast<std::uint8_t>(cfg.etherType & 0xFF);
        const std::uint64_t expHdrLo = loadU<std::uint64_t>(hdr.data() + kOffDstMac);   // bytes 0..7
        const std::uint64_t expHdrHi = loadU<std::uint64_t>(hdr.data() + kOffSrcMac);   // bytes 6..13

        AntiReplay    gate;
        std::uint64_t recorded   = 0;
        std::uint64_t lost       = 0;
        std::uint64_t dropped    = 0;
        std::uint32_t graceEmpty = 0;

        while (true) {
            RxFrame f = m_rx.tryReceive();
            if (f.data.empty()) {
                // After Tx is done, stop once the ring has stayed empty for a
                // bounded grace window (the last in-flight frames have arrived).
                if (m_sh.txDone.load(std::memory_order_acquire)) {
                    if (++graceEmpty >= kGraceEmptyPolls)
                        break;
                }
                continue;
            }
            graceEmpty = 0;

            const std::uint64_t t1 = tsc::now();           // arrival timestamp

            const std::uint8_t* packet = f.data.data();

            // Runt guard: a stray sub-26B frame must not let the stamp load read
            // past the buffer. Our frames are >= 64, so this is never taken.
            if (f.data.size() < kFrameMinBytes) [[unlikely]] {
                m_rx.release();
                continue;
            }

            // Branchless FULL-header check (dst MAC + src MAC + ethertype) via two
            // overlapping 8-byte loads. One OR, one test, no branch on the hot path.
            const std::uint64_t hdrLo = loadU<std::uint64_t>(packet + kOffDstMac);   // bytes 0..7
            const std::uint64_t hdrHi = loadU<std::uint64_t>(packet + kOffSrcMac);   // bytes 6..13
            if (((hdrLo ^ expHdrLo) | (hdrHi ^ expHdrHi)) != 0) [[unlikely]] {
                m_rx.release();                            // not ours — drop
                continue;
            }

            const std::uint32_t seqNet = loadU<std::uint32_t>(packet + kOffSeq);
            const std::uint64_t t0     = loadU<std::uint64_t>(packet + kOffTxStamp);
            m_rx.release();                                 // done with the buffer

            const std::uint32_t seq = ntohl(seqNet);
            if (!gate.accept(seq, lost, dropped)) {
                continue;                                   // stale / dup / reorder — not recorded
            }

            if (inWarmup(lost, dropped)) {
                continue;
            }

            if (!m_sh.toHist.try_push(t1 - t0)) [[unlikely]] {
                m_sh.pushFailures.fetch_add(1, std::memory_order_relaxed);
            }
            ++recorded;
        }

        // In-band end marker. The SPSC is FIFO, so Hist pops every real sample
        // before this — spin until it fits (Hist is draining, space frees).
        while (!m_sh.toHist.try_push(kEndSentinel));

        fmt::print(stderr,
                   "[SingleRec/Rx] recorded={} lost={} dropped={} push_fail={}\n",
                   recorded, lost, dropped,
                   m_sh.pushFailures.load(std::memory_order_relaxed));
    }

private:
    [[gnu::always_inline, gnu::hot]]
    inline bool inWarmup(std::uint64_t& lost, std::uint64_t& dropped) noexcept {
        if (m_warmupSeen >= kWarmupDiscard) {
            return false;
        }
        if (++m_warmupSeen == kWarmupDiscard) {
            fmt::print(stderr,
                "[SingleRec/Rx] warmup done — recorded={} lost={} dropped={} push_fail={} (counters reset)\n",
                m_warmupSeen, lost, dropped,
                m_sh.pushFailures.load(std::memory_order_relaxed));
            lost    = 0;
            dropped = 0;
            m_sh.pushFailures.store(0, std::memory_order_relaxed);
        }
        return true;
    }

    // Empty polls after txDone before declaring the stream drained (~tens of ms).
    static constexpr std::uint32_t kGraceEmptyPolls = 1U << 20;

    Shared&       m_sh;
    Rx&           m_rx;
    std::uint64_t m_warmupSeen = 0;   // accepted frames seen during warmup (caps at kWarmupDiscard)
};

// ── Hist: drain the SPSC into an HdrHistogram, then report ───────────────────
class HistThread {
public:
    // `title` labels the report ("One-Way", "Round-Trip"). When `reportOneWayHalf`
    // is set (RTT mode) the report also prints the RTT/2 one-way estimate.
    HistThread(Shared& sh, std::string outputPath, std::uint64_t durationSec,
               std::string title = "One-Way", bool reportOneWayHalf = false)
        : m_sh(sh), m_outputPath(std::move(outputPath)), m_durationSec(durationSec),
          m_title(std::move(title)), m_reportOneWayHalf(reportOneWayHalf) {}

    void run(std::stop_token stop) {
        const double tscHz = m_sh.tscHz;   // calibrated once by Bench, shared read-only

        // Record raw TSC CYCLE deltas (not truncated ns) so the histogram keeps the
        // full hardware resolution — 1 cycle ~= 0.4 ns at this TSC — and cycles->ns
        // happens once per statistic at report time. Range = 1 cycle .. 60 s of
        // cycles, 5 significant figures (HdrHistogram's max: 0.001% relative error).
        hdr_histogram* h = nullptr;
        const std::int64_t maxCyc = static_cast<std::int64_t>(tscHz * 60.0);
        if (hdr_init(1, maxCyc, 5, &h) != 0 || !h) {
            fmt::print(stderr, "[SingleRec/Hist] hdr_init failed\n");
            return;
        }

        std::uint64_t       recorded  = 0;
        const std::uint64_t startTsc = tsc::now();
        const std::uint64_t hbCyc    = static_cast<std::uint64_t>(tscHz) * 600;   // 10 min
        std::uint64_t       nextHb   = hbCyc;

        while (true) {
            // 10-minute heartbeat (off the hot path).
            const std::uint64_t el = tsc::now() - startTsc;
            if (el >= nextHb) [[unlikely]] {
                const double elapsedMin = static_cast<double>(el) / tscHz / 60.0;
                const double remainMin  = static_cast<double>(m_durationSec) / 60.0 - elapsedMin;
                fmt::println(stderr,
                    "[SingleRec/Hist] heartbeat: ~{:.0f} min elapsed, ~{:.0f} min remaining, recorded={}",
                    elapsedMin, remainMin < 0.0 ? 0.0 : remainMin, recorded);
                nextHb += hbCyc;
            }

            std::uint64_t* cyclePtr = m_sh.toHist.front();
            if (!cyclePtr) {
                if (stop.stop_requested()) {
                    break;                                  // SIGINT backstop
                }
                continue;
            }
            const std::uint64_t cyc = *cyclePtr;
            m_sh.toHist.pop();
            if (cyc == kEndSentinel) {
                break;                                      // RX signalled end-of-stream
            }

            // Warmup discard lives in RxThread (it skips pushing the cold-start
            // frames), so every sample delivered here is already steady-state.
            hdr_record_value(h, static_cast<std::int64_t>(cyc));   // raw cycles
            ++recorded;
        }

        report(h, recorded, tscHz);
        hdr_close(h);
    }

private:
    void report(hdr_histogram* h, std::uint64_t recorded, double tscHz) {
        // Histogram values are TSC cycles; convert to MICROSECONDS for display.
        // 3 decimal places (ns) preserves the full hardware resolution (1 cycle ~0.4 ns).
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

        if (m_outputPath.empty())
            return;

        // Percentile-distribution CSV — upload to hdrhistogram.github.io plotFiles
        // or feed to gnuplot. Dense in the TAIL. Values are cycles; scale by
        // cycles-per-us so the Value column comes out in microseconds.
        std::FILE* f = std::fopen(m_outputPath.c_str(), "w");
        if (!f) {
            fmt::print(stderr, "[SingleRec/Hist] cannot open {}: {}\n",
                       m_outputPath, std::strerror(errno));
            return;
        }
        const double cyclesPerUs = tscHz / 1e6;
        hdr_percentiles_print(h, f, 5, cyclesPerUs, CSV);   // 5 ticks/half, cycles -> us
        std::fclose(f);
        fmt::print("[SingleRec/Hist] wrote percentile CSV to {}\n", m_outputPath);

        // Full per-bucket histogram (value_us,count) for a latency(x)-vs-frequency(y)
        // plot — dense in the BODY, which the percentile export cannot reconstruct.
        // Bucket values converted cycles -> us; 4 decimals (0.1 ns) preserves the
        // native bucket resolution.
        // Derive the histogram name from the base (strip one trailing ".csv") so an
        // output_path of "foo.csv" yields "foo.hist.csv", not "foo.csv.hist.csv".
        std::string histPath = m_outputPath;
        if (histPath.size() >= 4 && histPath.compare(histPath.size() - 4, 4, ".csv") == 0)
            histPath.resize(histPath.size() - 4);
        histPath += ".hist.csv";
        if (std::FILE* hf = std::fopen(histPath.c_str(), "w")) {
            std::fprintf(hf, "value_us,count\n");
            hdr_iter it;
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

    Shared&       m_sh;
    std::string   m_outputPath;
    std::uint64_t m_durationSec;
    std::string   m_title;
    bool          m_reportOneWayHalf;
};

// ── Coordinator: owns the shared surface + the three threads + lifecycle ─────
template<TxRing Tx, RxRing Rx>
class Bench {
public:
    Bench(const TestConfig& cfg, Tx& tx, Rx& rx)
        : m_sh(cfg),
          m_tx(m_sh, tx),
          m_rx(m_sh, rx),
          m_hist(m_sh, cfg.outputPath, cfg.recDurationSec) {}

    void run(std::uint64_t durationSec, std::stop_token stop) {
        // Calibrate the TSC once (here, before any thread starts); both the Tx
        // duration bound and the Hist cycles->ns conversion read this rate.
        m_sh.tscHz = tsc::calibrateHz();
        const std::uint64_t durCyc = static_cast<std::uint64_t>(m_sh.tscHz) * durationSec;

        // Consumers up first; each thread self-pins to its isolated core (SCHED_OTHER).
        m_tHist = std::jthread([this, stop] {
            pinThread(m_sh.cfg.recRecorderCore);
            m_hist.run(stop);
        });
        m_tRx = std::jthread([this] {
            pinThread(m_sh.cfg.recRxCore);
            m_rx.run();
        });
        m_tTx = std::jthread([this, durCyc, stop] {
            pinThread(m_sh.cfg.recHotPathCore);
            m_tx.run(durCyc, stop);
        });

        // Pipeline shutdown order: Tx ends (duration) -> RX drains + sentinel ->
        // Hist pops sentinel + reports.
        m_tTx.join();
        m_tRx.join();
        m_tHist.join();
    }

private:
    Shared       m_sh;
    TxThread<Tx> m_tx;
    RxThread<Rx> m_rx;
    HistThread   m_hist;
    std::jthread m_tHist;
    std::jthread m_tRx;
    std::jthread m_tTx;
};
