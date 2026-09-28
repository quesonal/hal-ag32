/*
 * Copyright (c) 2026 AgRV Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief native_sim suite for drivers/ethernet/eth_agm.c.
 *
 * Three things this suite has to supply, none of which native_sim has out of
 * the box:
 *
 *  1. **The register window.** tests/drivers/common/agm_native/
 *     agm_fake_mmio.c maps 0x40000000+32 MiB with mmap(MAP_FIXED) before any
 *     device init, so the unmodified driver reaches RAM at the real MAC address
 *     (0x41040000, kept in the overlay).
 *
 *  2. **The MAC engine.** The only register the driver *waits* on is CTRL.RESET
 *     (cleared by hardware once the reset finished) -- `eth_agm_hw_reset()`
 *     polls it with k_busy_wait(), and native_sim only advances simulated time
 *     inside a busy wait. A 1 ms k_timer started at POST_KERNEL (before
 *     CONFIG_ETH_INIT_PRIORITY) plays that one bit. Everything else the driver
 *     does is fire-and-forget by design: TX is handed to the engine and
 *     completed by an interrupt the test raises, RX is whatever the test puts
 *     in a descriptor.
 *
 *  3. **The stack around it.** The driver is a NET_DEVICE_DT_INST_DEFINE()
 *     citizen, so the suite carries a minimal L2-Ethernet stack and observes
 *     the RX path the way a real user would: through an AF_PACKET socket.
 *
 * What that covers, in the order of the review findings it touches:
 *  - init: hw reset handshake, MAC address programming, TX/RX ring setup with
 *    the WRAP encoding that needs the ring's upper address bits (the
 *    "MAC walks past the ring" bug we found), and the ETH pinctrl state;
 *  - set_link_state(): the CTRL bits a link-up sets (RMII, both EN bits, both
 *    interrupt enables, MULTICAST_EN, duplex/speed) and the carrier callbacks;
 *  - send(): descriptor arming, the payload the engine would read, the
 *    zero-padding of runt frames, and the -ENETDOWN / -EMSGSIZE gates;
 *  - the ring-full path: send() waits on tx_sem instead of spinning, so
 *    the suite fills all four descriptors, gets -EBUSY, and then checks that a
 *    TX interrupt + the scheduled reset work put the engine back in service;
 *  - the ISR: RX_INT drains the ring into the net stack (frame comes out of a
 *    packet socket byte-for-byte, descriptors re-armed, CTRL.RX_EN re-asserted,
 *    driver stats updated), TX_INT releases tx_sem, and an AHB error schedules
 *    the halt-recovery work while INV_ADDR deliberately does not.
 *  - the halt-recovery budget: MAC_RESET_MAX_ATTEMPTS consecutive
 *    unattended faults are recovered, the next one gives up, and a completed
 *    TX or a new link event re-arms it; and the carrier starts *off* until the
 *    PHY reports a link.
 *
 * Address note: the MAC is a 32-bit bus master, so the driver stores
 * `(uint32_t)pointer` in the descriptors and in TXBASE/RXBASE. On a 64-bit
 * native build that truncates. The driver's descriptors and buffers all live in
 * one static object, so the missing high bits are exactly the ones of
 * dev->data -- agm_host_addr() re-attaches them, which is how the test reaches
 * the same memory the driver programmed.
 */

#include <zephyr/arch/posix/posix_soc_if.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/net/ethernet.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_ip.h>
#include <zephyr/net/net_pkt.h>
#include <zephyr/net/socket.h>
#include <zephyr/ztest.h>

#include <errno.h>
#include <string.h>

#define ETH_DEV  DEVICE_DT_GET(DT_NODELABEL(eth_agm0))

#define MAC_BASE   0x41040000UL
#define ETH_IRQ    12U

/* struct mac_regs (drivers/ethernet/eth_agm.c). */
#define R_CTRL   0x00U
#define R_STAT   0x04U
#define R_MACMSB 0x08U
#define R_MACLSB 0x0cU
#define R_TXBASE 0x14U
#define R_RXBASE 0x18U

/* CTRL bits. */
#define CTRL_TX_EN       BIT(0)
#define CTRL_RX_EN       BIT(1)
#define CTRL_TX_INTEN    BIT(2)
#define CTRL_RX_INTEN    BIT(3)
#define CTRL_DUPLEX      BIT(4)
#define CTRL_PROM        BIT(5)
#define CTRL_RESET       BIT(6)
#define CTRL_SPEED       BIT(7)
#define CTRL_PHY_INTEN   BIT(10)
#define CTRL_MULTICAST   BIT(11)
#define CTRL_RMII_MODE   BIT(16)

/* STAT bits (W1C in hardware; plain RAM in this model). */
#define STAT_RX_ERR    BIT(0)
#define STAT_TX_ERR    BIT(1)
#define STAT_RX_INT    BIT(2)
#define STAT_TX_INT    BIT(3)
#define STAT_RX_AHBERR BIT(4)
#define STAT_TX_AHBERR BIT(5)
#define STAT_TOO_SMALL BIT(6)
#define STAT_INV_ADDR  BIT(7)
#define STAT_PHY_STAT  BIT(8)

/* The mask eth_agm_hw_reset() writes into STAT (drivers/ethernet/eth_agm.c).
 * In this RAM model that store *sets* exactly these bits, which makes it a
 * probe for "the recovery work ran": only reset_work_fn() reaches hw_reset(). */
#define STAT_CLEAR_MASK 0x1ffU

/* Descriptor control word. */
#define DESC_LENGTH_MASK 0x7ffU
#define DESC_EN          BIT(11)
#define DESC_WRAP        BIT(12)
#define DESC_INTEN       BIT(13)
#define DESC_WRAP_SHIFT  27U

#define ETH_TX_DESC CONFIG_ETH_AGM_TX_DESCRIPTORS
#define ETH_RX_DESC CONFIG_ETH_AGM_RX_DESCRIPTORS

/* drivers/ethernet/eth_agm.c's halt-recovery budget. Duplicated on purpose: if
 * the driver's number moves, the test that pins the "give up" behaviour should
 * be the one that notices. */
#define MAC_RESET_MAX_ATTEMPTS 5U

/* The suite's own MAC address: must match local-mac-address in the overlay. */
static const uint8_t suite_mac[6] = { 0x02U, 0x00U, 0x5aU, 0x5aU, 0x00U, 0x01U };

/* GPIO banks the ETH pinctrl state touches. */
#define GPIO0_BASE     0x40014000UL
#define GPIO_STRIDE    0x1000UL
#define GPIO_DIR_OFF   0x400UL
#define GPIO_AFSEL_OFF 0x420UL

struct mac_desc {
	uint32_t ctrl;
	uint32_t addr;
} __packed;

extern void eth_agm_set_link_state(const struct device *dev, bool up);

static uint32_t rd(uint32_t off)
{
	return *(volatile uint32_t *)(MAC_BASE + off);
}

static void wr(uint32_t off, uint32_t val)
{
	*(volatile uint32_t *)(MAC_BASE + off) = val;
}

static uint32_t gpio_rd(uint32_t bank, uint32_t off)
{
	return *(volatile uint32_t *)(GPIO0_BASE + (bank * GPIO_STRIDE) + off);
}

/* See the file header: re-attach the high bits the 32-bit descriptor cannot carry. */
static uintptr_t agm_host_addr(uint32_t hw_addr)
{
	uintptr_t hi = (uintptr_t)ETH_DEV->data & ~(uintptr_t)0xffffffffULL;

	return hi | (uintptr_t)hw_addr;
}

static struct mac_desc *tx_ring(void)
{
	return (struct mac_desc *)agm_host_addr(rd(R_TXBASE));
}

static struct mac_desc *rx_ring(void)
{
	return (struct mac_desc *)agm_host_addr(rd(R_RXBASE));
}

static uint8_t *desc_buf(const struct mac_desc *desc)
{
	return (uint8_t *)agm_host_addr(desc->addr);
}

/* ---- the MAC engine model -------------------------------------------- */

static struct k_timer mac_engine_timer;

/* CTRL.RESET is write-1-to-start, self-clearing when the MAC finishes its
 * reset. This is the bit eth_agm_hw_reset() polls. */
static void mac_engine_tick(struct k_timer *timer)
{
	uint32_t ctrl = rd(R_CTRL);

	ARG_UNUSED(timer);

	if ((ctrl & CTRL_RESET) != 0U) {
		wr(R_CTRL, ctrl & ~CTRL_RESET);
	}
}

/* Priority 0 < CONFIG_ETH_INIT_PRIORITY, so the model is live before the
 * driver's own POST_KERNEL init runs its first reset. */
static int mac_engine_start(void)
{
	k_timer_init(&mac_engine_timer, mac_engine_tick, NULL);
	/* One tick, not one millisecond: eth_agm_hw_reset() gives the model only
	 * ~1000 x k_busy_wait(1) = 1 ms before it gives up, so the tick rate in
	 * prj.conf (10 kHz -> 100 us) is what makes the reset handshake work
	 * at all. If this model stops clearing CTRL.RESET in time, test_01 says
	 * so first. */
	k_timer_start(&mac_engine_timer, K_TICKS(1), K_TICKS(1));
	return 0;
}

SYS_INIT(mac_engine_start, POST_KERNEL, 0);

/* ---- playing the engine ---------------------------------------------- */

/* Pend the MAC's interrupt line and let the CPU take it: the handler really is
 * reached through the driver's IRQ_CONNECT(), like on the board. */
static void trigger_mac_irq(void)
{
	posix_sw_set_pending_IRQ(ETH_IRQ);
	k_sleep(K_MSEC(5));
}

/* The engine took every queued TX descriptor (it clears EN and writes the
 * length back) and raised TX_INT. */
static void model_complete_tx(void)
{
	for (uint8_t i = 0U; i < ETH_TX_DESC; i++) {
		tx_ring()[i].ctrl &= ~DESC_EN;
	}
	wr(R_STAT, STAT_TX_INT);
	trigger_mac_irq();
}

/* The engine received a frame into rx slot `idx` and raised RX_INT (plus
 * PHY_STAT, as it does in bursts on hardware). */
static void model_inject_rx(uint8_t idx, const uint8_t *frame, uint16_t length)
{
	struct mac_desc *desc = &rx_ring()[idx];

	memcpy(desc_buf(desc), frame, length);
	desc->ctrl = (uint32_t)length; /* EN cleared: the engine filled this slot */
	wr(R_STAT, STAT_RX_INT | STAT_PHY_STAT);
	trigger_mac_irq();
}

/* ---- helpers --------------------------------------------------------- */

static struct net_if *eth_iface(void)
{
	struct net_if *iface = net_if_lookup_by_dev(ETH_DEV);

	zassert_not_null(iface, "the MAC registered a net_if");
	return iface;
}

static const struct ethernet_api *eth_api(void)
{
	const struct ethernet_api *api = (const struct ethernet_api *)ETH_DEV->api;

	zassert_not_null(api, "the MAC exposes an ethernet_api");
	return api;
}

static struct net_pkt *pkt_with(struct net_if *iface, const uint8_t *data, size_t len)
{
	struct net_pkt *pkt = net_pkt_alloc_with_buffer(iface, len, NET_AF_UNSPEC, 0, K_NO_WAIT);

	zassert_not_null(pkt, "net_pkt alloc (%zu B)", len);
	if (len != 0U) {
		int ret = net_pkt_write(pkt, data, len);

		zassert_ok(ret, "pkt write of %zu B (err %d)", len, ret);
		/* The L2 resets the cursor before handing the packet to the driver
		 * (ethernet.c: net_pkt_cursor_init(pkt)) because eth_agm_send()
		 * reads the payload with net_pkt_read() -- which starts where the
		 * cursor is, i.e. at the end right after net_pkt_write(). */
		net_pkt_cursor_init(pkt);
	}
	return pkt;
}

/* A 60-byte Ethernet frame: two MACs, an ethertype and a payload pattern. */
static void build_frame(uint8_t *frame, uint8_t seed)
{
	memset(frame, 0, 60);
	memcpy(&frame[0], suite_mac, 6);
	frame[6] = 0x02U;
	frame[7] = 0x11U;
	frame[8] = 0x22U;
	frame[9] = 0x33U;
	frame[10] = 0x44U;
	frame[11] = 0x55U;
	frame[12] = 0x88U; /* "experimental" ethertype: no IP stack will eat it */
	frame[13] = 0xb5U;
	for (int i = 14; i < 60; i++) {
		frame[i] = (uint8_t)(seed + i);
	}
}

/* ---- suite hooks ----------------------------------------------------- */

static void suite_before(void *fixture)
{
	ARG_UNUSED(fixture);

	/* The test owns the volatile half of the register file. CTRL and the
	 * MAC address / ring-base registers are the driver's init output and are
	 * asserted by test_01. */
	wr(R_STAT, 0U);

	/* The engine finished everything it was given: clear EN on every TX
	 * descriptor, so each case starts with an empty ring. That is the one
	 * piece of engine state the *model* owns (the driver only ever sets EN). */
	for (uint8_t i = 0U; i < ETH_TX_DESC; i++) {
		tx_ring()[i].ctrl &= ~DESC_EN;
	}

	/* Every case starts from a stopped, carrier-down MAC. */
	eth_agm_set_link_state(ETH_DEV, false);
}

ZTEST_SUITE(eth_agm, NULL, NULL, suite_before, NULL, NULL);

/* ---- init, registers, pinctrl ---------------------------------------- */

ZTEST(eth_agm, test_01_init_programs_the_mac_and_both_rings)
{
	struct net_linkaddr *ll;
	const struct mac_desc *rx = rx_ring();

	zassert_true(device_is_ready(ETH_DEV), "the MAC came up against the fake window");

	/* hw_reset() wrote CTRL.RESET and waited for the model to clear it (see
	 * the file header); nothing else was written to CTRL at init (the engine
	 * is only started on link up). */
	zassert_equal(rd(R_CTRL), 0U, "reset self-cleared, engine idle (CTRL=0x%08x)", rd(R_CTRL));
	zassert_equal(rd(R_STAT), 0U, "boot-time status was cleared (STAT=0x%08x)", rd(R_STAT));

	/* MAC address, two big-endian byte pairs. */
	zassert_equal(rd(R_MACMSB), 0x0200U, "MACMSB holds bytes 0..1");
	zassert_equal(rd(R_MACLSB), 0x5a5a0001U, "MACLSB holds bytes 2..5");

	/* The net_if got the same address through net_if_set_link_addr(). */
	ll = net_if_get_link_addr(eth_iface());
	zassert_equal(ll->len, 6U, "link address length");
	zassert_mem_equal(ll->addr, suite_mac, 6U, "link address");
	zassert_equal(ll->type, NET_LINK_ETHERNET, "link type");

	/* Both rings are 256-byte aligned and announced to the engine. */
	zassert_not_equal(rd(R_TXBASE), 0U, "TXBASE programmed");
	zassert_not_equal(rd(R_RXBASE), 0U, "RXBASE programmed");
	zassert_equal(rd(R_TXBASE) & 0xffU, 0U, "TX ring is 256 B aligned");
	zassert_equal(rd(R_RXBASE) & 0xffU, 0U, "RX ring is 256 B aligned");

	/* TX descriptors start cleared (the driver fills them on demand). */
	for (uint8_t i = 0U; i < ETH_TX_DESC; i++) {
		zassert_equal(tx_ring()[i].ctrl, 0U, "TX desc[%u] starts idle", i);
		zassert_not_equal(tx_ring()[i].addr, 0U, "TX desc[%u] has a buffer", i);
	}

	/* RX descriptors are armed for the engine, and the last one carries the
	 * WRAP encoding -- which needs the upper 5 bits of the *ring* address in
	 * ctrl[31:27], not just the WRAP bit (earlier: without them the MAC
	 * walked past the ring into phy_agm_lan8720's data). */
	for (uint8_t i = 0U; i < ETH_RX_DESC; i++) {
		uint32_t expected = DESC_EN | DESC_INTEN;

		if (i == (ETH_RX_DESC - 1U)) {
			expected |= DESC_WRAP | ((((uint32_t)rx >> 8) & 0x1fU) << DESC_WRAP_SHIFT);
		}
		zassert_equal(rx[i].ctrl, expected, "RX desc[%u] armed (0x%08x)", i, rx[i].ctrl);
		zassert_not_equal(rx[i].addr, 0U, "RX desc[%u] has a buffer", i);
	}
	zassert_equal(tx_ring()[0].addr & 0x3U, 0U, "TX buffers are 4-byte aligned");

	/* Zephyr's NET_IF_INIT() creates the interface with
	 * NET_IF_LOWER_UP already set (carrier "on"), so a driver that does
	 * nothing reports a working link before the PHY has said a word --
	 * carrier stays on from boot until the first link event (2.0 s).
	 * eth_agm_init() now clears it; this is the first
	 * case to run, i.e. the state no link event has touched yet. */
	zassert_false(net_if_is_carrier_ok(eth_iface()),
		      "the carrier is off until the PHY reports a link");

	/* The generated ETH pinctrl state, decoded by the real
	 * pinctrl_configure_pins(): AFSEL on all ten cells, DIR only on the ones
	 * the generator marked OUTPUT. The last two cells are the bidirectional
	 * MDIO pin written OUTPUT then INPUT, so the later cell wins and DIR ends
	 * cleared -- that ordering in dts/riscv/agm/pinctrl-devmac.dtsi is
	 * load-bearing, so it is pinned here rather than assumed. */
	zassert_equal(gpio_rd(7U, GPIO_AFSEL_OFF), BIT(7) | BIT(5), "GPIO7 AFSEL (RXD0/TX_CLK)");
	zassert_equal(gpio_rd(8U, GPIO_AFSEL_OFF), BIT(0) | BIT(5), "GPIO8 AFSEL (RXD1/CRS)");
	zassert_equal(gpio_rd(9U, GPIO_AFSEL_OFF), BIT(1) | BIT(2) | BIT(5) | BIT(7),
		      "GPIO9 AFSEL (TXD0/TXD1/TX_EN/MDC)");
	zassert_equal(gpio_rd(4U, GPIO_AFSEL_OFF), BIT(0), "GPIO4 AFSEL (MDIO)");
	zassert_equal(gpio_rd(9U, GPIO_DIR_OFF), BIT(1) | BIT(2) | BIT(5) | BIT(7),
		      "GPIO9 DIR: the TX-side pins drive");
	zassert_equal(gpio_rd(7U, GPIO_DIR_OFF), 0U, "GPIO7 DIR: NO_DIR cells are left alone");
	zassert_equal(gpio_rd(8U, GPIO_DIR_OFF), 0U, "GPIO8 DIR: NO_DIR cells are left alone");
	zassert_equal(gpio_rd(4U, GPIO_DIR_OFF), 0U, "GPIO4 DIR: the MDIO INPUT cell wins");
}

/* ---- link state ------------------------------------------------------- */

ZTEST(eth_agm, test_02_link_state_programs_ctrl_and_drives_the_carrier)
{
	struct net_if *iface = eth_iface();
	const uint32_t start_bits = CTRL_RMII_MODE | CTRL_TX_INTEN | CTRL_RX_INTEN
				  | CTRL_PHY_INTEN | CTRL_MULTICAST | CTRL_TX_EN | CTRL_RX_EN;
	const uint32_t link_bits = CTRL_DUPLEX | CTRL_SPEED;

	eth_agm_set_link_state(ETH_DEV, true);
	zassert_equal(rd(R_CTRL), start_bits | link_bits,
		      "link up enables the engine (CTRL=0x%08x)", rd(R_CTRL));
	zassert_true(net_if_is_carrier_ok(iface), "carrier on");

	/* Re-reporting the same state is a no-op, not a second start. */
	wr(R_CTRL, 0U);
	eth_agm_set_link_state(ETH_DEV, true);
	zassert_equal(rd(R_CTRL), 0U, "the same link state does not re-program CTRL");

	eth_agm_set_link_state(ETH_DEV, false);
	zassert_equal(rd(R_CTRL) & (CTRL_TX_EN | CTRL_RX_EN | CTRL_TX_INTEN | CTRL_RX_INTEN), 0U,
		      "link down stops the engine");
	/* (stop() also W1C-clears STAT. A 32-bit store cannot express that here --
	 * writing 0x1ff into a RAM cell *sets* those bits -- so this model owns
	 * STAT and the suite sets it explicitly where it matters.) */
	zassert_false(net_if_is_carrier_ok(iface), "carrier off");

	eth_agm_set_link_state(ETH_DEV, true);
	zassert_equal(rd(R_CTRL), start_bits | link_bits, "re-negotiated link starts the engine again");
	zassert_true(net_if_is_carrier_ok(iface), "carrier back on");
}

/* ---- transmit -------------------------------------------------------- */

ZTEST(eth_agm, test_03_send_arms_the_descriptor_with_the_frame)
{
	const struct ethernet_api *api = eth_api();
	struct net_if *iface = eth_iface();
	uint8_t frame[60];
	struct net_pkt *pkt;

	eth_agm_set_link_state(ETH_DEV, true);
	build_frame(frame, 0x40U);

	pkt = pkt_with(iface, frame, sizeof(frame));
	zassert_ok(api->send(ETH_DEV, pkt), "send() accepted the frame");
	net_pkt_unref(pkt);

	/* The descriptor now belongs to the engine: length, enable, interrupt. */
	zassert_equal(tx_ring()[0].ctrl, sizeof(frame) | DESC_EN | DESC_INTEN,
		      "TX desc[0] armed (0x%08x)", tx_ring()[0].ctrl);
	zassert_mem_equal(desc_buf(&tx_ring()[0]), frame, sizeof(frame),
			  "the engine's buffer holds the frame");

	/* TX_EN is re-asserted for every frame (self-clearing start bit). */
	zassert_equal(rd(R_CTRL) & CTRL_TX_EN, CTRL_TX_EN, "TX_EN re-asserted");

	/* A runt frame is zero-padded to the 802.3 minimum, because the MAC's own
	 * padding is a non-zero pattern (0x55). */
	memset(frame, 0x11, sizeof(frame));
	pkt = pkt_with(iface, frame, 14U);
	zassert_ok(api->send(ETH_DEV, pkt), "send() accepted the short frame");
	net_pkt_unref(pkt);

	zassert_equal(tx_ring()[1].ctrl, 60U | DESC_EN | DESC_INTEN,
		      "runt frame went out padded to 60 B");
	zassert_mem_equal(desc_buf(&tx_ring()[1]), frame, 14U, "payload copied");
	for (uint8_t i = 14U; i < 60U; i++) {
		zassert_equal(desc_buf(&tx_ring()[1])[i], 0U, "padding byte %u is zero", i);
	}
}

ZTEST(eth_agm, test_04_ring_full_waits_then_recovers_through_the_reset_work)
{
	const struct ethernet_api *api = eth_api();
	struct net_if *iface = eth_iface();
	uint8_t frame[60];
	uint32_t txbase_before;
	uint32_t rxbase_before;

	eth_agm_set_link_state(ETH_DEV, true);
	build_frame(frame, 0x80U);
	txbase_before = rd(R_TXBASE);
	rxbase_before = rd(R_RXBASE);

	/* Fill the ring: four sends, four descriptors handed to the engine. */
	for (uint8_t i = 0U; i < ETH_TX_DESC; i++) {
		struct net_pkt *pkt = pkt_with(iface, frame, sizeof(frame));

		zassert_ok(api->send(ETH_DEV, pkt), "send %u of the ring", i);
		net_pkt_unref(pkt);
	}

	/* All four slots now belong to the engine (the starting index is whatever
	 * the previous case left in tx_idx, so count rather than index). */
	uint8_t armed = 0U;

	for (uint8_t i = 0U; i < ETH_TX_DESC; i++) {
		if ((tx_ring()[i].ctrl & DESC_EN) != 0U) {
			armed++;
		}
	}
	zassert_equal(armed, ETH_TX_DESC, "the ring is full (%u of %u armed)", armed, ETH_TX_DESC);

	/* The wrap bit is on the last slot only. */
	zassert_equal(tx_ring()[ETH_TX_DESC - 1U].ctrl & DESC_WRAP, DESC_WRAP,
		      "last TX descriptor wraps the ring");

	/* Fifth send: the ring is full, so send() waits (on tx_sem, not by
	 * spinning) and then gives up with -EBUSY and schedules the halt
	 * recovery. It must not block forever. */
	struct net_pkt *pkt = pkt_with(iface, frame, sizeof(frame));

	zassert_equal(api->send(ETH_DEV, pkt), -EBUSY, "a full ring is reported, not blocked on");
	net_pkt_unref(pkt);

	/* Let the recovery work run, then complete the four outstanding frames. */
	k_sleep(K_MSEC(100));
	zassert_equal(rd(R_CTRL) & (CTRL_TX_EN | CTRL_RX_EN), CTRL_TX_EN | CTRL_RX_EN,
		      "the reset work brought the engine back (CTRL=0x%08x)", rd(R_CTRL));
	zassert_equal(rd(R_TXBASE), txbase_before, "TX ring re-announced");
	zassert_equal(rd(R_RXBASE), rxbase_before, "RX ring re-announced");

	model_complete_tx();

	/* ... and the next frame goes out. */
	pkt = pkt_with(iface, frame, sizeof(frame));
	zassert_ok(api->send(ETH_DEV, pkt), "the ring is usable again");
	net_pkt_unref(pkt);
}

/* ---- receive --------------------------------------------------------- */

ZTEST(eth_agm, test_05_received_frame_reaches_a_packet_socket)
{
	const struct ethernet_api *api = eth_api();
	struct net_if *iface = eth_iface();
	struct sockaddr_ll bind_addr = { .sll_family = AF_PACKET,
					 .sll_ifindex = 0,
					 .sll_protocol = 0,
					 .sll_halen = 6 };
	uint8_t frame[60];
	uint8_t got[80];
	struct net_stats_eth *stats;
	int up_ret;
	ssize_t n;
	int sock;

	eth_agm_set_link_state(ETH_DEV, true);
	/* The stack auto-starts the iface at boot, so -EALREADY is the normal
	 * answer here; anything else is a real failure. */
	up_ret = net_if_up(iface);
	zassert_true(up_ret == 0 || up_ret == -EALREADY, "the interface is up (err %d)", up_ret);
	stats = api->get_stats(ETH_DEV, iface);
	zassert_not_null(stats, "the driver exposes stats");

	bind_addr.sll_ifindex = net_if_get_by_iface(iface);
	bind_addr.sll_protocol = htons(ETH_P_ALL);

	sock = zsock_socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
	zassert_true(sock >= 0, "packet socket created (%d)", sock);
	zassert_ok(zsock_bind(sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr)),
		   "packet socket bound to the MAC iface");

	/* First frame -> slot 0. */
	build_frame(frame, 0x10U);
	model_inject_rx(0U, frame, sizeof(frame));

	n = zsock_recv(sock, got, sizeof(got), 0);
	zassert_equal((int)n, (int)sizeof(frame), "the socket received the whole frame (%d)", (int)n);
	zassert_mem_equal(got, frame, sizeof(frame), "frame bytes survive RX -> L2 -> socket");

	/* The drain re-armed the slot for the next frame and kept RX enabled. */
	zassert_equal(rx_ring()[0].ctrl, DESC_EN | DESC_INTEN, "RX desc[0] re-armed");
	zassert_equal(rd(R_CTRL) & CTRL_RX_EN, CTRL_RX_EN, "RX_EN re-asserted after the drain");

	/* Second frame -> the next slot, proving rx_idx advanced (a drain that
	 * forgot to advance would re-arm slot 0 and drop slot 1). */
	build_frame(frame, 0x20U);
	model_inject_rx(1U, frame, sizeof(frame));

	n = zsock_recv(sock, got, sizeof(got), 0);
	zassert_equal((int)n, (int)sizeof(frame), "second frame received (%d)", (int)n);
	zassert_mem_equal(got, frame, sizeof(frame), "second frame bytes");
	zassert_equal(rx_ring()[1].ctrl, DESC_EN | DESC_INTEN, "RX desc[1] re-armed");

	/* The driver's own byte counter (CONFIG_NET_STATISTICS_ETHERNET). */
	zassert_equal(stats->bytes.received, (uint64_t)(2U * sizeof(frame)),
		      "stats counted both frames (got %llu)", (unsigned long long)stats->bytes.received);

	zsock_close(sock);
	zassert_ok(net_if_down(iface), "the interface goes down again");
}

/* ---- error paths ----------------------------------------------------- */

ZTEST(eth_agm, test_06_ahb_recovery_budget_and_what_re_arms_it)
{
	const struct ethernet_api *api = eth_api();
	struct net_if *iface = eth_iface();
	struct net_stats_eth *stats;
	uint8_t frame[60];
	struct net_pkt *pkt;

	/* A fresh link gives a full budget (this is also what re-arms a driver
	 * that had already given up). */
	eth_agm_set_link_state(ETH_DEV, true);

	/* MAC_RESET_MAX_ATTEMPTS consecutive faults with no working frame in
	 * between: every one of those is recovered. */
	for (uint8_t round = 0U; round < MAC_RESET_MAX_ATTEMPTS; round++) {
		wr(R_STAT, STAT_RX_AHBERR);
		trigger_mac_irq();
		k_sleep(K_MSEC(60));

		zassert_equal(rd(R_STAT), STAT_CLEAR_MASK,
			      "round %u: the recovery work ran (STAT=0x%08x)", round, rd(R_STAT));
		zassert_equal(rd(R_CTRL) & (CTRL_TX_EN | CTRL_RX_EN), CTRL_TX_EN | CTRL_RX_EN,
			      "round %u: engine restarted (CTRL=0x%08x)", round, rd(R_CTRL));
	}

	/* One more fault spends the budget: the recovery runs, finds the counter
	 * exhausted and returns *without* re-enabling the engine (and leaves
	 * mac_halted set, so the ISR stops queueing). Before that the
	 * counter was zeroed at the end of every run, which made this branch
	 * unreachable -- this assertion is the regression guard for that.
	 * "Did not run" is visible as STAT still holding only the AHB bit the
	 * ISR wrote back: hw_reset() is the only path that writes 0x1ff. */
	wr(R_STAT, STAT_RX_AHBERR);
	trigger_mac_irq();
	k_sleep(K_MSEC(60));
	zassert_equal(rd(R_STAT), STAT_RX_AHBERR,
		      "after %u unattended recoveries the driver gives up (STAT=0x%08x)",
		      MAC_RESET_MAX_ATTEMPTS, rd(R_STAT));

	/* ... and it stays given up while the faults keep coming. */
	wr(R_STAT, STAT_RX_AHBERR);
	trigger_mac_irq();
	k_sleep(K_MSEC(60));
	zassert_equal(rd(R_STAT), STAT_RX_AHBERR,
		      "a further fault does not restart recovery (STAT=0x%08x)", rd(R_STAT));

	stats = api->get_stats(ETH_DEV, iface);
	/* AHB faults are NOT counted as RX errors: eth_agm_isr() only bumps
	 * errors.tx/rx for STAT.TX_ERR/RX_ERR (test_07 covers that). */
	zassert_equal(stats->errors.rx, 0U, "AHB faults are not RX errors (got %u)",
		      (unsigned int)stats->errors.rx);

	/* A new link clears the halt state and the budget -- the path a real board
	 * takes when the cable is re-plugged after a permanent-looking fault. */
	eth_agm_set_link_state(ETH_DEV, false);
	eth_agm_set_link_state(ETH_DEV, true);
	wr(R_STAT, STAT_RX_AHBERR);
	trigger_mac_irq();
	k_sleep(K_MSEC(60));
	zassert_equal(rd(R_STAT), STAT_CLEAR_MASK,
		      "a new link re-arms recovery (STAT=0x%08x)", rd(R_STAT));
	zassert_equal(rd(R_CTRL) & (CTRL_TX_EN | CTRL_RX_EN), CTRL_TX_EN | CTRL_RX_EN,
		      "the engine is back (CTRL=0x%08x)", rd(R_CTRL));

	/* Evidence of life also clears the budget, which is what keeps a flaky
	 * bus usable: three faults, then one completed frame, then five more
	 * faults -- all recovered. Without the TX reset the counter would reach
	 * MAC_RESET_MAX_ATTEMPTS and give up on the third of those five. */
	for (uint8_t round = 0U; round < 3U; round++) {
		wr(R_STAT, STAT_RX_AHBERR);
		trigger_mac_irq();
		k_sleep(K_MSEC(60));
	}
	model_complete_tx(); /* IR.TI: a frame finished, the engine is alive */
	for (uint8_t round = 0U; round < MAC_RESET_MAX_ATTEMPTS; round++) {
		wr(R_STAT, STAT_RX_AHBERR);
		trigger_mac_irq();
		k_sleep(K_MSEC(60));
		zassert_equal(rd(R_STAT), STAT_CLEAR_MASK,
			      "round %u after the TX completion: recovery still runs (STAT=0x%08x)",
			      round, rd(R_STAT));
	}

	/* And a queued frame still goes out after all that. */
	build_frame(frame, 0x30U);
	pkt = pkt_with(iface, frame, sizeof(frame));
	zassert_ok(api->send(ETH_DEV, pkt), "send() works after the recoveries");
	net_pkt_unref(pkt);

	/* Leave the driver re-armed for the following cases: a link cycle is the
	 * other thing that resets the halt state and the budget. */
	eth_agm_set_link_state(ETH_DEV, false);
	eth_agm_set_link_state(ETH_DEV, true);
	eth_agm_set_link_state(ETH_DEV, false);
}

ZTEST(eth_agm, test_07_inv_addr_and_error_status_do_not_reset_the_engine)
{
	const struct ethernet_api *api = eth_api();
	struct net_if *iface = eth_iface();
	struct net_stats_eth *stats = api->get_stats(ETH_DEV, iface);
	uint32_t ctrl_before;

	zassert_not_null(stats, "the driver exposes stats");
	eth_agm_set_link_state(ETH_DEV, true);
	ctrl_before = rd(R_CTRL);

	/* This IP raises INV_ADDR twice per transmit; resetting on it is what made
	 * the board deaf for ~500 ms per frame . The ISR must ignore
	 * it -- no CTRL rewrite, no reset work. */
	wr(R_STAT, STAT_INV_ADDR);
	trigger_mac_irq();
	k_sleep(K_MSEC(60));
	zassert_equal(rd(R_CTRL), ctrl_before, "INV_ADDR does not touch CTRL (0x%08x)", rd(R_CTRL));

	/* Same for a plain (non-AHB) TX/RX error: counted, not fatal. */
	wr(R_STAT, STAT_TX_ERR | STAT_RX_ERR);
	trigger_mac_irq();
	k_sleep(K_MSEC(60));
	zassert_equal(rd(R_CTRL), ctrl_before, "TX_ERR/RX_ERR do not touch CTRL");
	zassert_equal(stats->errors.tx, 1U, "TX_ERR counted (got %u)", (unsigned int)stats->errors.tx);
	zassert_equal(stats->errors.rx, 1U, "RX_ERR counted (got %u)", (unsigned int)stats->errors.rx);
}

ZTEST(eth_agm, test_08_send_gates_on_link_and_length)
{
	const struct ethernet_api *api = eth_api();
	struct net_if *iface = eth_iface();
	uint8_t frame[60];
	uint32_t ctrl_before;
	struct net_pkt *pkt;

	build_frame(frame, 0x50U);

	/* Link down: refused before anything reaches the ring. */
	eth_agm_set_link_state(ETH_DEV, false);
	ctrl_before = tx_ring()[0].ctrl;

	pkt = pkt_with(iface, frame, sizeof(frame));
	zassert_equal(api->send(ETH_DEV, pkt), -ENETDOWN, "a down link refuses to send");
	net_pkt_unref(pkt);
	zassert_equal(tx_ring()[0].ctrl, ctrl_before, "nothing was armed");

	eth_agm_set_link_state(ETH_DEV, true);

	/* Zero-length frame. */
	pkt = pkt_with(iface, NULL, 0U);
	zassert_equal(api->send(ETH_DEV, pkt), -EMSGSIZE, "an empty frame is refused");
	net_pkt_unref(pkt);

	/* Larger than the driver's RX_BUF_SIZE (1536). */
	static uint8_t big[1537];

	/* Built as two fragments: net_pkt_write() only fills space a packet
	 * already has, and one pool buffer holds 1514 B here, so a single 1537 B
	 * allocation is not possible. Adding a second fragment is -- and
	 * net_pkt_get_len(), which is all the driver looks at, then reads 1600. */
	struct net_buf *frag;

	pkt = pkt_with(iface, big, 1000U);
	frag = net_pkt_get_frag(pkt, 600U, K_NO_WAIT);
	zassert_not_null(frag, "second fragment from the TX pool");
	net_buf_add_mem(frag, big, 600U);
	net_pkt_append_buffer(pkt, frag);
	net_pkt_cursor_init(pkt);
	zassert_equal(net_pkt_get_len(pkt), 1600U, "over-long pkt assembled (%zu)",
		      net_pkt_get_len(pkt));
	zassert_equal(api->send(ETH_DEV, pkt), -EMSGSIZE, "an over-long frame is refused");
	net_pkt_unref(pkt);
}
