#pragma once

#include "TestConfig.hpp"
#include "Server.hpp"
#include "Client.hpp"
#include "TxGenerator.hpp"
#include "RxSink.hpp"
#include "SocketOps.hpp"
#include "PacketMmapRx.hpp"
#include "PacketMmapTx.hpp"
#include "AFXDPSocket.hpp"
#include "AFXDPTx.hpp"
#include "AFXDPRx.hpp"
#include "common/HugePageHelpers.hpp"
#include "Intel_I210.hpp"

#include <cstdio>
#include <stop_token>
#include <string>

// =============================================================================
// Transport dispatch — creates the transport and runs the selected mode
// =============================================================================

enum class RunMode { Server, Client, TxGen, RxSink };

inline const char* runModeName(RunMode m) {
    switch (m) {
        case RunMode::Server: return "Server";
        case RunMode::Client: return "Client";
        case RunMode::TxGen:  return "TxGen";
        case RunMode::RxSink: return "RxSink";
    }
    return "Unknown";
}

// ── Dispatch a mode on concrete Tx/Rx objects ───────────────────────────────

template<TxRing Tx, RxRing Rx>
void dispatchMode(Tx& tx, Rx& rx, RunMode mode, const TestConfig& cfg,
                  std::uint32_t count, std::stop_token stop) {
    switch (mode) {
        case RunMode::Server:
            run_server(tx, rx, cfg, stop);
            break;
        case RunMode::Client:
            run_client(tx, rx, cfg, count, stop);
            break;
        case RunMode::TxGen:
            run_txgen(tx, cfg, count, stop);
            break;
        case RunMode::RxSink:
            run_rxsink(rx, cfg, stop);
            break;
    }
}

// ── Transport creation + dispatch ───────────────────────────────────────────

[[nodiscard]]
inline int runTransport(const TestConfig& cfg, const RoleConfig& role,
                        RunMode mode, std::uint32_t count, std::stop_token stop) {

    const char* roleName = runModeName(mode);

    if (cfg.transport == "packet_mmap") {
        RingConfig tx_cfg{};
        tx_cfg.interface     = role.interface.c_str();
        tx_cfg.direction     = RingDirection::TX;
        tx_cfg.blockSize     = cfg.mmapBlockSize;
        tx_cfg.blockNumber   = cfg.mmapBlockNumber;
        tx_cfg.protocol      = cfg.etherType;
        tx_cfg.packetVersion = TPACKET_V2;
        tx_cfg.qdiscBypass   = true;

        RingConfig rx_cfg{};
        rx_cfg.interface     = role.interface.c_str();
        rx_cfg.direction     = RingDirection::RX;
        rx_cfg.blockSize     = cfg.mmapBlockSize;
        rx_cfg.blockNumber   = cfg.mmapBlockNumber;
        rx_cfg.protocol      = cfg.etherType;
        rx_cfg.hwTimeStamp   = false;

        PacketMmapTx tx(tx_cfg);
        PacketMmapRx rx(rx_cfg);

        std::printf("[%s] Transport: packet_mmap on %s\n", roleName, role.interface.c_str());
        dispatchMode(tx, rx, mode, cfg, count, stop);
    }
    else if (cfg.transport == "af_xdp") {
        XdpConfig xdp_cfg{};
        xdp_cfg.interface  = role.interface.c_str();
        xdp_cfg.queueId    = role.xdpQueueId;
        xdp_cfg.frameSize  = cfg.xdpUmemFrameSize;
        xdp_cfg.frameCount = cfg.xdpFrameCount;
        xdp_cfg.etherType  = cfg.etherType;
        xdp_cfg.needWakeup = cfg.xdpNeedWakeup;

        AFXDPSocket sock(xdp_cfg);
        AFXDPTx tx(sock);
        AFXDPRx rx(sock);

        std::printf("[%s] Transport: af_xdp on %s (queue %u)\n",
                    roleName, role.interface.c_str(), role.xdpQueueId);
        dispatchMode(tx, rx, mode, cfg, count, stop);
    }
    else if (cfg.transport == "intel_i210") {
        if (!ensureHugepages(16)) {
            std::fprintf(stderr, "Error: hugepage allocation failed\n");
            return 1;
        }

        Intel_I210<DriverMode::RxTx> nic(role.interface);
        if (!nic.init()) {
            std::fprintf(stderr, "Error: I210 init failed\n");
            return 1;
        }

        std::printf("[%s] Transport: intel_i210 (PMD) on %s\n",
                    roleName, role.interface.c_str());
        dispatchMode(nic, nic, mode, cfg, count, stop);
    }
    else {
        std::fprintf(stderr, "Error: unknown transport '%s'\n", cfg.transport.c_str());
        return 1;
    }

    return 0;
}
