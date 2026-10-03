#!/usr/bin/env bash
#
# run-perf-stat.sh - Capture `perf stat` counters for a command, before/after.
#
# Wraps an arbitrary command with `perf stat`, saving the counter output and
# run metadata into a timestamped results folder. Use this to observe whether a
# tuning change shifted CPU-level behavior (cycles, cache misses, context
# switches). This script only captures data; see docs/METHODOLOGY.md.
#
# Usage:
#   ./run-perf-stat.sh --phase before -- ./your-benchmark --args
#   ./run-perf-stat.sh --help
#
# Exit codes: 0 success, 1 usage error, 2 tool failure.

set -euo pipefail

# shellcheck source=common.sh
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/common.sh"

PHASE="before"
REPEAT=3
EVENTS="task-clock,cycles,instructions,cache-misses,context-switches"
LABEL="perf-stat"
CMD=()

usage() {
	usage_for "${BASH_SOURCE[0]}" \
		"Capture perf stat counters into bench/results/<timestamp>-perf-stat/." \
		"" \
		"Options:" \
		"      --phase NAME      Label the run 'before' or 'after' (default: before)" \
		"  -r, --repeat N        Repeat the measurement N times (default: 3)" \
		"  -e, --events LIST     Comma-separated perf events to count" \
		"                        (default: ${EVENTS})" \
		"  -h, --help            Show this help and exit" \
		"" \
		"Everything after '--' is the command to measure." \
		"" \
		"Example:" \
		"  sudo ./run-perf-stat.sh --phase before -- ./my-bench --iterations 1000"
}

main() {
	local passthrough=0
	while [[ $# -gt 0 ]]; do
		if [[ "${passthrough}" -eq 1 ]]; then
			CMD+=("$1")
			shift
			continue
		fi
		case "$1" in
			--phase) PHASE="${2:-}"; shift 2 ;;
			-r|--repeat) REPEAT="${2:-}"; shift 2 ;;
			-e|--events) EVENTS="${2:-}"; shift 2 ;;
			-h|--help) usage; exit 0 ;;
			--) passthrough=1; shift ;;
			*) die "unexpected argument '$1' (use '--' before the command, or --help)" ;;
		esac
	done

	[[ "${#CMD[@]}" -gt 0 ]] || { usage; die "no command given after '--'"; }

	case "${PHASE}" in
		before|after) ;;
		*) die "--phase must be 'before' or 'after'" ;;
	esac

	require_cmd perf

	local run_dir
	run_dir="$(new_run_dir "${LABEL}-${PHASE}")"
	write_metadata "${run_dir}" "${PHASE}" \
		"repeat=${REPEAT}" "events=${EVENTS}" "command=${CMD[*]}"

	local i
	for ((i = 1; i <= REPEAT; i++)); do
		log "perf stat iteration ${i}/${REPEAT}"
		if ! tee_capture "${run_dir}/perf-stat-run${i}.txt" \
			perf stat -e "${EVENTS}" -- "${CMD[@]}"; then
			die "perf stat run ${i} failed"
		fi
	done

	log "results saved under ${run_dir}"
}

main "$@"
