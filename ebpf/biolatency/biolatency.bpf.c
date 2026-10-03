// SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause
/* biolatency.bpf.c - Block I/O latency histogram (CO-RE).
 *
 * Traces block I/O issue and completion, computing per-request latency and
 * recording it into log2-bucketed histograms, one per CPU (aggregated in
 * userspace). Works across devices; the device major:minor can optionally be
 * filtered from userspace via the filter map.
 *
 * Build with: make (uses bpftool gen to produce vmlinux.h if absent).
 */
#include <vmlinux.h>
#include <bpf/bpf_core_read.h>
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>

char LICENSE[] SEC("license") = "Dual BSD/GPL";

#define MAX_BUCKETS 32 /* log2 microseconds: 0..2^31 us */

/* In-flight request tracking, keyed by request pointer. */
struct {
	__uint(type, BPF_MAP_TYPE_HASH);
	__uint(max_entries, 16384);
	__type(key, __u64); /* (u64)struct request * */
	__type(value, __u64); /* start timestamp (ns) */
} starts SEC(".maps");

/* Per-CPU latency histograms, bucket = log2(latency_us). */
struct {
	__uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
	__uint(max_entries, MAX_BUCKETS);
	__type(key, __u32);
	__type(value, __u64);
} lat_hist SEC(".maps");

/* Optional device filter: if a key exists, only that major:minor is traced. */
struct {
	__uint(type, BPF_MAP_TYPE_HASH);
	__uint(max_entries, 1);
	__type(key, __u32); /* (major << 8) | minor is not used; dev_t encoded */
	__type(value, __u8);
} filter_map SEC(".maps");

/* Number of requests issued/completed, for sanity checks in userspace. */
struct {
	__uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
	__uint(max_entries, 2); /* [0]=issued [1]=completed */
	__type(key, __u32);
	__type(value, __u64);
} counts SEC(".maps");

static __always_inline __u32 dev_key(struct request *rq)
{
	struct gendisk *disk = BPF_CORE_READ(rq, q, disk);
	__u32 major = BPF_CORE_READ(disk, major);
	__u16 first_minor = BPF_CORE_READ(disk, first_minor);

	return (major << 16) | first_minor;
}

static __always_inline int allowed(struct request *rq)
{
	__u32 key;
	__u32 slot = 0;
	__u8 *v;

	if (bpf_map_lookup_elem(&filter_map, &slot) == NULL)
		return 1; /* no filter installed -> trace everything */

	key = dev_key(rq);
	v = bpf_map_lookup_elem(&filter_map, &key);
	return v != NULL;
}

static __always_inline void count_inc(__u32 slot)
{
	__u64 *c = bpf_map_lookup_elem(&counts, &slot);

	if (c)
		__sync_fetch_and_add(c, 1);
}

SEC("tracepoint/block/block_rq_issue")
int trace_block_rq_issue(struct trace_event_raw_block_rq_issue *ctx)
{
	__u64 ts = bpf_ktime_get_ns();
	__u64 *p;
	__u64 req = (__u64)ctx->rq;

	if (!allowed((struct request *)(unsigned long)req))
		return 0;

	p = bpf_map_lookup_elem(&starts, &req);
	if (p)
		*p = ts;
	else
		bpf_map_update_elem(&starts, &req, &ts, BPF_ANY);

	count_inc(0);
	return 0;
}

SEC("tracepoint/block/block_rq_complete")
int trace_block_rq_complete(struct trace_event_raw_block_rq_complete *ctx)
{
	__u64 req = (__u64)ctx->rq;
	__u64 *start, lat_ns, lat_us;
	__u32 bucket, slot = 1;
	__u64 *h;

	start = bpf_map_lookup_elem(&starts, &req);
	if (!start)
		return 0;

	lat_ns = bpf_ktime_get_ns() - *start;
	bpf_map_delete_elem(&starts, &req);
	lat_us = lat_ns / 1000;

	/* Bucket by integer log2 of microseconds. */
	bucket = lat_us ? (63 - __builtin_clzll(lat_us)) : 0;
	if (bucket >= MAX_BUCKETS)
		bucket = MAX_BUCKETS - 1;

	h = bpf_map_lookup_elem(&lat_hist, &bucket);
	if (h)
		__sync_fetch_and_add(h, 1);

	count_inc(slot);
	return 0;
}

char _license[] SEC("license") = "GPL";
