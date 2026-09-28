# SPDX-License-Identifier: Apache-2.0
#
# Thin CMake include for the agrv32flash west runner.
#
# Mirrors the layout of upstream Zephyr's openocd.board.cmake, jlink.board.cmake,
# etc.: just registers a default value for one runner argument so the runner
# doesn't have to specify --skip-bitstream explicitly on every invocation.
#
# All actual flashing logic lives in
#   modules/hal_ag32/zephyr/scripts/west_commands/runners/agrv32flash.py
#
# Not the board default any more : board_common.cmake picks
# agrv_openocd, which writes firmware + bitstream and has the
# --skip-bitstream / --bitstream-only pair. This runner stays as the
# no-probe fallback and keeps its own --skip-bitstream default: the west
# flash pipeline on this path ONLY writes the MCU firmware (zephyr.bin →
# FLASH @ 0x80000000), so the bitstream already in FLASH survives
# untouched. Quartus synthesis never happens under west.
#
# To write both bitstream + firmware in one step, opt back in with
# --write-bitstream:
#
#   west flash --runner agrv32flash --write-bitstream
#
# If both --write-bitstream and --skip-bitstream are passed on the CLI,
# --skip-bitstream wins (see agrv32flash.py).

board_set_flasher_ifnset(agrv32flash)

# Default: skip bitstream. User flashes via Quartus Programmer.
board_runner_args(agrv32flash
	"--skip-bitstream"
)
board_finalize_runner_args(agrv32flash)
