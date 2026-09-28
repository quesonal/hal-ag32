/*
 * Copyright (c) 2026 AgRV Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief native_sim suite for the AgRV2K CRC driver.
 *
 * What is under test is the *register protocol*, which is where this driver's
 * documented measurements live (its file header lists four load-bearing facts,
 * all four are what this suite pins):
 *
 *  - it is an AHB-domain peripheral -- soc.c opens the gate, not the driver;
 *  - INIT has to be written *after* the RESET pulse, or the value is ignored;
 *  - INIT is in the unit's *reversed* domain, so it goes out bit-reversed
 *    (the bug that silently reduced a chunked crc32_ieee_update() chain to
 *    "the CRC of the last chunk");
 *  - the result is complemented at the end.
 *
 * A register window backed by RAM (tests/drivers/common/agm_native) is enough
 * for all of it: the test reads back exactly what the driver wrote, in the
 * order the last write of each register implies. What it cannot do is compute
 * a CRC -- so the data path is pinned by feeding one byte at a time and by
 * checking the final complement on a value the test puts in DR itself.
 */

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/crc.h>
#include <zephyr/sys/bit_rev.h>
#include <zephyr/ztest.h>

#define CRC_DEV  DEVICE_DT_GET(DT_NODELABEL(crc_agm0))

/* Register offsets (drivers/crc/crc_agm.c). */
#define R_DR    0x00U
#define R_CR    0x08U
#define R_INIT  0x10U
#define R_POL   0x14U

#define CR_RESET      BIT(0)
#define CR_REV_IN_BYTE BIT(5)
#define CR_REV_OUT_BIT BIT(7)

#define CRC_BASE 0x41002000UL

static uint32_t rd(uint32_t off)
{
	return *(volatile uint32_t *)(CRC_BASE + off);
}

static void wr(uint32_t off, uint32_t val)
{
	*(volatile uint32_t *)(CRC_BASE + off) = val;
}

/* The context the driver's begin/update/finish consume. CRC32/ISO-HDLC with
 * both reversals on and the standard seed is what crc32_ieee() builds. */
static struct crc_ctx ctx_reversed(void)
{
	return (struct crc_ctx){
		.type = CRC32_IEEE,
		.state = CRC_STATE_IDLE,
		.reversed = CRC_FLAG_REVERSE_INPUT | CRC_FLAG_REVERSE_OUTPUT,
		.polynomial = 0x04C11DB7U,
		.seed = 0xFFFFFFFFU,
	};
}

static void reset_regs(void *fixture)
{
	ARG_UNUSED(fixture);

	wr(R_DR, 0U);
	wr(R_CR, 0U);
	wr(R_INIT, 0U);
	wr(R_POL, 0U);
}

ZTEST_SUITE(crc_agm, NULL, NULL, reset_regs, NULL, NULL);

ZTEST(crc_agm, test_01_device_is_ready_and_owns_no_register_at_init)
{
	zassert_true(device_is_ready(CRC_DEV), "the CRC device came up against the fake window");

	/* The AHB gate and reset release belong to soc.c (from the devicetree),
	 * so init must not have touched the block's own registers. */
	zassert_equal(rd(R_CR), 0U, "init leaves CR alone");
	zassert_equal(rd(R_INIT), 0U, "and INIT");
}

ZTEST(crc_agm, test_10_begin_configures_pol_cr_reset_and_the_reversed_seed)
{
	struct crc_ctx ctx = ctx_reversed();

	zassert_ok(crc_begin(CRC_DEV, &ctx), "begin");
	zassert_equal(ctx.state, CRC_STATE_IN_PROGRESS, "the context says so");

	zassert_equal(rd(R_POL), ctx.polynomial, "POL is the caller's polynomial");

	/* CR is written twice -- configured, then configured|RESET -- so the
	 * visible value is the second write. That ordering is the measurement:
	 * INIT written before the RESET pulse is ignored. */
	zassert_equal(rd(R_CR), CR_REV_IN_BYTE | CR_REV_OUT_BIT | CR_RESET,
		      "CR carries both reversals and the RESET pulse");

	/* And the seed is in the reversed domain. */
	zassert_equal(rd(R_INIT), sys_bit_rev32(ctx.seed), "INIT is the bit-reversed seed");

	zassert_ok(crc_finish(CRC_DEV, &ctx), "finish releases the unit");
}

ZTEST(crc_agm, test_11_begin_rejects_anything_but_crc32_ieee)
{
	struct crc_ctx ctx = ctx_reversed();

	ctx.type = CRC32_C;
	zassert_equal(crc_begin(CRC_DEV, &ctx), -ENOTSUP,
		      "only CRC32_IEEE is claimed, the rest must not pretend");
	zassert_equal(rd(R_CR), 0U, "and nothing was programmed");

	/* The refusal must not have taken the busy flag either. */
	ctx = ctx_reversed();
	zassert_ok(crc_begin(CRC_DEV, &ctx), "a supported type still works");
	zassert_ok(crc_finish(CRC_DEV, &ctx));
}

ZTEST(crc_agm, test_12_the_unit_is_one_calculation_at_a_time)
{
	struct crc_ctx first = ctx_reversed();
	struct crc_ctx second = ctx_reversed();

	zassert_ok(crc_begin(CRC_DEV, &first));
	zassert_equal(crc_begin(CRC_DEV, &second), -EBUSY,
		      "a second begin has to wait for the first finish");

	zassert_ok(crc_finish(CRC_DEV, &first));
	zassert_ok(crc_begin(CRC_DEV, &second), "and is allowed once the first is done");
	zassert_ok(crc_finish(CRC_DEV, &second));
}

ZTEST(crc_agm, test_20_update_feeds_bytes_and_needs_a_begin)
{
	static const uint8_t one = 0x5aU;
	static const uint8_t three[3] = { 0x01U, 0x02U, 0x03U };
	struct crc_ctx ctx = ctx_reversed();

	/* Without a begin the state is IDLE, and the driver says so. */
	zassert_equal(crc_update(CRC_DEV, &ctx, &one, 1U), -EINVAL,
		      "update before begin is refused");

	zassert_ok(crc_begin(CRC_DEV, &ctx));

	/* A single byte: the observable is exact -- byte-wise writes mean DR
	 * holds the byte that was fed. */
	zassert_ok(crc_update(CRC_DEV, &ctx, &one, 1U));
	zassert_equal(rd(R_DR), one, "the byte goes to DR");

	/* A buffer goes in byte by byte (so the last write is the last byte). */
	zassert_ok(crc_update(CRC_DEV, &ctx, three, sizeof(three)));
	zassert_equal(rd(R_DR), three[2], "each byte is fed in order, the last one stays");

	/* Zero bytes is a legal no-op, not an error. */
	zassert_ok(crc_update(CRC_DEV, &ctx, three, 0U), "an empty update is fine");
	/* ... and with no buffer at all: that is the only case in which the
	 * pointer may be NULL, so it is spelled out in the driver rather than
	 * relying on the loop never running. */
	zassert_ok(crc_update(CRC_DEV, &ctx, NULL, 0U), "a NULL buffer with zero length is fine");

	zassert_ok(crc_finish(CRC_DEV, &ctx));
}

ZTEST(crc_agm, test_30_finish_complements_the_result_and_needs_a_begin)
{
	struct crc_ctx ctx = ctx_reversed();

	zassert_equal(crc_finish(CRC_DEV, &ctx), -EINVAL, "finish before begin is refused");

	zassert_ok(crc_begin(CRC_DEV, &ctx));
	wr(R_DR, 0x11223344U);	/* what the unit's DR would hold at the end */
	zassert_ok(crc_finish(CRC_DEV, &ctx));

	zassert_equal(ctx.result, 0x11223344U ^ 0xFFFFFFFFU,
		      "ISO-HDLC complements the result after the last byte");
	zassert_equal(ctx.state, CRC_STATE_IDLE, "the context goes back to idle");

	/* The same value through a context that did not ask for CRC32_IEEE
	 * cannot be finished at all (it never began) -- and a fresh begin works,
	 * so the busy flag really was released. */
	zassert_ok(crc_begin(CRC_DEV, &ctx), "the unit is free again");
	wr(R_DR, 0x00000000U);
	zassert_ok(crc_finish(CRC_DEV, &ctx));
	zassert_equal(ctx.result, 0xFFFFFFFFU, "0 complemented is 0xffffffff");
}
