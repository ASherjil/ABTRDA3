#pragma once

// DPDKEal — process-global EAL state + the cold-path plumbing, in a NON-TEMPLATE
// base class so it has exactly ONE copy per process: EAL initialises once, and its
// -a allowlist fixes which PCI devices exist for the WHOLE process. (A static inside
// the DPDK<> class template would be duplicated per specialization and double-init
// EAL.) DPDK<> inherits it privately — empty base, no virtuals, zero cost, and none
// of this is on the hot path.
//
// Definitions live in DPDKEal.cpp, so rte_eal_init / the vfio bind are compiled once
// instead of in every TU that includes DPDK.hpp.
//
// Traps + rationale: docs/Known_driver_issues.md §3.

#include <cstddef>
#include <string>
#include <string_view>

class DPDKEal {
protected:
  DPDKEal() noexcept = default;

  // i40e RX/TX path select: AVX-512 and scalar measured IDENTICAL (~9.15us) — the
  // floor is the i40e hardware, not the vector path. 256 = AVX2 (KDI §3).
  static constexpr unsigned kMaxSimdBitwidth = 256;

  // Park EAL's control thread ("dpdk-intr") here, off the poll core — otherwise it
  // lands ON it and ping-pongs with our busy-poll ~80x/s -> tail spikes (KDI §3).
  // Core 0 = kernel housekeeping/IRQ core (irqaffinity=0,1).
  static constexpr int kControlThreadCore = 0;

  // Both lists are filled by prepare() BEFORE the single EAL init (the single-recorder
  // owns both loopback ports in one process). Dedup keys on the BDF/vdev name and the
  // FIRST registration wins, so a devargs-bearing entry survives (KDI §3).
  static void addAllowedBdf(const std::string& bdf);   // "<bdf>[,key=val...]"    -> -a
  static void addVdev(const std::string& arg);         // "net_af_xdpN,iface=..." -> --vdev

  [[nodiscard]] static std::size_t vdevCount() noexcept;   // names the next vdev

  // Runs rte_eal_init ONCE; later calls are no-ops returning true.
  [[nodiscard]] static bool ealInit(const std::string& bdf, int lcore) noexcept;

  static bool writeSysfs(const std::string& path, std::string_view val) noexcept;
  [[nodiscard]] static bool bindToVfio(const std::string& bdf) noexcept;
};
