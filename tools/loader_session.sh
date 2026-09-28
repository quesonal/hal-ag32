#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# loader_session.sh — reset the board, land in the spi_boot_loader console and
#                     then hand the serial port to the host command given.
#
# Why it is needed
#   samples/spi_boot_loader boots whatever store its record points at after a
#   1.5 s window, and a byte sent inside that window cancels the boot and
#   leaves the console in charge of the UART. Both standard upload paths need
#   that: agrv32flash (AN3155) and smpmgr (SMP) start talking the moment they
#   open the port, and if they miss the window the loader has already jumped
#   into the application and there is nothing left to talk to.
# `agrv32flash` straight after `openocd_reset_run.sh` fails with
#   "Failed to init device" for exactly this reason.
#
# Usage
#   tools/loader_session.sh [--port /dev/ttyACM0] [--window 2.2] <command ...>
#
# Examples
#   tools/loader_session.sh agrv32flash -b 115200 -m 8n1 -S 0x80000000 \
#       -w app.bin -g 0x80000000
#   tools/loader_session.sh smpmgr -p /dev/ttyACM0 image upload \
#       --format any --slot 0 app.bin
#
# The command may also be omitted, in which case the loader console is left at
# its prompt for whatever reads the port next.

set -eu

PORT=${AGM_LOADER_PORT:-/dev/ttyACM0}
WINDOW=2.2

while [ $# -gt 0 ]; do
	case "$1" in
	--port)
		PORT=$2
		shift 2
		;;
	--window)
		WINDOW=$2
		shift 2
		;;
	--)
		shift
		break
		;;
	*)
		break
		;;
	esac
done

SELF_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

# Keep whatever the loader prints on the way in for the caller to see when
# something goes wrong (AGM_LOADER_QUIET=1 silences it).
if [ "${AGM_LOADER_QUIET:-0}" = "1" ]; then
	"$SELF_DIR/loader_session_helper.py" "$PORT" "$WINDOW" >/dev/null
else
	"$SELF_DIR/loader_session_helper.py" "$PORT" "$WINDOW"
fi

if [ $# -eq 0 ]; then
	exit 0
fi

exec "$@"
