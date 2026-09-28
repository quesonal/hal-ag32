# SPDX-License-Identifier: Apache-2.0
#
# native_sim suite for the AgRV2K UART driver.
#
# Why this suite needs glue the boot_agm suite does not: uart_agm.c reaches the
# hardware through a raw register window (DT_INST_REG_ADDR) and calls
# pinctrl_apply_state(), which is an inline that ends in the *SoC's*
# pinctrl_configure_pins(). None of that exists on native_sim:
#
#   * pinctrl_soc.h (the pin-cell type every AGM driver's pinctrl state expands
#     through) and pinctrl_configure_pins() live in soc/agm/agrv2k, which is
#     only compiled when an AgRV2K board is the target;
#   * nothing maps the peripheral address space, so a raw dereference at
#     0x40025000 is a host access to an unmapped page (:
#     nothing is mapped there, and an mmap(MAP_FIXED) of the window makes it
#     work).
#
# So this file (a) puts the SoC's own pinctrl sources on the path, because the
# suite wants the *real* AFSEL/DIR decoding, and (b) links the shared fake-MMIO
# window (tests/drivers/common/agm_native), which maps the peripheral address
# space before any driver init. The devicetree keeps the *real* addresses, so
# the numbers under test stay the production ones.

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(uart_agm_native)

set(AGM_NATIVE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../common/agm_native)

zephyr_include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../../../../soc/agm/agrv2k)

target_sources(app PRIVATE
	src/main.c
	${AGM_NATIVE_DIR}/agm_fake_mmio.c
	${CMAKE_CURRENT_SOURCE_DIR}/../../../../soc/agm/agrv2k/pinctrl.c
)
