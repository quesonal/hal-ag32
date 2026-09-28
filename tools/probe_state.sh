#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
#
# probe_state.sh — one command that answers "what does the debugger see?".
#
# It prints, in order:
#   * the USB device and the console node (so a missing probe / a wedged
#     CDC-ACM bridge is visible immediately);
#   * the AgRV2K device and option-byte readback via the SDK's oo runner:
#     option byte, WRITE protection register, read protection, and the FPGA
#     configuration address. This is the readback that tells a real lock
#     apart from openocd's driver *deciding* the flash is protected
#     ;
#   * the SWD setup the board cfg will actually use (speed + backend);
#   * a few SoC registers read through the debugger: FCB STAT (0x40010010,
#     bit 1 = ACTIVE -- did the fabric come up?), SYS.APB_CLKENABLE and
#     SYS.RST_CNTL.
#
# Read-only: it never erases or writes anything. Use probe_recover.sh when
# it reports something that needs fixing.
#
# Environment: AGRV_SDK_PATH, AGM_OO, AGM_OPENOCD_BIN, AGM_SWD_SPEED,
# ZEPHYR_HAL_AGM_HOME, AGM_WORKSPACE.

set -eu

usage() { sed -n '4,20p' "$0" | sed 's/^# \{0,1\}//'; exit 2; }
[ "${1:-}" = "-h" ] && usage

if [ -z "${VIRTUAL_ENV:-}" ]; then
	echo "FATAL: no Zephyr virtualenv active - activate the repo venv first" >&2
	exit 3
fi

BOARD=${BOARD:-agrv2k_407}
AGRV_SDK_PATH=${AGRV_SDK_PATH:-$HOME/AgRV_pio}
AGM_OPENOCD_BIN=${AGM_OPENOCD_BIN:-$AGRV_SDK_PATH/packages/tool-agrv_openocd/bin}
SELF_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
AGM_HOME=${ZEPHYR_HAL_AGM_HOME:-$(dirname -- "$SELF_DIR")}

SUPPORT_CFG=""
for _cand in "${AGM_WORKSPACE:-$HOME/zephyrproject}/modules/hal_ag32" \
	     "$AGM_HOME" "$HOME/zephyr-hal-ag32"; do
	if [ -f "$_cand/zephyr/module.yml" ]; then
		AGM_HOME="$_cand"
		SUPPORT_CFG="${AGM_OPENOCD_CFG:-}"
		[ -n "$SUPPORT_CFG" ] || SUPPORT_CFG="${AGM_BUILD_DIR:+${AGM_BUILD_DIR}/logic/openocd.cfg}"
		[ -n "$SUPPORT_CFG" ] || SUPPORT_CFG=$(ls -t "${AGM_WORKSPACE:-$HOME/zephyrproject}"/build*/logic/openocd.cfg /tmp/b_*/logic/openocd.cfg 2>/dev/null | head -1)
		[ -n "$SUPPORT_CFG" ] || SUPPORT_CFG="$_cand/boards/agm/$BOARD/support/openocd.cfg"
		break
	fi
done

# The board cfg sources the SDK's agrv2k.cfg for the adapter/target setup and
# the `agrv` flash driver. When the SDK is not installed, fall back to this
# module's own SDK-free target cfg: same target, no flash driver -- enough for
# everything below. AGM_OPENOCD_CFG overrides either choice.
MINIMAL_CFG="$AGM_HOME/boards/agm/agrv2k/shared/support/agrv2k-minimal.cfg"
SDK_CFG="$AGRV_SDK_PATH/platforms/AgRV/etc/agrv2k.cfg"
if [ -n "${AGM_OPENOCD_CFG:-}" ]; then
	OPENOCD_CFG=$AGM_OPENOCD_CFG
elif [ -f "$SDK_CFG" ]; then
	OPENOCD_CFG=$SUPPORT_CFG
else
	OPENOCD_CFG=$MINIMAL_CFG
fi

echo "=== usb / console ============================================"
lsusb 2>/dev/null | grep -i "cafe:" || echo "  (no cafe:1001 probe on the bus!)"
ls -l /dev/ttyACM* 2>/dev/null || echo "  (no /dev/ttyACM*)"

echo
echo "=== device + option bytes (SDK oo -i) ======================="
"$SELF_DIR/agm_oo.sh" info 2>&1 |
	sed -n '/AGRV2K - Rev/,$p' | grep -vE "^shutdown|^cpu halted" || true

echo
echo "=== SWD setup the board cfg will use ========================"
if [ -n "$SUPPORT_CFG" ]; then
	echo "cfg:   $SUPPORT_CFG"
	grep -E "^[[:space:]]*(set ADAPTER_SPEED|if \{ !\[info exists ADAPTER_SPEED\]|cmsis-dap backend usb_bulk)" \
		"$SUPPORT_CFG" | sed 's/^[[:space:]]*/  /'
	if [ "$OPENOCD_CFG" != "$SUPPORT_CFG" ]; then
		echo "  (SDK cfg missing -> using $(basename "$OPENOCD_CFG") instead)"
	fi
else
	echo "cfg:   $OPENOCD_CFG"
fi

echo
echo "=== SoC registers via the debugger =========================="
if [ -n "$OPENOCD_CFG" ] && [ -f "$OPENOCD_CFG" ]; then
	# The examine fails intermittently on this dev board (2 runs in 6
	# with and without the SDK cfg), so retry a few times.
	for attempt in 1 2 3 4; do
		out=$(AGRV_ADAPTER=${AGRV_ADAPTER:-cmsis-dap} \
		"$AGM_OPENOCD_BIN/openocd_cmd" -s "$AGRV_SDK_PATH/platforms/AgRV/etc" \
			-c "variable ADAPTER cmsis-dap" \
			-c "variable ADAPTER_SPEED ${AGM_SWD_SPEED:-10000}" \
			-f "$OPENOCD_CFG" \
			-c "init" -c "halt" \
			-c "mdw 0x40010010 1" \
			-c "mdw 0x03000060 1" \
			-c "mdw 0x03000004 1" \
			-c "shutdown" 2>&1) || true
		case "$out" in
		*"Examination succeed"*) break ;;
		esac
		echo "  (examine attempt $attempt failed -- retrying)"
	done
	echo "  (0x40010010 = FCB STAT, 0x03000060 = SYS.APB_CLKENABLE,"
	echo "   0x03000004 = SYS.RST_CNTL)"
	printf '%s\n' "$out" | grep -E "^0x[0-9a-f]{8}:" | sed 's/^/  /'
fi
