/*
 * Copyright (c) 2026 AgRV Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * AGM AgRV2K I2C controller driver.
 *
 * The controller is an OpenCores I2C master core:
 *   PRERLO 0x00 / PRERHI 0x04  — clock prescaler (SCL = pclk/(5*(pre+1)))
 *   CTR    0x08                — EN(7) core enable, INTEN(6)
 *   TXR/RXR 0x0c               — transmit / receive byte
 *   CR/SR  0x10                — command (write) / status (read)
 *
 * The driver is polling-only: it waits on the TIP (transfer in
 * progress) status bit after every command, so the PLIC interrupt
 * (I2C0 = IRQ 30, I2C1 = IRQ 31) is not used.
 *
 * SoC glue is delegated, not open-coded: the devicetree pinctrl state
 * (i2c0_default / i2c1_default) carries the SCL/SDA routes as
 * AGM_PINCTRL() cells -- I2C0 on GPIO3 bit 4/5, I2C1 on bit 6/7, with
 * AGM_PINCTRL_NO_DIR because the open-drain pins' direction belongs to the
 * IP. init() applies that state through pinctrl_apply_state(), and soc.c
 * opens the APB gates (I2Cx + the pins' bank) at PRE_KERNEL_1 from the same
 * state plus the node's agm,apb-clkenable-bit. The per-instance
 * base-address table that used to live here is gone.
 */

#define DT_DRV_COMPAT agm_agrv2k_i2c

#include <errno.h>
#include <zephyr/device.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/sys/util.h>

#include <agm_sys.h>

LOG_MODULE_REGISTER(i2c_agm, CONFIG_I2C_LOG_LEVEL);

/* OpenCores I2C register offsets / bit definitions. */
#define AGM_I2C_PRERLO	0x00U
#define AGM_I2C_PRERHI	0x04U
#define AGM_I2C_CTR	0x08U
#define AGM_I2C_TXR	0x0cU
#define AGM_I2C_RXR	0x0cU
#define AGM_I2C_CR	0x10U
#define AGM_I2C_SR	0x10U

#define AGM_I2C_CTR_EN		BIT(7)
#define AGM_I2C_CTR_INTEN	BIT(6)

#define AGM_I2C_CR_IACK	BIT(0)
#define AGM_I2C_CR_NACK	BIT(3)
#define AGM_I2C_CR_WR	BIT(4)
#define AGM_I2C_CR_RD	BIT(5)
#define AGM_I2C_CR_STO	BIT(6)
#define AGM_I2C_CR_STA	BIT(7)

#define AGM_I2C_SR_IF	BIT(0)
#define AGM_I2C_SR_TIP	BIT(1)
#define AGM_I2C_SR_AL	BIT(5)
#define AGM_I2C_SR_BUSY	BIT(6)
#define AGM_I2C_SR_RXACK BIT(7)

struct i2c_agm_cfg {
	uint32_t base;
	uint32_t default_config;
	uint32_t pclk_hz;
	const struct pinctrl_dev_config *pincfg;
};

struct i2c_agm_data {
	uint32_t dev_config;
};

/*
 * Register access, through two weak functions.
 *
 * This controller puts the command (CR) and the status (SR) on the *same*
 * offset (0x10), so a test that backs the window with plain RAM cannot model
 * it: the driver's status read returns the command it just wrote, CR.STA (0x80)
 * lands in SR.RXACK (0x80) and every address phase looks unacknowledged -- a
 * write that should succeed returns -ENXIO. Overriding these
 * two lets a suite answer reads from a small command/status model instead; see
 * tests/drivers/i2c/i2c_agm. The defaults are the plain MMIO accesses, so a
 * build that overrides neither behaves exactly as before.
 *
 * `ctx` is the driver's internal config pointer, passed as void * so the test
 * does not have to know the type.
 */
__weak uint32_t i2c_agm_reg_read(const void *ctx, uint32_t off)
{
	const struct i2c_agm_cfg *cfg = ctx;

	return sys_read32(cfg->base + off);
}

__weak void i2c_agm_reg_write(const void *ctx, uint32_t off, uint32_t val)
{
	const struct i2c_agm_cfg *cfg = ctx;

	sys_write32(val, cfg->base + off);
}

/* Wait until the previous command finished (TIP clear). */
static int i2c_agm_wait_idle(const struct i2c_agm_cfg *cfg)
{
	uint32_t tries = 1000000U;

	while (tries-- > 0U) {
		if ((i2c_agm_reg_read(cfg, AGM_I2C_SR) & AGM_I2C_SR_TIP) == 0U) {
			return 0;
		}
	}
	return -ETIMEDOUT;
}

/* Issue a command (CR write) and wait for it to finish. */
static int i2c_agm_cmd(const struct i2c_agm_cfg *cfg, uint32_t cr)
{
	i2c_agm_reg_write(cfg, AGM_I2C_CR, cr);
	return i2c_agm_wait_idle(cfg);
}

/* Send STOP and leave the bus idle. */
static int i2c_agm_stop(const struct i2c_agm_cfg *cfg)
{
	int ret = i2c_agm_cmd(cfg, AGM_I2C_CR_STO);

	if (ret == 0) {
		/* Give the bus a moment to release (BUSY self-clears). */
		uint32_t tries = 1000000U;

		while (tries-- > 0U) {
			if ((i2c_agm_reg_read(cfg, AGM_I2C_SR) & AGM_I2C_SR_BUSY) == 0U) {
				break;
			}
		}
	}
	return ret;
}

static int i2c_agm_configure(const struct device *dev, uint32_t dev_config)
{
	const struct i2c_agm_cfg *cfg = dev->config;
	struct i2c_agm_data *data = dev->data;
	uint32_t speed = I2C_SPEED_GET(dev_config);
	uint32_t prescaler;
	int ret;

	if (!(dev_config & I2C_MODE_CONTROLLER)) {
		LOG_ERR("only controller mode is supported");
		return -ENOTSUP;
	}
	if (dev_config & I2C_ADDR_10_BITS) {
		LOG_ERR("10-bit addressing is not supported");
		return -ENOTSUP;
	}

	switch (speed) {
	case I2C_SPEED_STANDARD:
		prescaler = cfg->pclk_hz / (5U * 100000U) - 1U;
		break;
	case I2C_SPEED_FAST:
		prescaler = cfg->pclk_hz / (5U * 400000U) - 1U;
		break;
	case I2C_SPEED_FAST_PLUS:
		prescaler = cfg->pclk_hz / (5U * 1000000U) - 1U;
		break;
	default:
		LOG_ERR("unsupported bus speed 0x%x", speed);
		return -ENOTSUP;
	}

	/* Disable the core while reprogramming the prescaler. */
	i2c_agm_reg_write(cfg, AGM_I2C_CTR, 0U);
	i2c_agm_reg_write(cfg, AGM_I2C_PRERLO, prescaler & 0xffU);
	i2c_agm_reg_write(cfg, AGM_I2C_PRERHI, (prescaler >> 8) & 0xffU);
	i2c_agm_reg_write(cfg, AGM_I2C_CTR, AGM_I2C_CTR_EN);

	data->dev_config = dev_config;

	ret = i2c_agm_wait_idle(cfg);
	if (ret != 0) {
		return ret;
	}
	LOG_DBG("configured: speed=0x%x prescaler=%u", speed, prescaler);
	return 0;
}

static int i2c_agm_get_config(const struct device *dev, uint32_t *dev_config)
{
	struct i2c_agm_data *data = dev->data;

	*dev_config = data->dev_config;
	return 0;
}

/*
 * Send the address byte (START / repeated START). The R/W bit is
 * embedded in the byte, exactly like the AgRV SDK does.
 */
static int i2c_agm_address(const struct i2c_agm_cfg *cfg, uint16_t addr,
			   bool read)
{
	int ret;

	i2c_agm_reg_write(cfg, AGM_I2C_TXR, ((addr & 0x7fU) << 1) | (read ? 1U : 0U));
	ret = i2c_agm_cmd(cfg, AGM_I2C_CR_WR | AGM_I2C_CR_STA);
	if (ret != 0) {
		return ret;
	}

	if (i2c_agm_reg_read(cfg, AGM_I2C_SR) & AGM_I2C_SR_RXACK) {
		/* No device ACKed the address. */
		i2c_agm_stop(cfg);
		return -ENXIO;
	}
	return 0;
}

static int i2c_agm_transfer_msg(const struct i2c_agm_cfg *cfg,
				struct i2c_msg *msg, uint16_t addr, bool first)
{
	/* Address phase: the first message always gets one, and a later message
	 * gets one only when it carries I2C_MSG_RESTART. That is Zephyr's contract
	 * ("RESTART: do not send a stop before this message", i.e. continue with a
	 * repeated start), so a *chained* message has to be arranged by the caller:
	 * either the previous one must not carry I2C_MSG_STOP, or this one must set
	 * RESTART -- otherwise the driver, correctly, sends no address at all. */
	bool start = (msg->flags & I2C_MSG_RESTART) != 0 || first;
	bool stop = (msg->flags & I2C_MSG_STOP) != 0;
	int ret;

	if (start) {
		ret = i2c_agm_address(cfg, addr, !!(msg->flags & I2C_MSG_READ));
		if (ret != 0) {
			return ret;
		}
	}

	if (msg->flags & I2C_MSG_READ) {
		uint32_t i;

		for (i = 0; i < msg->len; i++) {
			bool last = (i == msg->len - 1U);
			uint32_t cr = AGM_I2C_CR_RD;

			if (last) {
				cr |= AGM_I2C_CR_NACK; /* NACK the final byte */
				if (stop) {
					cr |= AGM_I2C_CR_STO;
				}
			}
			ret = i2c_agm_cmd(cfg, cr);
			if (ret != 0) {
				i2c_agm_stop(cfg);
				return ret;
			}
			msg->buf[i] = (uint8_t)i2c_agm_reg_read(cfg, AGM_I2C_RXR);
		}
	} else {
		uint32_t i;

		for (i = 0; i < msg->len; i++) {
			bool last = (i == msg->len - 1U);
			uint32_t cr = AGM_I2C_CR_WR;

			if (last && stop) {
				cr |= AGM_I2C_CR_STO;
			}
			i2c_agm_reg_write(cfg, AGM_I2C_TXR, msg->buf[i]);
			ret = i2c_agm_cmd(cfg, cr);
			if (ret != 0) {
				i2c_agm_stop(cfg);
				return ret;
			}
			if (i2c_agm_reg_read(cfg, AGM_I2C_SR) & AGM_I2C_SR_RXACK) {
				i2c_agm_stop(cfg);
				return -EIO;
			}
		}
	}
	return 0;
}

static int i2c_agm_transfer(const struct device *dev, struct i2c_msg *msgs,
			    uint8_t num_msgs, uint16_t addr)
{
	const struct i2c_agm_cfg *cfg = dev->config;
	uint32_t i;
	int ret;

	if (num_msgs == 0U) {
		return -EINVAL;
	}

	/* Wait for the bus to become free before starting. */
	uint32_t tries = 1000000U;

	while (tries-- > 0U) {
		if ((i2c_agm_reg_read(cfg, AGM_I2C_SR) & AGM_I2C_SR_BUSY) == 0U) {
			break;
		}
	}
	if (tries == 0U) {
		LOG_WRN("bus busy");
		return -EBUSY;
	}

	for (i = 0; i < num_msgs; i++) {
		ret = i2c_agm_transfer_msg(cfg, &msgs[i], addr, i == 0U);
		if (ret != 0) {
			LOG_DBG("msg %u failed: %d", i, ret);
			return ret;
		}
	}
	return 0;
}

static int i2c_agm_recover_bus(const struct device *dev)
{
	const struct i2c_agm_cfg *cfg = dev->config;
	uint32_t tries = 100U;

	/* No GPIO access to SCL/SDA (owned by the AF mux), so recovery is
	 * best-effort: clear any latched interrupt/arbitration state and
	 * emit a STOP in case the master is stuck mid-transaction.
	 */
	while (tries-- > 0U) {
		i2c_agm_reg_write(cfg, AGM_I2C_CR, AGM_I2C_CR_IACK);
		if (i2c_agm_wait_idle(cfg) != 0) {
			continue;
		}
		i2c_agm_reg_write(cfg, AGM_I2C_CR, AGM_I2C_CR_IACK | AGM_I2C_CR_STO);
		if (i2c_agm_wait_idle(cfg) == 0) {
			return 0;
		}
	}
	return -EBUSY;
}

static DEVICE_API(i2c, i2c_agm_api) = {
	.configure = i2c_agm_configure,
	.get_config = i2c_agm_get_config,
	.transfer = i2c_agm_transfer,
	.recover_bus = i2c_agm_recover_bus,
#ifdef CONFIG_I2C_RTIO
	.iodev_submit = i2c_iodev_submit_fallback,
#endif
};

static int i2c_agm_init(const struct device *dev)
{
	const struct i2c_agm_cfg *cfg = dev->config;
	int err;

	/* Route SCL/SDA (AFSEL only -- the IP owns the open-drain pins).
	 * The APB gates were opened by soc.c at PRE_KERNEL_1. */
	err = pinctrl_apply_state(cfg->pincfg, PINCTRL_STATE_DEFAULT);
	if (err != 0) {
		LOG_ERR("pinctrl_apply_state failed: %d", err);
		return err;
	}

	/* Program the default bus speed from DT. */
	return i2c_agm_configure(dev, cfg->default_config);
}

#define I2C_AGM_SPEED_FROM_HZ(_hz) \
	((_hz) >= 1000000U ? I2C_SPEED_FAST_PLUS : \
	 (_hz) >= 400000U ? I2C_SPEED_FAST : I2C_SPEED_STANDARD)

#define I2C_AGM_INIT(n)							\
	PINCTRL_DT_INST_DEFINE(n);					\
	static const struct i2c_agm_cfg i2c_agm_cfg_##n = {		\
		.base = DT_INST_REG_ADDR(n),				\
		.pclk_hz = DT_INST_PROP_BY_PHANDLE(n, clocks, clock_frequency), \
		.default_config = I2C_MODE_CONTROLLER |			\
				  I2C_SPEED_SET(I2C_AGM_SPEED_FROM_HZ(	\
					  DT_INST_PROP(n, clock_frequency))), \
		.pincfg = PINCTRL_DT_INST_DEV_CONFIG_GET(n),		\
	};								\
	static struct i2c_agm_data i2c_agm_data_##n;			\
	I2C_DEVICE_DT_INST_DEFINE(n, i2c_agm_init, NULL,		\
				  &i2c_agm_data_##n,			\
				  &i2c_agm_cfg_##n, POST_KERNEL,	\
				  CONFIG_I2C_INIT_PRIORITY,		\
				  &i2c_agm_api)

DT_INST_FOREACH_STATUS_OKAY(I2C_AGM_INIT)
