//
// PciHelpers — generic PCI / sysfs lookups shared across transports.
//
// resolveBdf(): map a kernel network interface name (e.g. "enp1s0f1np1") to its
// PCI domain:bus:device.function id (e.g. "0000:01:00.1"). The DPDK transport
// addresses devices by BDF, so it resolves the name WHILE the netdev still
// exists — before the device is unbound from the kernel driver onto vfio-pci,
// which removes the interface name.
//
// (Intel_I210 / Cadence_GEM do NOT use this: they reach the PCIe / AXI bus
// through the ABTEdge backend, which handles bus access for them.)
//
#pragma once

#include "BackendBase.hpp"   // ABTEdge — sysfs-resource0 MMIO (for the i40e ITR read)

#include <fmt/core.h>

#include <fcntl.h>
#include <unistd.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>

namespace pci {

// basename of a sysfs symlink target (".../0000:01:00.1" -> "0000:01:00.1").
// Empty if the path is not a symlink / does not exist.
[[nodiscard]] inline std::string readlinkBasename(const std::string& path) noexcept {
  char buf[256];
  const ssize_t n = ::readlink(path.c_str(), buf, sizeof(buf) - 1);
  if (n <= 0) return {};
  buf[n] = '\0';
  std::string s(buf);
  const auto pos = s.find_last_of('/');
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
  std::string path = "/sys/bus/pci/devices/" + std::string(bdf) + "/resource";
  std::FILE* f = std::fopen(path.c_str(), "r");
  if (!f) return 0;
  char line[160]{};
  for (int i = 0; i <= barIndex; ++i) {
    if (!std::fgets(line, sizeof(line), f)) { std::fclose(f); return 0; }
  }
  std::fclose(f);
  unsigned long long start = 0, end = 0;
  if (std::sscanf(line, "%llx %llx", &start, &end) == 2 && end >= start && start != 0)
    return static_cast<std::size_t>(end - start + 1ULL);
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
  if (ifname.size() < 5 || ifname[0] != 'e' || ifname[1] != 'n' || ifname[2] != 'p')
    return {};
  std::size_t i = 3;
  auto readDec = [&](unsigned& out) -> bool {
    const std::size_t start = i;
    out = 0;
    while (i < ifname.size() && ifname[i] >= '0' && ifname[i] <= '9')
      out = out * 10u + static_cast<unsigned>(ifname[i++] - '0');
    return i > start;
  };
  unsigned bus = 0, dev = 0, func = 0;
  if (!readDec(bus)) return {};
  if (i >= ifname.size() || ifname[i] != 's') return {};
  ++i;
  if (!readDec(dev)) return {};
  if (i < ifname.size() && ifname[i] == 'f') { ++i; if (!readDec(func)) return {}; }

  char bdf[16];
  std::snprintf(bdf, sizeof(bdf), "0000:%02x:%02x.%x",
                bus & 0xffu, dev & 0xffu, func & 0xfu);
  if (::access((std::string("/sys/bus/pci/devices/") + bdf).c_str(), F_OK) != 0)
    return {};
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
inline constexpr std::size_t   I40E_PFINT_ITR0_0     = 0x00038000;
inline constexpr std::size_t   I40E_PFINT_ITRN_0_0   = 0x00030000;
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
inline bool reportItr(std::string_view ifname, const char* tag) {
  std::string bdf = resolveBdf(ifname);     // kernel driver: netdev present
  if (bdf.empty()) bdf = bdfFromName(ifname); // vfio-pci (DPDK): netdev gone
  if (bdf.empty()) {
    fmt::print(stderr, "[{}] {}: cannot resolve PCI BDF\n", tag, ifname);
    return false;
  }

  const std::size_t bar0Size = barSize(bdf, 0);
  const std::string resPath  = "/sys/bus/pci/devices/" + bdf + "/resource0";
  BackendBase bar0;
  if (bar0Size == 0 || !bar0.open(resPath.c_str(), 0, bar0Size)) {
    fmt::print(stderr, "[{}] {} ({}): resource0 mmap failed (need root?)\n",
               tag, ifname, bdf);
    return false;
  }

  const std::uint32_t itr0 = *bar0.registerPtr<std::uint32_t>(I40E_PFINT_ITR0_0);
  const std::uint32_t itrn = *bar0.registerPtr<std::uint32_t>(I40E_PFINT_ITRN_0_0);
  const std::uint32_t itr0Int = itr0 & I40E_ITR_INTERVAL_MASK;
  const std::uint32_t itrnInt = itrn & I40E_ITR_INTERVAL_MASK;
  const bool rxThrottleOff = (itrnInt == 0);   // ITRN = our data-queue RX ITR

  fmt::print(stderr,
    "[{}] {} ({}): PFINT_ITRN(data-RX)=0x{:08x} ({}us) PFINT_ITR0(misc)=0x{:08x} ({}us) "
    "=> RX path {}\n",
    tag, ifname, bdf, itrn, itrnInt * 2u, itr0, itr0Int * 2u,
    rxThrottleOff ? "UNTHROTTLED (ITRN=0)" : "THROTTLED (ITRN nonzero!)");
  return rxThrottleOff;
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
    fmt::print(stderr, "[PCI] {}: no BDF — i40e ITR not cleared, expect ~30us+ median RTT\n",
               ifname);
    return false;
  }
  const std::size_t bar0Size = barSize(bdf, 0);
  const std::string resPath  = "/sys/bus/pci/devices/" + bdf + "/resource0";
  BackendBase bar0;
  if (bar0Size == 0 || !bar0.open(resPath.c_str(), 0, bar0Size)) {
    fmt::print(stderr, "[PCI] {} ({}): resource0 mmap failed — i40e ITR not cleared, "
                       "expect ~30us+ median RTT\n", ifname, bdf);
    return false;
  }
  *bar0.registerPtr<std::uint32_t>(I40E_PFINT_ITR0_0)   = 0;
  *bar0.registerPtr<std::uint32_t>(I40E_PFINT_ITRN_0_0) = 0;
  const std::uint32_t itr0 = *bar0.registerPtr<std::uint32_t>(I40E_PFINT_ITR0_0);
  const std::uint32_t itrn = *bar0.registerPtr<std::uint32_t>(I40E_PFINT_ITRN_0_0);
  const bool off = ((itr0 | itrn) & I40E_ITR_INTERVAL_MASK) == 0;
  fmt::print(stderr, "[PCI] {} ({}): i40e ITR cleared (ITR0=0x{:08x} ITRN=0x{:08x}) "
                     "=> RX write-back {}\n",
             ifname, bdf, itr0, itrn, off ? "UNTHROTTLED" : "STILL THROTTLED!");
  return off;
}

// igc (i225): clear the EEE/LPI enable bits in EEER (0x0E30). VERIFIED REDUNDANT
// (2026-07-10): the igc PMD's base init already disables EEE, and the i225 has no
// 802.3az support at all (i226-only) — this always logs 0x0 -> 0x0. Kept as
// belt-and-braces for parity with the published soak binaries; see
// docs/Known_driver_issues.md §3.3.
inline bool igcDisableEee(const std::string& bdf, std::string_view ifname) noexcept {
  constexpr std::size_t   kEeer      = 0x00000E30;
  constexpr std::uint32_t kLpiEnable = 0x00030000;   // TX_LPI_EN | RX_LPI_EN
  if (bdf.empty()) {
    fmt::print(stderr, "[PCI] {}: no BDF — igc EEE left as-is\n", ifname);
    return false;
  }
  const std::size_t bar0Size = barSize(bdf, 0);
  const std::string resPath  = "/sys/bus/pci/devices/" + bdf + "/resource0";
  BackendBase bar0;
  if (bar0Size < kEeer + 4 || !bar0.open(resPath.c_str(), 0, bar0Size)) {
    fmt::print(stderr, "[PCI] {} ({}): resource0 mmap failed — igc EEE left as-is\n",
               ifname, bdf);
    return false;
  }
  auto* eeer = bar0.registerPtr<std::uint32_t>(kEeer);
  const std::uint32_t before = *eeer;
  *eeer = before & ~kLpiEnable;
  fmt::print(stderr, "[PCI] {} ({}): igc EEE/LPI disabled (EEER 0x{:08x} -> 0x{:08x})\n",
             ifname, bdf, before, *eeer);
  return true;
}

}  // namespace pci
