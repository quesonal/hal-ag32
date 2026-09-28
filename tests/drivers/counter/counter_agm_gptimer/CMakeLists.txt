# SPDX-License-Identifier: Apache-2.0
#
# native_sim suite for drivers/counter/counter_agm_gptimer.c .
#
# Same IP as the PWM suite (GPTIMER0-4), so the register window is the same
# passive model plus the two registers this driver *reads*: CNT (the counter
# value it reports and arms alarms against) and SR (the compare/wrap flags it
# clears and reports). The ISR itself is unreachable -- it needs the hardware's
# IRQ -- so the alarm path is pinned through the registers it programs.

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(counter_agm_gptimer_native)

set(AGM_NATIVE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../common/agm_native)

zephyr_include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../../../../soc/agm/agrv2k)

target_sources(app PRIVATE
	src/main.c
	${AGM_NATIVE_DIR}/agm_fake_mmio.c
)
