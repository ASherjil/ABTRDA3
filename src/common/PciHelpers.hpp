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

#include <fcntl.h>
#include <unistd.h>

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

}  // namespace pci
