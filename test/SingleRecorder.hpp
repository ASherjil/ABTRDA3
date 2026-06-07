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
//   * HotPath  (producer): time-bounded loop. acquire->commit->send->spin-receive
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

// ── Producer: the hot path ──────────────────────────────────────────────────
// Owns the Tx and Rx transports (different ports). One packet in flight: send,
// then spin until the matching echo (own MAC + seq) returns, timing the round.
template<TxRing Tx, RxRing Rx>
class HotPath {
public:
    HotPath(Tx& tx, Rx& rx, const TestConfig& cfg, RecorderChannel& ch,
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

        // Tail-diagnostic setup: allocate the fixed slow-round buffer ONCE here
        // (the single allocation, before the timing loop — never in the hot path)
        // and precompute the threshold in TSC cycles.
        const std::uint64_t slowThreshCyc =
            static_cast<std::uint64_t>(tscHz * SLOW_THRESHOLD_US / 1e6);
        if constexpr (INSTRUMENT_SLOW)
            m_slow = std::make_unique<SlowEvent[]>(SLOW_CAP);   // ~16 MB, one alloc

        // Recorder starts first: wait until it is draining before we produce, so
        // no samples are generated before the consumer is ready.
        while (!m_ch.ready.load(std::memory_order_acquire) && !stop.stop_requested())
            ;

        std::uint32_t seq = 0;
        std::uint64_t sent = 0, recv = 0;

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
            m_tx.commit();
            std::uint64_t tSend = 0;
            if constexpr (INSTRUMENT_SLOW) tSend = tsc::now();
            ++sent;

            // Spin until the Rx port delivers THIS frame (one-way, no reflection):
            // match dst MAC (the Rx port) + the seq we just wrote. The MAC+seq
            // check guarantees we time our own packet, not stray/background traffic.
            // No timeout: on a direct link the frame always returns. stop_token is
            // checked periodically so SIGINT can still break a wedged receive.
            std::uint32_t spins = 0;
            while (true) {
                if (RxFrame f = m_rx.tryReceive(); !f.data.empty()) [[unlikely]] {
                    if (std::memcmp(f.data.data(), m_cfg.server.mac.data(), 6) == 0) [[likely]] {
                        std::uint32_t rxSeq;
                        std::memcpy(&rxSeq, f.data.data() + 14, sizeof(rxSeq));
                        if (rxSeq == seqNet) [[likely]] {
                            std::uint64_t tMatch = 0;
                            if constexpr (INSTRUMENT_SLOW) tMatch = tsc::now();
                            m_rx.release();
                            const std::uint64_t t1 = tsc::now();
                            if (!m_ch.queue.try_push(t1 - t0)) [[unlikely]]
                                m_ch.pushFailures.fetch_add(1, std::memory_order_relaxed);
                            if constexpr (INSTRUMENT_SLOW) {
                                m_totalSpins += spins;   // baseline for normal rounds
                                if ((t1 - t0) > slowThreshCyc &&
                                    m_slowCount < SLOW_CAP) [[unlikely]] {
                                    m_slow[m_slowCount++] = {
                                        seq,
                                        static_cast<std::uint32_t>(tSend  - t0),
                                        static_cast<std::uint32_t>(tMatch - tSend),
                                        static_cast<std::uint32_t>(t1     - tMatch),
                                        spins};
                                }
                            }
                            ++recv;
                            break;
                        }
                    }
                    m_rx.release();   // stale / not-ours — drop, keep spinning
                }
                if ((++spins & 0xFFFF) == 0 && stop.stop_requested()) break;
            }
            ++seq;
        }

        m_ch.done.store(true, std::memory_order_release);
        fmt::print(stderr, "[HotPath] sent={} recv={} push_fail={}\n",
                   sent, recv, m_ch.pushFailures.load(std::memory_order_relaxed));

        if constexpr (INSTRUMENT_SLOW) dumpSlow(tscHz, recv);
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
        for (std::uint32_t w = 0; w < kWarmupPackets && !stop.stop_requested(); ++w, ++seq) {
            std::uint8_t* dst = m_tx.acquire(m_cfg.frameSize);
            if (!dst) continue;
            const std::uint32_t seqNet = htonl(seq);
            std::memcpy(dst + 14, &seqNet, sizeof(seqNet));
            m_tx.commit();
            // Spin until OUR echo returns — NO deadline, NO timer. A cold-start
            // packet may take several ms to settle (observed ~4.7 ms); we simply
            // wait it out so that one-time settle is ABSORBED here instead of
            // leaking into the timed loop. Mirrors the timed loop's unbounded spin.
            // Only SIGINT (stop_token, checked every 64K empty polls) can break it,
            // so a genuinely dead link is still escapable with Ctrl-C.
            std::uint32_t spins = 0;
            while (true) {
                RxFrame f = m_rx.tryReceive();
                if (!f.data.empty()) {
                    const bool mine = std::memcmp(f.data.data(), m_cfg.server.mac.data(), 6) == 0;
                    std::uint32_t rxSeq = 0;
                    if (mine) std::memcpy(&rxSeq, f.data.data() + 14, sizeof(rxSeq));
                    m_rx.release();
                    if (mine && rxSeq == seqNet) break;   // our echo -> next warmup packet
                }
                if ((++spins & 0xFFFF) == 0 && stop.stop_requested()) break;
            }
        }
    }

    // ── Tail diagnostics (compile-time gated; flip to false for the pristine
    //    production path). Splits each round into send / spin / release segments
    //    and captures the rare slow rounds so we can see WHICH segment balloons
    //    and whether the slow rounds are PERIODIC in the sequence number (a fixed
    //    gap => a per-N-packet event, e.g. DPDK mbuf-pool cache refill).
    //    ZERO hot-path allocation: ONE fixed buffer is allocated before the loop
    //    (unique_ptr<[]>, not a vector), then only plain index writes occur, and
    //    only on the rare slow branch. The fast path adds just two rdtscp reads
    //    (~20ns/packet, symmetric, preserves the tail shape) + one integer compare.
    //    Periodicity is read off the CSV's seq_gap column offline — no map/sort. ─
    static constexpr bool        INSTRUMENT_SLOW   = true;
    static constexpr double      SLOW_THRESHOLD_US = 6.0;       // ~just above P99.999 (~5.7us)
                                                               // so the 6-10us tail is captured
    static constexpr std::size_t SLOW_CAP          = 1'000'000; // ~16 MB, one alloc

    struct SlowEvent {
        std::uint32_t seq;
        std::uint32_t sendCyc;     // acquire + commit (TX submit, mbuf alloc)
        std::uint32_t spinCyc;     // flight + rx-poll until our frame matches
        std::uint32_t relCyc;      // rx release (mbuf free)
        std::uint32_t spins;       // # rx-poll iterations during spin: HIGH => NIC
                                   // was slow (kept polling empty ring); ~NORMAL
                                   // with high spinCyc => CPU descheduled mid-spin
    };

    void dumpSlow(double tscHz, std::uint64_t recv) {
        fmt::print(stderr, "[Slow] {} rounds > {}us threshold\n",
                   m_slowCount, SLOW_THRESHOLD_US);
        if (recv > 0)
            fmt::print(stderr, "[Slow] baseline: mean rx-poll spins over ALL {} rounds = {:.1f}\n",
                       recv, static_cast<double>(m_totalSpins) / static_cast<double>(recv));
        if (m_slowCount == 0) return;

        // Which segment balloons on slow rounds? (single pass, fixed scalars)
        // Also mean spins: HIGH => NIC was genuinely slow (we polled the empty ring
        // many times); ~NORMAL => CPU was descheduled mid-spin (few polls, long wall).
        std::uint64_t sumS = 0, sumSp = 0, sumR = 0, sumSpins = 0, minSpins = ~0ULL, maxSpins = 0;
        for (std::size_t i = 0; i < m_slowCount; ++i) {
            sumS += m_slow[i].sendCyc; sumSp += m_slow[i].spinCyc; sumR += m_slow[i].relCyc;
            sumSpins += m_slow[i].spins;
            if (m_slow[i].spins < minSpins) minSpins = m_slow[i].spins;
            if (m_slow[i].spins > maxSpins) maxSpins = m_slow[i].spins;
        }
        const double n = static_cast<double>(m_slowCount);
        fmt::print(stderr, "[Slow] mean segment on slow rounds (us): "
                   "send={:.2f} spin={:.2f} release={:.2f}\n",
                   tsc::cyclesToNs(static_cast<double>(sumS)  / n, tscHz) / 1000.0,
                   tsc::cyclesToNs(static_cast<double>(sumSp) / n, tscHz) / 1000.0,
                   tsc::cyclesToNs(static_cast<double>(sumR)  / n, tscHz) / 1000.0);
        fmt::print(stderr, "[Slow] rx-poll spins on slow rounds: mean={:.0f} min={} max={}"
                   " (HIGH=NIC slow / ~NORMAL=CPU descheduled mid-spin)\n",
                   static_cast<double>(sumSpins) / n, minSpins, maxSpins);

        // Full dump for offline analysis — the seq_gap column reveals periodicity
        // (a dominant fixed gap = a per-N-packet event like an mbuf-pool refill).
        if (std::FILE* f = std::fopen("slow_events.csv", "w")) {
            fmt::print(f, "seq,seq_gap,send_us,spin_us,release_us,total_us,spins\n");
            std::uint32_t prev = m_slow[0].seq;
            for (std::size_t i = 0; i < m_slowCount; ++i) {
                const SlowEvent& e = m_slow[i];
                const double s  = tsc::cyclesToNs(e.sendCyc, tscHz) / 1000.0;
                const double sp = tsc::cyclesToNs(e.spinCyc, tscHz) / 1000.0;
                const double r  = tsc::cyclesToNs(e.relCyc,  tscHz) / 1000.0;
                fmt::print(f, "{},{},{:.3f},{:.3f},{:.3f},{:.3f},{}\n",
                           e.seq, e.seq - prev, s, sp, r, s + sp + r, e.spins);
                prev = e.seq;
            }
            std::fclose(f);
            fmt::print(stderr, "[Slow] wrote slow_events.csv ({} rows)\n", m_slowCount);
        }
    }

    Tx&               m_tx;
    Rx&               m_rx;
    const TestConfig& m_cfg;
    RecorderChannel&  m_ch;
    std::uint64_t     m_durationSec;
    std::unique_ptr<SlowEvent[]> m_slow;       // fixed buffer, allocated once
    std::size_t                  m_slowCount = 0;
    std::uint64_t                m_totalSpins = 0;   // baseline: spins over all rounds
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
