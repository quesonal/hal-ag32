/*
 * Copyright (c) 2026 AgRV Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief native_sim suite for the AgRV2K USB host controller driver.
 *
 * The model is the shared RAM window plus two behaviours a RAM window does
 * not have, both polled by the driver with a bounded spin:
 *   - USBCMD.RST must clear itself (controller reset), otherwise uhc_init()
 *     spins out and fails;
 *   - USBSTS.HCH must follow USBCMD.RS (halt), otherwise uhc_disable() spins
 *     out and only logs.
 * Everything else in bring-up and in the queue-head build is register
 * programming, which the cases read back through the real addresses.
 *
 * Covered: host-mode bring-up (USBMODE.CM, both schedule base registers,
 * interrupt threshold, interrupts masked until enable), enable (run bit,
 * both schedule enables, frame-list size, interrupt mask), bus reset (PR
 * released, event reported), the EP0 queue-head build for a zero-length and a
 * data-stage control transfer (device address, endpoint, speed, mps, the
 * full/low-speed control flag, and the SETUP -> DATA1 qTD chain), the
 * dequeue path, the ISR completion path (the tests write the queue-head
 * overlay word and raise the controller's IRQ through native_sim's software
 * interrupt hook, which is how a stall, a transaction error and a retired
 * bulk-IN chain are injected without a device on the wire), the size guard on
 * the qTD byte count, and disable/shutdown. The one thing still not covered
 * here is a *port change* -- the fake window has no PORTSC model.
 */

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/usb/uhc.h>
#include <zephyr/kernel.h>
#include <zephyr/usb/usb_ch9.h>
#include <zephyr/ztest.h>

#include <string.h>

/*
 * native_sim's software interrupt hook -- the same one Zephyr's own native_sim
 * driver tests use to inject an interrupt (boards/native/native_sim/
 * irq_handler.h).
 */
#include <irq_handler.h>

#define UHC_DEV DEVICE_DT_GET(DT_NODELABEL(uhc_agm0))

/* Register offsets (drivers/usb/udc/udc_agm.h: struct udc_agm_regs). */
#define R_USBCMD		0x140U
#define R_USBSTS		0x144U
#define R_USBINTR		0x148U
#define R_PERIODICLISTBASE	0x154U
#define R_ASYNCLISTADDR		0x158U
#define R_PORTSC		0x184U
#define R_USBMODE		0x1a8U

#define CMD_RS	BIT(0)
#define CMD_RST	BIT(1)
#define CMD_FS0	BIT(2)
#define CMD_FS1	BIT(3)
#define CMD_PSE	BIT(4)
#define CMD_ASE	BIT(5)
#define CMD_FS2	BIT(15)
#define CMD_ITC	(0xffU << 16)

#define STS_HCH	BIT(12)

#define MODE_CM_MASK	0x3U
#define MODE_CM_HOST	0x3U

#define PORTSC_PR	BIT(8)
#define PORTSC_PP	BIT(12)

/* UI|UEI|PCI|FRI|SEI|AAI, the set uhc_agm_enable() unmasks. */
#define INTR_MASK	(BIT(0) | BIT(1) | BIT(2) | BIT(3) | BIT(4) | BIT(5))

/* Queue-head / qTD word offsets inside the driver's pool. */
#define QHD_NEXT		0x00U
#define QHD_EP_CHAR		0x04U
#define QHD_OVERLAY_NEXT	0x10U

#define QTD_NEXT		0x00U
#define QTD_WORD1		0x04U
#define QTD_TOKEN		0x08U

/* qTD word 1 bit 5: the driver's software "used" flag (see uhc_agm.h). */
#define QTD_USED		BIT(5)

#define QTD_PID_SHIFT		8U
#define QTD_PID_MASK		0x3U
#define QTD_ACTIVE		BIT(7)
#define QTD_DATA_TOGGLE		BIT(31)
#define QTD_BYTES_SHIFT		16U
#define QTD_BYTES_MASK		0x7fffU

/* Queue-head overlay (struct uhc_agm_qhd::overlay) at 0x10, whose token word
 * is at 0x18: the controller writes the halt / error / byte-count verdict
 * there and the driver reads it in slot_complete(). */
#define QHD_OVERLAY_TOKEN	0x18U

/* Token bits the completion path looks at (struct uhc_agm_qtd::token). */
#define TOK_XACT_ERR		BIT(3)
#define TOK_HALTED		BIT(6)
#define TOK_ERR_COUNT_SHIFT	10U

/* USBSTS.UI, the interrupt that drives the async/periodic completion scan. */
#define STS_UI			BIT(0)

/* PID values (drivers/usb/uhc/uhc_agm.h). */
#define PID_OUT		0U
#define PID_IN		1U
#define PID_SETUP	2U

#define USB_BASE	0x41001000UL
#define LINK_ALIGN	0x1fU

/* The controller's IRQ in this build (app.overlay: <14 1>). */
#define UHC_IRQ		DT_IRQN(DT_NODELABEL(uhc_agm0))

static uint32_t rd(uint32_t off)
{
	return *(volatile uint32_t *)(USB_BASE + off);
}

static void wr(uint32_t off, uint32_t val)
{
	*(volatile uint32_t *)(USB_BASE + off) = val;
}

static uint32_t rd32(uintptr_t addr)
{
	return *(volatile uint32_t *)addr;
}

static void wr32(uintptr_t addr, uint32_t val)
{
	*(volatile uint32_t *)addr = val;
}

/*
 * Raise the controller's interrupt once, with USBSTS.UI set.
 *
 * The window does not model write-one-to-clear, so the ISR's re-read always
 * sees the bit again: the driver runs all 16 rounds and its "USBSTS still set"
 * warning fires (once per session, see the driver). That is the model's
 * limitation, not a driver fault, and it has the side effect of exercising
 * both paths here. The bit is cleared again afterwards so the next case starts
 * clean.
 */
static void uhc_irq(void)
{
	wr(R_USBSTS, rd(R_USBSTS) | STS_UI);
	posix_sw_set_pending_IRQ(UHC_IRQ);
	wr(R_USBSTS, rd(R_USBSTS) & ~STS_UI);
}

/* ---- the two behaviours the RAM window does not have ------------------- */

static struct k_timer usb_timer;

static void usb_expire(struct k_timer *timer)
{
	uint32_t cmd = rd(R_USBCMD);

	ARG_UNUSED(timer);

	if ((cmd & CMD_RST) != 0U) {
		/* The controller clears its own reset bit when it is done. */
		wr(R_USBCMD, cmd & ~CMD_RST);
		cmd &= ~CMD_RST;
	}

	/* HCH is the inverse of the run bit. */
	if ((cmd & CMD_RS) == 0U) {
		wr(R_USBSTS, rd(R_USBSTS) | STS_HCH);
	} else {
		wr(R_USBSTS, rd(R_USBSTS) & ~STS_HCH);
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

/* The event callback uhc_init() requires; bus_reset() reports through it. */
static atomic_t evt_count;
static int last_evt;

static int test_event_cb(const struct device *dev,
			 const struct uhc_event *const event)
{
	ARG_UNUSED(dev);

	last_evt = (int)event->type;
	atomic_inc(&evt_count);

	return 0;
}

static int test_xfer_cb(struct usb_device *const udev,
			struct uhc_transfer *const xfer)
{
	ARG_UNUSED(udev);
	ARG_UNUSED(xfer);

	return 0;
}

ZTEST_SUITE(uhc_agm, NULL, suite_setup, NULL, NULL, suite_teardown);

/* ---- helpers ----------------------------------------------------------- */

static uintptr_t async_head(void)
{
	return (uintptr_t)(rd(R_ASYNCLISTADDR) & ~(uintptr_t)LINK_ALIGN);
}

/* First real queue head on the asynchronous ring (0 when only the head is
 * there, i.e. no endpoint has been opened). */
static uintptr_t async_first_qhd(void)
{
	uintptr_t head = async_head();
	uintptr_t next;

	if (head == 0U) {
		return 0U;
	}

	next = (uintptr_t)(rd32(head + QHD_NEXT) & ~(uintptr_t)LINK_ALIGN);

	return (next == head) ? 0U : next;
}

/* One device, used by both control-transfer cases: address 0 (pre-SET_ADDRESS
 * state), full speed, mps0 = 8. */
static struct usb_device test_udev;

static void test_udev_reset(void)
{
	memset(&test_udev, 0, sizeof(test_udev));
	test_udev.speed = USB_SPEED_SPEED_FS;
	test_udev.dev_desc.bMaxPacketSize0 = 8U;
}

/* ---- bring-up --------------------------------------------------------- */

ZTEST(uhc_agm, test_01_init_programs_host_mode_and_both_schedules)
{
	static int event_ctx;
	uint32_t mode;

	zassert_true(device_is_ready(UHC_DEV), "the UHC device came up against the fake window");
	zassert_equal(uhc_init(UHC_DEV, NULL, &event_ctx), -EINVAL,
		      "an event callback is required");
	zassert_ok(uhc_init(UHC_DEV, test_event_cb, &event_ctx), "uhc_init()");

	mode = rd(R_USBMODE);
	zassert_equal(mode & MODE_CM_MASK, MODE_CM_HOST, "USBMODE.CM = host");

	zassert_not_equal(rd(R_ASYNCLISTADDR), 0U, "the async queue-head ring is programmed");
	zassert_equal(rd(R_ASYNCLISTADDR) & LINK_ALIGN, 0U, "and 32-byte aligned");
	zassert_not_equal(rd(R_PERIODICLISTBASE), 0U, "the frame list is programmed");
	zassert_equal(rd(R_PERIODICLISTBASE) & 0xfffU, 0U, "and 4 KiB aligned");

	zassert_equal(rd(R_USBINTR), 0U, "interrupts stay masked until enable()");
	zassert_equal(rd(R_USBCMD) & CMD_ITC, 0U, "interrupt threshold 0");
	zassert_equal(rd(R_USBCMD) & (CMD_FS0 | CMD_FS1 | CMD_FS2),
		      CMD_FS0 | CMD_FS1 | CMD_FS2,
		      "8-entry frame list (ChipIdea encoding), set while RS is clear");
	zassert_equal(rd(R_USBCMD) & CMD_RS, 0U, "and the schedules are not started yet");

	zassert_equal(uhc_init(UHC_DEV, test_event_cb, &event_ctx), -EALREADY, "one-shot");
}

ZTEST(uhc_agm, test_10_enable_unmasks_and_starts_the_schedules)
{
	uint32_t cmd;

	zassert_ok(uhc_enable(UHC_DEV), "uhc_enable()");

	cmd = rd(R_USBCMD);
	zassert_equal(cmd & CMD_RS, CMD_RS, "the controller runs");
	zassert_equal(cmd & CMD_PSE, CMD_PSE, "the periodic schedule is enabled");
	zassert_equal(cmd & CMD_ASE, CMD_ASE, "the async schedule is enabled");
	zassert_equal(rd(R_USBINTR) & INTR_MASK, INTR_MASK, "the host interrupts are unmasked");

	zassert_equal(uhc_enable(UHC_DEV), -EALREADY, "already enabled");
}

ZTEST(uhc_agm, test_20_bus_reset_reports_the_reset_event)
{
	atomic_clear(&evt_count);
	last_evt = -1;

	zassert_ok(uhc_bus_reset(UHC_DEV), "uhc_bus_reset()");
	zassert_equal(rd(R_PORTSC) & PORTSC_PR, 0U, "PR released after the reset window");
	zassert_equal(last_evt, (int)UHC_EVT_RESETED,
		      "the host stack is told the reset finished");
}

/* ---- endpoint / transfer build ----------------------------------------- */

ZTEST(uhc_agm, test_30_zero_length_control_is_setup_plus_status)
{
	/* SET_ADDRESS(1), no data stage. */
	static const uint8_t setup[8] = { 0x00, 0x05, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00 };
	struct uhc_transfer *xfer;
	uintptr_t qhd;
	uintptr_t qtd;
	uintptr_t status_qtd;
	uint32_t ep_char;
	uint32_t token;

	test_udev_reset();

	xfer = uhc_xfer_alloc(UHC_DEV, 0x00, &test_udev, test_xfer_cb, NULL);
	zassert_not_null(xfer, "control transfer allocated");
	memcpy(xfer->setup_pkt, setup, sizeof(setup));

	zassert_ok(uhc_ep_enqueue(UHC_DEV, xfer), "enqueue");

	qhd = async_first_qhd();
	zassert_not_equal(qhd, 0U, "a queue head showed up on the async ring");

	ep_char = rd32(qhd + QHD_EP_CHAR);
	zassert_equal(ep_char & 0x7fU, 0U, "queue head device address 0");
	zassert_equal((ep_char >> 8) & 0x0fU, 0U, "endpoint 0");
	zassert_equal((ep_char >> 12) & 0x3U, 0U, "full speed");
	zassert_equal((ep_char >> 14) & 0x1U, 1U, "control uses the queue head data toggle");
	zassert_equal((ep_char >> 16) & 0x7ffU, 8U, "mps0 from the device descriptor");
	zassert_equal((ep_char >> 27) & 0x1U, 1U,
		      "FL control-endpoint flag: the controller runs the status stage");

	qtd = (uintptr_t)(rd32(qhd + QHD_OVERLAY_NEXT) & ~(uintptr_t)LINK_ALIGN);
	zassert_not_equal(qtd, 0U, "a qTD is attached to the overlay");

	token = rd32(qtd + QTD_TOKEN);
	zassert_equal((token >> QTD_PID_SHIFT) & QTD_PID_MASK, PID_SETUP, "SETUP pid");
	zassert_equal(token & QTD_ACTIVE, QTD_ACTIVE, "and armed");
	zassert_equal(token & QTD_DATA_TOGGLE, 0U, "the setup stage is DATA0");
	zassert_equal((token >> QTD_BYTES_SHIFT) & QTD_BYTES_MASK, 8U, "8 setup bytes");

	/*
	 * The zero-length request still gets a status stage, in the direction
	 * opposite to the request (to-device -> IN), toggle 1. Without it the
	 * controller answers nothing and the device never applies SET_ADDRESS:
	 * that was the first failure seen with a real device.
	 */
	status_qtd = (uintptr_t)(rd32(qtd + QTD_NEXT) & ~(uintptr_t)LINK_ALIGN);
	zassert_not_equal(status_qtd, qtd, "a status qTD follows the setup stage");
	token = rd32(status_qtd + QTD_TOKEN);
	zassert_equal((token >> QTD_PID_SHIFT) & QTD_PID_MASK, PID_IN,
		      "status direction is opposite to the request");
	zassert_equal(token & QTD_DATA_TOGGLE, QTD_DATA_TOGGLE, "status is DATA1");
	zassert_equal((token >> QTD_BYTES_SHIFT) & QTD_BYTES_MASK, 0U,
		      "and zero length");
	zassert_equal(rd32(status_qtd + QTD_NEXT) & 0x1U, 0x1U,
		      "the status stage ends the chain");

	zassert_ok(uhc_ep_dequeue(UHC_DEV, xfer), "dequeue");
	zassert_ok(uhc_xfer_free(UHC_DEV, xfer), "free");
}

ZTEST(uhc_agm, test_35_control_in_builds_setup_data1_and_status)
{
	/* GET_DESCRIPTOR(device), wLength 18. */
	static const uint8_t setup[8] = { 0x80, 0x06, 0x00, 0x01, 0x00, 0x00, 0x12, 0x00 };
	struct uhc_transfer *xfer;
	uintptr_t setup_qtd;
	uintptr_t data_qtd;
	uintptr_t status_qtd;
	uintptr_t qhd;
	uint32_t token;

	test_udev_reset();

	xfer = uhc_xfer_alloc_with_buf(UHC_DEV, 0x80, &test_udev, test_xfer_cb, NULL, 18U);
	zassert_not_null(xfer, "control IN transfer allocated with a buffer");
	memcpy(xfer->setup_pkt, setup, sizeof(setup));

	zassert_ok(uhc_ep_enqueue(UHC_DEV, xfer), "enqueue");

	/* Endpoint 0 keeps one queue head for both directions. */
	qhd = async_first_qhd();
	zassert_not_equal(qhd, 0U, "the EP0 queue head is still there");

	/* SETUP -> DATA1 -> STATUS, handed over as one chain: the controller
	 * is not restarted between stages, and it does not add the status
	 * stage itself. */
	setup_qtd = (uintptr_t)(rd32(qhd + QHD_OVERLAY_NEXT) & ~(uintptr_t)LINK_ALIGN);
	zassert_not_equal(setup_qtd, 0U, "a qTD chain is attached");

	token = rd32(setup_qtd + QTD_TOKEN);
	zassert_equal((token >> QTD_PID_SHIFT) & QTD_PID_MASK, PID_SETUP,
		      "the chain starts with SETUP");
	zassert_equal((token >> QTD_BYTES_SHIFT) & QTD_BYTES_MASK, 8U, "8 setup bytes");

	data_qtd = (uintptr_t)(rd32(setup_qtd + QTD_NEXT) & ~(uintptr_t)LINK_ALIGN);
	zassert_not_equal(data_qtd, setup_qtd, "the data stage follows");
	token = rd32(data_qtd + QTD_TOKEN);
	zassert_equal((token >> QTD_PID_SHIFT) & QTD_PID_MASK, PID_IN, "IN data stage");
	zassert_equal(token & QTD_DATA_TOGGLE, QTD_DATA_TOGGLE, "starting at DATA1");
	zassert_equal((token >> QTD_BYTES_SHIFT) & QTD_BYTES_MASK, 18U,
		      "wLength bytes armed");

	status_qtd = (uintptr_t)(rd32(data_qtd + QTD_NEXT) & ~(uintptr_t)LINK_ALIGN);
	zassert_not_equal(status_qtd, data_qtd, "and the status stage follows data");
	token = rd32(status_qtd + QTD_TOKEN);
	zassert_equal((token >> QTD_PID_SHIFT) & QTD_PID_MASK, PID_OUT,
		      "status direction is opposite to the data stage");
	zassert_equal(token & QTD_DATA_TOGGLE, QTD_DATA_TOGGLE, "status is DATA1");
	zassert_equal((token >> QTD_BYTES_SHIFT) & QTD_BYTES_MASK, 0U, "zero length");
	zassert_equal(rd32(status_qtd + QTD_NEXT) & 0x1U, 0x1U,
		      "the status stage ends the chain");

	zassert_ok(uhc_ep_dequeue(UHC_DEV, xfer), "dequeue");
	zassert_ok(uhc_xfer_free(UHC_DEV, xfer), "free");
}

/*
 * ---- ISR completion path ----------------------------------------------
 *
 * The fake window never retires anything, so the driver's completion logic
 * can only be reached by faking what the controller would leave behind in the
 * queue head's overlay and then raising the IRQ. Two verdicts are worth
 * covering: a STALL (halted with retries left) and a transaction error (the
 * device stopped answering), plus the clean case where the tail qTD retired.
 */

ZTEST(uhc_agm, test_36_stall_completion_reports_epipe_and_pools_the_chain)
{
	/* SET_ADDRESS(1): a control transfer with no data stage. */
	static const uint8_t setup[8] = { 0x00, 0x05, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00 };
	struct uhc_transfer *xfer;
	uintptr_t qhd;
	uintptr_t qtd;

	test_udev_reset();

	xfer = uhc_xfer_alloc(UHC_DEV, 0x00, &test_udev, test_xfer_cb, NULL);
	zassert_not_null(xfer, "control transfer allocated");
	memcpy(xfer->setup_pkt, setup, sizeof(setup));
	zassert_ok(uhc_ep_enqueue(UHC_DEV, xfer), "enqueue");

	qhd = async_first_qhd();
	zassert_not_equal(qhd, 0U, "a queue head is armed");
	qtd = (uintptr_t)(rd32(qhd + QHD_OVERLAY_NEXT) & ~(uintptr_t)LINK_ALIGN);
	zassert_not_equal(qtd, 0U, "and it has a qTD chain");

	/* The device answered STALL: the controller halts the queue head and
	 * leaves the retry counter untouched. */
	wr32(qhd + QHD_OVERLAY_TOKEN, TOK_HALTED | (3U << TOK_ERR_COUNT_SHIFT));

	uhc_irq();

	zassert_equal(xfer->err, -EPIPE, "a stall is reported as -EPIPE");
	zassert_equal(xfer->queued, 0U, "and the transfer left the core list");
	zassert_equal(last_evt, (int)UHC_EVT_EP_REQUEST, "the owner saw a completion");
	zassert_equal(rd32(qhd + QHD_OVERLAY_TOKEN) & TOK_HALTED, 0U,
		      "the halt is cleared so the endpoint can be used again");
	zassert_equal(rd32(qhd + QHD_OVERLAY_NEXT) & 0x1U, 0x1U,
		      "the queue head stopped fetching");
	zassert_equal(rd32(qtd + QTD_WORD1) & QTD_USED, 0U,
		      "and the qTD chain is back in the pool");

	zassert_ok(uhc_xfer_free(UHC_DEV, xfer), "free");
}

ZTEST(uhc_agm, test_37_transaction_error_completion_reports_eio)
{
	static const uint8_t setup[8] = { 0x00, 0x05, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00 };
	struct uhc_transfer *xfer;
	uintptr_t qhd;

	test_udev_reset();

	xfer = uhc_xfer_alloc(UHC_DEV, 0x00, &test_udev, test_xfer_cb, NULL);
	zassert_not_null(xfer, "control transfer allocated");
	memcpy(xfer->setup_pkt, setup, sizeof(setup));
	zassert_ok(uhc_ep_enqueue(UHC_DEV, xfer), "enqueue");

	qhd = async_first_qhd();
	zassert_not_equal(qhd, 0U, "a queue head is armed");

	/* No answer at all: the controller halts with a transaction error. */
	wr32(qhd + QHD_OVERLAY_TOKEN, TOK_HALTED | TOK_XACT_ERR |
					      (3U << TOK_ERR_COUNT_SHIFT));

	uhc_irq();

	zassert_equal(xfer->err, -EIO, "a transaction error is reported as -EIO");
	zassert_equal(xfer->queued, 0U, "and the transfer left the core list");
	zassert_equal(rd32(qhd + QHD_OVERLAY_TOKEN) & TOK_HALTED, 0U,
		      "the halt is cleared");

	zassert_ok(uhc_xfer_free(UHC_DEV, xfer), "free");
}

/* ---- dequeue ----------------------------------------------------------- */

ZTEST(uhc_agm, test_38_dequeue_retires_the_queue_head_but_keeps_the_qtds)
{
	/* SET_ADDRESS(1): a control transfer with no data stage. */
	static const uint8_t setup[8] = { 0x00, 0x05, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00 };
	struct uhc_transfer *xfer;
	uintptr_t qhd;
	uintptr_t qtd;

	test_udev_reset();

	xfer = uhc_xfer_alloc(UHC_DEV, 0x00, &test_udev, test_xfer_cb, NULL);
	zassert_not_null(xfer, "control transfer allocated");
	memcpy(xfer->setup_pkt, setup, sizeof(setup));
	zassert_ok(uhc_ep_enqueue(UHC_DEV, xfer), "enqueue");

	qhd = async_first_qhd();
	zassert_not_equal(qhd, 0U, "a queue head is armed");
	qtd = (uintptr_t)(rd32(qhd + QHD_OVERLAY_NEXT) & ~(uintptr_t)LINK_ALIGN);
	zassert_not_equal(qtd, 0U, "and it has a qTD");

	zassert_ok(uhc_ep_dequeue(UHC_DEV, xfer), "dequeue");

	/*
	 * The controller may still be using that qTD (the fake window never
	 * retires anything), so the dequeue must stop the queue head from
	 * fetching more while leaving the chain in place for the completion
	 * scan to pool later.
	 */
	zassert_equal(rd32(qhd + QHD_OVERLAY_NEXT) & 0x1U, 0x1U,
		      "the queue head stops fetching");
	zassert_equal(rd32(qtd + QTD_WORD1) & QTD_USED, QTD_USED,
		      "and the qTD is not back in the pool yet");

	zassert_ok(uhc_xfer_free(UHC_DEV, xfer), "the caller still owns the transfer");
}

/*
 * ---- bulk transfers, and the qTD size guard ----------------------------
 *
 * Both need an endpoint descriptor on the fake device: uhc_xfer_alloc() reads
 * mps / type / interval out of udev->ep_in[], exactly as the host stack fills
 * it in while parsing a configuration.
 */
static struct usb_ep_descriptor bulk_in_desc = {
	.bLength = sizeof(struct usb_ep_descriptor),
	.bDescriptorType = USB_DESC_ENDPOINT,
	.bEndpointAddress = 0x81U,
	.bmAttributes = USB_EP_TYPE_BULK,
	.wMaxPacketSize = 64U,
	.bInterval = 0U,
};

ZTEST(uhc_agm, test_39_retired_bulk_in_chain_moves_its_payload)
{
	struct uhc_transfer *xfer;
	uintptr_t qhd;
	uintptr_t qtd;
	uint32_t token;

	test_udev_reset();
	test_udev.ep_in[1].desc = &bulk_in_desc;

	xfer = uhc_xfer_alloc_with_buf(UHC_DEV, 0x81, &test_udev, test_xfer_cb, NULL, 64U);
	zassert_not_null(xfer, "bulk IN transfer allocated");
	zassert_ok(uhc_ep_enqueue(UHC_DEV, xfer), "enqueue");

	qhd = async_first_qhd();
	zassert_not_equal(qhd, 0U, "a queue head is armed");
	zassert_equal((rd32(qhd + QHD_EP_CHAR) >> 8) & 0x0fU, 1U, "endpoint 1");
	zassert_equal((rd32(qhd + QHD_EP_CHAR) >> 16) & 0x7ffU, 64U, "mps 64");

	qtd = (uintptr_t)(rd32(qhd + QHD_OVERLAY_NEXT) & ~(uintptr_t)LINK_ALIGN);
	zassert_not_equal(qtd, 0U, "the qTD is attached");

	/* The controller retired the tail qTD with 3 of the 64 bytes left. */
	token = (rd32(qtd + QTD_TOKEN) & ~(uint32_t)QTD_ACTIVE &
		 ~(uint32_t)(QTD_BYTES_MASK << QTD_BYTES_SHIFT)) |
		(3U << QTD_BYTES_SHIFT);
	wr32(qtd + QTD_TOKEN, token);

	uhc_irq();

	zassert_equal(xfer->err, 0, "a retired chain completes without error");
	zassert_equal(xfer->buf->len, 61U,
		      "the remaining qTD count becomes the payload length");
	zassert_equal(rd32(qtd + QTD_WORD1) & QTD_USED, 0U,
		      "the qTD went back to the pool");
	zassert_equal(rd32(qhd + QHD_OVERLAY_NEXT) & 0x1U, 0x1U,
		      "the queue head stops fetching");

	uhc_xfer_buf_free(UHC_DEV, xfer->buf);
	zassert_ok(uhc_xfer_free(UHC_DEV, xfer), "free");
}

ZTEST(uhc_agm, test_40_oversized_transfer_is_refused_instead_of_truncated)
{
	struct uhc_transfer *xfer;

	test_udev_reset();
	test_udev.ep_in[1].desc = &bulk_in_desc;

	xfer = uhc_xfer_alloc_with_buf(UHC_DEV, 0x81, &test_udev, test_xfer_cb, NULL, 64U);
	zassert_not_null(xfer, "bulk IN transfer allocated");

	/*
	 * The qTD byte count is 15 bits, so 32768 would be armed as 0 and the
	 * transfer would "succeed" having moved nothing. No buffer the pool can
	 * hand out today is that big (CONFIG_UHC_BUF_POOL_SIZE tops out at
	 * 32768 and the buffer has to fit inside it), so the shape is built by
	 * hand here: what is under test is the driver's guard, not the pool's
	 * arithmetic.
	 */
	xfer->buf->size = 32768U;

	atomic_clear(&evt_count);
	zassert_ok(uhc_ep_enqueue(UHC_DEV, xfer), "enqueue");

	zassert_equal(xfer->err, -EINVAL, "a transfer longer than the qTD count is refused");
	zassert_equal(xfer->queued, 0U, "and handed straight back");
	zassert_equal(atomic_get(&evt_count), 1, "the owner is told once");
	zassert_equal(rd(R_USBINTR), INTR_MASK, "the controller keeps running");

	uhc_xfer_buf_free(UHC_DEV, xfer->buf);
	zassert_ok(uhc_xfer_free(UHC_DEV, xfer), "free");
}

/* ---- teardown ---------------------------------------------------------- */

ZTEST(uhc_agm, test_50_disable_and_shutdown)
{
	zassert_ok(uhc_disable(UHC_DEV), "uhc_disable()");
	zassert_equal(rd(R_USBCMD) & CMD_RS, 0U, "disable stops the controller");
	zassert_equal(rd(R_USBINTR), 0U, "and masks every interrupt");
	zassert_equal(rd(R_PORTSC) & PORTSC_PP, 0U, "and drops port power");

	zassert_ok(uhc_shutdown(UHC_DEV), "uhc_shutdown()");
	zassert_not_equal(rd(R_ASYNCLISTADDR), 0U, "shutdown rebuilds the schedules");
}

/*
 * Device-removal path. The port-change ISR is not reachable from the fake
 * window, but it calls the same sched_reset() that shutdown() does: a transfer
 * that is still in flight when the schedule is dropped has to be reported, or
 * a class (a bulk read, an interrupt poll) waits for a completion that can
 * never come -- the host stack's own control requests do have a timeout, a
 * class has none. Leaves the controller shut down; the ring-walk case below
 * brings it back up.
 */
ZTEST(uhc_agm, test_60_in_flight_transfer_is_reported_when_the_schedule_is_dropped)
{
	static const uint8_t setup[8] = { 0x00, 0x05, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00 };
	static int event_ctx;
	struct uhc_transfer *xfer;
	uintptr_t qhd;

	zassert_ok(uhc_init(UHC_DEV, test_event_cb, &event_ctx), "init again after shutdown");
	zassert_ok(uhc_enable(UHC_DEV), "enable");

	test_udev_reset();

	xfer = uhc_xfer_alloc(UHC_DEV, 0x00, &test_udev, test_xfer_cb, NULL);
	zassert_not_null(xfer, "control transfer allocated");
	memcpy(xfer->setup_pkt, setup, sizeof(setup));
	zassert_ok(uhc_ep_enqueue(UHC_DEV, xfer), "enqueue");

	qhd = async_first_qhd();
	zassert_not_equal(qhd, 0U, "the transfer is armed when the device goes away");

	atomic_clear(&evt_count);
	last_evt = -1;

	zassert_ok(uhc_disable(UHC_DEV), "disable");
	zassert_ok(uhc_shutdown(UHC_DEV), "shutdown drops the schedule");

	zassert_equal(xfer->err, -ESHUTDOWN, "the in-flight transfer is reported as gone");
	zassert_equal(xfer->queued, 0U, "and leaves the core list");
	zassert_equal(last_evt, (int)UHC_EVT_EP_REQUEST, "the completion event was delivered");
	zassert_equal(atomic_get(&evt_count), 1, "exactly once");
	zassert_equal(async_first_qhd(), 0U, "the rebuilt schedule dropped the queue head");

	zassert_ok(uhc_xfer_free(UHC_DEV, xfer), "free");
}

/*
 * The async ring walk runs inside the ISR, following queue-head link pointers.
 * The driver bounds it (one step per slot plus the head); this case builds the
 * shape the bound exists for -- a link cycle that never reaches the head --
 * and asserts the handler comes back at all. Without the bound the walk would
 * spin here forever, which shows up as a suite timeout rather than a failed
 * assertion, and the assertion below pins the other half: the ring is left
 * exactly as it was found.
 */
ZTEST(uhc_agm, test_61_async_ring_walk_is_bounded)
{
	/* SET_CONFIGURATION(1): a control transfer with no data stage that is
	 * not SET_ADDRESS, so the completion is reported straight away. */
	static const uint8_t setup[8] = { 0x00, 0x09, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00 };
	static int event_ctx;
	struct uhc_transfer *xfer;
	uintptr_t head;
	uintptr_t qhd;
	uintptr_t qtd;
	uint32_t head_next;
	uint32_t qhd_next;

	zassert_ok(uhc_init(UHC_DEV, test_event_cb, &event_ctx), "init again");
	zassert_ok(uhc_enable(UHC_DEV), "enable");

	test_udev_reset();

	xfer = uhc_xfer_alloc(UHC_DEV, 0x00, &test_udev, test_xfer_cb, NULL);
	zassert_not_null(xfer, "control transfer allocated");
	memcpy(xfer->setup_pkt, setup, sizeof(setup));
	zassert_ok(uhc_ep_enqueue(UHC_DEV, xfer), "enqueue");

	head = async_head();
	qhd = async_first_qhd();
	zassert_not_equal(qhd, 0U, "the ring has the endpoint queue head in it");

	head_next = rd32(head + QHD_NEXT);
	qhd_next = rd32(qhd + QHD_NEXT);

	/* head -> qhd -> qhd: a cycle the walk can never leave. */
	wr32(head + QHD_NEXT, (uint32_t)qhd | (1U << 1));
	wr32(qhd + QHD_NEXT, (uint32_t)qhd | (1U << 1));

	uhc_irq();

	/* The bound stopped the walk where it was -- the ring itself is only
	 * read, never repaired, so the cycle is still there. */
	zassert_equal(rd32(qhd + QHD_NEXT), (uint32_t)qhd | (1U << 1),
		      "the walk stopped at its bound without touching the ring");

	/* And the schedule is usable again once the link is put back. */
	wr32(head + QHD_NEXT, head_next);
	wr32(qhd + QHD_NEXT, qhd_next);
	zassert_equal(rd32(head + QHD_NEXT), head_next, "the ring is put back");

	qtd = (uintptr_t)(rd32(qhd + QHD_OVERLAY_NEXT) & ~(uintptr_t)LINK_ALIGN);
	zassert_not_equal(qtd, 0U, "the transfer is still armed");

	/* The chain is SETUP -> STATUS: the completion verdict looks at the
	 * *last* link, so retire that one. */
	if ((rd32(qtd + QTD_NEXT) & 0x1U) == 0U) {
		qtd = (uintptr_t)(rd32(qtd + QTD_NEXT) & ~(uintptr_t)LINK_ALIGN);
	}
	wr32(qtd + QTD_TOKEN, rd32(qtd + QTD_TOKEN) & ~(uint32_t)QTD_ACTIVE);

	uhc_irq();

	zassert_equal(xfer->err, 0, "the ring still completes transfers afterwards");
	zassert_equal(xfer->queued, 0U, "and the transfer left the core list");
	zassert_ok(uhc_xfer_free(UHC_DEV, xfer), "free");

	zassert_ok(uhc_disable(UHC_DEV), "disable");
	zassert_ok(uhc_shutdown(UHC_DEV), "shutdown");
}
