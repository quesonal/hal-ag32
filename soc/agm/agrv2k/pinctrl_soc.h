/*
 * Copyright (c) 2026 AgRV Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * AgRV2K SoC pinctrl helpers.
 *
 * Which pin carries which signal is baked into the FPGA bitstream
 * (.ve → .bin), but the last step of every route is a runtime register:
 * the GPIO bank's AFSEL bit hands the pin to the peripheral, and DIR says
 * whether the pin drives. A pinctrl state therefore lists those bank/bit
 * pairs, packed one cell each — see
 * include/zephyr/dt-bindings/pinctrl/agm-agrv2k-pinctrl.h for the encoding
 * and soc/agm/agrv2k/pinctrl.c for the register writes.
 *
 * Electrical attributes (bias-pull-up, drive-strength) are NOT runtime
 * registers on this SoC — the bitstream's ASF settings own them, which is
 * why the binding does not accept them. A future dts2ve pass
 * could still regenerate the bitstream from the .dts.
 */

#ifndef ZEPHYR_SOC_AGM_AGRV2K_PINCTRL_SOC_H_
#define ZEPHYR_SOC_AGM_AGRV2K_PINCTRL_SOC_H_

#include <zephyr/devicetree.h>
#include <zephyr/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* One AGM_PINCTRL() cell (bank | pin << 4 | dir << 8). */
typedef uint32_t pinctrl_soc_pin_t;

#define Z_PINCTRL_STATE_PIN_INIT(group, pin_prop, idx)                                         \
	DT_PROP_BY_IDX(group, pin_prop, idx),

/* node_id is the consumer, prop its pinctrl-N property; the state node it
 * points at carries the routes in agm,pins. */
#define Z_PINCTRL_STATE_PINS_INIT(node_id, prop)                                               \
	{ DT_FOREACH_PROP_ELEM(DT_PHANDLE(node_id, prop), agm_pins, Z_PINCTRL_STATE_PIN_INIT) }

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_SOC_AGM_AGRV2K_PINCTRL_SOC_H_ */
