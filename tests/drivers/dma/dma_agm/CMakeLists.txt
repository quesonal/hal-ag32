# SPDX-License-Identifier: Apache-2.0
#
# native_sim suite for drivers/dma/dma_agm.c (third register model).
#
# The DMAC is the peripheral where a passive RAM window buys the most: the
# driver's whole job is to flatten the caller's block list into an LLI chain in
# SRAM and program the channel registers, and both halves are *memory* -- so the
# test can read the chain back and check it item by item. That is what pins
# the channel bounds on every entry point and a zero-length transfer must be refused instead of enabling the channel on a stale LLI[0]),
# and it is the first off-board coverage this driver has ever had.

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(dma_agm_native)

set(AGM_NATIVE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../common/agm_native)

zephyr_include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../../../../soc/agm/agrv2k)

target_sources(app PRIVATE
	src/main.c
	${AGM_NATIVE_DIR}/agm_fake_mmio.c
)
