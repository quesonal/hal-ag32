# SPDX-License-Identifier: Apache-2.0
#
# native_sim suite for drivers/counter/counter_agm_rtc.c (-13 (b)).
#
# The backup-domain RTC is the one peripheral here whose model has to be armed
# *before* the driver's init: every write is followed by a wait for CRL.RTOFF,
# and a RAM window answers 0, so a bare mapping would make the driver spin its
# full AGM_RTC_WAIT_SPINS (100 000 iterations) on every single write. src/main.c
# therefore sets CRL.RTOFF from its own SYS_INIT(EARLY, 1) -- after the shared
# window is mapped (EARLY, 0), before PRE_KERNEL_1 device init. That is the
# whole trick: the wait is bounded either way, but with the bit set the
# sequencing under test runs at full speed.

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(counter_agm_rtc_native)

set(AGM_NATIVE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../common/agm_native)

zephyr_include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../../../../soc/agm/agrv2k)
zephyr_include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../../../../include/zephyr/drivers/counter)

target_sources(app PRIVATE
	src/main.c
	${AGM_NATIVE_DIR}/agm_fake_mmio.c
)
