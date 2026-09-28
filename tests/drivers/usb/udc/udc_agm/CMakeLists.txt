# SPDX-License-Identifier: Apache-2.0
#
# native_sim suite for drivers/usb/udc/udc_agm.c (-13 (b)).
#
# The first suite whose model needs a *behaviour* hook rather than just RAM:
# usb_reset_controller() sets USBCMD.RST and polls until the controller clears
# it, and a RAM window never would -- so src/main.c runs a 1 ms timer that
# plays the controller for exactly that bit. Everything after it is register
# programming, which the cases read back through the real addresses.

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(udc_agm_native)

set(AGM_NATIVE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../../common/agm_native)

zephyr_include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../../../../../soc/agm/agrv2k)

target_sources(app PRIVATE
	src/main.c
	${AGM_NATIVE_DIR}/agm_fake_mmio.c
)
