#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
#
# clean_twister.sh — reclaim the disk space test and build runs leave behind.
#
# Why this exists: by default twister "cleans" its output directory by
# *renaming* it, so every run leaves <dir>.1, <dir>.2, ... behind, and a west
# build dir is 20 MB -- and a twister -O tree over all samples is 200 MB to
# over a gigabyte.: one session's leftovers filled the
# 97 GB workspace volume to 100%, and the next builds failed with
# "No space left on device" -- an error that looks like anything but a full
# disk (it surfaced as a bogus "CMake build failure" in twister and as a
# silently stale firmware in a capture).
#
# Usage
#   tools/clean_twister.sh                    # list what would go (dry run)
#   tools/clean_twister.sh --apply            # delete it
#   tools/clean_twister.sh --keep 2 --apply   # keep the 2 newest per pattern
#   tools/clean_twister.sh --apply /tmp/tw_* /tmp/b_*   # explicit roots
#
# Default roots: /tmp/tw_* plus /tmp/tw_*.1-style siblings (twister -O trees,
# including the renamed ones) and /tmp/b_* (west build dirs). Anything else --
# /tmp/verify_flow (the provisioning salt!), /tmp/agm100.overlay, ... -- is
# not touched: this tool only ever looks at those two patterns.
#
# It is a dry run unless --apply is given, and it prints the size it would
# reclaim either way.

set -eu

APPLY=0
KEEP=0

usage() {
	sed -n '3,30p' "$0" | sed 's/^# \{0,1\}//'
	exit 2
}

while [ $# -gt 0 ]; do
	case "$1" in
	--apply) APPLY=1; shift ;;
	--keep) KEEP=$2; shift 2 ;;
	--dry-run) APPLY=0; shift ;;
	-h|--help) usage ;;
	-*) echo "unknown option: $1" >&2; usage ;;
	*) break ;;
	esac
done

if [ $# -gt 0 ]; then
	ROOTS="$*"
else
	ROOTS="/tmp/tw_* /tmp/b_*"
fi

# Sorted newest first, one absolute path per line. `ls -dt` is fine here: the
# patterns are ours and the names have no spaces.
list_dirs() {
	for pat in $ROOTS; do
		# shellcheck disable=SC2086 # deliberate glob
		ls -dt $pat 2>/dev/null || true
	done
}

total_kb=0
removed=0
kept=0

for d in $(list_dirs); do
	[ -d "$d" ] || continue
	case "$d" in
	/tmp/tw_*|/tmp/b_*) ;;
	*)
		echo "refusing to touch $d (not one of the default patterns)" >&2
		continue
		;;
	esac
	if [ "$kept" -lt "$KEEP" ]; then
		kept=$((kept + 1))
		printf 'keep   %-40s %s\n' "$d" "$(du -sh "$d" 2>/dev/null | cut -f1)"
		continue
	fi
	kb=$(du -sk "$d" 2>/dev/null | cut -f1)
	total_kb=$((total_kb + kb))
	removed=$((removed + 1))
	if [ "$APPLY" -eq 1 ]; then
		printf 'remove %-40s %s\n' "$d" "$(du -sh "$d" 2>/dev/null | cut -f1)"
		# `find -depth -delete` rather than rm -rf: the repo uses the same
		# idiom in build_bitstream.sh, and some sandboxes reject rm -rf.
		find "$d" -depth -delete
	else
		printf 'would  %-40s %s\n' "$d" "$(du -sh "$d" 2>/dev/null | cut -f1)"
	fi
done

if [ "$APPLY" -eq 1 ]; then
	echo ">>> removed $removed director(ies), freed $((total_kb / 1024)) MB"
else
	echo ">>> $removed director(ies), $((total_kb / 1024)) MB -- re-run with --apply"
fi
