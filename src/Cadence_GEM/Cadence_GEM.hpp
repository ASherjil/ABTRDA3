#ifndef ABTRDA3_CADENCE_GEM_HPP
#define ABTRDA3_CADENCE_GEM_HPP

#include "macb.h"
#include "RxFrame.hpp"

#include "AXIBackend.hpp"
#include "DMARing.hpp"
#include "HugepageBuffer.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <span>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <utility>

// Cadence GEM register window is 4 KiB on every known variant.
constexpr std::size_t   GEM_MMIO_SIZE     = 0x1000;
// Zynq UltraScale+ has 2 DMA queues; DCFG6 is read at runtime to verify.
constexpr std::uint16_t GEM_NUM_QUEUES_HW = 2;
constexpr std::uint16_t GEM_LOCAL_EXPERIMENTAL_ETHERTYPE = 0x88B5;

enum class GEMDriverMode : std::uint8_t { RxOnly, TxOnly, RxTx };

// 64-bit DMA descriptor — macb.h provides the halves; we pack them contiguously
// so the hardware reads 16 bytes per descriptor (DMACFG.ADDR64=1).
struct alignas(16) GEMDescriptor64 {
    macb_dma_desc    base;
    macb_dma_desc_64 high;
};
static_assert(sizeof(GEMDescriptor64) == 16);
static_assert(std::is_trivially_copyable_v<GEMDescriptor64>);

template<GEMDriverMode M  = GEMDriverMode::RxTx, std::size_t NumRxDesc = 256, std::size_t NumTxDesc = 256, std::size_t BuffSize  = 2048>
class Cadence_GEM {
    static_assert(M == GEMDriverMode::TxOnly || (NumRxDesc >= 8 && (NumRxDesc & (NumRxDesc - 1)) == 0),
                  "NumRxDesc must be a power of 2 and >= 8");
    static_assert(M == GEMDriverMode::RxOnly || (NumTxDesc >= 8 && (NumTxDesc & (NumTxDesc - 1)) == 0),
                  "NumTxDesc must be a power of 2 and >= 8");
    static_assert(BuffSize == 256 || BuffSize == 512 || BuffSize == 1024 || BuffSize == 2048,
                  "BuffSize must be 256/512/1024/2048");
    static_assert((BuffSize % 64) == 0, "BuffSize must be a multiple of 64 (GEM RXBS unit)");

    static constexpr bool          HAS_RX       = (M == GEMDriverMode::RxOnly || M == GEMDriverMode::RxTx);
    static constexpr bool          HAS_TX       = (M == GEMDriverMode::TxOnly || M == GEMDriverMode::RxTx);
    static constexpr std::size_t   RX_RING_MASK = NumRxDesc - 1;
    static constexpr std::size_t   TX_RING_MASK = NumTxDesc - 1;
    static constexpr std::size_t   Q_HOT        = 1;   // priority queue — TX arbiter wins over Q0
    static constexpr std::size_t   Q_SLOW       = 0;

public:
    // If `deviceName` is non-empty (e.g. "ff0b0000.ethernet") it overrides the
    // ifname-based lookup — useful when a previous PMD run left the driver
    // unbound and /sys/class/net/<ifname> no longer exists.
    explicit Cadence_GEM(std::string_view ifname     = "end0",
                         std::string_view driverName = "macb",
                         std::string_view deviceName = {})
        : m_ifname{ifname}, m_driverName{driverName}, m_deviceName{deviceName} {}

    Cadence_GEM(const Cadence_GEM&)            = delete;
    Cadence_GEM& operator=(const Cadence_GEM&) = delete;

    Cadence_GEM(Cadence_GEM&& other) noexcept
        : m_bus{std::move(other.m_bus)},
          m_ifname{std::move(other.m_ifname)},
          m_driverName{std::move(other.m_driverName)},
          m_deviceName{std::move(other.m_deviceName)},
          m_mmioBase{std::exchange(other.m_mmioBase, 0)},
          m_rxRings{std::move(other.m_rxRings)},
          m_rxBuffers{std::move(other.m_rxBuffers)},
          m_rxTail{other.m_rxTail},
          m_txRings{std::move(other.m_txRings)},
          m_txBuffers{std::move(other.m_txBuffers)},
          m_txTail{other.m_txTail},
          m_txInFlight{other.m_txInFlight},
          m_mac{other.m_mac},
          m_unboundDriver{std::exchange(other.m_unboundDriver, false)} {}

    Cadence_GEM& operator=(Cadence_GEM&& other) noexcept {
        if (this != &other) {
            shutdown();
            m_bus           = std::move(other.m_bus);
            m_ifname        = std::move(other.m_ifname);
            m_driverName    = std::move(other.m_driverName);
            m_deviceName    = std::move(other.m_deviceName);
            m_mmioBase      = std::exchange(other.m_mmioBase, 0);
            m_rxRings       = std::move(other.m_rxRings);
            m_rxBuffers     = std::move(other.m_rxBuffers);
            m_rxTail        = other.m_rxTail;
            m_txRings       = std::move(other.m_txRings);
            m_txBuffers     = std::move(other.m_txBuffers);
            m_txTail        = other.m_txTail;
            m_txInFlight    = other.m_txInFlight;
            m_mac           = other.m_mac;
            m_unboundDriver = std::exchange(other.m_unboundDriver, false);
        }
        return *this;
    }

    ~Cadence_GEM() {
        shutdown();
        if (m_unboundDriver) {
            rebindKernelDriver();
        }
    }

    [[nodiscard]] bool init() {
        if (!resolveDeviceName()) return false;
        if (!parseMmioBase())     return false;
        if (!unbindKernelDriver()) return false;

        if (!m_bus.open("/dev/mem", m_mmioBase, GEM_MMIO_SIZE)) {
            std::fprintf(stderr, "[GEM] mmap of 0x%lx failed\n", m_mmioBase);
            return false;
        }
        std::fprintf(stderr, "[GEM] %s mapped at 0x%lx (%zu bytes)\n",
                     m_deviceName.c_str(), m_mmioBase, GEM_MMIO_SIZE);

        resetHw();
        disableInterrupts();
        readMacAddress();
        setMacAddress();

        configureNcfgr();
        configureDma();
        configureUsrio();

        if (!initRings()) {
            return false;
        }
        initBuffers();
        configureScreeners();

        if (!initPhy()) {
            return false;
        }
        enableRxTx();

        std::fprintf(stderr, "[GEM] Initialised. MAC=%02x:%02x:%02x:%02x:%02x:%02x link=%s\n",
                     m_mac[0], m_mac[1], m_mac[2], m_mac[3], m_mac[4], m_mac[5],
                     isLinkUp() ? "up" : "down");
        return true;
    }

    void shutdown() noexcept {
        if (!m_bus.isOpen()) return;
        std::uint32_t ncr = readReg(MACB_NCR);
        ncr &= ~(MACB_BIT(RE) | MACB_BIT(TE));
        writeReg(MACB_NCR, ncr);
        disableInterrupts();
    }

    // ---------------------------------------------------------------------
    // TxRing (hot path, Q_HOT)
    // ---------------------------------------------------------------------
    [[nodiscard, gnu::hot, gnu::always_inline]]
    inline std::uint8_t* acquire(std::uint32_t frameLen) noexcept requires(HAS_TX) {
        return txAcquire<Q_HOT>(frameLen);
    }
    [[gnu::hot, gnu::always_inline]]
    inline void commit() noexcept requires(HAS_TX) { txCommit<Q_HOT>(); }

    [[nodiscard]] bool send(std::span<const std::uint8_t> frame) noexcept requires(HAS_TX) {
        auto* dst = acquire(static_cast<std::uint32_t>(frame.size()));
        if (!dst) [[unlikely]] return false;
        std::memcpy(dst, frame.data(), frame.size());
        commit();
        return true;
    }

    void prefillRing(std::span<const std::uint8_t> frameTemplate) noexcept requires(HAS_TX) {
        for (std::size_t i = 0; i < NumTxDesc; ++i) {
            std::memcpy(m_txBuffers[Q_HOT].template ptrAt<std::uint8_t>(i * BuffSize),
                        frameTemplate.data(), frameTemplate.size());
        }
    }

    // ---------------------------------------------------------------------
    // RxRing (hot path, Q_HOT)
    // ---------------------------------------------------------------------
    [[nodiscard, gnu::hot, gnu::always_inline]]
    inline RxFrame tryReceive() noexcept requires(HAS_RX) {
        return rxTryReceive<Q_HOT>();
    }
    [[gnu::hot, gnu::always_inline]]
    inline void release() noexcept requires(HAS_RX) { rxRelease<Q_HOT>(); }

    // ---------------------------------------------------------------------
    // Slow path (Q0) exposed for TapBridge
    // ---------------------------------------------------------------------
    class SlowPath {
    public:
        SlowPath() = default;
        explicit SlowPath(Cadence_GEM* parent) noexcept
            : m_parent{parent} {}

        [[nodiscard]] std::uint8_t* acquire(std::uint32_t len) const noexcept {
            return m_parent->template txAcquire<Q_SLOW>(len);
        }
        void commit() noexcept {
            m_parent->template txCommit<Q_SLOW>();
        }

        [[nodiscard]] bool send(std::span<const std::uint8_t> frame) noexcept {
            auto* dst = acquire(static_cast<std::uint32_t>(frame.size()));
            if (!dst) return false;
            std::memcpy(dst, frame.data(), frame.size());
            commit();
            return true;
        }

        void prefillRing(std::span<const std::uint8_t>) noexcept {
            // No op. Exists to satify the ring concept. 
        }

        [[nodiscard]] RxFrame tryReceive() const noexcept {
            return m_parent->template rxTryReceive<Q_SLOW>();
        }

        void release() const noexcept {
            m_parent->template rxRelease<Q_SLOW>();
        }

    private:
        Cadence_GEM* m_parent{nullptr};
    };

    [[nodiscard]] SlowPath& slowPath() noexcept { return m_slowPath; }

    // ---------------------------------------------------------------------
    // Introspection
    // ---------------------------------------------------------------------
    [[nodiscard]] std::array<std::uint8_t, 6> macAddress() const noexcept { return m_mac; }

    [[nodiscard]] bool isLinkUp() noexcept {
        constexpr std::uint8_t  PHY_ADDR       = 0;
        constexpr std::uint8_t  PHY_REG_STATUS = 0x01;
        constexpr std::uint16_t LINK_UP_MASK   = 0x0004;
        return (mdioRead(PHY_ADDR, PHY_REG_STATUS) & LINK_UP_MASK) != 0;
    }

private:
    // =====================================================================
    // Register access
    // =====================================================================
    [[gnu::always_inline]]
    inline void writeReg(std::uint32_t offset, std::uint32_t value) noexcept {
        *m_bus.template registerPtr<std::uint32_t>(offset) = value;
    }
    [[nodiscard, gnu::always_inline]]
    inline std::uint32_t readReg(std::uint32_t offset) const noexcept {
        return *m_bus.template registerPtr<std::uint32_t>(offset);
    }
    [[gnu::always_inline]]
    inline void writeQueueReg(std::uint32_t base, std::size_t q, std::uint32_t v) noexcept {
        writeReg(base + static_cast<std::uint32_t>(q << 2), v);
    }

    // ARM64 DMA barriers — macb is in the Outer Shareable domain
    [[gnu::always_inline]] static inline void dmaStoreBarrier() noexcept {
#if defined(__aarch64__)
        asm volatile("dmb oshst" ::: "memory");
#else
        asm volatile("" ::: "memory");
#endif
    }
    [[gnu::always_inline]] static inline void dmaLoadBarrier() noexcept {
#if defined(__aarch64__)
        asm volatile("dmb oshld" ::: "memory");
#else
        asm volatile("" ::: "memory");
#endif
    }

    // =====================================================================
    // Kernel driver unbind/rebind (platform device via sysfs)
    // =====================================================================
    // Resolution chain:
    //   1. explicit deviceName ctor arg wins (recovery path)
    //   2. /sys/class/net/<ifname>/device      (normal case, driver bound)
    //   3. scan /sys/bus/platform/devices for a cdns,gem match (driver unbound
    //      after a crash — end0 no longer exists)
    [[nodiscard]] bool resolveDeviceName() {
        if (!m_deviceName.empty()) {
            return platformDeviceExists(m_deviceName);
        }
        if (resolveViaInterface()) return true;

        std::fprintf(stderr, "[GEM] /sys/class/net/%s missing — driver likely unbound; scanning platform bus\n",
                     m_ifname.c_str());
        return resolveViaPlatformScan();
    }

    [[nodiscard]] bool platformDeviceExists(const std::string& name) const {
        struct stat st{};
        const std::string path = "/sys/bus/platform/devices/" + name;
        if (::stat(path.c_str(), &st) == 0) return true;
        std::fprintf(stderr, "[GEM] platform device '%s' not found at %s\n",
                     name.c_str(), path.c_str());
        return false;
    }

    [[nodiscard]] bool resolveViaInterface() {
        const std::string link = "/sys/class/net/" + m_ifname + "/device";
        char resolved[256]{};
        const ssize_t n = ::readlink(link.c_str(), resolved, sizeof(resolved) - 1);
        if (n <= 0) return false;
        std::string_view view{resolved, static_cast<std::size_t>(n)};
        if (auto slash = view.find_last_of('/'); slash != std::string_view::npos)
            view.remove_prefix(slash + 1);
        m_deviceName.assign(view);
        return !m_deviceName.empty();
    }

    // Scan /sys/bus/platform/devices/ for a node whose compatible string lists
    // a Cadence GEM variant. The compatible file is a sequence of null-
    // terminated strings — we match any that looks like "cdns,gem"/"*-gem".
    [[nodiscard]] bool resolveViaPlatformScan() {
        DIR* dir = ::opendir("/sys/bus/platform/devices");
        if (!dir) return false;

        bool found = false;
        while (dirent* entry = ::readdir(dir)) {
            std::string_view name{entry->d_name};
            if (!name.ends_with(".ethernet")) continue;

            const std::string compat = std::string{"/sys/bus/platform/devices/"}
                                       + std::string{name} + "/of_node/compatible";
            if (!compatibleMatchesGem(compat)) continue;

            m_deviceName.assign(name);
            std::fprintf(stderr, "[GEM] Discovered %s via platform scan\n",
                         m_deviceName.c_str());
            found = true;
            break;
        }
        ::closedir(dir);
        return found;
    }

    [[nodiscard]] static bool compatibleMatchesGem(const std::string& path) {
        const int fd = ::open(path.c_str(), O_RDONLY);
        if (fd < 0) return false;
        char buf[512]{};
        const ssize_t n = ::read(fd, buf, sizeof(buf) - 1);
        ::close(fd);
        if (n <= 0) return false;

        // compatible is a sequence of NUL-terminated strings
        for (ssize_t i = 0; i < n; ) {
            std::string_view s{buf + i};
            if (s.find("gem") != std::string_view::npos &&
                (s.starts_with("cdns,") || s.starts_with("xlnx,") ||
                 s.starts_with("atmel,") || s.starts_with("microchip,"))) {
                return true;
            }
            i += static_cast<ssize_t>(s.size()) + 1;
        }
        return false;
    }

    // Linux platform-device naming convention is "<hex_address>.<type>",
    // e.g. "ff0b0000.ethernet". Parse the hex prefix to get the MMIO base.
    [[nodiscard]] bool parseMmioBase() {
        const auto dot = m_deviceName.find('.');
        if (dot == std::string::npos || dot == 0) {
            std::fprintf(stderr, "[GEM] device name '%s' has no address prefix\n",
                         m_deviceName.c_str());
            return false;
        }
        try {
            m_mmioBase = std::stoull(m_deviceName.substr(0, dot), nullptr, 16);
        } catch (...) {
            std::fprintf(stderr, "[GEM] cannot parse hex prefix of '%s'\n",
                         m_deviceName.c_str());
            return false;
        }
        return true;
    }

    // Check whether the platform device currently has any driver bound.
    // Returns true if /sys/bus/platform/devices/<name>/driver symlink exists.
    [[nodiscard]] bool driverIsBound() const noexcept {
        const std::string link = "/sys/bus/platform/devices/" + m_deviceName + "/driver";
        struct stat st{};
        return ::lstat(link.c_str(), &st) == 0;
    }

    [[nodiscard]] bool unbindKernelDriver() {
        if (m_driverName.empty()) return true;

        if (!driverIsBound()) {
            std::fprintf(stderr, "[GEM] %s already unbound — skipping\n",
                         m_deviceName.c_str());
            m_unboundDriver = true;         // so destructor rebinds on exit
            return true;
        }

        const std::string unbind = "/sys/bus/platform/drivers/" + m_driverName + "/unbind";
        const int fd = ::open(unbind.c_str(), O_WRONLY);
        if (fd < 0) {
            std::fprintf(stderr, "[GEM] open(%s) failed\n", unbind.c_str());
            return false;
        }
        const ssize_t w = ::write(fd, m_deviceName.data(), m_deviceName.size());
        ::close(fd);
        if (w != static_cast<ssize_t>(m_deviceName.size())) return false;

        m_unboundDriver = true;
        std::fprintf(stderr, "[GEM] Unbound %s from %s\n",
                     m_deviceName.c_str(), m_driverName.c_str());
        return true;
    }

    void rebindKernelDriver() noexcept {
        if (m_driverName.empty() || m_deviceName.empty()) return;
        const std::string bind = "/sys/bus/platform/drivers/" + m_driverName + "/bind";
        const int fd = ::open(bind.c_str(), O_WRONLY);
        if (fd < 0) return;
        (void)::write(fd, m_deviceName.data(), m_deviceName.size());
        ::close(fd);
        m_unboundDriver = false;
        std::fprintf(stderr, "[GEM] Rebound %s to %s\n",
                     m_deviceName.c_str(), m_driverName.c_str());
    }

    // =====================================================================
    // Hardware reset + interrupt mask
    // =====================================================================
    void resetHw() noexcept {
        std::uint32_t ncr = readReg(MACB_NCR);
        ncr &= ~(MACB_BIT(RE) | MACB_BIT(TE));
        ncr |=  MACB_BIT(CLRSTAT);
        writeReg(MACB_NCR, ncr);

        writeReg(MACB_TSR, 0xFFFFFFFFU);
        writeReg(MACB_RSR, 0xFFFFFFFFU);
        writeReg(GEM_PBUFRXCUT, 0);
    }

    void disableInterrupts() noexcept {
        for (std::size_t q = 0; q < GEM_NUM_QUEUES_HW; ++q) {
            writeQueueReg(GEM_IDR(0), q, 0xFFFFFFFFU);
            writeQueueReg(GEM_ISR(0), q, 0xFFFFFFFFU);
        }
    }

    // =====================================================================
    // MAC address
    // =====================================================================
    void readMacAddress() noexcept {
        const std::uint32_t bottom = readReg(GEM_SA1B);
        const std::uint32_t top    = readReg(GEM_SA1T);
        m_mac[0] = static_cast<std::uint8_t>( bottom        & 0xFF);
        m_mac[1] = static_cast<std::uint8_t>((bottom >>  8) & 0xFF);
        m_mac[2] = static_cast<std::uint8_t>((bottom >> 16) & 0xFF);
        m_mac[3] = static_cast<std::uint8_t>((bottom >> 24) & 0xFF);
        m_mac[4] = static_cast<std::uint8_t>( top           & 0xFF);
        m_mac[5] = static_cast<std::uint8_t>((top    >>  8) & 0xFF);
    }

    void setMacAddress() noexcept {
        const std::uint32_t bottom =
              static_cast<std::uint32_t>(m_mac[0])
            | static_cast<std::uint32_t>(m_mac[1]) <<  8
            | static_cast<std::uint32_t>(m_mac[2]) << 16
            | static_cast<std::uint32_t>(m_mac[3]) << 24;
        const std::uint32_t top =
              static_cast<std::uint32_t>(m_mac[4])
            | static_cast<std::uint32_t>(m_mac[5]) <<  8;
        writeReg(GEM_SA1B, bottom);
        writeReg(GEM_SA1T, top);
        writeReg(GEM_SA2B, 0); writeReg(GEM_SA2T, 0);
        writeReg(GEM_SA3B, 0); writeReg(GEM_SA3T, 0);
        writeReg(GEM_SA4B, 0); writeReg(GEM_SA4T, 0);
    }

    // =====================================================================
    // NCFGR / DMACFG / USRIO
    // =====================================================================
    void configureNcfgr() noexcept {
        std::uint32_t cfg = 0;
        cfg |= GEM_BF(CLK, GEM_CLK_DIV128);
        cfg |= MACB_BF(RBOF, 2);                      // RBOF is a MACB-common field
        cfg |= MACB_BIT(DRFCS);
        cfg |= MACB_BIT(BIG);
        cfg |= MACB_BIT(FD);
        cfg |= GEM_BIT(GBE);
        cfg |= GEM_BIT(RXCOEN);
        cfg |= GEM_BF(DBW, GEM_DBW64);
        writeReg(GEM_NCFGR, cfg);
    }

    void configureDma() noexcept {
        std::uint32_t cfg = 0;
        cfg |= GEM_BF(FBLDO, 16);
        cfg |= GEM_BF(RXBS, BuffSize / 64);
        cfg |= GEM_BIT(TXPBMS);
        cfg |= GEM_BF(RXBMS, -1L);
        cfg |= GEM_BIT(ADDR64);
        writeReg(GEM_DMACFG, cfg);

        for (std::size_t q = 0; q < GEM_NUM_QUEUES_HW; ++q) {
            writeQueueReg(GEM_RBQS(0), q, BuffSize / 64);
        }
    }

    void configureUsrio() noexcept {
        writeReg(GEM_USRIO, 0x00000001U);
    }

    // =====================================================================
    // Descriptor rings
    // =====================================================================
    [[nodiscard]] bool initRings() {
        for (std::size_t q = 0; q < GEM_NUM_QUEUES_HW; ++q) {
            if constexpr (HAS_RX) {
                if (!m_rxRings[q].allocate(NumRxDesc))                   return fail("RX ring", q);
                if (!m_rxBuffers[q].allocate(NumRxDesc * BuffSize))      return fail("RX buffers", q);
            }
            if constexpr (HAS_TX) {
                if (!m_txRings[q].allocate(NumTxDesc))                   return fail("TX ring", q);
                if (!m_txBuffers[q].allocate(NumTxDesc * BuffSize))      return fail("TX buffers", q);
            }
        }

        // Cold-path init — runtime queue iteration. The hot-path descriptor
        // accessors (rxDesc<Q>/txDesc<Q>) are compile-time-only; here we walk
        // the rings directly through the hugepage buffers.
        for (std::size_t q = 0; q < GEM_NUM_QUEUES_HW; ++q) {
            if constexpr (HAS_TX) {
                auto* tx = static_cast<GEMDescriptor64*>(m_txRings[q].getHugepageBuffer());
                for (std::size_t i = 0; i < NumTxDesc; ++i) {
                    tx[i].base.addr  = 0;
                    tx[i].high.addrh = 0;
                    tx[i].high.resvd = 0;
                    std::uint32_t ctrl = MACB_BIT(TX_USED);
                    if (i == NumTxDesc - 1) ctrl |= MACB_BIT(TX_WRAP);
                    tx[i].base.ctrl = ctrl;
                }
            }
            if constexpr (HAS_RX) {
                auto* rx = static_cast<GEMDescriptor64*>(m_rxRings[q].getHugepageBuffer());
                for (std::size_t i = 0; i < NumRxDesc; ++i) {
                    const std::uint64_t buf = m_rxBuffers[q].physicalAddrAt(i * BuffSize);
                    std::uint32_t addrLo = static_cast<std::uint32_t>(buf & ~0x3ULL);
                    if (i == NumRxDesc - 1) addrLo |= MACB_BIT(RX_WRAP);
                    rx[i].base.addr  = addrLo;
                    rx[i].base.ctrl  = 0;
                    rx[i].high.addrh = static_cast<std::uint32_t>(buf >> 32);
                    rx[i].high.resvd = 0;
                }
            }
        }
        dmaStoreBarrier();
        return true;
    }

    void initBuffers() noexcept {
        if constexpr (HAS_RX) {
            writeReg(MACB_RBQPH, static_cast<std::uint32_t>(m_rxRings[0].physicalBase() >> 32));
            for (std::size_t q = 0; q < GEM_NUM_QUEUES_HW; ++q) {
                const std::uint64_t b = m_rxRings[q].physicalBase();
                writeQueueReg(GEM_RBQP(0), q, static_cast<std::uint32_t>(b & 0xFFFFFFFFU));
            }
        }
        if constexpr (HAS_TX) {
            writeReg(MACB_TBQPH, static_cast<std::uint32_t>(m_txRings[0].physicalBase() >> 32));
            for (std::size_t q = 0; q < GEM_NUM_QUEUES_HW; ++q) {
                const std::uint64_t b = m_txRings[q].physicalBase();
                writeQueueReg(GEM_TBQP(0), q, static_cast<std::uint32_t>(b & 0xFFFFFFFFU));
            }
        }
    }

    // =====================================================================
    // Screener — steer our EtherType to Q_HOT, everything else to Q_SLOW
    // =====================================================================
    void configureScreeners() noexcept {
        // GEM_ETHT and GEM_SCRT2 are base addresses; each entry is 4 bytes.
        constexpr std::uint32_t ETHT_IDX  = 0;
        constexpr std::uint32_t SCR2_SLOT = 0;

        writeReg(GEM_ETHT  + (ETHT_IDX  << 2), GEM_LOCAL_EXPERIMENTAL_ETHERTYPE);
        writeReg(GEM_SCRT2 + (SCR2_SLOT << 2),
                   GEM_BF(QUEUE,    Q_HOT)
                 | GEM_BF(ETHT2IDX, ETHT_IDX)
                 | GEM_BIT(ETHTEN));
    }

    // =====================================================================
    // MDIO — Clause 22 reads/writes via MACB_MAN + NSR.IDLE polling
    // =====================================================================
    [[nodiscard]] bool mdioWaitIdle() noexcept {
        for (int i = 0; i < 100000; ++i) {
            if (readReg(MACB_NSR) & MACB_BIT(IDLE)) return true;
        }
        return false;
    }

    [[nodiscard]] std::uint16_t mdioRead(std::uint8_t phyAddr, std::uint8_t regAddr) noexcept {
        if (!mdioWaitIdle()) return 0xFFFF;
        const std::uint32_t man =
              MACB_BF(SOF,  1)     // Clause 22 start-of-frame
            | MACB_BF(RW,   2)     // read
            | MACB_BF(PHYA, phyAddr & 0x1F)
            | MACB_BF(REGA, regAddr & 0x1F)
            | MACB_BF(CODE, 2);
        writeReg(MACB_MAN, man);
        if (!mdioWaitIdle()) return 0xFFFF;
        return static_cast<std::uint16_t>(readReg(MACB_MAN) & 0xFFFFU);
    }

    [[nodiscard]] bool mdioWrite(std::uint8_t phyAddr, std::uint8_t regAddr, std::uint16_t data) noexcept {
        if (!mdioWaitIdle()) return false;
        const std::uint32_t man =
              MACB_BF(SOF,  1)
            | MACB_BF(RW,   1)     // write
            | MACB_BF(PHYA, phyAddr & 0x1F)
            | MACB_BF(REGA, regAddr & 0x1F)
            | MACB_BF(CODE, 2)
            | MACB_BF(DATA, data);
        writeReg(MACB_MAN, man);
        return mdioWaitIdle();
    }

    [[nodiscard]] bool initPhy() noexcept {
        std::uint32_t ncr = readReg(MACB_NCR);
        ncr |= MACB_BIT(MPE);
        writeReg(MACB_NCR, ncr);

        for (int i = 0; i < 500; ++i) {
            if (isLinkUp()) {
                std::fprintf(stderr, "[GEM] Link up\n");
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        std::fprintf(stderr, "[GEM] Warning: link not up after 5s (continuing)\n");
        return true;
    }

    void enableRxTx() noexcept {
        std::uint32_t ncr = readReg(MACB_NCR);
        ncr |= MACB_BIT(MPE);
        if constexpr (HAS_RX) ncr |= MACB_BIT(RE);
        if constexpr (HAS_TX) ncr |= MACB_BIT(TE);
        writeReg(MACB_NCR, ncr);
    }

    // =====================================================================
    // Hot/slow path — templated on queue index so the choice is compile-time.
    // Each public surface (Cadence_GEM::acquire() / SlowPath::acquire()) binds
    // a specific Q at its call site: Q_HOT for our EtherType traffic on Q1,
    // Q_SLOW for kernel traffic on Q0 via the TAP bridge.
    // =====================================================================
    template<std::size_t Q>
    [[nodiscard, gnu::always_inline]]
    inline GEMDescriptor64& rxDesc(std::size_t i) noexcept {
        static_assert(Q < GEM_NUM_QUEUES_HW, "queue index out of range");
        return static_cast<GEMDescriptor64*>(m_rxRings[Q].getHugepageBuffer())[i];
    }
    template<std::size_t Q>
    [[nodiscard, gnu::always_inline]]
    inline GEMDescriptor64& txDesc(std::size_t i) noexcept {
        static_assert(Q < GEM_NUM_QUEUES_HW, "queue index out of range");
        return static_cast<GEMDescriptor64*>(m_txRings[Q].getHugepageBuffer())[i];
    }

    template<std::size_t Q>
    [[nodiscard, gnu::hot]]
    RxFrame rxTryReceive() noexcept {
        static_assert(Q < GEM_NUM_QUEUES_HW);
        const std::size_t tail = m_rxTail[Q];
        auto& d = rxDesc<Q>(tail);

        // Volatile read — HW writes the USED bit by DMA; without volatile the
        // compiler may hoist this out of a polling loop.
        const std::uint32_t addr = *reinterpret_cast<volatile const std::uint32_t*>(&d.base.addr);
        if (!(addr & MACB_BIT(RX_USED))) [[likely]] return {};

        dmaLoadBarrier();
        const std::uint32_t ctrl = d.base.ctrl;
        const std::uint32_t len  = ctrl & MACB_RX_FRMLEN_MASK;

        return {
            .data   = { m_rxBuffers[Q].template ptrAt<std::uint8_t>(tail * BuffSize), len },
            .sec    = 0,
            .nsec   = 0,
            .status = 1,
        };
    }

    template<std::size_t Q>
    [[gnu::hot]]
    void rxRelease() noexcept {
        static_assert(Q < GEM_NUM_QUEUES_HW);
        const std::size_t tail = m_rxTail[Q];
        auto& d = rxDesc<Q>(tail);

        const bool wrap = (tail == NumRxDesc - 1);
        const std::uint64_t buf = m_rxBuffers[Q].physicalAddrAt(tail * BuffSize);
        std::uint32_t addr = static_cast<std::uint32_t>(buf & ~0x3ULL);
        if (wrap) addr |= MACB_BIT(RX_WRAP);

        d.base.ctrl  = 0;
        d.high.addrh = static_cast<std::uint32_t>(buf >> 32);
        dmaStoreBarrier();
        d.base.addr  = addr;                 // clears RX_USED — HW can refill
        dmaStoreBarrier();

        m_rxTail[Q] = (tail + 1) & RX_RING_MASK;
    }

    template<std::size_t Q>
    [[nodiscard, gnu::hot]]
    std::uint8_t* txAcquire(std::uint32_t frameLen) noexcept {
        static_assert(Q < GEM_NUM_QUEUES_HW);
        if (m_txInFlight[Q] >= NumTxDesc) [[unlikely]] {
            txReclaim<Q>();
            if (m_txInFlight[Q] >= NumTxDesc) [[unlikely]] return nullptr;
        }

        const std::size_t tail = m_txTail[Q];
        auto& d = txDesc<Q>(tail);
        const std::uint64_t buf = m_txBuffers[Q].physicalAddrAt(tail * BuffSize);
        d.base.addr  = static_cast<std::uint32_t>(buf & 0xFFFFFFFFU);
        d.high.addrh = static_cast<std::uint32_t>(buf >> 32);
        d.high.resvd = 0;
        m_txPendingLen[Q] = frameLen;

        return m_txBuffers[Q].template ptrAt<std::uint8_t>(tail * BuffSize);
    }

    template<std::size_t Q>
    [[gnu::hot]]
    void txCommit() noexcept {
        static_assert(Q < GEM_NUM_QUEUES_HW);
        const std::size_t tail = m_txTail[Q];
        auto& d = txDesc<Q>(tail);

        const bool wrap = (tail == NumTxDesc - 1);
        std::uint32_t ctrl = (m_txPendingLen[Q] & 0x3FFF) | MACB_BIT(TX_LAST);
        if (wrap) ctrl |= MACB_BIT(TX_WRAP);

        // Buffer writes must land before HW observes the cleared USED bit.
        dmaStoreBarrier();
        d.base.ctrl = ctrl;                  // USED=0 signals HW
        dmaStoreBarrier();

        m_txTail[Q]     = (tail + 1) & TX_RING_MASK;
        m_txInFlight[Q] = m_txInFlight[Q] + 1;

        // Edge-triggered kick. Harmless if HW is already running.
        writeReg(MACB_NCR, readReg(MACB_NCR) | MACB_BIT(TSTART));
    }

    template<std::size_t Q>
    void txReclaim() noexcept {
        static_assert(Q < GEM_NUM_QUEUES_HW);
        const std::size_t head = (m_txTail[Q] - m_txInFlight[Q]) & TX_RING_MASK;
        std::size_t scan  = head;
        std::size_t freed = 0;
        while (freed < m_txInFlight[Q]) {
            auto& d = txDesc<Q>(scan);
            const std::uint32_t ctrl =
                *reinterpret_cast<volatile const std::uint32_t*>(&d.base.ctrl);
            if (!(ctrl & MACB_BIT(TX_USED))) break;     // oldest descriptor not yet sent
            ++freed;
            scan = (scan + 1) & TX_RING_MASK;
        }
        m_txInFlight[Q] -= freed;
    }

    [[nodiscard]] bool fail(const char* what, std::size_t q) noexcept {
        std::fprintf(stderr, "[GEM] %s alloc failed (queue %zu)\n", what, q);
        return false;
    }

    // =====================================================================
    // Members
    // =====================================================================
    AXIBackend    m_bus;
    std::string   m_ifname;
    std::string   m_driverName;
    std::string   m_deviceName;
    std::uint64_t m_mmioBase{};                  // parsed from device name

    std::array<DMARing<GEMDescriptor64>, GEM_NUM_QUEUES_HW> m_rxRings;
    std::array<HugepageBuffer,           GEM_NUM_QUEUES_HW> m_rxBuffers;
    std::array<std::size_t,              GEM_NUM_QUEUES_HW> m_rxTail{};

    std::array<DMARing<GEMDescriptor64>, GEM_NUM_QUEUES_HW> m_txRings;
    std::array<HugepageBuffer,           GEM_NUM_QUEUES_HW> m_txBuffers;
    std::array<std::size_t,              GEM_NUM_QUEUES_HW> m_txTail{};
    std::array<std::size_t,              GEM_NUM_QUEUES_HW> m_txInFlight{};
    std::array<std::uint32_t,            GEM_NUM_QUEUES_HW> m_txPendingLen{};

    std::array<std::uint8_t, 6> m_mac{};
    bool                        m_unboundDriver{false};

    SlowPath m_slowPath{this};
};

#endif // ABTRDA3_CADENCE_GEM_HPP
