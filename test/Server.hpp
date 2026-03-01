#pragma once

#include "PacketMmapTx.hpp"
#include "PacketMmapRx.hpp"
#include "TestConfig.hpp"

#include <cstdio>
#include <cstring>
#include <stop_token>

inline void run_server(const TestConfig& cfg, std::stop_token stop) {
    RingConfig rx_cfg{};
    rx_cfg.interface   = cfg.server.interface.c_str();
    rx_cfg.direction   = RingDirection::RX;
    rx_cfg.blockSize   = cfg.blockSize;
    rx_cfg.blockNumber = cfg.blockNumber;
    rx_cfg.protocol    = cfg.etherType;
    rx_cfg.hwTimeStamp = false;

    RingConfig tx_cfg{};
    tx_cfg.interface     = cfg.server.interface.c_str();
    tx_cfg.direction     = RingDirection::TX;
    tx_cfg.blockSize     = cfg.blockSize;
    tx_cfg.blockNumber   = cfg.blockNumber;
    tx_cfg.protocol      = cfg.etherType;
    tx_cfg.packetVersion = TPACKET_V2;
    tx_cfg.qdiscBypass   = true;

    PacketMmapRx rx(rx_cfg);
    PacketMmapTx tx(tx_cfg);

    std::uint64_t packet_count = 0;

    // Outer loop checks stop token; inner loop spins tight without atomic reads
    while (true) {
        for (std::uint32_t i = 0; i < 65536; ++i) {
            RxFrame rxf = rx.tryReceive();
            if (rxf.data.empty()) continue;

            if (rxf.data.size() >= cfg.frameSize) {
                // Zero-copy: write directly into TX ring slot
                auto* dst = tx.acquire(cfg.frameSize);
                while (!dst) {
                    if (stop.stop_requested()) { rx.release(); return; }
                    dst = tx.acquire(cfg.frameSize);
                }

                // Swap MACs from RX ring directly into TX slot, copy payload
                std::memcpy(dst,      &rxf.data[6],  6);   // dst MAC = RX src
                std::memcpy(dst + 6,  &rxf.data[0],  6);   // src MAC = RX dst
                std::memcpy(dst + 12, &rxf.data[12], cfg.frameSize - 12);

                rx.release();  // free RX slot before TX syscall
                tx.commit();
                packet_count++;
            } else {
                rx.release();
            }
        }
        if (stop.stop_requested()) break;
    }

    std::printf("\n[Server] Reflected %lu packets.\n", packet_count);
}
