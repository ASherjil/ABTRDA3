#pragma once

#include <toml++/toml.hpp>
#include <array>
#include <cstdio>
#include <string>
#include <stdexcept>

enum class Transport { PacketMmap, AfXdp };

struct RoleConfig {
  std::string   interface;
  std::array<std::uint8_t, 6> mac;
  int           cpuCore;
};

struct TestConfig {
  Transport     transport;
  std::uint16_t etherType;
  std::uint32_t frameSize;       // Ethernet frame size (both transports)
  int           watchdogSec;
  bool          skipNicTuner = false;
  std::uint32_t sendIntervalUs = 0;   // µs between sends (0 = back-to-back, 1000 = 1ms)
  std::string   outputPath;           // latency output file (empty = no file)
  RoleConfig    server;
  RoleConfig    client;
  std::uint32_t clientCount = 10;     // number of packets to send (client only)

  // packet_mmap-specific
  std::uint32_t mmapBlockSize   = 4096;
  std::uint32_t mmapBlockNumber = 512;

  // AF_XDP-specific
  std::uint32_t xdpQueueId       = 0;
  std::uint32_t xdpUmemFrameSize = 4096;
  std::uint32_t xdpFrameCount    = 64;
  bool          xdpZeroCopy      = false;
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
    rc.interface = tbl[role]["interface"].value_or("eno2"s);
    rc.mac       = parseMac(tbl[role]["mac"].value_or(""s));
    rc.cpuCore   = tbl[role]["cpu_core"].value_or(4);
    return rc;
}

inline TestConfig loadConfig(const char* path) {
  auto tbl = toml::parse_file(path);
  TestConfig cfg{};

  // Transport selection
  auto transport_str = tbl["general"]["transport"].value_or(std::string("packet_mmap"));
  if (transport_str == "af_xdp")
      cfg.transport = Transport::AfXdp;
  else
      cfg.transport = Transport::PacketMmap;

  cfg.etherType      = static_cast<std::uint16_t>(tbl["general"]["ether_type"].value_or(0x88B5));
  cfg.frameSize      = static_cast<std::uint32_t>(tbl["general"]["frame_size"].value_or(64));
  cfg.watchdogSec    = tbl["general"]["watchdog_sec"].value_or(30);
  cfg.skipNicTuner   = tbl["general"]["skip_nic_tuner"].value_or(false);
  cfg.sendIntervalUs = static_cast<std::uint32_t>(tbl["general"]["send_interval_us"].value_or(0));
  cfg.outputPath     = tbl["general"]["output"].value_or(std::string{});

  cfg.server = loadRole(tbl, "server");
  cfg.client = loadRole(tbl, "client");

  // Client count — from [client] section, overridable by --count CLI arg
  cfg.clientCount = static_cast<std::uint32_t>(tbl["client"]["count"].value_or(10));

  // packet_mmap settings
  cfg.mmapBlockSize   = static_cast<std::uint32_t>(tbl["packet_mmap"]["block_size"].value_or(4096));
  cfg.mmapBlockNumber = static_cast<std::uint32_t>(tbl["packet_mmap"]["block_number"].value_or(512));

  // AF_XDP settings
  cfg.xdpQueueId       = static_cast<std::uint32_t>(tbl["af_xdp"]["queue_id"].value_or(0));
  cfg.xdpUmemFrameSize = static_cast<std::uint32_t>(tbl["af_xdp"]["umem_frame_size"].value_or(4096));
  cfg.xdpFrameCount    = static_cast<std::uint32_t>(tbl["af_xdp"]["frame_count"].value_or(64));
  cfg.xdpZeroCopy      = tbl["af_xdp"]["zero_copy"].value_or(false);
  cfg.xdpNeedWakeup    = tbl["af_xdp"]["need_wakeup"].value_or(true);

  return cfg;
}
