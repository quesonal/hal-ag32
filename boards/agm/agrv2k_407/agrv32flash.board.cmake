# SPDX-License-Identifier: Apache-2.0
#
# Thin per-board CMake include for the agrv32flash west runner.
# The actual content lives in shared/agrv32flash.board.cmake so it
# stays byte-identical across all AgRV2K variants.

include(${CMAKE_CURRENT_LIST_DIR}/../agrv2k/shared/agrv32flash.board.cmake)