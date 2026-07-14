#pragma once

#include "RingConcepts.hpp"
#include "TestConfig.hpp"
#include "Profiling.hpp"

#include <cstdio>
#include <cstring>
#include <atomic>
#include <stop_token>
#include <vector>

// Splice reflect. The TX slots are prefilled ONCE with the constant reply header —
// on the point-to-point rig dst ([client].mac), src ([server].mac) and the ethertype
// never change — and the hot path copies only bytes 14..25 (seq + stamp) out of the
// received frame. perf on the X2522 (cycles, -g): the full-echo memcpys cost
// ~50ns/round, and not for the copying (52B through AVX is ~1ns) — it's the
// dependent-load chain hanging off the FIRST touch of the just-DMA'd RX cacheline.
// The splice keeps the one irreducible RX load (the seq genuinely lives there) and
// drops the chain. Reply wire format is unchanged; payload past byte 25 is prefilled
// zeros instead of an echo — nothing validates it (the client checks dst MAC + seq).
// false = the published full-echo reflector (the CX4/igc campaign numbers used it).
inline constexpr bool kServerSpliceReflect = true;

template<TxRing Tx, RxRing Rx>
inline void run_server(Tx& tx, Rx& rx, const TestConfig& cfg, std::stop_token stop) {
    std::uint64_t packet_count = 0;
    [[maybe_unused]] prof::CycleStats reflectStats;   // debug-only; gated, elided in release

    if constexpr (kServerSpliceReflect) {
        std::vector<std::uint8_t> tmpl(cfg.frameSize, 0);
        std::memcpy(&tmpl[0], cfg.client.mac.data(), 6);   // reply dst = the originator
        std::memcpy(&tmpl[6], cfg.server.mac.data(), 6);   // reply src = us
        tmpl[12] = static_cast<std::uint8_t>(cfg.etherType >> 8);
        tmpl[13] = static_cast<std::uint8_t>(cfg.etherType & 0xFF);
        tx.prefillRing(tmpl);
    }

    // Outer loop checks stop token; inner loop spins tight without atomic reads
    while (true) {
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
                    return;
                }
                dst = tx.acquire(cfg.frameSize);
            }

            if constexpr (kServerSpliceReflect) {
                // Header is prefilled; only seq + stamp vary per frame
                std::memcpy(dst + 14, &rxf.data[14], 12);
                // Drain the store buffer BEFORE the CTPIO burst. This 12B store
                // cannot store-forward into the copy's 16B slot reads (partial
                // overlap), and its data hangs off the LLC-latency RX read — so
                // unfenced, that combined stall lands MID-BURST between aperture
                // writes and trips the NIC's ingest window (measured: 15.9%
                // server fallbacks). The fence relocates the same wait to before
                // the first aperture write, where no NIC timer is running.
                std::atomic_thread_fence(std::memory_order_seq_cst);
            } else {
                // Swap MACs from RX ring directly into TX slot, copy payload
                std::memcpy(dst,      &rxf.data[6],  6);   // dst MAC = RX src
                std::memcpy(dst + 6,  &rxf.data[0],  6);   // src MAC = RX dst
                std::memcpy(dst + 12, &rxf.data[12], cfg.frameSize - 12);
                // Same pre-burst drain as the splice path: with the RX re-post
                // doorbell decoupled out of release(), the reflect stores sit
                // undrained when the CTPIO burst begins and tear it per-send
                // (measured 1.2-9.8% noncontig, layout-modulated). The drain
                // makes the burst clean by construction, on any layout.
                std::atomic_thread_fence(std::memory_order_seq_cst);
            }

            rx.release();  // free RX slot before TX syscall
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

    std::printf("\n[Server] Reflected %lu packets.\n", packet_count);
    if constexpr (prof::kDebugProfiling) {
        reflectStats.report("server reflect (rx->tx)", prof::tscHz());
    }
}
