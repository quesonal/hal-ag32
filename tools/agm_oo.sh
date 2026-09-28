#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
#
# agm_oo.sh — drive the AgRV SDK's own OpenOCD runner (the vendor path).
#
# <AgRV_pio>/platforms/AgRV/etc/oo is what PlatformIO's upload / lock /
# unlock / wipe / opterase targets call. Unlike a hand-written openocd
# invocation it also knows about the AgRV2K flash controller's option-byte
# and write-protection registers (0x8100_0020+): those are NOT on the
# system bus, so `flash protect off` and system-bus reads fail on them,
# while `oo` reaches them through the AP path. See
# the board's own probing for the episode that made this matter.
#
# Usage:
#   agm_oo.sh info                          device / option byte / write
#                                           protect / FPGA address readback
#   agm_oo.sh options-erase                 erase option bytes (clears the
#                                           bogus "protected" state -- write
#                                           the bitstream again afterwards!)
#   agm_oo.sh unlock                        disable read protection (+erase)
#   agm_oo.sh lock                          enable read protection (RDP);
#                                           read the header before running it --
#                                           unlocking is the only way
#                                           back and it erases the chip
#   agm_oo.sh read-protect                  same as `lock`, but meant for a
#                                           combined run (oo runs it last)
#   agm_oo.sh fw <file.bin> [addr]          write+verify firmware
#                                           (default 0x80000000)
#   agm_oo.sh bitstream <file.bin> [addr]   write the FPGA configuration
#                                           (default 0x800e7000, sets OPTBY)
#   agm_oo.sh read <file> <length> [addr]   read flash into a file
#   agm_oo.sh erase-all                     erase the whole flash
#   agm_oo.sh raw <oo args...>              pass anything else through
#
# For the same option area over the ROM bootloader (BOOT0 high, no probe),
# including the FPGA config address this wrapper can only reach through
# `bitstream` above, see tools/rom_opt.py (`show` / `save` / `restore` /
# `set-fpga`).
#
# Environment: AGM_OO, AGM_OPENOCD_BIN, AGM_SWD_SPEED, AGRV_SDK_PATH.
# The SWD speed defaults to the vendor's 10 MHz; 15 MHz (14285 kHz here)
# is what made AP accesses fail in the first place.

set -eu

usage() {
	# Print the usage block from this file's header.
	sed -n '4,30p' "$0" | sed 's/^# \{0,1\}//'
	exit 2
}

if [ -z "${VIRTUAL_ENV:-}" ]; then
	echo "FATAL: no Zephyr virtualenv active - activate the repo venv first" >&2
	exit 3
fi

AGRV_SDK_PATH=${AGRV_SDK_PATH:-$HOME/AgRV_pio}
AGM_OO=${AGM_OO:-$AGRV_SDK_PATH/platforms/AgRV/etc/oo}
AGM_OPENOCD_BIN=${AGM_OPENOCD_BIN:-$AGRV_SDK_PATH/packages/tool-agrv_openocd/bin}
AGM_SWD_SPEED=${AGM_SWD_SPEED:-10000}

if [ ! -f "$AGM_OO" ]; then
	echo "FATAL: the SDK's oo runner is not at $AGM_OO" >&2
	echo "Set AGM_OO, or install the AgRV PlatformIO SDK (set AGRV_SDK_PATH)." >&2
	exit 3
fi

CMD=${1:-info}
[ $# -ge 1 ] && shift

case "$CMD" in
info)
	set -- -i ;;
options-erase)
	set -- -o ;;
unlock)
	set -- -u ;;
lock)
	set -- -L ;;
read-protect)
	set -- -p ;;
fw)
	[ $# -ge 1 ] || usage
	bin=$1 addr=${2:-0x80000000}
	set -- -a "$addr" -w "$bin" ;;
bitstream)
	[ $# -ge 1 ] || usage
	bin=$1 addr=${2:-0x800e7000}
	# An uncompressed Supra bitstream is exactly FCB_AUTO_WORDS*4 = 99944 B,
	# and the Zephyr FCB driver streams raw words unless the image is built
	# with CONFIG_AGM_FCB_BITSTREAM_COMPRESSED=y. A smaller file is the SDK's
	# compressed/encrypted form (board_logic.compress/encrypt); writing one
	# for a build that does not decompress leaves the board with a dead
	# fabric (recovery: BOOT0). Same check as the west runner.
	size=$(wc -c < "$bin" | tr -d ' ')
	if [ "$size" != 99944 ] && [ "${AGM_BITSTREAM_ANY_SIZE:-0}" != 1 ]; then
		echo "FATAL: $bin is $size B; an uncompressed Supra bitstream is 99944 B." >&2
		echo "       A compressed image needs CONFIG_AGM_FCB_BITSTREAM_COMPRESSED=y" >&2
		echo "       on the device (and the factory slot moves by" >&2
		echo "       AGM_FCB_BITSTREAM_ALGO_SIZE). Set" >&2
		echo "       AGM_BITSTREAM_ANY_SIZE=1 to write it anyway." >&2
		exit 4
	fi
	set -- -F "$addr" -f "$bin" ;;
read)
	[ $# -ge 2 ] || usage
	file=$1 len=$2 addr=${3:-0x80000000}
	set -- -a "$addr" -r "$file" "$len" ;;
erase-all)
	set -- -E ;;
raw)
	[ $# -ge 1 ] || usage ;;
*)
	usage ;;
esac

echo ">>> oo $CMD: speed ${AGM_SWD_SPEED} kHz, adapter cmsis-dap" >&2
exec python3 "$AGM_OO" \
	-d "$AGM_OPENOCD_BIN" -A cmsis-dap -s "$AGM_SWD_SPEED" -I agrv2k \
	"$@"
