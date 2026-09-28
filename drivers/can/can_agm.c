/*
 * Copyright (c) 2026 AgRV Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * CAN driver for the AGM AgRV2K on-die CAN controller.
 *
 * The AgRV CAN IP is register-compatible with the NXP SJA1000 in
 * PeliCAN mode (8-bit register view, same MOD/CMR/SR/IR/IER/BTR0/BTR1
 * layout, same ACR/AMR reset-mode filter window, same TX/RX frame
 * buffer window at reg 16..28). The SJA1000 8-bit registers are
 * mapped on a 32-bit word stride: byte address `reg` lives at
 * hardware offset `reg * 4`, low byte = the SJA1000 register value.
 *
 * The AgRV IP also exposes:
 *   - MOD_SELFTEST (bit 2) — self-test mode (transmit + self
 *     reception, no ACK from a peer needed). Used by the
 *     samples/can_loopback sample. NOTE: it still needs a bus the
 *     controller reads as idle/recessive. With nothing attached,
 *     PIN_38 (RX0) reads dominant and the transmitter never starts
 *     (SR.TS stuck, no error counters): same symptom as a floating
 *     RX0. The PASSes were taken with the
 *     CAN transceiver on the pins (2x SN65HVD230 + ESP32-C3 peer, see
 *     samples/can_pin_drive).
 *   - MOD_LISTONLY (bit 1) — listen-only mode (no ACK, no TX).
 *
 * AgRV-specific differences vs. real SJA1000:
 *   - CDR (reg 31, physical 0x7C) exists but the SDK struct labels it
 *     RESERVED2. It resets to 0xC0 = PeliCAN mode + CBP set, and
 *     CDR[7:4] are fixed read-only 1100; the shared SJA1000 init's
 *     write of config->cdr | CAN_MODE (0x80) only touches fixed bits,
 *     so we absorb it in write_reg() and leave the hardware state.
 *   - APB clock gate at SYS+0x60 bit 26 (matches SDK APB_MASK_CAN0).
 *   - TX0/RX0 pins must be switched to alternate-function mode (the SDK
 *     does this in PERIPHERAL_CAN_ENABLE via GPIO_AF_ENABLE(CAN0_RX0/
 *     TX0)); without it the pins stay in GPIO mode and the CAN IP never
 *     sees the bus. That route is not open-coded here any more: the
 *     devicetree pinctrl state `can0_default` carries both pins as
 *     AGM_PINCTRL() cells and the driver applies it with
 *     pinctrl_apply_state(), exactly like the PL011 driver does for
 *     uart0. One model, one place that writes AFSEL/DIR
 *     (soc/agm/agrv2k/pinctrl.c), and soc.c opens the banks' APB clocks
 *     from the same state.
 *
 *     Measured facts worth keeping across refactors:
 *       - DIR is a don't-care for CAN: clearing GPIO8 DIR bit 7 on a live
 *         125 kbit/s link did not change the peer's frame
 *         rate at all (260 frames / 13 s either way), and the vendor SDK
 *         never writes DIR for CAN. The state asks for OUTPUT on TX0
 *         anyway, matching the UART0 pattern; harmless either way.
 *       - the 407 fabric inverts the pin output enable for CAN0_TX0 only
 *         (Quartus project: "assign PIN_39_out_en =
 *         !gpio8_io_out_en[7];", gen_vlog's REV_OUT_EN = ['CAN0_TX0']).
 *         PIN_39 has been measured to drive in AF mode with either DIR
 *         polarity, so if a scope ever shows it tri-stated, try
 *         AGM_PINCTRL_INPUT for that cell first.
 *
 * Driver structure:
 *   - Thin frontend: read_reg / write_reg map byte addresses to the
 *     32-bit AgRV register file.
 *   - All CAN state machine logic (init, set_timing, send,
 *     add/remove_rx_filter, get_state, ISR, recovery) comes from the
 *     Zephyr shared can_sja1000.c driver, exactly like the Bouffalo Lab
 *     BL61x frontend (can_bflb_bl61x.c) does. Two methods are
 *     overridden, neither because of an IP bug:
 *       - set_mode() and init(), which clear MOD.AFM (see below); not
 *         required for TX/RX. init() has to do it separately because the
 *         shared init programs its first NORMAL mode through the shared
 *         set_mode(), not through this vfunc.
 *       - send(), a thin wrapper that only reclaims the tx_idle permit
 *         that upstream can_sja1000_send() leaks on its -EIO path
 *         (transmit buffer locked); see can_agm_send() below.
 *
 * On-target verification:
 *   The can_loopback sample sets CAN_MODE_LOOPBACK (MOD.STM), sends a
 *   frame with CMR.SRR, and the IP delivers it back through RX to the
 *   RX callback. On agrv2k_407 with MOD=0x05 read back: id=0x123 dlc=8
 *   payload intact, TX complete via IR.TI.
 *
 *   That PASS was taken with a transceiver on the pins (2x SN65HVD230 +
 *   ESP32-C3 peer). An idle bus is a precondition, not an extra: with
 *   nothing attached the same binary FAILs (can_loopback: tx_done=0, then
 *   -EAGAIN on the following sends). The register dump after the run reads PIN_38 /
 *   GPIO7 bit 3 = 0 (dominant, the pin floats low), SR = 0x34 (TBS |
 *   TCS | TS: transmitter stuck), IR = 0x00 and RXERR = TXERR = 0 — the
 *   "never sees bus-free" symptom samples/can_pin_drive documents, not a
 *   driver fault: an earlier revision of this file reproduces
 *   the identical failure. MOD.STM does not lift that precondition on
 *   this IP. Native coverage of the driver logic:
 *   tests/drivers/can/can_agm/.
 */

#define DT_DRV_COMPAT agm_agrv2k_can

#include "can_sja1000.h"
#include "can_sja1000_priv.h"

#include <zephyr/irq.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/sys/sys_io.h>

#include <agm_sys.h>

LOG_MODULE_REGISTER(can_agm, CONFIG_CAN_LOG_LEVEL);

/* SYS controller / GPIO addresses come from agm_sys.h (shared with the
 * other drivers and soc.c —).
 *
 * CAN0 TX0/RX0 pin routing is NOT hardcoded here and no longer derived
 * from ad-hoc properties: the devicetree pinctrl state carries it.
 *
 *   can0 { pinctrl-0 = <&can0_default>; }          (agrv2k.dtsi)
 *   can0_default { agm,pins = <AGM_PINCTRL(8, 7, AGM_PINCTRL_OUTPUT)>,
 *                              <AGM_PINCTRL(7, 3, AGM_PINCTRL_INPUT)>; }
 *   can0 { agm,apb-clkenable-bit = <26>; }         (CAN0's own gate)
 *
 * Like UART0  and I2C0/1 , the bitstream wires the pin to
 * the GPIO block: the pin only reaches the CAN IP once its AFSEL bit
 * (GPIOx + 0x420) selects alternate-function mode. pinctrl_configure_pins()
 * writes AFSEL (+ DIR, a don't-care in AF mode -- see the file header) and
 * soc.c opens the banks' gates from the same state. Changing the route is
 * a devicetree edit, and there is exactly one implementation of it in the
 * tree (`soc/agm/agrv2k/pinctrl.c`).
 */

/* SJA1000 8-bit registers are mapped on a 32-bit word stride. */
#define CAN_AGM_REG_STRIDE        4U
#define CAN_AGM_REG_MASK          0xFFU

/* SJA1000 register address of the CDR register (31, physical 0x7C).
 * The AgRV IP DOES have a CDR (the SDK struct mislabels it
 * RESERVED2); it resets to 0xC0 = CAN mode (PeliCAN) + CBP set,
 * CLOCKOFF=0, CD=000 (CLKOUT = XTAL1/2). Per the AG32 CAN datasheet
 * CDR[7:4] are fixed (read-only 1100), so the shared SJA1000 init's
 * write of config->cdr | CAN_MODE (0x80) is a no-op against the fixed
 * bits and leaves CD/CLOCKOFF at their reset values. We still absorb
 * the write in write_reg() below: the IP is hardwired in PeliCAN
 * mode, and CLKOUT is not bonded out on the 407 bitstream.
 */
#define CAN_SJA1000_CDR_ADDR      31U

/* Per-instance configuration (the shared SJA1000 driver consumes a
 * const void *custom for this).
 */
struct can_agm_config {
	mem_addr_t base;
	/* CAN engine clock (devicetree `clocks` -> clk0), i.e. the f_can
	 * this SoC feeds into the IP. The shared driver is told
	 * core_clock_hz / CAN_AGM_SJA1000_CLK_DIVIDER. */
	uint32_t core_clock_hz;
	/* Pin route, from the devicetree pinctrl state (can0_default):
	 * AGM_PINCTRL() cells applied by pinctrl_configure_pins(). */
	const struct pinctrl_dev_config *pincfg;
	void (*irq_config_func)(const struct device *dev);
};

static uint8_t can_agm_read_reg(const struct device *dev, uint8_t reg)
{
	const struct can_sja1000_config *sja_cfg = dev->config;
	const struct can_agm_config *cfg = sja_cfg->custom;
	mem_addr_t addr = cfg->base + (reg * CAN_AGM_REG_STRIDE);

	return (uint8_t)(sys_read32(addr) & CAN_AGM_REG_MASK);
}

static void can_agm_write_reg(const struct device *dev, uint8_t reg, uint8_t val)
{
	const struct can_sja1000_config *sja_cfg = dev->config;
	const struct can_agm_config *cfg = sja_cfg->custom;
	mem_addr_t addr = cfg->base + (reg * CAN_AGM_REG_STRIDE);

	if (reg == CAN_SJA1000_CDR_ADDR) {
		/* Absorb the shared driver's CDR write: the IP is
		 * hardwired in PeliCAN mode (CDR resets to 0xC0) and the
		 * written 0x80 only touches fixed/RO bits (see above), so
		 * the hardware already is in the requested state.
		 */
		return;
	}
	sys_write32((uint32_t)val & CAN_AGM_REG_MASK, addr);
}

/* SJA1000 has an internal /2 divider on its core clock input, so the
 * shared driver computes bitrate as core_clock/(BRP+1)/(1+TSEG1+TSEG2)
 * and we report core_clock = f_can/2. f_can (the CAN engine's "XTAL1"
 * input) is SYSCLK on this SoC, and the driver takes it from the
 * devicetree `clocks` phandle of the can0 node rather than a compile-
 * time constant, so a board whose bitstream runs SYSCLK at a different
 * rate only needs to override &clk0 (the 103/test boards already do
 * that for UART0).
 *
 * On the dev board (three runs, ESP32-C3 peer):
 *   - 200 MHz bitstream, driver told 125 kbit/s -> works; an ESP32-C3
 *     listener clocked from its own 40 MHz crystal measured 2.59 us of
 *     shortest run for BTR0=0x12/BTR1=0x1a, versus 2 * 19 * 14 /
 *     200 MHz = 2.66 us predicted for f_can = 200 MHz.
 *   - 100 MHz bitstream (only the 26 PLL defparams differ; CLKOUT[0]
 *     -> sys_clk, CLKOUT[3] -> bus_clk, bus_clk stays 100 MHz), driver
 *     told 125 kbit/s -> every frame bus-offs (SR=0x60, TXERR=128,
 *     rc=-2); at 250 kbit/s (BTR0=0x18) the same bitstream transmits
 *     indefinitely (TXERR=0) and the 125 kbit/s ESP32 peer receives
 *     every frame.
 *   => f_can tracks SYSCLK, NOT bus_clk: with SYSCLK 100 MHz a 250
 *      kbit/s request is what 125 kbit/s needs, i.e. the engine sees
 *      half the rate. This also retires the SDK-style assumption
 *      CAN_CalculatePrescaler() = SYS_GetPclkFreq() as a coincidence of
 *      the 407's SYSCLK == BUSCLK ratio (both 200/100).
 *
 * An earlier revision of this file concluded f_can = SYSCLK/3 =
 * 66.67 MHz and reported a "virtual" 33.25 MHz core clock from it.
 * That was an artefact of samples/can_txedge: its sampling loop only
 * latched prev_t while edges == 0, so the "min_gap" it printed was the
 * distance from the first edge to the second one rather than a pulse
 * width. For a frame whose SOF and first two ID bits are dominant
 * (id 0x123) that gap spans three bit times, which is exactly the 3x
 * factor every row of the old table showed (23.9 / 11.8 / 8.1 / 8.0 us
 * versus 24.0 / 12.0 / 8.16 / 7.98 us predicted for 200 MHz).
 *
 * The slowest bit rate the engine can represent is
 * f_can / (divider 2 * BRP 64 * TQ_total 25); the per-instance
 * CAN_AGM_MIN_BITRATE below derives it from the same devicetree clock,
 * so can_calc_timing() rejects anything lower with -ENOTSUP and
 * can_get_bitrate_min() advertises the matching floor (62.5 kbit/s on
 * the 200 MHz boards, 31.25 kbit/s on the 100 MHz ones).
 */
#define CAN_AGM_SJA1000_CLK_DIVIDER  2U

/*
 * IRQ glue.
 *
 * The state machine lives in the shared can_sja1000.c, whose entry point
 * can_sja1000_isr() takes `const struct device *` -- the pre-v4.4 Zephyr ISR
 * signature. Every Zephyr IRQ_CONNECT() from 3.x on hands the handler a
 * `const void *`, and the native/POSIX build of this driver (tests/drivers/can/
 * can_agm) passes the function pointer through `posix_isr_declare(void
 * isr_p(const void *), ...)`, which is a hard type mismatch. Upstream
 * can_sja1000.c cannot be touched, so the adapter lives here: same pattern as
 * agm_uart_isr()/agm_wdt_isr() in this tree, one function per
 * IRQ_CONNECT.
 */
static void can_agm_isr(const void *irq_arg)
{
	const struct device *dev = irq_arg;

	can_sja1000_isr(dev);
}

/* Minimum bit rate the engine can actually produce with the core clock
 * reported above. This is what can_calc_timing() enforces, so it is
 * also what can_get_bitrate_min() must advertise -- an earlier revision
 * passed a flat 25000, which the API then could not honour.
 */
#define CAN_AGM_MIN_BITRATE(inst)                                                       \
	(DT_INST_PROP_BY_PHANDLE(inst, clocks, clock_frequency) / (2U * 64U * 25U))

static int can_agm_get_core_clock(const struct device *dev, uint32_t *rate)
{
	const struct can_sja1000_config *sja_cfg = dev->config;
	const struct can_agm_config *cfg = sja_cfg->custom;

	*rate = cfg->core_clock_hz / CAN_AGM_SJA1000_CLK_DIVIDER;
	return 0;
}

/* Clear MOD.AFM (acceptance filter mode), leaving every other mode bit alone.
 * Called from both set_mode() and init() -- see the AFM block below. */
static void can_agm_clear_afm(const struct device *dev)
{
	const struct can_sja1000_config *sja_cfg = dev->config;
	const struct can_agm_config *cfg = sja_cfg->custom;
	uint8_t mod = (uint8_t)(sys_read32(cfg->base) & CAN_AGM_REG_MASK);

	if ((mod & CAN_SJA1000_MOD_AFM) != 0U) {
		sys_write32((uint32_t)(mod & ~CAN_SJA1000_MOD_AFM), cfg->base);
	}
}

static int can_agm_init(const struct device *dev)
{
	const struct can_sja1000_config *sja_cfg = dev->config;
	const struct can_agm_config *cfg = sja_cfg->custom;
	int err;

	/* CAN0's APB gate (agm,apb-clkenable-bit = 26) was opened by soc.c at
	 * PRE_KERNEL_1 from devicetree -- like every other peripheral's, so
	 * this driver no longer touches SYS.APB_CLKENABLE at all. */

	/* Route CAN0 RX0/TX0 to their pins (AFSEL + DIR) from the pinctrl
	 * state; the banks' APB clocks are opened by soc.c at PRE_KERNEL_1
	 * from the same state (a gated bank drops these writes silently). */
	err = pinctrl_apply_state(cfg->pincfg, PINCTRL_STATE_DEFAULT);
	if (err != 0) {
		LOG_ERR("pinctrl_apply_state failed: %d", err);
		return err;
	}

	/* Delegate state machine + filter + IRQ plumbing to the shared
	 * SJA1000 driver (enters reset mode, sets up ACR/AMR as accept-
	 * all, computes timing, exits reset mode, enables IRQs).
	 */
	err = can_sja1000_init(dev);
	if (err != 0) {
		LOG_ERR("can_sja1000_init failed: %d", err);
		return err;
	}

	/* can_sja1000_init() programs NORMAL mode through the *shared*
	 * can_sja1000_set_mode(), so the AFM clear below does not run there and
	 * the controller would come up with MOD.AFM set (MOD = 0x09 = RM | AFM)
	 * until the first can_set_mode() call. Apply it here too, so "AFM stays
	 * out of the mode register" holds from init on. (The bit is harmless
	 * either way -- see can_agm_set_mode() -- but the driver has exactly one
	 * filtered layout, so there is no reason to leave it set.) */
	can_agm_clear_afm(dev);

	cfg->irq_config_func(dev);

	return 0;
}

/*
 * TX completion and the TR/SRR command are NOT overridden: the shared
 * can_sja1000_send() / can_sja1000_isr() pair is used as-is.
 *
 * HISTORY: this driver used to carry a verbatim copy of can_sja1000_send()
 * that (a) forced a flat CMR.TR instead of the shared driver's
 * `SRR in LOOPBACK / TR otherwise` switch, and (b) busy-polled SR.TBS and
 * fired the TX callback itself instead of letting IR.TI do it. Both were
 * justified by "the IP does not complete CMR.SRR / never fires IR.TI",
 * which was observed on the faulty dev board (wrong transceiver VCC, 3x-off
 * bit rate) and is now known to be false:
 *
 *   - NORMAL mode, 125 kbit/s, ESP32-C3 peer: can_send() completes
 *     through IR.TI -> can_sja1000_tx_done() with status 0.
 *   - MOD.STM (MOD=0x05 read back), samples/can_loopback:
 *     CMR.SRR self-reception works -- the sent frame comes back through
 *     the RX FIFO, the filter and the callback, and the sample PASSes.
 *     Forcing TR in that mode (the old behaviour) completes the TX but
 *     never self-receives, which is exactly SJA1000 semantics.
 *
 * The TBS busy-poll was also unsound on its own: it never waited for TBS
 * to drop first, its "10 ms" timeout was really 2000 cycles = 10 us, and
 * it duplicated can_sja1000_tx_done() outside any lock while the ISR ran
 * the same code (double callback). Removed.
 */

/*
 * AFM (acceptance filter mode) handling.
 *
 * HISTORY: this override exists because an earlier session concluded that
 * "the on-die controller does NOT enable the receiver / transmitter when
 * MOD.AFM = 1", based on reading SR after forcing MOD=0x08 through
 * openocd (MOD=0x00 -> SR=0xf4, "TXERR climbing"; MOD=0x08 -> SR=0x04,
 * "RS=0, TS=0, TX never starts").
 *
 * That conclusion is WRONG (verified on a healthy 125 kbit/s link to an
 * ESP32-C3): SR.RS and SR.TS are *transient* status bits that
 * are only set while a frame is actually being received / transmitted, so
 * sampling them at idle always yields 0 -- with or without AFM. Flipping
 * MOD to 0x08 on the live link left the peer's frame rate unchanged
 * (260 frames / 13 s at MOD=0x08 vs 260 / 13 s at MOD=0x00).
 *
 * The override is kept because it is harmless and keeps the AFM bit out
 * of the mode register: the filter is programmed accept-all either way
 * (ACR=0x00 / AMR=0xFF), so single vs dual filter layout makes no
 * difference here. It is NOT required to make TX/RX work, and should not
 * be cited as evidence of an IP bug.
 *
 * can_sja1000_init() programs its initial NORMAL mode through the shared
 * can_sja1000_set_mode(), i.e. *not* through this vfunc, so init() clears the
 * bit itself (can_agm_clear_afm) -- otherwise the controller would come up
 * with MOD = 0x09 (RM | AFM) and only lose AFM at the first can_set_mode().
 */
static int can_agm_set_mode(const struct device *dev, can_mode_t mode)
{
	int err = can_sja1000_set_mode(dev, mode);

	if (err != 0) {
		return err;
	}
	if ((mode & CAN_MODE_LISTENONLY) == 0) {
		/* NORMAL or LOOPBACK: clear MOD.AFM (Bit 3). */
		can_agm_clear_afm(dev);
	}
	return 0;
}

/*
 * TX entry point: can_sja1000_send() is used as-is, wrapped only to
 * plug one upstream defect without patching drivers/can/can_sja1000.c.
 *
 * Upstream takes tx_idle and then returns -EIO when SR.TBS still reads
 * "transmit buffer locked":
 *
 *     if (k_sem_take(&data->tx_idle, timeout) != 0)      -> -EAGAIN
 *     ...
 *     if ((sr & CAN_SJA1000_SR_TBS) == 0)                -> -EIO  (leak)
 *
 * The -EIO path never gives tx_idle back. tx_idle is a limit-1
 * semaphore and the only other producer is can_sja1000_tx_done(), i.e.
 * IR.TI of a frame that was actually queued -- so one such return drains
 * the permit for good: every later send() fails with -EAGAIN and the
 * controller looks dead until the next start(). The bug is latent
 * upstream (TBS is almost always set when the permit is free), which is
 * why it is fixed here instead of in the shared driver.
 *
 * Reclaim the permit only when this call is provably the one that ate
 * it: the count was non-zero on entry and is zero on an -EIO return.
 * k_sem_give() cannot over-release (limit 1), so the reclaim is safe
 * even if another thread is inside send() concurrently.
 */
static int can_agm_send(const struct device *dev, const struct can_frame *frame,
			k_timeout_t timeout, can_tx_callback_t callback, void *user_data)
{
	struct can_sja1000_data *data = dev->data;
	bool permit_was_free = (k_sem_count_get(&data->tx_idle) > 0U);
	int err;

	err = can_sja1000_send(dev, frame, timeout, callback, user_data);

	if (err == -EIO && permit_was_free && k_sem_count_get(&data->tx_idle) == 0U) {
		k_sem_give(&data->tx_idle);
	}

	return err;
}

static DEVICE_API(can, can_agm_driver_api) = {
	.get_capabilities = can_sja1000_get_capabilities,
	.start            = can_sja1000_start,
	.stop             = can_sja1000_stop,
	.set_mode         = can_agm_set_mode,
	.set_timing       = can_sja1000_set_timing,
	.send             = can_agm_send,
	.add_rx_filter    = can_sja1000_add_rx_filter,
	.remove_rx_filter = can_sja1000_remove_rx_filter,
	.get_state        = can_sja1000_get_state,
	/*
	 * state-change callbacks are no longer a per-driver vfunc: upstream
	 * v4.4 moved them to a public slist managed by
	 * can_add_state_change_callback() / can_remove_state_change_callback(),
	 * and can_sja1000_init() already initialises data->common.state_change_callbacks
	 * and fires them via can_fire_state_change_callbacks() in the ISR.
	 * Leave the optional state_change_callbacks_enabled vfunc as NULL --
	 * the AgRV controller's PLIC IRQ is already enabled for the rest of
	 * the sja1000 state machine, so no per-callback IRQ gating is needed.
	 */
	.state_change_callbacks_enabled = NULL,
	.get_core_clock   = can_agm_get_core_clock,
	.get_max_filters  = can_sja1000_get_max_filters,
#ifdef CONFIG_CAN_MANUAL_RECOVERY_MODE
	.recover          = can_sja1000_recover,
#endif
	.timing_min       = CAN_SJA1000_TIMING_MIN_INITIALIZER,
	.timing_max       = CAN_SJA1000_TIMING_MAX_INITIALIZER,
};

#define CAN_AGM_INIT(inst)                                                                      \
	PINCTRL_DT_INST_DEFINE(inst);                                                           \
                                                                                               \
	static void can_agm_irq_config_##inst(const struct device *dev)                         \
	{                                                                                       \
		IRQ_CONNECT(DT_INST_IRQN(inst), DT_INST_IRQ(inst, priority), can_agm_isr,       \
			    DEVICE_DT_INST_GET(inst), 0);                                       \
		irq_enable(DT_INST_IRQN(inst));                                                 \
	}                                                                                       \
                                                                                               \
	static const struct can_agm_config can_agm_config_##inst = {                            \
		.base           = DT_INST_REG_ADDR(inst),                                        \
		.core_clock_hz  = DT_INST_PROP_BY_PHANDLE(inst, clocks, clock_frequency),        \
		.pincfg         = PINCTRL_DT_INST_DEV_CONFIG_GET(inst),                          \
		.irq_config_func = can_agm_irq_config_##inst,                                   \
	};                                                                                      \
                                                                                               \
	static const struct can_sja1000_config can_sja1000_config_##inst =                      \
		CAN_SJA1000_DT_CONFIG_INST_GET(inst, &can_agm_config_##inst, can_agm_read_reg,  \
					       can_agm_write_reg,                               \
					       CAN_SJA1000_OCR_OCMODE_NORMAL,                  \
					       0U, /* CDR write absorbed (IP resets 0xC0) */    \
					       CAN_AGM_MIN_BITRATE(inst));                     \
                                                                                               \
	static struct can_sja1000_data can_sja1000_data_##inst =                                \
		CAN_SJA1000_DATA_INITIALIZER(NULL);                                             \
                                                                                               \
	DEVICE_DT_INST_DEFINE(inst, can_agm_init, NULL, &can_sja1000_data_##inst,               \
			      &can_sja1000_config_##inst, POST_KERNEL,                         \
			      CONFIG_CAN_INIT_PRIORITY, &can_agm_driver_api);

DT_INST_FOREACH_STATUS_OKAY(CAN_AGM_INIT)
