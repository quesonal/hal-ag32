# SPDX-License-Identifier: Apache-2.0
#
# native_sim suite for drivers/i2c/i2c_agm.c (-13 (b)).
#
# This driver is polling-only (no IRQ_CONNECT, no interrupt path), and its
# "wait for the command to finish" loop reads SR.TIP -- which RAM keeps clear,
# so a *purely passive* register window is a complete model. That makes it the
# cheapest suite in this family, and it buys the parts nothing could check
# before: the prescaler arithmetic for each bus speed, the address/data/stop
# command sequence, the NACK paths, and the AFSEL-only pinctrl state the
# open-drain pins need (NO_DIR: the IP owns the direction).

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(i2c_agm_native)

set(AGM_NATIVE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../common/agm_native)

zephyr_include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../../../../soc/agm/agrv2k)

target_sources(app PRIVATE
	src/main.c
	${AGM_NATIVE_DIR}/agm_fake_mmio.c
	${CMAKE_CURRENT_SOURCE_DIR}/../../../../soc/agm/agrv2k/pinctrl.c
)
