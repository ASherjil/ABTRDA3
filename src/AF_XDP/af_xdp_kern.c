/* SPDX-License-Identifier: GPL-2.0 */
/*
 * AF_XDP kernel-side program — redirect ALL traffic on the queue to userspace.
 * On a dedicated direct-LAN interface there is nothing else on the wire, so no
 * ethertype filter is needed; redirecting everything is simpler and avoids any
 * header-parse cost in the XDP path. Loaded via xdp_program__open_file +
 * xdp_program__attach (xdpsock style).
 */

#include <linux/bpf.h>
#include <bpf/bpf_helpers.h>

struct {
	__uint(type, BPF_MAP_TYPE_XSKMAP);
	__type(key, int);
	__type(value, int);
	__uint(max_entries, 64);
} xsks_map SEC(".maps");

SEC("xdp")
int xdp_sock_prog(struct xdp_md *ctx)
{
	/* Redirect to the AF_XDP socket bound to this queue. The map lookup
	 * guard keeps us correct if a queue has no socket (then → kernel). */
	int index = ctx->rx_queue_index;
	if (bpf_map_lookup_elem(&xsks_map, &index))
		return bpf_redirect_map(&xsks_map, index, 0);

	return XDP_PASS;
}

char _license[] SEC("license") = "GPL";
