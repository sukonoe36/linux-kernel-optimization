#!/usr/bin/env bash
#
# run-fio.sh - Capture fio block I/O measurements for before/after runs.
#
# Runs a configurable fio workload (default: random read) and saves raw JSON
# output plus run metadata into a timestamped results folder. This script only
# captures data; interpretation lives in docs/METHODOLOGY.md.
#
# Usage:
#   ./run-fio.sh --target /dev/nvme0n1 --phase before
#   ./run-fio.sh --target /tmp/fio-test --rw randwrite --size 4G --help
#
# Exit codes: 0 success, 1 usage error, 2 tool failure.

set -euo pipefail

# shellcheck source=bench/common.sh
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/common.sh"

TARGET=""
PHASE="before"
RW="randread"
BS="4k"
IODEPTH=32
SIZE="2G"
RUNTIME=30
NUMJOBS=1
DIRECT=1
LABEL="fio"

usage() {
	usage_for "${BASH_SOURCE[0]}" \
		"Capture fio results into bench/results/<timestamp>-fio/." \
		"" \
		"Options:" \
		"  -f, --target PATH     File or block device to test (required)" \
		"  -w, --rw MODE         I/O pattern: randread|randwrite|read|write|randrw" \
		"                        (default: randread)" \
		"  -b, --bs SIZE         Block size (default: 4k)" \
		"  -d, --iodepth N       Queue depth per job (default: 32)" \
		"  -s, --size SIZE       Total size per job (default: 2G)" \
		"  -r, --runtime SECS    Run time in seconds (default: 30)" \
		"  -j, --numjobs N       Number of parallel jobs (default: 1)" \
		"      --buffered        Use buffered I/O instead of O_DIRECT" \
		"      --phase NAME      Label the run 'before' or 'after' (default: before)" \
		"  -h, --help            Show this help and exit"
}

main() {
	while [[ $# -gt 0 ]]; do
		case "$1" in
			-f|--target) TARGET="${2:-}"; shift 2 ;;
			-w|--rw) RW="${2:-}"; shift 2 ;;
			-b|--bs) BS="${2:-}"; shift 2 ;;
			-d|--iodepth) IODEPTH="${2:-}"; shift 2 ;;
			-s|--size) SIZE="${2:-}"; shift 2 ;;
			-r|--runtime) RUNTIME="${2:-}"; shift 2 ;;
			-j|--numjobs) NUMJOBS="${2:-}"; shift 2 ;;
			--buffered) DIRECT=0; shift ;;
			--phase) PHASE="${2:-}"; shift 2 ;;
			-h|--help) usage; exit 0 ;;
			*) die "unknown argument '$1' (try --help)" ;;
		esac
	done

	[[ -n "${TARGET}" ]] || { usage; die "--target is required"; }

	case "${PHASE}" in
		before|after) ;;
		*) die "--phase must be 'before' or 'after'" ;;
	esac

	case "${RW}" in
		randread|randwrite|read|write|randrw) ;;
		*) die "unsupported --rw '${RW}'" ;;
	esac

	require_cmd fio

	local run_dir
	run_dir="$(new_run_dir "${LABEL}-${PHASE}")"
	write_metadata "${run_dir}" "${PHASE}" \
		"target=${TARGET}" "rw=${RW}" "bs=${BS}" "iodepth=${IODEPTH}" \
		"size=${SIZE}" "runtime=${RUNTIME}s" "numjobs=${NUMJOBS}" \
		"direct=${DIRECT}"

	local direct_flag=1
	[[ "${DIRECT}" -eq 1 ]] && direct_flag=1 || direct_flag=0

	log "starting fio (${RW}, bs=${BS}) against ${TARGET}"
	if ! tee_capture "${run_dir}/fio.json" \
		fio --name="kernel-opt-${PHASE}" \
			--filename="${TARGET}" \
			--rw="${RW}" \
			--bs="${BS}" \
			--iodepth="${IODEPTH}" \
			--size="${SIZE}" \
			--runtime="${RUNTIME}" \
			--time_based \
			--numjobs="${NUMJOBS}" \
			--direct="${direct_flag}" \
			--ioengine=libaio \
			--group_reporting \
			--output-format=json; then
		die "fio run failed"
	fi

	log "results saved under ${run_dir}"
}

main "$@"
