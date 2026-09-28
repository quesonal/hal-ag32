# SPDX-License-Identifier: Apache-2.0

# Gate on SOC_AGM_AGRV2K so the AgRV2K SoC sources are only added when
# an AgRV2K board is the build target. Without this guard the
# hal_ag32/zephyr/module.yml soc_root: . pulls this CMakeLists in for
# ANY build (e.g. west build -b esp32c3_devkitm), and the DT_NODELABEL
# macros explode because the AgRV2K-only nodes (usb0, can0, etc.)
# don't exist in the ESP32 devicetree.
if(CONFIG_SOC_AGM_AGRV2K)
# fcb_lzw.c is always compiled: it is a pure decoder that CONFIG_AGM_FCB_
# BITSTREAM_COMPRESSED selects at the call site in fcb.c, and tests/soc/agm/
# fcb_lzw links the same file to run it on native_sim.
zephyr_sources(soc.c pinctrl.c reset.S fcb.c fcb_lzw.c clk.c pm.c busy_wait.c)

set(SOC_LINKER_SCRIPT
	${ZEPHYR_BASE}/include/zephyr/arch/riscv/common/linker.ld
	CACHE INTERNAL "AgRV2K linker script")
endif()
