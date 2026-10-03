#!/usr/bin/env bash
#
# run-iperf3.sh - Capture iperf3 network measurements for before/after runs.
#
# Runs iperf3 in client mode against a target server and saves raw output plus
# metadata into a timestamped results folder. This script only captures data;
# interpreting it is left to docs/METHODOLOGY.md.
#
# Usage:
#   ./run-iperf3.sh --server HOST [--phase before|after] [--duration 30]
#   ./run-iperf3.sh --help
#
# Exit codes: 0 success, 1 usage error, 2 tool failure.

set -euo pipefail

# shellcheck source=common.sh
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/common.sh"

SERVER=""
PHASE="before"
DURATION=30
STREAMS=1
PORT=5201
DIRECTION="upload"
LABEL="iperf3"

usage() {
	usage_for "${BASH_SOURCE[0]}" \
		"Capture iperf3 results into bench/results/<timestamp>-iperf3/." \
		"" \
		"Options:" \
		"  -s, --server HOST     iperf3 server hostname/IP (required)" \
		"  -p, --port PORT       iperf3 server port (default: 5201)" \
		"  -t, --duration SECS   Test duration in seconds (default: 30)" \
		"  -P, --streams N       Parallel streams (default: 1)" \
		"  -R, --reverse         Measure download instead of upload" \
		"      --phase NAME      Label the run 'before' or 'after' (default: before)" \
		"  -h, --help            Show this help and exit"
}

main() {
	while [[ $# -gt 0 ]]; do
		case "$1" in
			-s|--server) SERVER="${2:-}"; shift 2 ;;
			-p|--port) PORT="${2:-}"; shift 2 ;;
			-t|--duration) DURATION="${2:-}"; shift 2 ;;
			-P|--streams) STREAMS="${2:-}"; shift 2 ;;
			-R|--reverse) DIRECTION="download"; shift ;;
			--phase) PHASE="${2:-}"; shift 2 ;;
			-h|--help) usage; exit 0 ;;
			*) die "unknown argument '$1' (try --help)" ;;
		esac
	done

	[[ -n "${SERVER}" ]] || { usage; die "--server is required"; }

	case "${PHASE}" in
		before|after) ;;
		*) die "--phase must be 'before' or 'after'" ;;
	esac

	require_cmd iperf3

	local run_dir
	run_dir="$(new_run_dir "${LABEL}-${PHASE}")"
	write_metadata "${run_dir}" "${PHASE}" \
		"server=${SERVER}" "port=${PORT}" "duration=${DURATION}s" \
		"streams=${STREAMS}" "direction=${DIRECTION}"

	local -a args=(-c "${SERVER}" -p "${PORT}" -t "${DURATION}" -P "${STREAMS}" --json)
	if [[ "${DIRECTION}" == "download" ]]; then
		args+=(-R)
	fi

	log "starting iperf3 (${DIRECTION}) against ${SERVER}:${PORT}"
	if ! tee_capture "${run_dir}/iperf3.json" iperf3 "${args[@]}"; then
		die "iperf3 run failed"
	fi

	log "results saved under ${run_dir}"
}

main "$@"
