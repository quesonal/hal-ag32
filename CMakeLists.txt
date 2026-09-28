# SPDX-License-Identifier: Apache-2.0
#
# hal_ag32 — Zephyr module CMake entry point (zephyr/module.yml
# declares `build.cmake: .`, so Zephyr pulls this file in).
#
# We register the SoC glue (soc/) and the AGM driver sources (drivers/)
# and let the build system pick up boards/, dts/, dts/bindings/ via the
# dts_root / soc_root / board_root settings in module.yml.
#
# drivers/CMakeLists.txt holds the CONFIG_<X>_AGM source hooks that used
# to be inserted into zephyr/drivers/<subsys>/CMakeLists.txt; the matching
# Kconfig files are rsourced from zephyr/Kconfig. Neither the zephyr tree
# nor its drivers/ tree carries AGM-specific lines any more.
#
# If a future SoC change adds an init routine that must run before
# the kernel starts, add it to soc/agm/agrv2k/ — drivers will then
# see the symbols automatically.

add_subdirectory(soc)
add_subdirectory(drivers)

# include/ mirrors the Zephyr include tree: it holds both the AGM_PINCTRL()
# cell encoding (include/zephyr/dt-bindings/pinctrl/agm-agrv2k-pinctrl.h --
# the dtsi gets the cells from there and pinctrl.c has to decode the same
# numbers, so one definition) and the public driver API headers
# (include/zephyr/drivers/misc/cpld_agm.h). Applications include the latter
# as <zephyr/drivers/misc/cpld_agm.h>, so the directory goes on the global
# (zephyr_interface) include path rather than libzephyr's private one.
zephyr_include_directories(${CMAKE_CURRENT_SOURCE_DIR}/include)
