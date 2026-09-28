#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
#
# devsync.sh — make the west workspace build *this* working tree.
#
# The documented loop is "edit here -> commit -> push -> pull in
# <ws>/modules/hal_ag32 -> build", which costs one commit per debug
# iteration. This script replaces the push/pull half with a mirror, so a
# debug round is only "edit -> push -> build", and the commit happens once
# the result is good:
#
#   tools/devsync.sh status           # what would change (writes nothing)
#   tools/devsync.sh push --apply     # mirror dev tree into the workspace
#   ... build / flash / capture from <ws> ...
#   tools/devsync.sh restore --apply  # put the workspace back on origin/main
#
# Why this is safe now: hal_ag32 is the *manifest* repository of the
# workspace, and `west update` never touches the manifest repository
# (west 1.5: "This command does not alter the manifest repository's
# contents"). Before the manifest reversal  the next
# `west update` would have silently undone any such mirror.
#
# Rules while a mirror is in place:
#   * do not `git pull` / `git checkout` / `git clean` in the workspace
#     checkout: they either fail (tree is dirty) or silently discard the
#     mirror; use `restore` instead;
#   * do not hand-edit the workspace checkout -- `push` overwrites it;
#   * do not run `west update` (the mirrored west.yml can carry a
#     different pin) until you have run `restore`;
#   * a build off a mirrored tree is *not* a result "at <sha>": only
#     `restore` + a clean rebuild is. Record the context (pin, bitstream,
#     overlay) whenever a mirrored build's output goes into docs.
#
# Environment (the same names every hal_ag32 tool reads):
#   AGM_WORKSPACE   west workspace root   default: $HOME/zephyrproject
#   AGM_DEV_TREE    tree to mirror        default: the tree this script lives in
#
# Exit codes: 0 ok, 2 usage, 3 preflight failure (paths missing / same).

set -eu

CMD=${1:-status}
case "$CMD" in
status | push | restore) ;;
*) echo "usage: $0 [status|push|restore] [--apply]" >&2; exit 2 ;;
esac
[ $# -ge 1 ] && shift
APPLY=0
for arg in "$@"; do
	case "$arg" in
	--apply) APPLY=1 ;;
	-n | --dry-run) APPLY=0 ;;
	*) echo "unknown option: $arg" >&2; exit 2 ;;
	esac
done

SELF_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
DEV=${AGM_DEV_TREE:-$(dirname -- "$SELF_DIR")}
WS=${AGM_WORKSPACE:-$HOME/zephyrproject}
DEST=$WS/modules/hal_ag32

git -C "$DEV" rev-parse --git-dir >/dev/null 2>&1 ||
	{ echo "not a git tree: $DEV" >&2; exit 3; }
[ -d "$DEST" ] || { echo "no workspace module checkout: $DEST" >&2; exit 3; }
[ "$(readlink -f "$DEV")" != "$(readlink -f "$DEST")" ] ||
	{ echo "source and destination are the same directory ($DEV)" >&2; exit 3; }

# Generated files live in the source tree (they are gitignored); neither
# mirror nor delete them, the build regenerates what it needs.
# -c compares content instead of mtime, so `status` only reports real
# differences (a copy/checkout elsewhere bumps mtimes without changing
# anything); -O keeps directory mtimes out of the report.
RSYNC_OPTS="-a -c -O --delete --exclude=.git/ --exclude=.claude/ --exclude=code_review/
	--exclude=docs/datasheet/ --exclude=__pycache__/ --exclude=*.pyc
	--exclude=board.ve --exclude=openocd.cfg --exclude=logic/ --exclude=build/"
# shellcheck disable=SC2086 # RSYNC_OPTS is a deliberate word list
set -- $RSYNC_OPTS

head_line() {
	printf 'dev tree : %s (%s, %s dirty)\n' "$DEV" \
		"$(git -C "$DEV" rev-parse --short HEAD 2>/dev/null || echo '?')" \
		"$(git -C "$DEV" status --porcelain 2>/dev/null | wc -l)"
	printf 'mirror   : %s (%s, %s dirty)\n' "$DEST" \
		"$(git -C "$DEST" rev-parse --short HEAD 2>/dev/null || echo '?')" \
		"$(git -C "$DEST" status --porcelain 2>/dev/null | wc -l)"
}

case "$CMD" in
status)
	head_line
	# Itemize lines starting with '.' are already up to date (by content);
	# anything else is a transfer or a deletion that a push would perform.
	rsync -n --itemize-changes "$@" "$DEV/" "$DEST/" >"${TMPDIR:-/tmp}/devsync.$$" || true
	grep -v '^\.' "${TMPDIR:-/tmp}/devsync.$$" >"${TMPDIR:-/tmp}/devsync.real.$$" || true
	echo "--- a push would do (first 20) ---"
	head -20 "${TMPDIR:-/tmp}/devsync.real.$$"
	printf 'to transfer: %s   already identical: %s\n' \
		"$(wc -l <"${TMPDIR:-/tmp}/devsync.real.$$")" \
		"$(grep -c '^\.' "${TMPDIR:-/tmp}/devsync.$$" || true)"
	rm -f "${TMPDIR:-/tmp}/devsync.$$" "${TMPDIR:-/tmp}/devsync.real.$$"
	;;
push)
	head_line
	if [ "$APPLY" -eq 0 ]; then
		echo "--- dry run; re-run with --apply to mirror ---"
		rsync -n --itemize-changes "$@" "$DEV/" "$DEST/" |
			grep -v '^\.' | head -20
		printf 'to transfer: %s\n' "$(rsync -n --itemize-changes "$@" "$DEV/" "$DEST/" |
			grep -cv '^\.' || true)"
		exit 0
	fi
	rsync "$@" "$DEV/" "$DEST/"
	echo "--- mirrored; workspace now mirrors the dev tree ---"
	head_line
	echo "build from $WS; then: $0 restore --apply"
	;;
restore)
	if [ "$APPLY" -eq 0 ]; then
		echo "--- dry run: the workspace checkout would go back to origin/main ---"
		git -C "$DEST" status --short
		git -C "$DEST" clean -nd
		exit 0
	fi
	git -C "$DEST" checkout -- .
	git -C "$DEST" clean -fd
	echo "--- restored; workspace is back on its own HEAD ---"
	head_line
	;;
esac
