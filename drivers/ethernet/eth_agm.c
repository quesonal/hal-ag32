/*
 * Copyright (c) 2026 AgRV Contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * AgRV2K MAC0 Ethernet driver.
 *
 * Plugs the on-die MAC0 block (0x41040000) into Zephyr's Ethernet
 * framework (NET_DEVICE_DT_INST_DEFINE + struct ethernet_api).
 * Mirrors eth_stellaris.c for shape; the data path follows the SDK's
 * framework-agrv_lwip MAC pattern (TX/RX descriptor tables at
 * 256-byte-aligned addresses, the MAC walks them via EN/INTEN).
 *
 * Hardware (AltaRiscv.h: AHB_MASK_MAC0, MAC_TypeDef):
 *   CTRL  [0] TX_EN, [1] RX_EN, [2] TX_INTEN, [3] RX_INTEN,
 *         [4] DUPLEX, [5] PROM, [6] RESET, [7] SPEED,
 *         [10] PHY_INTEN, [11] MULTICAST_EN, [16] RMII_MODE
 *   STAT  cleared by writing 1: [0] RX_ERR, [1] TX_ERR,
 *         [2] RX_INT, [3] TX_INT, [4] RX_AHBERR, [5] TX_AHBERR,
 *         [6] TOO_SMALL, [7] INV_ADDR, [8] PHY_STAT
 *   MACMSB/MACLSB  MAC address, big-endian pairs of bytes
 *   MDIO   hardware MDIO controller (separate driver)
 *   TXBASE/RXBASE  descriptor table base addresses
 *   HTMSB/HTLSB    multicast hash table (not used here)
 *
 * Descriptor format (AltaRiscv.h: MAC_DescriptorTypeDef):
 *   struct mac_desc { uint32_t ctrl; uint32_t addr; }
 *   Ctrl[10:0]  LENGTH (TX bytes to send, RX bytes received)
 *   Ctrl[11]    EN    -- MAC clears when done
 *   Ctrl[12]    WRAP  -- last descriptor in the ring
 *   Ctrl[13]    INTEN -- fire IRQ on completion
 *   TX errors [14:15]; RX errors [14:18] + MULTI_CAST[26]
 *
 * soc.c has already opened the AHB gate from the DT
 * `agm,ahb-clkenable-bit` (3 on AgRV2K) and pinctrl_apply_state()
 * has set AFSEL on the MAC pins from the eth0_default pinctrl state
 * before this driver runs.
 */

#define DT_DRV_COMPAT agm_agrv2k_emac

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/init.h>
#include <zephyr/net/net_pkt.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/ethernet.h>
/* Zephyr's ethernet.h does not always export ETH_FRAME_MIN_LEN; the
 * standard 60 B min payload (without FCS) is what we need for padding
 * runt frames to the IEEE 802.3 minimum. */
#ifndef ETH_FRAME_MIN_LEN
#define ETH_FRAME_MIN_LEN 60U
#endif
#include <zephyr/net/mii.h>
#include <zephyr/drivers/mdio.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/net/phy.h>
#include <zephyr/irq.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(eth_agm, CONFIG_ETHERNET_LOG_LEVEL);

#define TX_NUM_DESC CONFIG_ETH_AGM_TX_DESCRIPTORS
#define RX_NUM_DESC CONFIG_ETH_AGM_RX_DESCRIPTORS

#define RX_BUF_SIZE 1536U

/* MAC register layout, duplicated locally to avoid pulling in the
 * in-tree SDK headers. */
struct mac_regs {
	volatile uint32_t ctrl;
	volatile uint32_t stat;
	volatile uint32_t macmsb;
	volatile uint32_t maclsb;
	volatile uint32_t mdio;
	volatile uint32_t txbase;
	volatile uint32_t rxbase;
	volatile uint32_t reserved0;
	volatile uint32_t htmsb;
	volatile uint32_t htlsb;
};

#define MAC_CTRL_TX_EN         BIT(0)
#define MAC_CTRL_RX_EN         BIT(1)
#define MAC_CTRL_TX_INTEN      BIT(2)
#define MAC_CTRL_RX_INTEN      BIT(3)
#define MAC_CTRL_DUPLEX        BIT(4)
#define MAC_CTRL_PROM          BIT(5)
#define MAC_CTRL_RESET         BIT(6)
#define MAC_CTRL_SPEED         BIT(7)
#define MAC_CTRL_PHY_INTEN     BIT(10)
#define MAC_CTRL_MULTICAST_EN  BIT(11)
#define MAC_CTRL_RMII_MODE     BIT(16)

#define MAC_STAT_RX_ERR     BIT(0)
#define MAC_STAT_TX_ERR     BIT(1)
#define MAC_STAT_RX_INT     BIT(2)
#define MAC_STAT_TX_INT     BIT(3)
#define MAC_STAT_RX_AHBERR  BIT(4)
#define MAC_STAT_TX_AHBERR  BIT(5)
#define MAC_STAT_TOO_SMALL  BIT(6)
#define MAC_STAT_INV_ADDR   BIT(7)
#define MAC_STAT_PHY_STAT   BIT(8)
#define MAC_STAT_CLEAR_MASK 0x1ffU

#define MAC_DESC_LENGTH_MASK  0x7ffU
#define MAC_DESC_EN           BIT(11)
#define MAC_DESC_WRAP         BIT(12)
#define MAC_DESC_INTEN        BIT(13)

#define MAC_DESC_BITS         10
#define MAC_DESC_MIN_ALIGN    (1U << (MAC_DESC_BITS + 3 - 5))  /* 256 */

/* The SDK's MAC_GET_DESC_WRAP encoding: setting MAC_DESC_WRAP alone
 * is not enough -- the hardware also needs the upper 5 bits of the
 * ring base address in ctrl bits 27..31 to compute the wrap point.
 * Without them the engine walks past desc[N-1] into whatever memory
 * follows the ring: the MAC then reads "RX descriptors" out of
 * phy_agm_lan8720_data_0 (0x20000000) and
 * writing "frame data" to 0x80000af6 (XIP flash) -- the only symptom
 * on the wire was a single outgoing ARP and silence thereafter.
 */
#define MAC_DESC_WRAP_OFFSET  27U
static inline uint32_t mac_desc_wrap_ctrl(const void *ring)
{
	return MAC_DESC_WRAP
	     | ((((uint32_t)ring >> 8) & 0x1fU) << MAC_DESC_WRAP_OFFSET);
}

/* DMA descriptor shared with the MAC. ctrl is written by us and cleared
 * by the hardware, and addr is read by the engine, so both fields must
 * be volatile -- the EN poll in eth_agm_send() and the EN test in
 * eth_agm_rx_drain() are meaningless if the compiler may cache the
 * first value it read. */
struct mac_desc {
	volatile uint32_t ctrl;
	volatile uint32_t addr;
} __packed;

struct eth_agm_config {
	struct mac_regs *const base;
	const uint8_t mac_addr[6];
#if defined(CONFIG_ETH_PHY_DRIVER)
	/* The PHY whose link state drives this MAC's carrier (DT phy-handle).
	 * NULL when the board has no PHY node -- the net stack then keeps the
	 * carrier in whatever state the application reports. */
	const struct device *const phy;
#endif
	void (*irq_config_func)(const struct device *dev);
#if defined(CONFIG_NET_STATISTICS_ETHERNET)
	struct net_stats_eth *(*get_stats)(const struct device *dev,
					   struct net_if *iface);
#endif
#if defined(CONFIG_PINCTRL)
	const struct pinctrl_dev_config *pincfg;
#endif
};

struct eth_agm_data {
	struct net_if *iface;
	bool link_up;
	struct k_sem tx_sem;
	uint8_t tx_idx;
	uint8_t rx_idx;
	/* Halt recovery: when the MAC raises RX_AHBERR / TX_AHBERR the
	 * engine self-disables TX_EN+RX_EN and never restarts on its own.
	 * The ISR sets mac_halted and schedules reset_work; the workqueue
	 * does hw_reset + re-announce the rings + start() to bring the
	 * engine back.
	 *
	 * mac_halted is a real guard, not just a status flag: it is set when
	 * a recovery is queued, so the ISR and send() do not queue another
	 * one, and it stays set once recovery gives up -- that is what keeps
	 * a permanently broken bus from driving the workqueue. */
	bool mac_halted;
	struct k_work reset_work;
	/* Consecutive recovery runs that were NOT followed by evidence that
	 * the engine works (a completed TX, a drained RX frame, or a new
	 * link). After MAC_RESET_MAX_ATTEMPTS of them the driver gives up;
	 * any of those three events clears the counter, so a transient fault
	 * gets a fresh budget. (Previously the counter was zeroed at
	 * the end of every recovery run, which made the cap unreachable: the
	 * "give up" branch below was dead code. Caught by
	 * tests/drivers/ethernet/eth_agm.) */
	uint8_t reset_attempts;
	struct mac_desc tx_desc[TX_NUM_DESC] __aligned(MAC_DESC_MIN_ALIGN);
	struct mac_desc rx_desc[RX_NUM_DESC] __aligned(MAC_DESC_MIN_ALIGN);
	uint8_t tx_buf[TX_NUM_DESC][RX_BUF_SIZE] __aligned(4);
	uint8_t rx_buf[RX_NUM_DESC][RX_BUF_SIZE] __aligned(4);
#if defined(CONFIG_NET_STATISTICS_ETHERNET)
	struct net_stats_eth stats;
#endif
};

#define MAC_RESET_MAX_ATTEMPTS 5U

/* How long eth_agm_send() waits for a descriptor the engine still owns (i.e.
 * the ring is full), in 1 ms sleeps. The wait used to be a k_busy_wait() spin
 * of the same length, which held the CPU for ~100 ms without letting any
 * other thread run. */
#define ETH_AGM_TX_WAIT_MS 100U


static inline struct eth_agm_data *eth_agm_data_get(const struct device *dev)
{
	return dev->data;
}

static inline const struct eth_agm_config *eth_agm_cfg_get(const struct device *dev)
{
	return dev->config;
}

static void eth_agm_assign_mac(struct mac_regs *regs, const uint8_t *mac_addr)
{
	regs->macmsb = ((uint32_t)mac_addr[0] << 8) | mac_addr[1];
	regs->maclsb = ((uint32_t)mac_addr[2] << 24)
		     | ((uint32_t)mac_addr[3] << 16)
		     | ((uint32_t)mac_addr[4] << 8)
		     |  mac_addr[5];
}

static void eth_agm_init_descriptors(const struct device *dev)
{
	struct eth_agm_data *const data = eth_agm_data_get(dev);
	const struct eth_agm_config *const cfg = eth_agm_cfg_get(dev);
	struct mac_regs *const mac = cfg->base;
	uint8_t i;

	data->tx_idx = 0U;
	data->rx_idx = 0U;

	/* TX descriptors start cleared. The driver fills them on demand. */
	for (i = 0U; i < TX_NUM_DESC; i++) {
		data->tx_desc[i].ctrl = 0U;
		data->tx_desc[i].addr = (uint32_t)data->tx_buf[i];
	}

	/* RX descriptors: arm every slot with its buffer; set WRAP on
	 * the last entry. The MAC walks them in order. */
	for (i = 0U; i < RX_NUM_DESC; i++) {
		uint32_t ctrl = MAC_DESC_EN | MAC_DESC_INTEN;

		if (i == (RX_NUM_DESC - 1U)) {
			ctrl |= mac_desc_wrap_ctrl(data->rx_desc);
		}
		data->rx_desc[i].ctrl = ctrl;
		data->rx_desc[i].addr = (uint32_t)data->rx_buf[i];
	}

	mac->txbase = (uint32_t)data->tx_desc;
	mac->rxbase = (uint32_t)data->rx_desc;
}

static void eth_agm_hw_reset(const struct device *dev)
{
	const struct eth_agm_config *const cfg = eth_agm_cfg_get(dev);
	struct mac_regs *const mac = cfg->base;
	uint32_t loops = 1000U;

	mac->ctrl = MAC_CTRL_RESET;
	while ((mac->ctrl & MAC_CTRL_RESET) != 0U) {
		k_busy_wait(1);
		if (--loops == 0U) {
			break;
		}
	}
	/* After RESET self-clears, W1C the documented STAT bits.
	 * STAT[24]/[25] are undocumented and read back as 1 once the
	 * engine has taken a fault (dev board: they are set from the first
	 * TX onwards and are NOT cleared by writing 1s, by 0xffffffff, or by
	 * a RESET). They do not gate RX_INT or TX_INT,
	 * so nothing in this driver waits on them -- see eth_agm_isr(). */
	mac->stat = MAC_STAT_CLEAR_MASK;
}

static void eth_agm_start(const struct device *dev)
{
	const struct eth_agm_config *const cfg = eth_agm_cfg_get(dev);
	struct mac_regs *const mac = cfg->base;
	uint32_t ctrl = MAC_CTRL_RMII_MODE
		      | MAC_CTRL_TX_INTEN
		      | MAC_CTRL_RX_INTEN
		      | MAC_CTRL_PHY_INTEN
		      /* Accept all multicast (incl. broadcast FF:FF:FF:FF:FF:FF).
		       * Without this the MAC hardware filter drops broadcast
		       * frames at the PHY side, so a PC trying to ARP / ping /
		       * send any unsolicited traffic to the board never gets a
		       * reply (the unicast reply path is unaffected). The MAC
		       * IP on this chip has no hash-table filter wired up
		       * (HTMSB/HTLSB are 0), so accepting every multicast is
		       * the only way to keep mDNS / DHCP / ARP broadcast alive
		       * until someone wires HTMSB/HTLSB. */
		      | MAC_CTRL_MULTICAST_EN
		      | MAC_CTRL_TX_EN
		      | MAC_CTRL_RX_EN;

	if (eth_agm_data_get(dev)->link_up) {
		ctrl |= MAC_CTRL_DUPLEX | MAC_CTRL_SPEED;
	}

	mac->ctrl = ctrl;
}

static void eth_agm_stop(const struct device *dev)
{
	const struct eth_agm_config *const cfg = eth_agm_cfg_get(dev);
	struct mac_regs *const mac = cfg->base;

	mac->ctrl &= ~(MAC_CTRL_TX_EN | MAC_CTRL_RX_EN
		     | MAC_CTRL_TX_INTEN | MAC_CTRL_RX_INTEN);
	mac->stat = MAC_STAT_CLEAR_MASK;
}

static int eth_agm_send(const struct device *dev, struct net_pkt *pkt)
{
	struct eth_agm_data *const data = eth_agm_data_get(dev);
	const struct eth_agm_config *const cfg = eth_agm_cfg_get(dev);
	struct mac_regs *const mac = cfg->base;
	struct mac_desc *desc;
	uint8_t *buf;
	uint16_t length;
	uint8_t next_idx;
	uint32_t spin;
	int ret;

	if (!data->link_up) {
		return -ENETDOWN;
	}

	length = net_pkt_get_len(pkt);
	if (length == 0U || length > RX_BUF_SIZE) {
		return -EMSGSIZE;
	}

	next_idx = (data->tx_idx + 1U) % TX_NUM_DESC;
	desc = &data->tx_desc[data->tx_idx];

	/* Ring-full check. This is the slot we are about to overwrite: if
	 * the engine still owns it, we have TX_NUM_DESC frames in flight.
	 * Wait for that one frame rather than failing the send -- zperf
	 * aborts its run on any error and the net stack would just drop
	 * the packet, so a bounded wait here is the cheaper behaviour.
	 *
	 * Sleep on tx_sem (the TX interrupt gives it) instead of burning the
	 * CPU in a k_busy_wait() loop: the descriptor's EN bit stays the
	 * authority, tx_sem is only the wakeup, so a coalesced or dropped
	 * interrupt costs an extra 1 ms poll rather than a lost token. */
	for (spin = 0U; (desc->ctrl & MAC_DESC_EN) != 0U; spin++) {
		if (spin > ETH_AGM_TX_WAIT_MS) {
			/* Queue the recovery once. mac_halted stays set until the
			 * work has run (or recovery has given up), so a ring that
			 * keeps filling while the engine is wedged cannot pile up
			 * requests. */
			if (!data->mac_halted) {
				data->mac_halted = true;
				(void)k_work_submit(&data->reset_work);
			}
			return -EBUSY;
		}
		(void)k_sem_take(&data->tx_sem, K_MSEC(1));
	}

	buf = data->tx_buf[data->tx_idx];
	ret = net_pkt_read(pkt, buf, length);
	if (ret < 0) {
		return ret;
	}

	/* Pin runt frames (< 60 B payload) with zeros. The MAC IP on this
	 * chip auto-pins to 60 B but with a non-zero pattern (observed on
	 * dev board: 0x55) -- the receiving NIC accepts the frame, but a strict
	 * switch might not, and the standard requires zero-padded padding. */
	if (length < ETH_FRAME_MIN_LEN) {
		memset(buf + length, 0, ETH_FRAME_MIN_LEN - length);
		length = ETH_FRAME_MIN_LEN;
	}

	uint32_t ctrl = (uint32_t)(length & MAC_DESC_LENGTH_MASK)
		      | MAC_DESC_EN | MAC_DESC_INTEN;
	if (data->tx_idx == (TX_NUM_DESC - 1U)) {
		ctrl |= mac_desc_wrap_ctrl(data->tx_desc);
	}
	desc->ctrl = ctrl;

	/* Kick the engine. TX_EN is a self-clearing start bit: hardware
	 * drops it once the descriptor has been consumed (the vendor
	 * SDK waits for exactly that in MAC_WaitForTx()), so it must be
	 * re-asserted for every frame. */
	mac->ctrl |= MAC_CTRL_TX_EN;

	/* Do NOT wait for this frame to complete. The engine owns the
	 * descriptor now, and the next frame can be queued behind it --
	 * waiting here serializes TX with the MAC's transmission time
	 * (~230 us for a full frame on the dev board, which capped a UDP
	 * flood at 16.8 Mbit/s). The in-flight window is bounded by the
	 * ring-full check above, and tx_buf for this slot only gets
	 * rewritten after that descriptor's EN has been observed clear,
	 * so the payload stays valid until the engine is done with it.
	 */


	data->tx_idx = next_idx;
	return 0;
}

/* Halt recovery: runs in workqueue context (never in ISR), so we can
 * call hw_reset / start() and let the net stack retransmit whatever
 * was in flight. We deliberately do NOT call init_descriptors() here:
 * the driver-owned descriptor rings stay armed, and a destructive
 * re-init would clobber an in-flight TX (e.g. the ICMP echo the
 * board was sending) and lose it. The hardware just needs the base
 * pointers re-asserted (they survive RESET, but the engine's
 * internal state does not) and CTRL re-written with all EN bits.
 * The IRQ line stays enabled, so a follow-up AHB error would
 * re-schedule and bump reset_attempts; once we hit
 * MAC_RESET_MAX_ATTEMPTS consecutive runs with no working frame in
 * between, the work returns with mac_halted still set, the ISR/send()
 * stop queueing, and a permanent fault can no longer pin the
 * workqueue. A completed TX, a drained RX frame or a new link event
 * clears the counter and re-arms recovery.
 *
 * This path is only reached for TX/RX_AHBERR now. INV_ADDR (bit 7)
 * used to land here too, and that was the dev board bug:
 * this IP raises INV_ADDR twice on every single transmit, so the MAC
 * was being hw_reset() after each frame, which dropped CTRL.RX_EN and
 * cost ~500 ms of deafness per packet. See eth_agm_isr().
 *
 * The line `mac->ctrl |= ...` at the bottom is what re-fires RX_INT
 * when the next packet arrives -- TX_EN/RX_EN do not survive an
 * engine halt on their own. */
static void eth_agm_reset_work_fn(struct k_work *work)
{
	struct eth_agm_data *const data =
		CONTAINER_OF(work, struct eth_agm_data, reset_work);
	const struct device *dev = DEVICE_DT_INST_GET(0);
	const struct eth_agm_config *const cfg = eth_agm_cfg_get(dev);
	struct mac_regs *const mac = cfg->base;

	if (data->reset_attempts >= MAC_RESET_MAX_ATTEMPTS) {
		/* Leave mac_halted set: the ISR / send() then stop queuing
		 * recoveries, so a permanently broken bus cannot pin the
		 * workqueue. A completed frame or a new link event clears both
		 * the flag and the counter and re-arms recovery. */
		LOG_ERR("recovery: giving up after %u attempts without a working frame",
			data->reset_attempts);
		return;
	}
	data->reset_attempts++;

	LOG_WRN("MAC: halt recovery attempt %u", data->reset_attempts);

	eth_agm_hw_reset(dev);
	/* Re-point the engine at the rings. After hw_reset the engine's
	 * internal "next desc" pointer is back at 0, so it will pick up
	 * desc[0] of each ring on the first TX_INT / RX_INT. */
	mac->txbase = (uint32_t)data->tx_desc;
	mac->rxbase = (uint32_t)data->rx_desc;

	/* If the link is up, the carrier is fine, only the engine
	 * was wedged -- restart the engine. The PHY monitor work
	 * will re-fire set_link_state() if we end up taking the
	 * carrier down. */
	if (data->link_up) {
		eth_agm_start(dev);
		/* start() sets TX_EN and RX_EN; TX_EN is a self-clearing
		 * start bit and will drop on its own, RX_EN is the level
		 * enable the receiver needs. */
		mac->stat = MAC_STAT_CLEAR_MASK;
	}
	data->mac_halted = false;
	/* NOTE: reset_attempts is deliberately NOT cleared here. Clearing it
	 * at the end of every run is what made MAC_RESET_MAX_ATTEMPTS
	 * unreachable; it is cleared by evidence that the engine works (TX
	 * completion, drained RX frame) or by a new link, see
	 * eth_agm_isr_core() / eth_agm_set_link_state(). */
}

static struct net_pkt *eth_agm_rx_pkt(const struct device *dev,
				      struct net_if *iface, uint8_t idx, uint16_t length)
{
	struct eth_agm_data *const data = eth_agm_data_get(dev);
	struct net_pkt *pkt;
	int ret;

	if (length == 0U || length > RX_BUF_SIZE) {
		return NULL;
	}

	pkt = net_pkt_rx_alloc_with_buffer(iface, length,
					   NET_AF_UNSPEC, 0, K_NO_WAIT);
	if (pkt == NULL) {
		return NULL;
	}

	ret = net_pkt_write(pkt, data->rx_buf[idx], length);
	if (ret < 0) {
		net_pkt_unref(pkt);
		return NULL;
	}
	return pkt;
}

/*
 * Re-arm control word for an RX descriptor. The MAC clears EN on a
 * consumed descriptor and the driver sets EN|INTEN to make it ready
 * again -- but the wrap bit on the last slot must be preserved,
 * with the upper 5 address bits, otherwise the MAC walks past the
 * end of the ring into bogus addresses (observed on dev board as
 * STAT_INV_ADDR + STAT_RX_AHBERR flooding in the ISR after the
 * first frame).
 */
static uint32_t rx_rearm_ctrl(const void *ring, uint8_t idx)
{
	uint32_t ctrl = MAC_DESC_EN | MAC_DESC_INTEN;

	if (idx == (RX_NUM_DESC - 1U)) {
		ctrl |= mac_desc_wrap_ctrl(ring);
	}
	return ctrl;
}

/* Drain every descriptor the engine has filled. Returns how many frames were
 * handed to the net stack -- the ISR uses a non-zero return as evidence that
 * the engine is alive (and therefore as a reason to forget past recovery
 * attempts). */
static uint8_t eth_agm_rx_drain(const struct device *dev)
{
	struct eth_agm_data *const data = eth_agm_data_get(dev);
	struct net_if *iface = data->iface;
	uint8_t drained = 0U;
	uint8_t i;

	/* Walk every descriptor. EN cleared == engine returned a
	 * frame. Refill and advance. */
	for (i = 0U; i < RX_NUM_DESC; i++) {
		uint8_t idx = data->rx_idx;
		struct mac_desc *desc = &data->rx_desc[idx];
		uint16_t length;
		struct net_pkt *pkt;

		/* Current slot not yet consumed by the engine: stop
		 * here. The engine always processes descriptors in
		 * order, so if desc[rx_idx] is still EN=1 the higher
		 * slots cannot have been consumed either. `continue`
		 * would spin 4x on the same slot and silently drop
		 * desc[rx_idx+1..3] if they get filled in a burst. */
		if ((desc->ctrl & MAC_DESC_EN) != 0U) {
			break;
		}

		length = (uint16_t)(desc->ctrl & MAC_DESC_LENGTH_MASK);
		if (length == 0U) {
			/* Spurious -- re-arm. */
			desc->ctrl = rx_rearm_ctrl(data->rx_desc, idx);
			continue;
		}

		pkt = eth_agm_rx_pkt(dev, iface, idx, length);
		if (pkt == NULL) {
			LOG_ERR("rx pkt alloc failed");
			desc->ctrl = rx_rearm_ctrl(data->rx_desc, idx);
			continue;
		}

		if (net_recv_data(iface, pkt) < 0) {
			LOG_ERR("net_recv_data failed");
			net_pkt_unref(pkt);
		}

		desc->ctrl = rx_rearm_ctrl(data->rx_desc, idx);
		data->rx_idx = (data->rx_idx + 1U) % RX_NUM_DESC;
		drained++;
		/* No `stats.bytes.received += length` here, on purpose: the
		 * Ethernet L2 already accounts every frame it hands up, and it
		 * writes into *this* struct --
		 * ethernet_update_rx_stats() -> eth_stats_update_bytes_rx(iface)
		 * -> eth_stats_get_common() -> api->get_stats(dev) ->
		 * &data->stats. Counting the bytes here as well made net_stats
		 * report twice the received bytes (caught by
		 * tests/drivers/ethernet/eth_agm). Upstream drivers
		 * (eth_stellaris.c, eth_sam_gmac.c, ...) leave bytes/pkts to the
		 * L2 for the same reason; only the hardware error counters stay
		 * in the driver, because the L2 never sees the MAC's STAT bits.
		 */
	}

	return drained;
}

static void eth_agm_isr_core(const struct device *dev)
{
	struct eth_agm_data *const data = eth_agm_data_get(dev);
	const struct eth_agm_config *const cfg = eth_agm_cfg_get(dev);
	struct mac_regs *const mac = cfg->base;
	uint32_t stat = mac->stat;
	uint32_t mask;

	/* Clear exactly the bits we observed, like the vendor SDK
	 * (MAC_ClearStatus(mac, stat)). Writing back bits that were not
	 * set is not harmless here: STAT[24]/[25] latch on the first
	 * fault and read back as 1 forever, and 0xffffffff does not
	 * clear them either (dev board). */
	if (stat != 0U) {
		mac->stat = stat;
	}

	if ((stat & MAC_STAT_RX_INT) != 0U) {
		if (eth_agm_rx_drain(dev) > 0U) {
			/* A frame made it through: the engine works, so the
			 * recovery budget starts over (and the engine is by
			 * definition not halted any more). */
			data->reset_attempts = 0U;
			data->mac_halted = false;
		}
		/* Re-assert RX_EN after re-arming the descriptors. The
		 * vendor SDK does exactly this on every RX_INT
		 * (MAC_Receive() == arm the next descriptor +
		 * MAC_StartRx()). */
		mac->ctrl |= MAC_CTRL_RX_EN;
	}

	if ((stat & MAC_STAT_TX_INT) != 0U) {
		k_sem_give(&data->tx_sem);
		/* ... and a completed frame is the same kind of evidence. */
		data->reset_attempts = 0U;
		data->mac_halted = false;
	}

	if ((stat & MAC_STAT_PHY_STAT) != 0U) {
		/* PHY link state changed; the LAN8720 driver's monitor
		 * work will fire the link callback on its own poll. */
	}

	mask = stat & (MAC_STAT_TX_ERR | MAC_STAT_RX_ERR
		     | MAC_STAT_TX_AHBERR | MAC_STAT_RX_AHBERR
		     | MAC_STAT_TOO_SMALL | MAC_STAT_INV_ADDR);
	if (mask != 0U) {
#if defined(CONFIG_NET_STATISTICS_ETHERNET)
		if ((mask & MAC_STAT_TX_ERR) != 0U) {
			data->stats.errors.tx += 1U;
		}
		if ((mask & MAC_STAT_RX_ERR) != 0U) {
			data->stats.errors.rx += 1U;
		}
#endif
		/* Only a genuine AHB fault warrants a reset. INV_ADDR
		 * (bit 7) is raised twice on *every* transmit on this IP
		 * -- dev board: 40 pings produced tx=41 and
		 * inv=82 -- so resetting on it put the MAC through
		 * hw_reset() after each and every frame, which is what
		 * killed RX for ~500 ms at a time (and pinned the round
		 * trip at ~0.8 s). The vendor SDK ignores INV_ADDR too;
		 * it only ever looks at RX_INT and PHY_STAT. */
		if ((mask & (MAC_STAT_TX_AHBERR | MAC_STAT_RX_AHBERR)) != 0U) {
			/* One recovery at a time: mac_halted stays set until the
			 * work has run, or until recovery gives up (in which case
			 * a storm of AHB errors must not keep re-queueing it). */
			if (!data->mac_halted) {
				data->mac_halted = true;
				(void)k_work_submit(&data->reset_work);
			}
		}
	}
	(void)cfg;
}

/*
 * IRQ adapter: Zephyr's IRQ_CONNECT() hands the handler a `const void *`, and
 * the native/POSIX arch's ARCH_IRQ_CONNECT() passes it straight to
 * posix_isr_declare(void isr_p(const void *), ...) -- a `const struct device *`
 * parameter is a hard type mismatch there. Same pattern as uart_agm.c /
 * dma_agm.c / udc_agm.c; this driver was missed because
 * nothing compiled it for native_sim until tests/drivers/ethernet/eth_agm/.
 */
static void eth_agm_isr(const void *irq_arg)
{
	const struct device *dev = irq_arg;

	eth_agm_isr_core(dev);
}

void eth_agm_set_link_state(const struct device *dev, bool up)
{
	struct eth_agm_data *const data = eth_agm_data_get(dev);

	/* Init-order guard. CONFIG_PHY_INIT_PRIORITY and
	 * CONFIG_ETH_INIT_PRIORITY are both 60 on this module, so the
	 * PHY's device init (and with it the monitor work) can run
	 * before this driver's; and the net-iface init below runs even
	 * later. If the cable is already plugged, the first monitor
	 * poll fires a link-state callback BEFORE eth_agm_init() has
	 * published data->iface -- calling net_if_carrier_* with
	 * iface == NULL faults. Track the carrier ourselves so the
	 * transition is replayed when the iface finally shows up
	 * (eth_agm_init() does that, and subscribes to the PHY there
	 * too -- see the longer note in eth_agm_init()). */
	if (data->iface == NULL) {
		data->link_up = up;
		return;
	}

	if (data->link_up == up) {
		return;
	}
	data->link_up = up;
	if (up) {
		LOG_INF("link UP");
		/* A fresh link is a fresh start: forget the halt state and the
		 * recovery budget, so a re-plugged cable gets the full
		 * MAC_RESET_MAX_ATTEMPTS again. */
		data->mac_halted = false;
		data->reset_attempts = 0U;
		eth_agm_start(dev);
		net_if_carrier_on(data->iface);
	} else {
		LOG_INF("link DOWN");
		eth_agm_stop(dev);
		net_if_carrier_off(data->iface);
	}
}

#if defined(CONFIG_ETH_PHY_DRIVER)
/*
 * The PHY's link callback. Subscribing here (instead of leaving it to the
 * application) is what makes the net stack's carrier follow the cable on its
 * own: net_config_init() -- and anything else that waits for the interface to
 * become operational -- otherwise waits for an event that only the app could
 * send, which turns into a full CONFIG_NET_CONFIG_INIT_TIMEOUT stall when the
 * app itself is blocked in that wait -- an app that waits for the link from
 * inside net_config_init() deadlocks against itself. The PHY driver fires this
 * once on registration with
 * the current state, which may be before this driver's iface exists --
 * eth_agm_set_link_state() stashes it and eth_agm_init() replays it.
 */
static void eth_agm_phy_link_changed(const struct device *phy,
				     struct phy_link_state *state, void *user_data)
{
	ARG_UNUSED(phy);

	eth_agm_set_link_state((const struct device *)user_data, state->is_up);
}
#endif /* CONFIG_ETH_PHY_DRIVER */

static void eth_agm_init(struct net_if *iface)
{
	const struct device *dev = net_if_get_device(iface);
	const struct eth_agm_config *const cfg = eth_agm_cfg_get(dev);
	struct eth_agm_data *const data = eth_agm_data_get(dev);
	bool phy_authoritative = false;
	bool replay;

	/* Order matters: the L2 context is initialised first and
	 * data->iface is published last. A PHY link callback runs on the PHY's
	 * monitor work and can land anywhere in here; while data->iface is NULL it
	 * only stashes the state, so net_if_carrier_on/off() is never pointed at
	 * an interface whose ethernet_init() has not finished. */
	net_if_set_link_addr(iface, cfg->mac_addr, 6, NET_LINK_ETHERNET);
	ethernet_init(iface);

	/* Zephyr's NET_IF_INIT() / NET_DEVICE_INIT() create every interface with
	 * NET_IF_LOWER_UP already set (include/zephyr/net/net_if.h), i.e. carrier
	 * reported "on" before the PHY has said anything. Upstream Ethernet
	 * drivers clear it in their iface init (28 of them call
	 * net_if_carrier_off()); this driver did not, so between boot and the
	 * PHY's first link event the stack advertised a working link while the
	 * MAC was not even started (data->link_up == false => send() answers
	 * -ENETDOWN) -- the stack advertised a working link from boot until
	 * the first link event. */
	net_if_carrier_off(iface);

	/* Everything a link callback saw while there was no iface to publish it
	 * on. Cleared before the iface is published so the state transition fires
	 * exactly once. */
	replay = data->link_up;
	data->link_up = false;
	data->iface = iface;

#if defined(CONFIG_ETH_PHY_DRIVER)
	/* Follow the PHY's link state (see eth_agm_phy_link_changed). This runs
	 * from net_if_init() -- i.e. after *every* device init -- on purpose:
	 * with CONFIG_ETH_INIT_PRIORITY == CONFIG_PHY_INIT_PRIORITY (both 60 on
	 * this module) the MAC's device init can run before the PHY's, so a
	 * subscription there would silently see "PHY not ready" -- the MAC
	 * warns long before the PHY registers. Here the PHY is up and the
	 * callback doubles as the initial
	 * state: it fires once on registration. */
	if (cfg->phy != NULL) {
		if (device_is_ready(cfg->phy)) {
			(void)phy_link_callback_set(cfg->phy,
						    eth_agm_phy_link_changed, (void *)dev);
			/* The subscription just reported the *current* state, so the
			 * stash above is stale by definition and is not replayed. */
			phy_authoritative = true;
		} else {
			LOG_WRN("PHY %s not ready -- carrier stays with the application",
				cfg->phy->name);
		}
	}
#endif

	/* Replay the link state captured during the pre-init window (see
	 * eth_agm_set_link_state -- the guard stashed data->link_up while iface
	 * was NULL), unless a PHY subscription has just told us what the link
	 * does *now*. Safe to call here: net_if_carrier_* is wired up by
	 * ethernet_init() above. */
	if (replay && !phy_authoritative) {
		eth_agm_set_link_state(dev, true);
	}
}

#if defined(CONFIG_NET_STATISTICS_ETHERNET)
static struct net_stats_eth *eth_agm_stats(const struct device *dev,
					   struct net_if *iface)
{
	return &eth_agm_data_get(dev)->stats;
}
#endif

static int eth_agm_dev_init(const struct device *dev)
{
	struct eth_agm_data *const data = eth_agm_data_get(dev);
	const struct eth_agm_config *const cfg = eth_agm_cfg_get(dev);
	struct mac_regs *const mac = cfg->base;
	int ret;

#if defined(CONFIG_PINCTRL)
	/* Apply the eth0_default pinctrl state (AFSEL/DIR on the MAC pins).
	 * soc.c opened the GPIO bank APB gates at PRE_KERNEL_1 from the
	 * same state, so the writes land -- without this call the pins
	 * stay GPIO-mode and the MAC IP talks to nothing. */
	ret = pinctrl_apply_state(cfg->pincfg, PINCTRL_STATE_DEFAULT);
	if (ret < 0) {
		LOG_ERR("eth_agm: pinctrl_apply_state failed: %d", ret);
		return ret;
	}
#endif

	k_sem_init(&data->tx_sem, 0, 1);
	k_work_init(&data->reset_work, eth_agm_reset_work_fn);

	eth_agm_hw_reset(dev);
	eth_agm_assign_mac(mac, cfg->mac_addr);
	eth_agm_init_descriptors(dev);

	/* Clear any STAT from the boot path / a previous reset. */
	mac->stat = MAC_STAT_CLEAR_MASK;

	data->link_up = false;

	/* Configure the IRQ last -- the ISR runs at PLIC priority and
	 * signals tx_sem; it must not fire until the descriptor rings
	 * are programmed. */
	cfg->irq_config_func(dev);

	return 0;
}

static void eth_agm_irq_config_func(const struct device *dev)
{
	IRQ_CONNECT(DT_INST_IRQN(0),
		    DT_INST_IRQ(0, priority),
		    eth_agm_isr, DEVICE_DT_INST_GET(0), 0);
	irq_enable(DT_INST_IRQN(0));
}

static const struct ethernet_api eth_agm_apis = {
	.iface_api.init = eth_agm_init,
	.send = eth_agm_send,
#if defined(CONFIG_NET_STATISTICS_ETHERNET)
	.get_stats = eth_agm_stats,
#endif
};

#if defined(CONFIG_PINCTRL)
#define ETH_AGM_PINCTRL_DEFINE(inst) PINCTRL_DT_INST_DEFINE(inst);
#define ETH_AGM_PINCTRL_INIT(inst) .pincfg = PINCTRL_DT_INST_DEV_CONFIG_GET(inst),
#else
#define ETH_AGM_PINCTRL_DEFINE(inst)
#define ETH_AGM_PINCTRL_INIT(inst)
#endif

#if defined(CONFIG_ETH_PHY_DRIVER)
#define ETH_AGM_PHY_INIT(inst)                                                                 \
	.phy = DEVICE_DT_GET_OR_NULL(DT_INST_PHANDLE(inst, phy_handle)),
#else
#define ETH_AGM_PHY_INIT(inst)
#endif

#define ETH_AGM_CONFIG(inst)                                                                   \
	static const struct eth_agm_config eth_agm_cfg_##inst = {                             \
		.base = (struct mac_regs *)DT_INST_REG_ADDR(inst),                             \
		.mac_addr = DT_INST_PROP(inst, local_mac_address),                             \
		.irq_config_func = eth_agm_irq_config_func,                                    \
		ETH_AGM_PINCTRL_INIT(inst)                                                     \
		ETH_AGM_PHY_INIT(inst)                                                         \
	}

#define ETH_AGM_DEVICE(inst)                                                                   \
	ETH_AGM_PINCTRL_DEFINE(inst);                                                         \
	ETH_AGM_CONFIG(inst);                                                                  \
	static struct eth_agm_data eth_agm_data_##inst;                                        \
	NET_DEVICE_DT_INST_DEFINE(inst,                                                         \
				  eth_agm_dev_init, NULL,                                       \
				  &eth_agm_data_##inst, &eth_agm_cfg_##inst,                    \
				  CONFIG_ETH_INIT_PRIORITY, &eth_agm_apis,                     \
				  ETHERNET_L2,                                                 \
				  NET_L2_GET_CTX_TYPE(ETHERNET_L2), NET_ETH_MTU);

DT_INST_FOREACH_STATUS_OKAY(ETH_AGM_DEVICE)
