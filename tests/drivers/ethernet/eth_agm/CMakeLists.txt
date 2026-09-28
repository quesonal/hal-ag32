# SPDX-License-Identifier: Apache-2.0
#
# native_sim suite for drivers/ethernet/eth_agm.c (-23, ETH half).
#
# Like the other AGM suites this one links the shared fake-MMIO helper and the
# SoC's own pinctrl_configure_pins(), and keeps the *real* MAC address
# (0x41040000) in its overlay. What is new here is the net stack: the driver is
# a NET_DEVICE_DT_INST_DEFINE() device, so the suite runs it inside Zephyr's
# Ethernet L2 rather than calling it as a bare driver.

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(eth_agm_native)

set(AGM_NATIVE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../common/agm_native)

zephyr_include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../../../../soc/agm/agrv2k)

target_sources(app PRIVATE
	src/main.c
	${AGM_NATIVE_DIR}/agm_fake_mmio.c
	${CMAKE_CURRENT_SOURCE_DIR}/../../../../soc/agm/agrv2k/pinctrl.c
)
