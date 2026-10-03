#!/usr/bin/env bash
#
# common.sh - Shared helpers for the benchmark scripts in this directory.
#
# Sourced (not executed) by run-iperf3.sh, run-fio.sh and run-perf-stat.sh.
# Provides logging, a run-directory factory, and reproducibility metadata.
# Never reports results itself: scripts only capture raw tool output.

set -euo pipefail

BENCH_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
RESULTS_ROOT="${BENCH_DIR}/results"

log() { printf '[%s] %s\n' "$(date -u +%H:%M:%SZ)" "$*" >&2; }
die() { log "error: $*"; exit 1; }

# require_cmd <name...> -> fail early if a required tool is missing.
require_cmd() {
	local missing=()
	local c
	for c in "$@"; do
		command -v "${c}" >/dev/null 2>&1 || missing+=("${c}")
	done
	if [[ "${#missing[@]}" -gt 0 ]]; then
		die "missing required command(s): ${missing[*]}"
	fi
}

# new_run_dir <label> -> creates and prints results/<UTC timestamp>-<label>.
new_run_dir() {
	local label="${1:-run}"
	local stamp dir
	stamp="$(date -u +%Y%m%dT%H%M%SZ)"
	dir="${RESULTS_ROOT}/${stamp}-${label}"
	mkdir -p -- "${dir}"
	printf '%s\n' "${dir}"
}

# write_metadata <dir> <phase> <notes...> -> records environment for the run.
write_metadata() {
	local dir="$1" phase="$2"
	shift 2
	local file="${dir}/metadata.txt"

	{
		printf 'timestamp_utc=%s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
		printf 'phase=%s\n' "${phase}"
		printf 'hostname=%s\n' "$(uname -n)"
		printf 'kernel=%s\n' "$(uname -sr)"
		printf 'arch=%s\n' "$(uname -m)"
		printf 'cpu_model=%s\n' \
			"$(awk -F: '/model name/{print $2; exit}' /proc/cpuinfo 2>/dev/null | sed 's/^ //')"
		printf 'cpu_count=%s\n' "$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo unknown)"
		printf 'congestion_control=%s\n' \
			"$(sysctl -n net.ipv4.tcp_congestion_control 2>/dev/null || echo unknown)"
		printf 'default_qdisc=%s\n' \
			"$(sysctl -n net.core.default_qdisc 2>/dev/null || echo unknown)"
		for note in "$@"; do
			printf 'note=%s\n' "${note}"
		done
	} > "${file}"

	log "wrote metadata to ${file}"
}

# usage_for <script> <extra-lines...>
usage_for() {
	local script="$1"
	shift
	printf 'Usage: %s [options]\n\n' "$(basename "${script}")"
	for line in "$@"; do
		printf '%s\n' "${line}"
	done
}

# tee_capture <file> <command...> -> run command, tee stdout+stderr to <file>.
tee_capture() {
	local file="$1"
	shift
	log "running: $*"
	# shellcheck disable=SC2068
	"$@" 2>&1 | tee -- "${file}"
	return "${PIPESTATUS[0]}"
}
