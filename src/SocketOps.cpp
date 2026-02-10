//
// Created by asherjil on 2/9/26.
//

#include "SocketOps.hpp"
#include <sys/socket.h>
#include <sys/mman.h>
#include <linux/if_packet.h>
#include <linux/net_tstamp.h>
#include <net/if.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <system_error>
#include <utility>


SocketOps::SocketOps(const RingConfig& ringConfig) {

  // Step 1: Create the packet socket
  m_fd = ::socket(AF_PACKET, SOCK_RAW, htons(ringConfig.protocol));
  if (m_fd < 0) {
    throw std::system_error(errno, std::generic_category(), "socket(AF_PACKET)");
  }

  // Step 2: Set TPACKET_V3
  int version = TPACKET_V3;
  if (::setsockopt(m_fd, SOL_PACKET, PACKET_VERSION, &version, sizeof(version)) < 0) {
    throw std::system_error(errno, std::generic_category(), "PACKET_VERSION");
  }

  // Step 3: Set the RX/TX ring: one frame per block
  tpacket_req3 req{};
  req.tp_block_size = ringConfig.blockSize;
  req.tp_frame_size = ringConfig.blockSize; // frame is the same size as the block
  req.tp_block_nr   = ringConfig.blockNumber;
  req.tp_frame_nr   = ringConfig.blockNumber;
  req.tp_retire_blk_tov   = 0;
  req.tp_sizeof_priv      = 0;
  req.tp_feature_req_word = 0;

  // Setup the TX/RX ring with the above definiations
  int ringOpt = (ringConfig.direction == RingDirection::TX) ? PACKET_TX_RING : PACKET_RX_RING;
  if (::setsockopt(m_fd, SOL_PACKET, ringOpt, &req, sizeof(req)) < 0) {
    throw std::system_error(errno, std::generic_category(),
	(ringConfig.direction == RingDirection::TX) ? "PACKET_TX_RING" : "PACKET_RX_RING");
  }

  // Step 4: Create a memory mapped space
  m_ringSize    = static_cast<std::size_t>(ringConfig.blockSize) * ringConfig.blockNumber;
  m_ringAddress = ::mmap(
    nullptr,
    m_ringSize,
    PROT_READ | PROT_WRITE,
    MAP_SHARED | MAP_LOCKED,
    m_fd,
    0);

  if (m_ringAddress == MAP_FAILED) {
    throw std::system_error(errno, std::generic_category(), "mmap ring");
  }

  // Step 5: Bind to the interface
  unsigned ifindex = ::if_nametoindex(ringConfig.interface);
  if (ifindex == 0) {
    throw std::system_error(errno, std::generic_category(), "if_nametoindex");
  }

  sockaddr_ll sll{};
  sll.sll_family   = AF_PACKET;
  sll.sll_protocol = htons(ringConfig.protocol);
  sll.sll_ifindex  = static_cast<int>(ifindex);

  if (::bind(m_fd, reinterpret_cast<sockaddr*>(&sll), sizeof(sll)) < 0) {
    throw std::system_error(errno, std::generic_category(), "bind");
  }

  // Step 6: Bypass the qdisc -> straight to the NIC driver

  // Step 6a: TX- bypass qdisc
  if (ringConfig.qdiscBypass && ringConfig.direction == RingDirection::TX) {
    int val = 1;
    if (::setsockopt(m_fd, SOL_PACKET, PACKET_QDISC_BYPASS, &val, sizeof(val)) < 0) {
      throw std::system_error(errno, std::generic_category(), "PACKET_QDISC_BYPASS");
    }
  }

 // Step 6b: RX- hardware timestamping (best-effort, not all NICs support it)
  if(ringConfig.hwTimeStamp && ringConfig.direction == RingDirection::RX){
	int val = SOF_TIMESTAMPING_RAW_HARDWARE | SOF_TIMESTAMPING_RX_HARDWARE;
	::setsockopt(m_fd, SOL_PACKET, PACKET_TIMESTAMP, &val, sizeof(val));
  }

}

SocketOps::SocketOps(SocketOps&& other) noexcept
	: m_fd(std::exchange(other.m_fd, -1)),
	  m_ringAddress(std::exchange(other.m_ringAddress, nullptr)),
    m_ringSize{std::exchange(other.m_ringSize, 0)}{}


SocketOps& SocketOps::operator=(SocketOps&& other) noexcept{
	if (this != &other){
	  // First unmap and close the opened connection on (this->) to make it easier to debug
      unmapRing(this->m_ringAddress, this->m_ringSize);
	  if (this->m_fd >= 0) {
	    ::close(this->m_fd);
	  }

	  m_fd          = std::exchange(other.m_fd, -1);
	  m_ringAddress = std::exchange(other.m_ringAddress, nullptr);
	  m_ringSize    = std::exchange(other.m_ringSize, 0);
	}

  return *this;
}

SocketOps::~SocketOps() {
  // Securly unmap and close the file descriptor
  unmapRing(m_ringAddress, m_ringSize);
  if (m_fd >= 0) {
    ::close(m_fd);
  }
}

void SocketOps::unmapRing(void* ptr, std::size_t ringSize) const noexcept {
  if (ptr && ptr != MAP_FAILED) {
    ::munmap(ptr, ringSize);
  }
}