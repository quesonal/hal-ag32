/*
 * Copyright (c) 2026 AgRV Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * AgRV2K pinctrl driver.
 *
 * One route, two halves, and only the second is software's:
 *
 *   1. function → pin belongs to the *bitstream* (board.ve → board.bin).
 *      The fabric's cross-bar picks the external pin a function lands on
 *      and no runtime register can move a signal to another pin. That is
 *      why a pinctrl state here carries no pin number at all, only
 *      (bank, bit) cells.
 *
 *   2. bank/bit → pin handover is ours: pinctrl_configure_pins() writes
 *      the AFSEL/DIR bits a state names. This is NOT a no-op (an earlier
 *      version of this comment claimed it was): the bitstream wires the
 *      GPIO peripheral to the pins, but each GPIO pin is still controlled
 *      by an AFSEL bit in the GPIO bank itself. AFSEL=0 means
 *      software-mode (DATA register drives the pin); AFSEL=1 means
 *      alternate-function mode (the peripheral drives the pin). Reset
 *      default is 0, so without this the UART signal never reaches PIN_68
 *      even though the UART IP is fully configured and clocked.
 *
 *      AFSEL only selects the signal *source*; the pin output driver
 *      is still gated by the bank's DIR bit. With DIR=0 the PL011
 *      completes the transfer (CR=0x301, FR.TXFE=1, `reset run` and
 *      the tick printk both running) yet a logic analyser sees no
 *      activity at all on PIN_68, and the probe's UART bridge receives
 *      0 bytes. DIR must therefore be set for the TX pin. Note the
 *      SDK's GPIO_AF_ENABLE_() macro (gpio.h) writes AFSEL only and
 *      leaves DIR to the caller, so this half of the pin setup is not
 *      something the SDK does for us.
 *
 *      Per AgRV SDK's framework-agrv_sdk/src/peripherals.h (the
 *      GPIO_AF_ENABLE macro is called from PERIPHERAL_UART_ENABLE
 *      blocks like SDK board.c:144-150):
 *        UART0_UARTRXD_AF_GPIO = 6, MASK = (1<<1)  → GPIO6 AFSEL bit 1
 *        UART0_UARTTXD_AF_GPIO = 7, MASK = (1<<6)  → GPIO7 AFSEL bit 6
 *      GPIO AFSEL register is at offset 0x420 from the bank base.
 *
 *      IMPORTANT: per SDK's GPIO_AF_ENABLE_() (gpio.h:72-77), the GPIO
 *      bank APB clock must be ENABLED around the AFSEL write — the
 *      bank's register file is gated by APB_CLKENABLE_GPIOx. Without
 *      enabling it, the AFSEL write silently no-ops and the UART
 *      signal still never reaches the pin.
 *
 *      In this Zephyr port soc.c::agrv2k_clock_init() permanently
 *      enables the APB clock of every bank that carries a routed pin
 *      (its step 4 walks the same pinctrl states this function programs,
 *      so PL011/CAN and GPIO DATA writes work without per-peripheral
 *      clock toggling). That removes the need to re-toggle the clock
 *      inside this function, but the read-modify-write AFSEL pattern is
 *      preserved to mirror the SDK macro exactly.
 *
 *      Which pins these are is devicetree data: the consumer's pinctrl-N
 *      state carries agm,pins = <AGM_PINCTRL(bank, pin, dir)>, ... (see
 *      include/zephyr/dt-bindings/pinctrl/agm-agrv2k-pinctrl.h), so
 *      re-routing the console on another board is a dtsi edit.
 */

#include <errno.h>

#include <zephyr/devicetree.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/dt-bindings/pinctrl/agm-agrv2k-pinctrl.h>
#include <zephyr/sys/util.h>
#include "agm_sys.h"

/* pinctrl_configure_pins() is called by the PL011 driver's
 * PLATFORM_INIT (pinctrl_apply_state_direct → pinctrl_configure_pins).
 *
 * Each entry is one AGM_PINCTRL() cell: the GPIO bank and bit whose AFSEL
 * register hands that pin to the peripheral, plus the direction the route
 * needs. Mirrors the GPIO_AF_ENABLE pattern in AgRV SDK
 * framework-agrv_sdk/src/peripherals.h.
 *
 * DIR is written before AFSEL so the pin is already driving when the
 * peripheral takes over: with DIR=0 the PL011 completes its transfer but a
 * logic analyser sees nothing on the pin (the SDK's GPIO_AF_ENABLE_ macro
 * leaves DIR to the caller; this half of the setup is ours).
 *
 * The bank's APB clock is opened by soc.c::agrv2k_clock_init() before any
 * driver inits, from the same pinctrl states (a gated bank silently drops
 * these writes).
 *
 * The cells are devicetree data, so out-of-range values are board bugs --
 * and an unchecked bank*0x1000 lands on a *different* peripheral's
 * registers (bank 15 = 0x40023000). They are therefore validated here and
 * rejected with -EINVAL, which propagates out of pinctrl_apply_state() and
 * fails the device's init loudly instead of corrupting unrelated MMIO.
 *
 * Registers are read-modify-write: only the routed bits are touched, so
 * pins of the same bank that stay in software mode (DATA/DIR of the
 * stellaris driver) are unaffected. The one interaction to keep in mind is
 * the opposite case -- a pin used as an alternate function *and* driven by
 * the GPIO driver would fight over DIR; the board devicetree must not do
 * that.
 */
struct agm_pin_route {
	uint8_t bank;
	uint8_t bit;
	uint8_t dir;
};

static struct agm_pin_route agm_pin_route_of(uint32_t cell)
{
	return (struct agm_pin_route){
		.bank = (uint8_t)(cell & 0xfU),
		.bit = (uint8_t)((cell >> 4) & 0xfU),
		.dir = (uint8_t)((cell >> 8) & 0x3U),
	};
}

int pinctrl_configure_pins(const pinctrl_soc_pin_t *pins, uint8_t pin_cnt,
			   uintptr_t reg)
{
	ARG_UNUSED(reg);

	for (uint8_t i = 0U; i < pin_cnt; i++) {
		const struct agm_pin_route route = agm_pin_route_of(pins[i]);
		volatile uint32_t *dir;
		volatile uint32_t *afsel;

		if (route.bank >= AGM_GPIO_BANK_COUNT ||
		    route.bit >= AGM_GPIO_PINS_PER_BANK ||
		    route.dir > AGM_PINCTRL_NO_DIR) {
			return -EINVAL;
		}

		dir = (volatile uint32_t *)(AGM_GPIO_BANK(route.bank) + AGM_GPIO_DIR_OFF);
		afsel = (volatile uint32_t *)(AGM_GPIO_BANK(route.bank) + AGM_GPIO_AFSEL_OFF);

		/* AGM_PINCTRL_NO_DIR leaves DIR alone: the peripheral (e.g. the
		 * open-drain I2C IP) owns the pin's direction. */
		if (route.dir == AGM_PINCTRL_OUTPUT) {
			*dir |= BIT(route.bit);
		} else if (route.dir == AGM_PINCTRL_INPUT) {
			*dir &= ~BIT(route.bit);
		}
		*afsel |= BIT(route.bit);
	}

	return 0;
}
