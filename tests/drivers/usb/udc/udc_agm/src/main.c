/*
 * Copyright (c) 2026 AgRV Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief native_sim suite for the AgRV2K USB device controller driver.
 *
 * The model is the shared RAM window plus one behaviour hook: a 1 ms timer
 * that clears USBCMD.RST, because usb_reset_controller() sets that bit and
 * waits for the *controller* to clear it. With plain RAM the driver would spin
 * for its whole bound (1 s of simulated time) on every init.
 * Everything else in this driver's bring-up is register programming, and that
 * is what the cases read back through the real addresses.
 *
 * Covered: device-mode bring-up (USBMODE, the endpoint-list address, port
 * power, the interrupt mask, the run bit), the EP0 enable udc_init() does, the
 * dQH repaint (setup tripwire) udc_enable() does, the address register,
 * per-endpoint halt/clear-halt with their direction bits, and
 * disable/shutdown. Not reachable without a host: the ISR, and with it the
 * whole transfer path.
 */

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/usb/udc.h>
#include <zephyr/kernel.h>
#include <zephyr/usb/usb_ch9.h>
#include <zephyr/ztest.h>

#define UDC_DEV  DEVICE_DT_GET(DT_NODELABEL(usb_agm0))

/* Register offsets (drivers/usb/udc/udc_agm.h: struct udc_agm_regs). */
#define R_USBCMD       0x140U
#define R_USBINTR      0x148U
#define R_DEVICEADDR   0x154U
#define R_EPLISTADDR   0x158U
#define R_PORTSC       0x184U
#define R_USBMODE      0x1a8U
#define R_ENDPTCTRL(n) (0x1c0U + (4U * (n)))

#define CMD_RS  BIT(0)
#define CMD_RST BIT(1)
#define CMD_ITC (0xffU << 16)

#define STS_UI  BIT(0)
#define STS_UEI BIT(1)
#define STS_PCI BIT(2)
#define STS_SEI BIT(4)
#define STS_URI BIT(6)
#define STS_SLI BIT(8)
#define STS_ALL (STS_UI | STS_UEI | STS_PCI | STS_SEI | STS_URI | STS_SLI)

#define PORTSC_PP BIT(12)

#define MODE_CM_MASK   0x3U
#define MODE_CM_DEVICE 0x2U
#define MODE_SLOM      BIT(3)

#define EPCTRL_RX_S          BIT(0)
#define EPCTRL_RX_TYPE_SHIFT 2U
#define EPCTRL_RX_TYPE_MASK  (0x3U << EPCTRL_RX_TYPE_SHIFT)
#define EPCTRL_RX_TR         BIT(6)
#define EPCTRL_RX_E          BIT(7)
#define EPCTRL_TX_S          BIT(16)
#define EPCTRL_TX_TYPE_SHIFT 18U
#define EPCTRL_TX_TR         BIT(22)
#define EPCTRL_TX_E          BIT(23)

#define USB_BASE 0x41001000UL

/* First word of the dQH the driver keeps at ENDPOINTLISTADDR (struct
 * udc_agm_dqh in udc_agm.h); only the setup-tripwire bit is needed. */
#define DQH_INFO_INT_ON_SETUP BIT(15)

static uint32_t rd(uint32_t off)
{
	return *(volatile uint32_t *)(USB_BASE + off);
}

static void wr(uint32_t off, uint32_t val)
{
	*(volatile uint32_t *)(USB_BASE + off) = val;
}

/* ---- the one behaviour the model needs -------------------------------- */

static struct k_timer usb_timer;

static void usb_expire(struct k_timer *timer)
{
	uint32_t cmd = rd(R_USBCMD);

	ARG_UNUSED(timer);

	if ((cmd & CMD_RST) != 0U) {
		/* The controller clears its own reset bit when it is done. */
		wr(R_USBCMD, cmd & ~CMD_RST);
	}
}

static void *suite_setup(void)
{
	k_timer_init(&usb_timer, usb_expire, NULL);
	k_timer_start(&usb_timer, K_MSEC(1), K_MSEC(1));

	return NULL;
}

static void suite_teardown(void *fixture)
{
	ARG_UNUSED(fixture);
	k_timer_stop(&usb_timer);
}

ZTEST_SUITE(udc_agm, NULL, suite_setup, NULL, NULL, suite_teardown);

/* The event callback udc_init() requires: it would be called for VBUS/reset
 * events, none of which happen without a host. */
static int dummy_event(const struct device *dev, const struct udc_event *const event)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(event);
	return 0;
}

/* ---- bring-up --------------------------------------------------------- */

ZTEST(udc_agm, test_01_init_programs_device_mode_and_the_endpoint_list)
{
	static int event_ctx;
	uint32_t eplist;
	uint32_t ep0;

	/* udc_agm_preinit() ran at boot (POST_KERNEL) and registered the
	 * endpoints; udc_init() is the part that touches the controller. */
	zassert_true(device_is_ready(UDC_DEV), "the UDC device came up against the fake window");
	zassert_equal(udc_init(UDC_DEV, NULL, &event_ctx), -EINVAL, "an event callback is required");
	zassert_ok(udc_init(UDC_DEV, dummy_event, &event_ctx), "udc_init()");

	/* Device mode, setup-lockout off (matches the vendor dcd). */
	zassert_equal(rd(R_USBMODE) & MODE_CM_MASK, MODE_CM_DEVICE, "USBMODE = device");
	zassert_equal(rd(R_USBMODE) & MODE_SLOM, MODE_SLOM, "SLOM set");

	/* The dQH table address: the driver's own 4096-aligned pool. */
	eplist = rd(R_EPLISTADDR);
	zassert_not_equal(eplist, 0U, "the endpoint list address is programmed");
	zassert_equal(eplist & 0xfffU, 0U, "and 4 KiB aligned, as the controller needs");

	zassert_equal(rd(R_PORTSC) & PORTSC_PP, PORTSC_PP, "port power on");
	zassert_equal(rd(R_USBCMD) & CMD_ITC, 0U, "interrupt threshold 0 (every transaction)");

	/* EP0 is enabled both ways as a control endpoint. */
	ep0 = rd(R_ENDPTCTRL(0));
	zassert_equal(ep0 & (EPCTRL_RX_E | EPCTRL_TX_E), EPCTRL_RX_E | EPCTRL_TX_E,
		      "EP0 enabled both ways");
	zassert_equal((ep0 & EPCTRL_RX_TYPE_MASK) >> EPCTRL_RX_TYPE_SHIFT, 0U, "RX type = control");
	zassert_equal((ep0 & (0x3U << EPCTRL_TX_TYPE_SHIFT)) >> EPCTRL_TX_TYPE_SHIFT, 0U,
		      "TX type = control");

	/* udc_init() is a one-shot. */
	zassert_equal(udc_init(UDC_DEV, dummy_event, &event_ctx), -EALREADY, "already initialised");
}

ZTEST(udc_agm, test_10_enable_arms_the_setup_tripwire_on_the_queue_head)
{
	uint32_t dqh0_info;

	/* udc_enable() rebuilds every dQH and sets the EP0 setup tripwire, which
	 * is what makes the controller hand a SETUP packet to the driver. It is
	 * also where the interrupt mask and the run bit are programmed --
	 * udc_init() leaves them alone. */
	zassert_ok(udc_enable(UDC_DEV), "udc_enable()");

	dqh0_info = *(volatile uint32_t *)(uintptr_t)rd(R_EPLISTADDR);
	zassert_equal(dqh0_info & DQH_INFO_INT_ON_SETUP, DQH_INFO_INT_ON_SETUP,
		      "the EP0 queue head is armed for SETUP");
	zassert_equal(rd(R_USBINTR) & STS_ALL, STS_ALL, "the device interrupts are unmasked");
	zassert_equal(rd(R_USBINTR) & ~STS_ALL, 0U, "and nothing else");
	zassert_equal(rd(R_USBCMD) & CMD_RS, CMD_RS, "and the controller runs");
}

ZTEST(udc_agm, test_20_the_device_address_register)
{
	/* DEVICEADDR: address in bits 31:25 with the always-set bit 24. */
	zassert_ok(udc_set_address(UDC_DEV, 0x42U));
	zassert_equal(rd(R_DEVICEADDR), (0x42U << 25) | BIT(24), "address 0x42");

	zassert_ok(udc_set_address(UDC_DEV, 0U));
	zassert_equal(rd(R_DEVICEADDR), BIT(24), "and back to 0");
}

ZTEST(udc_agm, test_30_halt_and_clear_halt_use_the_right_direction_bits)
{
	const uint8_t ep_out = USB_EP_GET_ADDR(1U, USB_EP_DIR_OUT);
	const uint8_t ep_in = USB_EP_GET_ADDR(1U, USB_EP_DIR_IN);

	zassert_ok(udc_ep_enable(UDC_DEV, ep_out, USB_EP_TYPE_BULK, 64U, 0U), "enable EP1 OUT");
	zassert_ok(udc_ep_set_halt(UDC_DEV, ep_out));
	zassert_equal(rd(R_ENDPTCTRL(1)) & EPCTRL_RX_S, EPCTRL_RX_S, "EP1 OUT stalled");
	zassert_equal(rd(R_ENDPTCTRL(1)) & EPCTRL_TX_S, 0U, "and the IN half is untouched");

	zassert_ok(udc_ep_clear_halt(UDC_DEV, ep_out));
	zassert_equal(rd(R_ENDPTCTRL(1)) & EPCTRL_RX_S, 0U, "stall cleared");
	zassert_equal(rd(R_ENDPTCTRL(1)) & EPCTRL_RX_TR, EPCTRL_RX_TR,
		      "and the data toggle reset, as the controller expects");

	zassert_ok(udc_ep_enable(UDC_DEV, ep_in, USB_EP_TYPE_BULK, 64U, 0U), "enable EP1 IN");
	zassert_ok(udc_ep_set_halt(UDC_DEV, ep_in));
	zassert_equal(rd(R_ENDPTCTRL(1)) & EPCTRL_TX_S, EPCTRL_TX_S, "EP1 IN stalled");
	zassert_equal(rd(R_ENDPTCTRL(1)) & EPCTRL_RX_S, 0U, "and the OUT half stays up");
	zassert_ok(udc_ep_clear_halt(UDC_DEV, ep_in));
}

ZTEST(udc_agm, test_40_disable_and_shutdown)
{
	zassert_ok(udc_disable(UDC_DEV));
	zassert_equal(rd(R_USBCMD) & CMD_RS, 0U, "disable stops the controller");
	zassert_equal(rd(R_USBINTR), 0U, "and masks every interrupt");

	/* shutdown drops EP0 in hardware; the endpoints stay registered. */
	zassert_ok(udc_shutdown(UDC_DEV));
	zassert_equal(rd(R_ENDPTCTRL(0)) & (EPCTRL_RX_E | EPCTRL_TX_E), 0U, "EP0 disabled");
}
