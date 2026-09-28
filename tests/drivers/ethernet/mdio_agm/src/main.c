/*
 * Copyright (c) 2026 AgRV Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief native_sim suite for drivers/ethernet/mdio/mdio_agm.c.
 *
 * The AgRV2K MDIO controller is one 32-bit register inside MAC0
 * (0x41040000 + 0x10) that carries the whole transaction: the command (phy
 * address, register address, MDC scaler, READ/WRITE trigger) and, on a write,
 * the 16-bit payload; hardware reports BUSY while the frame is on the wire and
 * puts the PHY's answer into the same register's DATA field. This suite runs the
 * production driver against that register backed by RAM
 * (tests/drivers/common/agm_native), so what it can and cannot check is worth
 * stating up front:
 *
 *  - It CAN check the command encoding, which is where a wrong shift shows up
 *    as "PHY not found" on the board: PHYADDR=0..4, REGADDR=6..10, MDCSC=4,
 *    READ=bit1 / WRITE=bit0, and the 16-bit data field on a write.
 *  - The read-back data path is modelled through the driver's weak register
 *    hooks: tests/drivers/common/agm_native/
 *    agm_fake_mdio_phy.c is a clause-22 PHY behind the bus, so the suite can
 *    show both halves of the story -- nothing answers (mdio_read() == 0, what
 *    a RAM window gives) and a PHY that does (the selected register comes
 *    back). The helper mirrors every command into the RAM window, which is why
 *    the encoding cases below still read the real register.
 *
 * The BUSY path is covered because it *is* observable: BUSY is a status bit the
 * model sets, the driver polls it with a bounded busy-wait, and the command
 * must still be issued afterwards.
 */

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/mdio.h>
#include <zephyr/kernel.h>
#include <zephyr/net/mii.h>
#include <zephyr/ztest.h>

#include <errno.h>

#include "agm_fake_mdio_phy.h"

#define MDIO_DEV  DEVICE_DT_GET(DT_NODELABEL(mdio0))

/* The MDIO register lives inside MAC0; the driver takes it from the parent
 * node's reg. */
#define MAC_BASE 0x41040000UL
#define R_MDIO   0x10U

#define MDIO_WRITE          BIT(0)
#define MDIO_READ           BIT(1)
#define MDIO_LINK_FAIL      BIT(2)
#define MDIO_BUSY           BIT(3)
#define MDIO_MDCSC_SHIFT    4U
#define MDIO_REGADDR_SHIFT  6U
#define MDIO_PHYADDR_SHIFT  11U
#define MDIO_DATA_SHIFT     16U

static struct agm_fake_mdio_phy phy_model;
/* Set to make the controller look permanently busy (a PHY holding MDIO, or
 * pins that are not routed at all): every read reports BUSY and never
 * clears it, which is the case this hook exists for. */
static bool bus_stuck;

/* The driver's two weak hooks: hand every command to the model (which also
 * mirrors it into the RAM window) and answer reads from the PHY register file. */
uint32_t mdio_agm_reg_read(const struct device *dev)
{
	ARG_UNUSED(dev);

	if (bus_stuck) {
		return MDIO_BUSY;
	}
	return agm_fake_mdio_phy_reg_read(&phy_model);
}

void mdio_agm_reg_write(const struct device *dev, uint32_t val)
{
	ARG_UNUSED(dev);

	agm_fake_mdio_phy_reg_write(&phy_model, val);
}

static uint32_t mdio_reg(void)
{
	return *(volatile uint32_t *)(MAC_BASE + R_MDIO);
}

static void mdio_reg_set(uint32_t val)
{
	*(volatile uint32_t *)(MAC_BASE + R_MDIO) = val;
}

static void *suite_setup(void)
{
	/* No PHY answers and no transaction is in flight. */
	mdio_reg_set(0U);
	agm_fake_mdio_phy_init(&phy_model, (volatile uint32_t *)(MAC_BASE + R_MDIO), 1U);
	phy_model.present = false;
	bus_stuck = false;
	return NULL;
}

ZTEST_SUITE(mdio_agm, NULL, suite_setup, NULL, NULL, NULL);

ZTEST(mdio_agm, test_01_read_encodes_the_clause22_command)
{
	uint16_t data = 0xabcdU;
	uint32_t reg;

	zassert_true(device_is_ready(MDIO_DEV), "the MDIO bus device is up");

	zassert_ok(mdio_read(MDIO_DEV, 1U, 3U, &data), "read transaction issued");

	reg = mdio_reg();
	zassert_equal((reg >> MDIO_PHYADDR_SHIFT) & 0x1fU, 1U, "PHYADDR field (reg=0x%08x)", reg);
	zassert_equal((reg >> MDIO_REGADDR_SHIFT) & 0x1fU, 3U, "REGADDR field");
	zassert_equal((reg >> MDIO_MDCSC_SHIFT) & 0x3U, 0U, "MDC scaler 0 = divide by 128");
	zassert_equal(reg & MDIO_READ, MDIO_READ, "READ trigger set");
	zassert_equal(reg & MDIO_WRITE, 0U, "WRITE trigger clear on a read");
	zassert_equal(reg & MDIO_BUSY, 0U, "no transaction left in flight");

	/* The register the driver read is the one it wrote (see the file header),
	 * so the answer is the command's own DATA field: zero. Pinned so a future
	 * change that *does* model the PHY has to update this case. */
	zassert_equal(data, 0U, "a RAM window answers mdio_read() with 0 (got 0x%04x)", data);
}

ZTEST(mdio_agm, test_02_write_carries_the_16bit_payload)
{
	uint32_t reg;

	zassert_ok(mdio_write(MDIO_DEV, 1U, 0U, 0x1140U), "write transaction issued");

	reg = mdio_reg();
	zassert_equal((reg >> MDIO_DATA_SHIFT) & 0xffffU, 0x1140U, "payload in DATA (reg=0x%08x)", reg);
	zassert_equal((reg >> MDIO_PHYADDR_SHIFT) & 0x1fU, 1U, "PHYADDR field");
	zassert_equal((reg >> MDIO_REGADDR_SHIFT) & 0x1fU, 0U, "REGADDR field (BMCR)");
	zassert_equal(reg & MDIO_WRITE, MDIO_WRITE, "WRITE trigger set");
	zassert_equal(reg & MDIO_READ, 0U, "READ trigger clear on a write");
}

ZTEST(mdio_agm, test_03_phy_and_register_addresses_are_independent_fields)
{
	/* The LAN8720 sits at prtad 1 on this board, but the driver must pass the
	 * caller's address through unchanged -- a fixed address here would break
	 * every PHY that straps differently. */
	zassert_ok(mdio_write(MDIO_DEV, 7U, 0x1fU, 0x0001U), "write to the top of both fields");
	zassert_equal((mdio_reg() >> MDIO_PHYADDR_SHIFT) & 0x1fU, 7U, "PHYADDR 7");
	zassert_equal((mdio_reg() >> MDIO_REGADDR_SHIFT) & 0x1fU, 0x1fU, "REGADDR 31");

	zassert_ok(mdio_write(MDIO_DEV, 0U, 0U, 0U), "write to address 0");
	zassert_equal((mdio_reg() >> MDIO_PHYADDR_SHIFT) & 0x1fU, 0U, "PHYADDR 0");
	zassert_equal((mdio_reg() >> MDIO_REGADDR_SHIFT) & 0x1fU, 0U, "REGADDR 0");
}

ZTEST(mdio_agm, test_04_busy_is_polled_before_the_command_is_issued)
{
	uint16_t data;

	/* A transaction from the boot path is still in flight. The driver has to
	 * wait for BUSY to drop (up to 100 ms) and then issue its own command --
	 * it must not hang and must not skip the write. */
	mdio_reg_set(MDIO_BUSY | 0xdead0000U | MDIO_LINK_FAIL);

	zassert_ok(mdio_read(MDIO_DEV, 1U, 2U, &data), "a stuck BUSY does not fail the call");
	zassert_equal(mdio_reg() & MDIO_BUSY, 0U, "the command replaced the stale transaction");
	zassert_equal((mdio_reg() >> MDIO_REGADDR_SHIFT) & 0x1fU, 2U, "our command went out");
}

ZTEST(mdio_agm, test_05_null_data_is_rejected)
{
	zassert_equal(mdio_read(MDIO_DEV, 1U, 1U, NULL), -EINVAL, "mdio_read() needs somewhere to put the data");
}

ZTEST(mdio_agm, test_06_a_phy_that_answers_returns_its_register)
{
	uint16_t data = 0U;

	/* The PHY at prtad 1 comes alive. Its PHYID1 (reg 2) is the LAN8720 OUI
	 * byte the PHY driver checks first. */
	phy_model.present = true;
	agm_fake_mdio_phy_set_reg(&phy_model, MII_PHYID1R, 0x0007U);

	zassert_ok(mdio_read(MDIO_DEV, 1U, MII_PHYID1R, &data), "read transaction issued");
	zassert_equal(data, 0x0007U, "the PHY's PHYID1 came back (got 0x%04x)", data);
	/* The RAM window keeps the *command* (that is what a RAM window can do:
	 * it stores writes), the answer only exists in the model -- which is
	 * exactly why these hooks are needed. */
	zassert_equal((mdio_reg() >> MDIO_REGADDR_SHIFT) & 0x1fU, MII_PHYID1R,
		      "the window holds the read command");
	zassert_equal(mdio_reg() & MDIO_READ, MDIO_READ, "which was a read");

	/* A different register of the same PHY, and a different PHY address. */
	agm_fake_mdio_phy_set_reg(&phy_model, MII_BMSR, 0x786dU);
	zassert_ok(mdio_read(MDIO_DEV, 1U, MII_BMSR, &data), "BMSR read");
	zassert_equal(data, 0x786dU, "BMSR (got 0x%04x)", data);

	zassert_ok(mdio_read(MDIO_DEV, 3U, MII_BMSR, &data), "read from a silent address");
	zassert_equal(data, 0U, "nobody answers at prtad 3 (got 0x%04x)", data);

	/* When the PHY answers a write, the register file takes the value --
	 * this is what makes the PHY suite's autoneg flow possible. */
	zassert_ok(mdio_write(MDIO_DEV, 1U, MII_BMCR, 0x1140U), "BMCR write");
	zassert_equal(agm_fake_mdio_phy_get_reg(&phy_model, MII_BMCR), 0x1140U,
		      "the PHY latched BMCR (got 0x%04x)",
		      agm_fake_mdio_phy_get_reg(&phy_model, MII_BMCR));
	zassert_ok(mdio_read(MDIO_DEV, 1U, MII_BMCR, &data), "BMCR read back");
	zassert_equal(data, 0x1140U, "BMCR reads back (got 0x%04x)", data);

	phy_model.present = false;
}

ZTEST(mdio_agm, test_07_a_stuck_controller_times_out_instead_of_reading_stale_data)
{
	uint16_t data = 0x5a5aU;

	/* BUSY never clears: the old mdio_wait_busy() returned silently, the call
	 * reported success and the caller got whatever the DATA field happened to
	 * hold -- which reads as "a PHY that answers wrong" rather than "a bus
	 * that hangs". */
	bus_stuck = true;
	zassert_equal(mdio_read(MDIO_DEV, 1U, MII_BMCR, &data), -ETIMEDOUT,
		      "a stuck controller times out");
	zassert_equal(data, 0x5a5aU, "and the caller's buffer is left alone (got 0x%04x)", data);

	zassert_equal(mdio_write(MDIO_DEV, 1U, MII_BMCR, 0x1140U), -ETIMEDOUT,
		      "the write path reports it too");

	bus_stuck = false;
	zassert_ok(mdio_read(MDIO_DEV, 1U, MII_BMCR, &data), "and the bus recovers");
}
