# SPDX-License-Identifier: Apache-2.0
#
# native_sim suite for drivers/can/can_agm.c (-23).
#
# Unlike the other AGM native suites this one needs the *shared* SJA1000 state
# machine (drivers/can/can_sja1000.c) next to the AgRV frontend -- the driver is
# a thin register shim (read_reg/write_reg on a 32-bit stride) plus three local
# overrides (CDR absorb, AFM clear, tx_idle reclaim) and the ISR adapter. The
# upstream file is compiled by Zephyr itself (CONFIG_CAN_SJA1000), so nothing
# extra is listed here.

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(can_agm_native)

set(AGM_NATIVE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../common/agm_native)

# soc/agm/agrv2k/pinctrl.c is the one place that decodes the AGM_PINCTRL()
# cells; linking it makes the AFSEL/DIR assertions below production code.
zephyr_include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../../../../soc/agm/agrv2k)

target_sources(app PRIVATE
	src/main.c
	${AGM_NATIVE_DIR}/agm_fake_mmio.c
	${CMAKE_CURRENT_SOURCE_DIR}/../../../../soc/agm/agrv2k/pinctrl.c
)
