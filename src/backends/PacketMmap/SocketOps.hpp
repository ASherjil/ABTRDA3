//
// Created by asherjil on 2/9/26.
//

#ifndef ABTRDA3_SOCKETOPS_H
#define ABTRDA3_SOCKETOPS_H

#include <cstddef>
#include <cstdint>
#include <linux/if_ether.h>
#include <linux/if_packet.h>

enum class RingDirection : std::uint8_t {TX, RX};

struct RingConfig {
  const char* interface; // e.g "eth0"
  RingDirection direction   = RingDirection::TX;
  std::uint32_t blockSize   = 4096; // each frame fills the whole block
  std::uint32_t blockNumber = 64; // ring depth
  std::uint16_t protocol    = ETH_P_ALL;
  int packetVersion         = TPACKET_V2; // default to V2 but V3 can also be used
  bool hwTimeStamp          = true;
  bool qdiscBypass          = true; // PACKET_QDISC_BYPASS
};

struct SocketOps {
  explicit SocketOps(const RingConfig& ringConfig);
  SocketOps(const SocketOps&) = delete;
  SocketOps(SocketOps&& other) noexcept;

  SocketOps& operator=(const SocketOps&) = delete;
  SocketOps& operator=(SocketOps&& other) noexcept;

  ~SocketOps();

  void unmapRing(void* ptr, std::size_t ringSize) const noexcept;

  int m_fd{};
  void* m_ringAddress{nullptr};
  std::size_t m_ringSize{};
};



#endif //ABTRDA3_SOCKETOPS_H
