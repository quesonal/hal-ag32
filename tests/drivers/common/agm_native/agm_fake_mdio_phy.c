/*
 * Copyright (c) 2026 AgRV Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "agm_fake_mdio_phy.h"

#include <stddef.h>

#include <zephyr/net/mii.h>

/* Bit positions of the AgRV2K MDIO command word (drivers/ethernet/mdio/mdio_agm.c). */
#define MDIO_WRITE          BIT(0)
#define MDIO_READ           BIT(1)
#define MDIO_REGADDR_SHIFT  6U
#define MDIO_PHYADDR_SHIFT  11U
#define MDIO_DATA_SHIFT     16U
#define MDIO_FIELDS_MASK    (0x3ffU | (0x1fU << MDIO_REGADDR_SHIFT) | (0x1fU << MDIO_PHYADDR_SHIFT))

void agm_fake_mdio_phy_init(struct agm_fake_mdio_phy *phy, volatile uint32_t *reg, uint8_t prtad)
{
	for (int i = 0; i < 32; i++) {
		phy->phy_regs[i] = 0U;
	}

	phy->reg = reg;
	phy->prtad = prtad;
	phy->present = true;
	phy->last_cmd = 0U;

	/* A LAN8720 that has autoneg to advertise and that has already completed
	 * it: what a plugged-in board looks like. BMSR default is "capable, link
	 * down until the model says otherwise". */
	phy->phy_regs[MII_PHYID1R] = AGM_FAKE_PHY_ID1;
	phy->phy_regs[MII_PHYID2R] = AGM_FAKE_PHY_ID2;
	/* Capability bits a LAN8720 reports, with the link down until the model
	 * says otherwise. */
	phy->phy_regs[MII_BMSR] = MII_BMSR_100BASE_T4 | MII_BMSR_100BASE_X_FULL
				| MII_BMSR_100BASE_X_HALF | MII_BMSR_10_FULL | MII_BMSR_10_HALF
				| MII_BMSR_AUTONEG_ABILITY | MII_BMSR_AUTONEG_COMPLETE;
	phy->phy_regs[MII_ANAR] = MII_ADVERTISE_100_FULL | MII_ADVERTISE_100_HALF
				| MII_ADVERTISE_10_FULL | MII_ADVERTISE_10_HALF;
}

void agm_fake_mdio_phy_set_reg(struct agm_fake_mdio_phy *phy, uint8_t regad, uint16_t val)
{
	phy->phy_regs[regad & 0x1fU] = val;
}

uint16_t agm_fake_mdio_phy_get_reg(const struct agm_fake_mdio_phy *phy, uint8_t regad)
{
	return phy->phy_regs[regad & 0x1fU];
}

void agm_fake_mdio_phy_set_link(struct agm_fake_mdio_phy *phy, bool up)
{
	uint16_t bmsr = phy->phy_regs[MII_BMSR];

	phy->phy_regs[MII_BMSR] = up ? (bmsr | MII_BMSR_LINK_STATUS)
				     : (bmsr & ~MII_BMSR_LINK_STATUS);

	/* The link partner's advertisement is what the driver decodes the speed
	 * from: 100 Mb full duplex when the link is up, nothing when it is down. */
	phy->phy_regs[MII_ANLPAR] = up ? (MII_ADVERTISE_100_FULL | MII_ADVERTISE_100_HALF
					  | MII_ADVERTISE_10_FULL | MII_ADVERTISE_10_HALF
					  )
				      : 0U;
}

uint32_t agm_fake_mdio_phy_reg_read(const struct agm_fake_mdio_phy *phy)
{
	uint32_t cmd = phy->last_cmd;
	uint16_t data = 0U;

	if (phy->present && (cmd & MDIO_READ) != 0U &&
	    ((cmd >> MDIO_PHYADDR_SHIFT) & 0x1fU) == phy->prtad) {
		data = phy->phy_regs[(cmd >> MDIO_REGADDR_SHIFT) & 0x1fU];
	}

	/* BUSY/LINK_FAIL stay clear: the transaction completed. */
	return (cmd & MDIO_FIELDS_MASK) | ((uint32_t)data << MDIO_DATA_SHIFT);
}

void agm_fake_mdio_phy_reg_write(struct agm_fake_mdio_phy *phy, uint32_t val)
{
	phy->last_cmd = val;

	if (phy->reg != NULL) {
		*phy->reg = val;
	}

	if (!phy->present || (val & MDIO_WRITE) == 0U ||
	    ((val >> MDIO_PHYADDR_SHIFT) & 0x1fU) != phy->prtad) {
		return;
	}

	uint8_t regad = (val >> MDIO_REGADDR_SHIFT) & 0x1fU;
	uint16_t data = (uint16_t)((val >> MDIO_DATA_SHIFT) & 0xffffU);

	if (regad == MII_BMCR && (data & MII_BMCR_RESET) != 0U) {
		/* The soft reset bit is self-clearing on the real part, and the
		 * driver polls for exactly that. */
		data &= ~MII_BMCR_RESET;
	}

	phy->phy_regs[regad] = data;
}
