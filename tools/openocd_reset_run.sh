#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
#
# openocd_reset_run.sh — reset the target and let it run; nothing else.
#
# The minimal "init / reset run / shutdown" sequence, for when you only
# want a clean boot. Typical use: start a UART reader first, then run
# this so the boot banner is not missed (see tools/test_uart_capture.sh,
# which does the same internally).
#
# Environment (the same names every hal_ag32 tool reads):
#   AGRV_SDK_PATH        default: $HOME/AgRV_pio
#   AGRV_OPENOCD         default: $AGRV_SDK_PATH/packages/tool-agrv_openocd/bin/openocd_cmd
#   AGRV_PLATFORM_ETC    default: $AGRV_SDK_PATH/platforms/AgRV/etc
#   ZEPHYR_HAL_AGM_HOME  default: the tree this script lives in
#   BOARD                default: agrv2k_407
#
# Usage:
#   tools/openocd_reset_run.sh [board]

set -eu

BOARD="${1:-${BOARD:-agrv2k_407}}"

SELF_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ZEPHYR_HAL_AGM_HOME=${ZEPHYR_HAL_AGM_HOME:-$(dirname -- "$SELF_DIR")}
AGRV_SDK_PATH=${AGRV_SDK_PATH:-$HOME/AgRV_pio}
AGRV_OPENOCD=${AGRV_OPENOCD:-$AGRV_SDK_PATH/packages/tool-agrv_openocd/bin/openocd_cmd}
AGRV_PLATFORM_ETC=${AGRV_PLATFORM_ETC:-$AGRV_SDK_PATH/platforms/AgRV/etc}
AGRV_ADAPTER=${AGRV_ADAPTER:-cmsis-dap}
# Probe setup lives in the board's support/openocd.cfg (never the SDK's
# agrv2k.cfg directly -- that cfg misses the CMSIS-DAP v2 backend switch and
# the vendor's 10 MHz clock). Prefer the workspace copy, which every
# `west build` re-renders; this repo's own checkout keeps a stale one.
SUPPORT_CFG=""
for _cand in "${AGM_WORKSPACE:-$HOME/zephyrproject}/modules/hal_ag32" \
	     "$ZEPHYR_HAL_AGM_HOME" "$HOME/zephyr-hal-ag32"; do
	if [ -f "$_cand/zephyr/module.yml" ]; then
		SUPPORT_CFG="${AGM_OPENOCD_CFG:-}"
		[ -n "$SUPPORT_CFG" ] || SUPPORT_CFG="${AGM_BUILD_DIR:+${AGM_BUILD_DIR}/logic/openocd.cfg}"
		[ -n "$SUPPORT_CFG" ] || SUPPORT_CFG=$(ls -t "${AGM_WORKSPACE:-$HOME/zephyrproject}"/build*/logic/openocd.cfg /tmp/b_*/logic/openocd.cfg 2>/dev/null | head -1)
		[ -n "$SUPPORT_CFG" ] || SUPPORT_CFG="$_cand/boards/agm/$BOARD/support/openocd.cfg"
		break
	fi
done

# The board cfg pulls in the SDK's agrv2k.cfg (adapter/target setup + the
# `agrv` flash driver). A reset needs none of the SDK files: when it is not
# installed, use this module's own target cfg instead. AGM_OPENOCD_CFG wins.
SUPPORT_DIR=$(dirname -- "$SUPPORT_CFG")
MINIMAL_CFG="$SUPPORT_DIR/../../agrv2k/shared/support/agrv2k-minimal.cfg"
SDK_CFG="$AGRV_SDK_PATH/platforms/AgRV/etc/agrv2k.cfg"
if [ -n "${AGM_OPENOCD_CFG:-}" ]; then
	OPENOCD_CFG=$AGM_OPENOCD_CFG
elif [ -f "$SDK_CFG" ]; then
	OPENOCD_CFG=$SUPPORT_CFG
else
	OPENOCD_CFG=$MINIMAL_CFG
fi

if [ ! -x "$AGRV_OPENOCD" ]; then
	echo "FATAL: openocd not found at $AGRV_OPENOCD (set AGRV_SDK_PATH)" >&2
	exit 3
fi
if [ ! -f "$SUPPORT_CFG" ]; then
	echo "FATAL: board support cfg not found at $SUPPORT_CFG" >&2
	exit 3
fi

# The SDK cfg picks its interface from $ADAPTER, which boards/.../openocd.cfg.in
# initialises from the AGRV_ADAPTER environment variable -- without the export
# it falls through to the SDK default (J-Link) and fails with
# "Error: No J-Link device found".
export AGRV_ADAPTER

# The examine fails intermittently on this dev board (2 runs in 6,
# with and without the SDK cfg), so retry before reporting a failure.
for attempt in 1 2 3 4; do
	OCD_RC=0
	OCD_OUT=$("$AGRV_OPENOCD" -s "$AGRV_PLATFORM_ETC" -f "$OPENOCD_CFG" \
		-c "init" -c "reset run" -c "shutdown" 2>&1) || OCD_RC=$?
	case "$OCD_OUT" in
	*"Examination failed"* | *"stalled AP"* | *"dmstatus=0x0"* | \
	*"Couldn't determine state"* | *"examine-end failed"*)
		[ "$attempt" -lt 4 ] && continue ;;
	esac
	break
done
printf '%s\n' "$OCD_OUT" | grep -iE "examined|shutdown|error" | head -5

if printf '%s' "$OCD_OUT" | grep -q "^Error"; then
	echo "FATAL: openocd reported an error (rc=${OCD_RC:-0}, adapter=$AGRV_ADAPTER)" >&2
	exit 3
fi
