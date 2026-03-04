// SPDX-License-Identifier: GPL-2.0
//
// XDP EtherType filter for ABTRDA3 AF_XDP transport
//
// Safety: Default action is XDP_PASS — all traffic continues to kernel stack.
// Only packets matching target_ethertype are redirected to AF_XDP socket.
// NFS, SSH, and all other traffic is completely unaffected.

#include <linux/bpf.h>
#include <linux/if_ether.h>
#include "bpf_helpers.h"
#include "bpf_endian.h"
volatile const __u16 target_ethertype = 0x88B5;

// XSKMAP - kernel populates it when an AF_XDP socket binds to a queue.
// bpf_redirect_map() looks up ctx->rx_queue_index to find the right socket.
struct {
  __uint(type, BPF_MAP_TYPE_XSKMAP);
  __uint(max_entries, 64);
  __type(key, __u32);
  __type(value, __u32);
} xsks_map SEC(".maps");

SEC("xdp")
int xdp_filter_ethertype(struct xdp_md* ctx) {
  void* data = (void*)(long)ctx->data;
  void* data_end = (void*)(long)ctx->data_end;

  // Bounds check - REQUIRED by BPF verifier
  struct ethhdr* eth = data;
  if ((void*)(eth + 1) > data_end) {
    return XDP_PASS;
  }

  // Important check for NFS traffic(NFS/SSH)
  if (eth->h_proto != bpf_htons(target_ethertype)) {
    return XDP_PASS;
  }

  // Redirect to AF_XDP socket on this queue, (third argument pass it to the kernel if no socket is bound)
  return bpf_redirect_map(&xsks_map, ctx->rx_queue_index, XDP_PASS);
}

char _license[] SEC("license") = "GPL";