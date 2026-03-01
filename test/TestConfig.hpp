#pragma once

#include <toml++/toml.hpp>

#include <array>
#include <cstdio>
#include <cstdint>
#include <string>
#include <stdexcept>

struct RoleConfig {
    std::string   interface;
    std::array<std::uint8_t, 6> mac;
    int           cpuCore;
};

struct TestConfig {
    std::uint16_t etherType;
    std::uint32_t blockSize;
    std::uint32_t blockNumber;
    std::uint32_t frameSize;
    int           watchdogSec;
    RoleConfig    server;
    RoleConfig    client;
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
    rc.interface = tbl[role]["interface"].value_or("eno2"s);
    rc.mac       = parseMac(tbl[role]["mac"].value_or(""s));
    rc.cpuCore   = tbl[role]["cpu_core"].value_or(4);
    return rc;
}

inline TestConfig loadConfig(const char* path) {
    auto tbl = toml::parse_file(path);
    TestConfig cfg{};

    cfg.etherType   = static_cast<std::uint16_t>(tbl["general"]["ether_type"].value_or(0x88B5));
    cfg.blockSize   = static_cast<std::uint32_t>(tbl["general"]["block_size"].value_or(4096));
    cfg.blockNumber = static_cast<std::uint32_t>(tbl["general"]["block_number"].value_or(64));
    cfg.frameSize   = static_cast<std::uint32_t>(tbl["general"]["frame_size"].value_or(64));
    cfg.watchdogSec = tbl["general"]["watchdog_sec"].value_or(30);

    cfg.server = loadRole(tbl, "server");
    cfg.client = loadRole(tbl, "client");

    return cfg;
}
