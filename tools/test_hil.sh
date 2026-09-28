#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
#
# test_hil.sh — run the on-board (twister device-testing) cases.
#
# Why this exists: the commit gate is
#
#     west twister -T modules/hal_ag32_samples/samples -p agrv2k_407 --build-only
#
# and that never *runs* anything. That is exactly how the AHB-gate
# regression stayed green in every single configuration while DMAC0, USB0 and
# EMAC sat behind a closed gate on the dev board: `dma_memcpy` failed, the SPI
# driver's long RX timed out with -ETIMEDOUT, and no build said a word. So
# changes to a driver -- and to the SoC bring-up it depends on (clocks, gates,
# resets, DMA) -- want a run on hardware, not just a build.
#
# The scenarios are the samples that print a verdict; the harness matches that
# line (the sample workflow has the table):
#
#   hello_world   the board+console+bitstream canary
#   dma_memcpy    DMAC0 (behind the AHB gate)
#   spi_flash_rw  SPI engine + its TX/RX DMA  -- erases NOR sector 0!
#   rtc_alarm     LSE 1 Hz counter + three alarms        (~20 s)
#   wdt_feed      WDOG0: INT-only, then the reset        (~25 s)
#   iwdg_basic    backup-domain IWDG, self-confirmed     (~12 s)
#   usb_host_bringup  USB0 host-mode bring-up: USBMODE/schedules/interrupt
#                     mask/run bit/PORTSC-vs-event/disable, all of it without
#                     a device on the connector
#   usb_host_enum     the host stack + driver come up with nothing plugged in
#
# Usage
#   tools/test_hil.sh                        # all six, canonical bitstream
#   tools/test_hil.sh -p /dev/ttyACM1        # a different console
#   tools/test_hil.sh sample.dma_memcpy.hil.agm_agrv2k_407    # just one
#
# Environment
#   AGM_BITSTREAM_BIN   bitstream `west flash` writes (default: the canonical
#                       200 MHz one; without it the flash step has nothing to
#                       write to the fabric)
#   AGM_HIL_PORT        console/probe serial device (default /dev/ttyACM0)
#   AGM_HIL_JOBS        twister -j (default 1: see the note below)
#   AGM_HIL_OUT         twister -O directory (default /tmp/tw_hil)
#   AGM_WORKSPACE       west workspace holding this module (default
#                       ~/zephyrproject)
#
# Note on -j 1: twister builds in parallel by default, and a few jobs then
# configure_file() into the same source-tree artifacts at once
# (boards/agm/<board>/support/openocd.cfg, board.ve), which fails a random job
# with "CMake Error ... configure_file: No such file or directory". That race
# is documented alongside the tool; building one case at a time avoids it.

set -eu

SELF_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
WS=${AGM_WORKSPACE:-$HOME/zephyrproject}
PORT=${AGM_HIL_PORT:-/dev/ttyACM0}
JOBS=${AGM_HIL_JOBS:-1}
OUT=${AGM_HIL_OUT:-/tmp/tw_hil}
BITSTREAM=${AGM_BITSTREAM_BIN:-$HOME/spi_full_mac_bitstream_200mhz/example_board.bin}
BOARD=${AGM_HIL_BOARD:-agrv2k_407}

DEFAULT_SCENARIOS="
sample.hello_world.hil.agm_agrv2k_407
sample.dma_memcpy.hil.agm_agrv2k_407
sample.spi_flash_rw.hil.agm_agrv2k_407
sample.rtc_alarm.hil.agm_agrv2k_407
sample.wdt_feed.hil.agm_agrv2k_407
sample.iwdg_basic.hil.agm_agrv2k_407
sample.usb_host_bringup.hil.agm_agrv2k_407
sample.usb_host_enum.hil.agm_agrv2k_407
"

if [ $# -gt 0 ]; then
	SCENARIOS="$*"
else
	SCENARIOS="$DEFAULT_SCENARIOS"
fi

[ -f "$BITSTREAM" ] || {
	echo "FATAL: no bitstream at $BITSTREAM" >&2
	echo "       set AGM_BITSTREAM_BIN (the canonical 200 MHz one lives in" >&2
	echo "       \$HOME/spi_full_mac_bitstream_200mhz/example_board.bin)" >&2
	exit 3
}
[ -e "$PORT" ] || {
	echo "FATAL: no serial device at $PORT (set AGM_HIL_PORT; ls /dev/ttyACM*)" >&2
	exit 3
}
[ -d "$WS/modules/hal_ag32" ] || {
	echo "FATAL: $WS/modules/hal_ag32 is not a west workspace with this module" >&2
	echo "       set AGM_WORKSPACE (and sync it: git -C ... pull --ff-only)" >&2
	exit 3
}

# Twister *silently ignores* an -s it cannot find, so a stale mirror turns this
# into a two-scenario run that still reports success (: the
# mirror was one commit behind the one that added four of the scenarios). Check
# the names against the tree that is about to be built, and say so instead.
missing=""
for s in $SCENARIOS; do
	grep -Fq "$s:" "$WS"/modules/hal_ag32_samples/samples/*/sample.yaml 2>/dev/null || \
		missing="$missing $s"
done
[ -z "$missing" ] || {
	echo "FATAL: these scenarios are not in $WS/modules/hal_ag32_samples:$missing" >&2
	echo "       the mirror is stale -- sync it first:" >&2
	echo "       git -C $WS/modules/hal_ag32 pull --ff-only origin main" >&2
	exit 3
}
command -v west >/dev/null 2>&1 || {
	echo "FATAL: no west on PATH -- activate your Python venv first" >&2
	exit 3
}

echo ">>> device-testing twister: board=$BOARD port=$PORT jobs=$JOBS"
echo ">>> bitstream: $BITSTREAM"
echo ">>> scenarios:"
for s in $SCENARIOS; do
	echo "      $s"
done
echo ">>> note: spi_flash_rw erases the on-board NOR's sector 0"

# Assemble the -s arguments without relying on word splitting later.
set --
for s in $SCENARIOS; do
	set -- "$@" -s "$s"
done

cd "$WS"
AGM_BITSTREAM_BIN="$BITSTREAM" exec west twister \
	-T modules/hal_ag32_samples/samples -p "$BOARD" \
	--device-testing --device-serial "$PORT" --west-flash \
	-j "$JOBS" -O "$OUT" --clobber-output "$@"
