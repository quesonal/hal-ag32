# SPDX-License-Identifier: Apache-2.0
#
# native_sim suite for drivers/watchdog/wdt_agm.c (-13 (b)).
#
# The SP805 is write-mostly from the driver's point of view: LOAD/CTRL/INTCLR
# are programmed and only MIS is ever read (in the ISR, which this suite cannot
# reach without a real interrupt). A RAM-backed window is therefore a complete
# model, and the assertions read back exactly what the driver programmed -- plus
# the ONE register outside the block that a WDT option touches: SYS.APB_CLKSTOP,
# which the shared fake window maps too.

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(wdt_agm_native)

set(AGM_NATIVE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../common/agm_native)

zephyr_include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../../../../soc/agm/agrv2k)

target_sources(app PRIVATE
	src/main.c
	${AGM_NATIVE_DIR}/agm_fake_mmio.c
)
