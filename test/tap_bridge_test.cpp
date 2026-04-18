// TapBridge standalone test — validates TAP device creation, bringUp, and
// bidirectional packet flow between the TAP fd and stubbed Tx/Rx rings.
//
// No NIC hardware involved. The physical driver (igb, macb, e1000e) stays
// bound. We only create a virtual TAP interface in the kernel and exercise
// the bridge's poll loop.
//
// What we validate:
//   - Constructor: /dev/net/tun open, TUNSETIFF, SIOCSIFFLAGS
//   - operator(): kernel→TX path (read(tap_fd) pulls packets)
//   - operator(): RX→kernel path (write(tap_fd) injects synthetic packets)
//   - jthread stop_token shutdown
//   - RAII destruction (fd close)
//
// Usage:  sudo ./tap_bridge_test [tap_name] [duration_sec]

#include "TapBridge.hpp"
#include "RingConcepts.hpp"
#include "RxFrame.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <span>
#include <thread>

// =============================================================================
// StubTx — satisfies TxRing concept. Counts committed packets, discards them.
// =============================================================================
class StubTx {
public:
    bool send(std::span<const std::uint8_t> frame) noexcept {
        if (frame.size() > m_buf.size()) return false;
        ++m_sent;
        return true;
    }

    std::uint8_t* acquire(std::uint32_t len) noexcept {
        if (len > m_buf.size()) return nullptr;
        return m_buf.data();
    }

    void commit() noexcept { ++m_sent; }

    void prefillRing(std::span<const std::uint8_t>) noexcept {}

    [[nodiscard]] std::uint64_t sent() const noexcept { return m_sent; }

private:
    alignas(64) std::array<std::uint8_t, 1518> m_buf{};
    std::uint64_t m_sent{};
};

// =============================================================================
// StubRx — satisfies RxRing concept. Supports lock-free injection of synthetic
// packets so the test can exercise the RX→kernel (write to tap_fd) path.
// =============================================================================
class StubRx {
public:
    RxFrame tryReceive() noexcept {
        if (!m_pending.load(std::memory_order_acquire)) return {};
        return RxFrame{
            .data = std::span<const std::uint8_t>{m_buf.data(), m_len},
            .sec = 0, .nsec = 0, .status = 0
        };
    }

    void release() noexcept {
        m_pending.store(false, std::memory_order_release);
        ++m_released;
    }

    // Called from the test main to inject a synthetic packet. Blocks briefly
    // if a previous packet is still pending (not yet consumed by bridge).
    void inject(std::span<const std::uint8_t> pkt) noexcept {
        while (m_pending.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
        std::memcpy(m_buf.data(), pkt.data(), pkt.size());
        m_len = pkt.size();
        m_pending.store(true, std::memory_order_release);
    }

    [[nodiscard]] std::uint64_t released() const noexcept { return m_released; }

private:
    alignas(64) std::array<std::uint8_t, 1518> m_buf{};
    std::size_t m_len{};
    std::atomic<bool> m_pending{false};
    std::uint64_t m_released{};
};

static_assert(TxRing<StubTx>, "StubTx must satisfy TxRing concept");
static_assert(RxRing<StubRx>, "StubRx must satisfy RxRing concept");

// =============================================================================
// Signal handling
// =============================================================================
static std::atomic<bool> g_stop{false};
static void signalHandler(int) { g_stop.store(true, std::memory_order_relaxed); }

// =============================================================================
// Build a synthetic broadcast ARP request — tests the RX→kernel injection path.
// Kernel will log this in `ip neigh show` if the bridge delivers it correctly.
// =============================================================================
static std::array<std::uint8_t, 42> buildSyntheticArp() {
    std::array<std::uint8_t, 42> pkt{};
    // Eth header: dst=broadcast, src=synthetic MAC, type=ARP(0x0806)
    std::memset(&pkt[0], 0xFF, 6);                                    // dst MAC
    pkt[6]=0x02; pkt[7]=0xDE; pkt[8]=0xAD; pkt[9]=0xBE; pkt[10]=0xEF; pkt[11]=0x01;
    pkt[12]=0x08; pkt[13]=0x06;                                       // EtherType ARP
    // ARP: hw=Ethernet(1), proto=IPv4(0x0800), hlen=6, plen=4, op=Request(1)
    pkt[14]=0x00; pkt[15]=0x01;
    pkt[16]=0x08; pkt[17]=0x00;
    pkt[18]=0x06; pkt[19]=0x04;
    pkt[20]=0x00; pkt[21]=0x01;
    // Sender MAC + IP (10.99.0.99)
    std::memcpy(&pkt[22], &pkt[6], 6);
    pkt[28]=10; pkt[29]=99; pkt[30]=0; pkt[31]=99;
    // Target MAC (zero) + IP (10.99.0.1)
    pkt[38]=10; pkt[39]=99; pkt[40]=0; pkt[41]=1;
    return pkt;
}

// =============================================================================
// Main
// =============================================================================
int main(int argc, char* argv[]) {
    const char* tapName = (argc > 1) ? argv[1] : "tap_pmd";
    int duration        = (argc > 2) ? std::atoi(argv[2]) : 20;

    std::signal(SIGINT,  signalHandler);
    std::signal(SIGTERM, signalHandler);

    std::printf("=== TapBridge Test ===\n");
    std::printf("TAP name:  %s\n", tapName);
    std::printf("Duration:  %d seconds\n\n", duration);

    try {
        StubTx tx;
        StubRx rx;

        TapBridge<StubTx, StubRx> bridge(tx, rx, tapName);

        std::printf("[Test] fd=%d, name=%.*s\n\n",
                    bridge.fd(),
                    static_cast<int>(bridge.name().size()), bridge.name().data());

        std::jthread bridgeThread(std::ref(bridge));

        const auto arp = buildSyntheticArp();
        const auto start = std::chrono::steady_clock::now();

        for (int sec = 0; sec < duration && !g_stop.load(std::memory_order_relaxed); ++sec) {
            std::this_thread::sleep_for(std::chrono::seconds(1));

            // Every 5 seconds, inject a synthetic ARP into StubRx —
            // validates the RX→kernel (write to tap_fd) path.
            if (sec > 0 && sec % 5 == 0) {
                std::printf("[Test] Injecting synthetic ARP → StubRx\n");
                rx.inject(std::span{arp.data(), arp.size()});
            }

            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now() - start).count();

            std::printf("[%2lds] bridge.rxCount=%lu  bridge.txCount=%lu  "
                        "stub.sent=%lu  stub.released=%lu\n",
                        elapsed, bridge.rxCount(), bridge.txCount(),
                        tx.sent(), rx.released());
        }

        std::printf("\n[Test] Requesting stop...\n");
        bridgeThread.request_stop();
        // jthread dtor joins automatically

        std::printf("[Test] Final: bridge.rx=%lu bridge.tx=%lu stub.sent=%lu stub.released=%lu\n",
                    bridge.rxCount(), bridge.txCount(), tx.sent(), rx.released());
    } catch (const std::exception& e) {
        std::fprintf(stderr, "[Test] FAILED: %s\n", e.what());
        return 1;
    }

    std::printf("[Test] PASS\n");
    return 0;
}
