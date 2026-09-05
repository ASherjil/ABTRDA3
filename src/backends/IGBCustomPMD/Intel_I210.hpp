// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Areeb Sherjil

//
// Created by asherjil on 4/5/26.
//

#ifndef ABTEDGE_INTEL_I210_H
#define ABTEDGE_INTEL_I210_H

#include <array>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <thread>

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "DMARing.hpp"
#include "HugepageBuffer.hpp"
#include "I210Registers.hpp"
#include "InterfaceDiscovery.hpp"
#include "PCIeBackend.hpp"

enum class DriverMode : std::uint8_t {
    RxOnly,
    TxOnly,
    RxTx
};

template <DriverMode M, std::size_t NumRxDesc = 256, std::size_t NumTxDesc = 256, std::size_t BuffSize = 2048>
class Intel_I210 {
    static_assert(M == DriverMode::TxOnly || (NumRxDesc >= 8 && (NumRxDesc & (NumRxDesc - 1)) == 0),
                  "NumRxDesc must be a power of 2 and >= 8");
    static_assert(M == DriverMode::RxOnly || (NumTxDesc >= 8 && (NumTxDesc & (NumTxDesc - 1)) == 0),
                  "NumTxDesc must be a power of 2 and >= 8");
    static_assert(BuffSize == 256 || BuffSize == 512 || BuffSize == 1024 || BuffSize == 2048,
                  "Buffer size must be power of 2: 256 -> 2048");

    static constexpr bool        HAS_RX       = (M == DriverMode::RxOnly || M == DriverMode::RxTx);
    static constexpr bool        HAS_TX       = (M == DriverMode::TxOnly || M == DriverMode::RxTx);
    static constexpr std::size_t RX_RING_MASK = NumRxDesc - 1;
    static constexpr std::size_t TX_RING_MASK = NumTxDesc - 1;
    // Batch RDT writes: flush after this many descriptors released (DPDK default: 32)
    static constexpr std::uint32_t RX_FREE_THRESH = 32;

public:
    // Rule of 5
    explicit Intel_I210(std::string_view ifname, int bar = 0, std::string_view driverName = {})
        : m_pciBusHandler{ifname, bar, driverName} {
    }

    Intel_I210(const Intel_I210&)            = delete;
    Intel_I210& operator=(const Intel_I210&) = delete;

    Intel_I210(Intel_I210&& other) noexcept
        : m_pciBusHandler{std::move(other.m_pciBusHandler)},
          m_rxRing{std::move(other.m_rxRing)},
          m_rxBuffer{std::move(other.m_rxBuffer)},
          m_rxTail{std::exchange(other.m_rxTail, 0)},
          m_rxHeld{std::exchange(other.m_rxHeld, 0)},
          m_txRing{std::move(other.m_txRing)},
          m_txBuffer{std::move(other.m_txBuffer)},
          m_txTail{std::exchange(other.m_txTail, 0)},
          m_txCleanHead{std::exchange(other.m_txCleanHead, 0)},
          m_txInFlight{std::exchange(other.m_txInFlight, 0)},
          m_mac{other.m_mac} {
    }

    Intel_I210& operator=(Intel_I210&& other) noexcept {
        if (this != &other) {
            shutdown();
            m_pciBusHandler = std::move(other.m_pciBusHandler);
            m_rxRing        = std::move(other.m_rxRing);
            m_rxBuffer      = std::move(other.m_rxBuffer);
            m_rxTail        = std::exchange(other.m_rxTail, 0);
            m_rxHeld        = std::exchange(other.m_rxHeld, 0);
            m_txRing        = std::move(other.m_txRing);
            m_txBuffer      = std::move(other.m_txBuffer);
            m_txTail        = std::exchange(other.m_txTail, 0);
            m_txCleanHead   = std::exchange(other.m_txCleanHead, 0);
            m_txInFlight    = std::exchange(other.m_txInFlight, 0);
            m_mac           = std::exchange(other.m_mac, {});
        }
        return *this;
    }

    ~Intel_I210() {
        shutdown();   // call shutdown makes it easier for debugging
    }

    // Initialisation functions on the cold path. START
    [[nodiscard]] bool init() {
        if (!m_pciBusHandler.open()) {
            return false;
        }

        disableASPM();
        reset();
        disableInterrupts();
        disableEEE();
        clearPhyLinkDown();
        zeroMulticastTable();
        readMacAddress();

        // Initialise PHY: reset, advertise all speeds, restart auto-negotiation
        if (!initPhy()) {
            return false;
        }

        // Set link up so MAC recognises PHY's LINK indication (Section 3.7.4.4.1)
        // Also clear FRCSPD so MAC auto-detects speed from PHY (Section 3.7.4.4.2)
        // Disable flow control — PAUSE frames from the link partner can stall
        // our transmitter, adding unpredictable latency (DPDK sets fc=none).
        std::uint32_t ctrl = readReg(CTRL);
        ctrl |= CTRL_SLU;
        ctrl &= ~(CTRL_FRCSPD | CTRL_FRCDPLX | CTRL_RFCE | CTRL_TFCE);
        writeReg(CTRL, ctrl);

        if constexpr (HAS_RX) {
            if (!initRx()) {
                return false;
            }
        }
        if constexpr (HAS_TX) {
            if (!initTx()) {
                return false;
            }
        }

        // Wait for link — 1000BASE-T auto-negotiation takes 3-5s
        for (int i{}; i < 5; i++) {
            if (isLinkUp()) {
                std::fprintf(stderr, "[I210] Link up at %u Mbps.\n", linkSpeedMbps());
                verifyLowLatencyConfig();
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1000));
        }
        std::fprintf(stderr, "[I210] Warning: link not up after 5s\n");
        verifyLowLatencyConfig();
        return true;
    }

    void shutdown() {
        if (!m_pciBusHandler.isOpen()) {
            return;
        }

        // Disable Rx/Tx
        if constexpr (HAS_RX) {
            writeReg(RCTL, readReg(RCTL) & ~RCTL_RXEN);
        }
        if constexpr (HAS_TX) {
            writeReg(TCTL, readReg(TCTL) & ~TCTL_EN);
        }

        disableInterrupts();
    }

    [[nodiscard]] bool isLinkUp() const {
        return (readReg(STATUS) & STATUS_LU) != 0;
    }

    [[nodiscard]] std::uint32_t linkSpeedMbps() const {
        const std::uint32_t speed = (readReg(STATUS) & STATUS_SPEED_MASK) >> STATUS_SPEED_SHIFT;
        constexpr std::array<std::uint32_t, 4> speeds = {10, 100, 1000, 1000};
        return speeds[speed];
    }

    [[nodiscard]] std::array<std::uint8_t, 6> macAddress() const {
        return m_mac;
    }

    void prefillRing(std::span<const std::uint8_t> frameTemplate) noexcept
        requires (HAS_TX)
    {
        for (std::size_t i{}; i < NumTxDesc; i++) {
            std::memcpy(m_txBuffer.ptrAt<std::uint8_t>(i * BuffSize), frameTemplate.data(),
                        frameTemplate.size());
        }
    }

    // Initialisation functions on the cold path. END

    // Receive functions on the hot path - START
    [[nodiscard, gnu::always_inline, gnu::hot]]
    inline std::span<const std::uint8_t> tryReceive() noexcept
        requires (HAS_RX)
    {
        // DPDK-style multi-level prefetch (igb_rxtx.c:886-921):
        //  - Prefetch next descriptor every 4 iterations (cache-line aligned)
        //  - Prefetch packet data into L2 for current descriptor
        if ((m_rxTail & 0x3) == 0) {
            __builtin_prefetch(&m_rxRing[(m_rxTail + 4) & RX_RING_MASK], 0, 3);
        }
        __builtin_prefetch(m_rxBuffer.ptrAt<std::uint8_t>(m_rxTail * BuffSize), 0, 1);

        // Volatile read — NIC writes statusError via DMA; without volatile the
        // compiler may cache this read and spin forever on a stale value.
        auto status = *reinterpret_cast<volatile const std::uint32_t*>(&m_rxRing[m_rxTail].wb.statusError);
        if (!(status & RXD_STAT_DD)) [[likely]] {
            return {};
        }

        // Read barrier — ensure subsequent reads (length, packet data) see the
        // NIC's completed write, not stale prefetched values.  On x86 this is
        // a compiler barrier only (strong memory model); matches igb's dma_rmb().
        asm volatile("" ::: "memory");

        return {m_rxBuffer.ptrAt<std::uint8_t>(m_rxTail * BuffSize), m_rxRing[m_rxTail].wb.length};
    }

    [[gnu::always_inline, gnu::hot]]
    inline void release() noexcept
        requires (HAS_RX)
    {
        RxDescriptor& desc = m_rxRing[m_rxTail];
        // Reset to read format — restore buffer address for hardware reuse
        desc.read.pktAddr = m_rxBuffer.physicalAddrAt(m_rxTail * BuffSize);
        desc.read.hdrAddr = 0;
        m_rxTail          = (m_rxTail + 1) & RX_RING_MASK;

        // Batch RDT writes (DPDK igb_rxtx.c:965-976): instead of an MMIO write
        // per packet, defer until rx_free_thresh descriptors have been released.
        // Each MMIO write costs ~100-200ns on PCIe; batching amortises this.
        m_rxHeld++;
        if (m_rxHeld >= RX_FREE_THRESH) [[unlikely]] {
            asm volatile("" ::: "memory");
            writeReg(RDT0, static_cast<std::uint32_t>((m_rxTail - 1) & RX_RING_MASK));
            m_rxHeld = 0;
        }
    }

    // Receive functions on the hot path - END

    // Transmit functions on the hot path - START
    [[nodiscard, gnu::always_inline, gnu::hot]]
    inline std::uint8_t* acquire(std::uint32_t frameLen) noexcept {
        if (m_txInFlight >= NumTxDesc) [[unlikely]] {
            txReclaim();
            if (m_txInFlight >= NumTxDesc) [[unlikely]] {
                return nullptr;   // ring genuinely full
            }
        }

        // Prefetch Tx packet buffer for write — cache line allocated in
        // Modified state before memcpy, avoiding a cold write miss.
        __builtin_prefetch(m_txBuffer.ptrAt<std::uint8_t>(m_txTail * BuffSize), 1, 3);

        TxDescriptor& desc   = m_txRing[m_txTail];
        desc.read.bufferAddr = m_txBuffer.physicalAddrAt(m_txTail * BuffSize);
        desc.read.cmdTypeLen = TXD_CMD_EOP | TXD_CMD_IFCS | TXD_CMD_RS | TXD_CMD_DEXT | TXD_DTYP_DATA |
                               frameLen;
        desc.read.olinfoStatus = static_cast<std::uint32_t>(frameLen) << TXD_PAYLEN_SHIFT;

        return m_txBuffer.ptrAt<std::uint8_t>(m_txTail * BuffSize);
    }

    [[gnu::hot, gnu::always_inline]]
    inline void commit() noexcept {
        // Increment the tail after filling the descriptor then write the new value to the TDT
        m_txTail = (m_txTail + 1) & TX_RING_MASK;
        m_txInFlight++;
        // Write barrier — ensure descriptor + packet data writes are visible
        // before the NIC sees the new tail.  Matches igb's dma_wmb().
        asm volatile("" ::: "memory");
        writeReg(TDT0, static_cast<std::uint32_t>(m_txTail));
    }

    [[nodiscard, gnu::always_inline]]
    inline bool send(std::span<const std::uint8_t> frame) noexcept
        requires (HAS_TX)
    {
        auto* buf = acquire(static_cast<std::uint32_t>(frame.size()));
        if (!buf) [[unlikely]] {
            return false;
        }
        std::memcpy(buf, frame.data(), frame.size());
        commit();
        return true;
    }

    // Transmit functions on the hot path - END
private:
    PCIeBackend<InterfaceDiscovery> m_pciBusHandler;

    // Rx
    DMARing<RxDescriptor> m_rxRing;
    HugepageBuffer        m_rxBuffer;
    std::size_t           m_rxTail{};
    std::uint32_t         m_rxHeld{};   // count of released-but-not-flushed descriptors

    // Tx
    DMARing<TxDescriptor>       m_txRing;
    HugepageBuffer              m_txBuffer;
    std::size_t                 m_txTail{};
    std::size_t                 m_txCleanHead{};
    std::size_t                 m_txInFlight{};
    std::array<std::uint8_t, 6> m_mac{};

    static constexpr std::uint32_t rctlBsize = []() consteval {
        if constexpr (BuffSize == 256) {
            return RCTL_BSIZE_256;
        }
        if constexpr (BuffSize == 512) {
            return RCTL_BSIZE_512;
        }
        if constexpr (BuffSize == 1024) {
            return RCTL_BSIZE_1024;
        }
        return RCTL_BSIZE_2048;
    }();

    // Register access - volatile MMIO
    [[gnu::always_inline]]
    inline void writeReg(std::uint32_t offset, std::uint32_t value) {
        *m_pciBusHandler.template registerPtr<std::uint32_t>(offset) = value;
    }

    [[nodiscard, gnu::always_inline]]
    inline std::uint32_t readReg(std::uint32_t offset) const {
        return *m_pciBusHandler.template registerPtr<std::uint32_t>(offset);
    }

    // NIC-specific: disable PCIe ASPM via PCI config space.
    // Uses BDF from the InterfaceDiscovery policy to open config.
    void disableASPM() {
        const std::string path = "/sys/bus/pci/devices/" + std::string(m_pciBusHandler.policy().bdf()) +
                                 "/config";
        const int fd = ::open(path.c_str(), O_RDWR);
        if (fd < 0) {
            return;
        }

        // Walk PCI capabilities list to find PCIe capability (ID=0x10)
        std::uint8_t capPtr{};
        if (::pread(fd, &capPtr, 1, 0x34) != 1) {
            ::close(fd);
            return;
        }

        while (capPtr != 0) {
            std::uint8_t capId{};
            if (::pread(fd, &capId, 1, capPtr) != 1) {
                break;
            }

            if (capId == 0x10) {
                std::uint16_t linkCtl{};
                const off_t   off = capPtr + 0x10;
                if (::pread(fd, &linkCtl, sizeof(linkCtl), off) == sizeof(linkCtl)) {
                    linkCtl &= static_cast<std::uint16_t>(~0x0003);
                    const ssize_t w = ::pwrite(fd, &linkCtl, sizeof(linkCtl), off);
                    std::fprintf(stderr, w == static_cast<ssize_t>(sizeof(linkCtl))
                                             ? "[I210] PCIe ASPM disabled\n"
                                             : "[I210] WARN: PCIe ASPM disable write FAILED\n");
                }
                break;
            }
            if (::pread(fd, &capPtr, 1, capPtr + 1) != 1) {
                break;
            }
        }
        ::close(fd);
    }

    void reset() {
        // Issue device reset
        writeReg(CTRL, readReg(CTRL) | CTRL_DEV_RST);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));

        // Wait for reset to complete (PF_RST_DONE in STATUS bit 21)
        for (int i{}; i < 100; i++) {
            if (readReg(STATUS) & (1U << 21)) {
                std::fprintf(stderr, "[i210] Device reset complete.\n");
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        std::fprintf(stderr, "[I210] Warning: reset did not complete within timeout.\n");
    }

    // Verify all power-saving features are disabled for low-latency operation
    void verifyLowLatencyConfig() {
        constexpr std::uint32_t EEER_R     = 0x0E30;
        constexpr std::uint32_t IPCNFG_R   = 0x0E38;
        constexpr std::uint32_t DMACR_R    = 0x02508;
        constexpr std::uint32_t PCIEMISC_R = 0x05BB8;
        constexpr std::uint32_t TQAVCTRL_R = 0x03570;

        int  warnings = 0;
        auto check = [&warnings](const char* name, std::uint32_t reg, std::uint32_t mask, bool expectClear) {
            const bool bitSet = (reg & mask) != 0;
            if (bitSet == expectClear) {
                std::fprintf(stderr, "[I210] WARN: %s=0x%08x (bit 0x%x should be %s)\n", name, reg, mask,
                             expectClear ? "0" : "1");
                warnings++;
            }
        };

        const std::uint32_t eeer     = readReg(EEER_R);
        const std::uint32_t ipcnfg   = readReg(IPCNFG_R);
        const std::uint32_t dmacr    = readReg(DMACR_R);
        const std::uint32_t pciemisc = readReg(PCIEMISC_R);
        const std::uint32_t ctrlExt  = readReg(CTRL_EXT);
        const std::uint32_t tqavctrl = readReg(TQAVCTRL_R);
        const std::uint32_t rxdctl   = readReg(RXDCTL0);
        const std::uint32_t txdctl   = readReg(TXDCTL0);
        const std::uint32_t eitr     = readReg(EITR0);

        // EEE MAC-side: LPI must be off
        check("EEER", eeer, 0x00070000, true);       // TX_LPI | RX_LPI | LPI_FC
        check("IPCNFG", ipcnfg, 0x0000000C, true);   // EEE_1G | EEE_100M

        // EEE PHY-side: read EEE advertisement via MMD (device 7, register 60)
        // PHY regs 13/14 are the MMD access control/data pair (IEEE 802.3 clause 45)
        constexpr std::uint16_t MMD_EEE_DEV   = 7;
        constexpr std::uint16_t MMD_EEE_ADDR  = 60;   // EEE Advertisement register
        constexpr std::uint16_t MMD_FUNC_DATA = 0x4000;
        // Best-effort MMD walk (this is the diagnostic path): a failed write just means
        // the readback below reports a stale/zero advertisement, which the check handles.
        (void)writePhy(13, MMD_EEE_DEV);                   // Select device 7
        (void)writePhy(14, MMD_EEE_ADDR);                  // Select register 60
        (void)writePhy(13, MMD_FUNC_DATA | MMD_EEE_DEV);   // Switch to data mode
        const std::uint16_t eeeAdv = readPhy(14);          // Read EEE advertisement
        (void)writePhy(13, 0);                             // Release MMD
        if (eeeAdv & 0x0006) {                             // bit 1 = 100M EEE, bit 2 = 1G EEE
            std::fprintf(stderr, "[I210] WARN: PHY EEE advertisement=0x%04x (should be 0)\n", eeeAdv);
            warnings++;
        }

        // DMA coalescing: must be fully off
        check("DMACR", dmacr, 0x80000000, true);   // DMAC_EN

        // PCIe Lx: NIC must not autonomously enter low-power
        check("PCIEMISC", pciemisc, 0x00000080, true);   // LX_DECISION

        // PHY power management: must be off
        check("CTRL_EXT", ctrlExt, 0x00100000, true);   // PHYPDEN
        check("CTRL_EXT", ctrlExt, 0x00200000, true);   // PHY_PWR_MGMT

        // Qav/FQTSS: must be off (adds launch-time delays)
        check("TQAVCTRL", tqavctrl, 0x00000001, true);   // XMIT_MODE

        // Flow control: must be off (PAUSE frames add unpredictable latency)
        const std::uint32_t ctrl = readReg(CTRL);
        check("CTRL", ctrl, CTRL_RFCE, true);   // Rx Flow Control
        check("CTRL", ctrl, CTRL_TFCE, true);   // Tx Flow Control

        // EITR: must be max value to disable interrupt-driven write-back (DPDK default)
        // Hardware masks bits [1:0] so 0xFFFF reads back as 0xFFFC — both are correct.
        if (eitr != 0xFFFF && eitr != 0xFFFC) {
            std::fprintf(stderr, "[I210] WARN: EITR0=0x%08x (expected 0xFFFC)\n", eitr);
            warnings++;
        }

        // DVMOLR: per-queue CRC stripping must be enabled (I210-specific)
        constexpr std::uint32_t DVMOLR0_R = 0x0C038;
        const std::uint32_t     dvmolr    = readReg(DVMOLR0_R);
        check("DVMOLR0", dvmolr, 0x80000000, false);   // STRCRC must be set

        // Queues: must be enabled
        check("RXDCTL0", rxdctl, RXDCTL_ENABLE, false);
        check("TXDCTL0", txdctl, TXDCTL_ENABLE, false);

        if (warnings == 0) {
            std::fprintf(stderr, "[I210] Low-latency config verified: all power-saving disabled\n");
        } else {
            std::fprintf(stderr, "[I210] Low-latency config: %d warning(s) — check register values above\n",
                         warnings);
        }
    }

    void disableInterrupts() {
        writeReg(IMC, IRQ_DISABLE_ALL);
        writeReg(EIMC, IRQ_DISABLE_ALL);
        // Clear any pending
        (void)readReg(ICR);
        (void)readReg(EICR);
        // EITR=0 → no interrupt throttle timer, descriptors written back
        // immediately (Section 7.1.4.4 / 7.2.2.6: EITR controls write-back
        // timing for BOTH Rx and Tx even without interrupts enabled)
        // EITR[n] = 0x1680 + 4*n, n = 0..4
        // DPDK sets 0xFFFF (max delay) to disable interrupt-driven write-back
        // entirely — write-back then only happens via WTHRESH/RS bit.
        for (std::uint32_t i{}; i < 5; i++) {
            writeReg(EITR0 + 4 * i, 0xFFFF);
        }

        // Disable all wake-up sources (DPDK: E1000_WRITE_REG(hw, E1000_WUC, 0))
        writeReg(WUC, 0);
    }

    // Disable Energy Efficient Ethernet (EEE / 802.3az)
    // EEE puts the PHY into Low Power Idle (LPI) between packets.
    // Wake-up from LPI adds ~30us latency — unacceptable for low-latency polling.
    // Matches igb's igb_set_eee_i350() with eee_disable=true.
    void disableEEE() {
        constexpr std::uint32_t EEER   = 0x0E30;
        constexpr std::uint32_t IPCNFG = 0x0E38;

        constexpr std::uint32_t EEER_TX_LPI_EN     = 0x00010000;
        constexpr std::uint32_t EEER_RX_LPI_EN     = 0x00020000;
        constexpr std::uint32_t EEER_LPI_FC        = 0x00040000;
        constexpr std::uint32_t IPCNFG_EEE_1G_AN   = 0x00000008;
        constexpr std::uint32_t IPCNFG_EEE_100M_AN = 0x00000004;

        std::uint32_t eeer = readReg(EEER);
        eeer &= ~(EEER_TX_LPI_EN | EEER_RX_LPI_EN | EEER_LPI_FC);
        writeReg(EEER, eeer);

        std::uint32_t ipcnfg = readReg(IPCNFG);
        ipcnfg &= ~(IPCNFG_EEE_1G_AN | IPCNFG_EEE_100M_AN);
        writeReg(IPCNFG, ipcnfg);

        // Disable DMA Coalescing — zero the entire register to clear
        // DMAC_EN, DMAC_LX_MASK (Lx transitions), and watchdog timer.
        constexpr std::uint32_t DMACR  = 0x02508;
        constexpr std::uint32_t DMCTLX = 0x02514;   // DMA Coalescing Time to Lx
        writeReg(DMACR, 0);
        writeReg(DMCTLX, 0);

        // Disable NIC-autonomous PCIe Lx transitions (PCIEMISC.LX_DECISION).
        // This is SEPARATE from ASPM — the NIC can enter Lx on its own even
        // with ASPM disabled at the link level. Set by igb_init_dmac().
        constexpr std::uint32_t PCIEMISC             = 0x05BB8;
        constexpr std::uint32_t PCIEMISC_LX_DECISION = 0x00000080;
        std::uint32_t           pciemisc             = readReg(PCIEMISC);
        pciemisc &= ~PCIEMISC_LX_DECISION;
        writeReg(PCIEMISC, pciemisc);

        // Disable PHY power-down and power management in CTRL_EXT
        constexpr std::uint32_t CTRL_EXT_PHYPDEN      = 0x00100000;   // PHY Power Down Enable
        constexpr std::uint32_t CTRL_EXT_PHY_PWR_MGMT = 0x00200000;   // PHY Power Management
        std::uint32_t           ctrlExt               = readReg(CTRL_EXT);
        ctrlExt &= ~(CTRL_EXT_PHYPDEN | CTRL_EXT_PHY_PWR_MGMT);
        writeReg(CTRL_EXT, ctrlExt);

        std::fprintf(stderr, "[I210] Power saving disabled (EEE + DMAC + PCIe Lx + PHY PM)\n");
    }

    // Clear "Go Link Down" in PHY power management (DPDK: e1000_82575.c:1588)
    // If set, the PHY can autonomously take the link down.
    void clearPhyLinkDown() {
        constexpr std::uint32_t PHY_POWER_MGMT = 0xE854;   // E1000_82580_PHY_POWER_MGMT
        constexpr std::uint32_t PM_GO_LINKD    = 0x00000001;
        std::uint32_t           phpm           = readReg(PHY_POWER_MGMT);
        phpm &= ~PM_GO_LINKD;
        writeReg(PHY_POWER_MGMT, phpm);
    }

    // Zero the Multicast Table Array (128 × 32-bit entries at 0x5200)
    // Stale entries from the igb driver can cause the NIC to accept
    // unwanted multicast packets, wasting DMA bandwidth and cache.
    void zeroMulticastTable() {
        constexpr std::uint32_t MTA_BASE = 0x5200;
        for (int i = 0; i < 128; i++) {
            writeReg(MTA_BASE + static_cast<std::uint32_t>(i) * 4, 0);
        }
    }

    // PHY register access via MDIC (Section 8.2.4)
    [[nodiscard]]
    bool writePhy(std::uint32_t reg, std::uint16_t value) {
        const std::uint32_t cmd = static_cast<std::uint32_t>(value) | (reg << MDIC_REGADD_SHIFT) |
                                  MDIC_OP_WRITE;
        writeReg(MDIC, cmd);

        for (int i{}; i < 100; i++) {
            const std::uint32_t mdic = readReg(MDIC);
            if (mdic & MDIC_READY) {
                return (mdic & MDIC_ERROR) == 0;
            }
            std::this_thread::sleep_for(std::chrono::microseconds(10));
        }
        std::fprintf(stderr, "[I210] MDIC write timeout (reg %u)\n", reg);
        return false;
    }

    [[nodiscard]]
    std::uint16_t readPhy(std::uint32_t reg) {
        const std::uint32_t cmd = (reg << MDIC_REGADD_SHIFT) | MDIC_OP_READ;
        writeReg(MDIC, cmd);

        for (int i{}; i < 100; i++) {
            const std::uint32_t mdic = readReg(MDIC);
            if (mdic & MDIC_READY) {
                if (mdic & MDIC_ERROR) {
                    std::fprintf(stderr, "[I210] MDIC read error (reg %u)\n", reg);
                    return 0;
                }
                return static_cast<std::uint16_t>(mdic & MDIC_DATA_MASK);
            }
            std::this_thread::sleep_for(std::chrono::microseconds(10));
        }
        std::fprintf(stderr, "[I210] MDIC read timeout (reg %u)\n", reg);
        return 0;
    }

    // PHY initialisation: reset PHY, advertise all speeds, restart auto-negotiation
    bool initPhy() {
        // 1. Reset PHY (self-clearing bit)
        if (!writePhy(PHY_BMCR, BMCR_RESET)) {
            std::fprintf(stderr, "[I210] PHY reset write failed\n");
            return false;
        }

        // 2. Wait for reset to self-clear (typically <1ms)
        for (int i{}; i < 100; i++) {
            if (!(readPhy(PHY_BMCR) & BMCR_RESET)) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        // Advertise + restart autoneg. Results deliberately ignored: a failed MDIC write
        // leaves the PHY on its previous (working) advertisement, and the link check that
        // follows init() is what actually gates the run.
        // 3. Advertise 10/100 Mbps modes
        (void)writePhy(PHY_ANAR, ANAR_SELECTOR | ANAR_10_HD | ANAR_10_FD | ANAR_100_HD | ANAR_100_FD);

        // 4. Advertise 1000 Mbps full duplex
        (void)writePhy(PHY_GBCR, GBCR_1000_FD);

        // 5. Enable auto-negotiation and restart it
        (void)writePhy(PHY_BMCR, BMCR_ANE | BMCR_RESTART_AN);

        std::fprintf(stderr, "[I210] PHY initialised: auto-negotiation restarted\n");
        return true;
    }

    void readMacAddress() {
        const std::uint32_t ral = readReg(RAL0);
        const std::uint32_t rah = readReg(RAH0);

        m_mac[0] = static_cast<std::uint8_t>(ral & 0xFF);
        m_mac[1] = static_cast<std::uint8_t>((ral >> 8) & 0xFF);
        m_mac[2] = static_cast<std::uint8_t>((ral >> 16) & 0xFF);
        m_mac[3] = static_cast<std::uint8_t>((ral >> 24) & 0xFF);
        m_mac[4] = static_cast<std::uint8_t>(rah & 0xFF);
        m_mac[5] = static_cast<std::uint8_t>((rah >> 8) & 0xFF);

        std::fprintf(stderr, "[I210] MAC: %02x:%02x:%02x:%02x:%02x:%02x \n", m_mac[0], m_mac[1], m_mac[2],
                     m_mac[3], m_mac[4], m_mac[5]);
    }

    bool initRx()
        requires (HAS_RX)
    {
        // 1. Allocate a region of memory for the receive descriptor list.
        if (!m_rxRing.allocate(NumRxDesc)) {
            std::fprintf(stderr, "[I210] Rx descriptor ring allocation failed.\n");
            return false;
        }

        // 2. Receive buffers of appropriate size should be allocated and pointers to these buffers should be
        // stored in the descriptor ring.
        if (!m_rxBuffer.allocate(NumRxDesc * BuffSize)) {
            std::fprintf(stderr, "[I210] Rx data buffer allocation failed.\n");
            return false;
        }

        // 3. Program the descriptor base address with the address of the region.
        for (std::size_t i{}; i < NumRxDesc; i++) {
            m_rxRing[i].read.pktAddr = m_rxBuffer.physicalAddrAt(i * BuffSize);
            m_rxRing[i].read.hdrAddr = 0;   // single-buffer mode, no header split
        }

        // 4. Program descriptor base address (base addr before length — matches igb driver order)
        const std::uint64_t rxBase = m_rxRing.physicalBase();
        writeReg(RDBAL0, static_cast<std::uint32_t>(rxBase & 0xFFFFFFFF));
        writeReg(RDBAH0, static_cast<std::uint32_t>(rxBase >> 32));

        // 4. Set the length register to the size of the descriptor ring.
        writeReg(RDLEN0, static_cast<std::uint32_t>(m_rxRing.sizeBytes()));

        // 5. Program SRRCTL of the queue according to the size of the buffers, the required header handling
        // and the drop policy.
        constexpr std::uint32_t bSizeKB = BuffSize / 1024;
        writeReg(SRRCTL0, (bSizeKB << SRRCTL_BSIZEPACKET_SHIFT) | SRRCTL_DESCTYPE_ADV_ONE | SRRCTL_DROP_EN);

        // TODO: 6. If header split or header replication is required for this queue, program the PSRTYPE
        // register according to the required headers ???

        // 7. Enable global receiver first — igb driver sets RCTL.RXEN in igb_setup_rctl()
        //    BEFORE enabling individual queues. RXDCTL.ENABLE won't stick without this.
        writeReg(RCTL, RCTL_RXEN | RCTL_BAM | RCTL_SECRC | rctlBsize);

        // 7. Enable the queue by setting RXDCTL.ENABLE. In the case of queue zero, the enable bit is set by
        // default - so the ring parameters should be set before RCTL.RXEN is set.
        //    igb driver sequence: disable queue, configure ring, then single write with ENABLE + thresholds
        writeReg(RXDCTL0, 0);
        writeReg(RDT0, 0);

        // Thresholds tuned for minimum latency (Section 7.1.4.4):
        // PTHRESH/HTHRESH > 0 for descriptor prefetching (NIC caches descriptors in advance)
        // WTHRESH=0 + EITR=0 → each descriptor written back immediately, no batching
        const std::uint32_t rxdctl = (8U << 0)      // PTHRESH = 8
                                     | (4U << 8)    // HTHRESH = 4
                                     | (0U << 16)   // WTHRESH = 0
                                     | RXDCTL_ENABLE;
        writeReg(RXDCTL0, rxdctl);

        // 8. Poll the RXDCTL register until the ENABLE bit is set. The tail should not be bumped before this
        // bit was read as one.
        bool rxEnabled = false;
        for (int i{}; i < 100; i++) {
            if (readReg(RXDCTL0) & RXDCTL_ENABLE) {
                rxEnabled = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (!rxEnabled) {
            std::fprintf(stderr, "[I210] RXDCTL0 enable timed out (RXDCTL0=0x%08x)\n", readReg(RXDCTL0));
            return false;
        }

        // 9. Program the direction of packets to this queue according to the mode selected in the MRQC
        // register. Packets directed to a disabled queue are dropped.
        // TODO:

        m_rxTail = 0;
        writeReg(RDT0, static_cast<std::uint32_t>(NumRxDesc - 1));

        // 10. DVMOLR per-queue CRC stripping (I210-specific, DPDK igb_rxtx.c:2559)
        //     Required in addition to RCTL_SECRC for I210/I350.
        constexpr std::uint32_t DVMOLR0       = 0x0C038;   // DMA VM Offload queue 0
        constexpr std::uint32_t DVMOLR_STRCRC = 0x80000000;
        std::uint32_t           dvmolr        = readReg(DVMOLR0);
        dvmolr |= DVMOLR_STRCRC;
        writeReg(DVMOLR0, dvmolr);

        std::fprintf(stderr, "[I210] Rx queue 0 initialised: %zu descriptors, %zu bytes buffers\n", NumRxDesc,
                     BuffSize);
        return true;
    }

    bool initTx()
        requires (HAS_TX)
    {
        // 1. Allocate the descriptor ring on hugepages
        if (!m_txRing.allocate(NumTxDesc)) {
            std::fprintf(stderr, "[I210] Tx descriptor ring allocation failed.\n");
            return false;
        }

        // 2. Allocate packet data buffers on hugepages
        if (!m_txBuffer.allocate(NumTxDesc * BuffSize)) {
            std::fprintf(stderr, "[I210] Tx data buffer allocation failed.\n");
            return false;
        }

        // 3. Program descriptor base address
        const std::uint64_t txBase = m_txRing.physicalBase();
        writeReg(TDBAL0, static_cast<std::uint32_t>(txBase & 0xFFFFFFFF));
        writeReg(TDBAH0, static_cast<std::uint32_t>(txBase >> 32));

        // 4. Program ring length bytes
        writeReg(TDLEN0, static_cast<std::uint32_t>(m_txRing.sizeBytes()));

        // 5. Program TXDCTL : WTHRESH=0, disable queue for configuration
        writeReg(TXDCTL0, 0);

        // 6. Enable the queue (DD polling for Tx completion, matching DPDK)
        //    No Head Write-Back — simpler and avoids wrap ambiguity.
        writeReg(TXDCTL0, readReg(TXDCTL0) | TXDCTL_ENABLE);
        bool txEnabled = false;
        for (int i{}; i < 100; i++) {
            if (readReg(TXDCTL0) & TXDCTL_ENABLE) {
                txEnabled = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (!txEnabled) {
            std::fprintf(stderr, "[I210] TXDCTL0 enable timed out\n");
            return false;
        }

        // 7. Set the TIPG to default values for 1000BASE-T
        writeReg(TIPG, TIPG_DEFAULT);

        // 8. Enable transmitter (DPDK also sets TCTL_RTLC for late collision retry)
        writeReg(TCTL, TCTL_EN | TCTL_PSP | TCTL_CT_IEEE | TCTL_BST_DEF | TCTL_RTLC);

        m_txTail      = 0;
        m_txCleanHead = 0;
        m_txInFlight  = 0;

        std::fprintf(stderr, "[I210] Tx queue 0 initialised: %zu descriptors, %zu byte buffers.\n", NumTxDesc,
                     BuffSize);
        return true;
    }

    [[gnu::always_inline, gnu::hot]]
    inline void txReclaim()
        requires (HAS_TX)
    {
        // DD polling (matches DPDK igb_rxtx.c:493) — poll the DD bit in each
        // completed descriptor. Simpler than Head Write-Back and avoids the
        // circular buffer wrap ambiguity.
        while (m_txInFlight > 0) {
            auto status = *reinterpret_cast<volatile const std::uint32_t*>(
                &m_txRing[m_txCleanHead].wb.status);
            if (!(status & TXD_STAT_DD)) {
                break;
            }
            m_txRing[m_txCleanHead].wb.status = 0;
            m_txCleanHead                     = (m_txCleanHead + 1) & TX_RING_MASK;
            m_txInFlight--;
        }
    }
};

#endif   // ABTEDGE_INTEL_I210_H
