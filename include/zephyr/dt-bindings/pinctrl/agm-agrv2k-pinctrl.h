/*
 * Copyright (c) 2026 AgRV Contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Devicetree helpers for the AgRV2K pin controller.
 *
 * An AgRV2K "pin" in a pinctrl state is one alternate-function route: the
 * GPIO bank and bit whose AFSEL register (GPIOx + 0x420) hands the pin to a
 * peripheral, plus the pin direction (DIR, GPIOx + 0x400) that route needs.
 * The three fields are packed into a single cell so a state can list several
 * pins in a plain array:
 *
 *   bits 0..3   GPIO bank index, 0..9   (base = 0x40014000 + bank*0x1000)
 *   bits 4..7   bit within the bank, 0..7
 *   bits 8..9   pin direction: AGM_PINCTRL_INPUT   -> DIR bit = 0
 *                              AGM_PINCTRL_OUTPUT  -> DIR bit = 1
 *                              AGM_PINCTRL_NO_DIR  -> leave DIR alone
 *
 * Usage:
 *
 *   #include <zephyr/dt-bindings/pinctrl/agm-agrv2k-pinctrl.h>
 *
 *   uart0_default: uart0_default {
 *           agm,pins = <AGM_PINCTRL(7, 6, AGM_PINCTRL_OUTPUT)>,  // TX
 *                      <AGM_PINCTRL(6, 1, AGM_PINCTRL_INPUT)>;   // RX
 *   };
 *
 * NO_DIR exists for pins whose direction is owned by the peripheral rather
 * than by a GPIO-style output enable -- the I2C pins are open-drain, and the
 * I2C IP drives them; forcing DIR to 1 there would be wrong, and forcing it
 * to 0 would clear a state the IP relies on. Such a state sets AFSEL only.
 *
 * The ranges above are enforced by pinctrl_configure_pins(): a cell that
 * names a bank beyond bank 9 or a bit beyond bit 7 is rejected with -EINVAL
 * (the consumer's init fails) rather than computing a register address
 * outside the GPIO block. They cannot be checked at compile time -- the
 * pack macro may only produce arithmetic, since the value ends up in a
 * devicetree cell.
 */

#ifndef ZEPHYR_DT_BINDINGS_PINCTRL_AGM_AGRV2K_PINCTRL_H_
#define ZEPHYR_DT_BINDINGS_PINCTRL_AGM_AGRV2K_PINCTRL_H_

#define AGM_PINCTRL_INPUT  0
#define AGM_PINCTRL_OUTPUT 1
#define AGM_PINCTRL_NO_DIR 2

#define AGM_PINCTRL(bank, pin, dir)                                                            \
	((((bank) & 0xf) << 0) | (((pin) & 0xf) << 4) | (((dir) & 0x3) << 8))

#endif /* ZEPHYR_DT_BINDINGS_PINCTRL_AGM_AGRV2K_PINCTRL_H_ */
