#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
#
# Flash the CDC-ACM echo firmware to agrv2k_407 + verify on-target.
#
# Usage:
#   tools/usb_cdc_echo/flash_and_test.sh [<zephyr.bin>]
#
# Default <zephyr.bin> = ~/zephyrproject/zephyr/build/zephyr/zephyr.bin
#
# Steps:
#   1. Flash firmware via flash_fw.sh (which itself ends with `reset run`
#      and `shutdown`, so the new firmware is already executing on the
#      CPU when this script resumes).
#   2. Wait for the new CDC-ACM /dev/ttyACM* to enumerate on the host.
#      The debug probe tty is always /dev/ttyACM0; the fresh CDC-ACM
#      enumerates ~1 s after reset, typically /dev/ttyACM1.
#   3. Send a known string ("hello agm\n") and verify the echo.
#
# Prerequisites:
#   * agrv2k_407 board powered on, USB0 (PIN_70/71) connected to host
#     (the second USB port, NOT the CDC-ACM debug port that already
#     enumerates as the openocd probe).
#   * ~/zephyrproject set up; the build done via
#       west build -b agrv2k_407 samples/subsys/usb/cdc_acm
#   * The default bitstream (example_board.bin) flashed once; this
#     brings up USB0 in OTG mode with 60 MHz CLKOUT[1]. The driver
#     forces USBMODE.CM=device at runtime.
#
# Note: this is a pure USB echo test — `zephyr,cdc-acm-uart` is a
# virtual UART (ring-buffer-backed) on the USB bus, no physical UART
# pins are involved, so no UART0/UART1 loopback is needed.

set -e

FIRMWARE="${1:-$HOME/zephyrproject/zephyr/build/zephyr/zephyr.bin}"
HAL_HOME="${ZEPHYR_HAL_AGM_HOME:-$HOME/zephyr-hal-ag32}"
TOOLS="$HAL_HOME/tools"

if [ ! -f "$FIRMWARE" ]; then
	echo "ERROR: firmware not found: $FIRMWARE" >&2
	echo "  build with: west build -b agrv2k_407 samples/subsys/usb/cdc_acm" >&2
	exit 1
fi

echo "==> Flashing $FIRMWARE (surgical, preserves bitstream) ..."
echo "    (flash_fw.sh ends with 'reset run' + 'shutdown'; new firmware"
echo "     is already executing when this script resumes)"
"$TOOLS/flash_fw.sh" "$FIRMWARE"

echo
echo "==> Waiting for the new CDC-ACM tty (debug probe is /dev/ttyACM0,"
echo "    the echo device usually enumerates as /dev/ttyACM1) ..."
T0=$(date +%s)
ACM=""
while [ -z "$ACM" ]; do
	# Prefer the *new* CDC-ACM tty over the debug probe tty: pick the
	# highest-numbered /dev/ttyACM* (debug tty is always 0).
	for d in /dev/ttyACM3 /dev/ttyACM2 /dev/ttyACM1; do
		if [ -c "$d" ]; then ACM="$d"; break; fi
	done
	[ -n "$ACM" ] && break
	sleep 0.5
	NOW=$(date +%s)
	if [ $((NOW - T0)) -gt 10 ]; then
		echo "ERROR: no new /dev/ttyACM* appeared in 10 s" >&2
		echo "  check: 'dmesg | tail -20' and the second USB connector" >&2
		exit 1
	fi
done

echo "==> Found $ACM, sending probe ..."
stty -F "$ACM" 115200 raw -echo 2>/dev/null || \
	stty -f "$ACM" 115200 raw -echo 2>/dev/null
# Drain any prior junk
dd if=/dev/zero of="$ACM" bs=1 count=0 2>/dev/null
sleep 0.2

PROBE="hello agm $(date +%H%M%S)\r"
printf '%s' "$PROBE" > "$ACM"
sleep 0.5
RESP=$(timeout 2 dd if="$ACM" bs=64 count=1 2>/dev/null | tr -d '\0')
echo "==> Sent:     $PROBE"
echo "==> Received: $RESP"
if printf '%s' "$RESP" | grep -q "hello agm"; then
	echo "==> PASS: CDC-ACM echo verified on $ACM"
	exit 0
else
	echo "==> FAIL: no echo match. Likely causes:" >&2
	echo "   - bitstream's USB0 not clocked (check 'openocd mdw 0x41001000')" >&2
	echo "   - DP/DM pins not wired on your 407 PCB revision" >&2
	echo "   - CMSIS-DAP debug tty already grabbed (use /dev/ttyACM1+)" >&2
	echo "   - UDC driver dQH/dTD not initialized (read PORTSC at 0x41001184)" >&2
	exit 2
fi
