#pragma once

#include <fmt/core.h>

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
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
//
// Each step prints a diagnostic line BEFORE and AFTER it runs so that if
// something hangs, the last printed line tells us exactly where.
// =============================================================================

class RuntimeSetup {
public:
    RuntimeSetup(int cpuCore, int watchdogSec, const char* roleName) {
        // Step 1 — mlockall (can hang on NFS-rooted systems if pages need
        // to be faulted over the network).
        fmt::println(stderr, "[RT] step 1/6 mlockall(MCL_CURRENT | MCL_FUTURE)…");
        if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0)
            fmt::println(stderr, "[RT] step 1/6 mlockall FAILED: {}", std::strerror(errno));
        else
            fmt::println(stderr, "[RT] step 1/6 mlockall OK");

        // Step 2 — signal handler (SIGINT → cooperative stop).
        fmt::println(stderr, "[RT] step 2/6 install SIGINT handler…");
        std::signal(SIGINT, [](int) { s_stop.request_stop(); });
        fmt::println(stderr, "[RT] step 2/6 SIGINT handler OK");

        // Step 3 — watchdog thread. Spawned BEFORE SCHED_FIFO so it inherits
        // SCHED_OTHER and will be scheduled on core 0. watchdogSec <= 0 DISABLES
        // it (used by --single, whose hot loop self-terminates on duration and
        // must not be cut short by a watchdog).
        if (watchdogSec > 0) {
            fmt::println(stderr, "[{}] Watchdog: auto-shutdown in {} seconds", roleName, watchdogSec);
            fmt::println(stderr, "[RT] step 3/6 spawning watchdog thread…");
            spawnWatchdog(watchdogSec);
            fmt::println(stderr, "[RT] step 3/6 watchdog thread detached OK");
        } else {
            fmt::println(stderr, "[{}] Watchdog: DISABLED (run self-terminates)", roleName);
        }

        // Step 4 — yield briefly so watchdog actually runs and settles on core 0
        // before we hog core cpuCore with SCHED_FIFO.
        fmt::println(stderr, "[RT] step 4/6 yielding 10 ms…");
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        fmt::println(stderr, "[RT] step 4/6 yield OK");

        // ABTRDA3_RT_BYPASS: skip CPU pin + SCHED_FIFO if set.
        // Use to isolate whether a hang is in RT tuning or in the PMD itself.
        if (std::getenv("ABTRDA3_RT_BYPASS")) {
            fmt::println(stderr, "[RT] ABTRDA3_RT_BYPASS set — skipping CPU pin and SCHED_FIFO");
            return;
        }

        // Step 5 — pin main thread to the hot-path core.
        fmt::println(stderr, "[RT] step 5/6 pinning to core {}…", cpuCore);
        pinToCore(cpuCore);
        fmt::println(stderr, "[RT] step 5/6 pin OK");

        // Step 6 — promote to SCHED_FIFO. After this, any CPU hog bug on this
        // core will starve SSH / ksoftirqd / networking on the same core.
        fmt::println(stderr, "[RT] step 6/6 SCHED_FIFO prio 49…");
        setFifo(49);
        fmt::println(stderr, "[RT] step 6/6 SCHED_FIFO OK — RuntimeSetup complete");
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

            fmt::println(stderr, "[Watchdog] running on core 0 (SCHED_OTHER), will fire in {} s",
                         timeoutSec);

            std::this_thread::sleep_for(std::chrono::seconds(timeoutSec));
            if (!s_stop.stop_requested()) {
                fmt::println(stderr, "[Watchdog] {} s timeout reached — requesting stop", timeoutSec);
                s_stop.request_stop();
            } else {
                fmt::println(stderr, "[Watchdog] stop already requested, exiting cleanly");
            }
        }).detach();
    }

    static void pinToCore(int core) {
        fmt::println(stderr, "[RT]   pinToCore: building cpuset…");
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET(core, &cpuset);
        fmt::println(stderr, "[RT]   pinToCore: calling sched_setaffinity(0, &mask_with_cpu_{})…", core);
        const int rc = sched_setaffinity(0, sizeof(cpuset), &cpuset);
        fmt::println(stderr, "[RT]   pinToCore: sched_setaffinity returned {} (errno={})",
                     rc, rc == 0 ? 0 : errno);
        if (rc != 0)
            fmt::println(stderr, "[Warn] sched_setaffinity(core {}) failed: {}",
                         core, std::strerror(errno));
        else
            fmt::println(stderr, "[RT] Pinned to core {}", core);
    }

    static void setFifo(int priority) {
        sched_param sp{};
        sp.sched_priority = priority;
        if (sched_setscheduler(0, SCHED_FIFO, &sp) != 0)
            fmt::println(stderr, "[Warn] SCHED_FIFO:{} failed: {}",
                         priority, std::strerror(errno));
    }
};
