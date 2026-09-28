# SPDX-License-Identifier: Apache-2.0
#
# native_sim suite for drivers/counter/counter_agm_timer.c (-13
# (b)); the SP804-style dual timer, TIMER0/1 at 0x4001e000/0x4001f000.

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(counter_agm_timer_native)

set(AGM_NATIVE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../common/agm_native)

zephyr_include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../../../../soc/agm/agrv2k)

target_sources(app PRIVATE
	src/main.c
	${AGM_NATIVE_DIR}/agm_fake_mmio.c
)
