/*
 * Copyright (c) 2026 AgRV Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief AgRV2K CPLD (FPGA fabric) window shell.
 *
 * Host-side tool for a bitstream whose register map is only known from its
 * Verilog: `cpld read/write/dump` reach the fabric through the same
 * bounds-checked API the driver exposes, so a typo prints -EINVAL instead
 * of stalling the CPU on an unanswered AHB transfer.
 */

#include <errno.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/misc/cpld_agm.h>
#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/util.h>

/* CONFIG_CPLD_AGM depends on DT_HAS_AGM_AGRV2K_CPLD_ENABLED, so at least
 * one node is status-okay whenever this file is compiled. */
#define CPLD_SHELL_DEV DEVICE_DT_GET(DT_COMPAT_GET_ANY_STATUS_OKAY(agm_agrv2k_cpld))

#define CPLD_SHELL_DUMP_MAX 64U

static bool parse_u32(const struct shell *sh, const char *arg, uint32_t *out)
{
	int err = 0;
	unsigned long val = shell_strtoul(arg, 0, &err);

	if ((err < 0) || (val > UINT32_MAX)) {
		shell_error(sh, "bad number '%s'", arg);
		return false;
	}

	*out = (uint32_t)val;

	return true;
}

static int cmd_info(const struct shell *sh, size_t argc, char **argv)
{
	const struct device *dev = CPLD_SHELL_DEV;
	uintptr_t base = 0U;
	size_t size = 0U;
	int ret;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	ret = agm_cpld_get_base(dev, &base);
	ret = (ret < 0) ? ret : agm_cpld_get_size(dev, &size);
	if (ret < 0) {
		shell_error(sh, "cannot query %s (%d)", dev->name, ret);
		return ret;
	}

	shell_print(sh, "%s: base 0x%08lx, size 0x%zx", dev->name, (unsigned long)base, size);

	return 0;
}

static int cmd_read(const struct shell *sh, size_t argc, char **argv)
{
	uint32_t offset = 0U;
	uint32_t val = 0U;
	int ret;

	if (!parse_u32(sh, argv[1], &offset)) {
		return -EINVAL;
	}

	ret = agm_cpld_read32(CPLD_SHELL_DEV, offset, &val);
	if (ret < 0) {
		shell_error(sh, "read +0x%x failed (%d)", offset, ret);
		return ret;
	}

	shell_print(sh, "+0x%04x: 0x%08x", offset, val);

	return 0;
}

static int cmd_write(const struct shell *sh, size_t argc, char **argv)
{
	uint32_t offset = 0U;
	uint32_t val = 0U;
	int ret;

	if (!parse_u32(sh, argv[1], &offset) || !parse_u32(sh, argv[2], &val)) {
		return -EINVAL;
	}

	ret = agm_cpld_write32(CPLD_SHELL_DEV, offset, val);
	if (ret < 0) {
		shell_error(sh, "write +0x%x failed (%d)", offset, ret);
		return ret;
	}

	shell_print(sh, "+0x%04x <- 0x%08x", offset, val);

	return 0;
}

static int cmd_dump(const struct shell *sh, size_t argc, char **argv)
{
	uint32_t offset = 0U;
	uint32_t count = 8U;

	if ((argc > 1) && !parse_u32(sh, argv[1], &offset)) {
		return -EINVAL;
	}

	if ((argc > 2) && !parse_u32(sh, argv[2], &count)) {
		return -EINVAL;
	}

	count = MIN(count, CPLD_SHELL_DUMP_MAX);

	for (uint32_t i = 0U; i < count; i++) {
		uint32_t val = 0U;
		int ret = agm_cpld_read32(CPLD_SHELL_DEV, offset + i * sizeof(uint32_t), &val);

		if (ret < 0) {
			shell_error(sh, "+0x%04x: error %d", offset + i * sizeof(uint32_t), ret);
			return ret;
		}

		shell_print(sh, "+0x%04x: 0x%08x", offset + i * sizeof(uint32_t), val);
	}

	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(cpld_cmds,
	SHELL_CMD(info, NULL, "Print the CPLD window base address and size", cmd_info),
	SHELL_CMD_ARG(read, NULL, "Read a 32-bit register: cpld read <offset>", cmd_read, 2, 0),
	SHELL_CMD_ARG(write, NULL, "Write a 32-bit register: cpld write <offset> <value>",
		      cmd_write, 3, 0),
	SHELL_CMD_ARG(dump, NULL, "Dump registers: cpld dump [<offset>] [<count>]", cmd_dump, 1, 2),
	SHELL_SUBCMD_SET_END
);

SHELL_CMD_REGISTER(cpld, &cpld_cmds, "AgRV2K CPLD (FPGA fabric) window commands", NULL);
