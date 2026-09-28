/*
 * Copyright (c) 2026 AgRV Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * AgRV2K SoC-level definitions.
 *
 * This header is the single point of integration between Zephyr
 * drivers (which #include <soc.h>) and AgRV2K register layout.
 *
 * The AgRV2K pin mux is FPGA-baked (board.ve → board.bin), so the
 * standard pin-control fields are placeholders. Per-bank register
 * offsets for the Stellaris-compatible GPIO blocks are defined in
 * gpio_stellaris.c — they match TI's LM3S layout.
 *
 * Real on-die peripherals (PL011, PLIC, CLINT) live at the addresses
 * listed in soc.c and dts/riscv/agm/agrv2k.dtsi; drivers access them
 * through their own reg=<> bindings, not through this header.
 */

#ifndef ZEPHYR_SOC_AGM_AGRV2K_SOC_H_
#define ZEPHYR_SOC_AGM_AGRV2K_SOC_H_

#ifdef __cplusplus
extern "C" {
#endif

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_SOC_AGM_AGRV2K_SOC_H_ */