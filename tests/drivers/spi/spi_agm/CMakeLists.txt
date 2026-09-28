# SPDX-License-Identifier: Apache-2.0
#
# Two suites for the SPI driver, in one binary (-13):
#
#   spi_agm_logic (src/main.c)  the TX-slicing arithmetic, pure -- no device.
#       That is the code that was wrong: it sliced into
#       SPI_AGM_MAX_PHASE_BYTES-sized chunks and handed each to one phase,
#       which spi_agm_transceive_phases() rejects. Nothing in the tree calls
#       spi_agm_write_long(), so neither the 62-config build gate nor any
#       sample reaches it.
#   spi_agm_regs (src/regs.c)   the phase engine itself, against a register
#       window backed by RAM plus a 1 ms "engine" timer that completes a
#       START. That is what makes the half-duplex RX semantics checkable: the
#       engine's CS window is one phase list, so the bytes the command occupied
#       come back as 0xFF and the captured stream lands at the SPI API's slot
#       offsets (the shape upstream spi-nor depends on).
#
# Both live in one app because the DT machinery the second suite needs is
# harmless to the first (and the first must keep running without a device).

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(spi_agm_native)

set(AGM_NATIVE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../common/agm_native)

zephyr_include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../../../../soc/agm/agrv2k)

target_sources(app PRIVATE
	src/main.c
	src/regs.c
	${AGM_NATIVE_DIR}/agm_fake_mmio.c
	${CMAKE_CURRENT_SOURCE_DIR}/../../../../soc/agm/agrv2k/pinctrl.c
)
