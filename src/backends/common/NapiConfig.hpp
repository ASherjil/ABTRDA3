//
// NapiConfig — set per-NAPI busy-poll parameters via the netdev generic-netlink
// family (NETDEV_CMD_NAPI_SET), hand-rolled over a raw AF_NETLINK socket so we
// pull in no libnl/libmnl link dependency (matches the header-only transport
// style). Two knobs the kernel only exposes through netlink, not sysfs:
//   * defer-hard-irqs / gro-flush-timeout  — also in sysfs, but set here too so
//     one call covers everything on the live NAPI;
//   * irq-suspend-timeout (kernel 6.13+)   — netlink ONLY. THIS is the lever:
//     while the app busy-polls, device IRQs stay SUSPENDED for up to this long,
//     so NAPI is driven inline by the poll syscall with no gro_flush per-packet
//     timer floor. Must be > gro_flush_timeout to take effect.
//
// The caller passes the live NAPI id from getsockopt(SO_INCOMING_NAPI_ID) on the
// bound AF_XDP socket (the kernel marks it at xsk_bind, before any traffic), so
// there is no ephemeral-id race and no guessing which of the PF's NAPIs is live.
//
#pragma once

#include <fmt/core.h>

#include <cerrno>
#include <cstdint>
#include <cstring>

#include <linux/genetlink.h>
#include <linux/netdev.h>
#include <linux/netlink.h>
#include <sys/socket.h>
#include <unistd.h>

namespace napi {

// netdev-genl uAPI constants (linux/netdev.h, kernel >= 6.13). Carried locally
// with their ABI-frozen values so the header builds against OLDER kernel
// headers too (e.g. a workstation on a pre-6.13 toolchain) — uAPI enum values
// never change once released, and enums can't be probed with #ifdef.
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
              static_cast<int>(NLMSG_HDRLEN) - GENL_HDRLEN;
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

}  // namespace napi
