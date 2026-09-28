# SPDX-License-Identifier: Apache-2.0
#
# hal_ag32 — AGM driver sources owned by the module.
#
# This file is where the driver hooks that used to be inserted (in sorted
# order) into zephyr/drivers/<subsys>/CMakeLists.txt now live.
#
# Why zephyr_library_*() works here: zephyr/CMakeLists.txt:780-793 pulls
# each module in with add_subdirectory() *after*
# `zephyr_library_named(zephyr)` (zephyr/CMakeLists.txt:122), so at this
# point ${ZEPHYR_CURRENT_LIBRARY} is `zephyr`. Sources added below land in
# the same libzephyr the in-tree drivers compile into, and
# zephyr_library_include_directories() is PRIVATE to that target — same
# semantics as calling these from zephyr/drivers/<subsys>/.
#
# Every source is compile-time gated on its CONFIG_<X> symbol, and those
# symbols live in the module's own Kconfig (zephyr/Kconfig rsources
# drivers/<subsys>/Kconfig.agm), so the in-tree
# `# zephyr-keep-sorted-start` blocks stay byte-identical to upstream and
# `git -C <zephyr> diff origin/main` stays empty.
#
# Two drivers consume *in-tree private* headers that must not be forked
# (their .c counterparts stay in-tree, so a copy would duplicate the type
# definitions): can/can_agm.c needs drivers/can/can_sja1000{,_priv}.h and
# usb/udc/udc_agm.c needs drivers/usb/udc/udc_common.h. Those include
# directories are added explicitly below rather than relying on the
# in-tree hook lines that happen to add them for other drivers.

# zephyr-keep-sorted-start
zephyr_library_sources_ifdef(CONFIG_BOOT_AGM misc/boot_agm.c)
zephyr_library_sources_ifdef(CONFIG_BOOT_AGM_BIND misc/boot_agm_bind.c)
zephyr_library_sources_ifdef(CONFIG_BOOT_AGM misc/boot_agm_auth.c)
zephyr_library_sources_ifdef(CONFIG_BOOT_AGM misc/boot_agm_verify.c)
zephyr_library_sources_ifdef(CONFIG_BOOT_AGM_AN3155 misc/boot_agm_an3155.c)
zephyr_library_sources_ifdef(CONFIG_BOOT_AGM_SMP misc/boot_agm_smp.c)
zephyr_library_sources_ifdef(CONFIG_CAN_AGRV2K can/can_agm.c)
zephyr_library_sources_ifdef(CONFIG_COUNTER_AGM_GPTIMER counter/counter_agm_gptimer.c)
zephyr_library_sources_ifdef(CONFIG_COUNTER_AGM_RTC counter/counter_agm_rtc.c)
zephyr_library_sources_ifdef(CONFIG_COUNTER_AGM_TIMER counter/counter_agm_timer.c)
zephyr_library_sources_ifdef(CONFIG_CRC_AGM crc/crc_agm.c)
zephyr_library_sources_ifdef(CONFIG_DMA_AGM dma/dma_agm.c)
zephyr_library_sources_ifdef(CONFIG_ETH_AGM ethernet/eth_agm.c)
zephyr_library_sources_ifdef(CONFIG_FLASH_AGM flash/flash_agm.c)
zephyr_library_sources_ifdef(CONFIG_MDIO_AGM ethernet/mdio/mdio_agm.c)
zephyr_library_sources_ifdef(CONFIG_PHY_AGM_LAN8720 ethernet/phy/phy_agm_lan8720.c)
zephyr_library_sources_ifdef(CONFIG_I2C_AGM i2c/i2c_agm.c)
zephyr_library_sources_ifdef(CONFIG_CPLD_AGM misc/cpld_agm.c)
zephyr_library_sources_ifdef(CONFIG_CPLD_AGM_SHELL misc/cpld_agm_shell.c)
zephyr_library_sources_ifdef(CONFIG_PWM_AGM_GPTIMER pwm/pwm_agm_gptimer.c)
zephyr_library_sources_ifdef(CONFIG_UART_AGM serial/uart_agm.c)
zephyr_library_sources_ifdef(CONFIG_SPI_AGM spi/spi_agm.c)
zephyr_library_sources_ifdef(CONFIG_UDC_AGM usb/udc/udc_agm.c)
zephyr_library_sources_ifdef(CONFIG_UHC_AGM usb/uhc/uhc_agm.c)
zephyr_library_sources_ifdef(CONFIG_WDT_AGRV2K watchdog/wdt_agm.c)
zephyr_library_sources_ifdef(CONFIG_WDT_AGRV2K_IWDG watchdog/wdt_iwdg_agm.c)
# zephyr-keep-sorted-end

# Two drivers include in-tree private headers whose .c counterparts stay
# in-tree, so a fork would duplicate the type definitions:
#   can_agm.c   -> drivers/can/can_sja1000{,_priv}.h
#   udc_agm.c   -> drivers/usb/udc/udc_common.h
# Add those include directories explicitly rather than relying on the
# in-tree hook lines that happen to add them for other drivers.
zephyr_library_include_directories_ifdef(CONFIG_CAN_AGRV2K ${ZEPHYR_BASE}/drivers/can)
zephyr_library_include_directories_ifdef(CONFIG_UDC_AGM ${ZEPHYR_BASE}/drivers/usb/udc)
# uhc_agm.c includes the in-tree private drivers/usb/uhc/uhc_common.h
# (uhc_get_private / uhc_submit_event / uhc_xfer_append / uhc_xfer_return),
# whose .c stays in-tree -- same situation as udc_agm above.
zephyr_library_include_directories_ifdef(CONFIG_UHC_AGM ${ZEPHYR_BASE}/drivers/usb/uhc)
# phy_agm_lan8720.c consumes the in-tree drivers/ethernet/phy/phy_mii.h
# helper (same shape as can_agm/udc_agm needing in-tree private
# headers). We don't fork it -- its .c counterpart stays in-tree.
zephyr_library_include_directories_ifdef(CONFIG_PHY_AGM_LAN8720 ${ZEPHYR_BASE}/drivers/ethernet/phy)
# The ECDSA profile verifies with MCUboot's vendored tinycrypt: Zephyr 4.4
# dropped tinycrypt from its own tree, and MCUboot carries a copy for exactly
# this. So the build needs the mcuboot module present in the workspace.
#
# Only ECDSA pulls tinycrypt in: its SHA-256 comes from the same library as the
# curve. The RSA profile verifies *and* hashes through PSA, so linking
# tinycrypt/sha256.c there would be a second SHA-256 in the same image
# (~1426 B; dropping it nets -496 B of ROM, see
# drivers/misc/Kconfig.agm).
#
# The signed *fabric* (CONFIG_BOOT_AGM_BITSTREAM_SIGNED) always pulls it in,
# whatever the application profile is: the fabric is verified from the boot
# path, before the kernel heap exists, so it has to use the allocation-free
# tinycrypt P-256 verifier -- an RSA build that signs the fabric links both
# backends (see include/zephyr/drivers/misc/agm_bitstream.h).
#
# The command authorization (CONFIG_BOOT_AGM_LOCK_PRODUCTION) uses the
# same verifier over a host-supplied signature: a locked build that could not
# verify its commands would be a board nobody can update again.
if(CONFIG_BOOT_AGM_SIG_ECDSA_P256 OR CONFIG_BOOT_AGM_BITSTREAM_SIGNED OR
   CONFIG_BOOT_AGM_LOCK_PRODUCTION OR CONFIG_BOOT_AGM_BIND)
	if(NOT DEFINED ZEPHYR_MCUBOOT_MODULE_DIR)
		message(FATAL_ERROR
			"CONFIG_BOOT_AGM_SIG_ECDSA_P256 / _BITSTREAM_SIGNED / _LOCK_PRODUCTION need \
the mcuboot module \
(ZEPHYR_MCUBOOT_MODULE_DIR is not set): its vendored tinycrypt provides the \
SHA-256 and the ECDSA P-256 verification. Fetch it with `west update`.")
	endif()

	set(AGM_TINYCRYPT ${ZEPHYR_MCUBOOT_MODULE_DIR}/ext/tinycrypt/lib)
	# sha256.c needs utils.c's _copy/_set/_compare.
	zephyr_library_sources(
		${AGM_TINYCRYPT}/source/sha256.c
		${AGM_TINYCRYPT}/source/utils.c
		${AGM_TINYCRYPT}/source/ecc.c
		${AGM_TINYCRYPT}/source/ecc_dsa.c
		${AGM_TINYCRYPT}/source/ecc_platform_specific.c
	)
	zephyr_library_include_directories(${AGM_TINYCRYPT}/include)
endif()
