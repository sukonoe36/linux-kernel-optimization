#!/usr/bin/env bash
#
# apply-profile.sh - Apply a commented sysctl profile, with backup and revert.
#
# Reads a "key = value" profile file (see ./network-latency.conf and
# ./high-throughput.conf), records the current value of every key it is about to
# change, and applies the new values with `sysctl`. The backup is written as a
# sysctl-compatible file so that --revert can restore the exact previous state.
#
# Usage:
#   sudo ./apply-profile.sh --profile network-latency.conf
#   sudo ./apply-profile.sh --revert
#   ./apply-profile.sh --help
#
# Exit codes: 0 success, 1 usage/error, 2 failed to apply one or more keys.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DEFAULT_PROFILE="${SCRIPT_DIR}/network-latency.conf"
DEFAULT_BACKUP_DIR="/var/backups/sysctl-profiles"

PROFILE=""
BACKUP_DIR="${DEFAULT_BACKUP_DIR}"
REVERT=0
DRY_RUN=0

usage() {
	cat <<EOF
Usage: $(basename "${BASH_SOURCE[0]}") [options]

Apply a sysctl tuning profile to the running kernel, backing up prior values so
the change can be undone.

Options:
  -p, --profile PATH    Profile file to apply (default: ${DEFAULT_PROFILE})
  -b, --backup-dir DIR  Where to store the backup of previous values
                        (default: ${DEFAULT_BACKUP_DIR})
  -r, --revert          Restore the most recent backup instead of applying
  -n, --dry-run         Print the commands that would run, change nothing
  -h, --help            Show this help and exit

Examples:
  sudo $(basename "${BASH_SOURCE[0]}") --profile network-latency.conf
  sudo $(basename "${BASH_SOURCE[0]}") --revert
EOF
}

log() { printf '%s\n' "$*" >&2; }
die() { log "error: $*"; exit 1; }

require_root() {
	if [[ "${EUID}" -ne 0 ]]; then
		die "this script must run as root (try sudo)"
	fi
	if ! command -v sysctl >/dev/null 2>&1; then
		die "'sysctl' not found in PATH"
	fi
}

# latest_backup <dir> -> prints path of the newest backup file, or fails.
latest_backup() {
	local dir="$1" newest=""
	[[ -d "${dir}" ]] || die "backup directory '${dir}' does not exist"

	newest="$(find "${dir}" -maxdepth 1 -name 'sysctl-backup-*.conf' -type f \
		-printf '%T@ %p\n' 2>/dev/null | sort -nr | head -n1 | cut -d' ' -f2-)"
	[[ -n "${newest}" ]] || die "no backups found in '${dir}'"
	printf '%s\n' "${newest}"
}

# read_current <key> -> prints the current value, or nothing if unknown.
read_current() {
	local key="$1"
	sysctl -n -- "${key}" 2>/dev/null || true
}

# normalize_line <raw> -> echo "key=value" with comments/space stripped, or "".
normalize_line() {
	local line="$1"
	line="${line%%#*}"
	line="$(printf '%s' "${line}" | sed -e 's/^[[:space:]]*//' -e 's/[[:space:]]*$//')"
	if [[ -z "${line}" || "${line}" != *=* ]]; then
		printf ''
		return 0
	fi
	printf '%s' "${line}"
}

# split_key <line> / split_value <line>
split_key()   { printf '%s' "${1%%=*}" | tr -d '[:space:]'; }
split_value() { printf '%s' "${1#*=}" | sed -e 's/^[[:space:]]*//' -e 's/[[:space:]]*$//'; }

# capture_backup <profile> <backup_file> -> write current values for profile keys.
capture_backup() {
	local profile="$1" backup_file="$2" line key cur

	[[ -r "${profile}" ]] || die "cannot read profile '${profile}'"
	{
		printf '# sysctl backup written by apply-profile.sh\n'
		printf '# source profile: %s\n' "${profile}"
		printf '# timestamp: %s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
		while IFS= read -r line || [[ -n "${line}" ]]; do
			line="$(normalize_line "${line}")"
			[[ -z "${line}" ]] && continue
			key="$(split_key "${line}")"
			cur="$(read_current "${key}")"
			if [[ -n "${cur}" ]]; then
				printf '%s = %s\n' "${key}" "${cur}"
			else
				printf '# %s was unset before apply\n' "${key}"
			fi
		done < "${profile}"
	} > "${backup_file}"
	log "saved previous values to ${backup_file}"
}

# apply_file <file> -> apply each key=value line. Returns 2 on any failure.
apply_file() {
	local file="$1" line key value applied=0 failed=0

	while IFS= read -r line || [[ -n "${line}" ]]; do
		line="$(normalize_line "${line}")"
		[[ -z "${line}" ]] && continue
		key="$(split_key "${line}")"
		value="$(split_value "${line}")"

		if [[ "${DRY_RUN}" -eq 1 ]]; then
			log "would run: sysctl -w ${key}=${value}"
			continue
		fi
		if sysctl -w "${key}=${value}" >/dev/null 2>&1; then
			applied=$((applied + 1))
			log "applied ${key} = ${value}"
		else
			failed=$((failed + 1))
			log "warning: failed to apply ${key} = ${value}"
		fi
	done < "${file}"

	if [[ "${DRY_RUN}" -eq 1 ]]; then
		return 0
	fi
	log "done: ${applied} applied, ${failed} failed"
	[[ "${failed}" -eq 0 ]] || return 2
	return 0
}

# apply_profile <profile> <backup_file>
apply_profile() {
	local profile="$1" backup_file="$2"

	mkdir -p -- "${BACKUP_DIR}"
	if [[ "${DRY_RUN}" -eq 0 ]]; then
		capture_backup "${profile}" "${backup_file}"
	fi
	apply_file "${profile}"
}

# revert_profile <backup_file>
revert_profile() {
	local backup_file="$1"

	[[ -r "${backup_file}" ]] || die "cannot read backup '${backup_file}'"
	log "restoring sysctl values from ${backup_file}"
	apply_file "${backup_file}" || return 2

	if [[ "${DRY_RUN}" -eq 0 ]]; then
		mv -- "${backup_file}" "${backup_file}.applied" 2>/dev/null || true
	fi
	return 0
}

main() {
	while [[ $# -gt 0 ]]; do
		case "$1" in
			-p|--profile) PROFILE="${2:-}"; shift 2 ;;
			-b|--backup-dir) BACKUP_DIR="${2:-}"; shift 2 ;;
			-r|--revert) REVERT=1; shift ;;
			-n|--dry-run) DRY_RUN=1; shift ;;
			-h|--help) usage; exit 0 ;;
			*) die "unknown argument '$1' (try --help)" ;;
		esac
	done

	require_root

	if [[ "${REVERT}" -eq 1 ]]; then
		local backup_file
		backup_file="$(latest_backup "${BACKUP_DIR}")"
		revert_profile "${backup_file}" || exit 2
		return 0
	fi

	[[ -n "${PROFILE}" ]] || PROFILE="${DEFAULT_PROFILE}"
	local stamp backup_file
	stamp="$(date -u +%Y%m%dT%H%M%SZ)"
	backup_file="${BACKUP_DIR}/sysctl-backup-${stamp}.conf"

	apply_profile "${PROFILE}" "${backup_file}" || exit 2
}

main "$@"

