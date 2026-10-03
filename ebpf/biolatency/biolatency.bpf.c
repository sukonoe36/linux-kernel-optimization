// SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause
/* biolatency.bpf.c - Block I/O latency histogram (CO-RE).
 *
 * Traces the start and completion of block requests, computes the per-request
 * latency and records it in log2-bucketed histograms, one per CPU (aggregated
 * in userspace). All devices are traced unless a single major:minor is
 * installed in the filter map from userspace.
 *
 * kprobes on blk_account_io_start/blk_account_io_done are used instead of the
 * block/block_rq_issue and block/block_rq_complete tracepoints because the
 * tracepoints' generated context types (trace_event_raw_*) are not part of BTF
 * and therefore absent from vmlinux.h. The kprobe arguments are plain
 * `struct request *` pointers, which CO-RE relocates safely.
 *
 * Build with: make (generates vmlinux.h with bpftool when it is missing).
 */
#include <vmlinux.h>
#include <bpf/bpf_core_read.h>
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>

char LICENSE[] SEC("license") = "Dual BSD/GPL";

#define MAX_BUCKETS 32 /* log2 microseconds: 0..2^31 us */

/* In-flight requests, keyed by (u64)struct request *. Value: start time in ns. */
struct {
	__uint(type, BPF_MAP_TYPE_HASH);
	__uint(max_entries, 16384);
	__type(key, __u64);
	__type(value, __u64);
} starts SEC(".maps");

/* Per-CPU latency histograms; bucket index = floor(log2(latency_us)). */
struct {
	__uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
	__uint(max_entries, MAX_BUCKETS);
	__type(key, __u32);
	__type(value, __u64);
} lat_hist SEC(".maps");

/* Optional device filter. Slot 0 is the "filter installed" marker; a key of
 * (major << 16) | first_minor selects the single device to trace. */
struct {
	__uint(type, BPF_MAP_TYPE_HASH);
	__uint(max_entries, 1024);
	__type(key, __u32);
	__type(value, __u8);
} filter_map SEC(".maps");

/* Counters for sanity checks in userspace: [0]=started [1]=completed. */
struct {
	__uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
	__uint(max_entries, 2);
	__type(key, __u32);
	__type(value, __u64);
} counts SEC(".maps");

/* filter_active reports whether userspace installed a device filter. */
static __always_inline int filter_active(void)
{
	__u32 slot = 0;

	return bpf_map_lookup_elem(&filter_map, &slot) != NULL;
}

/* device_key encodes major:minor the same way userspace does. */
static __always_inline __u32 device_key(struct request *rq)
{
	struct gendisk *disk = BPF_CORE_READ(rq, q, disk);
	__u32 major, minor;

	if (!disk)
		return 0;

	major = BPF_CORE_READ(disk, major);
	minor = BPF_CORE_READ(disk, first_minor);
	return (major << 16) | (minor & 0xffff);
}

/* allowed returns 1 when the request should be traced. */
static __always_inline int allowed(struct request *rq)
{
	__u32 key;

	if (!filter_active())
		return 1;

	key = device_key(rq);
	return bpf_map_lookup_elem(&filter_map, &key) != NULL;
}

static __always_inline void count_inc(__u32 slot)
{
	__u64 *c = bpf_map_lookup_elem(&counts, &slot);

	if (c)
		__sync_fetch_and_add(c, 1);
}

SEC("kprobe/blk_account_io_start")
int BPF_KPROBE(trace_io_start, struct request *rq)
{
	__u64 key, ts, *p;

	if (!rq || !allowed(rq))
		return 0;

	ts = bpf_ktime_get_ns();
	key = (__u64)rq;

	p = bpf_map_lookup_elem(&starts, &key);
	if (p)
		*p = ts;
	else
		bpf_map_update_elem(&starts, &key, &ts, BPF_ANY);

	count_inc(0);
	return 0;
}

SEC("kprobe/blk_account_io_done")
int BPF_KPROBE(trace_io_done, struct request *rq)
{
	__u64 key, *start, lat_ns, lat_us, *h;
	__u32 bucket, slot = 1;

	if (!rq)
		return 0;

	key = (__u64)rq;
	start = bpf_map_lookup_elem(&starts, &key);
	if (!start)
		return 0;

	lat_ns = bpf_ktime_get_ns() - *start;
	bpf_map_delete_elem(&starts, &key);

	lat_us = lat_ns / 1000;
	/* Bucket by integer log2 of the microsecond latency. */
	bucket = lat_us ? (__u32)(63 - __builtin_clzll(lat_us)) : 0;
	if (bucket >= MAX_BUCKETS)
		bucket = MAX_BUCKETS - 1;

	h = bpf_map_lookup_elem(&lat_hist, &bucket);
	if (h)
		__sync_fetch_and_add(h, 1);

	count_inc(slot);
	return 0;
}
