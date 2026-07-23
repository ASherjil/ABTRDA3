// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Areeb Sherjil

//
// Intel I210 Ethernet Controller Register Map
// Reference: Intel I210 Datasheet, Document 333016, Revision 3.7
// Device ID: 0x1533 (I210 Gigabit Network Connection - Copper)
//
// All offsets are relative to BAR0 (Memory-Mapped I/O base address).
// Only Queue 0 registers are defined — queues 1-3 are at +0x40*(n) from these base offsets.
//

#ifndef ABTEDGE_I210REGISTERS_HPP
#define ABTEDGE_I210REGISTERS_HPP

#include <cstdint>

// ============================================================================
// PCI Identity
// ============================================================================
constexpr std::uint16_t VENDOR_ID  = 0x8086;   // Intel Corporation
constexpr std::uint16_t DEVICE_ID  = 0x1533;   // I210 Gigabit Network Connection (Copper)

// ============================================================================
// Section 8.2 — General Registers
// ============================================================================
constexpr std::uint32_t CTRL       = 0x0000;   // Device Control (R/W)
constexpr std::uint32_t STATUS     = 0x0008;   // Device Status (RO)
constexpr std::uint32_t CTRL_EXT   = 0x0018;   // Extended Device Control (R/W)
constexpr std::uint32_t MDIC       = 0x0020;   // MDI Control (R/W) — PHY register access
constexpr std::uint32_t WUC        = 0x0030;   // Wake Up Control (R/W)
constexpr std::uint32_t CONNSW     = 0x0034;   // Copper/Fiber Switch Control (R/W)
constexpr std::uint32_t VET        = 0x0038;   // VLAN Ether Type (R/W)
constexpr std::uint32_t LEDCTL     = 0x0E00;   // LED Control (R/W)

// ============================================================================
// Section 8.2.1 — CTRL Register Bit Definitions
// ============================================================================
constexpr std::uint32_t CTRL_FD         = (1U << 0);    // Full Duplex
constexpr std::uint32_t CTRL_GIO_MD     = (1U << 2);    // GIO Master Disable
constexpr std::uint32_t CTRL_SLU        = (1U << 6);    // Set Link Up
constexpr std::uint32_t CTRL_FRCSPD     = (1U << 11);   // Force Speed
constexpr std::uint32_t CTRL_FRCDPLX    = (1U << 12);   // Force Duplex
constexpr std::uint32_t CTRL_RST        = (1U << 26);   // Software Reset (self-clearing)
constexpr std::uint32_t CTRL_RFCE       = (1U << 27);   // Receive Flow Control Enable
constexpr std::uint32_t CTRL_TFCE       = (1U << 28);   // Transmit Flow Control Enable
constexpr std::uint32_t CTRL_DEV_RST    = (1U << 29);   // Device Reset (self-clearing)
constexpr std::uint32_t CTRL_VME        = (1U << 30);   // VLAN Mode Enable
constexpr std::uint32_t CTRL_PHY_RST    = (1U << 31);   // PHY Reset

// ============================================================================
// Section 8.2.2 — STATUS Register Bit Definitions
// ============================================================================
constexpr std::uint32_t STATUS_FD       = (1U << 0);    // Full Duplex indication
constexpr std::uint32_t STATUS_LU       = (1U << 1);    // Link Up indication
constexpr std::uint32_t STATUS_TXOFF    = (1U << 4);    // Transmission Paused
constexpr std::uint32_t STATUS_SPEED_SHIFT = 6;
constexpr std::uint32_t STATUS_SPEED_MASK  = (0x3U << STATUS_SPEED_SHIFT); // bits [7:6]
// 00b = 10 Mb/s, 01b = 100 Mb/s, 10b = 1000 Mb/s, 11b = 1000 Mb/s

// ============================================================================
// Section 8.8 — Interrupt Registers
// We disable all interrupts since we use busy-polling for lowest latency.
// ============================================================================
constexpr std::uint32_t EICR       = 0x1580;   // Extended Interrupt Cause Read (RC/W1C)
constexpr std::uint32_t EICS       = 0x1520;   // Extended Interrupt Cause Set (WO)
constexpr std::uint32_t EIMS       = 0x1524;   // Extended Interrupt Mask Set/Read (RWM)
constexpr std::uint32_t EIMC       = 0x1528;   // Extended Interrupt Mask Clear (WO)
constexpr std::uint32_t EIAC       = 0x152C;   // Extended Interrupt Auto Clear (R/W)
constexpr std::uint32_t EIAM       = 0x1530;   // Extended Interrupt Auto Mask Enable (R/W)
constexpr std::uint32_t ICR        = 0x1500;   // Interrupt Cause Read (RC/W1C)
constexpr std::uint32_t ICS        = 0x1504;   // Interrupt Cause Set (WO)
constexpr std::uint32_t IMS        = 0x1508;   // Interrupt Mask Set/Read (R/W)
constexpr std::uint32_t IMC        = 0x150C;   // Interrupt Mask Clear (WO)
constexpr std::uint32_t IAM        = 0x1510;   // Interrupt Acknowledge Auto-mask (R/W)
constexpr std::uint32_t GPIE       = 0x1514;   // General Purpose Interrupt Enable (RW)
constexpr std::uint32_t EITR0      = 0x1680;   // Interrupt Throttle Register 0 (R/W)
// EITR[n] = 0x1680 + 4*n, n = 0..4
constexpr std::uint32_t IVAR0      = 0x1700;   // Interrupt Vector Allocation 0 (RW)
constexpr std::uint32_t IVAR_MISC  = 0x1740;   // Interrupt Vector Allocation Misc (RW)

// ============================================================================
// Section 8.3 — Internal Packet Buffer Size Registers
// ============================================================================
constexpr std::uint32_t RXPBSIZE   = 0x2404;   // Rx Packet Buffer Size (R/W)
constexpr std::uint32_t TXPBSIZE   = 0x3404;   // Tx Packet Buffer Size (R/W)

// ============================================================================
// Section 8.10 — Receive Registers (Queue 0)
// ============================================================================

// -- Receive Control --
constexpr std::uint32_t RCTL       = 0x0100;   // Receive Control Register (R/W)

// RCTL Bit Definitions (Section 8.10.1)
constexpr std::uint32_t RCTL_RXEN      = (1U << 1);    // Receiver Enable
constexpr std::uint32_t RCTL_SBP       = (1U << 2);    // Store Bad Packets
constexpr std::uint32_t RCTL_UPE       = (1U << 3);    // Unicast Promiscuous Enable
constexpr std::uint32_t RCTL_MPE       = (1U << 4);    // Multicast Promiscuous Enable
constexpr std::uint32_t RCTL_LPE       = (1U << 5);    // Long Packet Reception Enable
constexpr std::uint32_t RCTL_BAM       = (1U << 15);   // Broadcast Accept Mode
constexpr std::uint32_t RCTL_BSIZE_2048 = (0U << 16);  // Buffer Size 2048 bytes (default)
constexpr std::uint32_t RCTL_BSIZE_1024 = (1U << 16);  // Buffer Size 1024 bytes
constexpr std::uint32_t RCTL_BSIZE_512  = (2U << 16);  // Buffer Size 512 bytes
constexpr std::uint32_t RCTL_BSIZE_256  = (3U << 16);  // Buffer Size 256 bytes
constexpr std::uint32_t RCTL_VFE       = (1U << 18);   // VLAN Filter Enable
constexpr std::uint32_t RCTL_DPF       = (1U << 22);   // Discard Pause Frames
constexpr std::uint32_t RCTL_SECRC     = (1U << 26);   // Strip Ethernet CRC from incoming packet

// -- Rx Descriptor Ring Registers (Queue 0) --
constexpr std::uint32_t RDBAL0     = 0xC000;   // Rx Descriptor Base Address Low (R/W)
constexpr std::uint32_t RDBAH0     = 0xC004;   // Rx Descriptor Base Address High (R/W)
constexpr std::uint32_t RDLEN0     = 0xC008;   // Rx Descriptor Ring Length in bytes (R/W) — must be 128-byte aligned
constexpr std::uint32_t SRRCTL0    = 0xC00C;   // Split and Replication Receive Control (R/W)
constexpr std::uint32_t RDH0       = 0xC010;   // Rx Descriptor Head (RO)
constexpr std::uint32_t RDT0       = 0xC018;   // Rx Descriptor Tail (R/W)
constexpr std::uint32_t RXDCTL0    = 0xC028;   // Rx Descriptor Control (R/W)
constexpr std::uint32_t RQDPC0     = 0xC030;   // Rx Queue Drop Packet Count (RW)

// RXDCTL Bit Definitions (Section 8.10.9)
constexpr std::uint32_t RXDCTL_ENABLE  = (1U << 25);   // Receive Queue Enable
constexpr std::uint32_t RXDCTL_SWFLUSH = (1U << 26);   // Receive Software Flush

// SRRCTL Bit Definitions (Section 8.10.2)
constexpr std::uint32_t SRRCTL_BSIZEPACKET_SHIFT = 0;  // Buffer size in 1KB units (bits [6:0])
constexpr std::uint32_t SRRCTL_DESCTYPE_SHIFT    = 25; // Descriptor type (bits [27:25])
constexpr std::uint32_t SRRCTL_DESCTYPE_LEGACY   = (0U << 25);  // 000b = Legacy
constexpr std::uint32_t SRRCTL_DESCTYPE_ADV_ONE  = (1U << 25);  // 001b = Advanced, one buffer
constexpr std::uint32_t SRRCTL_DROP_EN           = (1U << 31);  // Drop Enable

// -- Receive Filtering --
constexpr std::uint32_t RXCSUM     = 0x5000;   // Receive Checksum Control (R/W)
constexpr std::uint32_t RLPML      = 0x5004;   // Receive Long Packet Max Length (R/W)
constexpr std::uint32_t RFCTL      = 0x5008;   // Receive Filter Control (R/W)
constexpr std::uint32_t MTA        = 0x5200;   // Multicast Table Array (R/W) — 128 entries, +4*n
constexpr std::uint32_t RAL0       = 0x5400;   // Receive Address Low 0 (R/W) — MAC address [31:0]
constexpr std::uint32_t RAH0       = 0x5404;   // Receive Address High 0 (R/W) — MAC address [47:32]
// RAL[n] = 0x5400 + 8*n, RAH[n] = 0x5404 + 8*n, n = 0..15

// RAH Bit Definitions (Section 8.10.17)
constexpr std::uint32_t RAH_AV         = (1U << 31);   // Address Valid
constexpr std::uint32_t RAH_ASEL_DEST  = (0U << 16);   // 00b = Destination address (normal)

constexpr std::uint32_t MRQC       = 0x5818;   // Multiple Receive Queues Command (R/W)

// ============================================================================
// Section 8.12 — Transmit Registers (Queue 0)
// ============================================================================

// -- Transmit Control --
constexpr std::uint32_t TCTL       = 0x0400;   // Transmit Control Register (R/W)
constexpr std::uint32_t TCTL_EXT   = 0x0404;   // Transmit Control Extended (R/W)
constexpr std::uint32_t TIPG       = 0x0410;   // Transmit IPG Register (R/W)

// TCTL Bit Definitions (Section 8.12.1)
constexpr std::uint32_t TCTL_EN        = (1U << 1);    // Transmit Enable
constexpr std::uint32_t TCTL_PSP       = (1U << 3);    // Pad Short Packets (to 64 bytes)
constexpr std::uint32_t TCTL_CT_SHIFT  = 4;            // Collision Threshold (bits [11:4])
constexpr std::uint32_t TCTL_CT_IEEE   = (0xFU << 4);  // IEEE 802.3 value = 15
constexpr std::uint32_t TCTL_BST_SHIFT = 12;           // Back-Off Slot Time (bits [21:12])
constexpr std::uint32_t TCTL_BST_DEF   = (0x40U << 12);// Default = 64 byte times
constexpr std::uint32_t TCTL_RTLC      = (1U << 24);   // Re-transmit on Late Collision

// -- Tx Descriptor Ring Registers (Queue 0) --
constexpr std::uint32_t TDBAL0     = 0xE000;   // Tx Descriptor Base Address Low (R/W)
constexpr std::uint32_t TDBAH0     = 0xE004;   // Tx Descriptor Base Address High (R/W)
constexpr std::uint32_t TDLEN0     = 0xE008;   // Tx Descriptor Ring Length in bytes (R/W) — must be 128-byte aligned
constexpr std::uint32_t TDH0       = 0xE010;   // Tx Descriptor Head (RO)
constexpr std::uint32_t TDT0       = 0xE018;   // Tx Descriptor Tail (R/W)
constexpr std::uint32_t TXDCTL0    = 0xE028;   // Tx Descriptor Control (R/W)
constexpr std::uint32_t TDWBAL0    = 0xE038;   // Tx Descriptor Completion Write-Back Address Low (R/W)
constexpr std::uint32_t TDWBAH0    = 0xE03C;   // Tx Descriptor Completion Write-Back Address High (R/W)

// TXDCTL Bit Definitions (Section 8.12.15)
constexpr std::uint32_t TXDCTL_PTHRESH_SHIFT  = 0;     // Prefetch Threshold (bits [4:0])
constexpr std::uint32_t TXDCTL_HTHRESH_SHIFT  = 8;     // Host Threshold (bits [12:8])
constexpr std::uint32_t TXDCTL_WTHRESH_SHIFT  = 16;    // Write-Back Threshold (bits [20:16])
constexpr std::uint32_t TXDCTL_ENABLE         = (1U << 25);  // Transmit Queue Enable
constexpr std::uint32_t TXDCTL_SWFLSH         = (1U << 26);  // Transmit Software Flush
constexpr std::uint32_t TXDCTL_PRIORITY       = (1U << 27);  // Transmit Queue Priority

// -- Transmit DMA --
constexpr std::uint32_t DTXCTL     = 0x3590;   // DMA Tx Control (R/W)
constexpr std::uint32_t DTXMXPKTSZ = 0x355C;   // DMA Tx Maximum Packet Size (RW)
constexpr std::uint32_t DTXMXSZRQ  = 0x3540;   // DMA Tx Max Outstanding Requests (RW)
constexpr std::uint32_t RETX_CTL   = 0x041C;   // Retry Buffer Control (RW)
constexpr std::uint32_t TQAVCTRL   = 0x3570;   // Tx Qav Control (R/W) — TransmitMode bit 0

// ============================================================================
// Section 8.7 — Semaphore Registers
// ============================================================================
constexpr std::uint32_t SWSM       = 0x5B50;   // Software Semaphore (R/W)
constexpr std::uint32_t FWSM       = 0x5B54;   // Firmware Semaphore (RO to Host)
constexpr std::uint32_t SW_FW_SYNC = 0x5B5C;   // Software-Firmware Synchronization (RWM)

// ============================================================================
// Section 8.6 — PCIe Registers
// ============================================================================
constexpr std::uint32_t GCR        = 0x5B00;   // PCIe Control (RW)
constexpr std::uint32_t GCR_EXT    = 0x5B6C;   // PCIe Control Extended (RW)

// ============================================================================
// Section 8.4 — EEPROM/Flash Registers
// ============================================================================
constexpr std::uint32_t EEC        = 0x12010;  // EEPROM-Mode Control (RW)
constexpr std::uint32_t EERD       = 0x12014;  // EEPROM-Mode Read (RW)

// ============================================================================
// Advanced Rx Descriptor (16 bytes) — Section 7.1.6 / igb: e1000_adv_rx_desc
//
// Read format (software writes before giving to hardware):
//   [63:0]   pktAddr  — physical address where NIC will DMA the packet
//   [127:64] hdrAddr  — header buffer address (0 for single-buffer mode)
//
// Write-back format (hardware fills after packet arrives):
//   [31:0]   rss          — RSS hash value
//   [63:32]  pktInfo      — packet type, header info
//   [95:64]  statusError  — extended status/error (DD = bit 0)
//   [111:96] length       — packet length in bytes
//   [127:112] vlan        — VLAN tag
// ============================================================================
union RxDescriptor {
    // Read format — software fills before giving descriptor to hardware
    struct {
        std::uint64_t pktAddr;       // Physical address of the packet buffer
        std::uint64_t hdrAddr;       // Header buffer address (0 for one-buffer mode)
    } read;
    // Write-back format — hardware fills after packet arrives
    struct {
        std::uint32_t rss;           // RSS hash value
        std::uint32_t pktInfo;       // Packet type, header info
        std::uint32_t statusError;   // Extended status/error (DD = bit 0)
        std::uint16_t length;        // Packet length in bytes
        std::uint16_t vlan;          // VLAN tag
    } wb;
};
static_assert(sizeof(RxDescriptor) == 16, "RxDescriptor must be exactly 16 bytes");

// Advanced RxDescriptor statusError bit definitions (Section 7.1.6.2)
constexpr std::uint32_t RXD_STAT_DD     = (1U << 0);    // Descriptor Done
constexpr std::uint32_t RXD_STAT_EOP    = (1U << 1);    // End of Packet
constexpr std::uint32_t RXD_STAT_VP     = (1U << 3);    // VLAN Packet (802.1Q match)
constexpr std::uint32_t RXDADV_STAT_TS  = (1U << 16);   // Packet was timestamped

// ============================================================================
// Advanced Tx Descriptor (16 bytes) — Section 7.2.3 / igb: e1000_adv_tx_desc
//
// Read format (software writes before giving to hardware):
//   [63:0]    bufferAddr    — physical address of packet data in host memory
//   [95:64]   cmdTypeLen    — command, type, and length combined
//   [127:96]  olinfoStatus  — offload info and status
//
// Write-back format (hardware fills after transmit completes):
//   [63:0]    reserved
//   [95:64]   nxtseqSeed
//   [127:96]  status        — DD (bit 0) = Descriptor Done
// ============================================================================
union TxDescriptor {
    // Read format — software fills before giving descriptor to hardware
    struct {
        std::uint64_t bufferAddr;     // Physical address of the transmit data
        std::uint32_t cmdTypeLen;     // Command, descriptor type, and data length
        std::uint32_t olinfoStatus;   // Offload info (paylen, popts) and status
    } read;
    // Write-back format — hardware fills after transmit completes
    struct {
        std::uint64_t reserved;
        std::uint32_t nxtseqSeed;
        std::uint32_t status;         // DD = bit 0
    } wb;
};
static_assert(sizeof(TxDescriptor) == 16, "TxDescriptor must be exactly 16 bytes");

// Advanced TxDescriptor cmdTypeLen bit definitions (Section 7.2.3 / igb: e1000_82575.h)
constexpr std::uint32_t TXD_CMD_EOP     = 0x01000000;   // End of Packet
constexpr std::uint32_t TXD_CMD_IFCS    = 0x02000000;   // Insert FCS (CRC)
constexpr std::uint32_t TXD_CMD_RS      = 0x08000000;   // Report Status (sets DD on completion)
constexpr std::uint32_t TXD_CMD_DEXT    = 0x20000000;   // Descriptor Extension (1 = advanced)
constexpr std::uint32_t TXD_DTYP_DATA   = 0x00300000;   // Advanced Data Descriptor type
constexpr std::uint32_t TXD_PAYLEN_SHIFT = 14;           // Payload length shift in olinfoStatus

// Advanced TxDescriptor write-back status bit definitions
constexpr std::uint32_t TXD_STAT_DD     = (1U << 0);    // Descriptor Done

// ============================================================================
// Typical TIPG values for 1000BASE-T (Section 8.12.3)
// IPGT = 8, IPGR1 = 8 (2/3 of 12), IPGR = 6
// ============================================================================
constexpr std::uint32_t TIPG_DEFAULT = (8U << 0) | (8U << 10) | (6U << 20);

// ============================================================================
// Convenience: disable all interrupts
// Write 0xFFFFFFFF to IMC and EIMC to mask everything
// ============================================================================
constexpr std::uint32_t IRQ_DISABLE_ALL = 0xFFFFFFFF;

// ============================================================================
// Section 8.2.4 — MDIC Register (0x0020; R/W)
//
// Software reads/writes internal PHY registers through this register.
// No PHY address field — internal vs external PHY is selected via MDICNFG
// (Section 8.2.5), which defaults to internal PHY (Destination=0).
//
// Usage: write command (opcode + reg addr + data), poll MDIC_READY.
// ============================================================================
constexpr std::uint32_t MDIC_DATA_MASK     = 0xFFFF;        // Bits [15:0]  — read/write data
constexpr std::uint32_t MDIC_REGADD_SHIFT  = 16;            // Bits [20:16] — PHY register address
constexpr std::uint32_t MDIC_OP_WRITE      = (1U << 26);    // Opcode 01b — MDI Write
constexpr std::uint32_t MDIC_OP_READ       = (2U << 26);    // Opcode 10b — MDI Read
constexpr std::uint32_t MDIC_READY         = (1U << 28);    // Ready — set by HW on completion
constexpr std::uint32_t MDIC_ERROR         = (1U << 30);    // Error — MDI transaction failed

// ============================================================================
// Standard MII PHY Registers (IEEE 802.3 clause 22)
//
// Accessed via MDIC. The I210 internal copper PHY implements this standard
// register set. Register addresses 0-31.
// ============================================================================
constexpr std::uint32_t PHY_BMCR = 0;    // Basic Mode Control Register
constexpr std::uint32_t PHY_BMSR = 1;    // Basic Mode Status Register
constexpr std::uint32_t PHY_ANAR = 4;    // Auto-Negotiation Advertisement Register
constexpr std::uint32_t PHY_GBCR = 9;    // 1000BASE-T Control Register

// BMCR bit definitions (PHY register 0)
constexpr std::uint16_t BMCR_RESET      = (1U << 15);  // PHY Reset (self-clearing)
constexpr std::uint16_t BMCR_ANE        = (1U << 12);  // Auto-Negotiation Enable
constexpr std::uint16_t BMCR_RESTART_AN = (1U << 9);   // Restart Auto-Negotiation

// ANAR bit definitions (PHY register 4) — advertise supported link modes
constexpr std::uint16_t ANAR_10_HD      = (1U << 5);   // 10BASE-T Half Duplex
constexpr std::uint16_t ANAR_10_FD      = (1U << 6);   // 10BASE-T Full Duplex
constexpr std::uint16_t ANAR_100_HD     = (1U << 7);   // 100BASE-TX Half Duplex
constexpr std::uint16_t ANAR_100_FD     = (1U << 8);   // 100BASE-TX Full Duplex
constexpr std::uint16_t ANAR_SELECTOR   = 0x0001;      // IEEE 802.3 selector field

// GBCR bit definitions (PHY register 9) — gigabit advertisement
constexpr std::uint16_t GBCR_1000_FD    = (1U << 9);   // Advertise 1000BASE-T Full Duplex

#endif //ABTEDGE_I210REGISTERS_HPP
