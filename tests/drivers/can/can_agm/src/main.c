/*
 * Copyright (c) 2026 AgRV Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief native_sim suite for drivers/can/can_agm.c.
 *
 * The driver is a thin AgRV frontend onto the shared can_sja1000.c state
 * machine: read_reg()/write_reg() map the SJA1000's 8-bit register file onto a
 * 32-bit word stride, and three local overrides plus one ISR adapter carry the
 * AgRV-specific behaviour. Two things this suite has to supply, neither of
 * which exists on native_sim:
 *
 *  1. **The register window.** tests/drivers/common/agm_native/
 *     agm_fake_mmio.c reserves 0x40000000+32 MiB with mmap(MAP_FIXED) before
 *     any device init, so the *unmodified* driver talks to RAM at the real
 *     CAN0 address (0x4002a000, kept in the overlay) and the test can read back
 *     what it programmed.
 *
 *  2. **Something for the SJA1000 bits to mean.** RAM answers reads with the
 *     last value written, so the test plays the hardware: it pre-loads the
 *     reset values the driver must reprogram (CDR, acceptance window), writes
 *     SR/IR to steer the state machine, decodes the frame buffer window the
 *     shared driver fills on TX, and injects a frame on RX.
 *
 * That buys coverage this driver never had, on one side or the other of the
 * register file:
 *  - the register *addressing* (byte register -> 32-bit word) and the CDR
 *    write absorb, which are the two places the AgRV IP differs from a real
 *    SJA1000 clone;
 *  - the accept-all filter programming (ACR/AMR through the reset-mode
 *    window), the reset-mode handshake, the interrupt enables;
 *  - the pinctrl route (GPIO8 bit 7 TX0 / GPIO7 bit 3 RX0, AFSEL + DIR) that
 *    only the board could show before;
 *  - can_agm_set_mode()'s AFM clear (upstream would leave MOD.AFM set);
 *  - the TX frame encoding into the buffer window and CMR selection
 *    (SRR in loopback, TR otherwise, AT in one-shot);
 *  - can_agm_send()'s tx_idle reclaim on the -EIO path -- upstream
 *    can_sja1000_send() leaks the permit there and the controller goes
 *    silently deaf to every later send() until the next start();
 *  - the ISR adapter. This is the only AGM driver whose interrupt path runs on
 *    native_sim: the suite pends IRQ 29 with posix_sw_set_pending_IRQ() and the
 *    real can_agm_isr() -> can_sja1000_isr() chain executes, driving the TX
 *    completion callback, the RX callback, the RX filter matching and the
 *    state-change callbacks.
 *
 * Two model decisions worth naming, because they are model limits rather than
 * IP behaviour:
 *  - **SR.RBS stays clear.** The shared RX handler loops `while (SR & RBS)`,
 *    and with a plain RAM window nothing can clear that bit *inside* the loop
 *    (the driver's CMR.RRB write and the SR read land on different addresses).
 *    The model therefore queues one frame per IR.RI and leaves RBS clear, so
 *    the loop body runs exactly once per interrupt -- which is what is under
 *    test here (decode, filter, callback, release). A real controller sets RBS
 *    for a second queued frame; that path would need an intercepting model.
 *  - **TX completion is played, not simulated.** After send() the test sets
 *    SR.TBS|TCS and IR.TI and pends the IRQ, which is exactly what the IP does
 *    when the frame wins arbitration (on hardware it
 *    completes through IR.TI -> can_sja1000_tx_done()).
 *
 * NOTE: test_01 must stay first -- it reads the acceptance code/mask out of
 * registers 16..23, the same window the frame buffer uses afterwards.
 */

#include <zephyr/arch/posix/posix_soc_if.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/can.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>
#include <zephyr/ztest.h>

#include <string.h>

#define CAN_DEV  DEVICE_DT_GET(DT_NODELABEL(can_agm0))

#define CAN_BASE   0x4002a000UL
#define CAN_STRIDE 4U

/* Interrupt line from the overlay (the SoC's PLIC CAN0 IRQ, which is also
 * inside native_sim's 0..31 range). */
#define CAN_IRQ 29U

/* SJA1000 register addresses (drivers/can/can_sja1000_priv.h). Duplicated on
 * purpose: if the shared driver's map moves, this suite should fail loudly. */
#define R_MOD        0U
#define R_CMR        1U
#define R_SR         2U
#define R_IR         3U
#define R_IER        4U
#define R_BTR0       6U
#define R_BTR1       7U
#define R_OCR        8U
#define R_EWLR       13U
#define R_RXERR      14U
#define R_TXERR      15U
#define R_FRAME_INFO 16U
#define R_XFF_ID1    17U
#define R_XFF_ID2    18U
#define R_ACR0       16U
#define R_ACR3       19U
#define R_AMR0       20U
#define R_AMR3       23U
#define R_SFF_DATA   19U
#define R_EFF_ID3    19U
#define R_EFF_ID4    20U
#define R_EFF_DATA   21U
#define R_CDR        31U

/* MOD bits. */
#define MOD_RM  BIT(0)
#define MOD_LOM BIT(1)
#define MOD_STM BIT(2)
#define MOD_AFM BIT(3)

/* CMR bits. */
#define CMR_TR  BIT(0)
#define CMR_AT  BIT(1)
#define CMR_RRB BIT(2)
#define CMR_SRR BIT(4)

/* SR bits. */
#define SR_RBS BIT(0)
#define SR_TBS BIT(2)
#define SR_TCS BIT(3)
#define SR_ES  BIT(6)
#define SR_BS  BIT(7)

/* IR bits. */
#define IR_RI  BIT(0)
#define IR_TI  BIT(1)
#define IR_EI  BIT(2)
#define IR_EPI BIT(5)

/* IER bits the shared driver enables unconditionally. */
#define IER_RIE  BIT(0)
#define IER_TIE  BIT(1)
#define IER_EIE  BIT(2)
#define IER_EPIE BIT(5)

/* Frame info bits. */
#define FRAME_INFO_RTR BIT(6)
#define FRAME_INFO_FF  BIT(7)

/* AgRV reset value of CDR: PeliCAN (CAN_MODE) + CBP, CD = /2, CLOCKOFF = 0.
 * The shared driver ORs in 0x80, which only touches fixed read-only bits (see
 * drivers/can/can_agm.c) -- so a device that really is in the requested state
 * must still read 0xc0 back. */
#define AGM_CDR_RESET 0xc0U

static uint8_t reg_rd(uint8_t reg)
{
	return (uint8_t)(*(volatile uint32_t *)(CAN_BASE + (reg * CAN_STRIDE)) & 0xffU);
}

static void reg_wr(uint8_t reg, uint8_t val)
{
	*(volatile uint32_t *)(CAN_BASE + (reg * CAN_STRIDE)) = (uint32_t)val;
}

/* GPIO bank registers the SoC pinctrl writes (soc/agm/agrv2k/agm_sys.h). */
#define GPIO0_BASE    0x40014000UL
#define GPIO_STRIDE   0x1000UL
#define GPIO_DIR_OFF  0x400UL
#define GPIO_AFSEL_OFF 0x420UL

static uint32_t gpio_rd(uint32_t bank, uint32_t off)
{
	return *(volatile uint32_t *)(GPIO0_BASE + (bank * GPIO_STRIDE) + off);
}

/*
 * Pre-set the register file the way the IP powers up, before any driver init.
 * EARLY priority 1 runs after the shared window's own EARLY priority 0
 * (agm_fake_mmio.c) and before the CAN driver's POST_KERNEL init.
 *
 * Only the parts the driver is expected to reprogram are touched, and each is
 * set to a *different* value than the driver will write, so the assertions in
 * test_01 mean something: an anonymous RAM page would otherwise already read
 * as "ACR = 0".
 */
static int can_suite_preset_hw_state(void)
{
	reg_wr(R_CDR, AGM_CDR_RESET);

	for (uint8_t reg = R_ACR0; reg <= R_AMR3; reg++) {
		reg_wr(reg, 0xa5U);
	}

	return 0;
}

SYS_INIT(can_suite_preset_hw_state, EARLY, 1);

/* ---- playing the controller ------------------------------------------ */

static int tx_cb_count;
static int tx_cb_status;

static void tx_callback(const struct device *dev, int error, void *user_data)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(user_data);

	tx_cb_status = error;
	tx_cb_count++;
}

static int rx_cb_count;
static struct can_frame rx_frame;
static void *rx_cb_user_data;

static void rx_callback(const struct device *dev, struct can_frame *frame, void *user_data)
{
	ARG_UNUSED(dev);

	rx_frame = *frame;
	rx_cb_user_data = user_data;
	rx_cb_count++;
}

/* Pend the controller's interrupt line and let the CPU take it. The native
 * interrupt controller delivers a pending line as soon as the simulated CPU
 * yields, which is why this is a sleep rather than a direct ISR call: the
 * handler really is reached through the driver's IRQ_CONNECT(). */
static void trigger_can_irq(void)
{
	posix_sw_set_pending_IRQ(CAN_IRQ);
	k_sleep(K_MSEC(5));
}

/* The IP finished sending the frame the driver queued: the buffer is released
 * again (TBS) and the last transfer completed (TCS) -> IR.TI. */
static void model_complete_tx(void)
{
	reg_wr(R_SR, SR_TBS | SR_TCS);
	reg_wr(R_IR, IR_TI);
	trigger_can_irq();
}

static void model_inject_rx_std(uint32_t id, uint8_t dlc, const uint8_t *data)
{
	reg_wr(R_FRAME_INFO, dlc);
	reg_wr(R_XFF_ID1, (uint8_t)(id >> 3));
	reg_wr(R_XFF_ID2, (uint8_t)((id & 0x07U) << 5));

	if (data != NULL) {
		for (uint8_t i = 0U; i < dlc; i++) {
			reg_wr(R_SFF_DATA + i, data[i]);
		}
	}

	/* One frame queued for delivery; RBS stays clear -- see the file header. */
	reg_wr(R_SR, SR_TBS);
	reg_wr(R_IR, IR_RI);
	trigger_can_irq();
}

static void model_inject_rx_ext(uint32_t id, uint8_t dlc, bool rtr, const uint8_t *data)
{
	reg_wr(R_FRAME_INFO, (uint8_t)(dlc | FRAME_INFO_FF | (rtr ? FRAME_INFO_RTR : 0U)));
	reg_wr(R_XFF_ID1, (uint8_t)(id >> 21));
	reg_wr(R_XFF_ID2, (uint8_t)(id >> 13));
	reg_wr(R_EFF_ID3, (uint8_t)(id >> 5));
	reg_wr(R_EFF_ID4, (uint8_t)((id & 0x1fU) << 3));

	if (data != NULL) {
		for (uint8_t i = 0U; i < dlc; i++) {
			reg_wr(R_EFF_DATA + i, data[i]);
		}
	}

	reg_wr(R_SR, SR_TBS);
	reg_wr(R_IR, IR_RI);
	trigger_can_irq();
}

/* ---- suite hooks ------------------------------------------------------ */

static void suite_reset_fake_regs(void *fixture)
{
	ARG_UNUSED(fixture);

	/* The test owns the volatile halves of the register file. Everything the
	 * driver programs at init (MOD, ACR/AMR, IER, BTR0/1, OCR, EWLR, CDR) is
	 * left alone: test_01 asserts it. */
	reg_wr(R_SR, 0U);
	reg_wr(R_IR, 0U);
	reg_wr(R_CMR, 0U);
	reg_wr(R_RXERR, 0U);
	reg_wr(R_TXERR, 0U);

	/* Filter slots live in the driver's data, not in the register window, so
	 * disarm every slot: each case starts with no filter armed. */
	for (int slot = 0; slot < CONFIG_CAN_SJA1000_MAX_FILTERS; slot++) {
		can_remove_rx_filter(CAN_DEV, slot);
	}

	tx_cb_count = 0;
	tx_cb_status = 0;
	rx_cb_count = 0;
	rx_cb_user_data = NULL;
	memset(&rx_frame, 0, sizeof(rx_frame));
}

static void suite_after_each(void *fixture)
{
	ARG_UNUSED(fixture);

	/* Every case leaves the controller stopped and in NORMAL mode (-EALREADY
	 * when it already is). Stopping also aborts a queued transmission, so no
	 * case can leak a pending tx callback into the next one. */
	(void)can_stop(CAN_DEV);
	(void)can_set_mode(CAN_DEV, CAN_MODE_NORMAL);

	/* The cached error state is sticky: can_start() does not reset it, so a
	 * case that left the controller BUS_OFF would make the next case's
	 * send() fail with -ENETUNREACH. Play the IP's "everything is fine"
	 * interrupt (SR clear + IR.EI), which is how the state machine returns to
	 * ERROR_ACTIVE. */
	reg_wr(R_SR, 0U);
	reg_wr(R_IR, IR_EI);
	trigger_can_irq();
}

ZTEST_SUITE(can_agm, NULL, NULL, suite_reset_fake_regs, suite_after_each, NULL);

/* ---- init, register addressing, pinctrl ------------------------------- */

ZTEST(can_agm, test_01_init_programs_the_peli_can_register_file)
{
	uint32_t core_clock = 0U;

	zassert_true(device_is_ready(CAN_DEV), "the CAN device came up against the fake window");

	/* CDR: the shared driver writes config->cdr | CAN_MODE (0x80). The AgRV IP
	 * resets to 0xc0 and bits 7:4 are fixed, so write_reg() absorbs the write
	 * (drivers/can/can_agm.c, CAN_SJA1000_CDR_ADDR). A plain RAM window that
	 * read 0x80 here would mean the absorb was lost. */
	zassert_equal(reg_rd(R_CDR), AGM_CDR_RESET,
		      "the CDR write is absorbed, the IP stays in the requested PeliCAN mode "
		      "(got 0x%02x)",
		      reg_rd(R_CDR));

	/* Reset-mode window: accept everything, filter in software. */
	for (uint8_t i = 0U; i < 4U; i++) {
		zassert_equal(reg_rd(R_ACR0 + i), 0x00U, "ACR%u is cleared (was 0xa5 before init)",
			      i);
		zassert_equal(reg_rd(R_AMR0 + i), 0xffU, "AMR%u is all-mask (was 0xa5 before init)",
			      i);
	}

	zassert_equal(reg_rd(R_IER) & (IER_RIE | IER_TIE | IER_EIE | IER_EPIE),
		      IER_RIE | IER_TIE | IER_EIE | IER_EPIE,
		      "RX, TX, error-warning and error-passive interrupts are enabled (IER=0x%02x)",
		      reg_rd(R_IER));
	zassert_equal(reg_rd(R_OCR), 0x02U, "OCR selects the normal output mode");
	zassert_equal(reg_rd(R_EWLR), 96U, "error warning limit from the shared init");

	/* The controller is left in reset mode (RM) -- only can_start() brings it
	 * out -- and carries no other mode bit: upstream can_sja1000_set_mode()
	 * sets MOD.AFM and the frontend clears it again, at init as well as on
	 * every later can_set_mode() (without the init-side clear this reads 0x09
	 * = RM | AFM). */
	zassert_equal(reg_rd(R_MOD), MOD_RM,
		      "init leaves the controller in reset mode, AFM-free (MOD=0x%02x)",
		      reg_rd(R_MOD));

	/* The AgRV-specific clock contract: SYSCLK / 2 is what the shared driver is
	 * told, because the IP halves its input internally. */
	zassert_ok(can_get_core_clock(CAN_DEV, &core_clock), "get_core_clock works");
	zassert_equal(core_clock, 100000000U, "f_can/2 for a 200 MHz bitstream");

	/* The advertised floor is the slowest frame the timing registers can
	 * represent: f_can / (2 * 64 * 25). */
	zassert_equal(can_get_bitrate_min(CAN_DEV), 62500U, "derived minimum bitrate");
	zassert_equal(can_get_bitrate_max(CAN_DEV), 1000000U, "maximum bitrate from DT config");

	/* The pin route, decoded from the real AGM_PINCTRL() cells by the real
	 * pinctrl_configure_pins(): TX0 = GPIO8 bit 7 OUTPUT, RX0 = GPIO7 bit 3
	 * INPUT. Without AFSEL the pins stay in software mode and the IP never
	 * reaches the bus. */
	zassert_equal(gpio_rd(8U, GPIO_AFSEL_OFF) & BIT(7), BIT(7),
		      "TX0 pin handed to the CAN IP (AFSEL)");
	zassert_equal(gpio_rd(8U, GPIO_DIR_OFF) & BIT(7), BIT(7), "TX0 pin set to drive (DIR)");
	zassert_equal(gpio_rd(7U, GPIO_AFSEL_OFF) & BIT(3), BIT(3),
		      "RX0 pin handed to the CAN IP (AFSEL)");
	zassert_equal(gpio_rd(7U, GPIO_DIR_OFF) & BIT(3), 0U, "RX0 pin left as an input");
}

ZTEST(can_agm, test_02_bit_timing_matches_the_dt_bitrate)
{
	struct can_timing timing;
	uint8_t btr0 = reg_rd(R_BTR0);
	uint8_t btr1 = reg_rd(R_BTR1);
	uint32_t brp = (uint32_t)(btr0 & 0x3fU) + 1U;         /* prescaler */
	uint32_t tseg1 = (uint32_t)(btr1 & 0x0fU) + 1U;       /* phase_seg1 */
	uint32_t tseg2 = (uint32_t)((btr1 >> 4) & 0x07U) + 1U; /* phase_seg2 */
	uint32_t total_tq = 1U + tseg1 + tseg2;

	/* Derived back through the SJA1000 formula with the bitstream's 200 MHz
	 * XTAL1 (the IP divides by 2 itself), which is what the peer on the bus
	 * sees -- independent of the driver's own arithmetic. */
	zassert_equal(200000000U / (2U * brp * total_tq), 125000U,
		      "BTR0=0x%02x BTR1=0x%02x is not 125 kbit/s", btr0, btr1);
	zassert_equal((1U + tseg1) * 1000U / total_tq, 875U,
		      "sample point should be the 87.5%% default for 125 kbit/s");
	zassert_equal(btr1 & BIT(7), 0U, "single sampling is off (BTR1.SAM)");

	/* And the same bytes re-derived from the public timing API the driver's
	 * init calls, packed by hand here. */
	zassert_ok(can_calc_timing(CAN_DEV, &timing, 125000U, 0U), "125 kbit/s is representable");
	zassert_equal(btr0, (uint8_t)(((timing.sjw - 1U) << 6) | (timing.prescaler - 1U)),
		      "BTR0 = SJW-1 | BRP-1");
	zassert_equal(btr1,
		      (uint8_t)((timing.phase_seg1 - 1U) | ((timing.phase_seg2 - 1U) << 4)),
		      "BTR1 = TSEG1-1 | (TSEG2-1) << 4");

	/* can_get_bitrate_min() advertises a floor the timing registers can
	 * actually reach: one step below it, can_calc_timing() runs out of
	 * prescaler and the API reports -ENOTSUP. */
	zassert_equal(can_set_bitrate(CAN_DEV, 60000U), -ENOTSUP,
		      "below the advertised floor");
}

ZTEST(can_agm, test_03_set_timing_encodes_brp_and_segments)
{
	const struct can_timing timing = {
		.sjw = 2U,
		.prop_seg = 0U,
		.phase_seg1 = 5U,
		.phase_seg2 = 3U,
		.prescaler = 4U,
	};
	struct can_timing bad = timing;

	zassert_ok(can_start(CAN_DEV), "the controller starts");
	zassert_equal(can_set_timing(CAN_DEV, &timing), -EBUSY, "timing is fixed while started");
	zassert_ok(can_stop(CAN_DEV));

	zassert_ok(can_set_timing(CAN_DEV, &timing), "timing accepted while stopped");
	zassert_equal(reg_rd(R_BTR0), 0x43U, "SJW=2 -> bits 7:6, prescaler 4 -> BRP-1 = 3");
	zassert_equal(reg_rd(R_BTR1), 0x24U, "TSEG1=5 -> 4, TSEG2=3 -> 2 << 4");

	bad.prescaler = 65U;
	zassert_equal(can_set_timing(CAN_DEV, &bad), -ENOTSUP, "the API range-checks the prescaler");

	/* Leave the 125 kbit/s the rest of the suite (and the board dts) uses. */
	zassert_ok(can_set_bitrate(CAN_DEV, 125000U), "back to the DT bitrate");
}

/* ---- modes ------------------------------------------------------------ */

ZTEST(can_agm, test_04_mode_bits_and_the_afm_override)
{
	zassert_ok(can_set_mode(CAN_DEV, CAN_MODE_LOOPBACK), "loopback is supported");
	/* Stopped, so the controller sits in reset mode: compare everything but
	 * RM. */
	zassert_equal(reg_rd(R_MOD) & ~MOD_RM, MOD_STM,
		      "LOOPBACK sets MOD.STM and the AgRV override clears MOD.AFM (MOD=0x%02x)",
		      reg_rd(R_MOD));
	zassert_equal(can_get_mode(CAN_DEV), CAN_MODE_LOOPBACK, "the mode is reported back");

	zassert_ok(can_set_mode(CAN_DEV, CAN_MODE_LISTENONLY), "listen-only is supported");
	zassert_equal(reg_rd(R_MOD) & ~(MOD_RM | MOD_AFM), MOD_LOM,
		      "LISTENONLY sets LOM, STM is cleared");
	/* ... and leaves AFM set: can_agm_set_mode() only clears the bit for the
	 * TX-capable modes. Harmless either way (ACR = 0x00 / AMR = 0xff accepts
	 * every frame in both filter layouts), but the
	 * asymmetry is real, so pin it rather than assume. */
	zassert_equal(reg_rd(R_MOD) & MOD_AFM, MOD_AFM,
		      "LISTENONLY keeps the shared driver's AFM bit");

	zassert_ok(can_set_mode(CAN_DEV, CAN_MODE_NORMAL), "back to normal");
	zassert_equal(reg_rd(R_MOD) & ~MOD_RM, 0U,
		      "NORMAL leaves no mode bit set -- upstream would leave AFM here");

	zassert_equal(can_set_mode(CAN_DEV, CAN_MODE_FD), -ENOTSUP,
		      "the IP is classic CAN 2.0 only");

	zassert_ok(can_start(CAN_DEV), "start");
	zassert_equal(can_set_mode(CAN_DEV, CAN_MODE_LOOPBACK), -EBUSY,
		      "the mode cannot change while started");
	zassert_ok(can_stop(CAN_DEV));
}

/* ---- transmit --------------------------------------------------------- */

ZTEST(can_agm, test_05_send_encodes_the_frame_and_selects_srr_in_loopback)
{
	const uint8_t data[8] = { 0x01U, 0x23U, 0x45U, 0x67U, 0x89U, 0xabU, 0xcdU, 0xefU };
	struct can_frame frame = { 0 };
	int err;

	zassert_ok(can_set_mode(CAN_DEV, CAN_MODE_LOOPBACK));
	zassert_ok(can_start(CAN_DEV));

	frame.id = 0x123U;
	frame.dlc = 8U;
	memcpy(frame.data, data, sizeof(data));

	reg_wr(R_SR, SR_TBS); /* transmit buffer released */
	err = can_send(CAN_DEV, &frame, K_MSEC(100), tx_callback, NULL);
	zassert_ok(err, "send() accepted the frame (err %d)", err);

	/* The buffer window now holds the frame the IP would put on the wire. */
	zassert_equal(reg_rd(R_FRAME_INFO), 8U, "FRAME_INFO: DLC 8, standard, data frame");
	zassert_equal(reg_rd(R_XFF_ID1), 0x24U, "ID10..3 of 0x123");
	zassert_equal(reg_rd(R_XFF_ID2), 0x60U, "ID2..0 in the top bits");
	for (uint8_t i = 0U; i < 8U; i++) {
		zassert_equal(reg_rd(R_SFF_DATA + i), data[i], "payload byte %u", i);
	}

	/* Self-reception inside the IP is what MOD.STM gives; the SJA1000 command
	 * for it is CMR.SRR, not CMR.TR. */
	zassert_equal(reg_rd(R_CMR), CMR_SRR, "loopback sends with SRR (CMR=0x%02x)",
		      reg_rd(R_CMR));

	/* IR.TI -> can_sja1000_tx_done() and the permit comes back. */
	model_complete_tx();
	zassert_equal(tx_cb_count, 1, "the TX completion callback ran");
	zassert_equal(tx_cb_status, 0, "status 0: SR.TCS was set on completion");

	/* ... which is exactly what a second send() needs. */
	zassert_ok(can_send(CAN_DEV, &frame, K_MSEC(100), tx_callback, NULL),
		   "the tx_idle permit was returned by the completion IRQ");
	zassert_equal(tx_cb_count, 1, "the second frame is still in flight");
	model_complete_tx();
	zassert_equal(tx_cb_count, 2, "second completion");
}

ZTEST(can_agm, test_06_send_uses_tr_and_one_shot_in_normal_mode)
{
	struct can_frame frame = { 0 };

	zassert_ok(can_set_mode(CAN_DEV, CAN_MODE_NORMAL | CAN_MODE_ONE_SHOT));
	zassert_ok(can_start(CAN_DEV));

	frame.id = 0x0aaU;
	frame.dlc = 1U;
	frame.data[0] = 0x5aU;

	reg_wr(R_SR, SR_TBS);
	zassert_ok(can_send(CAN_DEV, &frame, K_MSEC(100), tx_callback, NULL), "send() accepted");
	zassert_equal(reg_rd(R_CMR), CMR_TR | CMR_AT,
		      "normal mode transmits with TR, one-shot adds AT (CMR=0x%02x)", reg_rd(R_CMR));
	zassert_equal(reg_rd(R_FRAME_INFO), 1U, "DLC 1");
	zassert_equal(reg_rd(R_SFF_DATA), 0x5aU, "payload");

	model_complete_tx();
	zassert_equal(tx_cb_count, 1, "completion");
}

ZTEST(can_agm, test_07_locked_tx_buffer_reclaims_the_tx_idle_permit)
{
	struct can_frame frame = { 0 };
	int err;

	zassert_ok(can_set_mode(CAN_DEV, CAN_MODE_NORMAL));
	zassert_ok(can_start(CAN_DEV));

	frame.id = 0x100U;
	frame.dlc = 0U;

	/* SR.TBS clear: the IP still owns the transmit buffer. Upstream
	 * can_sja1000_send() takes tx_idle and then returns -EIO without giving it
	 * back, which drains the limit-1 semaphore for good -- every later send()
	 * would fail with -EAGAIN and the controller would look dead until the
	 * next start(). can_agm_send() reclaims it. */
	reg_wr(R_SR, 0U);
	err = can_send(CAN_DEV, &frame, K_MSEC(100), tx_callback, NULL);
	zassert_equal(err, -EIO, "a locked transmit buffer is -EIO (err %d)", err);
	zassert_equal(tx_cb_count, 0, "nothing was queued");

	/* The permit is back: this is the regression. */
	reg_wr(R_SR, SR_TBS);
	err = can_send(CAN_DEV, &frame, K_MSEC(100), tx_callback, NULL);
	zassert_ok(err, "the next send() is not starved by the failed one (err %d)", err);
	model_complete_tx();
	zassert_equal(tx_cb_count, 1, "and it completes normally");
}

/* ---- receive ---------------------------------------------------------- */

ZTEST(can_agm, test_08_received_frame_is_decoded_from_the_buffer_window)
{
	const uint8_t data[3] = { 0xdeU, 0xadU, 0xbeU };
	int filter_id;

	zassert_ok(can_set_mode(CAN_DEV, CAN_MODE_NORMAL));
	zassert_ok(can_start(CAN_DEV));

	filter_id = can_add_rx_filter(CAN_DEV, rx_callback, (void *)0x1234U,
				      &(struct can_filter){ .id = 0x123U, .mask = CAN_STD_ID_MASK,
							    .flags = 0U });
	zassert_true(filter_id >= 0, "filter added (id %d)", filter_id);

	reg_wr(R_CMR, 0U);
	model_inject_rx_std(0x123U, 3U, data);

	zassert_equal(rx_cb_count, 1, "the RX callback ran (IR.RI)");
	zassert_equal(rx_frame.id, 0x123U, "standard ID");
	zassert_equal(rx_frame.dlc, 3U, "DLC");
	zassert_equal(rx_frame.flags, 0U, "plain data frame");
	zassert_mem_equal(rx_frame.data, data, 3U, "payload");
	zassert_equal(rx_cb_user_data, (void *)0x1234U, "the filter's user_data comes back");
	zassert_equal(reg_rd(R_CMR), CMR_RRB,
		      "the shared handler released the receive buffer (CMR.RRB)");

	can_remove_rx_filter(CAN_DEV, filter_id);
}

ZTEST(can_agm, test_09_extended_frames_round_trip_through_the_buffer_window)
{
	const uint32_t ext_id = 0x1abcdefU;
	const uint8_t data[2] = { 0x11U, 0x22U };
	struct can_frame frame = { 0 };
	int filter_id;

	zassert_ok(can_set_mode(CAN_DEV, CAN_MODE_NORMAL));
	zassert_ok(can_start(CAN_DEV));

	/* TX: 29-bit ID in the two-stage layout of the extended frame window. */
	frame.id = ext_id;
	frame.flags = CAN_FRAME_IDE;
	frame.dlc = 2U;
	memcpy(frame.data, data, sizeof(data));

	reg_wr(R_SR, SR_TBS);
	zassert_ok(can_send(CAN_DEV, &frame, K_MSEC(100), tx_callback, NULL), "send() accepted");
	zassert_equal(reg_rd(R_FRAME_INFO), 2U | FRAME_INFO_FF, "DLC 2, extended frame");
	zassert_equal(reg_rd(R_XFF_ID1), (uint8_t)(ext_id >> 21), "ID28..21");
	zassert_equal(reg_rd(R_XFF_ID2), (uint8_t)(ext_id >> 13), "ID20..13");
	zassert_equal(reg_rd(R_EFF_ID3), (uint8_t)(ext_id >> 5), "ID12..5");
	zassert_equal(reg_rd(R_EFF_ID4), (uint8_t)((ext_id & 0x1fU) << 3), "ID4..0");
	zassert_equal(reg_rd(R_EFF_DATA), data[0], "payload in the extended data window");
	model_complete_tx();

	/* RX: same window, filtered on the extended ID, RTR flag included. */
	filter_id = can_add_rx_filter(CAN_DEV, rx_callback, NULL,
				      &(struct can_filter){ .id = ext_id & 0x1ffffc00U,
							    .mask = 0x1ffffc00U,
							    .flags = CAN_FILTER_IDE });
	zassert_true(filter_id >= 0, "extended filter added (id %d)", filter_id);

	model_inject_rx_ext(ext_id, 2U, false, data);
	zassert_equal(rx_cb_count, 1, "the extended frame arrived");
	zassert_equal(rx_frame.id, ext_id, "29-bit ID reassembled");
	zassert_equal(rx_frame.flags & CAN_FRAME_IDE, CAN_FRAME_IDE, "IDE flag");
	zassert_equal(rx_frame.dlc, 2U, "DLC");
	zassert_mem_equal(rx_frame.data, data, 2U, "payload");

	/* Remote frames are decoded out of the same window but not delivered: the
	 * shared handler drops them unless CONFIG_CAN_ACCEPT_RTR is enabled (it is
	 * not, here or on the boards). */
	model_inject_rx_ext(ext_id, 0U, true, NULL);
	zassert_equal(rx_cb_count, 1, "an RTR frame is filtered out without CONFIG_CAN_ACCEPT_RTR");

	can_remove_rx_filter(CAN_DEV, filter_id);
}

ZTEST(can_agm, test_10_rx_filters_allocate_release_and_match)
{
	const struct can_filter filter_a = { .id = 0x100U, .mask = CAN_STD_ID_MASK, .flags = 0U };
	const struct can_filter filter_b = { .id = 0x200U, .mask = CAN_STD_ID_MASK, .flags = 0U };
	int id_a;
	int id_b;
	uint8_t payload = 0x77U;

	zassert_ok(can_set_mode(CAN_DEV, CAN_MODE_NORMAL));
	zassert_ok(can_start(CAN_DEV));

	zassert_equal(can_get_max_filters(CAN_DEV, false), CONFIG_CAN_SJA1000_MAX_FILTERS,
		      "software filtering depth comes from Kconfig");

	id_a = can_add_rx_filter(CAN_DEV, rx_callback, (void *)0xaU, &filter_a);
	id_b = can_add_rx_filter(CAN_DEV, rx_callback, (void *)0xbU, &filter_b);
	zassert_true(id_a >= 0 && id_b >= 0 && id_a != id_b, "two distinct slots (%d, %d)", id_a,
		     id_b);

	zassert_equal(can_add_rx_filter(CAN_DEV, rx_callback, NULL,
					&(struct can_filter){ .id = 1U, .mask = 1U,
							      .flags = BIT(1) }),
		      -ENOTSUP, "only CAN_FILTER_IDE is supported");

	/* Frames are matched in software against each slot's mask. */
	model_inject_rx_std(0x100U, 1U, &payload);
	zassert_equal(rx_cb_count, 1, "only the matching filter fired");
	zassert_equal(rx_cb_user_data, (void *)0xaU, "filter A matched");

	/* Releasing a slot makes it reusable. */
	can_remove_rx_filter(CAN_DEV, id_a);
	model_inject_rx_std(0x100U, 1U, &payload);
	zassert_equal(rx_cb_count, 1, "the released filter no longer fires");
	model_inject_rx_std(0x200U, 1U, &payload);
	zassert_equal(rx_cb_count, 2, "filter B still does");
	zassert_equal(rx_cb_user_data, (void *)0xbU, "filter B matched");

	zassert_equal(can_add_rx_filter(CAN_DEV, rx_callback, NULL, &filter_a), id_a,
		      "the freed slot is handed out again");

	can_remove_rx_filter(CAN_DEV, id_a);
	can_remove_rx_filter(CAN_DEV, id_b);
	can_remove_rx_filter(CAN_DEV, 99); /* out of range: must not corrupt anything */
	model_inject_rx_std(0x100U, 1U, &payload);
	zassert_equal(rx_cb_count, 2, "no filter is armed any more");
}

/* ---- state machine ---------------------------------------------------- */

static struct can_state_change_callback state_cb;
static enum can_state cb_state;
static struct can_bus_err_cnt cb_err;
static int cb_count;

static void state_change_handler(const struct device *dev,
				 struct can_state_change_callback *callback, enum can_state state,
				 struct can_bus_err_cnt err_cnt)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(callback);

	cb_state = state;
	cb_err = err_cnt;
	cb_count++;
}

ZTEST(can_agm, test_11_state_change_callbacks_follow_the_status_register)
{
	zassert_ok(can_set_mode(CAN_DEV, CAN_MODE_NORMAL));
	zassert_ok(can_start(CAN_DEV));

	state_cb.handler = state_change_handler;
	zassert_ok(can_add_state_change_callback(CAN_DEV, &state_cb), "callback registered");

	/* SR.ES -> error warning, with the error counters read out of RXERR/TXERR. */
	reg_wr(R_RXERR, 3U);
	reg_wr(R_TXERR, 7U);
	reg_wr(R_SR, SR_ES);
	reg_wr(R_IR, IR_EI);
	trigger_can_irq();

	zassert_equal(cb_count, 1, "the state change fired");
	zassert_equal(cb_state, CAN_STATE_ERROR_WARNING, "SR.ES -> ERROR_WARNING");
	zassert_equal(cb_err.rx_err_cnt, 3U, "RX error counter");
	zassert_equal(cb_err.tx_err_cnt, 7U, "TX error counter");

	/* Counters back to zero: error active again. */
	reg_wr(R_RXERR, 0U);
	reg_wr(R_TXERR, 0U);
	reg_wr(R_SR, 0U);
	reg_wr(R_IR, IR_EI);
	trigger_can_irq();
	zassert_equal(cb_count, 2, "second transition");
	zassert_equal(cb_state, CAN_STATE_ERROR_ACTIVE, "no error bits -> ERROR_ACTIVE");

	/* IR.EPI toggles between error passive and the current warning level. */
	reg_wr(R_IR, IR_EPI);
	trigger_can_irq();
	zassert_equal(cb_state, CAN_STATE_ERROR_PASSIVE, "EPI from active -> ERROR_PASSIVE");
	reg_wr(R_IR, IR_EPI);
	trigger_can_irq();
	zassert_equal(cb_state, CAN_STATE_ERROR_WARNING, "EPI from passive -> ERROR_WARNING");
	zassert_equal(cb_count, 4, "four transitions in total");

	zassert_ok(can_remove_state_change_callback(CAN_DEV, &state_cb), "callback removed");
	reg_wr(R_IR, IR_EPI);
	trigger_can_irq();
	zassert_equal(cb_count, 4, "no callback after removal");
}

ZTEST(can_agm, test_12_bus_off_is_reported_and_refuses_transmissions)
{
	struct can_frame frame = { 0 };
	enum can_state state;

	zassert_ok(can_set_mode(CAN_DEV, CAN_MODE_NORMAL));
	zassert_ok(can_start(CAN_DEV));

	frame.id = 0x123U;
	frame.dlc = 0U;

	reg_wr(R_RXERR, 128U);
	reg_wr(R_TXERR, 128U);
	reg_wr(R_SR, SR_BS);
	reg_wr(R_IR, IR_EI);
	trigger_can_irq();

	zassert_ok(can_get_state(CAN_DEV, &state, NULL), "state readable");
	zassert_equal(state, CAN_STATE_BUS_OFF, "SR.BS -> BUS_OFF");

	/* Not in manual-recovery mode, so the driver re-arms the controller by
	 * leaving reset mode immediately (MOD.RM stays clear). */
	zassert_equal(reg_rd(R_MOD) & MOD_RM, 0U, "automatic recovery clears MOD.RM");

	reg_wr(R_SR, SR_TBS);
	zassert_equal(can_send(CAN_DEV, &frame, K_MSEC(100), tx_callback, NULL), -ENETUNREACH,
		      "a bus-off controller refuses to transmit");
	zassert_equal(tx_cb_count, 0, "nothing was queued");
}

ZTEST(can_agm, test_13_start_stop_lifecycle_and_aborted_transmission)
{
	struct can_frame frame = { 0 };
	enum can_state state;

	zassert_ok(can_set_mode(CAN_DEV, CAN_MODE_NORMAL));

	frame.id = 0x321U;
	frame.dlc = 0U;

	zassert_equal(can_send(CAN_DEV, &frame, K_MSEC(100), tx_callback, NULL), -ENETDOWN,
		      "a stopped controller refuses to transmit");
	zassert_ok(can_get_state(CAN_DEV, &state, NULL), "state readable while stopped");
	zassert_equal(state, CAN_STATE_STOPPED, "stopped state");

	zassert_ok(can_start(CAN_DEV), "start");
	zassert_equal(can_start(CAN_DEV), -EALREADY, "already started");
	zassert_equal(reg_rd(R_MOD) & MOD_RM, 0U, "start leaves reset mode");

	reg_wr(R_SR, SR_TBS);
	zassert_ok(can_send(CAN_DEV, &frame, K_MSEC(100), tx_callback, NULL), "queued");
	zassert_equal(tx_cb_count, 0, "still in flight");

	/* can_stop() aborts a pending transmission. */
	zassert_ok(can_stop(CAN_DEV), "stop");
	zassert_equal(tx_cb_count, 1, "the abort was reported through the tx callback");
	zassert_equal(tx_cb_status, -ENETDOWN, "status -ENETDOWN (err %d)", tx_cb_status);
	zassert_equal(reg_rd(R_MOD) & MOD_RM, MOD_RM, "stop enters reset mode");
	zassert_equal(can_stop(CAN_DEV), -EALREADY, "already stopped");
}
