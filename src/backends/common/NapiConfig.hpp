// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Areeb Sherjil

//
// NapiConfig — shared AF_XDP / NAPI control-plane helpers, used by BOTH the
// native AFXDP transport and the DPDK net_af_xdp path so the two stacks
// precondition and verify a port identically. Header-only, no libnl/libbpf
// link dependency (netlink is hand-rolled over raw AF_NETLINK sockets).
//
// Part 1 — per-NAPI busy-poll parameters via the netdev generic-netlink family
// (NETDEV_CMD_NAPI_SET/GET). Two knobs the kernel only exposes through netlink,
// not sysfs:
//   * defer-hard-irqs / gro-flush-timeout  — also in sysfs, but set here too so
//     one call covers everything on the live NAPI;
//   * irq-suspend-timeout (kernel 6.13+)   — netlink ONLY. THIS is the lever:
//     while the app busy-polls, device IRQs stay SUSPENDED for up to this long,
//     so NAPI is driven inline by the poll syscall with no gro_flush per-packet
//     timer floor. Must be > gro_flush_timeout to take effect.
// The caller passes the live NAPI id from getsockopt(SO_INCOMING_NAPI_ID) on the
// bound AF_XDP socket (the kernel marks it at xsk_bind, before any traffic), so
// there is no ephemeral-id race and no guessing which of the PF's NAPIs is live.
//
// Part 2 — netdev/XSK control-plane helpers shared by both stacks: sysfs NAPI
// deferral knobs, XDP program detach (rtnetlink, `ip link ... xdp off`
// equivalent), ethtool combined=1 steering, and XSK granted-mode readback
// (zero-copy / busy-poll validation — setsockopt return codes only say the
// kernel parsed the request; only a readback says what is in force).
//
#pragma once

#include <fmt/core.h>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include <dirent.h>
#include <fcntl.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <linux/ethtool.h>
#include <linux/genetlink.h>
#include <linux/if_link.h>
#include <linux/if_xdp.h>
#include <linux/netdev.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <linux/sockios.h>

#ifndef AF_XDP
#define AF_XDP 44
#endif
#ifndef SOL_XDP
#define SOL_XDP 283
#endif
#ifndef SO_BUSY_POLL
#define SO_BUSY_POLL 46
#endif
#ifndef SO_PREFER_BUSY_POLL
#define SO_PREFER_BUSY_POLL 69
#endif

namespace napi {

// netdev-genl uAPI constants (linux/netdev.h, kernel >= 6.13). Carried locally
// with their ABI-frozen values so the header builds against OLDER kernel
// headers too (e.g. a workstation on a pre-6.13 toolchain) — uAPI enum values
// never change once released, and enums can't be probed with #ifdef.
inline constexpr std::uint8_t  kCmdNapiGet              = 11;  // NETDEV_CMD_NAPI_GET
inline constexpr std::uint8_t  kCmdNapiSet              = 14;  // NETDEV_CMD_NAPI_SET
inline constexpr std::uint16_t kAttrNapiId              = 2;   // NETDEV_A_NAPI_ID
inline constexpr std::uint16_t kAttrNapiDeferHardIrqs   = 5;   // NETDEV_A_NAPI_DEFER_HARD_IRQS
inline constexpr std::uint16_t kAttrNapiGroFlushTimeout = 6;   // NETDEV_A_NAPI_GRO_FLUSH_TIMEOUT
inline constexpr std::uint16_t kAttrNapiIrqSuspend      = 7;   // NETDEV_A_NAPI_IRQ_SUSPEND_TIMEOUT

namespace detail {

// Append one netlink attribute (header + value, NLA_ALIGN-padded) to buf at off.
// Returns the new offset. Caller guarantees buf is large enough (our messages
// are tiny: a handful of u32 attrs).
template<typename T>
inline std::size_t putAttr(char* buf, std::size_t off, std::uint16_t type, T value) {
  auto* a = reinterpret_cast<nlattr*>(buf + off);
  a->nla_type = type;
  a->nla_len  = static_cast<std::uint16_t>(NLA_HDRLEN + sizeof(T));
  std::memcpy(buf + off + NLA_HDRLEN, &value, sizeof(T));
  return off + NLA_ALIGN(a->nla_len);
}

inline std::size_t putAttrStr(char* buf, std::size_t off, std::uint16_t type,
                              const char* s) {
  const std::size_t len = std::strlen(s) + 1;
  auto* a = reinterpret_cast<nlattr*>(buf + off);
  a->nla_type = type;
  a->nla_len  = static_cast<std::uint16_t>(NLA_HDRLEN + len);
  std::memcpy(buf + off + NLA_HDRLEN, s, len);
  return off + NLA_ALIGN(a->nla_len);
}

// Resolve a generic-netlink family id by name via the controller
// (GENL_ID_CTRL / CTRL_CMD_GETFAMILY). Returns 0 on failure.
inline std::uint16_t resolveFamily(int fd, const char* name) {
  char buf[512]{};
  auto* nlh = reinterpret_cast<nlmsghdr*>(buf);
  nlh->nlmsg_len   = NLMSG_LENGTH(GENL_HDRLEN);
  nlh->nlmsg_type  = GENL_ID_CTRL;
  nlh->nlmsg_flags = NLM_F_REQUEST;
  nlh->nlmsg_seq   = 1;
  auto* gnlh = reinterpret_cast<genlmsghdr*>(NLMSG_DATA(nlh));
  gnlh->cmd     = CTRL_CMD_GETFAMILY;
  gnlh->version = 1;
  std::size_t off = NLMSG_ALIGN(nlh->nlmsg_len);
  off = putAttrStr(buf, off, CTRL_ATTR_FAMILY_NAME, name);
  nlh->nlmsg_len = static_cast<std::uint32_t>(off);

  if (::send(fd, buf, nlh->nlmsg_len, 0) < 0) return 0;

  char rbuf[1024];
  ssize_t n = ::recv(fd, rbuf, sizeof(rbuf), 0);  // NLMSG_NEXT mutates n
  if (n < static_cast<ssize_t>(NLMSG_HDRLEN)) return 0;

  for (auto* rh = reinterpret_cast<nlmsghdr*>(rbuf);
       NLMSG_OK(rh, n); rh = NLMSG_NEXT(rh, n)) {
    if (rh->nlmsg_type == NLMSG_ERROR || rh->nlmsg_type == NLMSG_DONE) return 0;
    // Walk attrs after the genlmsghdr looking for CTRL_ATTR_FAMILY_ID (u16).
    auto* attr = reinterpret_cast<nlattr*>(
        reinterpret_cast<char*>(NLMSG_DATA(rh)) + GENL_HDRLEN);
    int rem = static_cast<int>(rh->nlmsg_len) -
              static_cast<int>(NLMSG_HDRLEN) - static_cast<int>(GENL_HDRLEN);
    for (; rem >= static_cast<int>(NLA_HDRLEN) && rem >= attr->nla_len;
         rem -= NLA_ALIGN(attr->nla_len),
         attr = reinterpret_cast<nlattr*>(reinterpret_cast<char*>(attr) +
                                          NLA_ALIGN(attr->nla_len))) {
      if (attr->nla_type == CTRL_ATTR_FAMILY_ID)
        return *reinterpret_cast<std::uint16_t*>(
            reinterpret_cast<char*>(attr) + NLA_HDRLEN);
    }
  }
  return 0;
}

}  // namespace detail

// Set busy-poll parameters on the NAPI identified by `napiId`. Best-effort:
// returns true on a clean kernel ACK, false otherwise (caller WARNs, never
// aborts). Root required (NETDEV_CMD_NAPI_SET is admin-gated).
//   deferHardIrqs    — empty polls before NAPI re-arms hard IRQs
//   groFlushNs       — repoll-timer delay (ns); idle-transition fallback
//   irqSuspendNs     — IRQs stay suspended this long during busy poll (ns);
//                      MUST be > groFlushNs to have any effect
inline bool setBusyPoll(std::uint32_t napiId,
                        std::uint32_t deferHardIrqs,
                        std::uint64_t groFlushNs,
                        std::uint64_t irqSuspendNs,
                        const char* tag = "AFXDP") {
  const int fd = ::socket(AF_NETLINK, SOCK_RAW, NETLINK_GENERIC);
  if (fd < 0) {
    fmt::print(stderr, "[{}] WARN: netlink socket: {}\n", tag, std::strerror(errno));
    return false;
  }

  const std::uint16_t family = detail::resolveFamily(fd, NETDEV_FAMILY_NAME);
  if (family == 0) {
    fmt::print(stderr, "[{}] WARN: cannot resolve '{}' genl family\n",
               tag, NETDEV_FAMILY_NAME);
    ::close(fd);
    return false;
  }

  char buf[512]{};
  auto* nlh = reinterpret_cast<nlmsghdr*>(buf);
  nlh->nlmsg_len   = NLMSG_LENGTH(GENL_HDRLEN);
  nlh->nlmsg_type  = family;
  nlh->nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;
  nlh->nlmsg_seq   = 2;
  auto* gnlh = reinterpret_cast<genlmsghdr*>(NLMSG_DATA(nlh));
  gnlh->cmd     = kCmdNapiSet;
  gnlh->version = 1;

  std::size_t off = NLMSG_ALIGN(nlh->nlmsg_len);
  off = detail::putAttr<std::uint32_t>(buf, off, kAttrNapiId, napiId);
  off = detail::putAttr<std::uint32_t>(buf, off, kAttrNapiDeferHardIrqs, deferHardIrqs);
  off = detail::putAttr<std::uint64_t>(buf, off, kAttrNapiGroFlushTimeout, groFlushNs);
  off = detail::putAttr<std::uint64_t>(buf, off, kAttrNapiIrqSuspend, irqSuspendNs);
  nlh->nlmsg_len = static_cast<std::uint32_t>(off);

  if (::send(fd, buf, nlh->nlmsg_len, 0) < 0) {
    fmt::print(stderr, "[{}] WARN: napi-set send: {}\n", tag, std::strerror(errno));
    ::close(fd);
    return false;
  }

  char rbuf[1024];
  const ssize_t n = ::recv(fd, rbuf, sizeof(rbuf), 0);
  ::close(fd);
  if (n < static_cast<ssize_t>(NLMSG_HDRLEN)) {
    fmt::print(stderr, "[{}] WARN: napi-set: no ACK\n", tag);
    return false;
  }
  auto* rh = reinterpret_cast<nlmsghdr*>(rbuf);
  if (rh->nlmsg_type == NLMSG_ERROR) {
    auto* err = reinterpret_cast<nlmsgerr*>(NLMSG_DATA(rh));
    if (err->error != 0) {   // 0 == ACK; nonzero == failure
      fmt::print(stderr, "[{}] WARN: napi-set rejected: {}\n",
                 tag, std::strerror(-err->error));
      return false;
    }
  }
  return true;
}

// Read back the live busy-poll parameters of one NAPI via NETDEV_CMD_NAPI_GET —
// the write-then-read-back verification for setBusyPoll(). valid=false means
// the readback itself failed (no netlink, no family, kernel rejected the GET);
// a valid result with different values means the SET did not stick.
struct NapiParams {
  bool          valid{false};
  std::uint32_t deferHardIrqs{0};
  std::uint64_t groFlushNs{0};
  std::uint64_t irqSuspendNs{0};
};

inline NapiParams getBusyPoll(std::uint32_t napiId, const char* tag = "AFXDP") {
  NapiParams out{};

  const int fd = ::socket(AF_NETLINK, SOCK_RAW, NETLINK_GENERIC);
  if (fd < 0) {
    fmt::print(stderr, "[{}] WARN: netlink socket: {}\n", tag, std::strerror(errno));
    return out;
  }
  const std::uint16_t family = detail::resolveFamily(fd, NETDEV_FAMILY_NAME);
  if (family == 0) {
    fmt::print(stderr, "[{}] WARN: cannot resolve '{}' genl family\n",
               tag, NETDEV_FAMILY_NAME);
    ::close(fd);
    return out;
  }

  char buf[256]{};
  auto* nlh = reinterpret_cast<nlmsghdr*>(buf);
  nlh->nlmsg_len   = NLMSG_LENGTH(GENL_HDRLEN);
  nlh->nlmsg_type  = family;
  nlh->nlmsg_flags = NLM_F_REQUEST;
  nlh->nlmsg_seq   = 3;
  auto* gnlh = reinterpret_cast<genlmsghdr*>(NLMSG_DATA(nlh));
  gnlh->cmd     = kCmdNapiGet;
  gnlh->version = 1;
  std::size_t off = NLMSG_ALIGN(nlh->nlmsg_len);
  off = detail::putAttr<std::uint32_t>(buf, off, kAttrNapiId, napiId);
  nlh->nlmsg_len = static_cast<std::uint32_t>(off);

  if (::send(fd, buf, nlh->nlmsg_len, 0) < 0) {
    fmt::print(stderr, "[{}] WARN: napi-get send: {}\n", tag, std::strerror(errno));
    ::close(fd);
    return out;
  }

  char rbuf[2048];
  ssize_t n = ::recv(fd, rbuf, sizeof(rbuf), 0);  // NLMSG_NEXT mutates n
  ::close(fd);
  if (n < static_cast<ssize_t>(NLMSG_HDRLEN)) {
    fmt::print(stderr, "[{}] WARN: napi-get: no reply\n", tag);
    return out;
  }

  for (auto* rh = reinterpret_cast<nlmsghdr*>(rbuf);
       NLMSG_OK(rh, n); rh = NLMSG_NEXT(rh, n)) {
    if (rh->nlmsg_type == NLMSG_ERROR) {
      auto* err = reinterpret_cast<nlmsgerr*>(NLMSG_DATA(rh));
      fmt::print(stderr, "[{}] WARN: napi-get rejected: {}\n",
                 tag, std::strerror(-err->error));
      return out;
    }
    if (rh->nlmsg_type != family) continue;

    auto* attr = reinterpret_cast<nlattr*>(
        reinterpret_cast<char*>(NLMSG_DATA(rh)) + GENL_HDRLEN);
    int rem = static_cast<int>(rh->nlmsg_len) -
              static_cast<int>(NLMSG_HDRLEN) - static_cast<int>(GENL_HDRLEN);
    for (; rem >= static_cast<int>(NLA_HDRLEN) && rem >= attr->nla_len;
         rem -= NLA_ALIGN(attr->nla_len),
         attr = reinterpret_cast<nlattr*>(reinterpret_cast<char*>(attr) +
                                          NLA_ALIGN(attr->nla_len))) {
      const char* payload = reinterpret_cast<char*>(attr) + NLA_HDRLEN;
      // netdev-genl encodes these as NLA_UINT: 4-byte payload when the value
      // fits in u32, 8-byte otherwise. Read exactly what the kernel sent —
      // assuming a fixed u64 runs into the NEXT attribute's header.
      const std::size_t plen =
          static_cast<std::size_t>(attr->nla_len) - NLA_HDRLEN;
      const auto readUint = [&]() -> std::uint64_t {
        if (plen >= 8) { std::uint64_t v; std::memcpy(&v, payload, 8); return v; }
        if (plen >= 4) { std::uint32_t v; std::memcpy(&v, payload, 4); return v; }
        return 0;
      };
      switch (attr->nla_type & NLA_TYPE_MASK) {
        case kAttrNapiDeferHardIrqs:
          out.deferHardIrqs = static_cast<std::uint32_t>(readUint());
          break;
        case kAttrNapiGroFlushTimeout:
          out.groFlushNs = readUint();
          break;
        case kAttrNapiIrqSuspend:
          out.irqSuspendNs = readUint();
          break;
        default: break;
      }
    }
    out.valid = true;
  }
  return out;
}

// ═════════════════════════════════════════════════════════════════════════════
// Part 2 — netdev/XSK control-plane helpers shared by the native AFXDP
// transport and the DPDK net_af_xdp path. Every function takes a `tag` for the
// log prefix so each transport keeps its own voice ("AFXDP" / "DPDK").
// ═════════════════════════════════════════════════════════════════════════════

// Canonical NAPI deferral regime — Config C of the AF_XDP benchmarks
// (docs/Benchmarks.md §3.2): busy-poll suppresses the hardware IRQ for
// kDeferHardIrqs empty polls; kGroFlushTimeoutNs is the re-arm backstop timer.
inline constexpr std::uint32_t kDeferHardIrqs     = 2;
inline constexpr std::uint64_t kGroFlushTimeoutNs = 200'000;

inline bool writeNetdevKnob(const char* ifname, const char* knob,
                            std::uint64_t value, const char* tag) {
  char path[128];
  std::snprintf(path, sizeof(path), "/sys/class/net/%s/%s", ifname, knob);
  const int fd = ::open(path, O_WRONLY | O_CLOEXEC);
  if (fd < 0) {
    fmt::print(stderr, "[{}] WARN: open {}: {}\n", tag, path, std::strerror(errno));
    return false;
  }
  char val[24];
  const int n = std::snprintf(val, sizeof(val), "%llu",
                              static_cast<unsigned long long>(value));
  const bool ok = (::write(fd, val, static_cast<std::size_t>(n)) == n);
  if (!ok)
    fmt::print(stderr, "[{}] WARN: write {}={}: {}\n", tag, path, value,
               std::strerror(errno));
  ::close(fd);
  return ok;
}

inline long readNetdevKnob(const char* ifname, const char* knob) {
  char path[128];
  std::snprintf(path, sizeof(path), "/sys/class/net/%s/%s", ifname, knob);
  const int fd = ::open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0) return -1;
  char buf[32]{};
  const ssize_t n = ::read(fd, buf, sizeof(buf) - 1);
  ::close(fd);
  if (n <= 0) return -1;
  return std::strtol(buf, nullptr, 10);
}

// Netdev-wide NAPI deferral (sysfs). These knobs PERSIST across runs and are
// inherited by newly-created NAPIs, so callers must write them on EVERY init —
// zeros to clear — making the chosen regime deterministic (a stale enable from
// a prior run cannot silently survive).
inline bool applyDeferralSysfs(const char* ifname, std::uint32_t deferHardIrqs,
                               std::uint64_t groFlushNs, const char* tag) {
  const bool a = writeNetdevKnob(ifname, "napi_defer_hard_irqs", deferHardIrqs, tag);
  const bool b = writeNetdevKnob(ifname, "gro_flush_timeout", groFlushNs, tag);
  return a && b;
}

// Detach any XDP program in any mode — `ip link set dev <if> xdp off` as raw
// rtnetlink (RTM_SETLINK with nested IFLA_XDP { IFLA_XDP_FD = -1 [, FLAGS] }),
// so no libbpf dependency. A stale program from a crashed run blocks the next
// attach and mis-routes RX. Best-effort, fired once per mode (DRV, SKB,
// unspecified) like libbpf's bpf_xdp_detach sweep; "nothing attached" errors
// are expected and ignored.
inline void detachXdpProgram(const char* ifname, const char* tag) {
  const unsigned ifindex = ::if_nametoindex(ifname);
  if (ifindex == 0) {
    fmt::print(stderr, "[{}] WARN: {}: no such netdev — XDP detach skipped\n",
               tag, ifname);
    return;
  }
  const int fd = ::socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
  if (fd < 0) {
    fmt::print(stderr, "[{}] WARN: rtnetlink socket: {}\n", tag, std::strerror(errno));
    return;
  }
  for (const std::uint32_t mode : {static_cast<std::uint32_t>(XDP_FLAGS_DRV_MODE),
                                   static_cast<std::uint32_t>(XDP_FLAGS_SKB_MODE),
                                   0u}) {
    struct {
      nlmsghdr  nlh;
      ifinfomsg ifi;
      char      attrs[64];
    } req{};
    req.nlh.nlmsg_len   = NLMSG_LENGTH(sizeof(ifinfomsg));
    req.nlh.nlmsg_type  = RTM_SETLINK;
    req.nlh.nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;
    req.nlh.nlmsg_seq   = 4;
    req.ifi.ifi_family  = AF_UNSPEC;
    req.ifi.ifi_index   = static_cast<int>(ifindex);

    char* base = reinterpret_cast<char*>(&req);
    const std::size_t nestOff = NLMSG_ALIGN(req.nlh.nlmsg_len);
    auto* nest = reinterpret_cast<nlattr*>(base + nestOff);
    nest->nla_type = IFLA_XDP;
    std::size_t off = nestOff + NLA_HDRLEN;
    off = detail::putAttr<std::int32_t>(base, off, IFLA_XDP_FD, -1);
    if (mode != 0)
      off = detail::putAttr<std::uint32_t>(base, off, IFLA_XDP_FLAGS, mode);
    nest->nla_len     = static_cast<std::uint16_t>(off - nestOff);
    req.nlh.nlmsg_len = static_cast<std::uint32_t>(off);

    if (::send(fd, &req, req.nlh.nlmsg_len, 0) >= 0) {
      char rbuf[256];
      (void)::recv(fd, rbuf, sizeof(rbuf), 0);   // consume ACK/err; best-effort
    }
  }
  ::close(fd);
  fmt::print(stderr, "[{}] {}: cleared any pre-existing XDP program "
                     "(`ip link ... xdp off` equivalent)\n", tag, ifname);
}

// Collapse the NIC to ONE combined channel (== `ethtool -L <if> combined 1`) so
// all RX lands on queue 0 — where both the net_af_xdp PMD and the native XSK
// bind. No-op (and no link bounce) if already 1. On Changed the link bounces:
// the caller MUST re-wait for carrier (AFXDP::waitForCarrier / DPDK::waitLink).
enum class CombinedResult { AlreadyOne, Changed, NoChannels, Failed };

inline CombinedResult ensureCombinedOne(const char* ifname, const char* tag) {
  const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) {
    fmt::print(stderr, "[{}] WARN: steering socket: {}\n", tag, std::strerror(errno));
    return CombinedResult::Failed;
  }
  auto ethtoolIoctl = [&](void* cmd) {
    ifreq ifr{};
    std::strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);
    ifr.ifr_data = static_cast<char*>(cmd);
    return ::ioctl(fd, SIOCETHTOOL, &ifr) == 0;
  };

  ethtool_channels ch{};
  ch.cmd = ETHTOOL_GCHANNELS;
  if (!ethtoolIoctl(&ch) || ch.max_combined == 0) {
    ::close(fd);
    fmt::print(stderr, "[{}] WARN: {} has no combined channels (driver lacks "
                       "`ethtool -L`); cannot guarantee single-queue steering\n",
               tag, ifname);
    return CombinedResult::NoChannels;
  }
  if (ch.combined_count == 1) {
    ::close(fd);
    fmt::print(stderr, "[{}] OK: {} already at 1 RX queue (combined=1) — "
                       "no change, no link bounce\n", tag, ifname);
    return CombinedResult::AlreadyOne;
  }

  ch.cmd            = ETHTOOL_SCHANNELS;
  ch.combined_count = 1;
  ethtoolIoctl(&ch);

  ethtool_channels rb{};
  rb.cmd = ETHTOOL_GCHANNELS;
  const bool ok = ethtoolIoctl(&rb) && rb.combined_count == 1;
  ::close(fd);

  if (!ok) {
    fmt::print(stderr, "[{}] FAIL: {} combined={} (want 1) — RX may not reach "
                       "the XSK; run: sudo ethtool -L {} combined 1\n",
               tag, ifname, rb.combined_count, ifname);
    return CombinedResult::Failed;
  }
  fmt::print(stderr, "[{}] {} steered to 1 RX queue (combined=1) — link "
                     "bounced, caller must re-wait carrier\n", tag, ifname);
  return CombinedResult::Changed;
}

// Read back what the kernel ACTUALLY granted an AF_XDP socket. NOTE:
// SO_BUSY_POLL_BUDGET is SET-ONLY in the kernel (no getsockopt case) and can
// never be read back — a clean setsockopt return is all the confirmation there is.
struct XskModes {
  bool          valid{false};          // XDP_OPTIONS readback succeeded
  bool          zeroCopy{false};
  std::uint32_t xdpOptionsFlags{0};
  int           busyPollUs{-1};
  int           preferBusyPoll{-1};
};

inline XskModes readXskModes(int fd) {
  XskModes m{};
  xdp_options xo{};
  socklen_t olen = sizeof(xo);
  if (::getsockopt(fd, SOL_XDP, XDP_OPTIONS, &xo, &olen) == 0) {
    m.valid           = true;
    m.xdpOptionsFlags = xo.flags;
    m.zeroCopy        = (xo.flags & XDP_OPTIONS_ZEROCOPY) != 0;
  }
  socklen_t il = sizeof(m.busyPollUs);
  (void)::getsockopt(fd, SOL_SOCKET, SO_BUSY_POLL, &m.busyPollUs, &il);
  il = sizeof(m.preferBusyPoll);
  (void)::getsockopt(fd, SOL_SOCKET, SO_PREFER_BUSY_POLL, &m.preferBusyPoll, &il);
  return m;
}

// Process-wide XSK validation for stacks that do NOT own the socket fd (the
// DPDK net_af_xdp PMD creates it internally): scan /proc/self/fd for AF_XDP
// sockets and read back each one's granted modes, plus the netdev deferral
// knobs. AF_XDP sockets do not answer getsockname (sock_no_getname in
// net/xdp/xsk.c), so fds are attributed by CREATION ORDER instead: each call
// reports only sockets that appeared since the previous call — callers init
// one port at a time, sequentially.
inline void printXskProcessValidation(const char* ifname, bool deferralExpected,
                                      const char* tag) {
  const long defer = readNetdevKnob(ifname, "napi_defer_hard_irqs");
  const long gro   = readNetdevKnob(ifname, "gro_flush_timeout");
  const bool deferOn = (defer > 0 && gro > 0);
  fmt::print(stderr, "[{}] {}: deferral: napi_defer_hard_irqs={} "
                     "gro_flush_timeout={}ns -> {}{}\n",
             tag, ifname, defer, gro, deferOn ? "ON" : "OFF",
             (deferOn == deferralExpected) ? "" : "  <-- MISMATCH vs requested config");

  static std::vector<long> reported;
  DIR* dir = ::opendir("/proc/self/fd");
  if (!dir) return;
  bool found = false;
  while (dirent* de = ::readdir(dir)) {
    char* end = nullptr;
    const long fd = std::strtol(de->d_name, &end, 10);
    if (end == de->d_name) continue;
    int domain = 0;
    socklen_t len = sizeof(domain);
    if (::getsockopt(static_cast<int>(fd), SOL_SOCKET, SO_DOMAIN, &domain, &len) != 0)
      continue;
    if (domain != AF_XDP) continue;
    bool seen = false;
    for (const long r : reported)
      if (r == fd) { seen = true; break; }
    if (seen) continue;
    reported.push_back(fd);
    found = true;

    const XskModes m = readXskModes(static_cast<int>(fd));
    const bool good = m.zeroCopy && m.preferBusyPoll > 0 && m.busyPollUs > 0;
    fmt::print(stderr, "[{}] {}: XSK fd={}: ZERO-COPY={} prefer_busy_poll={} "
                       "busy_poll={}us (XDP_OPTIONS=0x{:x}; budget is set-only) {}\n",
               tag, ifname, fd, m.zeroCopy ? "YES" : "NO(copy-mode!)",
               m.preferBusyPoll, m.busyPollUs, m.xdpOptionsFlags,
               good ? "[OK: real AF_XDP ZC + busy-poll]"
                    : "<-- NOT the intended ZC+busy-poll mode");
  }
  ::closedir(dir);
  if (!found)
    fmt::print(stderr, "[{}] {}: WARN: no (new) AF_XDP socket found in this "
                       "process — validation could not run\n", tag, ifname);
}

}  // namespace napi
