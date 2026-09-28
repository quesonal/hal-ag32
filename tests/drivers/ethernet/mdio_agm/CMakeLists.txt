# SPDX-License-Identifier: Apache-2.0
#
# native_sim suite for drivers/ethernet/mdio/mdio_agm.c (-23).

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(mdio_agm_native)

set(AGM_NATIVE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../common/agm_native)

# agm_native also carries agm_fake_mdio_phy.h (the clause-22 PHY model).
zephyr_include_directories(${AGM_NATIVE_DIR}
	${CMAKE_CURRENT_SOURCE_DIR}/../../../../soc/agm/agrv2k)

target_sources(app PRIVATE
	src/main.c
	${AGM_NATIVE_DIR}/agm_fake_mmio.c
	${AGM_NATIVE_DIR}/agm_fake_mdio_phy.c
	${CMAKE_CURRENT_SOURCE_DIR}/../../../../soc/agm/agrv2k/pinctrl.c
)
