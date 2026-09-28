# SPDX-License-Identifier: Apache-2.0
#
# native_sim suite for drivers/crc/crc_agm.c (-13 (b), fourth
# register model).
#
# The CRC unit itself cannot be modelled cheaply -- it *is* the algorithm -- so
# this suite pins what the driver's own file header calls load-bearing and what
# a model cannot fake: the register protocol. POL/CR/RESET/INIT order and the
# bit-reversed seed (the bug that made a chunked crc32_ieee_update() chain
# degrade to "the CRC of the last chunk"), the byte-wise feed into DR, the
# one-at-a-time busy flag, and the final complement.

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(crc_agm_native)

set(AGM_NATIVE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../common/agm_native)

target_sources(app PRIVATE
	src/main.c
	${AGM_NATIVE_DIR}/agm_fake_mmio.c
)
