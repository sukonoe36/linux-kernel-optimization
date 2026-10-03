// SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause
/* tcp_retrans.bpf.c - Trace TCP retransmissions per destination address.
 *
 * CO-RE (Compile Once - Run Everywhere) BPF program attached to a kprobe on
 * tcp_retransmit_skb. Counts retransmits keyed by the remote (destination)
 * address of the socket, so userspace can report which peers are suffering
 * packet loss.
 *
 * A kprobe is used instead of the tcp/tcp_retransmit_skb tracepoint because the
 * tracepoint's generated context type (trace_event_raw_tcp_retransmit_skb) is
 * not part of BTF and therefore absent from vmlinux.h, while the kprobe argument
 * is a plain `struct sock *` that CO-RE can relocate safely across kernels.
 *
 * Build with: make (generates vmlinux.h with bpftool when it is missing).
 */
#include <vmlinux.h>
#include <bpf/bpf_core_read.h>
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>

char LICENSE[] SEC("license") = "Dual BSD/GPL";

#ifndef AF_INET
#define AF_INET 2
#endif
#ifndef AF_INET6
#define AF_INET6 10
#endif

/* Key: remote address (IPv4 value or full IPv6). Value: retransmit count. */
struct dest_key {
	__u32 family; /* AF_INET or AF_INET6 */
	__u32 pad;
	union {
		__u32 addr4;
		__u8 addr6[16];
	};
};

struct {
	__uint(type, BPF_MAP_TYPE_HASH);
	__uint(max_entries, 10240);
	__type(key, struct dest_key);
	__type(value, __u64);
} retrans_map SEC(".maps");

/* Per-CPU grand total so userspace can cross-check per-destination sums. */
struct {
	__uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
	__uint(max_entries, 1);
	__type(key, __u32);
	__type(value, __u64);
} totals_map SEC(".maps");

SEC("kprobe/tcp_retransmit_skb")
int BPF_KPROBE(trace_tcp_retransmit_skb, struct sock *sk)
{
	struct dest_key key = {};
	__u64 *val, init = 1, *cnt;
	__u32 slot = 0;
	__u16 family;

	if (!sk)
		return 0;

	family = BPF_CORE_READ(sk, __sk_common.skc_family);
	if (family == AF_INET) {
		key.family = AF_INET;
		key.addr4 = BPF_CORE_READ(sk, __sk_common.skc_daddr);
	} else if (family == AF_INET6) {
		key.family = AF_INET6;
		BPF_CORE_READ_INTO(&key.addr6, sk,
				   __sk_common.skc_v6_daddr.in6_u.u6_addr8);
	} else {
		return 0;
	}

	val = bpf_map_lookup_elem(&retrans_map, &key);
	if (val)
		__sync_fetch_and_add(val, 1);
	else
		bpf_map_update_elem(&retrans_map, &key, &init, BPF_NOEXIST);

	cnt = bpf_map_lookup_elem(&totals_map, &slot);
	if (cnt)
		__sync_fetch_and_add(cnt, 1);

	return 0;
}
