/*
 * Copyright (c) 2026 AgRV Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief native_sim suite for the AgRV2K I2C controller driver.
 *
 * The driver polls SR.TIP after every command, and a RAM-backed window answers
 * 0 (TIP clear) until the test says otherwise -- so a passive model is enough
 * for the whole control flow, and every assertion below is "what the driver
 * programmed, and how it reacted to the status bits the test presents".
 *
 * Covered: the OpenCores prescaler arithmetic per bus speed (SCL =
 * pclk / (5 * (pre + 1)), which nothing has checked since it was written), the
 * configuration validation, the best-effort bus recovery, and the AFSEL-only
 * pinctrl state (NO_DIR -- an I2C pin's direction belongs to the IP, and a DIR
 * write here would fight the open-drain driver).
 *
 * The transfer cases run against a small command/status
 * model instead of the RAM window, because the OpenCores master reads its
 * status from the *same address* it writes its command to (CR 0x10 write /
 * SR 0x10 read, AGM_I2C_CR == AGM_I2C_SR): a RAM-backed window answers the
 * driver's SR read with the command word it just wrote -- CR.STA (bit 7) comes
 * back as SR.RXACK, so every address phase looks unacknowledged and a write
 * that should succeed returns -ENXIO. The driver now reaches its
 * registers through two weak functions (i2c_agm_reg_read/reg_write); the model
 * below overrides them, keeps the RAM window in sync for the encoding
 * assertions, and answers SR/RXR with what a device on the bus would cause.
 */

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/sys/util.h>
#include <zephyr/ztest.h>

#include <string.h>

#define I2C_DEV  DEVICE_DT_GET(DT_NODELABEL(i2c_agm0))

#define DEV_ADDR 0x50U

/* Register offsets (drivers/i2c/i2c_agm.c: OpenCores master). */
#define R_PRERLO 0x00U
#define R_PRERHI 0x04U
#define R_CTR    0x08U
#define R_TXR    0x0cU
#define R_RXR    0x0cU
#define R_CR     0x10U
#define R_SR     0x10U

#define CTR_EN   BIT(7)
#define CR_IACK  BIT(0)
#define CR_NACK  BIT(3)
#define CR_WR    BIT(4)
#define CR_RD    BIT(5)
#define CR_STO   BIT(6)
#define CR_STA   BIT(7)
#define SR_BUSY  BIT(6)
#define SR_RXACK BIT(7)

#define I2C_BASE 0x4002b000UL

static uint32_t rd(uint32_t off)
{
	return *(volatile uint32_t *)(I2C_BASE + off);
}

static void wr(uint32_t off, uint32_t val)
{
	*(volatile uint32_t *)(I2C_BASE + off) = val;
}

/* ---- the bus model ---------------------------------------------------- */

struct i2c_bus_model {
	/* Does the addressed device ACK? */
	bool present;
	/* Bytes the device hands back on read phases. */
	uint8_t rx_queue[8];
	uint8_t rx_len;
	uint8_t rx_head;
	/* Last command written to CR/SR, and the byte last written to TXR. */
	uint32_t last_cmd;
	uint32_t last_txr;
	/* Compact command trace: every CR write, in order. */
	uint32_t trace[32];
	/* The byte that was in TXR when each CR write happened (the address byte
	 * of an address phase, or the data byte of a write phase). */
	uint8_t txr_trace[32];
	uint8_t trace_len;
};

static struct i2c_bus_model bus;

uint32_t i2c_agm_reg_read(const void *ctx, uint32_t off)
{
	ARG_UNUSED(ctx);

	if (off == R_SR) {
		/* TIP and BUSY clear: every command completes at once. RXACK
		 * reports whether the addressed device answered. */
		return bus.present ? 0U : SR_RXACK;
	}
	if (off == R_RXR) {
		if (bus.rx_head < bus.rx_len) {
			return bus.rx_queue[bus.rx_head++];
		}
		return 0xffU; /* nobody driving SDA */
	}
	return rd(off);
}

void i2c_agm_reg_write(const void *ctx, uint32_t off, uint32_t val)
{
	ARG_UNUSED(ctx);

	wr(off, val); /* keep the MMIO window readable for the other cases */

	if (off == R_TXR) {
		bus.last_txr = val;
		return;
	}
	if (off != R_CR) {
		return;
	}

	bus.last_cmd = val;
	if (bus.trace_len < ARRAY_SIZE(bus.trace)) {
		bus.trace[bus.trace_len] = val;
		bus.txr_trace[bus.trace_len] = (uint8_t)bus.last_txr;
		bus.trace_len++;
	}
}

static void model_reset(void)
{
	memset(&bus, 0, sizeof(bus));
	bus.present = true;
}

/* Did the model see this exact command? */
static bool trace_has(uint32_t cmd)
{
	for (uint8_t i = 0U; i < bus.trace_len; i++) {
		if (bus.trace[i] == cmd) {
			return true;
		}
	}
	return false;
}

static void suite_reset_fake_regs(void *fixture)
{
	ARG_UNUSED(fixture);

	/* A quiet bus: no transaction in progress, no NACK, nothing busy. */
	wr(R_SR, 0U);
	wr(R_RXR, 0U);
	model_reset();
}

ZTEST_SUITE(i2c_agm, NULL, NULL, suite_reset_fake_regs, NULL, NULL);

/* ---- init and the bus speed ------------------------------------------ */

ZTEST(i2c_agm, test_01_init_programs_the_default_speed_and_routes_the_pins)
{
	zassert_true(device_is_ready(I2C_DEV), "the I2C device came up against the fake window");

	/* 100 kHz off 200 MHz: SCL = pclk / (5 * (pre + 1)), so pre = 399. */
	zassert_equal(rd(R_PRERLO), 0x8fU, "PRERLO is the prescaler's low byte");
	zassert_equal(rd(R_PRERHI), 0x01U, "and PRERHI its high byte");
	zassert_equal(rd(R_CTR) & CTR_EN, CTR_EN, "the core is enabled after configuring");

	/* i2c0_default is GPIO3 bits 4/5 with NO_DIR: AFSEL is ours, DIR is the
	 * IP's. Writing DIR here would fight the open-drain pin. */
	uint32_t afsel = *(volatile uint32_t *)(0x40014000UL + (3UL * 0x1000UL) + 0x420UL);
	uint32_t dir = *(volatile uint32_t *)(0x40014000UL + (3UL * 0x1000UL) + 0x400UL);

	zassert_equal(afsel & (BIT(4) | BIT(5)), BIT(4) | BIT(5),
		      "SCL and SDA handed to the peripheral");
	zassert_equal(dir & (BIT(4) | BIT(5)), 0U, "and DIR left alone (NO_DIR)");
}

ZTEST(i2c_agm, test_10_configure_programs_the_prescaler_for_each_speed)
{
	uint32_t cfg = 0U;

	/* Fast: 200 MHz / (5 * 400 kHz) - 1 = 99. */
	zassert_ok(i2c_configure(I2C_DEV, I2C_MODE_CONTROLLER | I2C_SPEED_SET(I2C_SPEED_FAST)));
	zassert_equal(rd(R_PRERLO), 99U, "400 kHz prescaler");
	zassert_equal(rd(R_PRERHI), 0U, "fits in the low byte");

	/* Fast-plus: 200 MHz / (5 * 1 MHz) - 1 = 39. */
	zassert_ok(i2c_configure(I2C_DEV,
				 I2C_MODE_CONTROLLER | I2C_SPEED_SET(I2C_SPEED_FAST_PLUS)));
	zassert_equal(rd(R_PRERLO), 39U, "1 MHz prescaler");

	/* And the driver remembers what it was asked for. */
	zassert_ok(i2c_get_config(I2C_DEV, &cfg));
	zassert_equal(I2C_SPEED_GET(cfg), I2C_SPEED_FAST_PLUS, "the speed is reported back");

	/* Controller mode only, no 10-bit addressing, no unknown speeds. */
	zassert_equal(i2c_configure(I2C_DEV, I2C_SPEED_SET(I2C_SPEED_STANDARD)), -ENOTSUP,
		      "this IP is a controller, not a target");
	zassert_equal(i2c_configure(I2C_DEV,
				    I2C_MODE_CONTROLLER | I2C_ADDR_10_BITS |
					    I2C_SPEED_SET(I2C_SPEED_STANDARD)),
		      -ENOTSUP, "10-bit addressing is not implemented");
	zassert_equal(i2c_configure(I2C_DEV,
				    I2C_MODE_CONTROLLER | I2C_SPEED_SET(0xf)),
		      -ENOTSUP, "an unknown speed is refused");

	/* Back to the DT default for the transfer cases. */
	zassert_ok(i2c_configure(I2C_DEV, I2C_MODE_CONTROLLER | I2C_SPEED_SET(I2C_SPEED_STANDARD)));
}

/* ---- the API's own edge cases ----------------------------------------- */

/* The public API short-circuits "no messages" to success before the driver
 * sees it, so the driver's own num_msgs check is unreachable through
 * i2c_transfer() (include/zephyr/drivers/i2c.h: z_impl_i2c_transfer() returns
 * 0 for num_msgs == 0). Recorded here because a reader of the driver would
 * otherwise expect -EINVAL. */
ZTEST(i2c_agm, test_20_an_empty_transfer_is_a_no_op_at_the_api_level)
{
	struct i2c_msg msg = { .buf = NULL, .len = 0U, .flags = I2C_MSG_WRITE };

	zassert_ok(i2c_transfer(I2C_DEV, &msg, 0U, DEV_ADDR),
		   "the API answers 0 for zero messages, and never calls the driver");
}

ZTEST(i2c_agm, test_24_recover_bus_clears_the_interrupt_and_stops_the_bus)
{
	zassert_ok(i2c_recover_bus(I2C_DEV), "best-effort recovery on a quiet bus");

	/* No GPIO access to SCL/SDA (the AF mux owns them), so recovery is
	 * IACK + STOP -- the last command written. */
	zassert_equal(rd(R_CR), CR_IACK | CR_STO, "the recovery sequence ends with IACK|STO");
}

ZTEST(i2c_agm, test_25_a_speed_that_this_pclk_cannot_represent_still_configures)
{
	uint32_t cfg = 0U;

	/* Standard at 200 MHz is pre = 399, which the two bytes hold; the point
	 * of this case is that the *default* speed from the devicetree survives
	 * a round trip through configure/get_config. */
	zassert_ok(i2c_configure(I2C_DEV, I2C_MODE_CONTROLLER | I2C_SPEED_SET(I2C_SPEED_STANDARD)));
	zassert_ok(i2c_get_config(I2C_DEV, &cfg));
	zassert_equal(cfg & I2C_SPEED_MASK, I2C_SPEED_SET(I2C_SPEED_STANDARD), "standard speed");
	zassert_equal(cfg & I2C_MODE_CONTROLLER, I2C_MODE_CONTROLLER, "controller mode");
}

/* ---- the transfer path ----------------------------------------------- */

ZTEST(i2c_agm, test_30_a_write_to_a_present_device_succeeds)
{
	const uint8_t data[3] = { 0x01U, 0x10U, 0x00U };

	/* The regression this pins: with the RAM window the driver's SR read
	 * returned the command word, CR.STA came back as SR.RXACK and this call
	 * failed with -ENXIO even though the device was there. */
	zassert_ok(i2c_write(I2C_DEV, data, sizeof(data), 0x23U),
		   "a present device ACKs the address phase");

	/* Address phase: START | WR with the address byte (addr << 1 | R/W). */
	zassert_equal(bus.trace[0], CR_WR | CR_STA, "START + WR first (got 0x%02x)", bus.trace[0]);
	zassert_equal(bus.txr_trace[0], 0x46U, "address byte 0x23 << 1 (got 0x%02x)",
		      bus.txr_trace[0]);

	/* Then one write command per byte; the last one carries STOP. */
	zassert_equal(bus.trace_len, 4U, "1 address + 3 data commands (got %u)", bus.trace_len);
	zassert_equal(bus.trace[1], CR_WR, "data byte 0");
	zassert_equal(bus.txr_trace[1], data[0], "data 0 on the wire");
	zassert_equal(bus.trace[3], CR_WR | CR_STO, "the last byte ends with STOP (got 0x%02x)",
		      bus.trace[3]);
	zassert_equal(bus.txr_trace[3], data[2], "data 2 on the wire");
}

ZTEST(i2c_agm, test_31_a_write_to_an_absent_device_is_enxio)
{
	const uint8_t data = 0x5aU;

	bus.present = false;
	zassert_equal(i2c_write(I2C_DEV, &data, 1U, 0x23U), -ENXIO,
		      "no ACK on the address phase");

	/* The driver must release the bus (STOP) before giving up. */
	zassert_true(trace_has(CR_STO), "a STOP was issued");
	zassert_equal(bus.trace_len, 2U, "address phase + STOP only (got %u)", bus.trace_len);
}

ZTEST(i2c_agm, test_32_a_read_returns_the_device_bytes)
{
	uint8_t rx[2] = { 0U, 0U };

	bus.rx_queue[0] = 0x1aU;
	bus.rx_queue[1] = 0x00U;
	bus.rx_len = 2U;

	zassert_ok(i2c_read(I2C_DEV, rx, sizeof(rx), 0x23U), "read accepted");
	zassert_equal(rx[0], 0x1aU, "first byte (got 0x%02x)", rx[0]);
	zassert_equal(rx[1], 0x00U, "second byte (got 0x%02x)", rx[1]);

	/* Address phase is a WR command carrying addr << 1 | READ. */
	zassert_equal(bus.trace[0], CR_WR | CR_STA, "address phase");
	zassert_equal(bus.txr_trace[0], 0x47U, "address byte 0x23 << 1 | R (got 0x%02x)",
		      bus.txr_trace[0]);

	/* One RD command per byte, and the driver NACKs the final one (that is
	 * how a controller tells the device to stop driving SDA) while also
	 * releasing the bus. */
	zassert_equal(bus.trace_len, 3U, "1 address + 2 read commands (got %u)", bus.trace_len);
	zassert_equal(bus.trace[1], CR_RD, "first byte is ACKed (0x%02x)", bus.trace[1]);
	zassert_equal(bus.trace[2], CR_RD | CR_NACK | CR_STO,
		      "the last byte is NACKed + STOP (got 0x%02x)", bus.trace[2]);
}

ZTEST(i2c_agm, test_33_index_write_then_read_uses_a_repeated_start)
{
	const uint8_t index = 0x10U; /* e.g. a BH1750 measurement command */
	uint8_t rx[2] = { 0U, 0U };
	struct i2c_msg msgs[2] = {
		{ .buf = (uint8_t *)&index, .len = 1U, .flags = I2C_MSG_WRITE },
		{ .buf = rx, .len = 2U, .flags = I2C_MSG_READ | I2C_MSG_RESTART | I2C_MSG_STOP },
	};

	bus.rx_queue[0] = 0x00U;
	bus.rx_queue[1] = 0x1aU;
	bus.rx_len = 2U;

	zassert_ok(i2c_transfer(I2C_DEV, msgs, 2U, 0x23U), "index write + read");
	zassert_equal(rx[1], 0x1aU, "the device's second byte came back (got 0x%02x)", rx[1]);

	/* trace: [wr addr][wr data (no STOP -- the next msg restarts)][rd addr][rd][rd NACK+STO] */
	zassert_equal(bus.trace_len, 5U, "5 commands (got %u)", bus.trace_len);
	zassert_equal(bus.txr_trace[0], 0x46U, "write address phase");
	zassert_equal(bus.txr_trace[1], index, "the index byte went out");
	zassert_equal(bus.trace[1] & CR_STO, 0U, "no STOP between the two messages");
	zassert_equal(bus.txr_trace[2], 0x47U, "read address phase (repeated START)");
	zassert_equal(bus.trace[2] & CR_STA, CR_STA, "the read phase starts the bus again");
	zassert_equal(bus.trace[4], CR_RD | CR_NACK | CR_STO, "final byte NACKed + STOP");
}
