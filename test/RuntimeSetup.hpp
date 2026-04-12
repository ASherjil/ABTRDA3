#pragma once

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <pthread.h>
#include <sched.h>
#include <stop_token>
#include <sys/mman.h>
#include <thread>

// =============================================================================
// RuntimeSetup — RT environment for the hot path
//
// Handles: mlockall, signal handler, watchdog, CPU pinning, SCHED_FIFO.
// Construct AFTER NicTuner (so watchdog inherits SCHED_OTHER, not FIFO).
// =============================================================================

class RuntimeSetup {
public:
    RuntimeSetup(int cpuCore, int watchdogSec, const char* roleName) {
        if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0)
            std::fprintf(stderr, "[Warn] mlockall failed\n");

        std::signal(SIGINT, [](int) { s_stop.request_stop(); });

        // Spawn watchdog BEFORE SCHED_FIFO so it inherits SCHED_OTHER
        std::fprintf(stderr, "[%s] Watchdog: auto-shutdown in %d seconds\n", roleName, watchdogSec);
        spawnWatchdog(watchdogSec);

        // Brief yield so watchdog thread migrates to core 0 and drops to SCHED_OTHER
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

        pinToCore(cpuCore);
        setFifo(49);
    }

    [[nodiscard]] std::stop_token stopToken() const { return s_stop.get_token(); }

private:
    // File-scope stop source shared with the signal handler
    static inline std::stop_source s_stop;

    static void spawnWatchdog(int timeoutSec) {
        std::thread([timeoutSec]() {
            // Pin watchdog to core 0 at normal priority
            cpu_set_t cpuset;
            CPU_ZERO(&cpuset);
            CPU_SET(0, &cpuset);
            pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);

            sched_param sp{};
            sp.sched_priority = 0;
            sched_setscheduler(0, SCHED_OTHER, &sp);

            std::this_thread::sleep_for(std::chrono::seconds(timeoutSec));
            if (!s_stop.stop_requested()) {
                std::fprintf(stderr, "\n[Watchdog] %ds timeout. Stopping...\n", timeoutSec);
                s_stop.request_stop();
            }
        }).detach();
    }

    static void pinToCore(int core) {
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET(core, &cpuset);
        if (sched_setaffinity(0, sizeof(cpuset), &cpuset) != 0)
            std::fprintf(stderr, "[Warn] sched_setaffinity(core %d) failed: %s\n",
                         core, std::strerror(errno));
        else
            std::fprintf(stderr, "[RT] Pinned to core %d\n", core);
    }

    static void setFifo(int priority) {
        sched_param sp{};
        sp.sched_priority = priority;
        if (sched_setscheduler(0, SCHED_FIFO, &sp) != 0)
            std::fprintf(stderr, "[Warn] SCHED_FIFO:%d failed: %s\n",
                         priority, std::strerror(errno));
    }
};
