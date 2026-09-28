# SPDX-License-Identifier: Apache-2.0
#
# native_sim suite for drivers/usb/uhc/uhc_agm.c.
#
# The model is the shared RAM window plus two behaviour hooks the driver polls
# for: USBCMD.RST has to clear itself (controller reset) and USBSTS.HCH has to
# follow USBCMD.RS (halt). Everything else in bring-up and in the queue-head
# build is register programming, which the cases read back through the real
# addresses.

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(uhc_agm_native)

set(AGM_NATIVE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../../common/agm_native)

target_sources(app PRIVATE
	src/main.c
	${AGM_NATIVE_DIR}/agm_fake_mmio.c
)
