// SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause
/* biolatency.c - Userspace loader for the block I/O latency BPF program.
 *
 * Loads biolatency.bpf.o, polls the per-CPU log2 latency histogram and renders
 * an ASCII histogram, optionally restricted to a single block device via
 * --device MAJOR:MINOR. Values are microseconds, bucketed by floor(log2(us)).
 *
 * Usage: sudo ./biolatency [-i seconds] [--device MAJOR:MINOR] [--object PATH]
 */
#include <bpf/bpf.h>
#include <bpf/libbpf.h>
#include <errno.h>
#include <getopt.h>
#include <inttypes.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAX_BUCKETS 32 /* must match biolatency.bpf.c */

static volatile sig_atomic_t stop;

static void on_signal(int signo)
{
	(void)signo;
	stop = 1;
}

static void usage(const char *prog)
{
	fprintf(stderr,
		"Usage: %s [options]\n"
		"\n"
		"Print a block I/O latency histogram (microseconds, log2 buckets).\n"
		"\n"
		"Options:\n"
		"  -i, --interval SECS      Poll interval in seconds (default: 1)\n"
		"  -d, --device MAJ:MIN     Restrict to one block device\n"
		"      --object PATH        Path to the BPF object file\n"
		"                           (default: biolatency.bpf.o next to the binary)\n"
		"  -h, --help               Show this help and exit\n",
		prog);
}

static int parse_device(const char *arg, __u32 *key)
{
	unsigned int major, minor;
	char extra;

	if (sscanf(arg, "%u:%u%c", &major, &minor, &extra) != 2) {
		fprintf(stderr, "error: invalid --device '%s' (expected MAJ:MIN)\n", arg);
		return -1;
	}
	if (major > 0xffff || minor > 0xffff) {
		fprintf(stderr, "error: --device values must fit in 16 bits\n");
		return -1;
	}
	/* Match the encoding used by the BPF side: (major << 16) | minor. */
	*key = (major << 16) | minor;
	return 0;
}

static int parse_interval(const char *value, long *out)
{
	char *end = NULL;
	long v;

	errno = 0;
	v = strtol(value, &end, 10);
	if (errno != 0 || end == value || *end != '\0' || v <= 0) {
		fprintf(stderr, "error: invalid interval '%s'\n", value);
		return -1;
	}
	*out = v;
	return 0;
}

static const char *default_object_path(char *buf, size_t len, const char *argv0)
{
	const char *slash, *dir;
	size_t dirlen;

	if (!argv0)
		return NULL;
	slash = strrchr(argv0, '/');
	dir = slash ? argv0 : ".";
	dirlen = slash ? (size_t)(slash - argv0) + 1 : 2;
	if (dirlen + strlen("biolatency.bpf.o") + 1 > len)
		return NULL;
	memcpy(buf, dir, dirlen);
	buf[dirlen] = '\0';
	strncat(buf, "biolatency.bpf.o", len - dirlen - 1);
	return buf;
}

static void read_hist(int map_fd, __u64 *hist)
{
	__u64 per_cpu[MAX_BUCKETS];
	__u32 key;

	for (key = 0; key < MAX_BUCKETS; key++) {
		if (bpf_map_lookup_elem(map_fd, &key, per_cpu) != 0)
			continue;
		for (size_t c = 0; c < sizeof(per_cpu) / sizeof(per_cpu[0]); c++)
			hist[key] += per_cpu[c];
	}
}

static void print_hist(const __u64 *hist)
{
	__u64 max = 0, total = 0;
	int first = -1, last = -1;

	for (int i = 0; i < MAX_BUCKETS; i++) {
		if (hist[i]) {
			if (first < 0)
				first = i;
			last = i;
		}
		if (hist[i] > max)
			max = hist[i];
		total += hist[i];
	}

	if (first < 0) {
		printf("(no block I/O completions observed yet)\n");
		return;
	}

	printf("block I/O latency (usec), total samples: %" PRIu64 "\n\n", total);
	for (int i = first; i <= last; i++) {
		__u64 lo = 1ULL << i;
		__u64 hi = (1ULL << (i + 1)) - 1;
		int bars = max ? (int)((hist[i] * 40) / max) : 0;

		printf("%8" PRIu64 " -> %8" PRIu64 " : %-40.*s %" PRIu64 "\n",
		       lo, hi, bars, "****************************************", hist[i]);
	}
}

int main(int argc, char **argv)
{
	struct bpf_object *obj = NULL;
	struct bpf_program *prog;
	struct bpf_link *link = NULL;
	struct bpf_map *hist_map, *filter_map;
	char obj_path[4096] = { 0 };
	const char *object = NULL;
	long interval = 1;
	bool have_device = false;
	__u32 device_key = 0;
	int rc, opt;

	static const struct option long_opts[] = {
		{ "interval", required_argument, NULL, 'i' },
		{ "device", required_argument, NULL, 'd' },
		{ "object", required_argument, NULL, 1 },
		{ "help", no_argument, NULL, 'h' },
		{ NULL, 0, NULL, 0 },
	};

	while ((opt = getopt_long(argc, argv, "i:d:h", long_opts, NULL)) != -1) {
		switch (opt) {
		case 'i':
			if (parse_interval(optarg, &interval))
				return 1;
			break;
		case 'd':
			if (parse_device(optarg, &device_key))
				return 1;
			have_device = true;
			break;
		case 1:
			object = optarg;
			break;
		case 'h':
			usage(argv[0]);
			return 0;
		default:
			usage(argv[0]);
			return 1;
		}
	}

	if (!object) {
		if (!default_object_path(obj_path, sizeof(obj_path), argv[0])) {
			fprintf(stderr, "error: object path too long\n");
			return 1;
		}
		object = obj_path;
	}

	if (geteuid() != 0)
		fprintf(stderr, "warning: not running as root; BPF load will likely fail\n");

	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);

	obj = bpf_object__open_file(object, NULL);
	rc = libbpf_get_error(obj);
	if (rc) {
		obj = NULL;
		fprintf(stderr, "error: failed to open BPF object '%s': %s\n",
			object, strerror(-rc));
		goto cleanup;
	}

	rc = bpf_object__load(obj);
	if (rc) {
		fprintf(stderr, "error: failed to load BPF object: %s\n", strerror(-rc));
		goto cleanup;
	}

	hist_map = bpf_object__find_map_by_name(obj, "lat_hist");
	filter_map = bpf_object__find_map_by_name(obj, "filter_map");
	if (!hist_map || !filter_map) {
		fprintf(stderr, "error: expected maps not found in BPF object\n");
		goto cleanup;
	}

	if (have_device) {
		__u8 one = 1;

		if (bpf_map_update_elem(bpf_map__fd(filter_map), &device_key, &one,
					BPF_ANY)) {
			fprintf(stderr, "error: failed to install device filter: %s\n",
				strerror(errno));
			goto cleanup;
		}
	}

	prog = bpf_object__next_program(obj, NULL);
	for (; prog; prog = bpf_object__next_program(obj, prog)) {
		link = bpf_program__attach(prog);
		rc = libbpf_get_error(link);
		if (rc) {
			link = NULL;
			fprintf(stderr, "error: failed to attach program: %s\n",
				strerror(-rc));
			goto cleanup;
		}
	}

	while (!stop) {
		__u64 hist[MAX_BUCKETS] = { 0 };

		sleep((unsigned int)interval);
		if (stop)
			break;

		read_hist(bpf_map__fd(hist_map), hist);
		print_hist(hist);
	}

	rc = 0;

cleanup:
	if (link)
		bpf_link__destroy(link);
	if (obj)
		bpf_object__close(obj);
	return rc ? 1 : 0;
}

