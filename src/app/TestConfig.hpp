// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Areeb Sherjil

#pragma once

#include "NicTuner.hpp"

#include <fmt/core.h>
#include <toml++/toml.hpp>

#include <array>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string>

// Per-role settings — server and client can use different transports so we can
// pair different NICs/hosts in one benchmark.
struct RoleConfig {
    std::string                 transport;          // "packet_mmap", "af_xdp", "dpdk", "verbs", "ef_vi", "intel_i210"
    std::string                 interface;          // e.g. "eno3", "end0"
    std::string                 driver;             // kernel driver to unbind for PMDs ("igb", "e1000e")
    std::array<std::uint8_t, 6> mac{};
    int                         cpuCore{};
    std::uint32_t               xdpQueueId = 0;

    // DPDK binding model (transport = "dpdk" only) — EXPLICIT, no driver-name
    // sniffing. Exactly one may be true; both false = vfio-pci pass-through.
    bool dpdkBifurcated = false;   // [role] dpdk_bifurcated: keep the kernel driver (mlx5-class)
    bool dpdkAfxdpPmd   = false;   // [role] dpdk_afxdp_pmd: DPDK-over-AF_XDP via the net_af_xdp vdev
};

struct TestConfig {
    std::uint16_t etherType;
    std::uint32_t frameSize;
    int           watchdogSec;
    NicTunerMode  nicTunerMode   = NicTunerMode::Full;
    std::uint32_t sendIntervalUs = 0;               // µs between sends (0 = back-to-back)
    std::string   outputPath;                       // latency output file (empty = no file)

    RoleConfig    server;
    RoleConfig    client;

    std::uint32_t clientCount = 10;            // --txgen burst length (--client is time-based)

    // [client] RTT (--client) — now DURATION-bounded like --single, not count.
    // The hot ping-pong loop runs on [client].cpu_core; a histogram thread on
    // recorder_core (must DIFFER from cpu_core) drains RTT samples into an
    // HdrHistogram, so a 24 h soak needs no unbounded std::vector.
    std::uint64_t clientDurationSec  = 60;
    int           clientRecorderCore = 4;

    std::uint32_t mmapBlockSize   = 4096;
    std::uint32_t mmapBlockNumber = 512;

    // AF_XDP frame size / frame count / need_wakeup are COMPILE-TIME template params
    // on AFXDP<> (FrameSize, NumRxFrames/NumTxFrames, NeedWakeup), NOT runtime config.
    // The only AF_XDP runtime knobs are the NAPI-regime ENABLE toggles below; their
    // VALUES (defer=2, gro=200000ns, irq-suspend=20ms) stay fixed in AFXDP.hpp.
    bool          afxdpEnableDeferral   = false;   // [af_xdp] enable_deferral
    bool          afxdpEnableIrqSuspend = false;   // [af_xdp] enable_irq_suspend

    // [single_recorder] — single-process one-way Tx->Rx latency benchmark.
    // hotPathCore runs the timed loop; recorderCore drains the SPSC queue into
    // the HdrHistogram (must be a DIFFERENT core so it never steals hot cycles).
    // durationSec bounds the run (time-based, not sample-count) — e.g. 60 for a
    // smoke test, 86400 for a 24 h determinism soak. Output CSV path = general
    // `output`. Tx reuses [client], Rx reuses [server].
    int           recHotPathCore  = 2;           // Tx thread
    int           recRxCore       = 3;           // Rx thread (distinct isolated core)
    int           recRecorderCore = 4;           // Hist thread
    std::uint64_t recDurationSec  = 60;
    std::size_t   recQueueCapacity = 1u << 20;   // 1,048,576 cycle-delta slots
};

inline std::array<std::uint8_t, 6> parseMac(const std::string& s) {
    std::array<std::uint8_t, 6> mac{};
    unsigned int b[6];
    if (std::sscanf(s.c_str(), "%x:%x:%x:%x:%x:%x",
                    &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) != 6) {
        throw std::runtime_error("Invalid MAC address: " + s);
    }
    for (int i = 0; i < 6; ++i)
        mac[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(b[i]);
    return mac;
}

inline RoleConfig loadRole(const toml::table& tbl, const char* role) {
    using namespace std::string_literals;
    RoleConfig rc{};
    rc.transport  = tbl[role]["transport"].value_or("packet_mmap"s);
    rc.interface  = tbl[role]["interface"].value_or("eno2"s);
    rc.driver     = tbl[role]["driver"].value_or(""s);
    rc.mac        = parseMac(tbl[role]["mac"].value_or(""s));
    rc.cpuCore    = tbl[role]["cpu_core"].value_or(4);
    rc.xdpQueueId = static_cast<std::uint32_t>(tbl[role]["xdp_queue_id"].value_or(0));
    rc.dpdkBifurcated = tbl[role]["dpdk_bifurcated"].value_or(false);
    rc.dpdkAfxdpPmd   = tbl[role]["dpdk_afxdp_pmd"].value_or(false);
    if (rc.dpdkBifurcated && rc.dpdkAfxdpPmd)
        throw std::runtime_error("dpdk_bifurcated and dpdk_afxdp_pmd are mutually exclusive");
    return rc;
}

inline TestConfig loadConfig(const char* path) {
    auto tbl = toml::parse_file(path);
    TestConfig cfg{};

    cfg.etherType      = static_cast<std::uint16_t>(tbl["general"]["ether_type"].value_or(0x88B5));
    cfg.frameSize      = static_cast<std::uint32_t>(tbl["general"]["frame_size"].value_or(64));
    cfg.watchdogSec    = tbl["general"]["watchdog_sec"].value_or(30);

    const auto nicTunerStr = tbl["general"]["nic_tuner"].value_or(std::string("full"));
    if      (nicTunerStr == "off")      cfg.nicTunerMode = NicTunerMode::Off;
    else if (nicTunerStr == "nfs_safe") cfg.nicTunerMode = NicTunerMode::NfsSafe;
    else                                cfg.nicTunerMode = NicTunerMode::Full;

    cfg.sendIntervalUs = static_cast<std::uint32_t>(tbl["general"]["send_interval_us"].value_or(0));
    cfg.outputPath     = tbl["general"]["output"].value_or(std::string{});

    cfg.server = loadRole(tbl, "server");
    cfg.client = loadRole(tbl, "client");

    cfg.clientCount        = static_cast<std::uint32_t>(tbl["client"]["count"].value_or(10));
    cfg.clientDurationSec  = static_cast<std::uint64_t>(tbl["client"]["duration_sec"].value_or(60));
    cfg.clientRecorderCore = tbl["client"]["recorder_core"].value_or(4);

    cfg.mmapBlockSize   = static_cast<std::uint32_t>(tbl["packet_mmap"]["block_size"].value_or(4096));
    cfg.mmapBlockNumber = static_cast<std::uint32_t>(tbl["packet_mmap"]["block_number"].value_or(512));

    // [af_xdp] — only the NAPI-regime enable toggles are runtime; frame size/count/
    // need_wakeup are compile-time template params on AFXDP<> (see struct note).
    cfg.afxdpEnableDeferral   = tbl["af_xdp"]["enable_deferral"].value_or(false);
    cfg.afxdpEnableIrqSuspend = tbl["af_xdp"]["enable_irq_suspend"].value_or(false);

    cfg.recHotPathCore   = tbl["single_recorder"]["hot_path_core"].value_or(2);
    cfg.recRxCore        = tbl["single_recorder"]["rx_core"].value_or(3);
    cfg.recRecorderCore  = tbl["single_recorder"]["benchmark_recorder_core"].value_or(4);
    cfg.recDurationSec   = static_cast<std::uint64_t>(tbl["single_recorder"]["duration_sec"].value_or(60));
    cfg.recQueueCapacity = static_cast<std::size_t>(tbl["single_recorder"]["queue_capacity"].value_or(1u << 20));

    return cfg;
}
