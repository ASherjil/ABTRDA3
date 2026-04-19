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
// pair an Intel I210 on one host with a Cadence GEM on another.
struct RoleConfig {
    std::string                 transport;          // "packet_mmap", "af_xdp", "intel_i210", "cadence_gem"
    std::string                 interface;          // e.g. "eno3", "end0"
    std::string                 driver;             // kernel driver to unbind for PMDs ("igb", "macb", "e1000e")
    std::array<std::uint8_t, 6> mac{};
    int                         cpuCore{};
    std::uint32_t               xdpQueueId = 0;
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

    std::uint32_t clientCount = 10;

    std::uint32_t mmapBlockSize   = 4096;
    std::uint32_t mmapBlockNumber = 512;

    std::uint32_t xdpUmemFrameSize = 4096;
    std::uint32_t xdpFrameCount    = 64;
    bool          xdpNeedWakeup    = true;
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

    cfg.clientCount = static_cast<std::uint32_t>(tbl["client"]["count"].value_or(10));

    cfg.mmapBlockSize   = static_cast<std::uint32_t>(tbl["packet_mmap"]["block_size"].value_or(4096));
    cfg.mmapBlockNumber = static_cast<std::uint32_t>(tbl["packet_mmap"]["block_number"].value_or(512));

    cfg.xdpUmemFrameSize = static_cast<std::uint32_t>(tbl["af_xdp"]["umem_frame_size"].value_or(4096));
    cfg.xdpFrameCount    = static_cast<std::uint32_t>(tbl["af_xdp"]["frame_count"].value_or(64));
    cfg.xdpNeedWakeup    = tbl["af_xdp"]["need_wakeup"].value_or(true);

    return cfg;
}
