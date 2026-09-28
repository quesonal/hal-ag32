#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
#
# probe_recover.sh — the ladder for "the probe/board looks dead".
#
# Walk it top to bottom and stop when the board is back:
#
#   0. probe_state.sh          see what the debugger actually sees first
#   1. this script             clear a stale host-side usbfs claim and warm
#                              the probe up (safe, no flash)
#   2. --options-erase         the SDK's oo -o: clears option bytes when
#                              openocd *claims* "device protected" while the
#                              device reports WRPR=0. Remember: the FPGA
#                              configuration pointer goes with them, so
#                              re-flash the bitstream right after
#                              (flash_logic.sh) -- this script reminds you.
#   3. BOOT0 + agrv32flash     only reachable when the CPU is not running at
#                              all; needs BOOT0 high + BOOT1 low at power-up
#                              (dev board: BOOT1 tied to GND, BOOT0 jumper
#                              to 3.3V -- human action). The
#                              exact recipe is printed at the end.
#
# Steps 1 and 2 are also the ones that matter when the SWD AP stalls:
# 10 MHz + the CMSIS-DAP v2 backend is what the vendor uses, and it is what
# the board cfg now configures.
#
# Usage:
#   probe_recover.sh                     steps 1 (+ print the rest)
#   probe_recover.sh --options-erase     step 1 + step 2
#   probe_recover.sh -h
#
# Environment: AGRV_SDK_PATH, AGM_OO, AGM_OPENOCD_BIN, AGM_SWD_SPEED.

set -eu

DO_OPTERASE=0
for arg in "$@"; do
	case "$arg" in
	--options-erase) DO_OPTERASE=1 ;;
	-h | --help) sed -n '4,26p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
	*) echo "unknown option: $arg" >&2; exit 2 ;;
	esac
done

if [ -z "${VIRTUAL_ENV:-}" ]; then
	echo "FATAL: no Zephyr virtualenv active - activate the repo venv first" >&2
	exit 3
fi

SELF_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
AGRV_SDK_PATH=${AGRV_SDK_PATH:-$HOME/AgRV_pio}

echo "=== step 1: clear a stale host-side usbfs claim, warm the probe =="
python3 - "$SELF_DIR" "$AGRV_SDK_PATH" <<'PY'
import subprocess, sys, time
self_dir, sdk = sys.argv[1], sys.argv[2]
try:
    import usb.core
except ImportError:
    print("  (pyusb missing -- run this inside the repo venv)")
    sys.exit(0)
dev = usb.core.find(idVendor=0xCAFE, idProduct=0x1001)
if dev is None:
    print("  no cafe:1001 probe on the bus -- check the cable / replug it")
else:
    try:
        dev.reset()          # USBDEVFS_RESET: releases an abandoned claim
        print("  usb reset OK (stale usbfs claim cleared, if there was one)")
    except Exception as exc:  # noqa: BLE001 - report, do not fail
        print(f"  usb reset: {exc}")
    time.sleep(0.5)
warmup = f"{self_dir}/openocd_warmup.py"
try:
    subprocess.run([sys.executable, warmup], check=False, timeout=30)
except Exception as exc:  # noqa: BLE001
    print(f"  warmup: {exc}")
PY

if [ "$DO_OPTERASE" -eq 1 ]; then
	echo
	echo "=== step 2: erase option bytes (SDK oo -o) =================="
	"$SELF_DIR/agm_oo.sh" options-erase 2>&1 |
		grep -E "Option bytes|complete|Error" | sed 's/^/  /' || true
	echo
	echo "  !! the FPGA configuration pointer was erased with them:"
	echo "     re-run tools/flash_logic.sh <bitstream.bin> now."
fi

cat <<'EOF'

=== if it is still dead: next rungs =========================
  * SWD session: every tool reads the probe setup from the board's
    support/openocd.cfg (10 MHz + CMSIS-DAP v2 bulk). AGM_SWD_SPEED=<kHz>
    overrides the speed for a margin experiment.
  * CPU not running at all (console silent AND the FCB STAT reads 0 AND
    openocd can read DPIDR but stalls on the AP): the ROM bootloader is the
    only path that does not need the core. It needs BOOT0 high AND BOOT1
    low at power-up or restart — and "restart" has to be a real nRESET,
    not openocd's soft `reset run`, because the strap is latched on the
    4th SYSCLK rising edge . On the dev
    board BOOT1 is already tied to GND and BOOT0 is a pull-down with a
    jumper to 3.3V:

      (fit the BOOT0/PIN_94 jumper to 3.3V)  +  power-cycle
        -- or, without unplugging: tools/probe_reset_target.py
           (nRESET pulse; start the write tool IMMEDIATELY after it)
      AF=$HOME/AgRV_pio/packages/tool-agrv_flashloader/bin/agrv32flash
      $AF -b 57600 -r /tmp/rom.bin -S 0x0:16 /dev/ttyACM0     # probe it
      $AF -b 57600 -w <zephyr.bin> /dev/ttyACM0               # write
      (remove the jumper, reset)

    Symptom table: the flash controller reads back 0xdeadbeef, or the probe
    enumerates but every access stalls -- both mean SWD, not the application.
EOF
