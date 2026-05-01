#ifndef ABTRDA3_CADENCE_GEM_HPP
#define ABTRDA3_CADENCE_GEM_HPP

#include "macb.h"
#include "RxFrame.hpp"

#include "AXIBackend.hpp"
#include "DMARing.hpp"
#include "HugepageBuffer.hpp"

#include <fmt/core.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <strings.h>          // strcasecmp (POSIX, not in std::)
#include <dirent.h>
#include <fcntl.h>
#include <span>
#include <string>
#include <string_view>
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <net/route.h>
#include <netinet/in.h>
#include <sched.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
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

// =============================================================================
// CoherentDmaPool — userspace handle for the gem_uio kernel module's coherent
// DMA buffer (uio mem[1]).  ARM with non-coherent DMA cannot safely back
// 16-byte descriptors with cached memory: a CPU writeback covers the whole
// 64-byte cache line and stomps HW writes to neighbouring descriptors.  The
// kernel solves this with dma_alloc_coherent (= non-cached on this platform);
// our gem_uio module exposes that buffer to userspace and we mmap it here.
// =============================================================================
class CoherentDmaPool {
public:
    CoherentDmaPool() = default;
    CoherentDmaPool(const CoherentDmaPool&)            = delete;
    CoherentDmaPool& operator=(const CoherentDmaPool&) = delete;

    [[nodiscard]] bool open(const std::string& deviceName) noexcept {
        // 1. Find /sys/bus/platform/devices/<dev>/uio/uio<N>
        const std::string uioDir = "/sys/bus/platform/devices/" + deviceName + "/uio";
        DIR* d = ::opendir(uioDir.c_str());
        if (!d) {
            fmt::println(stderr, "[CoherentDma] opendir({}): {}", uioDir, std::strerror(errno));
            return false;
        }
        std::string uioName;
        while (auto* e = ::readdir(d)) {
            if (std::strncmp(e->d_name, "uio", 3) == 0 && e->d_name[3] != '\0') {
                uioName = e->d_name;
                break;
            }
        }
        ::closedir(d);
        if (uioName.empty()) {
            fmt::println(stderr, "[CoherentDma] no uio entry under {}", uioDir);
            return false;
        }

        // 2. Read /sys/class/uio/uio<N>/maps/map1/{addr,size}
        const std::string mapDir = "/sys/class/uio/" + uioName + "/maps/map1";
        const auto readNumber = [&](const std::string& fname) -> std::uint64_t {
            const std::string path = mapDir + "/" + fname;
            const int fd = ::open(path.c_str(), O_RDONLY);
            if (fd < 0) return 0;
            char buf[32]{};
            (void)::read(fd, buf, sizeof(buf) - 1);
            ::close(fd);
            return std::strtoull(buf, nullptr, 0);
        };
        m_paddr = readNumber("addr");
        m_size  = readNumber("size");
        if (m_paddr == 0 || m_size == 0) {
            fmt::println(stderr, "[CoherentDma] {}/map1 missing addr/size", mapDir);
            return false;
        }

        // 3. mmap from /dev/uio<N> at offset = page_size (UIO map[1] index)
        const std::string devPath = "/dev/" + uioName;
        m_fd = ::open(devPath.c_str(), O_RDWR | O_SYNC);
        if (m_fd < 0) {
            fmt::println(stderr, "[CoherentDma] open({}): {}", devPath, std::strerror(errno));
            return false;
        }
        const long pageSize = ::sysconf(_SC_PAGESIZE);
        m_vaddr = ::mmap(nullptr, m_size, PROT_READ | PROT_WRITE, MAP_SHARED,
                         m_fd, static_cast<off_t>(1L * pageSize));
        if (m_vaddr == MAP_FAILED) {
            fmt::println(stderr, "[CoherentDma] mmap({}, off={}): {}",
                         devPath, pageSize, std::strerror(errno));
            ::close(m_fd);
            m_fd = -1;
            m_vaddr = nullptr;
            return false;
        }

        fmt::println(stderr,
            "[CoherentDma] {} mapped: paddr=0x{:x} vaddr={} size={} (non-cached, kernel-coherent)",
            devPath, m_paddr, m_vaddr, m_size);
        return true;
    }

    void close() noexcept {
        if (m_vaddr && m_vaddr != MAP_FAILED) {
            ::munmap(m_vaddr, m_size);
            m_vaddr = nullptr;
        }
        if (m_fd >= 0) {
            ::close(m_fd);
            m_fd = -1;
        }
    }

    ~CoherentDmaPool() { close(); }

    [[nodiscard]] void* virtAt(std::size_t offset) const noexcept {
        return static_cast<std::uint8_t*>(m_vaddr) + offset;
    }
    [[nodiscard]] std::uint64_t physAt(std::size_t offset) const noexcept {
        return m_paddr + offset;
    }
    [[nodiscard]] std::size_t   size() const noexcept { return m_size; }
    [[nodiscard]] bool          isOpen() const noexcept { return m_vaddr != nullptr; }

private:
    int           m_fd{-1};
    void*         m_vaddr{nullptr};
    std::uint64_t m_paddr{0};
    std::size_t   m_size{0};
};

// View of one descriptor ring inside a CoherentDmaPool.  Carries the same
// API surface as the prior DMARing<GEMDescriptor64> usage so the rest of
// the PMD doesn't need to know which backing store is in use.
struct DescRingSlice {
    GEMDescriptor64* virt{nullptr};
    std::uint64_t    phys{0};

    [[nodiscard]] GEMDescriptor64* getHugepageBuffer() const noexcept { return virt; }
    [[nodiscard]] std::uint64_t    physicalBase()     const noexcept { return phys; }
};

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
          m_unboundDriver{std::exchange(other.m_unboundDriver, false)},
          m_savedAddr{std::move(other.m_savedAddr)},
          m_savedGateway{std::move(other.m_savedGateway)} {}

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
            m_savedAddr     = std::move(other.m_savedAddr);
            m_savedGateway  = std::move(other.m_savedGateway);
        }
        return *this;
    }

    ~Cadence_GEM() {
        shutdown();
        if (m_unboundDriver) {
            resetForRebind();
            // Drop SCHED_FIFO + CPU pin BEFORE rebind.  macb_probe
            // runs in our process context; it triggers uevent helpers,
            // PHY driver probes, and workqueue items that are
            // SCHED_NORMAL — they cannot preempt SCHED_FIFO, causing
            // a priority-inversion deadlock inside the kernel.
            {
                sched_param sp{};
                sp.sched_priority = 0;
                sched_setscheduler(0, SCHED_OTHER, &sp);
                cpu_set_t mask;
                CPU_ZERO(&mask);
                const long n = ::sysconf(_SC_NPROCESSORS_ONLN);
                for (long c = 0; c < (n > 0 ? n : CPU_SETSIZE); ++c) CPU_SET(c, &mask);
                ::sched_setaffinity(0, sizeof(mask), &mask);
            }
            // Close our /dev/mem mapping so macb_probe's
            // devm_ioremap_resource doesn't see a conflicting mapping.
            m_bus.close();

            // Unbind gem_uio (closes pool, frees coherent DMA, gates clocks
            // off via CCF) and clear driver_override so macb can rebind.
            unbindGemUio();

            rebindKernelDriver();
            // rebindKernelDriver() sets m_unboundDriver = false on success.
            // Only run the network/NFS recovery if rebind actually worked —
            // otherwise end0 isn't back and there's nothing to reconfigure.
            if (!m_unboundDriver) {
                restoreNetworkAndNfs();
            } else {
                fmt::println(stderr,
                    "[GEM] destructor: rebind failed — skipping network/NFS restore");
            }
        }
    }

    [[nodiscard]] bool init() {
        if (!resolveDeviceName()) return false;
        if (!parseMmioBase())     return false;
        if (!m_bus.open("/dev/mem", m_mmioBase, GEM_MMIO_SIZE)) {
            fmt::println(stderr, "[GEM] mmap of 0x{:x} failed", m_mmioBase);
            return false;
        }

        saveNetworkConfig();
        stopMonitoringDaemons();
        stopNetworkd();

        if (!unbindKernelDriver()) return false;
        if (!bindGemUio()) {
            fmt::println(stderr, "[GEM] gem_uio bind failed — `sudo insmod gem_uio.ko` first");
            return false;
        }
        if (!m_descPool.open(m_deviceName)) {
            fmt::println(stderr, "[GEM] coherent DMA pool open failed");
            return false;
        }

        resetHw();
        disableInterrupts();
        readMacAddress();
        setMacAddress();
        configureNcfgr();
        configureDma();
        configureUsrio();

        if (!initRings()) return false;
        initBuffers();
        configureScreeners();

        if (!initPhy()) return false;
        enableRxTx();

        fmt::println(stderr,
            "[GEM] init OK — {} @ 0x{:x}, MAC {:02x}:{:02x}:{:02x}:{:02x}:{:02x}:{:02x}, link {}",
            m_deviceName, m_mmioBase,
            m_mac[0], m_mac[1], m_mac[2], m_mac[3], m_mac[4], m_mac[5],
            isLinkUp() ? "up" : "down");

        return true;
    }

    void shutdown() noexcept {
        if (!m_bus.isOpen()) return;
        // Reap any TX descriptors HW finished after the dispatcher's drain
        // sleep — without this, m_txInFlight stays inflated by whatever was
        // outstanding at the last successful txCommit and the counter dump
        // shows e.g. tx_inflight=256 even though every packet hit the wire.
        if constexpr (HAS_TX) {
            txReclaim<Q_HOT>();
            txReclaim<Q_SLOW>();
        }
        dumpHwCounters();
        std::uint32_t ncr = readReg(MACB_NCR);
        ncr &= ~(MACB_BIT(RE) | MACB_BIT(TE));
        writeReg(MACB_NCR, ncr);
        disableInterrupts();
    }


    // Print live silicon counters and per-queue state, alongside our own
    // SW view. Run this BEFORE macb rebind — macb_reset_hw() wipes stats.
    void dumpHwCounters() const noexcept {
        if (!m_bus.isOpen()) return;

        const std::uint32_t txFrames = readReg(GEM_TXCNT);
        const std::uint32_t txOctets = readReg(GEM_OCTTXL);
        const std::uint32_t rxFrames = readReg(GEM_RXCNT);
        const std::uint32_t rxOctets = readReg(GEM_OCTRXL);

        const std::uint32_t rxBcast   = readReg(GEM_RXBROADCNT);
        const std::uint32_t rxMcast   = readReg(GEM_RXMULTICNT);
        const std::uint32_t rxPause   = readReg(GEM_RXPAUSECNT);
        const std::uint32_t rxJab     = readReg(GEM_RXJABCNT);
        const std::uint32_t rxFcs     = readReg(GEM_RXFCSCNT);
        const std::uint32_t rxAlign   = readReg(GEM_RXALIGNCNT);
        const std::uint32_t rxResErr  = readReg(GEM_RXRESERRCNT);
        const std::uint32_t rxOver    = readReg(GEM_RXORCNT);

        const std::uint32_t ncr      = readReg(MACB_NCR);
        const std::uint32_t tsr      = readReg(MACB_TSR);
        const std::uint32_t rsr      = readReg(MACB_RSR);
        const std::uint32_t dmacfg   = readReg(GEM_DMACFG);

        const std::uint32_t tbqpQ0   = readReg(qTBQP(0));
        const std::uint32_t tbqpQ1   = readReg(qTBQP(1));
        const std::uint32_t rbqpQ0   = readReg(qRBQP(0));
        const std::uint32_t rbqpQ1   = readReg(qRBQP(1));

        fmt::println(stderr,
            "[GEM] HW counters (live silicon, before teardown):\n"
            "  Frames    TX={:<8} RX={:<8}   Octets TX={:<10} RX={}\n"
            "  RX errs   bcast={} mcast={} pause={} jab={} fcs={} align={} resErr={} over={}\n"
            "  Control   NCR=0x{:08x}  TSR=0x{:08x}  RSR=0x{:08x}  DMACFG=0x{:08x}\n"
            "  Q0 regs   TBQP=0x{:08x}  RBQP=0x{:08x}\n"
            "  Q1 regs   TBQP=0x{:08x}  RBQP=0x{:08x}\n"
            "  SW  Q0    tx_tail={:<4} tx_inflight={:<4} rx_tail={}\n"
            "  SW  Q1    tx_tail={:<4} tx_inflight={:<4} rx_tail={}",
            txFrames, rxFrames, txOctets, rxOctets,
            rxBcast, rxMcast, rxPause, rxJab, rxFcs, rxAlign, rxResErr, rxOver,
            ncr, tsr, rsr, dmacfg,
            tbqpQ0, rbqpQ0,
            tbqpQ1, rbqpQ1,
            m_txTail[Q_SLOW], m_txInFlight[Q_SLOW], m_rxTail[Q_SLOW],
            m_txTail[Q_HOT],  m_txInFlight[Q_HOT],  m_rxTail[Q_HOT]);
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
        if (m_phyAddr < 0) return false;
        return (mdioRead(static_cast<std::uint8_t>(m_phyAddr), 0x01) & 0x0004) != 0;
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
    // GEM/MACB has a historical quirk: hw_q 0 uses the ORIGINAL single-queue
    // registers (MACB_TBQP/RBQP/ISR/IDR at low offsets), and hw_q >= 1 uses
    // the per-queue array added later (GEM_TBQP(hw_q - 1) at 0x440+, etc.).
    // Ref: kernel drivers/net/ethernet/cadence/macb_main.c:4612-4628.
    [[nodiscard, gnu::always_inline]]
    static constexpr std::uint32_t qTBQP(std::size_t q) noexcept {
        return q == 0 ? MACB_TBQP : (GEM_TBQP(0) + static_cast<std::uint32_t>((q - 1) << 2));
    }
    [[nodiscard, gnu::always_inline]]
    static constexpr std::uint32_t qRBQP(std::size_t q) noexcept {
        return q == 0 ? MACB_RBQP : (GEM_RBQP(0) + static_cast<std::uint32_t>((q - 1) << 2));
    }
    [[nodiscard, gnu::always_inline]]
    static constexpr std::uint32_t qISR(std::size_t q) noexcept {
        return q == 0 ? MACB_ISR : (GEM_ISR(0) + static_cast<std::uint32_t>((q - 1) << 2));
    }
    [[nodiscard, gnu::always_inline]]
    static constexpr std::uint32_t qIDR(std::size_t q) noexcept {
        return q == 0 ? MACB_IDR : (GEM_IDR(0) + static_cast<std::uint32_t>((q - 1) << 2));
    }

    // ── ARM64 cache maintenance for non-coherent DMA (Zynq GEM) ────────
    // Zynq UltraScale+ GEM DMA traverses the LPD interconnect without
    // snooping CCI-400, so CPU cache is invisible to the MAC.
    // We use DC CVAC (clean to Point of Coherency = DRAM) to push CPU
    // writes out, and DC CIVAC (clean+invalidate) to drop stale lines
    // before reading DMA-written data.  DSB ensures completion before
    // the next step (ARM ARM K11.5.4: "A DMB is not sufficient").
    // Cortex-A53 cache line = 64 bytes.

    static constexpr std::size_t CACHE_LINE = 64;

    // Clean one cache line to DRAM (DC CVAC)
    [[gnu::always_inline]] static inline void dcClean(const void* addr) noexcept {
#if defined(__aarch64__)
        asm volatile("dc cvac, %0" :: "r"(addr) : "memory");
#else
        (void)addr;
#endif
    }

    // Clean + invalidate one cache line (DC CIVAC)
    [[gnu::always_inline]] static inline void dcCleanInvalidate(const void* addr) noexcept {
#if defined(__aarch64__)
        asm volatile("dc civac, %0" :: "r"(addr) : "memory");
#else
        (void)addr;
#endif
    }

    // DSB SY — completion barrier; all prior memory ops globally visible
    // (system-wide).  Used after descriptor stores before MMIO kicks (TSTART)
    // and after cache maintenance ops on packet buffers.
    [[gnu::always_inline]] static inline void dsbSy() noexcept {
#if defined(__aarch64__)
        asm volatile("dsb sy" ::: "memory");
#else
        asm volatile("" ::: "memory");
#endif
    }

    // DMB ISHLD — load-load barrier within the inner shareable domain.
    // Subsequent loads cannot be reordered before any preceding load.
    // Used in rxTryReceive between the RX_USED probe and the dependent
    // read of ctrl/length, to defeat speculative early-load of ctrl
    // before HW's USED-bit write becomes observable to us.  Cheap (a
    // handful of cycles) compared to the full dsbSy.
    [[gnu::always_inline]] static inline void dmbIshLd() noexcept {
#if defined(__aarch64__)
        asm volatile("dmb ishld" ::: "memory");
#else
        asm volatile("" ::: "memory");
#endif
    }

    // Clean a range to DRAM
    [[gnu::always_inline]]
    static inline void dcCleanRange(const void* addr, std::size_t bytes) noexcept {
        auto p   = reinterpret_cast<std::uintptr_t>(addr) & ~(CACHE_LINE - 1);
        auto end = reinterpret_cast<std::uintptr_t>(addr) + bytes;
        for (; p < end; p += CACHE_LINE)
            dcClean(reinterpret_cast<const void*>(p));
    }

    // Clean + invalidate a range
    [[gnu::always_inline]]
    static inline void dcCleanInvalidateRange(const void* addr, std::size_t bytes) noexcept {
        auto p   = reinterpret_cast<std::uintptr_t>(addr) & ~(CACHE_LINE - 1);
        auto end = reinterpret_cast<std::uintptr_t>(addr) + bytes;
        for (; p < end; p += CACHE_LINE)
            dcCleanInvalidate(reinterpret_cast<const void*>(p));
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

    // CERN FECOS ships collectd which periodically polls end0 via SIOCETHTOOL.
    // While macb is unbound (or mid-probe), phylink state is NULL → collectd's
    // ioctl hits phylink_ethtool_ksettings_get with a NULL pointer and oopses
    // the kernel (trace seen: python3 PID in macb_get_link_ksettings). The oops
    // is soft (kills the ioctl context, kernel survives) but it taints the
    // kernel and may interfere with macb_probe running concurrently.
    //
    // Best-effort: ignore failures (service may be absent, not running, or we
    // may lack perms). Opt out with ABTRDA3_KEEP_COLLECTD=1 for debugging.
    // (dumpRegisterSnapshot was used during driver bring-up to diff macb's
    // baseline configuration against ours.  Removed for log hygiene now that
    // the PMD is stable; recover from git history if future debugging needs it.)

    // (restoreGemClocks removed — clocks are held alive by the gem_uio kernel
    // module via the proper CCF/firmware path while the PMD owns the device.)

    // After a successful macb rebind, end0 is reborn as a fresh netdev but
    // nothing is auto-wired back up:
    //   - systemd-networkd sees the new interface via udev but hasn't re-
    //     applied fec.network yet, so there's no IP, no DHCP lease, no
    //     default route
    //   - the NFS client is stuck in "server unreachable" state from the
    //     unbind window; the hard-mounted /usr/local (NFSv3 RO) does not
    //     recover on its own once the link is back, which is why any later
    //     shell command that touches /usr/local/bin hangs
    //   - SSH/serial itself keeps working, but new shells fork-exec into
    //     /usr/local paths and then block
    //
    // This helper runs a minimal recovery sequence synchronously (from the
    // destructor, after rebind). All commands pin PATH to local-only
    // directories so /bin/sh never searches NFS-backed components, and are
    // Captures the interface's IPv4 address+prefix and default gateway before
    // unbind. Uses getifaddrs() + /proc/net/route — no subprocess, no NFS.
    void saveNetworkConfig() noexcept {
        struct ifaddrs* list = nullptr;
        if (::getifaddrs(&list) == 0) {
            for (struct ifaddrs* ifa = list; ifa; ifa = ifa->ifa_next) {
                if (!ifa->ifa_name || std::strcmp(ifa->ifa_name, m_ifname.c_str()) != 0) continue;
                if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET) continue;
                auto* sin = reinterpret_cast<sockaddr_in*>(ifa->ifa_addr);
                char ipbuf[INET_ADDRSTRLEN]{};
                ::inet_ntop(AF_INET, &sin->sin_addr, ipbuf, sizeof(ipbuf));
                int prefix = 0;
                if (ifa->ifa_netmask) {
                    auto* mask = reinterpret_cast<sockaddr_in*>(ifa->ifa_netmask);
                    std::uint32_t m = ntohl(mask->sin_addr.s_addr);
                    while (m & 0x80000000u) { ++prefix; m <<= 1; }
                }
                m_savedAddr = fmt::format("{}/{}", ipbuf, prefix);
                break;
            }
            ::freeifaddrs(list);
        }

        FILE* f = std::fopen("/proc/net/route", "r");
        if (f) {
            char line[256];
            std::fgets(line, sizeof(line), f); // skip header
            while (std::fgets(line, sizeof(line), f)) {
                char iface[32]{};
                unsigned long dest = 1, gw = 0;
                if (std::sscanf(line, "%31s %lx %lx", iface, &dest, &gw) == 3 &&
                    std::strcmp(iface, m_ifname.c_str()) == 0 && dest == 0) {
                    struct in_addr gwaddr{};
                    gwaddr.s_addr = static_cast<std::uint32_t>(gw);
                    char gwbuf[INET_ADDRSTRLEN]{};
                    ::inet_ntop(AF_INET, &gwaddr, gwbuf, sizeof(gwbuf));
                    m_savedGateway = gwbuf;
                    break;
                }
            }
            std::fclose(f);
        }

        if (!m_savedAddr.empty() && !m_savedGateway.empty())
            fmt::println(stderr, "[GEM] saved network config: addr={} gw={}", m_savedAddr, m_savedGateway);
        else
            fmt::println(stderr, "[GEM] WARNING: could not capture network config (addr='{}' gw='{}')",
                         m_savedAddr, m_savedGateway);
    }

    void stopNetworkd() noexcept {
        if (!driverIsBound()) return;  // already unbound — nothing to stop
        const int rc = std::system("timeout 5 systemctl stop systemd-networkd >/dev/null 2>&1");
        const int code = WIFEXITED(rc) ? WEXITSTATUS(rc) : -1;
        if (code != 0) fmt::println(stderr, "[GEM] systemd-networkd stop: rc={}", code);
    }

    // Find a netdev whose hardware address matches our GEM MAC (m_mac).
    // After macb rebinds, udevd may be wedged on NFS and never run the rename
    // rules — so the new netdev keeps its kernel-default name (eth0 etc.)
    // instead of becoming end0. Find it by MAC, regardless of name.
    [[nodiscard]] std::string findNetdevByMac() const noexcept {
        DIR* d = ::opendir("/sys/class/net");
        if (!d) return {};

        char want[24];
        std::snprintf(want, sizeof(want), "%02x:%02x:%02x:%02x:%02x:%02x",
                      m_mac[0], m_mac[1], m_mac[2], m_mac[3], m_mac[4], m_mac[5]);

        std::string found;
        while (struct dirent* e = ::readdir(d)) {
            const char* n = e->d_name;
            if (n[0] == '.') continue;
            if (std::strcmp(n, "lo") == 0) continue;
            if (std::strncmp(n, "tap_", 4) == 0) continue;  // skip our own TAP

            std::string path = "/sys/class/net/";
            path += n;
            path += "/address";
            const int fd = ::open(path.c_str(), O_RDONLY);
            if (fd < 0) continue;
            char buf[24]{};
            const ssize_t r = ::read(fd, buf, sizeof(buf) - 1);
            ::close(fd);
            if (r <= 0) continue;
            std::size_t len = static_cast<std::size_t>(r);
            while (len > 0 && (buf[len-1] == '\n' || buf[len-1] == ' ')) buf[--len] = '\0';
            if (::strcasecmp(buf, want) == 0) { found = n; break; }
        }
        ::closedir(d);
        return found;
    }

    // Rename a netdev via SIOCSIFNAME. Interface MUST be administratively
    // DOWN for the kernel to accept the rename — bring it down first.
    static bool renameNetdev(const std::string& from, const std::string& to) noexcept {
        if (from == to) return true;
        const int s = ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
        if (s < 0) return false;
        ifreq ifr{};
        std::strncpy(ifr.ifr_name, from.c_str(), IFNAMSIZ - 1);
        if (::ioctl(s, SIOCGIFFLAGS, &ifr) >= 0 && (ifr.ifr_flags & IFF_UP)) {
            ifr.ifr_flags &= ~static_cast<short>(IFF_UP);
            ::ioctl(s, SIOCSIFFLAGS, &ifr);
        }
        std::strncpy(ifr.ifr_name,    from.c_str(), IFNAMSIZ - 1);
        std::strncpy(ifr.ifr_newname, to.c_str(),   IFNAMSIZ - 1);
        const int rc = ::ioctl(s, SIOCSIFNAME, &ifr);
        ::close(s);
        return rc >= 0;
    }

    // Bring up + assign IP/netmask + add default route, all via ioctls.
    // Mirrors the TAP-bridge configuration in TransportDispatch — avoids
    // forking ip(8) which can wedge if NFS-backed PATH lookup happens.
    [[nodiscard]] bool configureNetdevByIoctl(const std::string& iface) noexcept {
        if (m_savedAddr.empty() || m_savedGateway.empty()) {
            fmt::println(stderr, "[GEM] configureNetdev: no saved address/gateway");
            return false;
        }
        const int s = ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
        if (s < 0) {
            fmt::println(stderr, "[GEM] configureNetdev: socket: {}", std::strerror(errno));
            return false;
        }

        // Parse "10.11.33.71/24"
        std::string ip = m_savedAddr;
        int prefix = 24;
        if (auto slash = m_savedAddr.find('/'); slash != std::string::npos) {
            ip = m_savedAddr.substr(0, slash);
            prefix = std::stoi(m_savedAddr.substr(slash + 1));
        }

        ifreq ifr{};
        std::strncpy(ifr.ifr_name, iface.c_str(), IFNAMSIZ - 1);

        // IP
        auto* sin = reinterpret_cast<sockaddr_in*>(&ifr.ifr_addr);
        sin->sin_family = AF_INET;
        ::inet_pton(AF_INET, ip.c_str(), &sin->sin_addr);
        if (::ioctl(s, SIOCSIFADDR, &ifr) < 0)
            fmt::println(stderr, "[GEM] SIOCSIFADDR: {}", std::strerror(errno));

        // Netmask
        auto* nmask = reinterpret_cast<sockaddr_in*>(&ifr.ifr_netmask);
        nmask->sin_family = AF_INET;
        nmask->sin_addr.s_addr = htonl(prefix == 0 ? 0U : ~((1U << (32 - prefix)) - 1));
        if (::ioctl(s, SIOCSIFNETMASK, &ifr) < 0)
            fmt::println(stderr, "[GEM] SIOCSIFNETMASK: {}", std::strerror(errno));

        // UP + RUNNING
        if (::ioctl(s, SIOCGIFFLAGS, &ifr) >= 0) {
            ifr.ifr_flags |= IFF_UP | IFF_RUNNING;
            if (::ioctl(s, SIOCSIFFLAGS, &ifr) < 0)
                fmt::println(stderr, "[GEM] SIOCSIFFLAGS UP: {}", std::strerror(errno));
        }

        // Default route via gateway
        rtentry rt{};
        reinterpret_cast<sockaddr_in*>(&rt.rt_dst)->sin_family     = AF_INET;
        reinterpret_cast<sockaddr_in*>(&rt.rt_genmask)->sin_family = AF_INET;
        auto* gwsin = reinterpret_cast<sockaddr_in*>(&rt.rt_gateway);
        gwsin->sin_family = AF_INET;
        ::inet_pton(AF_INET, m_savedGateway.c_str(), &gwsin->sin_addr);
        rt.rt_flags = RTF_UP | RTF_GATEWAY;
        char devBuf[IFNAMSIZ]{};
        std::strncpy(devBuf, iface.c_str(), IFNAMSIZ - 1);
        rt.rt_dev = devBuf;
        if (::ioctl(s, SIOCADDRT, &rt) < 0)
            fmt::println(stderr, "[GEM] SIOCADDRT: {}", std::strerror(errno));

        ::close(s);
        return true;
    }

    // Restores networking and NFS after macb rebind.
    //
    // udevd may be hung on NFS, so the rebound netdev keeps its kernel-default
    // name. We find it by MAC, rename to m_ifname if needed, and configure it
    // entirely via ioctls (no fork/exec, no PATH lookup). NFS hard-mounts
    // auto-recover within ~1s once the TCP path is back.
    void restoreNetworkAndNfs() noexcept {
        if (std::getenv("ABTRDA3_SKIP_NET_RESTORE")) {
            fmt::println(stderr, "[GEM] ABTRDA3_SKIP_NET_RESTORE set — skipping");
            return;
        }

        // 1. Wait for ANY netdev with our MAC to appear (default name may differ).
        fmt::println(stderr, "[GEM] scanning /sys/class/net for MAC {:02x}:{:02x}:{:02x}:{:02x}:{:02x}:{:02x} …",
                     m_mac[0], m_mac[1], m_mac[2], m_mac[3], m_mac[4], m_mac[5]);
        std::string iface;
        for (int i = 0; i < 600; ++i) {            // up to 60 s
            iface = findNetdevByMac();
            if (!iface.empty()) {
                fmt::println(stderr, "[GEM] found '{}' after ~{} ms", iface, i * 100);
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        if (iface.empty()) {
            fmt::println(stderr, "[GEM] WARNING: no netdev with our MAC after 60 s — aborting recovery");
            return;
        }

        // 2. Rename to m_ifname (e.g. eth0 → end0) if udev didn't.
        if (iface != m_ifname) {
            fmt::println(stderr, "[GEM] renaming '{}' → '{}' (udevd hung on NFS, doing it ourselves)",
                         iface, m_ifname);
            if (renameNetdev(iface, m_ifname)) {
                fmt::println(stderr, "[GEM] rename OK");
                iface = m_ifname;
            } else {
                fmt::println(stderr, "[GEM] rename failed: {} — continuing with name '{}'",
                             std::strerror(errno), iface);
            }
        }

        // 3. Configure: UP + IP + netmask + default route, all via ioctl.
        if (!configureNetdevByIoctl(iface)) return;
        fmt::println(stderr, "[GEM] {} configured: addr={} gw={}", iface, m_savedAddr, m_savedGateway);

        // 4. Wait for PHY link-up via sysfs carrier.
        fmt::println(stderr, "[GEM] waiting for PHY link-up (carrier)…");
        const std::string carrierPath = "/sys/class/net/" + iface + "/carrier";
        for (int i = 0; i < 200; ++i) {
            char buf[4]{};
            const int fd = ::open(carrierPath.c_str(), O_RDONLY);
            if (fd >= 0) {
                const ssize_t n = ::read(fd, buf, sizeof(buf) - 1);
                ::close(fd);
                if (n > 0 && buf[0] == '1') {
                    fmt::println(stderr, "[GEM] link up after ~{} ms", i * 100);
                    break;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        // 5. Wait for NFS to auto-recover.
        fmt::println(stderr, "[GEM] waiting for /usr/local NFS auto-recovery (up to 90 s)…");
        for (int i = 0; i < 90; ++i) {
            struct stat st{};
            if (::stat("/usr/local/bin", &st) == 0) {
                fmt::println(stderr, "[GEM] /usr/local recovered after ~{} s", i);
                fmt::println(stderr, "[GEM] network/NFS recovery complete");
                return;
            }
            if (i > 0 && (i % 15) == 0)
                fmt::println(stderr, "[GEM] … still waiting ({} s)", i);
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        fmt::println(stderr, "[GEM] WARNING: /usr/local did not auto-recover after 90 s");
    }

    void stopMonitoringDaemons() noexcept {
        if (!driverIsBound()) {
            fmt::println(stderr,
                "[GEM] macb already unbound — skipping daemon stops "
                "(systemctl would hang on dead network)");
            return;
        }
        // fec-check-ethernet-speed polls psutil.net_if_stats() every 5 min,
        // which does SIOCETHTOOL on every interface. If it races with
        // macb_probe during rebind, phylink_ethtool_ksettings_get
        // dereferences a NULL phydev → kernel oops.
        static constexpr const char* services[] = {
            "collectd",
            "fec-check-ethernet-speed",
        };
        for (const char* svc : services) {
            const std::string cmd = fmt::format(
                "timeout 5 systemctl stop {} >/dev/null 2>&1", svc);
            const int rc = std::system(cmd.c_str());
            const int code = WIFEXITED(rc) ? WEXITSTATUS(rc) : -1;
            if (code != 0)
                fmt::println(stderr, "[GEM] stop {}: rc={}", svc, code);
        }
    }

    // Shared helper: synchronous sysfs write of a string (no trailing newline).
    [[nodiscard]] static bool sysfsWrite(const std::string& path,
                                          std::string_view  value) noexcept {
        const int fd = ::open(path.c_str(), O_WRONLY);
        if (fd < 0) return false;
        const ssize_t n = ::write(fd, value.data(), value.size());
        ::close(fd);
        return n == static_cast<ssize_t>(value.size());
    }

    // Is the gem_uio platform driver registered (module loaded)?
    [[nodiscard]] static bool gemUioAvailable() noexcept {
        struct stat st{};
        return ::stat("/sys/bus/platform/drivers/gem_uio", &st) == 0;
    }

    // Bind the gem_uio kernel module to our device.  Sets driver_override so
    // the platform bus matches this driver to ff0b0000.ethernet despite the
    // device's compatible string ("cdns,gem") not naming gem_uio.
    [[nodiscard]] bool bindGemUio() noexcept {
        if (!gemUioAvailable()) {
            fmt::println(stderr, "[GEM] gem_uio module not loaded — `sudo insmod gem_uio.ko` first");
            return false;
        }

        const std::string overridePath =
            "/sys/bus/platform/devices/" + m_deviceName + "/driver_override";
        if (!sysfsWrite(overridePath, "gem_uio")) {
            fmt::println(stderr, "[GEM] write driver_override gem_uio failed: {}",
                         std::strerror(errno));
            return false;
        }
        if (!sysfsWrite("/sys/bus/platform/drivers/gem_uio/bind", m_deviceName)) {
            fmt::println(stderr, "[GEM] bind gem_uio failed: {}", std::strerror(errno));
            return false;
        }
        m_useGemUio = true;
        fmt::println(stderr, "[GEM] gem_uio bound — clocks alive via CCF, coherent DMA pool ready");
        return true;
    }

    // Unbind gem_uio and clear the driver_override so macb can rebind.
    void unbindGemUio() noexcept {
        if (!m_useGemUio) return;

        m_descPool.close();

        const std::string overridePath =
            "/sys/bus/platform/devices/" + m_deviceName + "/driver_override";
        (void)sysfsWrite("/sys/bus/platform/drivers/gem_uio/unbind", m_deviceName);
        (void)sysfsWrite(overridePath, "\n");                          // clear
        m_useGemUio = false;
        fmt::println(stderr, "[GEM] gem_uio unbound, driver_override cleared");
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
            fmt::println(stderr, "[GEM] open({}) failed: {}", unbind, std::strerror(errno));
            return false;
        }
        // Sysfs write runs macb_remove() synchronously in-kernel.
        const ssize_t w = ::write(fd, m_deviceName.data(), m_deviceName.size());
        ::close(fd);
        if (w != static_cast<ssize_t>(m_deviceName.size())) {
            fmt::println(stderr, "[GEM] unbind {}: write returned {} (errno={})",
                         m_deviceName, w, w < 0 ? errno : 0);
            return false;
        }
        m_unboundDriver = true;
        fmt::println(stderr, "[GEM] unbound {} from {}", m_deviceName, m_driverName);
        return true;
    }

    // Read a /proc file into a string for diagnostics. Returns empty on error.
    static std::string readProcFile(const std::string& path) noexcept {
        const int fd = ::open(path.c_str(), O_RDONLY);
        if (fd < 0) return {};
        std::string out;
        char buf[1024];
        for (;;) {
            const ssize_t n = ::read(fd, buf, sizeof(buf));
            if (n <= 0) break;
            out.append(buf, static_cast<std::size_t>(n));
        }
        ::close(fd);
        return out;
    }

    void rebindKernelDriver() noexcept {
        if (m_driverName.empty() || m_deviceName.empty()) return;
        const std::string bind = "/sys/bus/platform/drivers/" + m_driverName + "/bind";

        // macb_probe → __of_mdiobus_register → phy_device_create unconditionally
        // calls request_module("mdio_bus_phy_id_0x...") even when the matching
        // PHY driver is built-in.  /sbin/modprobe then searches the modules
        // tree which lives on /usr/local (NFS via end0 — hung once we unbound
        // macb).  Modprobe blocks indefinitely, taking macb_probe with it.
        //
        // The kernel comment in phy_device_create says request_module failure
        // is non-fatal: the kernel falls back to built-in driver matching.  So
        // point /proc/sys/kernel/modprobe at /bin/true for the bind window —
        // it exits 0 immediately without touching NFS.  Restore afterwards.
        constexpr const char* kModprobeProc = "/proc/sys/kernel/modprobe";
        const std::string savedModprobe = readProcFile(kModprobeProc);
        const std::string trimmedSaved  = savedModprobe.empty() ? std::string{}
            : savedModprobe.substr(0, savedModprobe.find_last_not_of("\n \t") + 1);
        const auto writeModprobe = [&](const char* val) noexcept {
            const int pfd = ::open(kModprobeProc, O_WRONLY);
            if (pfd < 0) return false;
            const ssize_t n = ::write(pfd, val, std::strlen(val));
            ::close(pfd);
            return n == static_cast<ssize_t>(std::strlen(val));
        };
        const bool wrapperSet = writeModprobe("/bin/true");
        if (!wrapperSet) {
            fmt::println(stderr,
                "[GEM] rebind: WARNING — failed to override kernel.modprobe ({}) — probe may hang on NFS",
                std::strerror(errno));
        }

        const int fd = ::open(bind.c_str(), O_WRONLY);
        if (fd < 0) {
            fmt::println(stderr, "[GEM] rebind: open({}) failed: {}",
                         bind, std::strerror(errno));
            if (wrapperSet && !trimmedSaved.empty()) writeModprobe(trimmedSaved.c_str());
            return;
        }
        std::fflush(stderr);

        // Run the sysfs write on a worker thread so we can detect a hang
        // and capture the worker's kernel stack from /proc/<tid>/stack.
        std::atomic<bool>     done{false};
        std::atomic<pid_t>    workerTid{0};
        ssize_t               writeResult = -1;
        int                   writeErrno  = 0;

        std::thread worker([&] {
            workerTid.store(static_cast<pid_t>(::syscall(SYS_gettid)),
                            std::memory_order_release);
            writeResult = ::write(fd, m_deviceName.data(), m_deviceName.size());
            writeErrno  = (writeResult < 0) ? errno : 0;
            done.store(true, std::memory_order_release);
        });

        constexpr int TIMEOUT_SEC = 10;
        for (int i = 0; i < TIMEOUT_SEC * 100; ++i) {
            if (done.load(std::memory_order_acquire)) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        if (!done.load(std::memory_order_acquire)) {
            const pid_t tid = workerTid.load(std::memory_order_acquire);
            fmt::println(stderr,
                "[GEM] rebind: macb_probe HUNG (no return after {} s) — capturing kernel state of worker tid={}",
                TIMEOUT_SEC, tid);

            // /proc/<tid>/wchan  — symbolic name of the function the task is sleeping in
            // /proc/<tid>/stack  — full kernel call stack (requires CONFIG_STACKTRACE)
            // /proc/<tid>/status — task state (D = uninterruptible sleep, R = running)
            // /proc/<tid>/syscall— current syscall + args, if in one
            const std::string base = "/proc/" + std::to_string(tid);
            const std::string wchan   = readProcFile(base + "/wchan");
            const std::string status  = readProcFile(base + "/status");
            const std::string syscall = readProcFile(base + "/syscall");
            const std::string stack   = readProcFile(base + "/stack");

            fmt::println(stderr, "[GEM] === wchan ===\n{}", wchan);
            fmt::println(stderr, "[GEM] === syscall ===\n{}", syscall);
            // status is verbose — print only the lines we care about.
            for (std::size_t pos = 0; pos < status.size();) {
                const std::size_t eol = status.find('\n', pos);
                const std::string line = status.substr(pos, eol - pos);
                if (line.starts_with("State:") || line.starts_with("Name:") ||
                    line.starts_with("Tgid:")  || line.starts_with("Pid:"))
                    fmt::println(stderr, "[GEM] status: {}", line);
                pos = (eol == std::string::npos) ? status.size() : eol + 1;
            }
            fmt::println(stderr, "[GEM] === stack (top of kernel call stack — innermost first) ===\n{}",
                         stack.empty() ? "(empty — CONFIG_STACKTRACE may be off, or 0 if task is running)" : stack);
            std::fflush(stderr);

            worker.detach();
            ::close(fd);
            if (wrapperSet && !trimmedSaved.empty()) writeModprobe(trimmedSaved.c_str());
            return;
        }

        worker.join();
        ::close(fd);

        if (wrapperSet && !trimmedSaved.empty()) writeModprobe(trimmedSaved.c_str());

        if (writeResult != static_cast<ssize_t>(m_deviceName.size())) {
            fmt::println(stderr, "[GEM] rebind: short write {} (errno={}) — rebind failed",
                         writeResult, writeErrno);
            return;
        }
        m_unboundDriver = false;
        fmt::println(stderr, "[GEM] rebound {} to {}", m_deviceName, m_driverName);
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
            writeReg(qIDR(q), 0xFFFFFFFFU);
            // Zynq GEM ISR is read-to-clear (no MACB_CAPS_ISR_CLEAR_ON_WRITE)
            (void)readReg(qISR(q));
        }
    }

    // Restore the GEM to the state macb_remove leaves it in so that
    // macb_probe can re-init cleanly. shutdown() only clears RE/TE —
    // this goes further: zeroes DMA pointers, clears DMACFG/NCFGR,
    // and gates the clocks back off so the kernel CCF ref-count matches
    // reality (0 = disabled). Without this, macb_probe hangs.
    void resetForRebind() noexcept {
        if (!m_bus.isOpen()) return;

        // 1. Gracefully halt TX DMA before touching any state.
        //    Mirrors kernel macb_halt_tx(): set THALT, poll TGO → 0.
        std::uint32_t ncr = readReg(MACB_NCR);
        writeReg(MACB_NCR, ncr | MACB_BIT(THALT));
        for (int i = 0; i < 4000; ++i) {
            if (!(readReg(MACB_TSR) & MACB_BIT(TGO))) break;
            for (int d = 0; d < 100; ++d) asm volatile("" ::: "memory");
        }
        if (readReg(MACB_TSR) & MACB_BIT(TGO))
            fmt::println(stderr, "[GEM] resetForRebind: WARNING — THALT timed out, forcing TE off");

        // 2. macb_reset_hw() — clear RE/TE, set CLRSTAT, flush status regs.
        {
            std::uint32_t n = readReg(MACB_NCR);
            n &= ~(MACB_BIT(RE) | MACB_BIT(TE));
            n |= MACB_BIT(CLRSTAT);
            writeReg(MACB_NCR, n);
        }
        writeReg(MACB_TSR, 0xFFFFFFFFU);
        writeReg(MACB_RSR, 0xFFFFFFFFU);
        writeReg(GEM_PBUFRXCUT, 0);
        disableInterrupts();

        fmt::println(stderr, "[GEM] resetForRebind: registers cleared");
        // (Clocks are managed by gem_uio via the kernel CCF; unbinding
        // gem_uio in the destructor disables them through the proper path,
        // so no direct CRL_APB writes are needed here.)
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
        // RBOF intentionally left at 0. The kernel macb driver sets
        // RBOF=2 so the IP header lands on a 4-byte boundary (skb_reserve).
        // Our PMD does pure L2 work on a custom ethertype — never parses
        // IP — so the alignment isn't earning anything, and a non-zero
        // RBOF means rxTryReceive must skip the same N bytes or every
        // consumer reads junk + truncated dst MAC. Keep it 0.
        cfg |= MACB_BIT(DRFCS);
        cfg |= MACB_BIT(BIG);
        cfg |= MACB_BIT(FD);
        cfg |= GEM_BIT(GBE);
        cfg |= GEM_BIT(RXCOEN);
        cfg |= GEM_BF(DBW, GEM_DBW64);
        // NBC intentionally NOT set — broadcast/multicast frames pass the
        // SA filter and get steered by the Type-2 screeners.  Slot 0
        // matches our 0x88B5 ethertype to Q1 (hot path); slot 1 is a
        // wildcard fall-through to Q0 (slow path → TAP bridge).  This
        // keeps the GEM's RX resource error counter at 0 *and* gives the
        // TAP bridge incoming ARP / DHCP / mDNS so SSH and NFS work
        // during the PMD run window.  See configureScreeners().
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

        // Per-queue RX buffer size exists only for hw_q >= 1. Q0 uses the
        // global DMACFG.RXBS (already set above).
        for (std::size_t q = 1; q < GEM_NUM_QUEUES_HW; ++q) {
            writeReg(GEM_RBQS(0) + static_cast<std::uint32_t>((q - 1) << 2),
                     BuffSize / 64);
        }
    }

    void configureUsrio() noexcept {
        writeReg(GEM_USRIO, 0x00000001U);
    }

    // =====================================================================
    // Descriptor rings
    // =====================================================================
    [[nodiscard]] bool initRings() {
        // Descriptor rings come out of the gem_uio coherent DMA pool (mem[1])
        // — non-cached on ARM, no software cache maintenance needed.  We
        // partition the pool into 4 contiguous regions: RX[0], RX[1], TX[0],
        // TX[1] — each NumRxDesc/NumTxDesc descriptors of 16 bytes.
        constexpr std::size_t rxBytes = NumRxDesc * sizeof(GEMDescriptor64);
        constexpr std::size_t txBytes = NumTxDesc * sizeof(GEMDescriptor64);
        const     std::size_t needed  = HAS_RX * GEM_NUM_QUEUES_HW * rxBytes
                                      + HAS_TX * GEM_NUM_QUEUES_HW * txBytes;
        if (needed > m_descPool.size()) {
            fmt::println(stderr, "[GEM] initRings: pool too small ({} need {})",
                         m_descPool.size(), needed);
            return false;
        }

        std::size_t off = 0;
        if constexpr (HAS_RX) {
            for (std::size_t q = 0; q < GEM_NUM_QUEUES_HW; ++q) {
                m_rxRings[q].virt = static_cast<GEMDescriptor64*>(m_descPool.virtAt(off));
                m_rxRings[q].phys = m_descPool.physAt(off);
                if (!m_rxBuffers[q].allocate(NumRxDesc * BuffSize)) return fail("RX buffers", q);
                off += rxBytes;
            }
        }
        if constexpr (HAS_TX) {
            for (std::size_t q = 0; q < GEM_NUM_QUEUES_HW; ++q) {
                m_txRings[q].virt = static_cast<GEMDescriptor64*>(m_descPool.virtAt(off));
                m_txRings[q].phys = m_descPool.physAt(off);
                if (!m_txBuffers[q].allocate(NumTxDesc * BuffSize)) return fail("TX buffers", q);
                off += txBytes;
            }
        }

        // Cold-path init — populate descriptor fields.  Memory is non-cached
        // (kernel-coherent), so plain stores reach DRAM directly; no dcClean
        // or dsb needed for descriptors.
        for (std::size_t q = 0; q < GEM_NUM_QUEUES_HW; ++q) {
            if constexpr (HAS_TX) {
                auto* tx = m_txRings[q].virt;
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
                auto* rx = m_rxRings[q].virt;
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
        // One dsb to publish all descriptor writes to DRAM before HW reads them
        // (uncached writes still go through a write buffer until drained).
        dsbSy();
        return true;
    }

    void initBuffers() noexcept {
        if constexpr (HAS_RX) {
            writeReg(MACB_RBQPH, static_cast<std::uint32_t>(m_rxRings[0].physicalBase() >> 32));
            for (std::size_t q = 0; q < GEM_NUM_QUEUES_HW; ++q) {
                const std::uint64_t b = m_rxRings[q].physicalBase();
                writeReg(qRBQP(q), static_cast<std::uint32_t>(b & 0xFFFFFFFFU));
            }
        }
        if constexpr (HAS_TX) {
            writeReg(MACB_TBQPH, static_cast<std::uint32_t>(m_txRings[0].physicalBase() >> 32));
            for (std::size_t q = 0; q < GEM_NUM_QUEUES_HW; ++q) {
                const std::uint64_t b = m_txRings[q].physicalBase();
                writeReg(qTBQP(q), static_cast<std::uint32_t>(b & 0xFFFFFFFFU));
            }
        }
    }

    // =====================================================================
    // Screener — steer our EtherType to Q_HOT, everything else to Q_SLOW
    // =====================================================================
    void configureScreeners() noexcept {
        // GEM_ETHT and GEM_SCRT2 are register banks — each entry is 4 bytes.
        // The Cadence GEM evaluates Type-2 screener slots in index order;
        // the first slot whose enabled compares all match wins, and the
        // packet is steered to that slot's QUEUE.
        //
        // Slot 0: 0x88B5 EtherType  → Q_HOT  (our test traffic, line-rate)
        // Slot 1: wildcard match    → Q_SLOW (everything else → TAP bridge)
        //
        // The slot-1 fall-through is what makes ARP / DHCP / mDNS / general
        // kernel networking traffic reach the TAP bridge — without it,
        // unmatched frames hit the GEM's "no descriptor" path on whatever
        // queue the chip picks as default and the resErr counter saturates.

        constexpr std::uint32_t ETHT_HOT_IDX  = 0;
        constexpr std::uint32_t SCR2_HOT_SLOT = 0;

        // Slot 0 — exact-match the hot-path EtherType, route to Q_HOT
        writeReg(GEM_ETHT  + (ETHT_HOT_IDX  << 2), GEM_LOCAL_EXPERIMENTAL_ETHERTYPE);
        writeReg(GEM_SCRT2 + (SCR2_HOT_SLOT << 2),
                   GEM_BF(QUEUE,    Q_HOT)
                 | GEM_BF(ETHT2IDX, ETHT_HOT_IDX)
                 | GEM_BIT(ETHTEN));

        // Compare register slot 0 — wildcard "match every frame".
        //   T2CMPW0:  value=0x0000, mask=0x0000  →  (frame & 0) == (0 & 0) → always true
        //   T2CMPW1:  T2CMPOFST=1 (compare from start of L2 frame),
        //             T2OFST=0 (byte 0 of frame), T2DISMSK=0 (mask mode)
        // The compare register pair lives at T2CMPW0/W1 + slot*8.
        constexpr std::uint32_t T2CMP_WILDCARD_SLOT = 0;
        writeReg(GEM_T2CMPW0 + (T2CMP_WILDCARD_SLOT << 3), 0x00000000u);
        writeReg(GEM_T2CMPW1 + (T2CMP_WILDCARD_SLOT << 3),
                   GEM_BF(T2CMPOFST, 1)   // start at L2 frame offset 0
                 | GEM_BF(T2OFST,    0)
                 /* T2DISMSK = 0 → mask-mode 16-bit compare */);

        // Slot 1 — fall-through catch-all, route to Q_SLOW (TAP bridge)
        constexpr std::uint32_t SCR2_SLOW_SLOT = 1;
        writeReg(GEM_SCRT2 + (SCR2_SLOW_SLOT << 2),
                   GEM_BF(QUEUE, Q_SLOW)
                 | GEM_BF(CMPA,  T2CMP_WILDCARD_SLOT)
                 | GEM_BIT(CMPAEN));
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
        // MPE must be set before any MDIO transaction.
        std::uint32_t ncr = readReg(MACB_NCR);
        ncr |= MACB_BIT(MPE);
        writeReg(MACB_NCR, ncr);

        // Scan MDIO 31→0 (reverse). The LAN8841 on mkdev50 is at addr 31
        // per the device tree, but also responds at addr 0 (broadcast alias).
        // Scanning high-to-low finds the real address before the alias.
        m_phyAddr = -1;
        for (int a = 31; a >= 0; --a) {
            const auto addr = static_cast<std::uint8_t>(a);
            const std::uint16_t id2 = mdioRead(addr, 2);
            const std::uint16_t id3 = mdioRead(addr, 3);
            if (id2 != 0xFFFF && id2 != 0x0000) {
                fmt::println(stderr, "[GEM] PHY at MDIO addr {:<2}: ID = 0x{:04x}{:04x}",
                             a, id2, id3);
                if (m_phyAddr < 0) m_phyAddr = static_cast<std::int8_t>(a);
            }
        }
        if (m_phyAddr < 0) {
            fmt::println(stderr, "[GEM] No PHY responded on MDIO scan — TX/RX cannot work");
            return false;
        }

        // Verify writes actually stick at the chosen address. Broadcast
        // aliases may accept reads but silently discard writes — which
        // would make our BMCR power-down clear a no-op.
        const auto ph = static_cast<std::uint8_t>(m_phyAddr);
        {
            const std::uint16_t anar = mdioRead(ph, 4);   // ANAR (reg 4)
            const std::uint16_t toggled = anar ^ 0x0020u;  // flip 10BASE-T advert bit
            if (!mdioWrite(ph, 4, toggled)) return false;
            const std::uint16_t readback = mdioRead(ph, 4);
            if (!mdioWrite(ph, 4, anar)) return false;     // restore
            if (readback != toggled) {
                fmt::println(stderr,
                    "[GEM] WARNING: MDIO write-readback FAILED at addr {} "
                    "(wrote 0x{:04x}, read 0x{:04x}) — may be a broadcast alias",
                    m_phyAddr, toggled, readback);
            } else {
                fmt::println(stderr, "[GEM] MDIO write-readback OK at addr {}", m_phyAddr);
            }
        }

        // macb_remove (from our unbind) runs phylink_disconnect → phy_stop,
        // which sets BMCR.POWER_DOWN (bit 11). Our PMD then sees no carrier
        // from the PHY → the MAC's TX state machine latches TXGO=1 waiting
        // for carrier sense and TXCNT stays at 0. Wake the PHY here.
        std::uint16_t bmcr = mdioRead(ph, 0);
        fmt::println(stderr,
            "[GEM] PHY BMCR=0x{:04x} (power_down={}, isolate={}, auto_neg_en={})",
            bmcr,
            (bmcr & 0x0800) ? 1 : 0,
            (bmcr & 0x0400) ? 1 : 0,
            (bmcr & 0x1000) ? 1 : 0);
        if (bmcr & 0x0C00) {
            fmt::println(stderr, "[GEM] Clearing POWER_DOWN / ISOLATE and restarting auto-neg");
            bmcr &= ~0x0C00;          // clear POWER_DOWN (bit 11) + ISOLATE (bit 10)
            bmcr |=  0x1200;          // set AUTO_NEG_EN (bit 12) + RESTART_AN (bit 9)
            if (!mdioWrite(ph, 0, bmcr)) {
                fmt::println(stderr,
                    "[GEM] Warning: mdioWrite to BMCR stalled — PHY may still be in POWER_DOWN/ISOLATE");
            }
        }

        // Poll the real PHY for link-up, not addr 0.
        for (int i = 0; i < 500; ++i) {
            const std::uint16_t bmsr = mdioRead(ph, 1);
            if (bmsr & 0x0004) {
                const std::uint16_t anlpar = mdioRead(ph, 5);
                const std::uint16_t gbstat = mdioRead(ph, 10);
                fmt::println(stderr,
                    "[GEM] Link up on PHY addr {}: BMSR=0x{:04x} ANLPAR=0x{:04x} GBStatus=0x{:04x}",
                    m_phyAddr, bmsr, anlpar, gbstat);
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        fmt::println(stderr, "[GEM] Warning: no link up on PHY addr {} after 5s — continuing",
                     m_phyAddr);
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
        return m_rxRings[Q].virt[i];
    }
    template<std::size_t Q>
    [[nodiscard, gnu::always_inline]]
    inline GEMDescriptor64& txDesc(std::size_t i) noexcept {
        static_assert(Q < GEM_NUM_QUEUES_HW, "queue index out of range");
        return m_txRings[Q].virt[i];
    }

    template<std::size_t Q>
    [[nodiscard, gnu::hot]]
    RxFrame rxTryReceive() noexcept {
        static_assert(Q < GEM_NUM_QUEUES_HW);
        const std::size_t tail = m_rxTail[Q];
        auto& d = rxDesc<Q>(tail);

        // Descriptor lives in the gem_uio coherent DMA pool (non-cached);
        // a plain volatile load reads the latest GEM-written value with no
        // cache maintenance.
        const std::uint32_t addr = *reinterpret_cast<volatile const std::uint32_t*>(&d.base.addr);
        if (!(addr & MACB_BIT(RX_USED))) [[likely]] return {};

        // ARM is weakly ordered: the load of ctrl below has only a control
        // dependency (the if-branch) on the addr load, which AArch64 does
        // NOT count as ordering.  Insert a load-load barrier so the ctrl
        // read happens after the USED probe — otherwise the CPU may have
        // speculatively read a stale ctrl from before HW's RX update.
        dmbIshLd();

        const std::uint32_t ctrl = d.base.ctrl;
        const std::uint32_t len  = ctrl & MACB_RX_FRMLEN_MASK;

        // Packet payload still lives on cached hugepages — invalidate before
        // reading what the GEM DMA-wrote, so we don't see stale CPU cache.
        auto* pkt = m_rxBuffers[Q].template ptrAt<std::uint8_t>(tail * BuffSize);
        dcCleanInvalidateRange(pkt, len);
        dsbSy();

        return {
            .data   = { pkt, len },
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
        d.base.addr  = addr;                 // clears RX_USED — HW can refill
        // Descriptor is in coherent DMA memory — no dcClean needed.  A dsb
        // ensures the writes drain from the CPU write buffer before the
        // next HW DMA fetch.
        dsbSy();

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

        // Clean packet data to DRAM so GEM DMA reads what the CPU wrote
        auto* pkt = m_txBuffers[Q].template ptrAt<std::uint8_t>(tail * BuffSize);
        dcCleanRange(pkt, m_txPendingLen[Q]);

        const bool wrap = (tail == NumTxDesc - 1);
        std::uint32_t ctrl = (m_txPendingLen[Q] & 0x3FFF) | MACB_BIT(TX_LAST);
        if (wrap) ctrl |= MACB_BIT(TX_WRAP);

        d.base.ctrl = ctrl;                  // USED=0 signals HW
        // Descriptor is non-cached (gem_uio coherent pool) — no dcClean.
        // DSB drains the packet-buffer dcCleanRange above plus this store
        // before TSTART (ARM K11.5.4: cache ops + uncached writes).
        dsbSy();

        m_txTail[Q]     = (tail + 1) & TX_RING_MASK;
        m_txInFlight[Q] = m_txInFlight[Q] + 1;

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
            // Descriptor is in non-cached coherent memory — a plain
            // volatile load returns the latest GEM-written value with
            // no cache ops needed.
            const std::uint32_t ctrl =
                *reinterpret_cast<volatile const std::uint32_t*>(&d.base.ctrl);
            if (!(ctrl & MACB_BIT(TX_USED))) break;
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

    // Descriptor rings live in the gem_uio module's coherent DMA buffer
    // (non-cached on ARM).  Packet payload buffers stay on cached hugepages
    // since we want CPU cache locality there.
    CoherentDmaPool                                       m_descPool;
    std::array<DescRingSlice,            GEM_NUM_QUEUES_HW> m_rxRings;
    std::array<HugepageBuffer,           GEM_NUM_QUEUES_HW> m_rxBuffers;
    std::array<std::size_t,              GEM_NUM_QUEUES_HW> m_rxTail{};

    std::array<DescRingSlice,            GEM_NUM_QUEUES_HW> m_txRings;
    std::array<HugepageBuffer,           GEM_NUM_QUEUES_HW> m_txBuffers;
    std::array<std::size_t,              GEM_NUM_QUEUES_HW> m_txTail{};
    std::array<std::size_t,              GEM_NUM_QUEUES_HW> m_txInFlight{};
    std::array<std::uint32_t,            GEM_NUM_QUEUES_HW> m_txPendingLen{};

    std::array<std::uint8_t, 6> m_mac{};
    std::int8_t                 m_phyAddr{-1};          // set by initPhy scan; -1 = not found
    bool                        m_unboundDriver{false};
    bool                        m_useGemUio{false};     // gem_uio bound for descriptor pool
    std::string m_savedAddr;        // e.g. "10.11.33.71/24" — captured before unbind
    std::string m_savedGateway;     // e.g. "10.11.33.1" — captured before unbind

    SlowPath m_slowPath{this};
};

#endif // ABTRDA3_CADENCE_GEM_HPP
