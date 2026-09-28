/*
 * Copyright (c) 2026 AgRV Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief AgRV2K CPLD (FPGA fabric) window driver API.
 *
 * The AgRV2K SoC decoder forwards every MCU load/store in
 * 0x60000000-0x7FFFFFFF to the fabric's `mem_ahb_*` slave port, where the
 * user's Verilog answers (or does not -- see below). This API is the
 * agreed way for board code and other AGM drivers to reach that window:
 * devicetree describes the window (`agm,agrv2k-cpld`), the driver owns the
 * MMIO and the bounds check, and the register map inside the window stays
 * a bitstream property.
 *
 * @warning An access the fabric does not answer never completes: the AHB
 *          `hreadyout` handshake is the only thing that ends the transfer,
 *          so a wrong offset stalls the CPU instead of faulting. That is
 *          why every entry point rejects offsets outside the node's `reg`
 *          before touching the bus.
 */

#ifndef ZEPHYR_INCLUDE_DRIVERS_MISC_CPLD_AGM_H_
#define ZEPHYR_INCLUDE_DRIVERS_MISC_CPLD_AGM_H_

#include <errno.h>
#include <stdint.h>

#include <zephyr/device.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief AgRV2K CPLD window driver API
 */
struct cpld_agm_driver_api {
	/**
	 * @brief Read a 32-bit fabric register
	 *
	 * @param dev CPLD device
	 * @param offset Window-relative byte offset (32-bit aligned)
	 * @param val Read value
	 *
	 * @retval 0 on success
	 * @retval -EINVAL if @p val is NULL, or @p offset is misaligned or
	 *         outside the window
	 */
	int (*read32)(const struct device *dev, uint32_t offset, uint32_t *val);

	/**
	 * @brief Write a 32-bit fabric register
	 *
	 * @param dev CPLD device
	 * @param offset Window-relative byte offset (32-bit aligned)
	 * @param val Value to write
	 *
	 * @retval 0 on success
	 * @retval -EINVAL if @p offset is misaligned or outside the window
	 */
	int (*write32)(const struct device *dev, uint32_t offset, uint32_t val);

	/**
	 * @brief Read-modify-write a 32-bit fabric register
	 *
	 * @param dev CPLD device
	 * @param offset Window-relative byte offset (32-bit aligned)
	 * @param mask Bits to modify
	 * @param val Value to write into the masked bits
	 *
	 * @retval 0 on success
	 * @retval -EINVAL as for read32()
	 */
	int (*update_bits)(const struct device *dev, uint32_t offset, uint32_t mask, uint32_t val);

	/**
	 * @brief Get the window base address
	 *
	 * @param dev CPLD device
	 * @param base Window base address as seen by the CPU
	 *
	 * @retval 0 on success
	 * @retval -EINVAL if @p base is NULL
	 */
	int (*get_base)(const struct device *dev, uintptr_t *base);

	/**
	 * @brief Get the window size
	 *
	 * @param dev CPLD device
	 * @param size Size of the window in bytes
	 *
	 * @retval 0 on success
	 * @retval -EINVAL if @p size is NULL
	 */
	int (*get_size)(const struct device *dev, size_t *size);
};

/**
 * @brief Read a 32-bit fabric register
 *
 * @param dev CPLD device
 * @param offset Window-relative byte offset (32-bit aligned)
 * @param val Read value
 *
 * @retval 0 on success
 * @retval -EINVAL if @p val is NULL, or @p offset is misaligned or outside
 *         the window
 */
static inline int agm_cpld_read32(const struct device *dev, uint32_t offset, uint32_t *val)
{
	const struct cpld_agm_driver_api *api = (const struct cpld_agm_driver_api *)dev->api;

	return api->read32(dev, offset, val);
}

/**
 * @brief Write a 32-bit fabric register
 *
 * @param dev CPLD device
 * @param offset Window-relative byte offset (32-bit aligned)
 * @param val Value to write
 *
 * @retval 0 on success
 * @retval -EINVAL if @p offset is misaligned or outside the window
 */
static inline int agm_cpld_write32(const struct device *dev, uint32_t offset, uint32_t val)
{
	const struct cpld_agm_driver_api *api = (const struct cpld_agm_driver_api *)dev->api;

	return api->write32(dev, offset, val);
}

/**
 * @brief Read-modify-write a 32-bit fabric register
 *
 * @param dev CPLD device
 * @param offset Window-relative byte offset (32-bit aligned)
 * @param mask Bits to modify
 * @param val Value to write into the masked bits
 *
 * @retval 0 on success
 * @retval -EINVAL as for agm_cpld_read32()
 */
static inline int agm_cpld_update_bits(const struct device *dev, uint32_t offset, uint32_t mask,
				       uint32_t val)
{
	const struct cpld_agm_driver_api *api = (const struct cpld_agm_driver_api *)dev->api;

	return api->update_bits(dev, offset, mask, val);
}

/**
 * @brief Get the window base address
 *
 * @param dev CPLD device
 * @param base Window base address as seen by the CPU
 *
 * @retval 0 on success
 * @retval -EINVAL if @p base is NULL
 */
static inline int agm_cpld_get_base(const struct device *dev, uintptr_t *base)
{
	const struct cpld_agm_driver_api *api = (const struct cpld_agm_driver_api *)dev->api;

	return api->get_base(dev, base);
}

/**
 * @brief Get the window size
 *
 * @param dev CPLD device
 * @param size Size of the window in bytes
 *
 * @retval 0 on success
 * @retval -EINVAL if @p size is NULL
 */
static inline int agm_cpld_get_size(const struct device *dev, size_t *size)
{
	const struct cpld_agm_driver_api *api = (const struct cpld_agm_driver_api *)dev->api;

	return api->get_size(dev, size);
}

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_INCLUDE_DRIVERS_MISC_CPLD_AGM_H_ */
