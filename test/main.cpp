// ABTRDA3 - Ultra-Low Latency Ping-Pong Test
//
// Usage:
//   Server:  sudo ./abtrda3_test --server --config ../../../test/abtrda3_test.toml
//   Client:  sudo ./abtrda3_test --client --config ../../../test/abtrda3_test.toml
//   Override count: sudo ./abtrda3_test --client --count 1000 --config ../../../test/abtrda3_test.toml

#include "NicTuner.hpp"
#include "TestConfig.hpp"
#include "Server.hpp"
#include "Client.hpp"
#include "SocketOps.hpp"
#include "PacketMmapRx.hpp"
#include "PacketMmapTx.hpp"
#ifdef ABTRDA3_HAS_AF_XDP
#include "AFXDPSocket.hpp"
#include "AFXDPTx.hpp"
#include "AFXDPRx.hpp"
#endif

#include <cstdio>
#include <cstring>
#include <cerrno>
#include <csignal>
#include <stop_token>
#include <sched.h>
#include <pthread.h>
#include <sys/mman.h>
#include <thread>
#include <chrono>
#include <optional>

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

        // Hard exit if graceful shutdown hangs (e.g. bpf_xdp_detach blocks)
        std::this_thread::sleep_for(std::chrono::seconds(3));
        std::fprintf(stderr, "[Watchdog] Graceful shutdown stuck. Force exit.\n");
        std::_Exit(0);
    }).detach();
}

// =============================================================================
// Main
// =============================================================================

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::printf("Usage:\n  %s --server [--config <file>]\n  %s --client [--count <N>] [--config <file>]\n",
                    argv[0], argv[0]);
        return 1;
    }

    // Parse CLI args
    const char* config_path = "abtrda3_test.toml";
    const char* mode = nullptr;
    std::int64_t count_override = -1;  // -1 = use config value

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--config") == 0 && i + 1 < argc) {
            config_path = argv[++i];
        } else if (std::strcmp(argv[i], "--server") == 0) {
            mode = "server";
        } else if (std::strcmp(argv[i], "--client") == 0) {
            mode = "client";
        } else if (std::strcmp(argv[i], "--count") == 0 && i + 1 < argc) {
            count_override = std::atol(argv[++i]);
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

    // CLI --count overrides config value
    std::uint32_t count = cfg.clientCount;
    if (count_override >= 0)
        count = static_cast<std::uint32_t>(count_override);

    std::printf("[Config] Loaded from %s\n", config_path);

    const bool is_server = std::strcmp(mode, "server") == 0;
    const RoleConfig& role = is_server ? cfg.server : cfg.client;

    // NicTuner applies all system tuning (NAPI polling, interrupt coalescing,
    // NIC offloads, RT throttling, ksoftirqd priority, IRQ affinity) and
    // restores originals on destruction. Skipped for NFS-boot interfaces.
    std::optional<NicTuner> tuner;
    if (cfg.nicTunerMode != NicTunerMode::Off) {
        tuner.emplace(role.interface.c_str(), role.cpuCore, cfg.nicTunerMode);
    } else {
        std::fprintf(stderr, "[NicTuner] Off\n");
    }

    if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0) {
        std::fprintf(stderr, "[Warn] mlockall failed\n");
    }

    std::signal(SIGINT, sigint_handler);

    // Spawn watchdog BEFORE SCHED_FIFO. The watchdog thread inherits the
    // current (normal) scheduling policy so it can immediately migrate to
    // core 0 and sleep. If we set FIFO first, the watchdog inherits FIFO:49
    // on the same core and may never get CPU time to run its setup.
    std::printf("[%s] Watchdog: auto-shutdown in %d seconds\n",
                is_server ? "Server" : "Client", cfg.watchdogSec);
    spawn_watchdog(cfg.watchdogSec);

    // Brief yield so the watchdog thread runs its setup (move to core 0,
    // drop to SCHED_OTHER) before we go FIFO and never yield again.
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    // Pin to the configured CPU core (replaces taskset -c on the command line)
    {
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET(role.cpuCore, &cpuset);
        if (sched_setaffinity(0, sizeof(cpuset), &cpuset) != 0)
            std::fprintf(stderr, "[Warn] sched_setaffinity(core %d) failed: %s\n",
                         role.cpuCore, std::strerror(errno));
        else
            std::fprintf(stderr, "[RT] Pinned to core %d\n", role.cpuCore);
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


      // ── Transport dispatch ──────────────────────────────────────────────
      if (cfg.transport == Transport::PacketMmap) {
          // Build packet_mmap ring configs
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

          std::printf("[%s] Transport: packet_mmap on %s\n",
                      is_server ? "Server" : "Client", role.interface.c_str());

          if (is_server) {
              run_server(tx, rx, cfg, g_stop.get_token());
          } else {
              run_client(tx, rx, cfg, count, g_stop.get_token());
          }
      }
  #ifdef ABTRDA3_HAS_AF_XDP
      else if (cfg.transport == Transport::AfXdp) {
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
                      is_server ? "Server" : "Client", role.interface.c_str(),
                      role.xdpQueueId);

          if (is_server) {
              run_server(tx, rx, cfg, g_stop.get_token());
          } else {
              run_client(tx, rx, cfg, count, g_stop.get_token());
          }
      }
  #endif
      else {
          std::fprintf(stderr, "Error: unknown transport (AF_XDP not compiled in?)\n");
          return 1;
      }

      return 0;
}
