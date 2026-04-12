// ABTRDA3 - Ultra-Low Latency Network Test
//
// Modes:
//   --server   Reflect incoming packets (ping-pong server)
//   --client   Send packets and measure RTT (ping-pong client)
//   --txgen    TX-only traffic generator
//   --rxsink   RX-only packet counter
//
// Usage:
//   sudo ./abtrda3_test --server --config ../../../test/abtrda3_test.toml
//   sudo ./abtrda3_test --client --count 1000 --config ../../../test/abtrda3_test.toml
//   sudo ./abtrda3_test --txgen  --count 1000 --config ../../../test/abtrda3_test.toml
//   sudo ./abtrda3_test --rxsink --config ../../../test/abtrda3_test.toml

#include "NicTuner.hpp"
#include "TestConfig.hpp"
#include "RuntimeSetup.hpp"
#include "TransportDispatch.hpp"

#include <cstdio>
#include <cstring>
#include <optional>

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::printf("Usage:\n"
                    "  %s --server  [--config <file>]\n"
                    "  %s --client  [--count <N>] [--config <file>]\n"
                    "  %s --txgen   [--count <N>] [--config <file>]\n"
                    "  %s --rxsink  [--config <file>]\n",
                    argv[0], argv[0], argv[0], argv[0]);
        return 1;
    }

    // ── Parse CLI ────────────────────────────────────────────────────────
    const char* config_path = "abtrda3_test.toml";
    const char* mode = nullptr;
    std::int64_t count_override = -1;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--config") == 0 && i + 1 < argc)
            config_path = argv[++i];
        else if (std::strcmp(argv[i], "--server") == 0)
            mode = "server";
        else if (std::strcmp(argv[i], "--client") == 0)
            mode = "client";
        else if (std::strcmp(argv[i], "--txgen") == 0)
            mode = "txgen";
        else if (std::strcmp(argv[i], "--rxsink") == 0)
            mode = "rxsink";
        else if (std::strcmp(argv[i], "--count") == 0 && i + 1 < argc)
            count_override = std::atol(argv[++i]);
    }

    if (!mode) {
        std::fprintf(stderr, "Error: specify --server, --client, --txgen, or --rxsink\n");
        return 1;
    }

    // ── Load config ──────────────────────────────────────────────────────
    TestConfig cfg;
    try {
        cfg = loadConfig(config_path);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "Config error: %s\n", e.what());
        return 1;
    }

    std::uint32_t count = cfg.clientCount;
    if (count_override >= 0)
        count = static_cast<std::uint32_t>(count_override);

    std::printf("[Config] Loaded from %s\n", config_path);

    // Map CLI mode string to RunMode enum and select the correct role config
    RunMode runMode{};
    bool useServerRole{};

    if (std::strcmp(mode, "server") == 0)      { runMode = RunMode::Server; useServerRole = true;  }
    else if (std::strcmp(mode, "client") == 0)  { runMode = RunMode::Client; useServerRole = false; }
    else if (std::strcmp(mode, "txgen") == 0)   { runMode = RunMode::TxGen;  useServerRole = false; }
    else if (std::strcmp(mode, "rxsink") == 0)  { runMode = RunMode::RxSink; useServerRole = true;  }

    const RoleConfig& role = useServerRole ? cfg.server : cfg.client;

    // ── System tuning ────────────────────────────────────────────────────
    std::optional<NicTuner> tuner;
    if (cfg.nicTunerMode != NicTunerMode::Off)
        tuner.emplace(role.interface.c_str(), role.cpuCore, cfg.nicTunerMode);
    else
        std::fprintf(stderr, "[NicTuner] Off\n");

    // ── RT setup (watchdog, CPU pin, SCHED_FIFO) ─────────────────────────
    RuntimeSetup rt(role.cpuCore, cfg.watchdogSec, runModeName(runMode));

    // ── Run ──────────────────────────────────────────────────────────────
    return runTransport(cfg, role, runMode, count, rt.stopToken());
}
