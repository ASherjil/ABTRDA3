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
// RuntimeSetup — runtime environment for the hot path
//
// Handles: mlockall, signal handler, watchdog, CPU pinning. The hot thread runs
// SCHED_OTHER (busy-poll on its isolated core): a single polling thread already
// saturates a dedicated isolated core without real-time priority, and SCHED_OTHER
// avoids the RT-throttle / per-CPU-kthread starvation risk. (Measured equal to
// SCHED_FIFO on this rig — both NICs, quiet box — so OTHER wins on simplicity:
// no RT-throttle dependency.) Construct AFTER NicTuner.
//
// The early steps print a diagnostic line before/after so that if init hangs
// (e.g. mlockall over NFS, PMD bring-up), the last printed line localises it.
// =============================================================================

class RuntimeSetup {
public:
    RuntimeSetup(int cpuCore, int watchdogSec, const char* roleName) {
        // Step 1 — mlockall (can hang on NFS-rooted systems if pages need
        // to be faulted over the network).
        fmt::println(stderr, "[RT] step 1/5 mlockall(MCL_CURRENT | MCL_FUTURE)…");
        if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0)
            fmt::println(stderr, "[RT] step 1/5 mlockall FAILED: {}", std::strerror(errno));
        else
            fmt::println(stderr, "[RT] step 1/5 mlockall OK");

        // Step 2 — signal handler (SIGINT → cooperative stop).
        fmt::println(stderr, "[RT] step 2/5 install SIGINT handler…");
        std::signal(SIGINT, [](int) { s_stop.request_stop(); });
        fmt::println(stderr, "[RT] step 2/5 SIGINT handler OK");

        // Step 3 — watchdog thread. It pins itself to core 0 at SCHED_OTHER.
        // watchdogSec <= 0 DISABLES it (used by --single, whose hot loop
        // self-terminates on duration and must not be cut short by a watchdog).
        if (watchdogSec > 0) {
            fmt::println(stderr, "[{}] Watchdog: auto-shutdown in {} seconds", roleName, watchdogSec);
            fmt::println(stderr, "[RT] step 3/5 spawning watchdog thread…");
            spawnWatchdog(watchdogSec);
            fmt::println(stderr, "[RT] step 3/5 watchdog thread detached OK");
        } else {
            fmt::println(stderr, "[{}] Watchdog: DISABLED (run self-terminates)", roleName);
        }

        // Step 4 — yield briefly so watchdog actually runs and settles on core 0
        // before we busy-poll core cpuCore (SCHED_OTHER).
        fmt::println(stderr, "[RT] step 4/5 yielding 10 ms…");
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        fmt::println(stderr, "[RT] step 4/5 yield OK");

        // ABTRDA3_RT_BYPASS: skip CPU pinning if set.
        // Use to isolate whether a hang is in RT tuning or in the PMD itself.
        if (std::getenv("ABTRDA3_RT_BYPASS")) {
            fmt::println(stderr, "[RT] ABTRDA3_RT_BYPASS set — skipping CPU pin");
            return;
        }

        // Step 5 — pin the main thread to its isolated hot-path core and run it
        // SCHED_OTHER (busy-poll). See class comment for why not SCHED_FIFO.
        fmt::println(stderr, "[RT] step 5/5 pinning to core {} (SCHED_OTHER)…", cpuCore);
        pinToCore(cpuCore);
        setSchedOther();
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
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET(core, &cpuset);
        if (sched_setaffinity(0, sizeof(cpuset), &cpuset) != 0)
            fmt::println(stderr, "[Warn] sched_setaffinity(core {}) failed: {}",
                         core, std::strerror(errno));
    }

    // Run the hot thread under SCHED_OTHER (prio 0). On an isolated core a single
    // busy-poll thread already occupies ~100% of it without real-time priority,
    // and SCHED_OTHER avoids the RT-throttle / per-CPU-kthread lockup risk.
    static void setSchedOther() {
        sched_param sp{};
        sp.sched_priority = 0;
        if (sched_setscheduler(0, SCHED_OTHER, &sp) != 0)
            fmt::println(stderr, "[Warn] SCHED_OTHER failed: {}", std::strerror(errno));
    }
};
