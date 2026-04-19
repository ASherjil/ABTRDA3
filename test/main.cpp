// ABTRDA3 — Ultra-Low Latency Network Test
//
// Modes:
//   --server   Reflect incoming packets (ping-pong server)
//   --client   Send packets and measure RTT (ping-pong client)
//   --txgen    TX-only traffic generator
//   --rxsink   RX-only packet counter
//
// Each role selects its own transport in the TOML config so an x86 box
// (intel_i210) can pair with an ARM64 SoC (cadence_gem).
//
// Usage:
//   sudo ./abtrda3_test --server [--config <file>]
//   sudo ./abtrda3_test --client [--count N] [--config <file>]
//   sudo ./abtrda3_test --txgen  [--count N] [--config <file>]
//   sudo ./abtrda3_test --rxsink [--config <file>]

#include "NicTuner.hpp"
#include "TestConfig.hpp"
#include "RuntimeSetup.hpp"
#include "TransportDispatch.hpp"

#include <fmt/core.h>

#include <cstdint>
#include <cstring>
#include <optional>

namespace {

struct CliArgs {
    const char*   configPath = "abtrda3_test.toml";
    RunMode       runMode{};
    bool          useServerRole{};
    std::int64_t  countOverride = -1;
    bool          valid         = false;
};

[[nodiscard]] CliArgs parseCli(int argc, char* argv[]) {
    CliArgs a{};
    bool haveMode = false;

    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--config" && i + 1 < argc) {
            a.configPath = argv[++i];
        }
        else if (arg == "--server")  { a.runMode = RunMode::Server; a.useServerRole = true;  haveMode = true; }
        else if (arg == "--client")  { a.runMode = RunMode::Client; a.useServerRole = false; haveMode = true; }
        else if (arg == "--txgen")   { a.runMode = RunMode::TxGen;  a.useServerRole = false; haveMode = true; }
        else if (arg == "--rxsink")  { a.runMode = RunMode::RxSink; a.useServerRole = true;  haveMode = true; }
        else if (arg == "--count" && i + 1 < argc) {
            a.countOverride = std::atoll(argv[++i]);
        }
    }

    a.valid = haveMode;
    return a;
}

void printUsage(const char* argv0) {
    fmt::println("Usage:\n"
                 "  {0} --server  [--config <file>]\n"
                 "  {0} --client  [--count <N>] [--config <file>]\n"
                 "  {0} --txgen   [--count <N>] [--config <file>]\n"
                 "  {0} --rxsink  [--config <file>]",
                 argv0);
}

} // anonymous namespace

int main(int argc, char* argv[]) {
    if (argc < 2) {
        printUsage(argv[0]);
        return 1;
    }

    const CliArgs args = parseCli(argc, argv);
    if (!args.valid) {
        fmt::println(stderr, "Error: specify --server, --client, --txgen, or --rxsink");
        return 1;
    }

    TestConfig cfg;
    try {
        cfg = loadConfig(args.configPath);
    } catch (const std::exception& e) {
        fmt::println(stderr, "Config error: {}", e.what());
        return 1;
    }

    const RoleConfig& role = args.useServerRole ? cfg.server : cfg.client;

    std::uint32_t count = cfg.clientCount;
    if (args.countOverride >= 0)
        count = static_cast<std::uint32_t>(args.countOverride);

    fmt::println("[Config] Loaded from {} — role={} transport={} iface={}",
                 args.configPath, runModeName(args.runMode),
                 role.transport, role.interface);

    // System tuning (optional per config)
    std::optional<NicTuner> tuner;
    if (cfg.nicTunerMode != NicTunerMode::Off) {
        tuner.emplace(role.interface.c_str(), role.cpuCore, cfg.nicTunerMode);
    } else {
        fmt::println(stderr, "[NicTuner] Off");
    }

    // RT setup (watchdog, CPU pin, SCHED_FIFO, mlockall, signal handler)
    RuntimeSetup rt(role.cpuCore, cfg.watchdogSec, runModeName(args.runMode));

    return runTransport(cfg, role, args.runMode, count, rt.stopToken());
}
