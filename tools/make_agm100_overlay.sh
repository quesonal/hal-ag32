#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
#
# make_agm100_overlay.sh — write the /tmp/agm100.overlay the docs refer to.
#
# Some dev board bitstreams run the PLL at 100 MHz (SYSCLK 100 / BUSCLK 50)
# while boards/agm/agrv2k_407/agrv2k_407.dts describes 200 MHz / 100 MHz.
# A firmware built from the board defaults then prints at 57600 while the
# console reads 115200 — garbled text, healthy board. The fix is a
# *temporary* devicetree overlay, deliberately not committed (the
# documented workflow keeps dev board clock overrides out of the tree, see
# the sample workflow 0.2), so it has to be recreated after a reboot:
#
#   source <your-venv>/bin/activate
#   bash tools/make_agm100_overlay.sh                  # /tmp/agm100.overlay
#   west build -d /tmp/b_x -b agrv2k_407 <app> \
#       -- -DEXTRA_DTC_OVERLAY_FILE=/tmp/agm100.overlay
#
# Confirm the board is at 100 MHz before using it: build one app twice (with
# and without the overlay) and capture both banners with
# tools/test_uart_capture.sh — the run with a clean printable_ratio is the
# rate the bitstream actually provides.: the board's
# current canonical bitstream runs at **200 MHz**, so it does *not* want this
# overlay; confirm the board's actual rate first.
#
# Usage: make_agm100_overlay.sh [<out.overlay>]   default /tmp/agm100.overlay

set -eu

OUT=${1:-/tmp/agm100.overlay}

cat > "$OUT" <<'EOF'
/*
 * Clock override for the 100 MHz bitstream: it runs SYSCLK 100 MHz /
 * BUSCLK 50 MHz instead of the 200/100 the board dts describes. Every
 * consumer of the rate has to agree (&clk0 for the peripheral dividers,
 * &cpu0 for the kernel tick, the pin map for board.ve's SYSCLK line, &sys
 * for the flash ceiling) — generate_board_ve.py fails the build if the pin
 * map disagrees with &clk0/&cpu0.
 *
 * Written by tools/make_agm100_overlay.sh; not committed.
 */
&clk0 {
	clock-frequency = <100000000>;
};

&cpu0 {
	clock-frequency = <100000000>;
};

&agrv2k_pins {
	sysclk-frequency = <100000000>;
};

&sys {
	flash-max-frequency = <50000000>;
};
EOF

echo "wrote $OUT"
echo "build with: -- -DEXTRA_DTC_OVERLAY_FILE=$OUT"
echo "and remember: confirm the board's rate first (two builds, two captures)."
