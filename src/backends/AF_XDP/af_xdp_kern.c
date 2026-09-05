/* SPDX-License-Identifier: GPL-2.0 */
/*
 * AF_XDP kernel-side program — redirect ONLY our custom L2 timing frames
 * (EtherType 0x88B5) to the AF_XDP socket bound to this queue. Everything else
 * (ARP, IPv6 ND, LLDP, and assorted kernel chatter) is XDP_PASS'd to the normal
 * stack. Filtering in XDP keeps the XSK ring clean so userspace only ever sees
 * timing frames — no kernel noise to fluff through. Loaded via
 * xdp_program__open_file + xdp_program__attach (xdpsock style).
 */

#include <linux/bpf.h>
#include <linux/if_ether.h>

#include <bpf/bpf_endian.h>
#include <bpf/bpf_helpers.h>

#define ABT_ETHERTYPE 0x88B5 /* our timing-frame EtherType */

struct {
    __uint(type, BPF_MAP_TYPE_XSKMAP);
    __type(key, int);
    __type(value, int);
    __uint(max_entries, 64);
} xsks_map SEC(".maps");

SEC("xdp")

int xdp_sock_prog(struct xdp_md* ctx) {
    void*          data     = (void*)(long)ctx->data;
    void*          data_end = (void*)(long)ctx->data_end;
    struct ethhdr* eth      = data;

    /* Bounds-check the Ethernet header — mandatory for the verifier. */
    if ((void*)(eth + 1) > data_end) {
        return XDP_PASS;
    }

    /* Only our timing EtherType goes to userspace; the rest stays in-kernel. */
    if (eth->h_proto != bpf_htons(ABT_ETHERTYPE)) {
        return XDP_PASS;
    }

    /* Redirect to the AF_XDP socket bound to this queue. The map lookup guard
     * keeps us correct if a queue has no socket (then -> kernel). */
    int index = ctx->rx_queue_index;
    if (bpf_map_lookup_elem(&xsks_map, &index)) {
        return bpf_redirect_map(&xsks_map, index, 0);
    }

    return XDP_PASS;
}

char _license[] SEC("license") = "GPL";
