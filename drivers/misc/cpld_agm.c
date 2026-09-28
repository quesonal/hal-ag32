/*
 * Copyright (c) 2026 AgRV Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief AgRV2K CPLD (FPGA fabric) window driver.
 *
 * Phase B of the CPLD port: the devicetree node now binds this driver
 * instead of the generic syscon driver, so the window gets an AGM API
 * (`agm_cpld_read32()` etc., include/zephyr/drivers/misc/cpld_agm.h), a
 * shell and a place for bitstream-specific behaviour.
 *
 * Why the node cannot keep both drivers: `DEVICE_DT_INST_DEFINE()` names
 * the device after the devicetree node's dependency ordinal, so a node
 * with two bindings gets two definitions of the same symbol. The node
 * therefore lists `agm,agrv2k-cpld` alone; the in-tree syscon driver is
 * still available for other nodes (sys@0x3000000 keeps `syscon`).
 *
 * The bounds check before every access is not cosmetic: an offset the
 * fabric does not answer leaves the AHB transfer without `hreadyout`, and
 * the CPU stalls there with no fault and no console. Rejecting it at the
 * driver keeps a typo a -EINVAL instead of a hang.
 */

#define DT_DRV_COMPAT agm_agrv2k_cpld

#include <errno.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/misc/cpld_agm.h>
#include <zephyr/kernel.h>
#include <zephyr/spinlock.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/sys/util.h>

struct cpld_agm_config {
	DEVICE_MMIO_ROM;
	size_t size;
};

struct cpld_agm_data {
	DEVICE_MMIO_RAM;
	struct k_spinlock lock;
};

/* The fabric decodes 32-bit AHB beats and answers only inside `reg`
 * (same rule as drivers/syscon/syscon_common.h::syscon_sanitize_reg()). */
static int cpld_agm_check_offset(uint32_t offset, size_t size)
{
	if ((offset % sizeof(uint32_t)) != 0U) {
		return -EINVAL;
	}

	if (offset >= size) {
		return -EINVAL;
	}

	return 0;
}

static int cpld_agm_get_base(const struct device *dev, uintptr_t *base)
{
	if (base == NULL) {
		return -EINVAL;
	}

	*base = DEVICE_MMIO_GET(dev);

	return 0;
}

static int cpld_agm_get_size(const struct device *dev, size_t *size)
{
	const struct cpld_agm_config *config = dev->config;

	if (size == NULL) {
		return -EINVAL;
	}

	*size = config->size;

	return 0;
}

static int cpld_agm_read32_nolock(const struct device *dev, uint32_t offset, uint32_t *val)
{
	const struct cpld_agm_config *config = dev->config;
	int ret;

	ret = cpld_agm_check_offset(offset, config->size);
	if (ret < 0) {
		return ret;
	}

	*val = sys_read32(DEVICE_MMIO_GET(dev) + offset);

	return 0;
}

static int cpld_agm_write32_nolock(const struct device *dev, uint32_t offset, uint32_t val)
{
	const struct cpld_agm_config *config = dev->config;
	int ret;

	ret = cpld_agm_check_offset(offset, config->size);
	if (ret < 0) {
		return ret;
	}

	sys_write32(val, DEVICE_MMIO_GET(dev) + offset);

	return 0;
}

static int cpld_agm_read32(const struct device *dev, uint32_t offset, uint32_t *val)
{
	struct cpld_agm_data *data = dev->data;
	k_spinlock_key_t key;
	int ret;

	if (val == NULL) {
		return -EINVAL;
	}

	key = k_spin_lock(&data->lock);
	ret = cpld_agm_read32_nolock(dev, offset, val);
	k_spin_unlock(&data->lock, key);

	return ret;
}

static int cpld_agm_write32(const struct device *dev, uint32_t offset, uint32_t val)
{
	struct cpld_agm_data *data = dev->data;
	k_spinlock_key_t key;
	int ret;

	key = k_spin_lock(&data->lock);
	ret = cpld_agm_write32_nolock(dev, offset, val);
	k_spin_unlock(&data->lock, key);

	return ret;
}

static int cpld_agm_update_bits(const struct device *dev, uint32_t offset, uint32_t mask,
				uint32_t val)
{
	struct cpld_agm_data *data = dev->data;
	k_spinlock_key_t key;
	uint32_t tmp = 0U;
	int ret;

	key = k_spin_lock(&data->lock);

	ret = cpld_agm_read32_nolock(dev, offset, &tmp);
	if (ret == 0) {
		ret = cpld_agm_write32_nolock(dev, offset, (tmp & ~mask) | (val & mask));
	}

	k_spin_unlock(&data->lock, key);

	return ret;
}

static const struct cpld_agm_driver_api cpld_agm_api = {
	.read32 = cpld_agm_read32,
	.write32 = cpld_agm_write32,
	.update_bits = cpld_agm_update_bits,
	.get_base = cpld_agm_get_base,
	.get_size = cpld_agm_get_size,
};

static int cpld_agm_init(const struct device *dev)
{
	DEVICE_MMIO_MAP(dev, K_MEM_CACHE_NONE);

	return 0;
}

#define CPLD_AGM_INIT(inst)                                                                        \
	static const struct cpld_agm_config cpld_agm_config_##inst = {                             \
		DEVICE_MMIO_ROM_INIT(DT_DRV_INST(inst)),                                           \
		.size = DT_INST_REG_SIZE(inst),                                                    \
	};                                                                                         \
	static struct cpld_agm_data cpld_agm_data_##inst;                                          \
	DEVICE_DT_INST_DEFINE(inst, cpld_agm_init, NULL, &cpld_agm_data_##inst,                    \
			      &cpld_agm_config_##inst, PRE_KERNEL_1, CONFIG_CPLD_AGM_INIT_PRIORITY, \
			      &cpld_agm_api);

DT_INST_FOREACH_STATUS_OKAY(CPLD_AGM_INIT)
