#pragma once

// SingleRecorder — single-process one-way latency benchmark.
//
// Port 1 transmits and Port 2 receives, both in ONE process, so a single host
// TSC brackets each packet: there is no cross-machine clock-sync problem and no
// cross-port PHC problem — t0 and t1 are read from the same core's TSC. This
// isolates the wire+delivery half of latency (NIC-TX + DMA + wire + NIC-RX +
// busy-poll RX delivery) that an RTT bundles together with peer turnaround.
//
// Two threads, each a functor (operator()), connected by a lock-free SPSC queue:
//   * LoopBack (producer): time-bounded loop. acquire->commit->send->spin-receive
//     ->verify(MAC+seq)->release, brackets with rdtscp, pushes the raw cycle
//     delta. NOTHING else touches the hot core — no allocation, no syscalls
//     beyond the transport's own, no histogram, no I/O.
//   * Recorder (consumer): pops cycle deltas, converts to ns, records into an
//     HdrHistogram (fixed ~1.5 MB regardless of sample count — the only way a
//     24 h / ~19 billion-sample run is feasible). At the end it prints the
//     percentile table and dumps a plottable CSV.
//
// The hot loop is bounded by DURATION (TSC cycles), not a sample count, so the
// same binary does a 60 s smoke test or a 24 h determinism soak by changing one
// TOML field.

#include "RingConcepts.hpp"
#include "TestConfig.hpp"
#include "TscClock.hpp"

#include <rigtorp/SPSCQueue.h>
#include <hdr/hdr_histogram.h>

#include <fmt/core.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>
#include <arpa/inet.h>

// ── Shared state between the two functors ──────────────────────────────────
struct RecorderChannel {
    rigtorp::SPSCQueue<std::uint64_t> queue;   // raw TSC cycle deltas
    std::atomic<bool>                 ready{false};  // recorder is draining
    std::atomic<bool>                 done{false};   // producer finished
    std::atomic<std::uint64_t>        pushFailures{0};

    explicit RecorderChannel(std::size_t capacity) : queue(capacity) {}
};

// ── Producer: the loopback hot path ─────────────────────────────────────────
// Owns the Tx and Rx transports (different ports). One packet in flight: send,
// then spin until the matching echo (own MAC + seq) returns, timing the round.
template<TxRing Tx, RxRing Rx>
class LoopBack {
public:
    LoopBack(Tx& tx, Rx& rx, const TestConfig& cfg, RecorderChannel& ch,
             std::uint64_t durationSec)
        : m_tx(tx), m_rx(rx), m_cfg(cfg), m_ch(ch), m_durationSec(durationSec) {}

    void operator()(std::stop_token stop) {
        // Prefill the Tx ring's ethernet header so the hot path only writes the
        // sequence number (zero-copy: header lives in the ring slot).
        std::vector<std::uint8_t> tmpl(m_cfg.frameSize, 0);
        std::memcpy(&tmpl[0], m_cfg.server.mac.data(), 6);  // dst = Rx port
        std::memcpy(&tmpl[6], m_cfg.client.mac.data(), 6);  // src = Tx port
        tmpl[12] = static_cast<std::uint8_t>(m_cfg.etherType >> 8);
        tmpl[13] = static_cast<std::uint8_t>(m_cfg.etherType & 0xFF);
        m_tx.prefillRing(tmpl);

        // Run duration in TSC cycles — the SOLE stop condition (no watchdog, no
        // per-packet timeout). On a direct one-in-flight link every packet comes
        // back, so we simply send then spin-receive until our frame arrives.
        const double        tscHz       = tsc::calibrateHz();
        const std::uint64_t durationCyc = static_cast<std::uint64_t>(tscHz)
                                        * m_durationSec;

        // Recorder starts first: wait until it is draining before we produce, so
        // no samples are generated before the consumer is ready.
        while (!m_ch.ready.load(std::memory_order_acquire) && !stop.stop_requested())
            ;

        std::uint32_t seq = 0;
        std::uint64_t sent = 0, recv = 0, lost = 0;

        // Per-round match pattern for bytes 12..17 of the frame: ethertype (2B,
        // constant) + seq (4B, rewritten each round). Together with the dst-MAC
        // compare this verifies dst MAC + ethertype + seq on every counted
        // sample — one contiguous 6-byte compare on an already-loaded cacheline.
        std::uint8_t expect[6] = {
            static_cast<std::uint8_t>(m_cfg.etherType >> 8),
            static_cast<std::uint8_t>(m_cfg.etherType & 0xFF), 0, 0, 0, 0 };

        // Warm the NIC datapath BEFORE timing starts — kick the "diesel engine".
        // Sends/receives a burst that is NOT timed and discarded, so the cold
        // first-packet cost (TX path bring-up, i40e GLQF register init, descriptor
        // rings, CPU caches) AND the documented XXV710 cold-start frame loss
        // (first ~0-63 frames) never enter the measurement. Transport-generic.
        primeDatapath(seq, stop);

        const std::uint64_t runStart    = tsc::now();   // timing starts AFTER warmup

        while (true) {
            const std::uint64_t t0 = tsc::now();
            if (t0 - runStart >= durationCyc || stop.stop_requested()) break;

            std::uint8_t* dst = m_tx.acquire(m_cfg.frameSize);
            if (!dst) [[unlikely]] { continue; }
            const std::uint32_t seqNet = htonl(seq);
            std::memcpy(dst + 14, &seqNet, sizeof(seqNet));
            std::memcpy(expect + 2, &seqNet, sizeof(seqNet));
            m_tx.commit();
            ++sent;

            // Spin until the Rx port delivers THIS frame (one-way, no reflection):
            // match dst MAC (the Rx port) + ethertype + the seq we just wrote, so
            // every counted sample is provably our own packet — never stray or
            // stale traffic. The spin is BOUNDED at kLostSpins (~seconds, five
            // orders of magnitude beyond any real frame) — a frame killed by a
            // line bit-error (25G BER 1e-15 => ~2% odds of one per 24h soak) is
            // declared LOST: tallied, NOT recorded, and the loop moves on with
            // the next seq (the late original then fails the seq match and is
            // dropped). Without the bound, one lost frame = infinite spin =
            // a dead 24h run. The same rare branch checks stop_requested so
            // SIGINT always salvages the histogram collected so far.
            std::uint32_t spins = 0;
            bool gotIt = true;
            while (true) {
                if (RxFrame f = m_rx.tryReceive(); !f.data.empty()) [[unlikely]] {
                    if (std::memcmp(f.data.data(), m_cfg.server.mac.data(), 6) == 0 &&
                        std::memcmp(f.data.data() + 12, expect, 6) == 0) [[likely]] {
                        m_rx.release();
                        const std::uint64_t t1 = tsc::now();
                        if (!m_ch.queue.try_push(t1 - t0)) [[unlikely]]
                            m_ch.pushFailures.fetch_add(1, std::memory_order_relaxed);
                        ++recv;
                        break;
                    }
                    m_rx.release();   // stale / not-ours — drop, keep spinning
                }
                if ((++spins & 0xFFFF) == 0) [[unlikely]] {
                    if (spins >= kLostSpins) { ++lost; gotIt = false; break; }
                    if (stop.stop_requested()) { gotIt = false; break; }
                }
            }
            (void)gotIt;
            ++seq;
        }

        m_ch.done.store(true, std::memory_order_release);
        fmt::print(stderr, "[LoopBack] sent={} recv={} lost={} push_fail={}\n",
                   sent, recv, lost, m_ch.pushFailures.load(std::memory_order_relaxed));
    }

private:
    // Warm the TX->RX datapath before timing — kick the NIC "diesel engine". A
    // LONG fixed burst (500k packets ~= 2.5s at ~5us/round-trip), each waited
    // UNBOUNDED (no per-packet deadline). The length is deliberate: the clean
    // dev_stop shutdown parks the PHY, so THIS run's dev_start brings the link up
    // FROM PARKED -> a cold settle plus a one-time ~4.7ms link re-sync ~85ms in.
    // A 2.5s window comfortably covers that, and the unbounded per-packet wait lets
    // the settling round-trip complete and be ABSORBED here instead of leaking into
    // the timed loop. Advances `seq` so timing starts past the cold packets. NOT
    // timed, results discarded. Transport-generic (acquire/commit + tryReceive/
    // release). Only SIGINT (stop_token) can break it.
    [[gnu::cold]]
    void primeDatapath(std::uint32_t& seq, std::stop_token& stop) {
        constexpr std::uint32_t kWarmupPackets = 500'000;   // ~2.5s; covers the ~85ms re-sync
        std::uint8_t expect[6] = {
            static_cast<std::uint8_t>(m_cfg.etherType >> 8),
            static_cast<std::uint8_t>(m_cfg.etherType & 0xFF), 0, 0, 0, 0 };
        for (std::uint32_t w = 0; w < kWarmupPackets && !stop.stop_requested(); ++w, ++seq) {
            std::uint8_t* dst = m_tx.acquire(m_cfg.frameSize);
            if (!dst) continue;
            const std::uint32_t seqNet = htonl(seq);
            std::memcpy(dst + 14, &seqNet, sizeof(seqNet));
            std::memcpy(expect + 2, &seqNet, sizeof(seqNet));
            m_tx.commit();
            std::uint32_t spins = 0;
            while (true) {
                RxFrame f = m_rx.tryReceive();
                if (!f.data.empty()) {
                    const bool ours =
                        std::memcmp(f.data.data(), m_cfg.server.mac.data(), 6) == 0 &&
                        std::memcmp(f.data.data() + 12, expect, 6) == 0;
                    m_rx.release();
                    if (ours) break;   // our echo -> next warmup packet
                }
                if ((++spins & 0xFFFF) == 0) [[unlikely]] {
                    if (spins >= kLostSpins || stop.stop_requested()) break;
                }
            }
        }
    }

    static constexpr std::uint32_t kLostSpins = 1u << 26;   // ~0.2-3s of empty polls

    Tx&               m_tx;
    Rx&               m_rx;
    const TestConfig& m_cfg;
    RecorderChannel&  m_ch;
    std::uint64_t     m_durationSec;
};

// ── Consumer: the recorder ──────────────────────────────────────────────────
// Drains the SPSC queue into an HdrHistogram. Fixed memory; every sample is
// tallied (not stored), so a 19-billion-sample run stays at ~1.5 MB. Runs on a
// SEPARATE core so its work never steals cycles from the hot path.
class Recorder {
public:
    Recorder(RecorderChannel& ch, std::string outputPath)
        : m_ch(ch), m_outputPath(std::move(outputPath)) {}

    void operator()(std::stop_token /*stop*/) {
        // 1 ns .. 60 s, 3 significant figures. ~1.5 MB, fixed.
        hdr_histogram* h = nullptr;
        if (hdr_init(1, 60'000'000'000LL, 3, &h) != 0 || !h) {
            fmt::print(stderr, "[Recorder] hdr_init failed\n");
            return;
        }
        // tsc_hz on this thread; producer calibrated its own — both read the same
        // invariant TSC, so the rate matches. Recalibrating here keeps the
        // consumer self-contained for the cycles->ns conversion.
        const double tscHz = tsc::calibrateHz();

        std::uint64_t recorded = 0;
        auto drain = [&] {
            while (std::uint64_t* cyc = m_ch.queue.front()) {
                const double ns = tsc::cyclesToNs(*cyc, tscHz);
                hdr_record_value(h, static_cast<std::int64_t>(ns));
                m_ch.queue.pop();
                ++recorded;
            }
        };

        // Signal the producer that we are calibrated and ready to drain — it waits
        // on this before sending its first packet, so no samples are produced
        // before the consumer exists.
        m_ch.ready.store(true, std::memory_order_release);

        // Spin-drain until the producer is done, then a final drain for anything
        // left in the queue after the done flag was set.
        while (!m_ch.done.load(std::memory_order_acquire))
            drain();
        drain();

        report(h, tscHz, recorded);
        hdr_close(h);
    }

private:
    void report(hdr_histogram* h, double /*tscHz*/, std::uint64_t recorded) {
        auto us = [](std::int64_t ns) { return static_cast<double>(ns) / 1000.0; };

        fmt::print("\n=== One-Way Latency Results ({} samples) ===\n", recorded);
        fmt::print("Min:    {:.3f} us\n", us(hdr_min(h)));
        fmt::print("Median: {:.3f} us\n", us(hdr_value_at_percentile(h, 50.0)));
        fmt::print("P99:    {:.3f} us\n", us(hdr_value_at_percentile(h, 99.0)));
        fmt::print("P99.9:  {:.3f} us\n", us(hdr_value_at_percentile(h, 99.9)));
        fmt::print("P99.99: {:.3f} us\n", us(hdr_value_at_percentile(h, 99.99)));
        fmt::print("P99.999:{:.3f} us\n", us(hdr_value_at_percentile(h, 99.999)));
        fmt::print("Max:    {:.3f} us\n", us(hdr_max(h)));
        fmt::print("Mean:   {:.3f} us\n", us(static_cast<std::int64_t>(hdr_mean(h))));
        fmt::print("-----------------------------\n");

        if (m_outputPath.empty()) return;

        // Dump a plottable CSV (percentile distribution) — upload to
        // hdrhistogram.github.io/HdrHistogram/plotFiles.html or feed to gnuplot.
        std::FILE* f = std::fopen(m_outputPath.c_str(), "w");
        if (!f) {
            fmt::print(stderr, "[Recorder] cannot open {}: {}\n",
                       m_outputPath, std::strerror(errno));
            return;
        }
        // CLASSIC=hdr_percentiles_csv? The C lib exposes hdr_percentiles_print.
        // 5 ticks/half-distance, value scale 1000.0 -> microseconds, CSV format.
        hdr_percentiles_print(h, f, 5, 1000.0, CSV);
        std::fclose(f);
        fmt::print("[Recorder] wrote percentile CSV to {}\n", m_outputPath);
    }

    RecorderChannel& m_ch;
    std::string      m_outputPath;
};
