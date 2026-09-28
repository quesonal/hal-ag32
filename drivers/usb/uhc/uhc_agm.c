/*
 * Copyright (c) 2026 AgRV Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * AgRV2K USB0 host controller driver.
 *
 * Hardware: the same FSL/ChipIdea-like dual-role controller that
 * drivers/usb/udc/udc_agm.c drives in device mode, here driven in host mode.
 * The host-side register interface is EHCI-compatible: USBCMD/USBSTS/PORTSC,
 * the asynchronous queue-head ring (ASYNCLISTADDR 0x158) for control/bulk
 * endpoints and the periodic frame list (PERIODICLISTBASE 0x154) for
 * interrupt endpoints.
 *
 * Algorithm: a register-for-register port of the vendor's own host stack --
 * framework-agrv_tinyusb/src/portable/ehci/ehci.c (TinyUSB's ChipIdea/EHCI
 * host driver) on top of the SDK bring-up in framework-agrv_sdk/src/usb.c
 * (USB_InitHost(): reset -> USBMODE.CM = host -> interrupt mask -> run; the
 * vendor hcd then hands &USB0->CAPLENGTH and &USB0->USBCMD to that driver).
 * Everything not forced by the Zephyr UHC API (uhc.h) follows that
 * implementation on purpose, so a stack-level failure can be diffed
 * against a known-working one instead of re-derived from the EHCI spec.
 *
 * Scope: full speed only (the block has no HS PHY; the binding is
 * "full-speed"), one root port, no external hubs, control + bulk + interrupt
 * transfers, no isochronous scheduling. One transfer is in flight per
 * endpoint; further transfers for the same endpoint wait in the UHC core's
 * transfer list (uhc_xfer_append()) and are started from the completion path,
 * the same shape uhc_dwc2 and uhc_max3421e use.
 *
 * Threading: init() does the controller bring-up (schedules, addresses, port
 * power) with the interrupt masked; enable() unmasks and starts the
 * schedules. All schedule and transfer work happens in the ISR. The static
 * queue-head/qTD pool and the shared transfer list are the only state shared
 * with the enqueue path, which holds the UHC mutex the ISR cannot take, so
 * every mutation there is wrapped in irq_lock(). Completions go back through
 * uhc_xfer_return(), which the UHC core turns into an event on its own
 * message queue -- no work queue is needed.
 *
 * Device removal: a port change rebuilds both schedules and the pools from
 * scratch (sched_reset()), then reports every transfer still on the core's
 * lists as -ESHUTDOWN. The callback for those runs on the host stack's own
 * thread, so a class sees exactly one completion per transfer it submitted --
 * including the ones that were mid-flight or waiting to be armed -- and can
 * free them there. A class must therefore not touch its transfer again from
 * its removed() callback: that one runs on the bus thread and the two orders
 * are both legal.
 *
 * The development bench log -- what was tried, what the numbers came
 * out, which bug each round fixed -- is in the commit history. The
 * conclusions that shaped the code are in the comments below.
 */

#define DT_DRV_COMPAT agm_agrv2k_uhc

#include "uhc_agm.h"
#include "uhc_common.h"

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/init.h>
#include <zephyr/sys/util.h>
#include <zephyr/drivers/usb/uhc.h>
#include <zephyr/usb/usb_ch9.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(uhc_agm, CONFIG_UHC_DRIVER_LOG_LEVEL);

/* Queue-head / qTD pools. One queue head per endpoint, one qTD per transfer
 * and a second one for the data stage of a control transfer. A control
 * transfer needs three qTDs in one chain (SETUP, DATA, STATUS) and EP0 has to
 * have a queue head of its own, so the pools cannot be smaller than that. */
#define UHC_AGM_QHD_MAX		8U
#define UHC_AGM_QTD_MAX		16U
#define UHC_AGM_PERIOD_HEADS	4U

BUILD_ASSERT(UHC_AGM_QHD_MAX >= 2U, "EP0 plus at least one more endpoint");
BUILD_ASSERT(UHC_AGM_QTD_MAX >= 3U, "a control transfer needs three qTDs");

/* The asynchronous ring is its head plus at most one queue head per slot, so
 * walking it back to the head takes UHC_AGM_QHD_MAX + 1 steps (2 spare). */
#define UHC_AGM_RING_WALK_MAX	(UHC_AGM_QHD_MAX + 2U)

/* The ISR acks, processes and re-reads in a loop (see uhc_agm_isr()); the cap
 * covers every documented source arriving back to back in one entry. */
#define UHC_AGM_ISR_ROUNDS	16U

/* Bounded register poll; the same shape and the same bound udc_agm's
 * usb_wait_clear() uses. Every wait that can be answered by the controller is
 * bounded, so a missing handshake logs and returns instead of hanging -- and
 * the native_sim suite runs on this bound (a shorter one never gives the
 * model's timer a chance to answer; udc_agm's native model needs the same).
 * That is the budget for the thread-context callers: bring-up, disable and
 * shutdown can afford to be patient. */
#define UHC_AGM_WAIT_SPINS	1000000U
/*
 * The port-change ISR cannot be: there every spin burns time with interrupts
 * masked. The schedule-status bits (PS/AS) are dropped by the very USBCMD
 * write that sched_reset() starts with, so 2 ms is already orders of magnitude
 * above the hardware's worst case; the cap is only there so a bit that never
 * clears cannot freeze the system for the full second.
 */
#define UHC_AGM_WAIT_SPINS_ISR	2000U

/*
 * One qTD carries a 15-bit byte count (EHCI 3.5.3, and struct
 * uhc_agm_qtd::total_bytes). A longer transfer has to be chained, which this
 * driver does not do (the UHC buffer pool is smaller than this today), so a
 * class asking for more is refused instead of silently truncated: 32768
 * bytes would be armed as 0, and the transfer would "succeed" with no data.
 */
#define UHC_AGM_QTD_MAX_BYTES	32767U

/* USB 2.0 7.1.7.5: a root-port reset has to be held for at least 10 ms. */
#define UHC_AGM_RESET_HOLD_MS	15

/*
 * ... and the device then gets TRSTRCY of recovery time before the host may
 * send the first transaction. The NXP EHCI HAL waits 11 ms after PORTSC.PR
 * clears (USB_HOST_EHCI_PORT_RESET_DELAY) and Zephyr's uhc_dwc2 waits 30 ms
 * (CONFIG_UHC_DWC2_RESET_RECOVERY_MS); skipping it made the very first
 * GET_DESCRIPTOR time out on a card reader, while a mouse receiver happened to
 * tolerate it. Cheap insurance either way: once per plug-in.
 */
#define UHC_AGM_RESET_RECOVERY_MS	20

/*
 * USB 2.0 Table 7-14: after the status stage of SET_ADDRESS a device is
 * allowed TDRSTR (2 ms) before it has to answer at its new address. The
 * Zephyr host stack sends the next request the moment the completion is
 * handed back -- microseconds after the status stage -- and the wireless
 * receiver used that window and looked dead at its new address
 * (SETUP to it got no response at all, three retries, xact_err). Hold the
 * SET_ADDRESS completion back so the device is inside its contract.
 */
#define UHC_AGM_SET_ADDRESS_DELAY	K_MSEC(10)

/*
 * Interrupt set: transfer completion (UI), transfer error (UEI), port change
 * (PCI), frame-list rollover (FRI), host system error (SEI) and async
 * advance (AAI). This is TinyUSB's ehci inten value on this IP; USBSTS and
 * USBINTR share bit positions.
 */
#define UHC_AGM_USBINTR_MASK						\
	(AGM_USB_STS_UI | AGM_USB_STS_UEI | AGM_USB_STS_PCI |		\
	 AGM_USB_STS_FRI | AGM_USB_STS_SEI | AGM_USB_STS_AAI)

/*
 * Software state for one queue head, in a parallel array so the 64-byte
 * hardware layout (and therefore the 32-byte link alignment) is untouched.
 *
 * A control transfer carries its whole qTD chain at once -- SETUP -> DATA ->
 * STATUS, linked through the qTD "next" pointers and handed to the queue head
 * in one go (the shape Linux's EHCI driver uses). Two findings decided
 * that, and both are worth keeping:
 *
 *   1. The controller does not run the status stage on its own. With only
 *      SETUP+DATA queued the device answered both, but SET_ADDRESS never took
 *      effect: a SETUP to the freshly assigned address got no response
 *      (xact_err with two retries) because the zero-length status packet was
 *      never sent.
 *   2. Queuing one stage at a time (TinyUSB's staged control flow) does not
 *      restart an *idle* queue head here. The SETUP qTD retired cleanly
 *      (total_bytes 0, err_count untouched, no error bit), the DATA qTD was
 *      linked into the overlay, and then no interrupt arrived for the whole
 *      5 s chapter-9 timeout.
 */
struct uhc_agm_slot {
	struct uhc_agm_qhd *qhd;
	struct usb_device *udev;
	struct uhc_transfer *xfer;
	struct uhc_agm_qtd *qtd;	/* chain head; freed by walking it */
	struct uhc_agm_qtd *qtd_data;	/* data-stage qTD, for IN accounting */
	struct uhc_agm_qtd *qtd_tail;	/* last qTD: its Active bit ends the xfer */
	uint8_t used;
	uint8_t removing;
	uint8_t busy;
	uint8_t deferred;	/* retired, waiting for a delayed return */
	uint8_t pid;
	uint8_t ep_num;
	uint8_t ep_addr;
	uint8_t type;
	uint8_t interval_ms;
};

/*
 * One 4K-aligned block holds everything the controller walks by physical
 * address: the frame list must be 4K aligned and every queue head (and
 * standalone qTD) must be 32-byte aligned, so the members are declared in an
 * order that keeps each of them aligned by construction:
 *   framelist    @ 0    (4K aligned, 8 entries)
 *   period_head  @ 32   (32 * 1)
 *   async_head   @ 288  (32 * 9)
 *   qhd[]        @ 352  (32 * 11)
 *   qtd[]        @ 864  (32 * 27)
 */
struct uhc_agm_pool {
	union uhc_agm_link framelist[UHC_AGM_FRAMELIST_SIZE];
	struct uhc_agm_qhd period_head[UHC_AGM_PERIOD_HEADS];
	struct uhc_agm_qhd async_head;
	struct uhc_agm_qhd qhd[UHC_AGM_QHD_MAX];
	struct uhc_agm_qtd qtd[UHC_AGM_QTD_MAX];
};

/*
 * The controller walks these by physical address: a slipped layout or a
 * broken alignment cannot be caught at runtime (the queue head just never
 * retires), so the sizes and offsets above are pinned at build time.
 */
BUILD_ASSERT(sizeof(struct uhc_agm_qtd) == 32U, "EHCI qTD is 32 bytes");
BUILD_ASSERT(sizeof(struct uhc_agm_qhd) == 64U,
	     "EHCI queue head is 48 bytes plus 16 reserved");
BUILD_ASSERT(offsetof(struct uhc_agm_qhd, overlay) == 16U,
	     "the transfer overlay starts at word 4");
BUILD_ASSERT(offsetof(struct uhc_agm_qtd, token) == 8U, "the token is word 2");
BUILD_ASSERT(offsetof(struct uhc_agm_pool, period_head) == 32U,
	     "the frame list is 8 entries of 4 bytes");
BUILD_ASSERT(offsetof(struct uhc_agm_pool, async_head) == 288U,
	     "4 periodic heads after the frame list");
BUILD_ASSERT(offsetof(struct uhc_agm_pool, qhd) == 352U,
	     "the async head follows the periodic heads");
BUILD_ASSERT(offsetof(struct uhc_agm_pool, qtd) == 864U,
	     "the queue heads follow the async head");

struct uhc_agm_data {
	struct uhc_agm_pool pool;
	struct uhc_agm_slot slot[UHC_AGM_QHD_MAX];
	const struct device *dev;
	struct k_work_delayable addr_work;
	struct uhc_transfer *addr_xfer;
	uint32_t uframe;
	bool enabled;
	bool connected;
	/* One bit per uhc_agm_warn_* condition: each of those means "the driver
	 * is about to misbehave or a device is misbehaving", and the ISR can be
	 * re-entered immediately while one of them holds, so they are reported
	 * once instead of once per occurrence. Which "once" applies depends on
	 * the condition -- see sched_reset(), which re-arms the per-device ones
	 * for the next device. */
	uint8_t warned;
};

struct uhc_agm_config {
	volatile struct udc_agm_regs *regs;
	void (*irq_config_func)(const struct device *dev);
};

/* Conditions that should never happen and are worth one line in the log; see
 * uhc_agm_data::warned. */
enum uhc_agm_warn {
	UHC_AGM_WARN_STICKY_STS	= BIT(0),
	UHC_AGM_WARN_RING	= BIT(1),
	UHC_AGM_WARN_POOL	= BIT(2),
};

/* Claim the one report a warn condition gets. Runs from the ISR as well; the
 * read-modify-write is a single byte and a duplicate line from a race would be
 * harmless, so no lock is taken. */
static bool take_warn(struct uhc_agm_data *priv, uint8_t flag)
{
	if ((priv->warned & flag) != 0U) {
		return false;
	}

	priv->warned |= flag;

	return true;
}

/* ---- register access -------------------------------------------------- */

static inline volatile struct udc_agm_regs *agm_regs(const struct device *dev)
{
	return ((const struct uhc_agm_config *)dev->config)->regs;
}

#define R(dev, field)		(agm_regs(dev)->field)
#define W(dev, field, val)	(agm_regs(dev)->field = (val))

static bool agm_wait_mask(const struct device *dev, uint32_t off,
			  uint32_t mask, uint32_t want, uint32_t spins)
{
	volatile uint32_t *reg =
		(volatile uint32_t *)((uintptr_t)agm_regs(dev) + off);

	for (uint32_t i = spins; i > 0U; i--) {
		if ((*reg & mask) == want) {
			return true;
		}
		k_busy_wait(1);
	}

	return false;
}

#define agm_wait_clear(dev, off, mask) \
	agm_wait_mask(dev, off, mask, 0U, UHC_AGM_WAIT_SPINS)

/* ---- schedule primitives --------------------------------------------- */

static inline bool link_terminated(const union uhc_agm_link *link)
{
	return (link->address & 0x1U) != 0U;
}

static inline struct uhc_agm_qhd *link_to_qhd(const union uhc_agm_link *link)
{
	return (struct uhc_agm_qhd *)(uintptr_t)(link->address & ~0x1FU);
}

static inline struct uhc_agm_qhd *qhd_next(const struct uhc_agm_qhd *qhd)
{
	return (struct uhc_agm_qhd *)(uintptr_t)(qhd->next.address & ~0x1FU);
}

/* Insert a queue head after "head" (link type field 1 = queue head). */
static void link_insert(union uhc_agm_link *head, struct uhc_agm_qhd *qhd)
{
	qhd->next.address = head->address;
	head->address = (uint32_t)(uintptr_t)qhd | (1U << 1);
}

/* Periodic-list bucket for a polling interval in ms: index = log2(interval),
 * clamped to the 8 ms frame-list size (TinyUSB's list_get_period_head). An
 * endpoint that asked for more than 8 ms lands in the 8 ms bucket, so it ends
 * up polled more often than it asked for -- never less. */
static uint32_t period_idx(uint32_t interval_ms)
{
	uint32_t idx = 0U;

	interval_ms = CLAMP(interval_ms, 1U, UHC_AGM_FRAMELIST_SIZE);
	while (interval_ms > 1U) {
		interval_ms >>= 1;
		idx++;
	}

	return MIN(idx, UHC_AGM_PERIOD_HEADS - 1U);
}

static void sched_init(const struct device *dev)
{
	struct uhc_agm_data *priv = uhc_get_private(dev);

	/* Asynchronous ring: the head is its own next queue head, is flagged
	 * "head of list" and sits idle (halted) until a transfer is added. */
	memset(&priv->pool.async_head, 0, sizeof(priv->pool.async_head));
	priv->pool.async_head.next.address =
		(uint32_t)(uintptr_t)&priv->pool.async_head | (1U << 1);
	priv->pool.async_head.head_list_flag = 1U;
	priv->pool.async_head.overlay.halted = 1U;
	priv->pool.async_head.overlay.next.address = 0x1U;

	/* Periodic tree with only 1/2/4/8 ms buckets: every frame list entry
	 * points at the 1 ms head, and the coarser heads are spliced in front
	 * of it at the matching stride (TinyUSB's init_periodic_list). */
	memset(priv->pool.period_head, 0, sizeof(priv->pool.period_head));
	for (uint32_t i = 0U; i < UHC_AGM_PERIOD_HEADS; i++) {
		/* A queue head in the periodic list must have a non-zero
		 * interrupt schedule mask, and the dummy head is always
		 * inactive. */
		priv->pool.period_head[i].int_smask = 1U;
		priv->pool.period_head[i].overlay.halted = 1U;
	}

	for (uint32_t i = 0U; i < UHC_AGM_FRAMELIST_SIZE; i++) {
		priv->pool.framelist[i].address =
			(uint32_t)(uintptr_t)&priv->pool.period_head[0] | (1U << 1);
	}
	for (uint32_t i = 0U; i < UHC_AGM_FRAMELIST_SIZE; i += 2U) {
		link_insert(&priv->pool.framelist[i], &priv->pool.period_head[1]);
	}
	for (uint32_t i = 1U; i < UHC_AGM_FRAMELIST_SIZE; i += 4U) {
		link_insert(&priv->pool.framelist[i], &priv->pool.period_head[2]);
	}
	link_insert(&priv->pool.framelist[3], &priv->pool.period_head[3]);

	priv->pool.period_head[0].next.terminate = 1U;
}

/* Hand back every transfer that was still on one of the core's lists. Called
 * with the schedule already torn down: none of these transfers can complete
 * any more, so the owner has to be told. `uhc_xfer_return()` is the same call
 * the completion path makes from the ISR -- it only unlinks the transfer and
 * queues an event, the class's callback runs on the host stack's own thread
 * afterwards, where the device life cycle may be touched safely. */
static void sched_reset_xfers(const struct device *dev, sys_dlist_t *list)
{
	struct uhc_transfer *xfer;
	struct uhc_transfer *tmp;

	SYS_DLIST_FOR_EACH_CONTAINER_SAFE(list, xfer, tmp, node) {
		LOG_DBG("ep 0x%02x dropped with the schedule", xfer->ep);
		uhc_xfer_return(dev, xfer, -ESHUTDOWN);
	}
}

/* Drop every endpoint from both schedules and start over. Cheaper and more
 * obviously correct than unlinking one queue head at a time, and correct
 * here because this host supports exactly one root device and no hubs.
 *
 * `spins` is the poll budget for the "schedule status cleared" wait: the
 * port-change path calls this from the ISR and passes the short bound (the
 * wait itself is redundant there -- PS/AS are dropped by the USBCMD write
 * below -- and it must not burn a second with interrupts masked), the
 * thread-context callers pass the long one. */
static void sched_reset(const struct device *dev, uint32_t spins)
{
	struct uhc_agm_data *priv = uhc_get_private(dev);
	struct uhc_data *data = dev->data;

	/*
	 * A SET_ADDRESS completion can be sitting in the delayed work while the
	 * device goes away. Drop the single-tenant pointer first so the delayed
	 * work finds nothing left to do; the transfer itself is still on the
	 * control list and is reported by the walk at the end of this function.
	 */
	priv->addr_xfer = NULL;

	/*
	 * Re-arm the per-device warnings: whether the pools run out and whether
	 * the ring walk goes wrong depends on what the next device asks for, so
	 * that device deserves its own report. The status-bit warning is about
	 * the controller, not about a device, and stays latched for the life of
	 * this driver instance -- otherwise every plug would repeat it.
	 */
	priv->warned &= (uint8_t)UHC_AGM_WARN_STICKY_STS;

	W(dev, USBCMD, R(dev, USBCMD) & ~(AGM_USB_CMD_PSE | AGM_USB_CMD_ASE));
	if (!agm_wait_mask(dev, offsetof(struct udc_agm_regs, USBSTS),
			   AGM_USB_STS_PS | AGM_USB_STS_AS, 0U, spins)) {
		LOG_WRN("schedule status PS|AS did not clear in %u spins, rebuilding anyway",
			spins);
	}

	memset(priv->slot, 0, sizeof(priv->slot));
	memset(priv->pool.qhd, 0, sizeof(priv->pool.qhd));
	memset(priv->pool.qtd, 0, sizeof(priv->pool.qtd));
	sched_init(dev);

	W(dev, ASYNCLISTADDR, (uint32_t)(uintptr_t)&priv->pool.async_head);
	W(dev, PERIODICLISTBASE, (uint32_t)(uintptr_t)priv->pool.framelist);
	W(dev, USBCMD, R(dev, USBCMD) | AGM_USB_CMD_PSE | AGM_USB_CMD_ASE);

	/*
	 * The slot pool is gone, so the core's lists are the only record of
	 * transfers that were in flight (or waiting to be armed) when the
	 * device went away. Without this walk a class transfer -- a bulk read
	 * or an interrupt IN poll -- would never see a completion: the host
	 * stack's control requests recover through their own timeout, a class
	 * has no such net and would wait forever.
	 */
	sched_reset_xfers(dev, &data->ctrl_xfers);
	sched_reset_xfers(dev, &data->bulk_xfers);
}

/* ---- qTD / queue head bookkeeping ------------------------------------ */

static struct uhc_agm_qtd *qtd_alloc(const struct device *dev)
{
	struct uhc_agm_data *priv = uhc_get_private(dev);

	for (uint32_t i = 0U; i < UHC_AGM_QTD_MAX; i++) {
		if (priv->pool.qtd[i].used == 0U) {
			return &priv->pool.qtd[i];
		}
	}

	/* Every qTD is in a chain. The transfer that asked for this one is
	 * reported as -ENOMEM; say where it came from, the owner's error only
	 * says "allocation failed". */
	if (take_warn(priv, UHC_AGM_WARN_POOL)) {
		LOG_WRN("qTD pool exhausted (%u qTDs in use); arming will fail with -ENOMEM",
			UHC_AGM_QTD_MAX);
	}

	return NULL;
}

static void qtd_free(struct uhc_agm_qtd *qtd)
{
	qtd->used = 0U;
	qtd->next.address = 0x1U;
}

/*
 * Arm one qTD. Word 1 doubles as the software "used"/"expected bytes" cache
 * (the controller never writes it); the buffer page list is filled the way
 * the SDK's USB_InitDTD/USB_InitQTD do it so a buffer crossing a 4K page
 * keeps a valid page pointer for every page it spans.
 */
static void qtd_init(struct uhc_agm_qtd *qtd, const void *buf, uint16_t len,
		     uint8_t pid, uint8_t data_toggle)
{
	memset(qtd, 0, sizeof(*qtd));
	qtd->next.address = 0x1U;
	qtd->alternate.address = 0x1U;
	qtd->used = 1U;
	qtd->active = 1U;
	qtd->err_count = 3U;
	qtd->pid = pid;
	qtd->int_on_complete = 1U;
	qtd->data_toggle = data_toggle;
	qtd->total_bytes = len;
	qtd->expected_bytes = len;
	qtd->buffer[0] = (uint32_t)(uintptr_t)buf;
	for (uint8_t i = 1U; i < 5U; i++) {
		qtd->buffer[i] = (qtd->buffer[i - 1U] & ~0xFFFU) + 0x1000U;
	}
}

/* UHC device speed -> queue-head "endpoint speed" field (0 FS / 1 LS / 2 HS,
 * the same encoding PORTSC.PSPD uses). */
static uint8_t to_qhd_speed(enum usb_device_speed speed)
{
	switch (speed) {
	case USB_SPEED_SPEED_LS:
		return AGM_USB_SPEED_LS;
	case USB_SPEED_SPEED_HS:
		return AGM_USB_SPEED_HS;
	default:
		return AGM_USB_SPEED_FS;
	}
}

static void qhd_init(struct uhc_agm_qhd *qhd, struct uhc_transfer *xfer)
{
	uint8_t speed = to_qhd_speed(xfer->udev->speed);
	uint8_t type = xfer->type;

	memset(qhd, 0, sizeof(*qhd));
	qhd->dev_addr = xfer->udev->addr;
	qhd->ep_number = USB_EP_GET_IDX(xfer->ep);
	qhd->ep_speed = speed;
	/* Control endpoints use the queue head's data toggle (it is fixed per
	 * stage); bulk/interrupt carry it in the qTD. */
	qhd->data_toggle_control = (type == USB_EP_TYPE_CONTROL) ? 1U : 0U;
	qhd->max_packet_size = xfer->mps;
	/*
	 * The "full/low-speed control endpoint" flag, as in the vendor stack and
	 * the NXP HAL. Note that it does *not* make the controller produce the
	 * status stage -- that one is queued explicitly (see slot_arm); the
	 * a device otherwise sits stuck waiting for a status stage that
	 * never comes until the status qTD is added.
	 */
	qhd->fl_ctrl_ep_flag =
		(type == USB_EP_TYPE_CONTROL && speed != AGM_USB_SPEED_HS) ? 1U : 0U;
	qhd->nak_reload = 0U;
	qhd->mult = 1U;

	if (type == USB_EP_TYPE_INTERRUPT && speed != AGM_USB_SPEED_HS) {
		/* Full/low-speed interrupt: start-split in microframe 0,
		 * complete-split mask covering microframes 1..3. */
		qhd->int_smask = 0x01U;
		qhd->fl_int_cmask = 0x1CU;
	}

	qhd->overlay.halted = 0U;
	qhd->overlay.next.address = 0x1U;
	qhd->overlay.alternate.address = 0x1U;
}

/* Find the software slot backing a queue-head pointer, or NULL when the
 * pointer is not one of the endpoint queue heads (async/periodic heads). */
static struct uhc_agm_slot *slot_for_qhd(struct uhc_agm_data *priv,
					 struct uhc_agm_qhd *qhd)
{
	uintptr_t base = (uintptr_t)priv->pool.qhd;
	uintptr_t p = (uintptr_t)qhd;

	if (p < base || p >= base + sizeof(priv->pool.qhd)) {
		return NULL;
	}

	return &priv->slot[(p - base) / sizeof(priv->pool.qhd[0])];
}

/*
 * Look up (or, when asked, create) the slot an endpoint address maps to.
 *
 * The queue head's device address is part of the key on purpose. The
 * controller caches a queue head's static fields, so patching the address of
 * a queue head that is already in the schedule does not reach the wire -- the
 * effect: after SET_ADDRESS the device answers neither its old nor its
 * new address, because the wire still carried the old one.
 * TinyUSB's host stack (the vendor's, host/usbh.c) re-opens the control
 * endpoint with the new address instead -- usbh_edpt_control_open(new_addr,
 * ...) right after SET_ADDRESS -- which hands a *fresh* queue head to the
 * controller; a key that includes the address gives this driver the same
 * behaviour. (Zephyr's host stack has no equivalent call: nothing in
 * subsys/usb/host opens or closes an endpoint.)
 */
static struct uhc_agm_slot *slot_get(struct uhc_agm_data *priv,
				     struct uhc_transfer *xfer, bool alloc)
{
	uint8_t ep_num = USB_EP_GET_IDX(xfer->ep);
	uint8_t dev_addr = xfer->udev->addr;

	for (uint32_t i = 0U; i < UHC_AGM_QHD_MAX; i++) {
		struct uhc_agm_slot *slot = &priv->slot[i];

		if (slot->used == 0U || slot->removing != 0U) {
			continue;
		}
		if (slot->udev != xfer->udev || slot->ep_num != ep_num ||
		    slot->qhd->dev_addr != dev_addr) {
			continue;
		}
		/* Endpoint 0 serves both directions from one queue head. */
		if (ep_num != 0U && slot->ep_addr != xfer->ep) {
			continue;
		}

		return slot;
	}

	if (!alloc) {
		return NULL;
	}

	for (uint32_t i = 0U; i < UHC_AGM_QHD_MAX; i++) {
		struct uhc_agm_slot *slot = &priv->slot[i];

		if (slot->used != 0U || slot->removing != 0U) {
			continue;
		}

		memset(slot, 0, sizeof(*slot));
		slot->qhd = &priv->pool.qhd[i];
		slot->udev = xfer->udev;
		slot->ep_num = ep_num;
		slot->ep_addr = xfer->ep;
		slot->type = xfer->type;
		slot->interval_ms =
			period_idx(xfer->interval != 0U ? xfer->interval : 1U);
		slot->pid = USB_EP_DIR_IS_IN(xfer->ep) ? UHC_AGM_PID_IN
						       : UHC_AGM_PID_OUT;
		slot->used = 1U;
		qhd_init(slot->qhd, xfer);

		if (slot->type == USB_EP_TYPE_INTERRUPT) {
			link_insert(&priv->pool.period_head[slot->interval_ms].next,
				    slot->qhd);
		} else {
			link_insert(&priv->pool.async_head.next, slot->qhd);
		}

		return slot;
	}

	return NULL;
}

/* ---- transfer path ---------------------------------------------------- */

/* Free a whole qTD chain, following the "next" links. */
static void qtd_chain_free(struct uhc_agm_qtd *head)
{
	while (head != NULL) {
		struct uhc_agm_qtd *next;

		next = link_terminated(&head->next)
			       ? NULL
			       : (struct uhc_agm_qtd *)(uintptr_t)
					 (head->next.address & ~0x1FU);

		qtd_free(head);
		head = next;
	}
}

static void slot_release(struct uhc_agm_slot *slot)
{
	unsigned int key = irq_lock();

	/*
	 * Retire the queue head *before* the qTDs go back to the pool. The
	 * queue head stays in the schedule until the device is removed, so the
	 * controller may still be looking at it; invalidating the pointers
	 * first means it can never walk into memory this slot no longer owns.
	 */
	slot->qhd->current = 0x1U;
	slot->qhd->overlay.next.address = 0x1U;

	qtd_chain_free(slot->qtd);

	slot->xfer = NULL;
	slot->qtd = NULL;
	slot->qtd_data = NULL;
	slot->qtd_tail = NULL;
	slot->busy = 0U;
	slot->deferred = 0U;
	slot->removing = 0U;

	irq_unlock(key);
}

/* Build the qTD chain for one transfer and hand it to the controller. */
static int slot_arm(const struct device *dev, struct uhc_agm_slot *slot,
		    struct uhc_transfer *xfer)
{
	struct uhc_agm_qtd *head = NULL;
	struct uhc_agm_qtd *tail = NULL;
	bool req_in = USB_EP_DIR_IS_IN(xfer->ep);
	uint32_t payload;

	/*
	 * The qTD byte count is 15 bits wide and this driver arms one qTD per
	 * transfer, so anything longer would be truncated -- 32768 bytes
	 * becomes 0 and the transfer "succeeds" having moved nothing, which is
	 * exactly the silent-short-read failure this project refuses to ship.
	 * The control data stage and the bulk/interrupt payload pick the same
	 * number below, so one check covers both.
	 */
	if (xfer->buf != NULL) {
		payload = req_in ? xfer->buf->size : xfer->buf->len;
		if (payload > UHC_AGM_QTD_MAX_BYTES) {
			LOG_ERR("ep 0x%02x: %u-byte transfer exceeds the 15-bit qTD count",
				xfer->ep, payload);
			return -EINVAL;
		}
	}

	if (xfer->type == USB_EP_TYPE_CONTROL) {
		/* SETUP: 8 bytes, DATA0. */
		head = qtd_alloc(dev);
		if (head == NULL) {
			return -ENOMEM;
		}
		qtd_init(head, xfer->setup_pkt, 8U, UHC_AGM_PID_SETUP, 0U);
		tail = head;

		/* DATA: the chapter-9 request's own direction, starting at DATA1.
		 * A control request with wLength 0 has no data stage. */
		if (xfer->buf != NULL && xfer->buf->size > 0U) {
			struct uhc_agm_qtd *data = qtd_alloc(dev);

			if (data == NULL) {
				qtd_chain_free(head);
				return -ENOMEM;
			}
			qtd_init(data, xfer->buf->data,
				 req_in ? xfer->buf->size : xfer->buf->len,
				 req_in ? UHC_AGM_PID_IN : UHC_AGM_PID_OUT, 1U);
			tail->next.address = (uint32_t)(uintptr_t)data;
			tail = data;
			slot->qtd_data = data;
		}

		/* STATUS: zero-length packet, opposite direction, DATA1. The
		 * controller does not generate this one by itself. */
		{
			struct uhc_agm_qtd *status = qtd_alloc(dev);

			if (status == NULL) {
				qtd_chain_free(head);
				return -ENOMEM;
			}
			qtd_init(status, NULL, 0U,
				 req_in ? UHC_AGM_PID_OUT : UHC_AGM_PID_IN, 1U);
			tail->next.address = (uint32_t)(uintptr_t)status;
			tail = status;
		}
	} else {
		if (xfer->buf == NULL) {
			return -EINVAL;
		}
		head = qtd_alloc(dev);
		if (head == NULL) {
			return -ENOMEM;
		}
		/* The endpoint's own direction, toggle carried in the qTD. */
		qtd_init(head, xfer->buf->data,
			 USB_EP_DIR_IS_IN(xfer->ep) ? xfer->buf->size
						    : xfer->buf->len,
			 slot->pid, 0U);
		tail = head;
	}

	slot->qtd = head;
	slot->qtd_tail = tail;

	/* The queue head can be left halted by an earlier error, and this host
	 * has no clear-stall path, so it is cleared before the chain goes in. */
	if (slot->qhd->overlay.halted) {
		slot->qhd->overlay.halted = 0U;
	}
	slot->qhd->overlay.next.address = (uint32_t)(uintptr_t)head;

	LOG_DBG("armed ep 0x%02x type %u chain @%p..%p head len %u", xfer->ep,
		xfer->type, (void *)head, (void *)tail, head->expected_bytes);

	slot->xfer = xfer;
	slot->busy = 1U;

	return 0;
}

/* Arm every transfer waiting on one of the core's lists whose endpoint is
 * free. Transfers that cannot be started stay where they are and are picked up
 * from the next completion. */
static void submit_list(const struct device *dev, struct uhc_agm_data *priv,
			sys_dlist_t *list)
{
	struct uhc_transfer *xfer;
	struct uhc_transfer *tmp;

	SYS_DLIST_FOR_EACH_CONTAINER_SAFE(list, xfer, tmp, node) {
		struct uhc_agm_slot *slot = slot_get(priv, xfer, true);
		int err;

		if (slot == NULL) {
			/* No queue head for this endpoint: every slot is held by
			 * another one. Slots are only recycled when the device is
			 * removed, so this transfer can stay queued for the rest
			 * of the session -- worth a line in the log. */
			if (take_warn(priv, UHC_AGM_WARN_POOL)) {
				LOG_WRN("ep 0x%02x waiting: all %u queue heads are in use",
					xfer->ep, UHC_AGM_QHD_MAX);
			}
			continue;
		}

		if (slot->busy != 0U) {
			/* Endpoint busy: it stays queued and is picked up from
			 * the next completion. */
			continue;
		}

		if (xfer->err != 0) {
			/* Cancelled before it reached the wire. */
			uhc_xfer_return(dev, xfer, xfer->err);
			continue;
		}

		err = slot_arm(dev, slot, xfer);
		if (err != 0) {
			/* The transfer never reached the wire, so its owner gets
			 * the reason slot_arm() gave (-ENOMEM / -EINVAL). */
			LOG_ERR("Failed to arm ep 0x%02x (%d)", xfer->ep, err);
			uhc_xfer_return(dev, xfer, err);
		}
	}
}

/* Start every queued transfer whose endpoint is free. Called from the
 * enqueue path (UHC mutex held) and from the ISR (no mutex), so the whole
 * scan runs with interrupts locked. */
static void submit_pending(const struct device *dev)
{
	struct uhc_data *data = dev->data;
	struct uhc_agm_data *priv = uhc_get_private(dev);
	unsigned int key = irq_lock();

	/*
	 * The core's queue helper links a transfer into one of these two lists
	 * (`uhc_xfer_return()` later unlinks it from whichever it is on), and
	 * today it sends every transfer type to the control list, leaving the
	 * bulk list unused -- which is why the in-tree UHC drivers only look at
	 * the first one. Scanning both costs an empty pass and keeps this
	 * driver working if bulk transfers ever move to the list that was
	 * reserved for them; `uhc_xfer_get_next()` already peeks control first
	 * and bulk second, so the order below matches the core's own.
	 */
	submit_list(dev, priv, &data->ctrl_xfers);
	submit_list(dev, priv, &data->bulk_xfers);

	irq_unlock(key);
}

/* Hand a held-back SET_ADDRESS completion to the host stack. */
static void set_address_delay_done(struct k_work *work)
{
	struct k_work_delayable *dwork = k_work_delayable_from_work(work);
	struct uhc_agm_data *priv =
		CONTAINER_OF(dwork, struct uhc_agm_data, addr_work);
	struct uhc_transfer *xfer = priv->addr_xfer;

	/*
	 * Single tenant by construction: the host stack runs one control
	 * transfer at a time, and every path that could take this transfer away
	 * (dequeue, device removal) clears the pointer before freeing anything.
	 */
	priv->addr_xfer = NULL;
	if (xfer == NULL) {
		return;
	}

	for (uint32_t i = 0U; i < UHC_AGM_QHD_MAX; i++) {
		struct uhc_agm_slot *slot = &priv->slot[i];

		if (slot->used != 0U && slot->xfer == xfer) {
			slot_release(slot);
			break;
		}
	}

	uhc_xfer_return(priv->dev, xfer, 0);
}

/* Finish one transfer the controller has retired. */
static void slot_complete(const struct device *dev, struct uhc_agm_slot *slot)
{
	struct uhc_agm_qhd *qhd = slot->qhd;
	struct uhc_transfer *xfer = slot->xfer;
	bool stopped;
	int err = 0;

	if (slot->busy == 0U || slot->deferred != 0U) {
		return;
	}

	/*
	 * Done when the controller stopped with an error (the overlay halts where
	 * it stopped) or when the *last* qTD of the chain has retired. Watching
	 * the overlay's Active bit alone is not enough: between the SETUP qTD
	 * retiring and the controller fetching the next one the overlay is
	 * briefly inactive as well, and acting on that reported a transfer that
	 * had moved no data at all -- the host stack then read a buffer that was
	 * never filled (it surfaced as "Configuration descriptor read mismatch").
	 */
	stopped = (qhd->overlay.halted != 0U) ||
		  ((slot->qtd_tail != NULL) && (slot->qtd_tail->active == 0U));

	if (xfer == NULL) {
		/*
		 * The transfer was dequeued while the controller might still have
		 * been using it, so the qTDs were left armed on purpose. The
		 * caller owns the transfer; this path only waits until the
		 * controller has let go of the chain and then pools it again.
		 */
		if (stopped) {
			LOG_DBG("ep 0x%02x abandoned, qTDs pooled", slot->ep_addr);
			slot_release(slot);
		}

		return;
	}

	if (!stopped) {
		return;
	}

	if (qhd->overlay.halted) {
		if (qhd->overlay.xact_err || qhd->overlay.buffer_err ||
		    qhd->overlay.babble_err || qhd->overlay.err_count == 0U) {
			/* Bus error (or a vanished device); keep the endpoint
			 * usable for the next transfer. */
			err = -EIO;
		} else {
			err = -EPIPE;
		}
		qhd->overlay.halted = 0U;
	}

	if (xfer->err != 0) {
		err = xfer->err;
	}

	if (err != 0) {
		/* The overlay token carries the controller's own verdict (which
		 * error bit, how many retries were left), and PORTSC says whether
		 * the device is still on the port at all -- without both, a
		 * failed transfer is indistinguishable from an unplug. */
		LOG_WRN("ep 0x%02x type %u addr %u failed (%d): ep_char 0x%08x overlay 0x%08x qtd 0x%08x PORTSC 0x%08x",
			xfer->ep, slot->type, xfer->udev->addr, err,
			qhd->ep_char, qhd->overlay.token,
			slot->qtd != NULL ? slot->qtd->token : 0U,
			R(dev, PORTSC));

		slot_release(slot);
		uhc_xfer_return(dev, xfer, err);

		return;
	}

	/*
	 * The chain has retired. For an IN transfer the payload count comes
	 * from the data-stage qTD: for a control transfer that is the middle
	 * link (the chain head is SETUP, the tail the zero-length STATUS), for
	 * a bulk/interrupt transfer it is the only link.
	 */
	if (xfer->type == USB_EP_TYPE_CONTROL) {
		/* Decisive when a control transfer "succeeds" with nothing moved,
		 * and the way to see whether the status stage really went out: the
		 * overlay token is the last link the controller fetched (SETUP's
		 * token means it never reached the status stage). */
		LOG_DBG("control done: head 0x%08x data 0x%08x overlay 0x%08x",
			slot->qtd != NULL ? slot->qtd->token : 0U,
			slot->qtd_data != NULL ? slot->qtd_data->token : 0U,
			qhd->overlay.token);
	}

	if (xfer->buf != NULL && USB_EP_DIR_IS_IN(xfer->ep)) {
		const struct uhc_agm_qtd *qtd = slot->qtd_data != NULL
							? slot->qtd_data
							: slot->qtd;
		uint32_t xferred = (uint32_t)qtd->expected_bytes -
				   (uint32_t)qtd->total_bytes;

		LOG_DBG("IN transfer: %u bytes moved", xferred);

		xferred = MIN(xferred, (uint32_t)xfer->buf->size);
		if (xferred > 0U) {
			net_buf_add(xfer->buf, xferred);
		}
	}

	/*
	 * SET_ADDRESS: hold the completion back so the device has reached its
	 * new address before the stack fires the next request (see
	 * UHC_AGM_SET_ADDRESS_DELAY). The transfer stays in the core's list, so
	 * uhc_xfer_return() from the work handler unlinks it as usual.
	 */
	if (xfer->setup_pkt[0] == 0x00U &&
	    xfer->setup_pkt[1] == USB_SREQ_SET_ADDRESS) {
		struct uhc_agm_data *priv = uhc_get_private(dev);

		/* Keep the slot busy and flagged: submit_pending() must not arm
		 * this transfer again (its node is still in the core list), and
		 * the completion scan must not report it a second time. */
		slot->deferred = 1U;
		priv->addr_xfer = xfer;
		k_work_schedule(&priv->addr_work, UHC_AGM_SET_ADDRESS_DELAY);

		return;
	}

	slot_release(slot);
	uhc_xfer_return(dev, xfer, 0);
}

/* Walk the asynchronous ring once, starting and ending at its head. The ring
 * holds the head plus at most one queue head per slot, so the walk is bounded:
 * a link that was corrupted (or a queue head the controller keeps pointing
 * back into) would otherwise spin here forever -- and this runs in the ISR. */
static void scan_chain(const struct device *dev, struct uhc_agm_qhd *head)
{
	struct uhc_agm_data *priv = uhc_get_private(dev);
	struct uhc_agm_qhd *qhd = head;
	uint32_t budget = UHC_AGM_RING_WALK_MAX;

	do {
		struct uhc_agm_slot *slot = slot_for_qhd(priv, qhd);

		if (slot != NULL) {
			slot_complete(dev, slot);
		}
		qhd = qhd_next(qhd);
	} while ((qhd != head) && (--budget > 0U));

	if (qhd != head) {
		if (take_warn(priv, UHC_AGM_WARN_RING)) {
			LOG_WRN("async ring walk did not return to its head in %u steps",
				UHC_AGM_RING_WALK_MAX);
		}
	}
}

static void scan_periodic(const struct device *dev, uint32_t interval_ms)
{
	struct uhc_agm_data *priv = uhc_get_private(dev);
	struct uhc_agm_qhd *head_1ms = &priv->pool.period_head[0];
	union uhc_agm_link link =
		priv->pool.period_head[period_idx(interval_ms)].next;

	while (!link_terminated(&link)) {
		struct uhc_agm_qhd *qhd = link_to_qhd(&link);

		if (interval_ms > 1U && qhd == head_1ms) {
			break;
		}
		if (link.type == 1U) {
			struct uhc_agm_slot *slot = slot_for_qhd(priv, qhd);

			if (slot != NULL) {
				slot_complete(dev, slot);
			}
		}
		link = qhd->next;
	}
}

/* ---- port / bus ------------------------------------------------------- */

static void submit_connect(const struct device *dev, uint32_t portsc)
{
	uint32_t speed = (portsc & AGM_USB_PORTSC_PSPD_MASK) >>
			 AGM_USB_PORTSC_PSPD_SHIFT;

	switch (speed) {
	case AGM_USB_SPEED_LS:
		LOG_INF("Device connected, low speed");
		uhc_submit_event(dev, UHC_EVT_DEV_CONNECTED_LS, 0);
		break;
	case AGM_USB_SPEED_HS:
		LOG_INF("Device connected, high speed");
		uhc_submit_event(dev, UHC_EVT_DEV_CONNECTED_HS, 0);
		break;
	default:
		LOG_INF("Device connected, full speed");
		uhc_submit_event(dev, UHC_EVT_DEV_CONNECTED_FS, 0);
		break;
	}
}

static void port_change(const struct device *dev)
{
	struct uhc_agm_data *priv = uhc_get_private(dev);
	uint32_t portsc = R(dev, PORTSC);

	if ((portsc & AGM_USB_PORTSC_CSC) != 0U) {
		if ((portsc & AGM_USB_PORTSC_CCS) != 0U) {
			priv->connected = true;
			submit_connect(dev, portsc);
		} else {
			priv->connected = false;
			LOG_INF("Device removed");
			sched_reset(dev, UHC_AGM_WAIT_SPINS_ISR);
			uhc_submit_event(dev, UHC_EVT_DEV_REMOVED, 0);
		}
	}

	/* Acknowledge every change bit; the R/W bits are written back with the
	 * values just read. */
	W(dev, PORTSC, portsc);
}

/* Zephyr's ISR type: the handler has to be a bare identifier for
 * IRQ_CONNECT()'s __isr_ ## name paste, and the native/POSIX arch is the only
 * one whose ARCH_IRQ_CONNECT() does not cast the argument (same note as
 * udc_agm.c / uart_agm.c). */
static void uhc_agm_isr(const void *isr_arg)
{
	const struct device *dev = isr_arg;
	struct uhc_agm_data *priv = uhc_get_private(dev);

	if (!priv->enabled) {
		W(dev, USBSTS, R(dev, USBSTS) & UHC_AGM_USBINTR_MASK);
		return;
	}

	/*
	 * Acknowledge *before* processing, then re-read and process again, the
	 * order the NXP EHCI HAL uses (USB_HostEhciIsrFunction: USBSTS =
	 * interruptStatus first, then the handlers, then loop).
	 *
	 * Doing it the other way round loses completions: a control transfer's
	 * IOC interrupt fires when the DATA qTD retires, and the STATUS qTD
	 * often retires while this ISR is still scanning. The trailing
	 * "USBSTS = snapshot" write then swallowed the second interrupt and the
	 * transfer was never reported (a 5 s chapter-9 timeout with
	 * the queue head already idle, USBSTS.UI clear).
	 */
	for (uint32_t rounds = 0U; rounds < UHC_AGM_ISR_ROUNDS; rounds++) {
		uint32_t status = R(dev, USBSTS) & UHC_AGM_USBINTR_MASK;

		if (status == 0U) {
			break;
		}

		W(dev, USBSTS, status);

		if ((status & AGM_USB_STS_SEI) != 0U) {
			LOG_ERR("Host system error");
		}

		if ((status & AGM_USB_STS_FRI) != 0U) {
			priv->uframe += UHC_AGM_FRAMELIST_SIZE *
					UHC_AGM_MICROFRAMES_PER_FRAME;
		}

		if ((status & AGM_USB_STS_PCI) != 0U) {
			port_change(dev);
		}

		if ((status & (AGM_USB_STS_UI | AGM_USB_STS_UEI)) != 0U) {
			LOG_DBG("isr: USBSTS 0x%08x", status);
			scan_chain(dev, &priv->pool.async_head);
			for (uint32_t i = 0U; i < UHC_AGM_PERIOD_HEADS; i++) {
				scan_periodic(dev, 1U << i);
			}
			submit_pending(dev);
		}

		/* Nothing is recycled behind the async-advance doorbell yet
		 * (sched_reset() rebuilds the whole schedule instead); the bit
		 * only needs clearing. */
	}

	/*
	 * Leaving the loop with a status bit still set means the controller
	 * re-asserted it inside the loop, so this handler will be entered again
	 * right away. That was measured to be normal here, not a fault: on the
	 * (HID receiver streaming reports) the loop covered its two
	 * rounds in 7 us and USBSTS.UI was set again immediately after the ack,
	 * with transfers completing throughout. It is worth a debug line -- the
	 * useful thing to see when something really is stuck -- but not a
	 * warning that a healthy mouse makes appear.
	 */
	if (((R(dev, USBSTS) & UHC_AGM_USBINTR_MASK) != 0U) &&
	    take_warn(priv, UHC_AGM_WARN_STICKY_STS)) {
		LOG_DBG("USBSTS 0x%08x still set after %u rounds",
			R(dev, USBSTS), UHC_AGM_ISR_ROUNDS);
	}
}

/* ---- UHC API ---------------------------------------------------------- */

static int uhc_agm_init(const struct device *dev)
{
	struct uhc_agm_data *priv = uhc_get_private(dev);
	struct uhc_data *data = dev->data;
	uint32_t mode;

	data->caps.hs = 0U;	/* full speed only */

	/* 1. Reset the controller (SDK USB_Reset). */
	W(dev, USBCMD, R(dev, USBCMD) | AGM_USB_CMD_RST);
	if (!agm_wait_clear(dev, offsetof(struct udc_agm_regs, USBCMD),
			    AGM_USB_CMD_RST)) {
		LOG_ERR("Controller reset (USBCMD.RST) did not clear");
		return -ETIMEDOUT;
	}

	/* 2. Force host mode, exactly like the device driver forces CM=device:
	 * the bitstream may hand USB0 over as OTG and software picks the role
	 * (the vendor does the same in USB_InitHost()). */
	mode = R(dev, USBMODE) & ~AGM_USB_MODE_CM_MASK;
	W(dev, USBMODE, mode | AGM_USB_MODE_CM_HOST);
	LOG_DBG("USBMODE 0x%08x (CM=host)", R(dev, USBMODE));

	/* 3. Mask interrupts until enable(): a device that is already plugged
	 * in must not deliver a connect event before usbh_init() has finished
	 * initialising the host context. */
	W(dev, USBINTR, 0U);
	/* Keep port change latched so enable() can still see a pre-connected
	 * device (TinyUSB clears all status except PORT_CHANGE here). */
	W(dev, USBSTS, UHC_AGM_USBINTR_MASK & ~AGM_USB_STS_PCI);

	/* 4. Both schedules. */
	sched_init(dev);
	W(dev, ASYNCLISTADDR, (uint32_t)(uintptr_t)&priv->pool.async_head);
	W(dev, PERIODICLISTBASE, (uint32_t)(uintptr_t)priv->pool.framelist);
	/* NXP embedded transaction translator control: unused (0). */
	W(dev, RESERVED5, 0U);

	/* 5. Interrupt threshold 0, i.e. interrupt as soon as possible
	 * (SDK USB_SetIntThreshold(USB_INT_THRESHOLD0)), and the frame-list
	 * size. FS may only change while RS is clear, and RS is not set until
	 * enable(), so this is the one place it can go. */
	W(dev, USBCMD, (R(dev, USBCMD) & ~AGM_USB_CMD_ITC) |
		       (0U << AGM_USB_CMD_ITC_SHIFT) |
		       UHC_AGM_FRAMELIST_CMD);

	/* 6. Port power, but only if the controller implements power control
	 * (HCSPARAMS.PPC); otherwise the port is powered unconditionally. */
	if ((R(dev, HCSPARAMS) & BIT(4)) != 0U) {
		W(dev, PORTSC, (R(dev, PORTSC) & ~AGM_USB_PORTSC_W1C) |
			       AGM_USB_PORTSC_PP);
	}

	/* INF on purpose: this is the driver's bring-up verdict, and a host
	 * that never sees a connect is diagnosed from exactly these four
	 * registers (CM = host? port powered? CCS set?). */
	LOG_INF("init: USBMODE 0x%08x USBCMD 0x%08x USBSTS 0x%08x PORTSC 0x%08x",
		R(dev, USBMODE), R(dev, USBCMD), R(dev, USBSTS), R(dev, PORTSC));

	return 0;
}

static int uhc_agm_enable(const struct device *dev)
{
	struct uhc_agm_data *priv = uhc_get_private(dev);
	uint32_t portsc;

	W(dev, USBSTS, UHC_AGM_USBINTR_MASK & ~AGM_USB_STS_PCI);
	W(dev, USBINTR, UHC_AGM_USBINTR_MASK);
	W(dev, USBCMD, R(dev, USBCMD) | AGM_USB_CMD_RS |
		       AGM_USB_CMD_PSE | AGM_USB_CMD_ASE);

	/* Let the ISR in only once the schedules are live, so it never runs
	 * against a half-programmed controller. */
	priv->enabled = true;

	portsc = R(dev, PORTSC);
	if ((portsc & AGM_USB_PORTSC_CCS) != 0U) {
		/* The device was already attached when the controller started,
		 * so the change event never fired: raise it here. */
		priv->connected = true;
		submit_connect(dev, portsc);
		W(dev, PORTSC, R(dev, PORTSC));
	}

	LOG_INF("enabled: USBCMD 0x%08x USBINTR 0x%08x USBSTS 0x%08x PORTSC 0x%08x",
		R(dev, USBCMD), R(dev, USBINTR), R(dev, USBSTS), R(dev, PORTSC));

	return 0;
}

static int uhc_agm_disable(const struct device *dev)
{
	struct uhc_agm_data *priv = uhc_get_private(dev);

	priv->enabled = false;
	priv->connected = false;
	W(dev, USBINTR, 0U);
	W(dev, USBCMD, R(dev, USBCMD) &
		       ~(AGM_USB_CMD_RS | AGM_USB_CMD_PSE | AGM_USB_CMD_ASE));
	if (!agm_wait_mask(dev, offsetof(struct udc_agm_regs, USBSTS),
			   AGM_USB_STS_HCH, AGM_USB_STS_HCH, UHC_AGM_WAIT_SPINS)) {
		LOG_ERR("Controller did not halt (USBSTS.HCH)");
	}
	W(dev, PORTSC, R(dev, PORTSC) & ~AGM_USB_PORTSC_W1C &
		       ~AGM_USB_PORTSC_PP);

	return 0;
}

static int uhc_agm_shutdown(const struct device *dev)
{
	struct uhc_agm_data *priv = uhc_get_private(dev);
	int ret;

	/* Mask the ISR first, then take the endpoint pool apart; the other
	 * order would let a completion walk a pool that is being cleared. */
	priv->enabled = false;
	ret = uhc_agm_disable(dev);
	if (ret != 0) {
		return ret;
	}

	sched_reset(dev, UHC_AGM_WAIT_SPINS);

	return 0;
}

static int uhc_agm_bus_reset(const struct device *dev)
{
	uint32_t portsc;

	/* Skip if a reset is already in progress. */
	if ((R(dev, PORTSC) & AGM_USB_PORTSC_PR) != 0U) {
		return 0;
	}

	/* Select the reset signalling: clear the change bits, drop port
	 * enable (EHCI requires write-zero to it alongside write-one to PR). */
	portsc = R(dev, PORTSC) & ~AGM_USB_PORTSC_W1C;
	portsc &= ~AGM_USB_PORTSC_PE;
	portsc |= AGM_USB_PORTSC_PR;
	W(dev, PORTSC, portsc);

	k_msleep(UHC_AGM_RESET_HOLD_MS);

	/*
	 * The controller releases PR itself once the reset handshake with the
	 * device has finished and enables the port -- that is what the NXP HAL
	 * polls for. Only release it from software if the controller did not,
	 * so a chip that waits for the software clear still works.
	 */
	if (!agm_wait_clear(dev, offsetof(struct udc_agm_regs, PORTSC),
			    AGM_USB_PORTSC_PR)) {
		LOG_WRN("PORTSC.PR did not self-clear, releasing it from software");
		portsc = R(dev, PORTSC) & ~AGM_USB_PORTSC_W1C;
		W(dev, PORTSC, portsc & ~AGM_USB_PORTSC_PR);
		(void)agm_wait_clear(dev, offsetof(struct udc_agm_regs, PORTSC),
				     AGM_USB_PORTSC_PR);
	}

	/* Recovery time before the first transaction (see the define). */
	k_msleep(UHC_AGM_RESET_RECOVERY_MS);

	LOG_DBG("bus reset done: PORTSC 0x%08x", R(dev, PORTSC));
	uhc_submit_event(dev, UHC_EVT_RESETED, 0);

	return 0;
}

static int uhc_agm_sof_enable(const struct device *dev)
{
	if ((R(dev, USBCMD) & AGM_USB_CMD_RS) != 0U) {
		return -EALREADY;
	}

	W(dev, USBCMD, R(dev, USBCMD) | AGM_USB_CMD_RS);

	return 0;
}

static int uhc_agm_bus_suspend(const struct device *dev)
{
	uint32_t portsc = R(dev, PORTSC) & ~AGM_USB_PORTSC_W1C;

	if ((portsc & AGM_USB_PORTSC_SUSP) != 0U) {
		return -EALREADY;
	}

	W(dev, PORTSC, portsc | AGM_USB_PORTSC_SUSP);
	uhc_submit_event(dev, UHC_EVT_SUSPENDED, 0);

	return 0;
}

static int uhc_agm_bus_resume(const struct device *dev)
{
	uint32_t portsc = R(dev, PORTSC) & ~AGM_USB_PORTSC_W1C;

	/* Signal resume for at least 20 ms, then release the bus. */
	W(dev, PORTSC, (portsc | AGM_USB_PORTSC_FPR) & ~AGM_USB_PORTSC_SUSP);
	k_msleep(20);

	portsc = R(dev, PORTSC) & ~AGM_USB_PORTSC_W1C;
	W(dev, PORTSC, portsc & ~AGM_USB_PORTSC_FPR);
	uhc_submit_event(dev, UHC_EVT_RESUMED, 0);

	return 0;
}

static int uhc_agm_enqueue(const struct device *dev,
			   struct uhc_transfer *const xfer)
{
	/* The UHC core already holds the driver mutex here. */
	(void)uhc_xfer_append(dev, xfer);
	submit_pending(dev);

	return 0;
}

static int uhc_agm_dequeue(const struct device *dev,
			   struct uhc_transfer *const xfer)
{
	struct uhc_agm_data *priv = uhc_get_private(dev);
	unsigned int key = irq_lock();

	for (uint32_t i = 0U; i < UHC_AGM_QHD_MAX; i++) {
		struct uhc_agm_slot *slot = &priv->slot[i];

		if (slot->used == 0U || slot->xfer != xfer) {
			continue;
		}

		/*
		 * The controller may still be reading this transfer's qTDs and
		 * writing into its buffer, so the chain must not go back to the
		 * pool yet. Retire the queue head so nothing further is fetched,
		 * hand the transfer back to the caller (which owns the free) and
		 * let the completion scan pool the qTDs once the controller has
		 * let go of them. The slot stays busy + removing, so it is neither
		 * re-armed nor reused in the meantime.
		 */
		slot->qhd->overlay.next.address = 0x1U;
		slot->xfer = NULL;
		slot->deferred = 0U;
		slot->removing = 1U;
		LOG_DBG("ep 0x%02x dequeued mid-flight; qTDs kept until the controller stops",
			xfer->ep);
		break;
	}

	/* A deferred SET_ADDRESS completion must not fire for a transfer the
	 * caller is about to free. */
	if (priv->addr_xfer == xfer) {
		priv->addr_xfer = NULL;
	}

	/* The transfer is still in the core list here (that is where the
	 * "never started" case lives too), so the caller owns the free. It can
	 * also have completed between the timeout and this call, in which case
	 * uhc_xfer_return() already unlinked it and only the queued flag is
	 * stale. */
	if (sys_dnode_is_linked(&xfer->node)) {
		sys_dlist_remove(&xfer->node);
	}
	irq_unlock(key);

	return 0;
}

/* The common helpers take a timeout; the UHC API's lock/unlock take none. */
static int uhc_agm_lock(const struct device *dev)
{
	return uhc_lock_internal(dev, K_FOREVER);
}

static int uhc_agm_unlock(const struct device *dev)
{
	return uhc_unlock_internal(dev);
}

static DEVICE_API(uhc, uhc_agm_api) = {
	.lock = uhc_agm_lock,
	.unlock = uhc_agm_unlock,
	.init = uhc_agm_init,
	.enable = uhc_agm_enable,
	.disable = uhc_agm_disable,
	.shutdown = uhc_agm_shutdown,
	.bus_reset = uhc_agm_bus_reset,
	.sof_enable = uhc_agm_sof_enable,
	.bus_suspend = uhc_agm_bus_suspend,
	.bus_resume = uhc_agm_bus_resume,
	.ep_enqueue = uhc_agm_enqueue,
	.ep_dequeue = uhc_agm_dequeue,
};

/* ---- device definition ------------------------------------------------ */

static int uhc_agm_preinit(const struct device *dev)
{
	const struct uhc_agm_config *cfg = dev->config;
	struct uhc_data *data = dev->data;
	struct uhc_agm_data *priv = uhc_get_private(dev);

	k_mutex_init(&data->mutex);
	priv->dev = dev;
	priv->enabled = false;
	priv->connected = false;
	k_work_init_delayable(&priv->addr_work, set_address_delay_done);

	/* Wire the PLIC interrupt before the controller can raise one. */
	cfg->irq_config_func(dev);

	return 0;
}

#define UHC_AGM_DECLARE_IRQ_CONFIG(n) \
	static void uhc_agm_irq_config_func_##n(const struct device *dev);

DT_INST_FOREACH_STATUS_OKAY(UHC_AGM_DECLARE_IRQ_CONFIG)

#define UHC_AGM_DEVICE_DEFINE(n)						\
	__aligned(4096) static struct uhc_agm_data uhc_agm_priv_##n;		\
										\
	static const struct uhc_agm_config uhc_agm_config_##n = {		\
		.regs = (volatile struct udc_agm_regs *)			\
			DT_REG_ADDR(DT_DRV_INST(n)),				\
		.irq_config_func = uhc_agm_irq_config_func_##n,			\
	};									\
										\
	static struct uhc_data uhc_data_##n = {					\
		.mutex = Z_MUTEX_INITIALIZER(uhc_data_##n.mutex),		\
		.priv = &uhc_agm_priv_##n,					\
	};									\
										\
	DEVICE_DT_INST_DEFINE(n, uhc_agm_preinit, NULL,				\
			      &uhc_data_##n, &uhc_agm_config_##n,		\
			      POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEVICE,	\
			      &uhc_agm_api);					\
										\
	static void uhc_agm_irq_config_func_##n(const struct device *dev)	\
	{									\
		ARG_UNUSED(dev);						\
		IRQ_CONNECT(DT_INST_IRQN(n), DT_INST_IRQ(n, priority),		\
			    uhc_agm_isr, DEVICE_DT_INST_GET(n), 0);		\
		irq_enable(DT_INST_IRQN(n));					\
	}

DT_INST_FOREACH_STATUS_OKAY(UHC_AGM_DEVICE_DEFINE)
