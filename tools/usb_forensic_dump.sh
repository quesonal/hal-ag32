#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
#
# usb_forensic_dump.sh — Halt the target and dump USB0 controller
# registers + the udc_agm dQH/dTD pools for post-mortem analysis of a
# USB control-transfer hang (e.g. host/VMware CDC-ACM switch).
#
# Usage (activate the repo venv first, then):
#   bash tools/usb_forensic_dump.sh [outfile]
#
# Default outfile: /tmp/usb_forensic_<timestamp>.txt
#
# Captures (addresses from zephyr.map of the current build):
#   - SoC clock gates   : AHB_RESET(0x03000050) AHB_CLKENABLE(0x03000070)
#                          APB_CLKENABLE(0x03000060)
#   - USB0 @0x41001000  : USBCMD/USBSTS/USBINTR/FRINDEX/DEVICEADDR/
#                          ENDPOINTLISTADDR(0x140-0x15C) CONFIGFLAG/PORTSC/
#                          OTGSC/USBMODE/ENDPTSETUPSTAT/ENDPTPRIME/
#                          ENDPTFLUSH/ENDPTSTATUS/ENDPTCOMPLETE(0x180-0x1BC)
#                          ENDPTCTRL[0..3](0x1C0)
#   - dQH pool @0x20002000 (8 x 0x40)  — EP0 OUT/IN ... EP3 OUT/IN
#   - dTD pool @0x20002200 (8 x 0x20)  — one dTD per QH
# Raw words are decoded by tools/usb_forensic_decode.py.
#
# Exit codes:
#   1  tools missing / openocd not found
#   2  openocd session failed
#
# NOTE: the target is left HALTED after the dump so the dQH/dTD overlay
# snapshot stays coherent. Before handing the board back to the host,
# resume it (or pass AGM_LEAVE_RUNNING=1 to auto-resume):
#   openocd ... -c "init" -c "reset run" -c "shutdown"

set -eu

# --- venv guard  ----
# See tools/flash_fw.sh for the rationale.
if [ -z "${VIRTUAL_ENV:-}" ]; then
	echo "FATAL: no Zephyr virtualenv active - activate the repo venv first" >&2
	exit 3
fi
PYTHON=${PYTHON:-$VIRTUAL_ENV/bin/python3}

OUTFILE="${1:-/tmp/usb_forensic_$(date +%Y%m%d_%H%M%S).txt}"

# This script lives in <module>/tools/, so the module root is its parent.
SELF_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ZEPHYR_HAL_AGM_HOME=${ZEPHYR_HAL_AGM_HOME:-$(dirname -- "$SELF_DIR")}
ZEPHYR_HAL_AGM_TOOLS=${ZEPHYR_HAL_AGM_TOOLS:-$ZEPHYR_HAL_AGM_HOME/tools}
AGRV_SDK_PATH=${AGRV_SDK_PATH:-$HOME/AgRV_pio}
AGRV_OPENOCD=${AGRV_OPENOCD:-$AGRV_SDK_PATH/packages/tool-agrv_openocd/bin/openocd_cmd}
AGRV_ADAPTER=${AGRV_ADAPTER:-cmsis-dap}
BOARD="${BOARD:-agrv2k_407}"
SUPPORT_CFG="${SUPPORT_CFG:-${AGM_BUILD_DIR:-$HOME/zephyrproject/build}/logic/openocd.cfg}"
[ -f "$SUPPORT_CFG" ] || SUPPORT_CFG="$ZEPHYR_HAL_AGM_HOME/boards/agm/$BOARD/support/openocd.cfg"

if [ ! -x "$AGRV_OPENOCD" ]; then
	echo "Error: openocd not found at $AGRV_OPENOCD" >&2
	exit 1
fi
if [ ! -f "$SUPPORT_CFG" ]; then
	echo "Error: openocd support cfg not found at $SUPPORT_CFG" >&2
	exit 1
fi

# Warm up the CMSIS-DAP probe (required before every openocd session).
"$PYTHON" "$ZEPHYR_HAL_AGM_TOOLS/openocd_warmup.py" || {
	echo "Error: probe warmup failed" >&2
	exit 1
}

echo "=== usb_forensic_dump $(date -u +%Y-%m-%dT%H:%M:%SZ) ===" > "$OUTFILE"

export AGRV_ADAPTER
# Dump session: halt, read, leave halted (overlay must not move while
# we read it; openocd's examine-end already halts, so no explicit halt
# needed, but keep it for clarity).
"$AGRV_OPENOCD" -f "$SUPPORT_CFG" \
	-c "init" \
	-c "reg pc" \
	-c "mdw 0x03000050 0x1" \
	-c "mdw 0x03000070 0x1" \
	-c "mdw 0x03000060 0x1" \
	-c "mdw 0x41001140 0x8" \
	-c "mdw 0x41001180 0x10" \
	-c "mdw 0x410011c0 0x4" \
	-c "mdw 0x20002000 0x80" \
	-c "mdw 0x20002200 0x40" \
	-c "shutdown" 2>&1 | tee -a "$OUTFILE" || {
	echo "Error: openocd dump session failed" >&2
	exit 2
}

if [ "${AGM_LEAVE_RUNNING:-0}" = "1" ]; then
	export AGRV_ADAPTER
	"$AGRV_OPENOCD" -f "$SUPPORT_CFG" \
		-c "init" -c "resume" -c "shutdown" >/dev/null 2>&1 || true
fi

"$PYTHON" "$ZEPHYR_HAL_AGM_TOOLS/usb_forensic_decode.py" "$OUTFILE"

echo ""
echo "raw dump : $OUTFILE"
if [ "${AGM_LEAVE_RUNNING:-0}" = "1" ]; then
	echo "target   : RUNNING (resume issued)"
else
	echo "target   : HALTED - resume with 'openocd ... -c \"reset run\"' before host re-test"
fi
