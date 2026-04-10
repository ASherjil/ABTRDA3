//
// Created by asherjil on 4/5/26.
//

#ifndef ABTEDGE_INTEL_I210_H
#define ABTEDGE_INTEL_I210_H

#include "I210Registers.hpp"
#include "RxFrame.hpp"
#include "DMARing.hpp"
#include "HugepageBuffer.hpp"

#include <array>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <sys/mman.h>
#include <thread>
#include <unistd.h>

enum class DriverMode : std::uint8_t {
  RxOnly,
  TxOnly,
  RxTx
};

template<DriverMode M, std::size_t NumRxDesc = 256, std::size_t NumTxDesc = 256, std::size_t BuffSize = 2048>
class Intel_I210 {
  static_assert(M == DriverMode::TxOnly || (NumRxDesc >= 8 && (NumRxDesc & (NumRxDesc - 1)) == 0),
                "NumRxDesc must be a power of 2 and >= 8");
  static_assert(M == DriverMode::RxOnly || (NumTxDesc >= 8 && (NumTxDesc & (NumTxDesc - 1)) == 0),
                "NumTxDesc must be a power of 2 and >= 8");
  static_assert(BuffSize == 256 || BuffSize == 512 || BuffSize == 1024 || BuffSize == 2048,
                "Buffer size must be power of 2: 256 -> 2048");

  static constexpr bool HAS_RX = (M == DriverMode::RxOnly || M == DriverMode::RxTx);
  static constexpr bool HAS_TX = (M == DriverMode::TxOnly || M == DriverMode::RxTx);
  static constexpr std::size_t RX_RING_MASK = NumRxDesc - 1;
  static constexpr std::size_t TX_RING_MASK = NumTxDesc - 1;

public:
  // Rule of 5
  Intel_I210() = default;
  Intel_I210(const Intel_I210&) = delete;
  Intel_I210& operator=(const Intel_I210&) = delete;

  Intel_I210(Intel_I210&& other) noexcept
     : m_regs{std::exchange(other.m_regs, nullptr)},
       m_barFd{std::exchange(other.m_barFd, -1)},
       m_barSize{std::exchange(other.m_barSize, 0)},
       m_bdf{std::move(other.m_bdf)},
       m_rxRing{std::move(other.m_rxRing)},
       m_rxBuffer{std::move(other.m_rxBuffer)},
       m_rxTail{std::exchange(other.m_rxTail, 0)},
       m_txRing{std::move(other.m_txRing)},
       m_txBuffer{std::move(other.m_txBuffer)},
       m_txTail{std::exchange(other.m_txTail, 0)},
       m_txCleanHead{std::exchange(other.m_txCleanHead, 0)},
       m_txInFlight{std::exchange(other.m_txInFlight, 0)},
       m_mac{std::move(other.m_mac)} {}


  Intel_I210& operator=(Intel_I210&& other) noexcept {
    if (this != &other) {
      shutdown();
      m_regs        = std::exchange(other.m_regs, nullptr);
      m_barFd       = std::exchange(other.m_barFd, -1);
      m_barSize     = std::exchange(other.m_barSize, 0);
      m_bdf         = std::move(other.m_bdf);
      m_rxRing      = std::move(other.m_rxRing);
      m_rxBuffer    = std::move(other.m_rxBuffer);
      m_rxTail      = std::exchange(other.m_rxTail, 0);
      m_txRing      = std::move(other.m_txRing);
      m_txBuffer    = std::move(other.m_txBuffer);
      m_txTail      = std::exchange(other.m_txTail, 0);
      m_txCleanHead = std::exchange(other.m_txCleanHead, 0);
      m_txInFlight  = std::exchange(other.m_txInFlight, 0);
      m_mac         = std::move(other.m_mac);
    }
    return *this;
  }

  ~Intel_I210() {
    shutdown(); // call shutdown makes it easier for debugging
  }

  // Initialisation functions on the cold path. START
  [[nodiscard]] bool init(std::string_view pciBdf) {
    m_bdf = std::string(pciBdf);

    if (!unbindKernelDriver()) {
      return false;
    }
    if (!mmapBar0()) {
      return false;
    }
    if (!enableBusMaster()) {
      return false;
    }

    reset();
    disableInterrupts();
    readMacAddress();

    // Initialise PHY: reset, advertise all speeds, restart auto-negotiation
    if (!initPhy()) {
      return false;
    }

    // Set link up so MAC recognises PHY's LINK indication (Section 3.7.4.4.1)
    // Also clear FRCSPD so MAC auto-detects speed from PHY (Section 3.7.4.4.2)
    std::uint32_t ctrl = readReg(CTRL);
    ctrl |= CTRL_SLU;
    ctrl &= ~(CTRL_FRCSPD | CTRL_FRCDPLX);
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
        return true;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    }
    std::fprintf(stderr, "[I210] Warning: link not up after 5s\n");
    return true;
  }

  void shutdown() {
    if (!m_regs) {
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

    // Unmap BAR0
    ::munmap(const_cast<std::uint32_t*>(m_regs), m_barSize);
    m_regs = nullptr;

    if (m_barFd >= 0) {
      ::close(m_barFd);
      m_barFd = -1;
    }

    //rebindKernelDriver();
  }

  [[nodiscard]] bool isLinkUp() const {
    return (readReg(STATUS) & STATUS_LU) != 0;
  }

  [[nodiscard]] std::uint32_t linkSpeedMbps() const {
    std::uint32_t speed = (readReg(STATUS) & STATUS_SPEED_MASK) >> STATUS_SPEED_SHIFT;
    constexpr std::array<std::uint32_t, 4> speeds = {10, 100, 1000, 1000};
    return speeds[speed];
  }

  [[nodiscard]] std::array<std::uint8_t, 6> macAddress() const {
    return m_mac;
  }

  void prefillRing(std::span<const std::uint8_t> frameTemplate) noexcept requires (HAS_TX) {
    for (std::size_t i{}; i < NumTxDesc; i++) {
      std::memcpy(m_txBuffer.ptrAt<std::uint8_t>(i * BuffSize),
                  frameTemplate.data(), frameTemplate.size());
    }
  }
  // Initialisation functions on the cold path. END

  // Receive functions on the hot path - START
  [[nodiscard, gnu::always_inline]]
  inline RxFrame tryReceive() noexcept requires (HAS_RX){
    const RxDescriptor& desc = m_rxRing[m_rxTail];
    if (!(desc.wb.statusError & RXD_STAT_DD))[[likely]] {
      return {};
    }

    return {
      .data   = {m_rxBuffer.ptrAt<std::uint8_t>(m_rxTail * BuffSize), desc.wb.length},
      .sec    = 0,
      .nsec   = 0,
      .status = 1   // non-zero = frame present
    };
  }

  [[gnu::always_inline]]
  inline void release() noexcept requires (HAS_RX){
    RxDescriptor& desc = m_rxRing[m_rxTail];
    // Reset to read format — restore buffer address for hardware reuse
    desc.read.pktAddr = m_rxBuffer.physicalAddrAt(m_rxTail * BuffSize);
    desc.read.hdrAddr = 0;
    std::size_t prev = m_rxTail;
    m_rxTail = (m_rxTail + 1) & RX_RING_MASK;
    writeReg(RDT0, static_cast<std::uint32_t>(prev));
  }
  // Receive functions on the hot path - END

  // Transmit functions on the hot path - START
  [[nodiscard, gnu::always_inline, gnu::hot]]
  inline std::uint8_t* acquire(std::uint32_t frameLen) noexcept {
    if (m_txInFlight >= NumTxDesc)[[unlikely]] {
      txReclaim();
      if (m_txInFlight >= NumTxDesc)[[unlikely]] {
        return nullptr; // ring genuinely full
      }
    }

    TxDescriptor& desc = m_txRing[m_txTail];
    desc.read.bufferAddr   = m_txBuffer.physicalAddrAt(m_txTail * BuffSize);
    desc.read.cmdTypeLen   = TXD_CMD_EOP | TXD_CMD_IFCS | TXD_CMD_RS
                           | TXD_CMD_DEXT | TXD_DTYP_DATA
                           | frameLen;
    desc.read.olinfoStatus = static_cast<std::uint32_t>(frameLen) << TXD_PAYLEN_SHIFT;

    return m_txBuffer.ptrAt<std::uint8_t>(m_txTail * BuffSize);
  }

  [[gnu::hot, gnu::always_inline]]
  inline void commit() noexcept {
    // Increment the tail after filling the descriptor then write the new value to the TDT
    m_txTail = (m_txTail + 1) & TX_RING_MASK;
    m_txInFlight++;
    writeReg(TDT0, static_cast<std::uint32_t>(m_txTail));
  }

  [[nodiscard, gnu::always_inline]]
  inline bool send(std::span<const std::uint8_t> frame) noexcept requires (HAS_TX) {
    auto* buf = acquire(static_cast<std::uint32_t>(frame.size()));
    if (!buf) [[unlikely]]
      return false;
    std::memcpy(buf, frame.data(), frame.size());
    commit();
    return true;
  }
  // Transmit functions on the hot path - END
private:
  volatile std::uint32_t* m_regs{nullptr};
  int m_barFd{-1};
  std::size_t m_barSize{};
  std::string m_bdf{};

  // Rx
  DMARing<RxDescriptor>       m_rxRing;
  HugepageBuffer              m_rxBuffer;
  std::size_t                 m_rxTail{};

  // Tx
  DMARing<TxDescriptor>       m_txRing;
  HugepageBuffer              m_txBuffer;
  std::size_t                 m_txTail{};
  std::size_t                 m_txCleanHead{};
  std::size_t                 m_txInFlight{};
  std::array<std::uint8_t, 6> m_mac;

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
    m_regs[offset/4] = value;
  }

  [[nodiscard, gnu::always_inline]]
  inline std::uint32_t readReg(std::uint32_t offset) const{
    return m_regs[offset/4];
  }

  // Initialisation Helper functions
  bool unbindKernelDriver() {
    // Write BDF to /sys/bus/pci/drivers/igb/unbind
    std::string path = "/sys/bus/pci/drivers/igb/unbind";
    int fd = ::open(path.c_str(), O_WRONLY);
    if (fd < 0) {
      // Driver may already be unbound
      std::fprintf(stderr, "[I210] igb driver not bound(or permission denied).\n");
      return true;
    }
    ssize_t written = ::write(fd, m_bdf.c_str(), m_bdf.size());
    ::close(fd);

    // Verify the driver is actually unbound by checking the sysfs symlink
    std::string boundPath = "/sys/bus/pci/drivers/igb/" + m_bdf;
    if (::access(boundPath.c_str(), F_OK) == 0) {
      std::fprintf(stderr, "[I210] Failed to unbind igb driver from %s\n", m_bdf.c_str());
      return false;
    }

    if (written > 0) {
      std::fprintf(stderr, "[I210] Unbound igb driver from %s\n", m_bdf.c_str());
    } else {
      std::fprintf(stderr, "[I210] igb driver already unbound from %s\n", m_bdf.c_str());
    }
    return true;
  }

  void rebindKernelDriver() {
    if (m_bdf.empty()) {
      return;
    }
    std::string path = "/sys/bus/pci/drivers/igb/bind";
    int fd = ::open(path.c_str(), O_WRONLY);
    if (fd < 0) {
      std::fprintf(stderr, "[I210] Cannot open igb bind (driver module not loaded?)\n");
      return;
    }
    ssize_t written = ::write(fd, m_bdf.c_str(), m_bdf.size());
    ::close(fd);
    if (written > 0) {
      std::fprintf(stderr, "[I210] Rebound igb driver to %s\n", m_bdf.c_str());
    } else {
      std::fprintf(stderr, "[I210] Failed to rebind igb driver to %s\n", m_bdf.c_str());
    }
  }

  bool mmapBar0() {
    // Open /sys/bus/pci/devices/{bdf}/resource0
    std::string path = "/sys/bus/pci/devices/" + m_bdf + "/resource0";
    m_barFd = ::open(path.c_str(), O_RDWR | O_SYNC);
    if (m_barFd < 0) {
      std::fprintf(stderr, "[I210] Cannot open %s\n", path.c_str());
      return false;
    }

    // Determine BAR size from resource file
    std::string resPath = "/sys/bus/pci/devices/" + m_bdf + "/resource";
    FILE* f = std::fopen(resPath.c_str(), "r");
    if (!f) {
      std::fprintf(stderr, "[I210] Cannot open %s\n", resPath.c_str());
      return false;
    }

    // First line: BAR0: start end flags
    std::uint64_t start{}, end{}, flags{};
    std::fscanf(f, "%" SCNx64 " %" SCNx64 " %" SCNx64, &start, &end, &flags);
    std::fclose(f);
    m_barSize = end - start + 1;

    void* mapped = ::mmap(nullptr, m_barSize, PROT_READ | PROT_WRITE, MAP_SHARED, m_barFd, 0);
    if (mapped == MAP_FAILED) {
      std::fprintf(stderr, "[I210] mmap BAR0 failed.\n");
      ::close(m_barFd);
      m_barFd = -1;
      return false;
    }

    m_regs = static_cast<volatile std::uint32_t*>(mapped);
    std::fprintf(stderr, "[I210] Mapped BAR0 at %p, size %zu\n", mapped, m_barSize);
    return true;
  }

  // Enable PCI bus mastering — required for DMA.
  // The kernel clears this when the igb driver unbinds.
  bool enableBusMaster() {
    std::string path = "/sys/bus/pci/devices/" + m_bdf + "/config";
    int fd = ::open(path.c_str(), O_RDWR);
    if (fd < 0) {
      std::fprintf(stderr, "[I210] Cannot open PCI config: %s\n", path.c_str());
      return false;
    }

    // PCI Command Register is at offset 0x04, 2 bytes
    std::uint16_t cmd{};
    if (::pread(fd, &cmd, sizeof(cmd), 0x04) != sizeof(cmd)) {
      std::fprintf(stderr, "[I210] Failed to read PCI command register\n");
      ::close(fd);
      return false;
    }

    cmd |= (1U << 2);  // Bus Master Enable

    if (::pwrite(fd, &cmd, sizeof(cmd), 0x04) != sizeof(cmd)) {
      std::fprintf(stderr, "[I210] Failed to write PCI command register\n");
      ::close(fd);
      return false;
    }

    ::close(fd);
    std::fprintf(stderr, "[I210] PCI bus mastering enabled\n");
    return true;
  }

  void reset() {
    // Issue device reset
    writeReg(CTRL, readReg(CTRL) | CTRL_DEV_RST);
    std::this_thread::sleep_for(std::chrono::milliseconds(5));

    // Wait for reset to complete (PF_RST_DONE in STATUS bit 21)
    for (int i{}; i<100; i++) {
      if (readReg(STATUS) & (1U << 21)) {
        std::fprintf(stderr, "[i210] Device reset complete.\n");
        return;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    std::fprintf(stderr, "[I210] Warning: reset did not complete within timeout.\n");
  }

  void disableInterrupts() {
    writeReg(IMC, IRQ_DISABLE_ALL);
    writeReg(EIMC, IRQ_DISABLE_ALL);
    // Clear any pending, TODO: what is this readReg?
    (void)readReg(ICR);
    (void)readReg(EICR);
  }

  // PHY register access via MDIC (Section 8.2.4)
  [[nodiscard]]
  bool writePhy(std::uint32_t reg, std::uint16_t value) {
    std::uint32_t cmd = static_cast<std::uint32_t>(value)
                      | (reg << MDIC_REGADD_SHIFT)
                      | MDIC_OP_WRITE;
    writeReg(MDIC, cmd);

    for (int i{}; i < 100; i++) {
      std::uint32_t mdic = readReg(MDIC);
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
    std::uint32_t cmd = (reg << MDIC_REGADD_SHIFT)
                      | MDIC_OP_READ;
    writeReg(MDIC, cmd);

    for (int i{}; i < 100; i++) {
      std::uint32_t mdic = readReg(MDIC);
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

    // 3. Advertise 10/100 Mbps modes
    writePhy(PHY_ANAR, ANAR_SELECTOR | ANAR_10_HD | ANAR_10_FD
                     | ANAR_100_HD | ANAR_100_FD);

    // 4. Advertise 1000 Mbps full duplex
    writePhy(PHY_GBCR, GBCR_1000_FD);

    // 5. Enable auto-negotiation and restart it
    writePhy(PHY_BMCR, BMCR_ANE | BMCR_RESTART_AN);

    std::fprintf(stderr, "[I210] PHY initialised: auto-negotiation restarted\n");
    return true;
  }

  void readMacAddress() {
    std::uint32_t ral = readReg(RAL0);
    std::uint32_t rah = readReg(RAH0);

    m_mac[0] = ral & 0xFF;
    m_mac[1] = (ral >> 8) & 0xFF;
    m_mac[2] = (ral >> 16) & 0xFF;
    m_mac[3] = (ral >> 24) & 0xFF;
    m_mac[4] = rah & 0xFF;
    m_mac[5] = (rah >> 8) & 0xFF;

    std::fprintf(stderr, "[I210] MAC: %02x:%02x:%02x:%02x:%02x:%02x \n",
        m_mac[0], m_mac[1], m_mac[2], m_mac[3], m_mac[4], m_mac[5]);
  }

  bool initRx() requires (HAS_RX){
    // 1. Allocate a region of memory for the receive descriptor list.
    if (!m_rxRing.allocate(NumRxDesc)) {
      std::fprintf(stderr, "[I210] Rx descriptor ring allocation failed.\n");
      return false;
    }

    // 2. Receive buffers of appropriate size should be allocated and pointers to these buffers should be stored in the descriptor ring.
    if (!m_rxBuffer.allocate(NumRxDesc * BuffSize)) {
      std::fprintf(stderr, "[I210] Rx data buffer allocation failed.\n");
      return false;
    }

    // 3. Program the descriptor base address with the address of the region.
    for (std::size_t i{}; i<NumRxDesc; i++) {
      m_rxRing[i].read.pktAddr = m_rxBuffer.physicalAddrAt(i * BuffSize);
      m_rxRing[i].read.hdrAddr = 0;  // single-buffer mode, no header split
    }

    // 4. Program descriptor base address (base addr before length — matches igb driver order)
    std::uint64_t rxBase = m_rxRing.physicalBase();
    writeReg(RDBAL0, static_cast<std::uint32_t>(rxBase & 0xFFFFFFFF));
    writeReg(RDBAH0, static_cast<std::uint32_t>(rxBase >> 32));

    // 4. Set the length register to the size of the descriptor ring.
    writeReg(RDLEN0, static_cast<std::uint32_t>(m_rxRing.sizeBytes()));

    // 5. Program SRRCTL of the queue according to the size of the buffers, the required header handling and the drop policy.
    constexpr std::uint32_t bSizeKB = BuffSize/1024;
    writeReg(SRRCTL0, (bSizeKB << SRRCTL_BSIZEPACKET_SHIFT) | SRRCTL_DESCTYPE_ADV_ONE | SRRCTL_DROP_EN);

    // TODO: 6. If header split or header replication is required for this queue, program the PSRTYPE register according to the required headers ???

    // 7. Enable global receiver first — igb driver sets RCTL.RXEN in igb_setup_rctl()
    //    BEFORE enabling individual queues. RXDCTL.ENABLE won't stick without this.
    writeReg(RCTL, RCTL_RXEN | RCTL_BAM | RCTL_SECRC | rctlBsize);

    // 7. Enable the queue by setting RXDCTL.ENABLE. In the case of queue zero, the enable bit is set by default - so the ring parameters should be set before RCTL.RXEN is set.
    //    igb driver sequence: disable queue, configure ring, then single write with ENABLE + thresholds
    writeReg(RXDCTL0, 0);
    writeReg(RDT0, 0);

    std::uint32_t rxdctl = (8U << 0)              // PTHRESH = 8
                         | (8U << 8)              // HTHRESH = 8
                         | (4U << 16)             // WTHRESH = 4
                         | RXDCTL_ENABLE;
    writeReg(RXDCTL0, rxdctl);

    // 8. Poll the RXDCTL register until the ENABLE bit is set. The tail should not be bumped before this bit was read as one.
    bool rxEnabled = false;
    for (int i{}; i < 100; i++) {
      if (readReg(RXDCTL0) & RXDCTL_ENABLE) {
        rxEnabled = true;
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (!rxEnabled) {
      std::fprintf(stderr, "[I210] RXDCTL0 enable timed out (RXDCTL0=0x%08x)\n",
                   readReg(RXDCTL0));
      return false;
    }

    // 9. Program the direction of packets to this queue according to the mode selected in the MRQC register. Packets directed to a disabled queue are dropped.
    // TODO:   

    // TODO: Is this needed ?
    m_rxTail = 0;
    writeReg(RDT0, static_cast<std::uint32_t>(NumRxDesc-1));

    std::fprintf(stderr, "[I210] Rx queue 0 initialised: %zu descriptors, %zu bytes buffers\n", NumRxDesc, BuffSize);
    return true;
  }

  bool initTx() requires (HAS_TX){
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
    std::uint64_t txBase = m_txRing.physicalBase();
    writeReg(TDBAL0, static_cast<std::uint32_t>(txBase & 0xFFFFFFFF));
    writeReg(TDBAH0, static_cast<std::uint32_t>(txBase >> 32));

    // 4. Program ring length bytes
    writeReg(TDLEN0, static_cast<std::uint32_t>(m_txRing.sizeBytes()));

    // 5. Program TXDCTL : WTHRESH=1 for immediate write-back then enable
    writeReg(TXDCTL0, (1U << TXDCTL_WTHRESH_SHIFT));
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

    // 6. Set the TIPG to default values for 1000BASE-T
    writeReg(TIPG, TIPG_DEFAULT);

    // 7. Enable transmitter
    writeReg(TCTL, TCTL_EN | TCTL_PSP | TCTL_CT_IEEE | TCTL_BST_DEF);

    m_txTail = 0;
    m_txCleanHead = 0;
    m_txInFlight = 0;

    std::fprintf(stderr, "[I210] Tx queue 0 initialised: %zu descriptors, %zu byte buffers.\n",
      NumTxDesc, BuffSize);
    return true;
  }

  [[gnu::always_inline, gnu::hot]]
  inline void txReclaim() requires (HAS_TX){
    while (m_txInFlight > 0) {
      TxDescriptor& desc = m_txRing[m_txCleanHead];
      if (!(desc.wb.status & TXD_STAT_DD)) {
        break;
      }

      desc.wb.status = 0;
      m_txCleanHead = (m_txCleanHead + 1) & TX_RING_MASK;
      m_txInFlight--;
    }
  }
};



#endif //ABTEDGE_INTEL_I210_H
