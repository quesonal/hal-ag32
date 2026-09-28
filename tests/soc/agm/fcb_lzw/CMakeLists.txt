# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(fcb_lzw)

target_sources(app PRIVATE src/main.c)

# The decoder under test, straight from the module (the SoC's own CMakeLists
# would only add it for an AgRV2K target, and this test runs on native_sim).
target_sources(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../../../soc/agm/agrv2k/fcb_lzw.c)
target_include_directories(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../../../soc/agm/agrv2k)

# Embed the compressed fixture so the
# test needs no tooling at build time, the same convention as the signed-image
# fixtures under tests/drivers/misc/boot_agm_*.
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/example_board.compressed.bin" _hex HEX)
string(REGEX REPLACE "(..)" "0x\\1," _bytes "${_hex}")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/fx_compressed.c"
"/* Generated from fixtures/example_board.compressed.bin -- do not edit. */
#include <stddef.h>
const unsigned char agm_compressed_bitstream[] = {
${_bytes}
};
const size_t agm_compressed_bitstream_len = sizeof(agm_compressed_bitstream);
")
target_sources(app PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/fx_compressed.c")
