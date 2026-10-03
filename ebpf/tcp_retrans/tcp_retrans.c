// SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause
/* tcp_retrans.c - Userspace loader for the tcp_retrans BPF program.
 *
 * Loads tcp_retrans.bpf.o via libbpf, polls the per-destination retransmit
 * map on an interval, and prints a ranked list of destinations ordered by
 * retransmit count. Emits JSON with --json for machine consumption.
 *
 * Usage: sudo ./tcp_retrans [-i seconds] [-n top] [--json] [--object PATH]
 */
#include <arpa/inet.h>
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

#ifndef AF_INET
#define AF_INET 2
#endif
#ifndef AF_INET6
#define AF_INET6 10
#endif

struct dest_key {
	__u32 family;
	__u32 pad;
	union {
		__u32 addr4;
		__u8 addr6[16];
	};
};

struct dest_entry {
	struct dest_key key;
	__u64 count;
};

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
		"Trace TCP retransmissions per destination address.\n"
		"\n"
		"Options:\n"
		"  -i, --interval SECS   Poll interval in seconds (default: 2)\n"
		"  -n, --top COUNT       Show only the top COUNT destinations (default: 10)\n"
		"      --json            Emit one JSON object per poll instead of a table\n"
		"      --object PATH     Path to the BPF object file\n"
		"                        (default: tcp_retrans.bpf.o next to this binary)\n"
		"  -h, --help            Show this help and exit\n",
		prog);
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
	const char *slash;
	const char *dir;
	size_t dirlen;

	if (!argv0)
		return NULL;
	slash = strrchr(argv0, '/');
	dir = slash ? argv0 : ".";
	dirlen = slash ? (size_t)(slash - argv0) + 1 : 2;
	if (dirlen + strlen("tcp_retrans.bpf.o") + 1 > len)
		return NULL;
	memcpy(buf, dir, dirlen);
	buf[dirlen] = '\0';
	strncat(buf, "tcp_retrans.bpf.o", len - dirlen - 1);
	return buf;
}

static void format_addr(const struct dest_key *key, char *out, size_t len)
{
	if (key->family == AF_INET6) {
		struct in6_addr a6;

		memcpy(&a6, key->addr6, sizeof(a6));
		if (!inet_ntop(AF_INET6, &a6, out, (socklen_t)len))
			snprintf(out, len, "<invalid-ipv6>");
		return;
	}
	if (!inet_ntop(AF_INET, &key->addr4, out, (socklen_t)len))
		snprintf(out, len, "<invalid-ipv4>");
}

/* Comparator sorts descending by retransmit count. */
static int cmp_entries(const void *a, const void *b)
{
	const struct dest_entry *ea = a;
	const struct dest_entry *eb = b;

	if (ea->count < eb->count)
		return 1;
	if (ea->count > eb->count)
		return -1;
	return 0;
}

static int collect(int map_fd, struct dest_entry **entries, size_t *count)
{
	struct dest_key key, next;
	struct dest_entry *out = NULL;
	size_t n = 0, cap = 0;
	__u64 value;
	int rc;

	rc = bpf_map_get_next_key(map_fd, NULL, &key);
	while (rc == 0) {
		if (bpf_map_lookup_elem(map_fd, &key, &value) == 0) {
			if (n == cap) {
				size_t newcap = cap ? cap * 2 : 64;
				struct dest_entry *tmp =
					realloc(out, newcap * sizeof(*out));

				if (!tmp) {
					free(out);
					return -ENOMEM;
				}
				out = tmp;
				cap = newcap;
			}
			out[n].key = key;
			out[n].count = value;
			n++;
		}
		/* Snapshot the current key before fetching the next one: the
		 * kernel may remove the entry we just looked up. */
		memcpy(&next, &key, sizeof(next));
		rc = bpf_map_get_next_key(map_fd, &next, &key);
	}

	*entries = out;
	*count = n;
	return 0;
}

static void print_table(struct dest_entry *entries, size_t n, size_t top,
			unsigned long long total)
{
	size_t i;

	printf("%-8s %-40s %s\n", "COUNT", "DESTINATION", "SHARE");
	for (i = 0; i < n && i < top; i++) {
		char addr[INET6_ADDRSTRLEN] = { 0 };
		double share = total ?
			(100.0 * (double)entries[i].count / (double)total) : 0.0;

		format_addr(&entries[i].key, addr, sizeof(addr));
		printf("%-8" PRIu64 " %-40s %6.2f%%\n",
		       entries[i].count, addr, share);
	}
	printf("%-8llu %-40s\n", total, "TOTAL");
	fflush(stdout);
}

static void print_json(struct dest_entry *entries, size_t n, size_t top,
		       unsigned long long total, unsigned int interval)
{
	size_t i;

	printf("{\"interval_s\":%u,\"total\":%llu,\"destinations\":[",
	       interval, total);
	for (i = 0; i < n && i < top; i++) {
		char addr[INET6_ADDRSTRLEN] = { 0 };

		format_addr(&entries[i].key, addr, sizeof(addr));
		printf("%s{\"addr\":\"%s\",\"count\":%" PRIu64 "}",
		       i ? "," : "", addr, entries[i].count);
	}
	printf("]}\n");
	fflush(stdout);
}

int main(int argc, char **argv)
{
	struct bpf_object *obj = NULL;
	struct bpf_program *prog;
	struct bpf_link *link = NULL;
	struct bpf_map *map;
	char obj_path[4096] = { 0 };
	const char *object = NULL;
	long interval = 2;
	size_t top = 10;
	bool json = false;
	int map_fd, rc, opt;
	unsigned long long total;

	static const struct option long_opts[] = {
		{ "interval", required_argument, NULL, 'i' },
		{ "top", required_argument, NULL, 'n' },
		{ "json", no_argument, NULL, 1 },
		{ "object", required_argument, NULL, 2 },
		{ "help", no_argument, NULL, 'h' },
		{ NULL, 0, NULL, 0 },
	};

	while ((opt = getopt_long(argc, argv, "i:n:h", long_opts, NULL)) != -1) {
		switch (opt) {
		case 'i':
			if (parse_interval(optarg, &interval))
				return 1;
			break;
		case 'n': {
			char *end = NULL;

			top = strtoul(optarg, &end, 10);
			if (end == optarg || *end != '\0' || top == 0) {
				fprintf(stderr, "error: invalid --top '%s'\n", optarg);
				return 1;
			}
			break;
		}
		case 1:
			json = true;
			break;
		case 2:
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

	map = bpf_object__find_map_by_name(obj, "retrans_map");
	if (!map) {
		fprintf(stderr, "error: retrans_map not found in BPF object\n");
		goto cleanup;
	}
	map_fd = bpf_map__fd(map);

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

	if (!json)
		printf("%-8s %-40s %s\n", "COUNT", "DESTINATION", "SHARE");

	while (!stop) {
		struct dest_entry *entries = NULL;
		size_t n = 0;

		sleep((unsigned int)interval);
		if (stop)
			break;

		rc = collect(map_fd, &entries, &n);
		if (rc) {
			fprintf(stderr, "error: collecting map entries: %s\n",
				strerror(-rc));
			free(entries);
			break;
		}

		total = 0;
		for (size_t i = 0; i < n; i++)
			total += entries[i].count;

		qsort(entries, n, sizeof(*entries), cmp_entries);
		if (json)
			print_json(entries, n, top, total, (unsigned int)interval);
		else
			print_table(entries, n, top, total);

		free(entries);
	}

	rc = 0;

cleanup:
	if (link)
		bpf_link__destroy(link);
	if (obj)
		bpf_object__close(obj);
	return rc ? 1 : 0;
}

