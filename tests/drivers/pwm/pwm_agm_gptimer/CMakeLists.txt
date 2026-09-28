# SPDX-License-Identifier: Apache-2.0
#
# native_sim suite for drivers/pwm/pwm_agm_gptimer.c (second register model).
#
# The GPTIMER is the cheapest peripheral to model in this family: the PWM driver
# only *writes* registers (CR1/CCMR/CCER/ARR/CCRx/EGR/BDTR) and never polls a
# status bit, so a RAM-backed window is a complete model -- no behaviour hook is
# needed. That makes it the right place to pin the CCER/CCMR bit arithmetic and
# the channel bounds check (the masks used to be built before the range check, which is a >= 32-bit shift on a rejected channel).

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(pwm_agm_gptimer_native)

set(AGM_NATIVE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../common/agm_native)

# The driver's init applies its pinctrl state through the SoC's
# pinctrl_configure_pins(), and includes <agm_sys.h>: both live in
# soc/agm/agrv2k, which is only compiled for an AgRV2K board.
zephyr_include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../../../../soc/agm/agrv2k)

target_sources(app PRIVATE
	src/main.c
	${AGM_NATIVE_DIR}/agm_fake_mmio.c
	${CMAKE_CURRENT_SOURCE_DIR}/../../../../soc/agm/agrv2k/pinctrl.c
)
