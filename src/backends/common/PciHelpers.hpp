// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Areeb Sherjil

//
// PciHelpers — generic PCI / sysfs lookups shared across transports.
//
// resolveBdf(): map a kernel network interface name (e.g. "enp1s0f1np1") to its
// PCI domain:bus:device.function id (e.g. "0000:01:00.1"). The DPDK transport
// addresses devices by BDF, so it resolves the name WHILE the netdev still
// exists — before the device is unbound from the kernel driver onto vfio-pci,
// which removes the interface name.
//
// (Intel_I210 does NOT use this: it reaches the PCIe bus through the ABTEdge
// backend, which handles bus access for it.)
//
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>

#include <fcntl.h>
#include <unistd.h>

#include <fmt/core.h>

#include "BackendBase.hpp"   // ABTEdge — sysfs-resource0 MMIO (for the i40e ITR read)

namespace pci {

// basename of a sysfs symlink target (".../0000:01:00.1" -> "0000:01:00.1").
// Empty if the path is not a symlink / does not exist.
[[nodiscard]] inline std::string readlinkBasename(const std::string& path) noexcept {
    char          buf[256];
    const ssize_t n = ::readlink(path.c_str(), buf, sizeof(buf) - 1);
    if (n <= 0) {
        return {};
    }
    buf[n] = '\0';
    const std::string s(buf);
    const auto        pos = s.find_last_of('/');
    return pos == std::string::npos ? s : s.substr(pos + 1);
}

// Interface name -> PCI BDF, via /sys/class/net/<if>/device. Empty if the netdev
// doesn't exist (e.g. already bound to vfio-pci) or isn't backed by a PCI device.
[[nodiscard]] inline std::string resolveBdf(std::string_view ifname) noexcept {
    return readlinkBasename("/sys/class/net/" + std::string(ifname) + "/device");
}

// The driver currently bound to a PCI device (basename of its driver symlink),
// or empty if the device is bound to no driver.
[[nodiscard]] inline std::string currentDriver(std::string_view bdf) noexcept {
    return readlinkBasename("/sys/bus/pci/devices/" + std::string(bdf) + "/driver");
}

// Size of a PCI BAR (in bytes) for the given device, read from
// /sys/bus/pci/devices/<bdf>/resource. The file has one line per BAR:
//   0xSTART 0xEND 0xFLAGS
// Size = END - START + 1, or 0 if the BAR is unused. Returns 0 on any error.
[[nodiscard]] inline std::size_t barSize(std::string_view bdf, int barIndex) noexcept {
    const std::string path = "/sys/bus/pci/devices/" + std::string(bdf) + "/resource";
    std::FILE*        f    = std::fopen(path.c_str(), "r");
    if (!f) {
        return 0;
    }
    char line[160]{};
    for (int i = 0; i <= barIndex; ++i) {
        if (!std::fgets(line, sizeof(line), f)) {
            std::fclose(f);
            return 0;
        }
    }
    std::fclose(f);
    unsigned long long start = 0;
    unsigned long long end   = 0;
    if (std::sscanf(line, "%llx %llx", &start, &end) == 2 && end >= start && start != 0) {
        return static_cast<std::size_t>(end - start + 1ULL);
    }
    return 0;
}

// Derive a PCI BDF from a systemd "predictable" interface name
// (enp<bus>s<dev>[f<func>]), which is itself assigned from the firmware/BIOS PCI
// location. This resolves the BDF even AFTER the kernel driver is unbound (the
// netdev is gone, so resolveBdf() can't help). The numbers in the name are
// DECIMAL (e.g. 0000:3b:00.0 -> enp59s0); we convert to the hex BDF and verify a
// matching PCI device exists. Empty if the name isn't the enp form or no device
// matches.
[[nodiscard]] inline std::string bdfFromName(std::string_view ifname) noexcept {
    if (ifname.size() < 5 || ifname[0] != 'e' || ifname[1] != 'n' || ifname[2] != 'p') {
        return {};
    }
    std::size_t i       = 3;
    auto        readDec = [&](unsigned& out) -> bool {
        const std::size_t start = i;
        out                     = 0;
        while (i < ifname.size() && ifname[i] >= '0' && ifname[i] <= '9') {
            out = out * 10u + static_cast<unsigned>(ifname[i++] - '0');
        }
        return i > start;
    };
    unsigned bus  = 0;
    unsigned dev  = 0;
    unsigned func = 0;
    if (!readDec(bus)) {
        return {};
    }
    if (i >= ifname.size() || ifname[i] != 's') {
        return {};
    }
    ++i;
    if (!readDec(dev)) {
        return {};
    }
    if (i < ifname.size() && ifname[i] == 'f') {
        ++i;
        if (!readDec(func)) {
            return {};
        }
    }

    char bdf[16];
    std::snprintf(bdf, sizeof(bdf), "0000:%02x:%02x.%x", bus & 0xffu, dev & 0xffu, func & 0xfu);
    if (::access((std::string("/sys/bus/pci/devices/") + bdf).c_str(), F_OK) != 0) {
        return {};
    }
    return bdf;
}

// ── i40e Interrupt Throttle Rate verification ────────────────────────────────
//
// The XXV710/i40e honors the per-vector ITR even in pure polling mode: ITR
// governs when completed RX descriptors are DMA'd back to host RAM, so a nonzero
// ITR adds latency no matter how hard userspace busy-polls. The DPDK transport
// zeroes it with a BAR0 write; `ethtool -C rx-usecs 0` (the AF_XDP / packet_mmap
// path) is supposed to do the same but was NEVER verified in-register. reportItr()
// READS the two ITR registers via a second sysfs-resource0 mmap (BackendBase) so
// we can confirm, mid-run, that the throttle is genuinely off — on EITHER stack:
// kernel-driver (AF_XDP/packet_mmap, netdev present -> resolveBdf) OR vfio-pci
// (DPDK, netdev gone -> bdfFromName). Read-only (opens O_RDWR because resource0
// offers no RO mode, but only LOADS). Root required (resource0 is 0600 root).

// Register offsets (drivers/net/intel/i40e/base/i40e_register.h):
//   I40E_PFINT_ITR0(_i)         = 0x00038000 + (_i)*128       -> ITR0(0)
//   I40E_PFINT_ITRN(_i, _INTPF) = 0x00030000 + (_i)*2048 + (_INTPF)*4 -> ITRN(0,0)
// _i = ITR index (0 = RX). The INTERVAL field is bits [11:0] in 2us units; 0 ==
// no throttling. Other bits reserved/zero, so a raw read of 0 also means 0us.
//
// WHICH register governs OUR queue-0 RX depends on the stack, because the two
// drivers wire queue 0 to different MSI-X vectors (confirmed via /proc/interrupts:
// vector 0 = "...:misc" admin/link, vector 1 = "...-TxRx-0" = our data queue):
//   * KERNEL i40e (AF_XDP / packet_mmap): queue 0 -> data vector 1 -> ITRN(0,0).
//     ITR0(0) is the MISC vector's RX ITR and does NOT touch packet RX, so it is
//     routinely left at the driver default (~122us) — harmless. Watch ITRN.
//   * DPDK poll-mode PMD: queue 0 -> the "zero" vector -> ITR0(0). (DPDK.hpp
//     writes BOTH to be safe; the 30->9us win came from ITR0.)
// So the verdict below keys on ITRN for the kernel path and reports ITR0 as
// informational. A NONZERO ITRN is the real "RX throttle still active" signal.
inline constexpr std::size_t   I40E_PFINT_ITR0_0      = 0x00038000;
inline constexpr std::size_t   I40E_PFINT_ITRN_0_0    = 0x00030000;
inline constexpr std::uint32_t I40E_ITR_INTERVAL_MASK = 0xFFF;

// True if the device backing `ifname` is currently bound to the kernel i40e
// driver (the AF_XDP / packet_mmap path). False for vfio-pci (DPDK) or non-i40e.
[[nodiscard]] inline bool boundToI40e(std::string_view ifname) noexcept {
    const std::string bdf = resolveBdf(ifname);
    return !bdf.empty() && currentDriver(bdf) == "i40e";
}

// Read and print the live ITR registers for the NIC backing `ifname`. `tag`
// labels the log line (e.g. "DPDK-ITR" / "AFXDP-ITR"). Returns true iff the
// data-queue RX throttle (ITRN(0,0)) is 0 — the one that actually gates our RX
// on the kernel path. ITR0(0) is printed for context (misc vector on the kernel
// path; the data vector under DPDK). Best-effort: logs + returns false on any
// resolve/mmap failure (never throws, never aborts).
// BDF-explicit overload: once vfio removes the netdev, a RENAMED port ("xxv0", "cx1") cannot
// be re-derived from its name — only enp<bus>s<dev>f<func> can. Callers that already hold the
// BDF (dispatch resolves it pre-unbind) must pass it, or this silently reports nothing.
inline bool reportItr(const std::string& bdf, std::string_view ifname, const char* tag) {
    if (bdf.empty()) {
        fmt::print(stderr, "[{}] {}: cannot resolve PCI BDF\n", tag, ifname);
        return false;
    }

    const std::size_t bar0Size = barSize(bdf, 0);
    const std::string resPath  = "/sys/bus/pci/devices/" + bdf + "/resource0";
    BackendBase       bar0;
    if (bar0Size == 0 || !bar0.open(resPath.c_str(), 0, bar0Size)) {
        fmt::print(stderr, "[{}] {} ({}): resource0 mmap failed (need root?)\n", tag, ifname, bdf);
        return false;
    }

    const std::uint32_t itr0          = *bar0.registerPtr<std::uint32_t>(I40E_PFINT_ITR0_0);
    const std::uint32_t itrn          = *bar0.registerPtr<std::uint32_t>(I40E_PFINT_ITRN_0_0);
    const std::uint32_t itr0Int       = itr0 & I40E_ITR_INTERVAL_MASK;
    const std::uint32_t itrnInt       = itrn & I40E_ITR_INTERVAL_MASK;
    const bool          rxThrottleOff = (itrnInt == 0);   // ITRN = our data-queue RX ITR

    fmt::print(stderr,
               "[{}] {} ({}): PFINT_ITRN(data-RX)=0x{:08x} ({}us) PFINT_ITR0(misc)=0x{:08x} ({}us) "
               "=> RX path {}\n",
               tag, ifname, bdf, itrn, itrnInt * 2u, itr0, itr0Int * 2u,
               rxThrottleOff ? "UNTHROTTLED (ITRN=0)" : "THROTTLED (ITRN nonzero!)");
    return rxThrottleOff;
}

// Name-only convenience for the KERNEL paths (AF_XDP / packet_mmap), where the netdev still
// exists so the BDF is resolvable. Do NOT use it after a vfio unbind.
inline bool reportItr(std::string_view ifname, const char* tag) {
    std::string bdf = resolveBdf(ifname);   // kernel driver: netdev present
    if (bdf.empty()) {
        bdf = bdfFromName(ifname);   // enp<bus>s<dev>f<func> only
    }
    return reportItr(bdf, ifname, tag);
}

// ── Driver-specific BAR0 fixes (applied by the DISPATCH layer) ───────────────
//
// The DPDK transport is PMD-agnostic — every driver-specific register poke lives
// here, and TransportDispatch (which knows the TOML driver name) applies it
// AFTER the port is started (a device reset during start would wipe the write).
// The BDF must be resolved from the port name BEFORE a vfio unbind removes the
// netdev (renamed interfaces like "xxv0" cannot be re-derived afterwards).
// All best-effort: log + return false, never abort.

// i40e: zero the per-vector Interrupt Throttle Rate. The XXV710 silicon honors
// ITR even in pure polling mode — it gates when completed RX descriptors are
// written back to host RAM. DPDK 25.11 leaves the firmware default and exposes
// NO public API to change it (no devarg, nothing in rte_pmd_i40e.h). Proof it is
// the lever (2026-05-26, packet_mmap A/B with ethtool -C): rx-usecs 50 -> median
// 49.81us; rx-usecs 0 -> 11.88us. Under the DPDK PMD, queue 0 rides the "zero"
// vector -> ITR0(0); on the kernel path (DPDK-over-AF_XDP) it is ITRN(0,0). Both
// are written — harmless — and both are read back to verify (the kernel path's
// adaptive ITR could re-arm). Without this the i40e RTT is ~5x slower
// (ITR-gated ~50us vs ~10us).
inline bool i40eClearItr(const std::string& bdf, std::string_view ifname) noexcept {
    if (bdf.empty()) {
        fmt::print(stderr, "[PCI] {}: no BDF — i40e ITR not cleared, expect ~30us+ median RTT\n", ifname);
        return false;
    }
    const std::size_t bar0Size = barSize(bdf, 0);
    const std::string resPath  = "/sys/bus/pci/devices/" + bdf + "/resource0";
    BackendBase       bar0;
    if (bar0Size == 0 || !bar0.open(resPath.c_str(), 0, bar0Size)) {
        fmt::print(stderr,
                   "[PCI] {} ({}): resource0 mmap failed — i40e ITR not cleared, "
                   "expect ~30us+ median RTT\n",
                   ifname, bdf);
        return false;
    }
    *bar0.registerPtr<std::uint32_t>(I40E_PFINT_ITR0_0)   = 0;
    *bar0.registerPtr<std::uint32_t>(I40E_PFINT_ITRN_0_0) = 0;
    const std::uint32_t itr0 = *bar0.registerPtr<std::uint32_t>(I40E_PFINT_ITR0_0);
    const std::uint32_t itrn = *bar0.registerPtr<std::uint32_t>(I40E_PFINT_ITRN_0_0);
    const bool          off  = ((itr0 | itrn) & I40E_ITR_INTERVAL_MASK) == 0;
    fmt::print(stderr,
               "[PCI] {} ({}): i40e ITR cleared (ITR0=0x{:08x} ITRN=0x{:08x}) "
               "=> RX write-back {}\n",
               ifname, bdf, itr0, itrn, off ? "UNTHROTTLED" : "STILL THROTTLED!");
    return off;
}

// i40e: switch the bound RX queues to NoITR (QINT_RQCTL.ITR_INDX = 3).
//
// WHY THIS IS NOT THE SAME AS i40eClearItr(). Zeroing PFINT_ITR0/ITRN sets the moderation
// INTERVAL to 0, but RX descriptor write-back still routes through the interrupt-moderation
// logic. Datasheet 332464 rev4.1 §8.3.3.1.4.2: the 700-series writes RX descriptors back
// ONLY when (a) a full 128B descriptor line completes (4x32B — NEVER at one frame in
// flight), (b) the queue context is evicted, or (c) the interrupt logic initiates it. There
// is no "write back immediately" case. The escape hatch is Intel's own, verbatim: "The
// receive descriptors could be reported instantly for each packet by setting the ITR_INDX to
// NoITR." DPDK binds the main VSI with I40E_ITR_INDEX_DEFAULT (0) — a real ITR — at
// i40e_ethdev.c:2473, and uses I40E_ITR_INDEX_NONE (3) ONLY for the FDIR VSI
// (i40e_fdir.c:233), i.e. exactly where it wants prompt write-back. Never on the data path.
//
// SELF-LOCATING SCAN. QINT_RQCTL is indexed by the ABSOLUTE queue number (vsi->base_queue +
// i), which we cannot know from outside the PMD — a guessed index silently no-ops. But
// __vsi_queues_bind_intr (i40e_ethdev.c:2088) sets CAUSE_ENA (bit 30) on every queue it
// binds, and the register resets to 0. So: scan, and patch exactly the entries with
// CAUSE_ENA set. Read-modify-write bits 12:11 only — MSIX_INDX, NEXTQ_INDX and NEXTQ_TYPE
// are preserved, so the interrupt linked-list stays intact.
//
// PROVEN TO WORK, AND IT REPLACES i40eClearItr (2026-07-11, XXV710, 32M samples):
//   ITR interval 32us (PMD default) + ITR_INDX 0  -> ~30us RTT   (stock DPDK)
//   ITR interval 0    (i40eClearItr) + ITR_INDX 0 ->  9.028us
//   ITR interval 32us (PMD default) + ITR_INDX 3  ->  9.029us    <== NoITR ALONE
// So NoITR genuinely detaches the queue from the ITR: the 32us interval becomes irrelevant,
// exactly as the datasheet says. It and interval-zero are two routes to the SAME floor —
// either alone suffices, and neither goes below ~9us (that floor is silicon, not write-back).
// We use NoITR because it is ONE write instead of two and is structurally immune to anything
// re-arming the interval (the kernel path's adaptive ITR rewrites the INTERVAL, never the
// INDEX). i40eClearItr is kept for reference but is no longer called.
// The post-dev_start write IS honored: a positive control (i40eItrProbe — point the queue at
// a 20us ITR index) took the median 9.03 -> 20.65us. So a readback of 3 means it really is in
// force, not merely stored.
// VALIDATING THE READ IS MANDATORY. Most of the 1536-entry window is UNIMPLEMENTED on this
// part and reads back 0xDEADBEEF — Intel's signature for an unbacked register. That value is
// a trap for a naive scan: bit 30 (CAUSE_ENA) is SET in it and bits 12:11 (ITR_INDX) are
// already 3, so it masquerades as "a bound queue that accepted the write".
//
// DO NOT KEY ON NEXTQ_TYPE == 0. The two drivers chain the interrupt linked-list differently:
//   DPDK   __vsi_queues_bind_intr  -> NEXTQ_TYPE = 0 (I40E_QUEUE_TYPE_RX)
//   kernel i40e_vsi_configure_msix -> NEXTQ_TYPE = 1 (I40E_QUEUE_TYPE_TX)  <-- RX chains to TX
// (i40e_type.h:226 — RX=0, TX=1, PE_CEQ=2, UNKNOWN=3.) An earlier filter required 0 and so
// found NOTHING on the kernel path (native AF_XDP / DPDK-over-AF_XDP). What actually rejects
// 0xDEADBEEF is reserved bit 31 (set in it, clear in every real value); NEXTQ_TYPE=3
// (UNKNOWN, also set in it) is a second, independent guard.
inline constexpr std::size_t   I40E_QINT_RQCTL_BASE    = 0x0003A000;
inline constexpr std::size_t   I40E_QINT_RQCTL_COUNT   = 1536;   // _Q = 0..1535
inline constexpr std::uint32_t I40E_ITR_INDX_SHIFT     = 11;
inline constexpr std::uint32_t I40E_ITR_INDX_MASK      = 0x3u << I40E_ITR_INDX_SHIFT;
inline constexpr std::uint32_t I40E_CAUSE_ENA_MASK     = 0x1u << 30;
inline constexpr std::uint32_t I40E_NEXTQ_TYPE_SHIFT   = 27;
inline constexpr std::uint32_t I40E_NEXTQ_TYPE_MASK    = 0x3u << I40E_NEXTQ_TYPE_SHIFT;
inline constexpr std::uint32_t I40E_QUEUE_TYPE_UNKNOWN = 3;   // i40e_type.h:229
inline constexpr std::uint32_t I40E_RQCTL_RSVD_MASK    = 0x1u << 31;
inline constexpr std::uint32_t I40E_ITR_INDEX_NONE     = 3;
inline constexpr std::uint32_t I40E_REG_UNIMPL         = 0xDEADBEEFu;

// A real driver-written QINT_RQCTL, on EITHER stack: bound (CAUSE_ENA), reserved bit clear,
// a valid NEXTQ_TYPE (RX/TX/PE_CEQ — not UNKNOWN), and not an unbacked-read signature.
[[nodiscard]] inline bool i40eIsBoundRxQueue(std::uint32_t v) noexcept {
    if (v == I40E_REG_UNIMPL || v == 0xFFFFFFFFu) {
        return false;
    }
    if ((v & I40E_CAUSE_ENA_MASK) == 0) {
        return false;
    }
    if ((v & I40E_RQCTL_RSVD_MASK) != 0) {
        return false;
    }
    if (((v & I40E_NEXTQ_TYPE_MASK) >> I40E_NEXTQ_TYPE_SHIFT) == I40E_QUEUE_TYPE_UNKNOWN) {
        return false;
    }
    return true;
}

inline bool i40eSetNoItr(const std::string& bdf, std::string_view ifname) noexcept {
    if (bdf.empty()) {
        fmt::print(stderr, "[PCI] {}: no BDF — i40e NoITR not applied\n", ifname);
        return false;
    }
    const std::size_t bar0Size = barSize(bdf, 0);
    const std::string resPath  = "/sys/bus/pci/devices/" + bdf + "/resource0";
    BackendBase       bar0;
    if (bar0Size == 0 || !bar0.open(resPath.c_str(), 0, bar0Size)) {
        fmt::print(stderr, "[PCI] {} ({}): resource0 mmap failed — i40e NoITR not applied\n", ifname, bdf);
        return false;
    }

    // Clamp the scan to what BAR0 actually maps.
    std::size_t count = I40E_QINT_RQCTL_COUNT;
    if (bar0Size < I40E_QINT_RQCTL_BASE + count * 4) {
        if (bar0Size <= I40E_QINT_RQCTL_BASE) {
            fmt::print(stderr, "[PCI] {} ({}): BAR0 too small for QINT_RQCTL — NoITR not applied\n", ifname,
                       bdf);
            return false;
        }
        count = (bar0Size - I40E_QINT_RQCTL_BASE) / 4;
    }

    unsigned      patched     = 0;
    unsigned      failed      = 0;
    std::size_t   firstQ      = 0;
    std::uint32_t firstBefore = 0;
    std::uint32_t firstAfter  = 0;
    for (std::size_t q = 0; q < count; ++q) {
        auto*               reg    = bar0.registerPtr<std::uint32_t>(I40E_QINT_RQCTL_BASE + q * 4);
        const std::uint32_t before = *reg;
        if (!i40eIsBoundRxQueue(before)) {
            continue;   // unbacked offset, or queue not bound
        }
        const std::uint32_t want = (before & ~I40E_ITR_INDX_MASK) |
                                   (I40E_ITR_INDEX_NONE << I40E_ITR_INDX_SHIFT);
        *reg                      = want;
        const std::uint32_t after = *reg;   // read back: did the write land?
        if (patched + failed == 0) {
            firstQ      = q;
            firstBefore = before;
            firstAfter  = after;
        }
        ((after & I40E_ITR_INDX_MASK) == I40E_ITR_INDX_MASK) ? ++patched : ++failed;
    }

    if (patched == 0 && failed == 0) {
        // Self-diagnosing miss: dump what the scan actually saw, so a validator that is wrong for
        // some future driver says WHY instead of just failing (this is how the kernel path's
        // NEXTQ_TYPE=TX was found). Skip zeros (unbound) and the unbacked-read signatures.
        fmt::print(stderr,
                   "[PCI] {} ({}): i40e NoITR NOT applied — no bound RX queue in "
                   "QINT_RQCTL. Non-empty entries seen:\n",
                   ifname, bdf);
        unsigned shown = 0;
        for (std::size_t q = 0; q < count && shown < 6; ++q) {
            const std::uint32_t v = *bar0.registerPtr<std::uint32_t>(I40E_QINT_RQCTL_BASE + q * 4);
            if (v == 0 || v == I40E_REG_UNIMPL || v == 0xFFFFFFFFu) {
                continue;
            }
            fmt::print(stderr,
                       "[PCI]   q={} 0x{:08x} (cause_ena={} nextq_type={} itr_indx={} "
                       "msix={} rsvd31={})\n",
                       q, v, (v & I40E_CAUSE_ENA_MASK) ? 1 : 0,
                       (v & I40E_NEXTQ_TYPE_MASK) >> I40E_NEXTQ_TYPE_SHIFT,
                       (v & I40E_ITR_INDX_MASK) >> I40E_ITR_INDX_SHIFT, v & 0xFFu,
                       (v & I40E_RQCTL_RSVD_MASK) ? 1 : 0);
            ++shown;
        }
        if (shown == 0) {
            fmt::print(stderr, "[PCI]   (none — every entry was 0 or unbacked)\n");
        }
        return false;
    }
    if (failed != 0) {
        fmt::print(stderr,
                   "[PCI] {} ({}): i40e NoITR REFUSED on {} of {} RX queue(s) — RX "
                   "write-back is STILL ITR-gated\n",
                   ifname, bdf, failed, patched + failed);
        return false;
    }
    fmt::print(stderr, "[PCI] {} ({}): i40e NoITR on {} RX queue(s) (q{}: 0x{:08x} -> 0x{:08x})\n", ifname,
               bdf, patched, firstQ, firstBefore, firstAfter);
    return true;
}

// ── i40e QINT_RQCTL POSITIVE CONTROL (a probe, NOT a tuning lever) ───────────────────────
//
// THE PROBLEM IT SOLVES. i40eSetNoItr() reads ITR_INDX back as 3, yet the median RTT does not
// move (9.028 -> 9.027us). A register readback proves the REGISTER took the write; it says
// NOTHING about whether the queue's internal CONTEXT is using it. The PMD programs
// QINT_RQCTL during dev_start BEFORE i40e_dev_rx_queue_start enables the queue, so if the
// silicon latches ITR_INDX into the context at enable time, the register will happily report
// 3 while the datapath still uses 0. Two hypotheses, indistinguishable by reading:
//   (A) the post-start write is IGNORED  -> NoITR was never actually applied
//   (B) the write IS honored             -> NoITR is applied and simply buys nothing
//
// THE DISCRIMINATOR. Point ITR_INDX at a DIFFERENT index (1 or 2 — i40eClearItr only zeroes
// index 0, so these are free) and program THAT index with a deliberately large interval. The
// outcome is unmissable either way:
//   latency EXPLODES  -> silicon follows our post-start QINT_RQCTL write -> (B): NoITR is
//                        genuinely applied, genuinely useless. i40e is at its floor.
//   latency UNCHANGED -> silicon ignores it -> (A): patch i40e_ethdev.c:2473 to pass
//                        I40E_ITR_INDEX_NONE at BIND time; that is the only way to test NoITR.
//
// PFINT_ITRN(_i,_INTPF) = 0x30000 + _i*2048 + _INTPF*4 (_i=0..2); PFINT_ITR0(_i) = 0x38000 +
// _i*128. Vector 0 (the MISC vector) uses ITR0; vectors 1..N use ITRN(_i, msix_vect-1).
// INTERVAL is bits [11:0] in 2us units, so interval2us=10 => 20us — a ~3x RTT blowup here.
inline constexpr std::size_t I40E_PFINT_ITRN_BASE   = 0x00030000;
inline constexpr std::size_t I40E_PFINT_ITRN_STRIDE = 2048;
inline constexpr std::size_t I40E_PFINT_ITR0_BASE   = 0x00038000;
inline constexpr std::size_t I40E_PFINT_ITR0_STRIDE = 128;

inline bool i40eItrProbe(const std::string& bdf, std::string_view ifname, std::uint32_t itrIdx,
                         std::uint32_t interval2us) noexcept {
    if (bdf.empty() || itrIdx > 2) {
        fmt::print(stderr, "[PCI] {}: ITR probe skipped (bad BDF or itrIdx>2)\n", ifname);
        return false;
    }
    const std::size_t bar0Size = barSize(bdf, 0);
    const std::string resPath  = "/sys/bus/pci/devices/" + bdf + "/resource0";
    BackendBase       bar0;
    if (bar0Size == 0 || !bar0.open(resPath.c_str(), 0, bar0Size)) {
        fmt::print(stderr, "[PCI] {} ({}): resource0 mmap failed — ITR probe skipped\n", ifname, bdf);
        return false;
    }
    std::size_t count = I40E_QINT_RQCTL_COUNT;
    if (bar0Size < I40E_QINT_RQCTL_BASE + count * 4) {
        if (bar0Size <= I40E_QINT_RQCTL_BASE) {
            return false;
        }
        count = (bar0Size - I40E_QINT_RQCTL_BASE) / 4;
    }

    unsigned n = 0;
    for (std::size_t q = 0; q < count; ++q) {
        auto*               rqctl  = bar0.registerPtr<std::uint32_t>(I40E_QINT_RQCTL_BASE + q * 4);
        const std::uint32_t before = *rqctl;
        if (!i40eIsBoundRxQueue(before)) {
            continue;
        }

        // Load the chosen ITR index with a big interval, on the vector this queue rides.
        const std::uint32_t msix   = before & 0xFFu;
        const std::size_t   itrOff = (msix == 0) ? I40E_PFINT_ITR0_BASE + itrIdx * I40E_PFINT_ITR0_STRIDE
                                                 : I40E_PFINT_ITRN_BASE + itrIdx * I40E_PFINT_ITRN_STRIDE +
                                                     static_cast<std::size_t>((msix - 1) * 4);
        if (itrOff + 4 > bar0Size) {
            continue;
        }
        auto* itr                   = bar0.registerPtr<std::uint32_t>(itrOff);
        *itr                        = interval2us & I40E_ITR_INTERVAL_MASK;
        const std::uint32_t itrBack = *itr;

        // Point the queue at that index.
        const std::uint32_t want = (before & ~I40E_ITR_INDX_MASK) |
                                   ((itrIdx << I40E_ITR_INDX_SHIFT) & I40E_ITR_INDX_MASK);
        *rqctl                    = want;
        const std::uint32_t after = *rqctl;
        ++n;
        fmt::print(stderr,
                   "[PCI] {} ({}): ITR PROBE q={} msix={} ITR[{}]@0x{:05x}={} ({}us)  QINT_RQCTL "
                   "0x{:08x} -> 0x{:08x} (ITR_INDX {} -> {})\n",
                   ifname, bdf, q, msix, itrIdx, itrOff, itrBack, itrBack * 2, before, after,
                   (before & I40E_ITR_INDX_MASK) >> I40E_ITR_INDX_SHIFT,
                   (after & I40E_ITR_INDX_MASK) >> I40E_ITR_INDX_SHIFT);
    }
    fmt::print(stderr,
               "[PCI] {} ({}): ITR PROBE armed on {} queue(s). RTT MUST now blow up. "
               "If it does NOT, the silicon is IGNORING post-start QINT_RQCTL writes "
               "=> NoITR was never applied and needs the bind-time DPDK patch.\n",
               ifname, bdf, n);
    return n > 0;
}

// igc (i225): clear the EEE/LPI enable bits in EEER (0x0E30). VERIFIED REDUNDANT
// (2026-07-10): the igc PMD's base init already disables EEE, and the i225 has no
// 802.3az support at all (i226-only) — this always logs 0x0 -> 0x0. Kept as
// belt-and-braces for parity with the published soak binaries; see
// docs/Known_driver_issues.md §2.4.
inline bool igcDisableEee(const std::string& bdf, std::string_view ifname) noexcept {
    constexpr std::size_t   kEeer      = 0x00000E30;
    constexpr std::uint32_t kLpiEnable = 0x00030000;   // TX_LPI_EN | RX_LPI_EN
    if (bdf.empty()) {
        fmt::print(stderr, "[PCI] {}: no BDF — igc EEE left as-is\n", ifname);
        return false;
    }
    const std::size_t bar0Size = barSize(bdf, 0);
    const std::string resPath  = "/sys/bus/pci/devices/" + bdf + "/resource0";
    BackendBase       bar0;
    if (bar0Size < kEeer + 4 || !bar0.open(resPath.c_str(), 0, bar0Size)) {
        fmt::print(stderr, "[PCI] {} ({}): resource0 mmap failed — igc EEE left as-is\n", ifname, bdf);
        return false;
    }
    auto*               eeer   = bar0.registerPtr<std::uint32_t>(kEeer);
    const std::uint32_t before = *eeer;
    *eeer                      = before & ~kLpiEnable;
    fmt::print(stderr, "[PCI] {} ({}): igc EEE/LPI disabled (EEER 0x{:08x} -> 0x{:08x})\n", ifname, bdf,
               before, *eeer);
    return true;
}

}   // namespace pci
