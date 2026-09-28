/*
 * Copyright (c) 2026 AgRV Contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * AgRV2K MAC0 MDIO bus driver.
 *
 * AgRV2K has a hardware MDIO controller inside the on-die MAC0
 * peripheral at 0x41040000, register offset 0x10. Same register that
 * the SDK's framework-agrv_lwip / framework-agrv_sdk MAC_Init() flow
 * uses; this driver is just the Zephyr MDIO bus glue around it.
 *
 * Hardware bits (AltaRiscv.h: AHB_MASK_MAC0, MAC_TypeDef.MDIO):
 *   MAC_MDIO_WRITE          (1 << 0)   trigger a write
 *   MAC_MDIO_READ           (1 << 1)   trigger a read
 *   MAC_MDIO_LINK_FAIL      (1 << 2)   ro: PHY lost MDC ack
 *   MAC_MDIO_BUSY           (1 << 3)   ro: transaction in flight
 *   MAC_MDIO_MDCSC_OFFSET   4          MDC scaler 2 bits (00..11)
 *   MAC_MDIO_REGADDR_OFFSET 6          reg addr (5 bits)
 *   MAC_MDIO_PHYADDR_OFFSET 11         PHY addr (5 bits)
 *   MAC_MDIO_DATA_OFFSET    16         16-bit data payload
 *
 * The driver only needs the AFSEL on the two MAC pins (MAC0_MDC,
 * MAC0_MDIO). PHY address selection is per-call (the standard Zephyr
 * MDIO API is addressed by prtad). No C45 -- the LAN8720 is a C22
 * PHY, and the AgRV2K IP doesn't expose C45 either.
 *
 * The MAC0 AHB clock gate is opened by soc.c via the DT-defined
 * `agm,ahb-clkenable-bit` on the eth0 node; the AFSEL on the pins
 * is set by pinctrl_apply_state() from the eth0_default pinctrl
 * state. This driver assumes both have already happened by the time
 * it runs (POST_KERNEL, after the net stack + pinctrl).
 */

#define DT_DRV_COMPAT agm_agrv2k_mdio

#include <zephyr/device.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/drivers/mdio.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(mdio_agm, CONFIG_MDIO_LOG_LEVEL);

/* Mirror the mac.h register shape. Kept locally so the driver does
 * not have to consume the in-tree SDK headers -- they're not in
 * the module include path.
 *
 * Every field is volatile: these are memory-mapped registers living in
 * the fabric and the hardware changes BUSY / DATA behind the CPU's
 * back. Without volatile the compiler may collapse or reorder the
 * poll loads, and it did so differently in two otherwise identical
 * builds: one read the PHY ID correctly, the other read 0x00000000 on
 * every boot. The debugger read the same address correctly while the
 * driver saw zeroes, which is what identified it as a CPU-side access
 * problem rather than a dead PHY (dev board). */
struct mdio_regs {
	volatile uint32_t ctrl;
	volatile uint32_t stat;
	volatile uint32_t macmsb;
	volatile uint32_t maclsb;
	volatile uint32_t mdio;
};

#define MAC_MDIO_BUSY             BIT(3)
#define MAC_MDIO_REGADDR_OFFSET   6
#define MAC_MDIO_PHYADDR_OFFSET   11
#define MAC_MDIO_DATA_OFFSET      16
/* MAC_MDIO_MDCSC_OFFSET = 4, value 0 = div 128 (slowest).
 * For the 100 MHz eval bitstream HCLK = 100 MHz, MDC ~= 780 kHz,
 * well below the LAN8720 max of 25 MHz. */
#define MAC_MDIO_MDCSC_128        (0U << 4)

#define MDIO_TIMEOUT_MS 100U

struct mdio_agm_config {
	struct mdio_regs *const base;
};

/*
 * Register access, through two weak functions.
 *
 * The MDIO register carries the command *and* the PHY's answer in one word, so
 * a test that backs it with plain RAM can never model a PHY: the driver's read
 * returns the command it just wrote, and mdio_read() answers 0 forever (the
 * same boundary I2C's CR/SR register has). Overriding these two
 * lets a suite put a clause-22 PHY behind the bus -- see
 * tests/drivers/common/agm_native/agm_fake_mdio_phy.c and the two suites that
 * use it. The defaults below are the plain MMIO accesses, so a build that
 * overrides neither behaves exactly as before.
 */
__weak uint32_t mdio_agm_reg_read(const struct device *dev)
{
	const struct mdio_agm_config *const cfg = dev->config;

	return cfg->base->mdio;
}

__weak void mdio_agm_reg_write(const struct device *dev, uint32_t val)
{
	const struct mdio_agm_config *const cfg = dev->config;

	cfg->base->mdio = val;
}

/* Wait for BUSY to clear. Returns -ETIMEDOUT if the controller never finishes
 * (a PHY holding the bus, or a board whose MDIO pins are not routed): the old
 * version returned silently and the caller then read stale DATA, which looked
 * like a PHY that answers wrong rather than a bus that hangs. */
static int mdio_wait_busy(const struct device *dev)
{
	uint32_t loops = MDIO_TIMEOUT_MS * 1000U;

	while ((mdio_agm_reg_read(dev) & MAC_MDIO_BUSY) != 0U) {
		/* 1 us granularity at 100 MHz */
		k_busy_wait(1);
		if (--loops == 0U) {
			return -ETIMEDOUT;
		}
	}
	return 0;
}

static int mdio_agm_read(const struct device *dev, uint8_t prtad, uint8_t regad,
			 uint16_t *data)
{
	uint32_t cmd;
	int ret;

	if (data == NULL) {
		return -EINVAL;
	}

	cmd = ((uint32_t)prtad << MAC_MDIO_PHYADDR_OFFSET)
	    | ((uint32_t)regad << MAC_MDIO_REGADDR_OFFSET)
	    | MAC_MDIO_MDCSC_128
	    | BIT(1);  /* MAC_MDIO_READ */

	ret = mdio_wait_busy(dev);
	if (ret != 0) {
		return ret;
	}
	mdio_agm_reg_write(dev, cmd);
	ret = mdio_wait_busy(dev);
	if (ret != 0) {
		return ret;
	}

	*data = (uint16_t)((mdio_agm_reg_read(dev) >> MAC_MDIO_DATA_OFFSET) & 0xFFFFU);
	return 0;
}

static int mdio_agm_write(const struct device *dev, uint8_t prtad, uint8_t regad,
			  uint16_t data)
{
	uint32_t cmd;
	int ret;

	cmd = ((uint32_t)data << MAC_MDIO_DATA_OFFSET)
	    | ((uint32_t)prtad << MAC_MDIO_PHYADDR_OFFSET)
	    | ((uint32_t)regad << MAC_MDIO_REGADDR_OFFSET)
	    | MAC_MDIO_MDCSC_128
	    | BIT(0);  /* MAC_MDIO_WRITE */

	ret = mdio_wait_busy(dev);
	if (ret != 0) {
		return ret;
	}
	mdio_agm_reg_write(dev, cmd);
	ret = mdio_wait_busy(dev);
	if (ret != 0) {
		return ret;
	}
	return 0;
}

static DEVICE_API(mdio, mdio_agm_driver_api) = {
	.read = mdio_agm_read,
	.write = mdio_agm_write,
};

static int mdio_agm_init(const struct device *dev)
{
	/* Drain any sticky state from boot -- the controller may have
	 * started a transaction that hasn't completed. A single read
	 * is enough to make sure BUSY is 0 before any driver calls. */
	(void)mdio_agm_reg_read(dev);
	return 0;
}

#define MDIO_AGM_CONFIG(inst)                                                                   \
	static const struct mdio_agm_config mdio_agm_cfg_##inst = {                             \
		/* MDIO is a child of the eth0 node (see agm,agrv2k-mdio.yaml); */             \
		/* share the parent MAC's reg -- no MDIO reg of its own. */                     \
		.base = (struct mdio_regs *)DT_REG_ADDR(DT_INST_PARENT(inst)),                 \
	}

#define MDIO_AGM_DEVICE(inst)                                                                   \
	MDIO_AGM_CONFIG(inst);                                                                  \
	DEVICE_DT_INST_DEFINE(inst, mdio_agm_init, NULL, NULL, &mdio_agm_cfg_##inst,            \
			      POST_KERNEL, CONFIG_MDIO_INIT_PRIORITY, &mdio_agm_driver_api);

DT_INST_FOREACH_STATUS_OKAY(MDIO_AGM_DEVICE)
