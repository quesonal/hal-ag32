/*
 * Copyright (c) 2026 AgRV Contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * LAN8720 10/100 Ethernet PHY driver — AgRV2K-side PHY framework driver.
 *
 * Scope
 *   PHY reset, ID probe (sanity-checks against the LAN8720 OUI), MII
 *   autoneg, and link-state polling. Everything that talks to the PHY
 *   on the MDIO bus goes through here; nothing about this driver
 *   touches the Ethernet MAC. The AgRV2K has no on-die MAC, so a
 *   fabric-soft MAC IP is required for TX/RX; that lives outside the
 *   PHY framework
 *   (TODO: register spec for the next commit).
 *
 * Why own it instead of reusing upstream microchip,lan8742
 *   The LAN8720 and LAN8742 share the Microchip OUI (0x0007C0xx /
 *   0x0007C1xx), the same BMCR / BMSR / ANAR / ANLPAR register map,
 *   and the same soft-reset timing. The LAN8742 driver does NOT
 *   check the revision bits, so on this hardware the LAN8720 actually
 *   *would* init under the upstream driver — but the next time the
 *   revision semantics diverge (or a board genuinely has a LAN8742
 *   strapped onto the same MDIO bus), we'd silently bind to the
 *   wrong part. Owning the compatible costs ~440 lines and pays back
 *   the day a real LAN8742 shows up on the dev board.
 *
 * Known limits
 *   - 10/100 only (LAN8720 has no GMII path; MODE[2:0]=000b with the
 *     default-speeds DT knob controls what we advertise, the binding
 *     drops the 1000BASE entries).
 *   - Clause 22 only (no C45).
 *   - No interrupt-driven link-state reporting yet — the upstream
 *     pattern uses a polled work-delayable because the LAN8720 nINT
 *     pin is open-drain and tying it to a PLIC line would require
 *     another fabric MAC register. Polling is good enough at 100 ms
 *     granularity for now.
 */

#define DT_DRV_COMPAT microchip_lan8720

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(phy_agm_lan8720, CONFIG_PHY_LOG_LEVEL);

#include <string.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/mdio.h>
#include <zephyr/net/mii.h>
#include <zephyr/kernel.h>
#include <zephyr/net/phy.h>
#include <zephyr/sys/util_macro.h>

#include "phy_mii.h"

#define PHY_LAN8720_ID1 0x0007U   /* Microchip OUI (was SMSC), bits[31:16] */
#define PHY_LAN8720_ID2_OUI 0xC0F0U /* OUI bits[15:10] + model=0, low nibble = rev */
#define PHY_LAN8720_ID2_OUI_MASK 0xFFF0U

struct phy_agm_lan8720_config {
	uint8_t phy_addr;
	enum phy_link_speed default_speeds;
	const struct device *const mdio;
	struct gpio_dt_spec gpio_reset;
};

struct phy_agm_lan8720_data {
	const struct device *dev;
	phy_callback_t cb;
	void *cb_data;
	struct phy_link_state state;
	struct k_work_delayable monitor_work;
	bool autoneg_in_progress;
	k_timepoint_t autoneg_timeout;
	/* Reset + ID probe + autoneg kick-off have run (see
	 * phy_agm_lan8720_probe()). Their sleeps used to live in the device
	 * init; they run from the monitor work now. */
	bool probed;
	/* The probe gave up (the PHY never answered): keep the result so the
	 * work item does not re-attempt it -- and does not hammer the MDIO bus
	 * and the system workqueue -- for the rest of the boot. */
	bool probe_failed;
};

#define PHY_LAN8720_AUTONEG_TIMEOUT_MS_DEFAULT 4000
#define PHY_LAN8720_AUTONEG_POLL_INTERVAL_MS 100

/* Safety net around the ID probe.
 *
 * The real cause of the "ID 0x00000000" cold-boot failures seen on
 * The missing `volatile` on the MDIO register struct was --
 * see mdio_agm.c. These retries are a cheap guard for the remaining
 * case where the PHY is simply slow to answer after the soft reset
 * (a probe failure here costs the whole Ethernet device, so one extra
 * round trip is worth it). */
#define PHY_LAN8720_ID_PROBE_RETRIES 2U
#define PHY_LAN8720_ID_PROBE_DELAY_MS 50U

static void phy_agm_lan8720_invoke_link_cb(const struct device *dev);
static int phy_agm_lan8720_cfg_link(const struct device *dev,
				    enum phy_link_speed adv_speeds, uint32_t flags);

static int phy_agm_lan8720_reset(const struct device *dev)
{
	const struct phy_agm_lan8720_config *const cfg = dev->config;
	uint32_t timeout = 12U;
	uint16_t value;
	int ret;

	/* Optional GPIO reset (nRST, active-low). The driver asserts it
	 * (drive low), holds for 1 ms, then releases (drive high).
	 * gpio-reset spec uses GPIO_ACTIVE_LOW, so gpio_pin_set_dt(1)
	 * writes 0 to the pin. */
	if (cfg->gpio_reset.port != NULL) {
		ret = gpio_pin_configure_dt(&cfg->gpio_reset, GPIO_OUTPUT_ACTIVE);
		if (ret != 0) {
			LOG_ERR("PHY %d: failed to init reset GPIO: %d",
				cfg->phy_addr, ret);
			return ret;
		}
		gpio_pin_set_dt(&cfg->gpio_reset, 1);
		k_sleep(K_MSEC(1));
		gpio_pin_set_dt(&cfg->gpio_reset, 0);
	}

	k_sleep(K_MSEC(25));

	/* Soft reset via MII_BMCR (bit15 = RESET, self-clearing). */
	if (mdio_write(cfg->mdio, cfg->phy_addr, MII_BMCR, MII_BMCR_RESET) < 0) {
		return -EIO;
	}

	/* Wait up to ~0.6 s for the soft reset to clear. */
	do {
		if (timeout-- == 0U) {
			return -ETIMEDOUT;
		}
		k_sleep(K_MSEC(50));
		if (mdio_read(cfg->mdio, cfg->phy_addr, MII_BMCR, &value) < 0) {
			return -EIO;
		}
	} while ((value & MII_BMCR_RESET) != 0U);

	return 0;
}

static int phy_agm_lan8720_get_id(const struct device *dev, uint32_t *phy_id)
{
	const struct phy_agm_lan8720_config *const cfg = dev->config;
	uint16_t id1, id2;

	if (mdio_read(cfg->mdio, cfg->phy_addr, MII_PHYID1R, &id1) < 0) {
		return -EIO;
	}
	if (mdio_read(cfg->mdio, cfg->phy_addr, MII_PHYID2R, &id2) < 0) {
		return -EIO;
	}

	*phy_id = ((uint32_t)id1 << 16) | id2;

	/* OUI check: ID1 must equal the Microchip OUI (0x0007), and
	 * ID2 must match the LAN8720 model bits (0xC0F0 mask). Revision
	 * nibble (ID2[3:0]) is ignored — different silicon revisions put
	 * different values there. */
	if (id1 != PHY_LAN8720_ID1) {
		return -ENODEV;
	}
	if ((id2 & PHY_LAN8720_ID2_OUI_MASK) != PHY_LAN8720_ID2_OUI) {
		return -ENODEV;
	}

	return 0;
}

static int phy_agm_lan8720_update_link_state(const struct device *dev)
{
	const struct phy_agm_lan8720_config *const cfg = dev->config;
	struct phy_agm_lan8720_data *const data = dev->data;
	uint16_t bmcr_reg = 0U;
	uint16_t bmsr_reg = 0U;
	bool link_up;

	if (mdio_read(cfg->mdio, cfg->phy_addr, MII_BMSR, &bmsr_reg) < 0) {
		return -EIO;
	}

	link_up = (bmsr_reg & MII_BMSR_LINK_STATUS) != 0U;

	if (!link_up) {
		if (data->state.is_up) {
			data->state.is_up = false;
			data->state.speed = 0U;
			LOG_INF("PHY %d: link DOWN", cfg->phy_addr);
			return 0;
		}
		return -EAGAIN;
	}

	/* Link up — figure out speed/duplex. If autoneg is disabled, the
	 * speed is in BMCR[6:13] directly. */
	if (mdio_read(cfg->mdio, cfg->phy_addr, MII_BMCR, &bmcr_reg) < 0) {
		return -EIO;
	}

	if ((bmcr_reg & MII_BMCR_AUTONEG_ENABLE) == 0U) {
		enum phy_link_speed new_speed = phy_mii_get_link_speed_bmcr_reg(dev, bmcr_reg);

		if (!data->state.is_up || data->state.speed != new_speed) {
			data->state.is_up = true;
			data->state.speed = new_speed;
			LOG_INF("PHY %d: link UP, %s Mb %s duplex (forced)",
				cfg->phy_addr,
				PHY_LINK_IS_SPEED_100M(new_speed) ? "100" : "10",
				PHY_LINK_IS_FULL_DUPLEX(new_speed) ? "full" : "half");
			return 0;
		}
		return -EAGAIN;
	}

	/* Autoneg path: if link was already up last poll, no change. */
	if (data->state.is_up) {
		return -EAGAIN;
	}

	/* Don't flip state.is_up here: autoneg hasn't actually completed
	 * yet. If we did, the very next monitor poll would hit the
	 * `if (data->state.is_up) return -EAGAIN;` short-circuit above
	 * and skip check_autoneg() entirely, so the link-up callback
	 * would never fire (autoneg completes silently inside
	 * check_autoneg() below). check_autoneg() sets state.is_up
	 * only when it sees the AUTONEG_COMPLETE bit in BMSR. */
	data->autoneg_in_progress = true;
	data->autoneg_timeout = sys_timepoint_calc(K_MSEC(CONFIG_PHY_AUTONEG_TIMEOUT_MS));
	LOG_DBG("PHY %d: autoneg started", cfg->phy_addr);
	return -EINPROGRESS;
}

/*
 * Decode the negotiated speed/duplex from the link partner's advertised
 * abilities (ANLPAR). The PHY picked the highest common mode with our
 * ANAR -- for a 10/100 PHY like the LAN8720, that's either 100/full,
 * 100/half, 10/full, or 10/half. The upstream phy_mii.h only has a
 * BMCR-decode helper (forced mode); autoneg needs ANLPAR + ANAR.
 */
static enum phy_link_speed phy_agm_lan8720_decode_anlpar(uint16_t anlpar)
{
	/* The PHY picked the highest common mode with our ANAR, so check the
	 * full-duplex bits first. The ANLPAR[8]/[7]/[6]/[5] bits correspond
	 * to 100/full, 100/half, 10/full, 10/half respectively. */
	if (anlpar & MII_ADVERTISE_100_FULL) {
		return LINK_FULL_100BASE;
	}
	if (anlpar & MII_ADVERTISE_100_HALF) {
		return LINK_HALF_100BASE;
	}
	if (anlpar & MII_ADVERTISE_10_FULL) {
		return LINK_FULL_10BASE;
	}
	if (anlpar & MII_ADVERTISE_10_HALF) {
		return LINK_HALF_10BASE;
	}
	return 0U;
}

static int phy_agm_lan8720_check_autoneg(const struct device *dev)
{
	const struct phy_agm_lan8720_config *const cfg = dev->config;
	struct phy_agm_lan8720_data *const data = dev->data;
	uint16_t anlpar = 0U;
	uint16_t bmsr = 0U;

	/* BMSR latch caveat: first read may return stale latched values;
	 * second read gives the live status. */
	if (mdio_read(cfg->mdio, cfg->phy_addr, MII_BMSR, &bmsr) < 0) {
		return -EIO;
	}
	if (mdio_read(cfg->mdio, cfg->phy_addr, MII_BMSR, &bmsr) < 0) {
		return -EIO;
	}

	if ((bmsr & MII_BMSR_AUTONEG_COMPLETE) == 0U) {
		if (sys_timepoint_expired(data->autoneg_timeout)) {
			LOG_WRN("PHY %d: autoneg timed out", cfg->phy_addr);
			data->autoneg_in_progress = false;
			return -ETIMEDOUT;
		}
		return -EINPROGRESS;
	}

	if (mdio_read(cfg->mdio, cfg->phy_addr, MII_ANLPAR, &anlpar) < 0) {
		return -EIO;
	}

	data->state.is_up = true;
	data->state.speed = phy_agm_lan8720_decode_anlpar(anlpar);
	data->autoneg_in_progress = false;
	LOG_INF("PHY %d: link UP, %s Mb %s duplex (autoneg)",
		cfg->phy_addr,
		PHY_LINK_IS_SPEED_100M(data->state.speed) ? "100" : "10",
		PHY_LINK_IS_FULL_DUPLEX(data->state.speed) ? "full" : "half");
	return 0;
}

/*
 * Reset the PHY, probe its ID and kick off autoneg -- everything that sleeps
 * (the GPIO reset hold, the MII soft-reset poll and the ID retries).
 *
 * This used to run inside phy_agm_lan8720_init(), i.e. on the init thread at
 * CONFIG_PHY_INIT_PRIORITY, where its sleeps blocked every later POST_KERNEL
 * device and the application's main thread behind it.
 * (lan8720_link, 200 MHz MAC bitstream, no reset-gpios so the fixed 25 ms +
 * one 50 ms soft-reset poll): the PHY's ID line was the first timestamped
 * line of the boot at device-uptime 75 ms, and the sample's main() only
 * started at 150 ms -- the whole first 75 ms of kernel time was this
 * function sleeping inside init.
 *
 * Runs from the monitor work now. On failure the link simply never comes up
 * (the consumer's own link timeout reports it); the probe is not retried, so a
 * board with no PHY does not keep a workqueue thread busy with MDIO polls.
 */
static int phy_agm_lan8720_probe(const struct device *dev)
{
	const struct phy_agm_lan8720_config *const cfg = dev->config;
	struct phy_agm_lan8720_data *const data = dev->data;
	uint32_t phy_id = 0U;
	uint8_t attempt;
	int ret;

	/* Reset + probe, retrying while the MDIO bus still looks dead
	 * (all-zero / all-one reads). Any other failure -- or a wrong but
	 * plausible OUI -- is reported immediately. */
	for (attempt = 0U;; attempt++) {
		bool bus_dead;

		ret = phy_agm_lan8720_reset(dev);
		if (ret < 0) {
			LOG_ERR("PHY %d: reset failed: %d", cfg->phy_addr, ret);
			return ret;
		}

		ret = phy_agm_lan8720_get_id(dev, &phy_id);
		if (ret == 0) {
			break;
		}

		bus_dead = (phy_id == 0x00000000U) || (phy_id == 0xFFFFFFFFU);

		if (ret != -ENODEV || !bus_dead ||
		    attempt == PHY_LAN8720_ID_PROBE_RETRIES) {
			if (ret == -ENODEV) {
				LOG_ERR("PHY %d: ID 0x%08x does not match LAN8720 OUI (0x0007C0Fx)",
					cfg->phy_addr, phy_id);
			} else {
				LOG_ERR("PHY %d: ID read failed: %d",
					cfg->phy_addr, ret);
			}
			return ret;
		}

		if (attempt == 0U) {
			LOG_WRN("PHY %d: ID probe returned 0x%08x, retrying",
				cfg->phy_addr, phy_id);
		} else {
			LOG_DBG("PHY %d: ID probe %u returned 0x%08x, retrying",
				cfg->phy_addr, attempt, phy_id);
		}
		k_msleep(PHY_LAN8720_ID_PROBE_DELAY_MS);
	}

	LOG_INF("PHY %d: ID 0x%08x (LAN8720 OK)", cfg->phy_addr, phy_id);

	ret = phy_agm_lan8720_cfg_link(dev, cfg->default_speeds, 0);
	if (ret == -EALREADY) {
		data->autoneg_in_progress = true;
		data->autoneg_timeout = sys_timepoint_calc(K_MSEC(CONFIG_PHY_AUTONEG_TIMEOUT_MS));
	}

	return 0;
}

static void phy_agm_lan8720_monitor_work(struct k_work *work)
{
	struct k_work_delayable *dwork = k_work_delayable_from_work(work);
	struct phy_agm_lan8720_data *const data =
		CONTAINER_OF(dwork, struct phy_agm_lan8720_data, monitor_work);
	const struct device *dev = data->dev;
	const struct phy_agm_lan8720_config *const cfg = dev->config;
	bool changed = false;
	int rc;

	/* First run after init: do the (sleeping) reset + probe here instead of
	 * on the init thread. Every later run skips straight to the poll. */
	if (!data->probed) {
		if (data->probe_failed || phy_agm_lan8720_probe(dev) < 0) {
			data->probe_failed = true;
			return;
		}
		data->probed = true;
	}

	rc = phy_agm_lan8720_update_link_state(dev);
	if (rc == 0) {
		changed = true;
	} else if (rc == -EINPROGRESS) {
		/* Autoneg in flight (or just completed this very poll).
		 * Drive check_autoneg and propagate completion to the
		 * callback path; otherwise the link-up event is silently
		 * lost (this is how Board->PC ping worked once on init
		 * but PC->Board ARP never received a reply -- the
		 * sample's on_link_state_change() only ever saw link DOWN). */
		rc = phy_agm_lan8720_check_autoneg(dev);
		if (rc == 0) {
			changed = true;
		}
		k_work_schedule(&data->monitor_work,
				K_MSEC(PHY_LAN8720_AUTONEG_POLL_INTERVAL_MS));
		if (!changed) {
			return;
		}
	} else if (rc == -EAGAIN) {
		/* No state change, just reschedule. */
	} else {
		LOG_ERR("PHY %d: link poll failed: %d", cfg->phy_addr, rc);
	}

	if (data->autoneg_in_progress) {
		k_work_schedule(&data->monitor_work,
				K_MSEC(PHY_LAN8720_AUTONEG_POLL_INTERVAL_MS));
		return;
	}

	if (changed && data->cb != NULL) {
		phy_agm_lan8720_invoke_link_cb(dev);
	}

	k_work_schedule(&data->monitor_work, K_MSEC(CONFIG_PHY_MONITOR_PERIOD));
}

static int phy_agm_lan8720_get_link_state(const struct device *dev,
					 struct phy_link_state *state)
{
	struct phy_agm_lan8720_data *const data = dev->data;

	*state = data->state;
	return 0;
}

static int phy_agm_lan8720_cfg_link(const struct device *dev,
				    enum phy_link_speed adv_speeds, uint32_t flags)
{
	struct phy_agm_lan8720_data *const data = dev->data;
	int ret;

	(void)flags; /* LAN8720 has no PLCA / T1S knobs */

	ret = phy_mii_cfg_link_autoneg(dev, adv_speeds, false);
	if (ret < 0 && ret != -EALREADY) {
		return ret;
	}

	if (ret == 0) {
		/* Autoneg kicked off; the monitor work will pick up the
		 * completion on its next poll. */
		data->autoneg_in_progress = true;
		data->autoneg_timeout = sys_timepoint_calc(K_MSEC(CONFIG_PHY_AUTONEG_TIMEOUT_MS));
	}

	return 0;
}

static int phy_agm_lan8720_read(const struct device *dev, uint16_t reg_addr, uint32_t *data)
{
	const struct phy_agm_lan8720_config *const cfg = dev->config;
	uint16_t value;
	int rc;

	rc = mdio_read(cfg->mdio, cfg->phy_addr, reg_addr, &value);
	if (rc < 0) {
		return rc;
	}
	*data = value;
	return 0;
}

static int phy_agm_lan8720_write(const struct device *dev, uint16_t reg_addr, uint32_t data)
{
	const struct phy_agm_lan8720_config *const cfg = dev->config;

	return mdio_write(cfg->mdio, cfg->phy_addr, reg_addr, (uint16_t)data);
}

static int phy_agm_lan8720_link_cb_set(const struct device *dev,
				       phy_callback_t cb, void *user_data)
{
	struct phy_agm_lan8720_data *const data = dev->data;

	data->cb = cb;
	data->cb_data = user_data;

	/* Fire the callback once with the current state so the consumer
	 * doesn't have to race against the monitor work. */
	if (cb != NULL) {
		phy_agm_lan8720_invoke_link_cb(dev);
	}
	return 0;
}

static void phy_agm_lan8720_invoke_link_cb(const struct device *dev)
{
	struct phy_agm_lan8720_data *const data = dev->data;

	if (data->cb == NULL) {
		return;
	}
	data->cb(dev, &data->state, data->cb_data);
}

static DEVICE_API(ethphy, phy_agm_lan8720_api) = {
	.get_link = phy_agm_lan8720_get_link_state,
	.cfg_link = phy_agm_lan8720_cfg_link,
	.read = phy_agm_lan8720_read,
	.write = phy_agm_lan8720_write,
	.link_cb_set = phy_agm_lan8720_link_cb_set,
};

static int phy_agm_lan8720_init(const struct device *dev)
{
	const struct phy_agm_lan8720_config *const cfg = dev->config;
	struct phy_agm_lan8720_data *const data = dev->data;

	data->state.is_up = false;
	data->state.speed = 0U;

	if (!device_is_ready(cfg->mdio)) {
		LOG_ERR("PHY %d: MDIO bus %s not ready",
			cfg->phy_addr, cfg->mdio->name);
		return -ENODEV;
	}

	/* Everything that sleeps (GPIO reset hold, MII soft-reset poll, ID
	 * retries, autoneg kick-off) runs from the monitor work's first pass:
	 * doing it here kept the init thread -- and therefore every later
	 * POST_KERNEL device and main() -- waiting for the PHY (the cost
	 * and the before/after are in the file header of
	 * phy_agm_lan8720_probe()). */
	k_work_init_delayable(&data->monitor_work, phy_agm_lan8720_monitor_work);
	k_work_schedule(&data->monitor_work, K_NO_WAIT);
	return 0;
}

#define PHY_AGM_LAN8720_CONFIG(n)                                                                  \
	static const struct phy_agm_lan8720_config phy_agm_lan8720_cfg_##n = {                     \
		.phy_addr = DT_INST_REG_ADDR(n),                                                   \
		.default_speeds = PHY_INST_GENERATE_DEFAULT_SPEEDS(n),                             \
		.mdio = DEVICE_DT_GET(DT_INST_PARENT(n)),                                           \
		.gpio_reset = GPIO_DT_SPEC_INST_GET_OR(n, reset_gpios, {0}),                        \
	}

#define PHY_AGM_LAN8720_DATA(n)                                                                    \
	static struct phy_agm_lan8720_data phy_agm_lan8720_data_##n = {                            \
		.dev = DEVICE_DT_INST_GET(n),                                                      \
	}

#define PHY_AGM_LAN8720_DEVICE(n)                                                                  \
	PHY_AGM_LAN8720_CONFIG(n);                                                                  \
	PHY_AGM_LAN8720_DATA(n);                                                                    \
	DEVICE_DT_INST_DEFINE(n, phy_agm_lan8720_init, NULL,                                        \
			      &phy_agm_lan8720_data_##n,                                           \
			      &phy_agm_lan8720_cfg_##n, POST_KERNEL,                               \
			      CONFIG_PHY_INIT_PRIORITY, &phy_agm_lan8720_api);

DT_INST_FOREACH_STATUS_OKAY(PHY_AGM_LAN8720_DEVICE)
