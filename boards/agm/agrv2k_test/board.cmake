# SPDX-License-Identifier: Apache-2.0
#
# AgRV2K Test board.cmake — per-board hardware parameters for the
# 100 MHz / 256 KB flash SDK test fixture variant. All shared build
# glue (bitstream generation, runner registration, flash + flash-logic
# targets, support/openocd.cfg rendering) lives in agrv2k/shared/board_common.cmake.

set(BOARD_DIR ${CMAKE_CURRENT_LIST_DIR})

# --- Per-board hardware parameters ---------------------------------
# SYSCLK 100 MHz per board.ve line 1, 256 KB on-die FLASH per board.json.
set(AGRV_FLASH_SIZE     "0x40000")    # 256 KB
set(AGRV_FLASH_SIZE_KB  256)          # for check_device_id proc arg
set(AGRV_BITSTREAM_ADDR "0x80027000") # FLASH end - 100 KB reserved

# --- Shared build glue ----------------------------------------------
include(${CMAKE_CURRENT_LIST_DIR}/../agrv2k/shared/board_common.cmake)