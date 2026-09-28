# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(boot_agm_api)

target_sources(app PRIVATE src/main.c src/an3155.c)

# The locked scenario (CONFIG_BOOT_AGM_LOCK_PRODUCTION) signs the device's own
# nonce in the test, with the same tinycrypt the driver verifies with -- the
# sources are compiled into libzephyr by drivers/CMakeLists.txt, but their
# headers are on that library's include path, not the application's.
if(CONFIG_BOOT_AGM_LOCK_PRODUCTION)
	if(NOT DEFINED ZEPHYR_MCUBOOT_MODULE_DIR)
		message(FATAL_ERROR "CONFIG_BOOT_AGM_LOCK_PRODUCTION needs the mcuboot module")
	endif()
	target_include_directories(app PRIVATE
		${ZEPHYR_MCUBOOT_MODULE_DIR}/ext/tinycrypt/lib/include)
endif()
