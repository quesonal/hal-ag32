/*
 * Copyright (c) 2026 AgRV Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief native_sim suite for drivers/ethernet/phy/phy_agm_lan8720.c.
 *
 * This is the "no PHY on the bus" case, and that is deliberate: it is what the
 * LAN8720's move of its reset + ID probe off
 * the device init thread (where its sleeps held up every later POST_KERNEL
 * device and main()) into the first monitor-work pass. The MDIO register here
 * is a RAM window (tests/drivers/common/agm_native), so no PHY answers and the
 * probe follows its failure path -- which is exactly what used to be visible as
 * "the whole boot waits ~0.4 s for a missing PHY" and as
 * "device_is_ready(phy0) means the PHY answered".
 *
 * What the suite checks:
 *  - the boot does not wait for the PHY (init returns after scheduling the
 *    monitor work; the sleeping probe happens on the workqueue);
 *  - device_is_ready(phy0) is true even though nothing answered -- the
 *    semantic change, pinned so it is a decision and not an
 *    accident;
 *  - a consumer that registers a link callback gets exactly one "down" event
 *    and then silence: the driver remembers the probe failure instead of
 *    hammering the MDIO bus for the rest of the boot;
 *  - the write path (phy_configure_link -> BMCR/ANAR through the MDIO bus) and
 *    the phy_read/phy_write passthroughs reach the MAC's MDIO register with the
 *    right encoding.
 *  - and, since the driver's MDIO register access goes through the weak hooks
 *    described in tests/drivers/common/agm_native/agm_fake_mdio_phy.h, a PHY
 *    that *does* answer: phy1 (prtad 2) has a LAN8720
 *    register file behind it, so the suite also drives the parts that only
 *    exist once a PHY is there -- ID decode, autoneg completion, the link
 *    up/down transitions and the speed the driver reports. phy0 (prtad 1)
 *    stays silent and keeps the "nothing answers" cases honest.
 */

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/net/mii.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/phy.h>
#include <zephyr/ztest.h>

#include "agm_fake_mdio_phy.h"

#define PHY_DEV  DEVICE_DT_GET(DT_NODELABEL(phy0))
#define PHY2_DEV DEVICE_DT_GET(DT_NODELABEL(phy1))
#define ETH_DEV  DEVICE_DT_GET(DT_NODELABEL(eth_agm0))

#define MAC_BASE 0x41040000UL
#define R_MDIO   0x10U

#define MDIO_WRITE         BIT(0)
#define MDIO_READ          BIT(1)
#define MDIO_BUSY          BIT(3)
#define MDIO_REGADDR_SHIFT 6U
#define MDIO_PHYADDR_SHIFT 11U
#define MDIO_DATA_SHIFT    16U

/* The board's PHY straps to prtad 1 (see agrv2k.dtsi's phy0 reg). */
#define PHY_ADDR 1U

/* The one that answers (overlay: phy1). */
#define PHY2_ADDR 2U

/* Two instances of the clause-22 model on one bus, routed by the address in
 * each MDIO command. phy_a is the "nothing answers" case, phy_b the one
 * the link tests drive. */
static struct agm_fake_mdio_phy phy_a;
static struct agm_fake_mdio_phy phy_b;
static uint32_t bus_last_cmd;

static struct agm_fake_mdio_phy *model_for(uint32_t cmd)
{
	uint8_t prtad = (uint8_t)((cmd >> MDIO_PHYADDR_SHIFT) & 0x1fU);

	if (prtad == PHY_ADDR) {
		return &phy_a;
	}
	if (prtad == PHY2_ADDR) {
		return &phy_b;
	}
	return NULL;
}

uint32_t mdio_agm_reg_read(const struct device *dev)
{
	struct agm_fake_mdio_phy *phy = model_for(bus_last_cmd);

	ARG_UNUSED(dev);

	if (phy == NULL || !phy->present) {
		/* Nobody answers at that address: DATA reads 0, exactly what the
		 * RAM window gives (and what phy0's cases rely on). */
		return bus_last_cmd;
	}
	return agm_fake_mdio_phy_reg_read(phy);
}

void mdio_agm_reg_write(const struct device *dev, uint32_t val)
{
	ARG_UNUSED(dev);

	bus_last_cmd = val;
	/* Keep the RAM window in sync: the driver's own encoding assertions
	 * (test_04/test_05) read the command back from there. */
	*(volatile uint32_t *)(MAC_BASE + R_MDIO) = val;

	agm_fake_mdio_phy_reg_write(&phy_a, val);
	agm_fake_mdio_phy_reg_write(&phy_b, val);
}

static uint32_t mdio_reg(void)
{
	return *(volatile uint32_t *)(MAC_BASE + R_MDIO);
}

static uint32_t mdio_data(void)
{
	return (mdio_reg() >> MDIO_DATA_SHIFT) & 0xffffU;
}

static uint32_t mdio_regad(void)
{
	return (mdio_reg() >> MDIO_REGADDR_SHIFT) & 0x1fU;
}

static int link_events;
static const struct device *cb_dev;
static struct phy_link_state last_state;

static void link_cb(const struct device *dev, struct phy_link_state *state, void *user_data)
{
	ARG_UNUSED(user_data);

	cb_dev = dev;
	last_state = *state;
	link_events++;
}

/*
 * Boot-time model, i.e. before the PHY driver's monitor work runs its one and
 * only probe: phy0 (prtad 1) is silent, phy1 (prtad 2) is a LAN8720 that has
 * completed autoneg and whose link is still down.
 */
static int phy_suite_boot_model(void)
{
	agm_fake_mdio_phy_init(&phy_a, NULL, PHY_ADDR);
	phy_a.present = false;

	agm_fake_mdio_phy_init(&phy_b, NULL, PHY2_ADDR);
	phy_b.present = true;
	agm_fake_mdio_phy_set_link(&phy_b, false);

	return 0;
}

SYS_INIT(phy_suite_boot_model, EARLY, 2);

ZTEST_SUITE(phy_agm_lan8720, NULL, NULL, NULL, NULL, NULL);

/* ---- the probe no longer runs on the init thread ---------------------- */

ZTEST(phy_agm_lan8720, test_01_boot_does_not_wait_for_the_phy)
{
	/* With the reset + probe still inside phy_agm_lan8720_init() this number
	 * was the probe: k_sleep(25) + one 50 ms soft-reset poll per attempt, up
	 * to PHY_LAN8720_ID_PROBE_RETRIES extra rounds -- ~375 ms with no PHY
	 * answering, all of it in the init thread, before ztest could even
	 * start. It is now the boot itself (a few ms). The before/after is in
	 * phy_agm_lan8720_probe()'s header comment; this is
	 * the same guard where it can run in CI. */
	zassert_true(k_uptime_get() < 100, "init did not sleep for the PHY (uptime %lld ms)",
		     k_uptime_get());
}

ZTEST(phy_agm_lan8720, test_02_device_is_ready_although_no_phy_answers)
{
	zassert_true(device_is_ready(PHY_DEV),
		     "the PHY device is ready even though the bus stays silent -- readiness means "
		     "\"driver initialised\", not \"the PHY answered\"");
}

ZTEST(phy_agm_lan8720, test_03_silent_bus_reports_link_down_once)
{
	link_events = 0;
	zassert_ok(phy_link_callback_set(PHY_DEV, link_cb, NULL), "callback registered");

	/* phy_agm_lan8720_link_cb_set() fires the current state right away, so the
	 * consumer sees "down" without having to race the monitor work. */
	zassert_equal(link_events, 1, "the registered callback saw the current state once");
	zassert_false(last_state.is_up, "a silent bus is not a link");

	/* Then it must go quiet: the probe failed, so the monitor work returns
	 * without polling (probe_failed) instead of waking every 500 ms. */
	k_sleep(K_MSEC(CONFIG_PHY_MONITOR_PERIOD + 200));
	zassert_equal(link_events, 1, "no further events for a PHY that never answered");

	struct phy_link_state state;

	zassert_ok(phy_get_link_state(PHY_DEV, &state), "link state readable");
	zassert_false(state.is_up, "still down");
	zassert_equal(cb_dev, PHY_DEV, "the event came from the silent PHY");

	zassert_ok(phy_link_callback_set(PHY_DEV, NULL, NULL), "callback removed");
}

/* ---- the write path --------------------------------------------------- */

ZTEST(phy_agm_lan8720, test_04_configure_link_advertises_over_mdio)
{
	/* 100BASE-TX full duplex only: the PHY answers nothing, but the driver
	 * still has to put a correct clause-22 command on the bus.
	 *
	 * phy_mii_cfg_link_autoneg() writes the advertisement (MII_ANAR) first and
	 * then BMCR to start the negotiation, so what a single-register RAM model
	 * can see is that *last* write -- asserting the ANAR bits would only work
	 * with an intercepting model (the same boundary as the I2C suite). */
	zassert_ok(phy_configure_link(PHY_DEV, LINK_FULL_100BASE, 0), "link configured");

	zassert_equal((mdio_reg() >> MDIO_PHYADDR_SHIFT) & 0x1fU, PHY_ADDR, "PHYADDR from DT reg");
	zassert_equal(mdio_regad(), MII_BMCR, "the last write is BMCR (reg %u)", mdio_regad());
	zassert_equal(mdio_data() & MII_BMCR_AUTONEG_ENABLE, MII_BMCR_AUTONEG_ENABLE,
		      "autoneg enabled (BMCR=0x%04x)", mdio_data());
	zassert_equal(mdio_data() & MII_BMCR_AUTONEG_RESTART, MII_BMCR_AUTONEG_RESTART,
		      "autoneg restarted");
	zassert_equal(mdio_data() & (MII_BMCR_SPEED_MASK | MII_BMCR_DUPLEX_MODE), 0U,
		      "no forced speed/duplex while negotiating");
	zassert_equal(mdio_reg() & MDIO_WRITE, MDIO_WRITE, "a write command went out");

	/* A different speed set restarts the negotiation the same way. */
	zassert_ok(phy_configure_link(PHY_DEV, LINK_HALF_10BASE, 0), "reconfigured");
	zassert_equal(mdio_data(), 0x1200U, "the same BMCR command (BMCR=0x%04x)", mdio_data());
}

ZTEST(phy_agm_lan8720, test_05_phy_read_and_write_reach_the_mdio_register)
{
	uint32_t value = 0U;

	zassert_ok(phy_write(PHY_DEV, MII_BMSR, 0x782dU), "phy_write");
	zassert_equal(mdio_data(), 0x782dU, "the payload is on the bus");
	zassert_equal(mdio_regad(), MII_BMSR, "in the caller's register");
	zassert_equal((mdio_reg() >> MDIO_PHYADDR_SHIFT) & 0x1fU, PHY_ADDR, "at the caller's PHY");

	zassert_ok(phy_read(PHY_DEV, MII_BMSR, &value), "phy_read");
	/* The bus is a RAM window: the read answer is the command's own DATA
	 * field, i.e. 0 (documented in tests/drivers/ethernet/mdio_agm). */
	zassert_equal(value, 0U, "mdio_read() answers 0 on a RAM window");
	zassert_equal(mdio_reg() & MDIO_READ, MDIO_READ, "a read command went out");
	zassert_equal(mdio_reg() & MDIO_BUSY, 0U, "nothing left busy");
}

/* ---- the same driver with a PHY that answers ------------------------- */

ZTEST(phy_agm_lan8720, test_06_the_probe_reads_a_lan8720_id)
{
	uint32_t id1 = 0U;
	uint32_t id2 = 0U;

	/* phy1 was probed at boot, before this test existed; the ID the driver
	 * checked is the model's. Reading it back through the driver's own
	 * passthrough shows the data path works both ways. */
	zassert_true(device_is_ready(PHY2_DEV), "the answering PHY is up");
	zassert_ok(phy_read(PHY2_DEV, MII_PHYID1R, &id1), "PHYID1 read");
	zassert_ok(phy_read(PHY2_DEV, MII_PHYID2R, &id2), "PHYID2 read");
	zassert_equal(id1, AGM_FAKE_PHY_ID1, "PHYID1 (got 0x%04x)", id1);
	zassert_equal(id2, AGM_FAKE_PHY_ID2, "PHYID2 (got 0x%04x)", id2);

	/* The bus is addressed by the caller's prtad: the same register read at
	 * phy0's address (nothing there) answers 0. */
	zassert_ok(phy_read(PHY_DEV, MII_PHYID1R, &id1), "read at the silent address");
	zassert_equal(id1, 0U, "the silent PHY answers 0 (got 0x%04x)", id1);
}

ZTEST(phy_agm_lan8720, test_07_link_up_and_down_are_reported_with_the_speed)
{
	struct net_if *iface = net_if_lookup_by_dev(ETH_DEV);
	struct phy_link_state state = { 0 };

	zassert_not_null(iface, "the MAC has an iface");
	zassert_ok(phy_get_link_state(PHY2_DEV, &state), "state readable");
	zassert_false(state.is_up, "phy1 starts with the link down");
	zassert_false(net_if_is_carrier_ok(iface), "and the carrier is off");

	/* Plug the cable in: the monitor work polls BMSR
	 * (CONFIG_PHY_MONITOR_PERIOD) and updates its state with the speed it
	 * decoded from the link partner's advertisement. The suite does NOT
	 * register its own PHY callback here on purpose: a PHY has a single
	 * callback slot and it belongs to the MAC driver (DT phy-handle), which
	 * is exactly the coupling under test. */
	agm_fake_mdio_phy_set_link(&phy_b, true);
	for (int i = 0; i < 20 && !net_if_is_carrier_ok(iface); i++) {
		k_sleep(K_MSEC(CONFIG_PHY_MONITOR_PERIOD / 4 + 50));
	}

	zassert_ok(phy_get_link_state(PHY2_DEV, &state), "state readable");
	zassert_true(state.is_up, "the link came up");
	zassert_equal(state.speed, LINK_FULL_100BASE,
		      "100 Mb full duplex decoded (speed 0x%x)", state.speed);
	zassert_true(net_if_is_carrier_ok(iface),
		     "the PHY's link-up reached the net stack through the MAC driver ");

	/* ... and unplugging it reports the other direction. */
	agm_fake_mdio_phy_set_link(&phy_b, false);
	for (int i = 0; i < 20 && net_if_is_carrier_ok(iface); i++) {
		k_sleep(K_MSEC(CONFIG_PHY_MONITOR_PERIOD / 4 + 50));
	}

	zassert_ok(phy_get_link_state(PHY2_DEV, &state), "state readable");
	zassert_false(state.is_up, "link down again");
	zassert_false(net_if_is_carrier_ok(iface), "and the carrier followed it down");
}

ZTEST(phy_agm_lan8720, test_08_a_silent_phy_zeroes_every_read)
{
	uint32_t value = 0xffffU;

	/* phy0 never answered -- neither its ID nor any status register -- so the
	 * driver's probe stayed failed (it does not retry) and reads keep
	 * returning 0. Pinned because it is the contract that matters here: a missing PHY
	 * costs one attempt, not a polling loop. */
	zassert_ok(phy_read(PHY_DEV, MII_BMSR, &value), "read from the silent PHY");
	zassert_equal(value, 0U, "no PHY, no data (got 0x%04x)", value);

	zassert_ok(phy_get_link_state(PHY_DEV, &last_state), "state readable");
	zassert_false(last_state.is_up, "and the link is down");
}
