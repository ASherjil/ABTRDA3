// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Areeb Sherjil

// ABTRDA3 — Ultra-Low Latency Network Test
//
// Modes:
//   --server   Reflect incoming packets (ping-pong server)
//   --client   Send packets and measure RTT (ping-pong client)
//   --txgen    TX-only traffic generator
//   --rxsink   RX-only packet counter
//
// Each role selects its own transport in the TOML config so heterogeneous
// hosts (e.g. an x86 box and an ARM64 SoC) can pair in one benchmark.
//
// Usage:
//   sudo ./abtrda3_test --server [--config <file>]
//   sudo ./abtrda3_test --client [--config <file>]          (RTT; time-bounded via duration_sec)
//   sudo ./abtrda3_test --txgen  [--count N] [--config <file>]
//   sudo ./abtrda3_test --rxsink [--config <file>]

#include <cstdint>
#include <cstring>
#include <optional>

#include <fmt/core.h>

#include "NicTuner.hpp"
#include "RuntimeSetup.hpp"
#include "TestConfig.hpp"
#include "TransportDispatch.hpp"

namespace {

struct CliArgs {
    const char*  configPath = "abtrda3_test.toml";
    RunMode      runMode{};
    bool         useServerRole{};
    std::int64_t countOverride = -1;
    bool         valid         = false;
};

[[nodiscard]] CliArgs parseCli(int argc, char* argv[]) {
    CliArgs a{};
    bool    haveMode = false;

    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--config" && i + 1 < argc) {
            a.configPath = argv[++i];
        } else if (arg == "--server") {
            a.runMode       = RunMode::Server;
            a.useServerRole = true;
            haveMode        = true;
        } else if (arg == "--client") {
            a.runMode       = RunMode::Client;
            a.useServerRole = false;
            haveMode        = true;
        } else if (arg == "--txgen") {
            a.runMode       = RunMode::TxGen;
            a.useServerRole = false;
            haveMode        = true;
        } else if (arg == "--rxsink") {
            a.runMode       = RunMode::RxSink;
            a.useServerRole = true;
            haveMode        = true;
        } else if (arg == "--single") {
            a.runMode       = RunMode::SingleRecorder;
            a.useServerRole = false;
            haveMode        = true;
        } else if (arg == "--count" && i + 1 < argc) {
            a.countOverride = std::atoll(argv[++i]);
        }
    }

    a.valid = haveMode;
    return a;
}

void printUsage(const char* argv0) {
    fmt::println("Usage:\n"
                 "  {0} --server  [--config <file>]\n"
                 "  {0} --client  [--config <file>]   (RTT; time-bounded via [client].duration_sec)\n"
                 "  {0} --txgen   [--count <N>] [--config <file>]\n"
                 "  {0} --rxsink  [--config <file>]\n"
                 "  {0} --single  [--config <file>]   (single-process one-way Tx->Rx latency)",
                 argv0);
}

}   // anonymous namespace

int main(int argc, char* argv[]) {
    if (argc < 2) {
        printUsage(argv[0]);
        return 1;
    }

    const CliArgs args = parseCli(argc, argv);
    if (!args.valid) {
        fmt::println(stderr, "Error: specify --server, --client, --txgen, --rxsink, or --single");
        return 1;
    }

    TestConfig cfg;
    try {
        cfg = loadConfig(args.configPath);
    } catch (const std::exception& e) {
        fmt::println(stderr, "Config error: {}", e.what());
        return 1;
    }

    // ── Single-process one-way recorder: needs BOTH ports + its own core layout.
    // Tx=client port, Rx=server port; the hot path runs on recHotPathCore (pinned
    // + SCHED_OTHER here), the recorder self-pins to recRecorderCore inside the
    // dispatch. NicTuner is applied to both ports inside runSingleRecorder.
    if (args.runMode == RunMode::SingleRecorder) {
        fmt::println("[Config] Loaded from {} — mode=SingleRecorder tx={} rx={}", args.configPath,
                     cfg.client.interface, cfg.server.interface);
        std::optional<NicTuner> txTuner;
        std::optional<NicTuner> rxTuner;
        if (cfg.nicTunerMode != NicTunerMode::Off) {
            txTuner.emplace(cfg.client.interface.c_str(), cfg.recHotPathCore, cfg.nicTunerMode);
            rxTuner.emplace(cfg.server.interface.c_str(), cfg.recRecorderCore, cfg.nicTunerMode);
        } else {
            fmt::println(stderr, "[NicTuner] Off");
        }
        // Watchdog disabled (0): the hot loop self-terminates on duration_sec.
        const RuntimeSetup rt(cfg.recHotPathCore, 0, "SingleRecorder");
        return runSingleRecorder(cfg, rt.stopToken());
    }

    const RoleConfig& role = args.useServerRole ? cfg.server : cfg.client;

    std::uint32_t count = cfg.clientCount;
    if (args.countOverride >= 0) {
        count = static_cast<std::uint32_t>(args.countOverride);
    }

    fmt::println("[Config] Loaded from {} — role={} transport={} iface={}", args.configPath,
                 runModeName(args.runMode), role.transport, role.interface);

    // System tuning (optional per config)
    std::optional<NicTuner> tuner;
    if (cfg.nicTunerMode != NicTunerMode::Off) {
        tuner.emplace(role.interface.c_str(), role.cpuCore, cfg.nicTunerMode);
    } else {
        fmt::println(stderr, "[NicTuner] Off");
    }

    // RT setup (watchdog, CPU pin, SCHED_OTHER, mlockall, signal handler)
    const RuntimeSetup rt(role.cpuCore, cfg.watchdogSec, runModeName(args.runMode));

    return runTransport(cfg, role, args.runMode, count, rt.stopToken());
}
