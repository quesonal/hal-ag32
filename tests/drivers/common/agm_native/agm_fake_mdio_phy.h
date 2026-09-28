/*
 * Copyright (c) 2026 AgRV Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief Clause-22 PHY model for the AgRV2K MDIO bus, for native_sim suites.
 *
 * `mdio_agm.c` reaches its register through two weak functions
 * (`mdio_agm_reg_read()/mdio_agm_reg_write()`); a suite overrides them and
 * forwards to this model. That is what makes a PHY testable off-target: the
 * MDIO register holds the command *and* the answer in one word, so a plain RAM
 * window always reads back the command.
 *
 * The model is a register file plus the transaction rules the driver depends
 * on:
 *  - a read answers with the selected register's 16 bits in DATA[31:16];
 *  - a write to BMCR with RESET (bit 15) set does not keep that bit (the PHY
 *    self-clears it, which is what phy_agm_lan8720_reset() polls for);
 *  - with no PHY (`present == false`) DATA reads as 0, i.e. exactly what the
 *    RAM window does -- that is the "silent bus" case the PHY suite pins.
 *
 * The model also mirrors every command into the real (RAM-backed) MMIO
 * register so a suite can still assert the command encoding it wrote.
 */

#ifndef AGM_FAKE_MDIO_PHY_H_
#define AGM_FAKE_MDIO_PHY_H_

#include <stdbool.h>
#include <stdint.h>

/* The LAN8720 identification the driver checks against (PHYID1/PHYID2). */
#define AGM_FAKE_PHY_ID1 0x0007U
#define AGM_FAKE_PHY_ID2 0xc0f1U

struct agm_fake_mdio_phy {
	/* The RAM-backed MDIO register (MAC0 + 0x10), kept in sync on writes. */
	volatile uint32_t *reg;
	/* Clause-22 register file, indexed by the 5-bit register address. */
	uint16_t phy_regs[32];
	/* Address this PHY answers to (the driver's prtad). */
	uint8_t prtad;
	/* false: nothing on the bus -- DATA reads as 0. */
	bool present;
	/* Last command word written by the driver. */
	uint32_t last_cmd;
};

/** @brief Bind the model to a register window and give it the LAN8720 ID. */
void agm_fake_mdio_phy_init(struct agm_fake_mdio_phy *phy, volatile uint32_t *reg,
			    uint8_t prtad);

/** @brief Set a clause-22 register (and the link state bits for BMSR/ANLPAR). */
void agm_fake_mdio_phy_set_reg(struct agm_fake_mdio_phy *phy, uint8_t regad, uint16_t val);

/** @brief Read back a clause-22 register. */
uint16_t agm_fake_mdio_phy_get_reg(const struct agm_fake_mdio_phy *phy, uint8_t regad);

/**
 * @brief Pretend the cable was plugged/unplugged.
 *
 * Sets/clears MII_BMSR's LINK_STATUS (0x0004) and mirrors the negotiated
 * speed into ANLPAR (100 Mb full duplex) so the driver's link-state decode has
 * something consistent to report.
 */
void agm_fake_mdio_phy_set_link(struct agm_fake_mdio_phy *phy, bool up);

/** @brief The model's half of the two weak hooks. */
uint32_t agm_fake_mdio_phy_reg_read(const struct agm_fake_mdio_phy *phy);
void agm_fake_mdio_phy_reg_write(struct agm_fake_mdio_phy *phy, uint32_t val);

#endif /* AGM_FAKE_MDIO_PHY_H_ */
