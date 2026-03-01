// ABTRDA3 - Ultra-Low Latency Ping-Pong Test
//
// Usage:
//   Server (Reflector): sudo taskset -c 4 ./abtrda3_test --server
//   Client (Measurer):  sudo taskset -c 4 ./abtrda3_test --client --count 1000
//   Custom config:      sudo taskset -c 4 ./abtrda3_test --server --config /path/to/config.toml

#include "NicTuner.hpp"
#include "TestConfig.hpp"
#include "Server.hpp"
#include "Client.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <csignal>
#include <stop_token>
#include <sched.h>
#include <pthread.h>
#include <sys/mman.h>
#include <thread>
#include <chrono>

// =============================================================================
// Global stop source — single cancellation point for signal + watchdog
// =============================================================================

static std::stop_source g_stop;

static void sigint_handler(int) { g_stop.request_stop(); }

// =============================================================================
// Watchdog — safety net for RT lockups / unreachable SSH
//
// Runs on core 0 at SCHED_OTHER (normal priority) so it can always fire
// even when the hot path busy-spins at SCHED_FIFO on another core.
// After timeout it requests stop, allowing the program to exit cleanly.
// =============================================================================

static void spawn_watchdog(int timeout_sec) {
    std::thread([timeout_sec](){
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET(0, &cpuset);
        pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);

        sched_param sp{};
        sp.sched_priority = 0;
        sched_setscheduler(0, SCHED_OTHER, &sp);

        std::this_thread::sleep_for(std::chrono::seconds(timeout_sec));
        if (!g_stop.stop_requested()) {
            std::fprintf(stderr, "\n[Watchdog] %ds timeout. Stopping...\n", timeout_sec);
            g_stop.request_stop();
        }
    }).detach();
}

// =============================================================================
// Main
// =============================================================================

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::printf("Usage:\n  %s --server [--config <file>]\n  %s --client --count <N> [--config <file>]\n",
                    argv[0], argv[0]);
        return 1;
    }

    // Parse CLI args
    const char* config_path = "abtrda3_test.toml";
    const char* mode = nullptr;
    std::uint32_t count = 10;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--config") == 0 && i + 1 < argc) {
            config_path = argv[++i];
        } else if (std::strcmp(argv[i], "--server") == 0) {
            mode = "server";
        } else if (std::strcmp(argv[i], "--client") == 0) {
            mode = "client";
        } else if (std::strcmp(argv[i], "--count") == 0 && i + 1 < argc) {
            count = static_cast<std::uint32_t>(std::atoi(argv[++i]));
        }
    }

    if (!mode) {
        std::fprintf(stderr, "Error: specify --server or --client\n");
        return 1;
    }

    // Load config
    TestConfig cfg;
    try {
        cfg = loadConfig(config_path);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "Config error: %s\n", e.what());
        return 1;
    }

    std::printf("[Config] Loaded from %s\n", config_path);

    const bool is_server = std::strcmp(mode, "server") == 0;
    const RoleConfig& role = is_server ? cfg.server : cfg.client;

    // NicTuner applies all system tuning (NAPI polling, interrupt coalescing,
    // NIC offloads, RT throttling, ksoftirqd priority, IRQ affinity) and
    // restores originals on destruction.
    NicTuner tuner(role.interface.c_str(), role.cpuCore);

    if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0) {
        std::fprintf(stderr, "[Warn] mlockall failed\n");
    }

    // SCHED_FIFO:49 — below ksoftirqd (FIFO:50, set by NicTuner) so NAPI
    // can still deliver packets to the mmap ring on this RT kernel.
    {
        sched_param sp{};
        sp.sched_priority = 49;
        if (sched_setscheduler(0, SCHED_FIFO, &sp) != 0) {
            std::fprintf(stderr, "[Warn] SCHED_FIFO:49 failed: %s\n", std::strerror(errno));
        }
    }

    std::signal(SIGINT, sigint_handler);

    std::printf("[%s] Watchdog: auto-shutdown in %d seconds\n",
                is_server ? "Server" : "Client", cfg.watchdogSec);
    spawn_watchdog(cfg.watchdogSec);

    if (is_server) {
        std::printf("[Server] Listening on %s (Reflector)\n", role.interface.c_str());
        run_server(cfg, g_stop.get_token());
    } else {
        std::printf("[Client] Sending %u packets to measure RTT\n", count);
        run_client(cfg, count, g_stop.get_token());
    }

    return 0;
}
