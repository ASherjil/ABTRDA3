/* SPDX-License-Identifier: GPL-2.0 */
/*
 * AF_XDP kernel-side program — only redirect 0x88B5 traffic.
 * Loaded via xdp_program__open_file + xdp_program__attach (xdpsock style).
 */

#include <linux/bpf.h>
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_endian.h>
#include <linux/if_ether.h>

struct {
	__uint(type, BPF_MAP_TYPE_XSKMAP);
	__type(key, int);
	__type(value, int);
	__uint(max_entries, 64);
} xsks_map SEC(".maps");

SEC("xdp")
int xdp_sock_prog(struct xdp_md *ctx)
{
	void *data_end = (void *)(long)ctx->data_end;
	void *data     = (void *)(long)ctx->data;
	struct ethhdr *eth = data;

	/* Bounds check */
	if (eth + 1 > (struct ethhdr *)data_end)
		return XDP_PASS;

	/* Only redirect our custom ethertype; everything else → kernel */
	if (eth->h_proto != bpf_htons(0x88B5))
		return XDP_PASS;

	/* Redirect to AF_XDP socket on this queue */
	int index = ctx->rx_queue_index;
	if (bpf_map_lookup_elem(&xsks_map, &index))
		return bpf_redirect_map(&xsks_map, index, 0);

	return XDP_PASS;
}

char _license[] SEC("license") = "GPL";
