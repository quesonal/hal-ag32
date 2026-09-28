# SPDX-License-Identifier: Apache-2.0
#
# AgRV2K 407 board.cmake — per-board hardware parameters for the
# 240 MHz / 1 MB flash variant. All shared build glue (bitstream
# generation, runner registration, flash + flash-logic targets,
# support/openocd.cfg rendering) lives in agrv2k/shared/board_common.cmake.

set(BOARD_DIR ${CMAKE_CURRENT_LIST_DIR})

# --- Per-board hardware parameters ---------------------------------
# SYSCLK 240 MHz per board.ve line 1, 1 MB on-die FLASH per board.json.
set(AGRV_FLASH_SIZE     "0x100000")   # 1 MB
set(AGRV_FLASH_SIZE_KB  1024)         # for check_device_id proc arg
set(AGRV_BITSTREAM_ADDR "0x800e7000") # FLASH end - 100 KB reserved

# --- Shared build glue ----------------------------------------------
include(${CMAKE_CURRENT_LIST_DIR}/../agrv2k/shared/board_common.cmake)