/*
 * Copyright (c) 2026 AgRV Contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * AgRV2K on-die flash driver: erase and program through the controller at
 * 0x4000_1000. The array is XIP-mapped, so reads are plain memory accesses
 * (and this driver stays free of any read path beyond memcpy).
 *
 * Register layout and sequences come from the vendor SDK
 * (framework-agrv_sdk/src/flash.{c,h}):
 *
 *   unlock        KEYR <- 0x45670123, then 0xCDEF89AB   (when CR.LOCK)
 *   erase sector  AR <- addr; CR.SER; start; wait SR.BSY; CR erase bits <- 0
 *   program word  CR.PG; *(uint32_t *)addr = data; wait SR.BSY; CR.PG <- 0
 *   start         wait SCLK_DIV_HIGH * 256 cycles, then CR.STRT
 *
 * The start delay is the SDK's FLASH_Start() comment verbatim ("make sure
 * FLASH_CR_STRT is set when flash is not active"): setting STRT while the
 * array is still busy with the previous access drops the bit. That is why
 * the routine is cycle-counted rather than a plain register write.
 */

#define DT_DRV_COMPAT agm_agrv2k_flash_controller

#include <zephyr/device.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <agm_sys.h>

#include <string.h>

LOG_MODULE_REGISTER(flash_agm, CONFIG_FLASH_LOG_LEVEL);

/* Controller registers (offsets from the node's reg). */
#define FLASH_AGM_KEYR       0x04U
#define FLASH_AGM_SR         0x0cU
#define FLASH_AGM_CR         0x10U
#define FLASH_AGM_AR         0x14U
#define FLASH_AGM_WRPR       0x20U

#define FLASH_AGM_SR_BSY     BIT(0)
#define FLASH_AGM_SR_PGERR   BIT(2)
#define FLASH_AGM_SR_WRPRTERR BIT(4)
#define FLASH_AGM_SR_EOP     BIT(5)
#define FLASH_AGM_SR_ERR     (FLASH_AGM_SR_PGERR | FLASH_AGM_SR_WRPRTERR)

#define FLASH_AGM_CR_PG      BIT(0)
#define FLASH_AGM_CR_SER     BIT(1)
#define FLASH_AGM_CR_MER     BIT(2)
#define FLASH_AGM_CR_BER     BIT(3)
#define FLASH_AGM_CR_OPTPG   BIT(4)
#define FLASH_AGM_CR_OPTER   BIT(5)
#define FLASH_AGM_CR_STRT    BIT(6)
#define FLASH_AGM_CR_LOCK    BIT(7)
#define FLASH_AGM_CR_ERASE_MASK (FLASH_AGM_CR_SER | FLASH_AGM_CR_MER | \
				 FLASH_AGM_CR_BER | FLASH_AGM_CR_OPTER)

#define FLASH_AGM_KEY1       0x45670123U
#define FLASH_AGM_KEY2       0xCDEF89ABU

/* SYS.CLK_CNTL SCLK_DIV_HIGH: the flash SPI clock divider the SDK uses to
 * size the STRT delay (system.h SYS_GetSclkDividerHigh()). The register
 * address lives with the rest of the SYS map. */
#define FLASH_AGM_SYS_CLK_CNTL       AGM_SYS_CLK_CNTL_ADDR
#define FLASH_AGM_SCLK_DIV_HIGH_MASK 0xF000U
#define FLASH_AGM_SCLK_DIV_HIGH_SHIFT 12

struct flash_agm_config {
	mm_reg_t base;
	uint8_t *flash_base;
	size_t size;
	size_t erase_block;
#if defined(CONFIG_FLASH_PAGE_LAYOUT)
	/* Per-instance page layout, filled from the same DT properties. */
	struct flash_pages_layout layout;
#endif
};

static inline uint32_t agm_rd(const struct flash_agm_config *cfg, uint32_t off)
{
	return sys_read32(cfg->base + off);
}

static inline void agm_wr(const struct flash_agm_config *cfg, uint32_t off,
			  uint32_t val)
{
	sys_write32(val, cfg->base + off);
}

/* Bounded on purpose. A controller that never clears BSY used to spin here for
 * ever, and in a bootloader image the only thing running is that loop: the
 * console stops answering, SWD may be unusable, and the board looks dead
 * without a single line of output (the shape of that bitstream incident).
 * The bound is a stuck-controller detector, not a timing
 * requirement: a real sector erase is tens of ms, so this is ~1 s at 200 MHz
 * (~50M reads) -- far beyond anything legitimate. */
#define FLASH_AGM_BUSY_SPINS 50000000U

static int agm_wait_busy(const struct flash_agm_config *cfg)
{
	for (uint32_t spin = 0U; spin < FLASH_AGM_BUSY_SPINS; spin++) {
		if ((agm_rd(cfg, FLASH_AGM_SR) & FLASH_AGM_SR_BSY) == 0U) {
			return 0;
		}
	}
	LOG_ERR("flash stayed busy (SR=0x%08x, WRPR=0x%08x)", agm_rd(cfg, FLASH_AGM_SR),
		agm_rd(cfg, FLASH_AGM_WRPR));
	return -ETIMEDOUT;
}

static int agm_unlock(const struct flash_agm_config *cfg)
{
	if ((agm_rd(cfg, FLASH_AGM_CR) & FLASH_AGM_CR_LOCK) != 0U) {
		agm_wr(cfg, FLASH_AGM_KEYR, FLASH_AGM_KEY1);
		agm_wr(cfg, FLASH_AGM_KEYR, FLASH_AGM_KEY2);
		if ((agm_rd(cfg, FLASH_AGM_CR) & FLASH_AGM_CR_LOCK) != 0U) {
			LOG_ERR("flash stayed locked");
			return -EIO;
		}
	}
	return 0;
}

static void agm_lock(const struct flash_agm_config *cfg)
{
	agm_wr(cfg, FLASH_AGM_CR, agm_rd(cfg, FLASH_AGM_CR) | FLASH_AGM_CR_LOCK);
}

/* SR's error bits and EOP are write-1-to-clear and sticky: leaving PGERR set
 * makes every later operation look failed (a deliberate "program over a
 * written word" test poisoned the erase that followed it, SR=0x24 in both).
 * The vendor SDK clears them explicitly
 * (FLASH_ClearProgramError/FLASH_ClearEndOfOperation). */
static void agm_clear_flags(const struct flash_agm_config *cfg)
{
	agm_wr(cfg, FLASH_AGM_SR, FLASH_AGM_SR_ERR | FLASH_AGM_SR_EOP);
}

/* The SDK's FLASH_Start(): wait SCLK_DIV_HIGH * 256 cycles, then set STRT.
 * The SDK counts mcycle; a nop loop is used here so the driver stays free of
 * CSR plumbing -- each iteration is at least one cycle, so the wait is a
 * little longer than required, which is harmless. */
static void agm_start(const struct flash_agm_config *cfg)
{
	uint32_t div = (sys_read32(FLASH_AGM_SYS_CLK_CNTL) &
			FLASH_AGM_SCLK_DIV_HIGH_MASK) >>
		       FLASH_AGM_SCLK_DIV_HIGH_SHIFT;

	for (volatile uint32_t i = 0; i < div * 256U; i++) {
		arch_nop();
	}

	agm_wr(cfg, FLASH_AGM_CR, agm_rd(cfg, FLASH_AGM_CR) | FLASH_AGM_CR_STRT);
}

/* Sector erase with the controller already unlocked (the caller unlocks once
 * per flash_agm_erase() call: unlocking per sector made a 100 KiB bitstream
 * update need thousands of key writes). */
static int agm_erase_sector(const struct flash_agm_config *cfg, uint32_t addr)
{
	uint32_t sr;
	int ret;

	agm_clear_flags(cfg);
	agm_wr(cfg, FLASH_AGM_AR, addr);
	agm_wr(cfg, FLASH_AGM_CR,
	       (agm_rd(cfg, FLASH_AGM_CR) & ~FLASH_AGM_CR_ERASE_MASK) |
		       FLASH_AGM_CR_SER);
	agm_start(cfg);
	ret = agm_wait_busy(cfg);
	agm_wr(cfg, FLASH_AGM_CR,
	       agm_rd(cfg, FLASH_AGM_CR) & ~FLASH_AGM_CR_ERASE_MASK);
	if (ret < 0) {
		return ret;
	}

	sr = agm_rd(cfg, FLASH_AGM_SR);

	if ((sr & FLASH_AGM_SR_ERR) != 0U) {
		LOG_ERR("erase @0x%08x failed (SR=0x%08x, WRPR=0x%08x)", addr, sr,
			agm_rd(cfg, FLASH_AGM_WRPR));
		return -EIO;
	}
	return 0;
}

static int agm_program_word(const struct flash_agm_config *cfg, uint32_t addr,
			    uint32_t data)
{
	uint32_t sr;
	int ret;

	agm_clear_flags(cfg);
	agm_wr(cfg, FLASH_AGM_CR, agm_rd(cfg, FLASH_AGM_CR) | FLASH_AGM_CR_PG);
	*(volatile uint32_t *)addr = data;
	ret = agm_wait_busy(cfg);
	agm_wr(cfg, FLASH_AGM_CR, agm_rd(cfg, FLASH_AGM_CR) & ~FLASH_AGM_CR_PG);
	if (ret < 0) {
		return ret;
	}

	sr = agm_rd(cfg, FLASH_AGM_SR);

	if ((sr & FLASH_AGM_SR_ERR) != 0U) {
		LOG_ERR("program @0x%08x failed (SR=0x%08x, WRPR=0x%08x)", addr, sr,
			agm_rd(cfg, FLASH_AGM_WRPR));
		return -EIO;
	}
	return 0;
}

/* ---- Zephyr flash API ---------------------------------------------- */

static int flash_agm_read(const struct device *dev, off_t offset, void *data,
			  size_t len)
{
	const struct flash_agm_config *cfg = dev->config;

	if (offset < 0 || (size_t)offset + len > cfg->size) {
		return -EINVAL;
	}
	if (len == 0U) {
		return 0;
	}

	memcpy(data, cfg->flash_base + offset, len);
	return 0;
}

static int flash_agm_write(const struct device *dev, off_t offset,
			   const void *data, size_t len)
{
	const struct flash_agm_config *cfg = dev->config;
	const uint8_t *p = data;
	int ret;

	if (offset < 0 || (size_t)offset + len > cfg->size) {
		return -EINVAL;
	}
	if (len % 4U != 0U || offset % 4U != 0U) {
		/* Programming is 32-bit (SDK FLASH_ProgramWord). */
		return -EINVAL;
	}

	ret = agm_unlock(cfg);
	if (ret != 0) {
		return ret;
	}

	for (size_t done = 0; done < len; done += 4U) {
		uint32_t word;

		memcpy(&word, p + done, sizeof(word));
		ret = agm_program_word(cfg, (uint32_t)(cfg->flash_base + offset + done),
				       word);
		if (ret != 0) {
			agm_lock(cfg);
			return ret;
		}
	}
	agm_lock(cfg);
	return 0;
}

static int flash_agm_erase(const struct device *dev, off_t offset, size_t len)
{
	const struct flash_agm_config *cfg = dev->config;
	int ret;

	if (offset < 0 || (size_t)offset + len > cfg->size) {
		return -EINVAL;
	}
	if (offset % cfg->erase_block != 0U || len % cfg->erase_block != 0U) {
		return -EINVAL;
	}

	ret = agm_unlock(cfg);
	if (ret != 0) {
		return ret;
	}

	for (size_t done = 0; done < len; done += cfg->erase_block) {
		ret = agm_erase_sector(cfg,
				       (uint32_t)(cfg->flash_base + offset + done));

		if (ret != 0) {
			agm_lock(cfg);
			return ret;
		}
	}
	agm_lock(cfg);
	return 0;
}

static const struct flash_parameters *
flash_agm_get_parameters(const struct device *dev)
{
	ARG_UNUSED(dev);

	static const struct flash_parameters params = {
		.write_block_size = 4,
		.erase_value = 0xff,
	};

	return &params;
}

#if defined(CONFIG_FLASH_PAGE_LAYOUT)
/* The layout is part of the (const) per-device config, built by the compiler
 * from the same DT properties the rest of it uses. It used to be a single
 * mutable `static` that every instance overwrote, so a caller holding the
 * pointer across two devices -- or across two calls -- could see the other
 * device's numbers. */
static void flash_agm_pages_layout(const struct device *dev,
				   const struct flash_pages_layout **layout,
				   size_t *layout_size)
{
	const struct flash_agm_config *cfg = dev->config;

	*layout = &cfg->layout;
	*layout_size = 1;
}
#endif /* CONFIG_FLASH_PAGE_LAYOUT */

static DEVICE_API(flash, flash_agm_api) = {
	.read = flash_agm_read,
	.write = flash_agm_write,
	.erase = flash_agm_erase,
	.get_parameters = flash_agm_get_parameters,
#if defined(CONFIG_FLASH_PAGE_LAYOUT)
	.page_layout = flash_agm_pages_layout,
#endif
};

static int flash_agm_init(const struct device *dev)
{
	const struct flash_agm_config *cfg = dev->config;

	if (cfg->size == 0U || cfg->erase_block == 0U) {
		return -EINVAL;
	}

	/* Clear any error flags a previous session left behind. */
	agm_clear_flags(cfg);
	agm_lock(cfg);

	LOG_INF("%s: %u KiB, erase 0x%x, write 4, WRPR=0x%08x",
		dev->name, (uint32_t)(cfg->size / 1024U), (uint32_t)cfg->erase_block,
		agm_rd(cfg, FLASH_AGM_WRPR));
	return 0;
}

#define FLASH_AGM_DEVICE(child)                                                \
	static const struct flash_agm_config flash_agm_config_##child = {      \
		.base = DT_REG_ADDR(DT_PARENT(child)),                         \
		.flash_base = (uint8_t *)DT_REG_ADDR(child),                   \
		.size = DT_REG_SIZE(child),                                    \
		.erase_block = DT_PROP(child, erase_block_size),               \
		COND_CODE_1(CONFIG_FLASH_PAGE_LAYOUT,                          \
			(.layout = {                                           \
				.pages_count = DT_REG_SIZE(child) /            \
					       DT_PROP(child, erase_block_size),\
				.pages_size = DT_PROP(child, erase_block_size),\
			},), ())                                               \
	};                                                                     \
	DEVICE_DT_DEFINE(child, flash_agm_init, NULL, NULL,                    \
			 &flash_agm_config_##child, POST_KERNEL,               \
			 CONFIG_FLASH_INIT_PRIORITY, &flash_agm_api)

#define FLASH_AGM_INST(inst)                                                   \
	DT_FOREACH_CHILD_STATUS_OKAY(DT_DRV_INST(inst), FLASH_AGM_DEVICE)

DT_INST_FOREACH_STATUS_OKAY(FLASH_AGM_INST)
