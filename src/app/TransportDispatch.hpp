#pragma once

// TransportDispatch — pick the transport (runtime string) and run the selected mode.
// Per-transport knowledge lives in TransportTraits.hpp; the dispatch logic is written
// ONCE here, generic over those traits:
//   withTransport()      the ONE runtime-string -> compile-time-type boundary
//   runModes<Tr>()       Server / Client / TxGen / RxSink  (one role, one port)
//   runSingleFor<Tr>()   SingleRecorder                    (Tx + Rx port, one process)
// Adding a transport touches neither runner.

#include "TestConfig.hpp"
#include "RingConcepts.hpp"
#include "TransportTraits.hpp"
#include "Server.hpp"
#include "Client.hpp"
#include "TxGenerator.hpp"
#include "RxSink.hpp"
#include "SingleRecorder.hpp"

#include <fmt/core.h>

#include <cstdint>
#include <stop_token>
#include <string_view>
#include <type_traits>

enum class RunMode { Server, Client, TxGen, RxSink, SingleRecorder };

inline const char* runModeName(RunMode m) {
    switch (m) {
        case RunMode::Server:         return "Server";
        case RunMode::Client:         return "Client";
        case RunMode::TxGen:          return "TxGen";
        case RunMode::RxSink:         return "RxSink";
        case RunMode::SingleRecorder: return "SingleRecorder";
    }
    return "Unknown";
}

// ── Run one mode on concrete Tx/Rx objects ──────────────────────────────────
template<TxRing Tx, RxRing Rx>
inline void dispatchMode(Tx& tx, Rx& rx, RunMode mode, const TestConfig& cfg,
                         std::uint32_t count, std::stop_token stop) {
    switch (mode) {
        case RunMode::Server: run_server(tx, rx, cfg, stop);   break;
        case RunMode::Client: run_client(tx, rx, cfg, stop);   break;
        case RunMode::TxGen:  run_txgen(tx, cfg, count, stop); break;
        case RunMode::RxSink: run_rxsink(rx, cfg, stop);       break;
        case RunMode::SingleRecorder:                          break;   // see runSingleFor
    }
}

// ── The runtime -> compile-time boundary: the ONLY place transports are named ──
// Calls fn(std::type_identity<Traits>{}); fn must return int for every traits type.
template<class Fn>
[[nodiscard]] inline int withTransport(std::string_view name, Fn&& fn) {
    if (name == PacketMmapTraits::kName) return fn(std::type_identity<PacketMmapTraits>{});
    if (name == AfxdpTraits::kName)      return fn(std::type_identity<AfxdpTraits>{});
    if (name == DpdkTraits::kName)       return fn(std::type_identity<DpdkTraits>{});
    if (name == VerbsTraits::kName)      return fn(std::type_identity<VerbsTraits>{});
    if (name == EtherFabricTraits::kName) return fn(std::type_identity<EtherFabricTraits>{});
    if (name == I210Traits::kName)       return fn(std::type_identity<I210Traits>{});
    fmt::println(stderr, "Error: unknown transport '{}'", name);
    return 1;
}

// ── Server / Client / TxGen / RxSink — once, for every transport ────────────
// Mode -> direction is UNIVERSAL: TxGen->TxOnly, RxSink->RxOnly, Server/Client->RxTx
// (a PAIRED transport uses a separate Tx + Rx object instead of one duplex object).
template<class Tr>
[[nodiscard]] inline int runModes(const TestConfig& cfg, const RoleConfig& role,
                                  RunMode mode, std::uint32_t count, std::stop_token stop) {
    if (!Tr::preflight(cfg, role)) return 1;
    const char* roleName = runModeName(mode);

    auto bringUp = [&](auto& nic) -> bool {
        if (Tr::init(nic, cfg, role)) return true;
        fmt::println(stderr, "Error: {} init failed on {}", Tr::kName, role.interface);
        return false;
    };

    switch (mode) {
        case RunMode::TxGen: {
            auto nic = Tr::makeTx(cfg, role);
            if (!bringUp(nic)) return 1;
            Tr::banner(cfg, role, roleName);
            Tr::withAux(nic, cfg, role, [&] { run_txgen(nic, cfg, count, stop); });
            return 0;
        }
        case RunMode::RxSink: {
            auto nic = Tr::makeRx(cfg, role);
            if (!bringUp(nic)) return 1;
            Tr::banner(cfg, role, roleName);
            Tr::withAux(nic, cfg, role, [&] { run_rxsink(nic, cfg, stop); });
            return 0;
        }
        case RunMode::Server:
        case RunMode::Client: {
            if constexpr (Tr::kPairedRxTx) {
                auto tx = Tr::makeTx(cfg, role);
                auto rx = Tr::makeRx(cfg, role);
                if (!bringUp(tx) || !bringUp(rx)) return 1;
                Tr::banner(cfg, role, roleName);
                dispatchMode(tx, rx, mode, cfg, count, stop);
            } else {
                auto nic = Tr::makeRxTx(cfg, role);   // one duplex object: sends AND reflects
                if (!bringUp(nic)) return 1;
                Tr::banner(cfg, role, roleName);
                Tr::withAux(nic, cfg, role,
                            [&] { dispatchMode(nic, nic, mode, cfg, count, stop); });
            }
            return 0;
        }
        case RunMode::SingleRecorder:
            return 0;                                  // handled by runSingleRecorder
    }
    return 0;
}

[[nodiscard]]
inline int runTransport(const TestConfig& cfg, const RoleConfig& role,
                        RunMode mode, std::uint32_t count, std::stop_token stop) {
    return withTransport(role.transport, [&](auto tag) {
        using Tr = typename decltype(tag)::type;
        return runModes<Tr>(cfg, role, mode, count, stop);
    });
}

// ── SingleRecorder — one-way latency, Tx port + Rx port in ONE process ──────
// Bench owns the shared SPSC + three SCHED_OTHER threads (Tx/Rx/Hist), each self-pinned
// to its own isolated core. run() blocks until the soak completes (Tx duration -> Rx
// drains + sentinel -> Hist reports). Design: SingleRecorder.hpp.
template<TxRing Tx, RxRing Rx>
inline void driveSingleRecorder(Tx& tx, Rx& rx, const TestConfig& cfg, std::stop_token stop) {
    Bench<Tx, Rx> bench(cfg, tx, rx);
    bench.run(cfg.recDurationSec, stop);
}

template<class Tr>
[[nodiscard]] inline int runSingleFor(const TestConfig& cfg, std::stop_token stop) {
    if constexpr (!Tr::kSingleCapable) {
        fmt::println(stderr, "Error: single_recorder: transport '{}' is not supported "
                             "(needs two ports in one process)", Tr::kName);
        return 1;
    } else {
        if (!Tr::preflight(cfg, cfg.client) || !Tr::preflight(cfg, cfg.server)) return 1;
        return Tr::withSinglePair(cfg, [&](auto& tx, auto& rx) {
            driveSingleRecorder(tx, rx, cfg, stop);
        });
    }
}

[[nodiscard]]
inline int runSingleRecorder(const TestConfig& cfg, std::stop_token stop) {
    const std::string& transport = cfg.client.transport;
    if (cfg.server.transport != transport) {
        fmt::println(stderr, "Error: single_recorder needs client.transport == "
                             "server.transport (Tx=client port, Rx=server port); "
                             "got tx='{}' rx='{}'",
                     transport, cfg.server.transport);
        return 1;
    }
    if (cfg.recHotPathCore == cfg.recRxCore ||
        cfg.recHotPathCore == cfg.recRecorderCore ||
        cfg.recRxCore == cfg.recRecorderCore) {
        fmt::println(stderr, "Error: single_recorder needs three DISTINCT cores "
                             "(tx={} rx={} recorder={})",
                     cfg.recHotPathCore, cfg.recRxCore, cfg.recRecorderCore);
        return 1;
    }

    fmt::println("[SingleRecorder] {} | Tx={} (core {}) Rx={} (core {}) Hist core {} "
                 "duration={}s",
                 transport, cfg.client.interface, cfg.recHotPathCore,
                 cfg.server.interface, cfg.recRxCore, cfg.recRecorderCore,
                 cfg.recDurationSec);

    return withTransport(transport, [&](auto tag) {
        using Tr = typename decltype(tag)::type;
        return runSingleFor<Tr>(cfg, stop);
    });
}
