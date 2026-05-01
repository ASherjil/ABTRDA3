//
// Created by asherjil on 4/16/26.
//

#ifndef ABTRDA3_TAPBRIDGE_HPP
#define ABTRDA3_TAPBRIDGE_HPP

#include "RingConcepts.hpp"

#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <arpa/inet.h>
#include <fcntl.h>
#include <net/if.h>           // POSIX — IFNAMSIZ, struct ifreq, IFF_UP/RUNNING.
                              // Use this instead of <linux/if.h> so the kernel
                              // UAPI header doesn't clash with glibc's: when
                              // both reach the same TU via different paths,
                              // they define overlapping anonymous unions with
                              // incompatible layouts (ODR-violation warning at
                              // link time on the CERN FECOS sysroot).
#include <net/route.h>
#include <linux/if_tun.h>     // TUN/TAP macros (TUNSETIFF, IFF_TAP, IFF_NO_PI)
                              // — standalone, does not pull in <linux/if.h>.
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <utility>


// RAII bridge between a NIC's slow-path hardware queue and the kernel networking
// stack via a TAP device. Packets received on the NIC queue are injected into the
// kernel (NFS, SSH, ARP, DHCP); packets the kernel wants to send are read from
// the TAP fd and transmitted on the NIC queue.
//
// The class satisfies the Callable<std::stop_token> requirement for std::jthread:
//
//     CadenceGEM nic(...);
//     TapBridge tap(nic.tapTx(), nic.tapRx(), "tap_pmd");
//     std::jthread tapThread(std::ref(tap));
//
// Designed for single-port devices (ARM64 SoC, Intel I219-LM) where the only
// network interface is also used for PXE boot, NFS mounts, and SSH access.
template <TxRing Tx, RxRing Rx>
class TapBridge {
public:
  TapBridge(Tx& tx, Rx& rx, std::string_view tapName = "tap_pmd")
    :m_tx{tx}, m_rx{rx}{

    m_fd = ::open("/dev/net/tun", O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (m_fd <0) {
      throw std::runtime_error{"TapBridge: open /dev/net/tun: " + std::string{std::strerror(errno)}};
    }

    ifreq ifr1{};
    ifr1.ifr_flags = IFF_TAP | IFF_NO_PI; // L2 frames, no protocol info header

    if (tapName.size() >= IFNAMSIZ) {
      throw std::invalid_argument{"TapBridge: interface name exceeds IFNAMSIZ"};
    }

    tapName.copy(ifr1.ifr_name, tapName.size());

    if (::ioctl(m_fd, TUNSETIFF, &ifr1) < 0) {
      ::close(m_fd);
      throw std::runtime_error{"TapBridge: TUNSETIFF: " + std::string{std::strerror(errno)}};
    }

    m_name = ifr1.ifr_name;

    int sock = ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (sock < 0) {
      ::close(m_fd);
      m_fd = -1;
      throw std::runtime_error{"TapBridge: socket() for bringUp: " + std::string{std::strerror(errno)}};
    }

    ifreq ifr2{}; // reset the ifreq and reuse it
    m_name.copy(ifr2.ifr_name, m_name.size());

    if (::ioctl(sock, SIOCGIFFLAGS, &ifr2) == 0) {
      ifr2.ifr_flags |= IFF_UP | IFF_RUNNING;
      ::ioctl(sock, SIOCSIFFLAGS, &ifr2);
    }
    ::close(sock);

    std::printf("[TapBridge] Created %s (fd=%d)\n", m_name.c_str(), m_fd);
  }

  // Movable — transfers fd ownership
  TapBridge(TapBridge&& other) noexcept
    : m_tx{other.m_tx}, m_rx{other.m_rx},
      m_fd{std::exchange(other.m_fd, -1)},
      m_name{std::move(other.m_name)},
      m_rxCount{other.m_rxCount},
      m_txCount{other.m_txCount} {}

  // Non-copyable and cannot be moved
  TapBridge(const TapBridge&)            = delete;
  TapBridge& operator=(const TapBridge&) = delete;
  TapBridge& operator=(TapBridge&&)      = delete;

  ~TapBridge() {
    if (m_fd >= 0) {
      ::close(m_fd);
      std::printf("[TapBridge] Closed %s — RX→kernel: %lu, kernel→TX: %lu\n", m_name.c_str(), m_rxCount, m_txCount);
    }
  }

  // std::jthread entry point - bridges NIC slow-path queue and kernel via TAP
  // Polls NIC RX -> writes to TAP fd (kernel receives)
  // Reads TAP fd -> writes to NIC TX (kernel transmits)
  void operator()(std::stop_token stop) {
    using namespace std::chrono_literals;
    std::printf("[TapBridge] Bridge running on %s\n", m_name.c_str());

    alignas(64) std::array<std::uint8_t, 1518> buf{};

    while (!stop.stop_requested()) {
      bool activity{};

      // RX: NIC queue -> TAP fd packets destined for the kernel
      if (RxFrame rxf = m_rx.tryReceive(); !rxf.data.empty()) {
        ::write(m_fd, rxf.data.data(), rxf.data.size());
        m_rx.release();
        m_rxCount++;
        activity = true;
      }

      // Tx : TAP fd -> NIC queue packets originating from kernel
      if (ssize_t n = ::read(m_fd, buf.data(), buf.size()); n>0) {
        std::uint32_t len = static_cast<std::uint32_t>(n);

        if (std::uint8_t* dst = m_tx.acquire(len)) {
          std::memcpy(dst, buf.data(), len);
          m_tx.commit();
          m_txCount++;
          activity = true;
        }
      }

      if (!activity) {
        std::this_thread::sleep_for(200us);
      }
    }
    std::printf("[TapBridge] Stopped — RX→kernel: %lu, kernel→TX: %lu\n", m_rxCount, m_txCount);
  }

  // Assign this TAP the L3 identity of the NIC it's proxying for.
  // Sets MAC, IPv4 address/prefix, subnet mask, and default route via raw
  // ioctls — no fork/exec, no PATH lookup.  Returns true if every required
  // ioctl succeeded; partial config is still better than none.
  [[nodiscard]] bool setupAlias(const std::array<std::uint8_t, 6>& mac,
                                const std::string&                 cidr,
                                const std::string&                 gateway) noexcept {
    if (cidr.empty() || gateway.empty()) {
      std::fprintf(stderr,
                   "[TapBridge] %s: empty cidr/gateway — leaving without IP/route\n",
                   m_name.c_str());
      return false;
    }
    const int s = ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (s < 0) {
      std::fprintf(stderr, "[TapBridge] %s: socket: %s\n", m_name.c_str(), std::strerror(errno));
      return false;
    }

    ifreq ifr{};
    std::strncpy(ifr.ifr_name, m_name.c_str(), IFNAMSIZ - 1);

    // Bring DOWN — SIOCSIFHWADDR is rejected on a live interface
    if (::ioctl(s, SIOCGIFFLAGS, &ifr) == 0) {
      ifr.ifr_flags &= ~static_cast<short>(IFF_UP);
      ::ioctl(s, SIOCSIFFLAGS, &ifr);
    }

    // hwaddr ← NIC's MAC
    ifr.ifr_hwaddr.sa_family = 1;   // ARPHRD_ETHER
    std::memcpy(ifr.ifr_hwaddr.sa_data, mac.data(), 6);
    if (::ioctl(s, SIOCSIFHWADDR, &ifr) < 0)
      std::fprintf(stderr, "[TapBridge] %s: SIOCSIFHWADDR: %s\n", m_name.c_str(), std::strerror(errno));

    // Parse "<ip>/<prefix>" → ip + mask
    std::string ip = cidr;
    int prefix = 24;
    if (auto slash = cidr.find('/'); slash != std::string::npos) {
      ip = cidr.substr(0, slash);
      prefix = std::stoi(cidr.substr(slash + 1));
    }

    auto* sin = reinterpret_cast<sockaddr_in*>(&ifr.ifr_addr);
    sin->sin_family = AF_INET;
    ::inet_pton(AF_INET, ip.c_str(), &sin->sin_addr);
    if (::ioctl(s, SIOCSIFADDR, &ifr) < 0)
      std::fprintf(stderr, "[TapBridge] %s: SIOCSIFADDR: %s\n", m_name.c_str(), std::strerror(errno));

    auto* nmask = reinterpret_cast<sockaddr_in*>(&ifr.ifr_netmask);
    nmask->sin_family = AF_INET;
    nmask->sin_addr.s_addr = htonl(prefix == 0 ? 0U : ~((1U << (32 - prefix)) - 1));
    if (::ioctl(s, SIOCSIFNETMASK, &ifr) < 0)
      std::fprintf(stderr, "[TapBridge] %s: SIOCSIFNETMASK: %s\n", m_name.c_str(), std::strerror(errno));

    // Bring UP + RUNNING
    if (::ioctl(s, SIOCGIFFLAGS, &ifr) == 0) {
      ifr.ifr_flags |= IFF_UP | IFF_RUNNING;
      if (::ioctl(s, SIOCSIFFLAGS, &ifr) < 0)
        std::fprintf(stderr, "[TapBridge] %s: SIOCSIFFLAGS UP: %s\n", m_name.c_str(), std::strerror(errno));
    }

    // Default route via gateway through this TAP
    rtentry rt{};
    reinterpret_cast<sockaddr_in*>(&rt.rt_dst)->sin_family     = AF_INET;
    reinterpret_cast<sockaddr_in*>(&rt.rt_genmask)->sin_family = AF_INET;
    auto* gwsin = reinterpret_cast<sockaddr_in*>(&rt.rt_gateway);
    gwsin->sin_family = AF_INET;
    ::inet_pton(AF_INET, gateway.c_str(), &gwsin->sin_addr);
    rt.rt_flags = RTF_UP | RTF_GATEWAY;
    char devBuf[IFNAMSIZ]{};
    std::strncpy(devBuf, m_name.c_str(), IFNAMSIZ - 1);
    rt.rt_dev = devBuf;
    if (::ioctl(s, SIOCADDRT, &rt) < 0)
      std::fprintf(stderr, "[TapBridge] %s: SIOCADDRT: %s\n", m_name.c_str(), std::strerror(errno));

    ::close(s);
    std::fprintf(stderr,
                 "[TapBridge] %s alias set: mac=%02x:%02x:%02x:%02x:%02x:%02x addr=%s gw=%s\n",
                 m_name.c_str(),
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
                 cidr.c_str(), gateway.c_str());
    return true;
  }

  [[nodiscard]] int fd() const noexcept {
    return m_fd;
  }

  [[nodiscard]] std::string_view name() const noexcept {
    return m_name;
  }

  [[nodiscard]] std::uint64_t rxCount() const noexcept {
    return m_rxCount;
  }

  [[nodiscard]] std::uint64_t txCount() const noexcept {
    return m_txCount;
  }

private:
  Tx& m_tx;
  Rx& m_rx;
  int           m_fd{-1};
  std::string   m_name;
  std::uint64_t m_rxCount{};
  std::uint64_t m_txCount{};
};


#endif //ABTRDA3_TAPBRIDGE_HPP
