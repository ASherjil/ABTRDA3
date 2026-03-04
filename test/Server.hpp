#pragma once

#include "RingConcepts.hpp"
#include "TestConfig.hpp"

#include <cstdio>
#include <cstring>
#include <stop_token>

template<TxRing Tx, RxRing Rx>
inline void run_server(Tx& tx, Rx& rx, const TestConfig& cfg, std::stop_token stop) {
    std::uint64_t packet_count = 0;

    // Outer loop checks stop token; inner loop spins tight without atomic reads
    while (true) {
        for (std::uint32_t i = 0; i < 65536; ++i) {
            RxFrame rxf = rx.tryReceive();
            if (rxf.data.empty())[[likely]] {
                continue;
            }

            if (rxf.data.size() >= cfg.frameSize) {
                // Zero-copy: write directly into TX ring slot
                auto* dst = tx.acquire(cfg.frameSize);
                while (!dst) {
                    if (stop.stop_requested())[[unlikely]]{
                        rx.release();
                        return;
                    }
                    dst = tx.acquire(cfg.frameSize);
                }

                // Swap MACs from RX ring directly into TX slot, copy payload
                std::memcpy(dst,      &rxf.data[6],  6);   // dst MAC = RX src
                std::memcpy(dst + 6,  &rxf.data[0],  6);   // src MAC = RX dst
                std::memcpy(dst + 12, &rxf.data[12], cfg.frameSize - 12);

                rx.release();  // free RX slot before TX syscall
                tx.commit();
                packet_count++;
            }
            else {
                rx.release();
            }
        }
        if (stop.stop_requested()) break;
    }

    std::printf("\n[Server] Reflected %lu packets.\n", packet_count);
}
