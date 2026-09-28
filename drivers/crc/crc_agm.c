/*
 * Copyright (c) 2026 AgRV Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * AGM AgRV2K CRC driver.
 *
 * The block (@ 0x4100_2000) is the familiar STM32-CRC
 * register set: DR (feed/read, 8/16/32-bit access), IDR (8-bit scratch),
 * CR (poly size, input/output reversal, endianness, RESET), INIT (seed) and
 * POL (polynomial, default 0x04C11DB7). Four things had to be measured
 * rather than guessed, and all four are load-bearing:
 *
 *   1. it is an *AHB*-domain peripheral: its clock gate is bit 2 of
 *      SYS.AHB_CLKENABLE and its reset release bit 2 of SYS.AHB_RESET (the
 *      vendor example calls SYS_EnableAHBClock(AHB_MASK_CRC0)). With the APB
 *      bit the registers read back as 0xffffffff/0x0 and nothing computes.
 *      soc.c opens both from the devicetree; this driver never touches SYS.
 *   2. **configure, then write RESET**: writing INIT alone does not load the
 *      seed into DR here; the RESET bit is what does it (the STM32 driver's
 *      order, LL_CRC_ResetCRCCalculationUnit()) -- and INIT has to be written
 *      *after* that RESET, not before it (see 3.).
 *   3. CRC32/ISO-HDLC (Zephyr's CRC32_IEEE) = polynomial 0x04C11DB7,
 *      REV_IN = byte-wise, REV_OUT = bit-wise, and a final complement of the
 *      result -- verified against the standard check value ("123456789" ->
 *      0xCBF43926) and against crc32_ieee() over 64 B and 99944 B buffers.
 *      With byte-wise feeding the POLY/ENDIAN fields do not matter, which is
 *      why this driver feeds bytes like the STM32 one does. **The seed is in
 *      the reversed domain**: the unit computes
 *          DR = raw_reflected(bit_rev32(INIT), data),
 *      so INIT gets sys_bit_rev32(ctx->seed). The
 *      one-shot path cannot see this -- its seed is 0xffffffff, which is
 *      symmetric under bit reversal -- but a *chunked* accumulation
 *      (crc32_ieee_update() chains, which is how the bootloader CRCs a store,
 *      a slot or an upload) silently degraded to "the CRC of the last chunk"
 *      with the unreversed seed: the loader then refused an image it had just
 *      verified, because its own CRC of the same bytes came out different.
 *      Guarded by samples/crc_agm's chunked check.
 *   4. initialization is early on purpose: the fabric verifier CRC's the
 *      slot from soc.c's PRE_KERNEL_1 hook, i.e. before any driver normally
 *      exists (see soc/agm/agrv2k/soc.c step 0). Zephyr's crc_hardware.c
 *      routes crc32_ieee() here as soon as a driver is present, so that call
 *      has to work from the very first line of user code -- hence EARLY and
 *      an atomic "busy" flag instead of a semaphore (a blocking take is not
 *      legal before the kernel is up).
 *
 * Only CRC32_IEEE is claimed (and therefore enabled through Zephyr's
 * CRC_DRIVER_HAS_CRC32_IEEE): the other algorithms this unit could compute
 * (CRC32_C, CRC-32/MPEG-2, the 16/8/7-bit sizes) are unmeasured here and
 * return -ENOTSUP rather than pretend.
 */

#define DT_DRV_COMPAT agm_agrv2k_crc

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/drivers/crc.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/bit_rev.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/sys/util.h>

/* Register offsets (AltaRiscv.h::CRC_TypeDef). */
#define CRC_AGM_DR		0x00U
#define CRC_AGM_CR		0x08U
#define CRC_AGM_INIT		0x10U
#define CRC_AGM_POL		0x14U

/* CR bits (AltaRiscv.h::CRC_CR_*). */
#define CRC_AGM_CR_RESET	BIT(0)
#define CRC_AGM_CR_POLY32	(0U << 3)
#define CRC_AGM_CR_REV_IN_BYTE	BIT(5)
#define CRC_AGM_CR_REV_OUT_BIT	BIT(7)

/* ISO-HDLC complements the result after the last byte. */
#define CRC_AGM_IEEE_XOR_OUT	0xFFFFFFFFU

struct crc_agm_config {
	mem_addr_t base;
};

struct crc_agm_data {
	atomic_t busy;
};

static int crc_agm_begin(const struct device *dev, struct crc_ctx *ctx)
{
	const struct crc_agm_config *config = dev->config;
	struct crc_agm_data *data = dev->data;
	uint32_t cr;

	if (ctx->type != CRC32_IEEE) {
		return -ENOTSUP;
	}

	/* Non-blocking: this can run before the kernel is up. */
	if (!atomic_cas(&data->busy, 0, 1)) {
		return -EBUSY;
	}

	cr = CRC_AGM_CR_POLY32;
	if ((ctx->reversed & CRC_FLAG_REVERSE_INPUT) != 0U) {
		cr |= CRC_AGM_CR_REV_IN_BYTE;
	}
	if ((ctx->reversed & CRC_FLAG_REVERSE_OUTPUT) != 0U) {
		cr |= CRC_AGM_CR_REV_OUT_BIT;
	}

	sys_write32(ctx->polynomial, config->base + CRC_AGM_POL);
	sys_write32(cr, config->base + CRC_AGM_CR);
	sys_write32(cr | CRC_AGM_CR_RESET, config->base + CRC_AGM_CR);
	/* Order and bit order both matter, and both are measured (see the file
	 * header): INIT has to come *after* the RESET (written before it, the
	 * value is ignored and every operation restarts from the default), and
	 * it has to be bit-reversed, because the unit's register domain is the
	 * reversed one -- DR = raw_reflected(bit_rev32(INIT), data). The
	 * one-shot path cannot see either mistake (its seed is the symmetric
	 * 0xffffffff); a crc32_ieee_update() chain can. */
	sys_write32(sys_bit_rev32(ctx->seed), config->base + CRC_AGM_INIT);

	ctx->state = CRC_STATE_IN_PROGRESS;

	return 0;
}

static int crc_agm_update(const struct device *dev, struct crc_ctx *ctx, const void *buffer,
			  size_t bufsize)
{
	const struct crc_agm_config *config = dev->config;
	const uint8_t *buf = buffer;
	volatile uint8_t *dr;

	if (ctx->state != CRC_STATE_IN_PROGRESS) {
		return -EINVAL;
	}

	/* Zero is a legal chunk (and the only case in which `buffer` may be
	 * NULL): nothing is fed, the running CRC is untouched. Stated here
	 * instead of relying on the loop body never executing. */
	if (bufsize == 0U) {
		return 0;
	}

	/* Byte-wide feeds: an 8-bit write keeps the byte order out of the
	 * picture (the unit reverses bits and bytes per REV_IN itself). */
	/* The address has to be in a register, not re-read from the config
	 * struct: the store below is volatile, so the compiler is free to
	 * assume it clobbers `config->base` and reload it -- from XIP flash --
	 * on every byte: 66 cycles/byte with the reload, 7 without
	 * (the unit's own cost is ~1 AHB cycle per byte). */
	dr = (volatile uint8_t *)(config->base + CRC_AGM_DR);
	for (size_t i = 0U; i < bufsize; i++) {
		*dr = buf[i];
	}

	return 0;
}

static int crc_agm_finish(const struct device *dev, struct crc_ctx *ctx)
{
	const struct crc_agm_config *config = dev->config;
	struct crc_agm_data *data = dev->data;

	if (ctx->state != CRC_STATE_IN_PROGRESS) {
		return -EINVAL;
	}

	ctx->result = sys_read32(config->base + CRC_AGM_DR);
	if (ctx->type == CRC32_IEEE) {
		ctx->result ^= CRC_AGM_IEEE_XOR_OUT;
	}

	ctx->state = CRC_STATE_IDLE;
	atomic_clear(&data->busy);

	return 0;
}

static DEVICE_API(crc, crc_agm_api) = {
	.begin = crc_agm_begin,
	.update = crc_agm_update,
	.finish = crc_agm_finish,
};

static int crc_agm_init(const struct device *dev)
{
	ARG_UNUSED(dev);

	/* The clock gate and the reset release are opened by soc.c from the
	 * devicetree, and this driver has nothing else to set up. */
	return 0;
}

#define CRC_AGM_DEFINE(inst)                                                                \
	static const struct crc_agm_config crc_agm_config_##inst = {                        \
		.base = DT_INST_REG_ADDR(inst),                                             \
	};                                                                                  \
	static struct crc_agm_data crc_agm_data_##inst;                                     \
	/* PRE_KERNEL_1 priority 0: this has to be up before soc.c's hook, which           \
	 * SYS_INITs at PRE_KERNEL_1 priority 1 for exactly this reason -- the fabric       \
	 * verifier CRCs a slot from inside soc.c's bring-up and Zephyr routes             \
	 * crc32_ieee() to this driver the moment it is present. (EARLY would be the        \
	 * natural level, but it is not legal for a device, and the priority is pasted     \
	 * into the section name, so it has to be a plain token.) */                       \
	DEVICE_DT_INST_DEFINE(inst, crc_agm_init, NULL, &crc_agm_data_##inst,               \
			      &crc_agm_config_##inst, PRE_KERNEL_1, 0, &crc_agm_api);

DT_INST_FOREACH_STATUS_OKAY(CRC_AGM_DEFINE)
