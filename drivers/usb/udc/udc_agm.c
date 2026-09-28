/*
 * Copyright (c) 2026 AgRV Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * USB0 device controller driver for AgRV2K.
 *
 * Hardware: FSL/ChipIdea-like dual-role controller, register layout
 * identical to EHCI device-mode companion controllers (CAPLENGTH,
 * USBCMD/USBSTS/USBINTR, PORTSC, OTGSC, USBMODE, ENDPTSETUPSTAT,
 * ENDPTPRIME/FLUSH/STATUS/COMPLETE, ENDPTCTRL[0..3], dQH, dTD).
 *
 * Driver scope: device-only, FS-only, 4 bi-directional endpoints,
 * 64 B MPS, control / bulk / interrupt transfers. ISO is rejected
 * (driver returns -EINVAL from ep_enable for ISO); host mode and OTG
 * role-switch are not in scope.
 *
 * Algorithm mirrors the official AgRV TinyUSB dcd (200 lines, see
 * framework-agrv_tinyusb/hw/mcu/agm/agrv2k_dcd.c) and the SDK usb.c
 * wrapper: bus reset → repaint dQH table → run; per-transfer dTD →
 * dQH.Overlay.Next → ENDPTPRIME; ISR reads ENDPTSETUPSTAT +
 * ENDPTCOMPLETE → emits udc_submit_event / udc_submit_ep_event.
 *
 * Public API: struct udc_api (see include/zephyr/drivers/usb/udc.h).
 *
 * Threading model: the ISR snapshots USBSTS/ENDPTSETUPSTAT/ENDPTCOMPLETE
 * into atomics and submits a work item on the UDC workqueue. The workqueue
 * handler (udc_agm_evt_handler) drains the snapshots in hardware order and
 * runs udc_setup_received() / udc_submit_ep_event() in thread context.
 * EP enqueue runs in process context with the UDC mutex held by
 * udc_common.c; the net_buf FIFO and the per-endpoint busy flag are the
 * synchronization points with the completion path (irq_lock sections).
 * Buffers are armed one at a time: one dTD per endpoint direction, fed from
 * the head of the endpoint FIFO (see udc_agm_ep_kick()).
 */

#define DT_DRV_COMPAT agm_agrv2k_usb0

#include "udc_agm.h"
#include "udc_common.h"

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/devicetree.h>
#include <zephyr/sys/util.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/arch/common/ffs.h>
#include <zephyr/drivers/usb/udc.h>
#include <zephyr/usb/usb_ch9.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(udc_agm, CONFIG_UDC_DRIVER_LOG_LEVEL);

/* QH_MAX = 4 EPs * 2 directions, matching the FSL/ChipIdea index
 * convention used in TinyUSB agrv2k_dcd.c (dcd_ep_num2idx). */
#define QH_MAX         AGM_USB_NUM_QH
#define EP_IDX_OUT(ep) ((ep) * 2U)
#define EP_IDX_IN(ep)  ((ep) * 2U + 1U)
#define EP_BIT_OUT(ep) AGM_USB_EP_BIT_OUT(ep)
#define EP_BIT_IN(ep)  AGM_USB_EP_BIT_IN(ep)

/* Driver-private data. */
struct udc_agm_data {
	struct udc_data common;
	/* Cached dQH + dTD pointers for fast ISR dispatch. */
	struct udc_agm_dqh *dQH;
	struct udc_agm_dtd *dTD;
	/* Device handle for the deferred work handler. */
	const struct device *dev;
	/*
	 * ISR -> thread handoff. udc_setup_received() may only run in
	 * thread context (it takes the UDC mutex), so the ISR snapshots
	 * USB events and the UDC workqueue processes them. Mirrors the
	 * event model of udc_mcux_ehci (FSL family), udc_dwc2 and
	 * udc_sam0.
	 */
	struct k_work evt_work;
	atomic_t evt_flags;        /* AGM_USB_EVT_* bits below */
	atomic_t pending_complete; /* ENDPTCOMPLETE snapshot */
	uint8_t setup[8];          /* SETUP copy for the workqueue */
};

/* ISR -> workqueue event flags. */
#define AGM_USB_EVT_RESET BIT(0)
#define AGM_USB_EVT_SETUP BIT(1)

/* Per-instance config. */
struct udc_agm_config {
	volatile struct udc_agm_regs *regs;
	uint16_t num_endpoints;
	/* Per-instance endpoint config tables. */
	struct udc_ep_config *ep_cfg_out;
	struct udc_ep_config *ep_cfg_in;
	/* dQH + dTD storage. */
	struct udc_agm_dqh *dqh_pool;
	struct udc_agm_dtd *dtd_pool;
	/* Per-instance IRQ hook (called early in udc_agm_init). */
	void (*irq_config_func)(const struct device *dev);
};

/* ---- register helpers ------------------------------------------------- */

static inline volatile uint32_t *reg32(const struct device *dev, uint32_t off)
{
	return (volatile uint32_t *)((uintptr_t)
		((const struct udc_agm_config *)dev->config)->regs + off);
}

static inline uint32_t r32(const struct device *dev, uint32_t off)
{
	return sys_read32((mem_addr_t)reg32(dev, off));
}

static inline void w32(const struct device *dev, uint32_t off, uint32_t v)
{
	sys_write32(v, (mem_addr_t)reg32(dev, off));
}

#define R(dev, field) r32((dev), offsetof(struct udc_agm_regs, field))
#define W(dev, field, v) w32((dev), offsetof(struct udc_agm_regs, field), (v))

/*
 * Upper bound for every "wait for the controller to finish something" loop in
 * this file. None of them is a timing requirement -- each polls a status bit
 * that a working controller clears in microseconds -- so the bound only has to
 * be far longer than a real handshake. Without it a wedged controller (or a
 * host that halves a port reset) pins the caller forever: USB0's control and
 * status waits run in the UDC workqueue, so every later enqueue and
 * udc_setup_received() would stall behind them.
 */
#define AGM_USB_WAIT_SPINS 1000000U

/* Poll `off` until `mask` is clear, bounded. false == it never cleared. */
static bool usb_wait_clear(const struct device *dev, uint32_t off, uint32_t mask)
{
	for (uint32_t spins = AGM_USB_WAIT_SPINS; spins > 0U; spins--) {
		if ((r32(dev, off) & mask) == 0U) {
			return true;
		}
		k_busy_wait(1);
	}

	return false;
}

/* ---- dQH / dTD helpers ------------------------------------------------ */

static void dqh_init(struct udc_agm_dqh *qh)
{
	memset(qh, 0, sizeof(*qh));
	/* ZLT disabled (zlt_disable=1) on every dQH, including EP0. Matches
	 * the AgRV TinyUSB reference (SDK USB_InitDQH() sets
	 * ZeroLengthTerminationDisable=true unconditionally). The dTD will
	 * terminate naturally when its TotalBytes field reaches zero, which
	 * is correct for control transfers with a non-MPS data phase
	 * (e.g. SET_LINE_CODING: 7-byte DATA OUT). Keeping ZLT off here
	 * also avoids a VMware CDC ACM quirk where zero-data probing can
	 * otherwise stall the controller waiting for the next token.
	 */
	qh->zlt_disable = 1;
	qh->max_packet_length = AGM_USB_EP0_MPS;
	qh->overlay.next = AGM_USB_TD_TERMINATE;
}

static void dtd_init(struct udc_agm_dtd *dtd, void *buf, uint32_t len)
{
	memset(dtd, 0, sizeof(*dtd));
	dtd->next = AGM_USB_TD_TERMINATE;
	dtd->total_bytes = len;
	dtd->expected_bytes = len;
	dtd->int_on_complete = 1;
	dtd->active = 1;
	/* BufferPointer Page i = ((buf >> 12) + i) << 12 for i = 0..4.
	 * Mirrors SDK USB_InitDTD. */
	dtd->buffer_pointer[0] = (uint32_t)(uintptr_t)buf;
	for (int i = 1; i < 5; i++) {
		dtd->buffer_pointer[i] = (((uintptr_t)buf >> 12) + i) << 12;
	}
}

/* Translate Zephyr attributes byte (USB EP type in low 2 bits) to
 * the AgRV ENDPTCTRL TYPE field value. */
static uint32_t attrib_to_type_field(uint8_t attributes)
{
	switch (attributes & USB_EP_TRANSFER_TYPE_MASK) {
	case USB_EP_TYPE_CONTROL:    return AGM_USB_EP_TYPE_CTRL;
	case USB_EP_TYPE_ISO:        return AGM_USB_EP_TYPE_ISO;
	case USB_EP_TYPE_BULK:       return AGM_USB_EP_TYPE_BULK;
	case USB_EP_TYPE_INTERRUPT:  return AGM_USB_EP_TYPE_INT;
	default:                     return AGM_USB_EP_TYPE_CTRL;
	}
}

/* ---- core register operations ----------------------------------------- */

static void usb_reset_controller(const struct device *dev)
{
	W(dev, USBCMD, R(dev, USBCMD) | AGM_USB_CMD_RST);
	if (!usb_wait_clear(dev, offsetof(struct udc_agm_regs, USBCMD),
			    AGM_USB_CMD_RST)) {
		LOG_ERR("USB: controller reset (USBCMD.RST) did not clear");
	}
}

static void usb_set_mode_device(const struct device *dev)
{
	uint32_t mode = R(dev, USBMODE);

	mode &= ~AGM_USB_MODE_CM_MASK;
	mode |= AGM_USB_MODE_CM_DEVICE;
	/* SLOM = 1: setup lockout off (matches agrv2k_dcd.c). */
	mode |= AGM_USB_MODE_SLOM;
	W(dev, USBMODE, mode);
}

static void usb_run(const struct device *dev)
{
	W(dev, USBCMD, R(dev, USBCMD) | AGM_USB_CMD_RS);
}

static void usb_stop(const struct device *dev)
{
	W(dev, USBCMD, R(dev, USBCMD) & ~AGM_USB_CMD_RS);
}

static void udc_agm_evt_handler(struct k_work *item);

/* ---- init / shutdown / enable / disable ------------------------------- */

/*
 * udc_agm_preinit -- lightweight device init, called once by
 * DEVICE_DT_INST_DEFINE at POST_KERNEL. Sets up kernel primitives and
 * registers endpoints, but does NOT touch the controller hardware.
 *
 * The controller hardware setup (reset, mode, dQH, port power, EP0
 * enable) lives in udc_agm_init below, which is called by usbd via
 * udc_init() -> api->init. Splitting these is essential because
 * Zephyr's UDC stack calls api->init exactly once when the upper
 * layer initializes the device; doing the same hardware setup twice
 * would corrupt the controller state (udc_ep_enable_internal rejects
 * re-enabling an already-enabled EP, returning -EALREADY).
 */
static int udc_agm_preinit(const struct device *dev)
{
	const struct udc_agm_config *cfg = dev->config;
	struct udc_data *data = dev->data;
	struct udc_ep_config *ep_cfg;
	uint16_t mps;
	int err;

	k_mutex_init(&data->mutex);

	struct udc_agm_data *priv = udc_get_private(dev);

	priv->dev = dev;
	k_work_init(&priv->evt_work, udc_agm_evt_handler);

	/* Wire PLIC IRQ -> udc_agm_isr as early as possible so we do not
	 * miss USB events that arrive between reset and enable. */
	cfg->irq_config_func(dev);

	/* Fill in caps (FS only). */
	data->caps.mps0 = UDC_MPS0_64;
	data->caps.rwup = true;
	data->caps.addr_before_status = true;
	mps = AGM_USB_EP0_MPS;

	/* Register all endpoints. */
	for (uint16_t i = 0; i < cfg->num_endpoints; i++) {
		ep_cfg = &cfg->ep_cfg_out[i];
		ep_cfg->addr = USB_EP_GET_ADDR(i, USB_EP_DIR_OUT);
		ep_cfg->caps.out = 1;
		if (i == 0U) {
			ep_cfg->caps.control = 1;
			ep_cfg->caps.mps = mps;
		} else {
			ep_cfg->caps.bulk = 1;
			ep_cfg->caps.interrupt = 1;
			ep_cfg->caps.mps = AGM_USB_FS_MPS;
		}
		err = udc_register_ep(dev, ep_cfg);
		if (err != 0) {
			LOG_ERR("Failed to register OUT ep%u: %d", i, err);
			return err;
		}

		ep_cfg = &cfg->ep_cfg_in[i];
		ep_cfg->addr = USB_EP_GET_ADDR(i, USB_EP_DIR_IN);
		ep_cfg->caps.in = 1;
		if (i == 0U) {
			ep_cfg->caps.control = 1;
			ep_cfg->caps.mps = mps;
		} else {
			ep_cfg->caps.bulk = 1;
			ep_cfg->caps.interrupt = 1;
			ep_cfg->caps.mps = AGM_USB_FS_MPS;
		}
		err = udc_register_ep(dev, ep_cfg);
		if (err != 0) {
			LOG_ERR("Failed to register IN ep%u: %d", i, err);
			return err;
		}
	}

	return 0;
}

/*
 * udc_agm_init -- heavy controller init, called by the UDC stack via
 * udc_init() -> api->init. Resets the controller, sets device mode,
 * installs the dQH table, powers the port, and enables EP0 in
 * hardware. Mirrors the SDK USB_InitDevice() sequence:
 *   reset -> CM=device -> USBINTR set -> USB_Run, plus the FSL-style
 *   ENDPOINTLISTADDR + EP0 ctrl/bulk-capable type setup.
 */
static int udc_agm_init(const struct device *dev)
{
	const struct udc_agm_config *cfg = dev->config;
	struct udc_agm_data *priv = udc_get_private(dev);

	/* Reset controller and put in device mode. */
	usb_reset_controller(dev);
	usb_set_mode_device(dev);

	/* Install dQH table address. */
	W(dev, ENDPOINTLISTADDR, (uint32_t)(uintptr_t)priv->dQH);
	/* Interrupt threshold = 0 (every transaction can interrupt). */
	W(dev, USBCMD, (R(dev, USBCMD) & ~AGM_USB_CMD_ITC) |
		      (0U << AGM_USB_CMD_ITC_SHIFT));

	/* Port power on. */
	W(dev, PORTSC, R(dev, PORTSC) | AGM_USB_PORTSC_PP);

	/* Enable default endpoints. */
	if (udc_ep_enable_internal(dev, USB_CONTROL_EP_OUT,
				   USB_EP_TYPE_CONTROL, AGM_USB_EP0_MPS, 0)) {
		LOG_ERR("Failed to enable EP0 OUT");
		return -EIO;
	}
	if (udc_ep_enable_internal(dev, USB_CONTROL_EP_IN,
				   USB_EP_TYPE_CONTROL, AGM_USB_EP0_MPS, 0)) {
		LOG_ERR("Failed to enable EP0 IN");
		return -EIO;
	}

	(void)cfg;
	return 0;
}

static int udc_agm_enable(const struct device *dev)
{
	struct udc_agm_data *priv = udc_get_private(dev);

	/* Repaint all dQH (terminated + MPS=64) and mark EP0 OUT
	 * with setup-tripwire (mirrors agrv2k_dcd.c URI handler). */
	for (int i = 0; i < QH_MAX; i++) {
		dqh_init(&priv->dQH[i]);
	}
	priv->dQH[0].int_on_setup = 1;

	/*
	 * Drop events snapshotted before this enable (e.g. from a previous
	 * session); the controller state is being rebuilt.
	 */
	atomic_set(&priv->evt_flags, 0);
	atomic_set(&priv->pending_complete, 0);

	W(dev, ENDPOINTLISTADDR, (uint32_t)(uintptr_t)priv->dQH);

	/* Enable device-relevant interrupts. */
	W(dev, USBINTR, AGM_USB_STS_UI  | AGM_USB_STS_UEI |
			AGM_USB_STS_PCI | AGM_USB_STS_SEI |
			AGM_USB_STS_URI | AGM_USB_STS_SLI);

	usb_run(dev);
	return 0;
}

static int udc_agm_disable(const struct device *dev)
{
	usb_stop(dev);
	W(dev, USBINTR, 0);
	W(dev, USBSTS, R(dev, USBSTS));
	return 0;
}

static int udc_agm_shutdown(const struct device *dev)
{
	if (udc_ep_disable_internal(dev, USB_CONTROL_EP_OUT)) {
		return -EIO;
	}
	if (udc_ep_disable_internal(dev, USB_CONTROL_EP_IN)) {
		return -EIO;
	}
	return 0;
}

/* ---- per-endpoint enable / disable ----------------------------------- */

static int udc_agm_ep_enable(const struct device *dev,
			     struct udc_ep_config *const cfg)
{
	const struct udc_agm_config *dcfg = dev->config;
	uint8_t ep_num = USB_EP_GET_IDX(cfg->addr);
	uint8_t dir_in = USB_EP_DIR_IS_IN(cfg->addr);

	if (ep_num >= dcfg->num_endpoints) {
		return -EINVAL;
	}

	uint32_t v = R(dev, ENDPTCTRL[ep_num]);
	uint32_t type_field = attrib_to_type_field(cfg->attributes);

	if (dir_in) {
		v &= ~(AGM_USB_EPCTRL_TX_TYPE_MASK | AGM_USB_EPCTRL_TX_E |
		       AGM_USB_EPCTRL_TX_S | AGM_USB_EPCTRL_TX_TI);
		v |= (type_field << AGM_USB_EPCTRL_TX_TYPE_SHIFT) |
		     AGM_USB_EPCTRL_TX_E | AGM_USB_EPCTRL_TX_TR;
	} else {
		v &= ~(AGM_USB_EPCTRL_RX_TYPE_MASK | AGM_USB_EPCTRL_RX_E |
		       AGM_USB_EPCTRL_RX_S | AGM_USB_EPCTRL_RX_TI);
		v |= (type_field << AGM_USB_EPCTRL_RX_TYPE_SHIFT) |
		     AGM_USB_EPCTRL_RX_E | AGM_USB_EPCTRL_RX_TR;
	}
	W(dev, ENDPTCTRL[ep_num], v);
	return 0;
}

static int udc_agm_ep_disable(const struct device *dev,
			      struct udc_ep_config *const cfg)
{
	const struct udc_agm_config *dcfg = dev->config;
	uint8_t ep_num = USB_EP_GET_IDX(cfg->addr);
	uint8_t dir_in = USB_EP_DIR_IS_IN(cfg->addr);

	if (ep_num >= dcfg->num_endpoints) {
		return -EINVAL;
	}

	uint32_t v = R(dev, ENDPTCTRL[ep_num]);

	if (dir_in) {
		v &= ~(AGM_USB_EPCTRL_TX_E | AGM_USB_EPCTRL_TX_S);
	} else {
		v &= ~(AGM_USB_EPCTRL_RX_E | AGM_USB_EPCTRL_RX_S);
	}
	W(dev, ENDPTCTRL[ep_num], v);
	return 0;
}

/* ---- stall / clear stall --------------------------------------------- */

static int udc_agm_ep_set_halt(const struct device *dev,
			       struct udc_ep_config *const cfg)
{
	const struct udc_agm_config *dcfg = dev->config;
	uint8_t ep_num = USB_EP_GET_IDX(cfg->addr);
	uint8_t dir_in = USB_EP_DIR_IS_IN(cfg->addr);

	if (ep_num >= dcfg->num_endpoints) {
		return -EINVAL;
	}

	if (ep_num != 0U) {
		cfg->stat.halted = true;
	}
	if (dir_in) {
		W(dev, ENDPTCTRL[ep_num],
		  R(dev, ENDPTCTRL[ep_num]) | AGM_USB_EPCTRL_TX_S);
	} else {
		W(dev, ENDPTCTRL[ep_num],
		  R(dev, ENDPTCTRL[ep_num]) | AGM_USB_EPCTRL_RX_S);
	}
	return 0;
}

static int udc_agm_ep_clear_halt(const struct device *dev,
				 struct udc_ep_config *const cfg)
{
	const struct udc_agm_config *dcfg = dev->config;
	uint8_t ep_num = USB_EP_GET_IDX(cfg->addr);
	uint8_t dir_in = USB_EP_DIR_IS_IN(cfg->addr);

	if (ep_num >= dcfg->num_endpoints) {
		return -EINVAL;
	}

	cfg->stat.halted = false;
	uint32_t v = R(dev, ENDPTCTRL[ep_num]);

	if (dir_in) {
		v &= ~AGM_USB_EPCTRL_TX_S;
		v |= AGM_USB_EPCTRL_TX_TR;
	} else {
		v &= ~AGM_USB_EPCTRL_RX_S;
		v |= AGM_USB_EPCTRL_RX_TR;
	}
	W(dev, ENDPTCTRL[ep_num], v);
	return 0;
}

/* ---- address / wakeup / speed ---------------------------------------- */

static int udc_agm_set_address(const struct device *dev, const uint8_t addr)
{
	/* DEVICEADDR: bits 24:31 hold the device address plus the
	 * AD (advanced? always-set) bit. Mirrors SDK
	 * USB_SetDeviceAddress: "addr << 25 | 1 << 24". */
	W(dev, DEVICEADDR, ((uint32_t)addr << 25) | BIT(24));
	return 0;
}

static int udc_agm_host_wakeup(const struct device *dev)
{
	W(dev, PORTSC, R(dev, PORTSC) | AGM_USB_PORTSC_FPR);
	return 0;
}

static enum udc_bus_speed udc_agm_device_speed(const struct device *dev)
{
	ARG_UNUSED(dev);
	return UDC_BUS_SPEED_FS;
}

/* ---- transfer enqueue / dequeue -------------------------------------- */

/*
 * Arm the hardware with the head of the endpoint buffer FIFO, but only if
 * no transfer is currently in flight on this endpoint direction.
 *
 * The USB stack may enqueue several buffers back-to-back before any of them
 * completes (e.g. the CDC ACM class pre-queues its whole RX pool). Like the
 * AgRV SDK / TinyUSB agrv2k_dcd reference, this controller keeps a single
 * dTD per endpoint direction, so buffers queued behind the armed one are
 * parked in the net_buf FIFO and fed to the controller one at a time:
 * ep_enqueue() calls kick(), and after each completion the completion
 * handler releases the endpoint and calls kick() again to feed the next
 * queued buffer.
 *
 * EP0 SETUP placeholders (udc_buf_info.setup) are never armed: the
 * controller captures SETUP packets into dQH[0].setup_buffer on its own,
 * and the placeholder net_buf only carries the SETUP to udc_setup_received().
 * Mirrors udc_mcux_ehci (queue-only for setup) and TinyUSB agrv2k_dcd.
 */
static void udc_agm_ep_kick(const struct device *dev,
			    struct udc_ep_config *const cfg)
{
	const struct udc_agm_config *dcfg = dev->config;
	struct udc_agm_data *priv = udc_get_private(dev);
	struct udc_buf_info *bi;
	struct net_buf *buf;
	uint8_t ep_num = USB_EP_GET_IDX(cfg->addr);
	bool dir_in = USB_EP_DIR_IS_IN(cfg->addr);
	uint8_t ep_idx = dir_in ? EP_IDX_IN(ep_num) : EP_IDX_OUT(ep_num);
	uint32_t dtd_len;
	uint32_t bit;
	unsigned int key;

	if (ep_num >= dcfg->num_endpoints) {
		return;
	}

	/*
	 * Zephyr receive buffers (control Data-OUT, Status-OUT and class
	 * RX) carry their capacity in buf->size; buf->len is zero until the
	 * driver reports the received byte count. Mirroring the SDK and the
	 * TinyUSB agrv2k_dcd reference (dcd_edpt_xfer() total_bytes), arm
	 * the dTD with the number of bytes the controller may accept:
	 *  - IN:                    buf->len (bytes to send)
	 *  - Status-OUT stage:      zero-length packet
	 *  - EP0 Data-OUT / class RX: buffer capacity. usbd_ch9 allocates
	 *    the EP0 Data-OUT buffer as ROUND_UP(wLength, MPS0), so the
	 *    capacity is always >= wLength; a short (< MPS) Data-OUT packet
	 *    terminates the dTD at exactly the received byte count.
	 */
	/* Bounded rather than `for (;;)`: the EP0 branch below waits for the
	 * workqueue to clear the SETUP tripwire, so a wedged workqueue (or a
	 * halt that never reports completions) would otherwise spin here
	 * forever while holding the UDC mutex the caller holds. */
	for (uint32_t spins = AGM_USB_WAIT_SPINS; spins > 0U; spins--) {
		key = irq_lock();

		if (cfg->stat.halted || udc_ep_is_busy(cfg)) {
			/* A transfer is in flight (or the EP is stalled):
			 * leave the queue alone, the completion path will
			 * feed the next buffer. */
			irq_unlock(key);
			return;
		}

		buf = udc_buf_peek(cfg);
		if (buf == NULL || udc_get_buf_info(buf)->setup) {
			irq_unlock(key);
			return;
		}

		if (ep_num == 0U && !dir_in &&
		    (R(dev, ENDPTSETUPSTAT) & BIT(0)) != 0U) {
			/* EP0 data/status may only be primed after the SETUP
			 * tripwire has been cleared by the workqueue. Wait
			 * with IRQs enabled so that clearing can happen. */
			irq_unlock(key);
			k_busy_wait(1);
			continue;
		}

		/*
		 * Keep the busy flag, the dTD/overlay writes and the PRIME
		 * under one irq_lock section: the completion handler pops the
		 * FIFO head under the same lock, so it can never release a
		 * buffer while this endpoint is between "about to arm" and
		 * "armed".
		 */
		udc_ep_set_busy(cfg, true);

		bi = udc_get_buf_info(buf);
		if (dir_in || bi->setup) {
			dtd_len = buf->len;
		} else if (bi->status) {
			dtd_len = 0U;
		} else {
			dtd_len = buf->size;
		}

		dtd_init(&priv->dTD[ep_idx], buf->data, dtd_len);
		priv->dQH[ep_idx].overlay.next =
			(uint32_t)(uintptr_t)&priv->dTD[ep_idx];
		priv->dQH[ep_idx].overlay.active = 0;
		priv->dQH[ep_idx].overlay.halted = 0;

		bit = dir_in ? EP_BIT_IN(ep_num) : EP_BIT_OUT(ep_num);

		LOG_DBG("arm ep 0x%02x len %u size %u setup %d status %d dtd %u prime bit %u",
			cfg->addr, buf->len, buf->size, bi->setup, bi->status,
			dtd_len, bit);
		W(dev, ENDPTPRIME, BIT(bit));
		irq_unlock(key);
		return;
	}

	/* Out of spins waiting for ENDPTSETUPSTAT to clear. Leave the buffer
	 * queued: the next enqueue or transfer completion calls kick() again,
	 * so this is a delay, not a dropped transfer. */
	LOG_ERR("USB: ep 0x%02x SETUP tripwire stuck; buffer left queued", cfg->addr);
}

static int udc_agm_ep_enqueue(const struct device *dev,
			      struct udc_ep_config *const cfg,
			      struct net_buf *buf)
{
	const struct udc_agm_config *dcfg = dev->config;
	uint8_t ep_num = USB_EP_GET_IDX(cfg->addr);

	if (ep_num >= dcfg->num_endpoints) {
		return -EINVAL;
	}

	/* Queue first: the stack may enqueue several buffers while a
	 * transfer is in flight; kick() feeds them one at a time. */
	udc_buf_put(cfg, buf);

	if (!cfg->stat.halted) {
		udc_agm_ep_kick(dev, cfg);
	}

	return 0;
}

static int udc_agm_ep_dequeue(const struct device *dev,
			      struct udc_ep_config *const cfg)
{
	unsigned int key = irq_lock();

	udc_ep_cancel_queued(dev, cfg);
	/* The in-flight dTD, if any, is orphaned: its completion will pop an
	 * empty queue and be dropped. Allow the next enqueue to re-arm. */
	udc_ep_set_busy(cfg, false);
	irq_unlock(key);
	return 0;
}

/* ---- ISR ------------------------------------------------------------- */

static void handle_bus_reset(const struct device *dev)
{
	struct udc_agm_data *priv = udc_get_private(dev);
	const struct udc_agm_config *dcfg = dev->config;
	unsigned int key;

	W(dev, ENDPTNAKEN, 0);
	W(dev, ENDPTSETUPSTAT, R(dev, ENDPTSETUPSTAT));
	W(dev, ENDPTCOMPLETE, R(dev, ENDPTCOMPLETE));
	W(dev, ENDPTNAK, R(dev, ENDPTNAK));

	/* Every wait below is bounded (AGM_USB_WAIT_SPINS): the previous
	 * unbounded spins pinned the UDC workqueue whenever the controller did
	 * not answer. Draining is still expected to be immediate; a timeout is
	 * reported and the dQH repaint that follows is the documented recovery
	 * (the engine is being re-initialised anyway). */
	if (!usb_wait_clear(dev, offsetof(struct udc_agm_regs, ENDPTPRIME), 0xFFFFFFFFU)) {
		LOG_ERR("USB: ENDPTPRIME did not drain");
	}
	W(dev, ENDPTFLUSH, 0xFFFFFFFFU);
	if (!usb_wait_clear(dev, offsetof(struct udc_agm_regs, ENDPTFLUSH), 0xFFFFFFFFU)) {
		LOG_ERR("USB: ENDPTFLUSH did not drain");
	}

	/* Wait for the host-driven port-reset handshake to complete.
	 * Mirrors SDK USB_PortWaitForReset() (usb.h:249). PORTSC.PR
	 * remains asserted by the controller until the host releases
	 * reset; repainting dQH before PR clears can desync the EHCI
	 * dQH state machine. A host that never releases it (half-inserted
	 * cable, brown-out) used to hang here forever. */
	if (!usb_wait_clear(dev, offsetof(struct udc_agm_regs, PORTSC),
			    AGM_USB_PORTSC_PR)) {
		LOG_ERR("USB: PORTSC.PR stuck; repainting dQH anyway");
	}

	for (int i = 0; i < QH_MAX; i++) {
		dqh_init(&priv->dQH[i]);
	}
	priv->dQH[0].int_on_setup = 1;

	W(dev, ENDPOINTLISTADDR, (uint32_t)(uintptr_t)priv->dQH);

	/*
	 * The flush above drops every dTD that was in flight. Queued net_bufs
	 * stay in the endpoint FIFOs (the stack re-arms them for the new
	 * session; control EP0 is drained by udc_setup_received() on the next
	 * SETUP), but no endpoint may keep the busy flag: nothing will ever
	 * complete the old transfers, so kick() must be allowed to re-arm the
	 * head buffer when the new session enqueues.
	 */
	key = irq_lock();
	for (uint16_t i = 0; i < dcfg->num_endpoints; i++) {
		udc_ep_set_busy(&dcfg->ep_cfg_out[i], false);
		udc_ep_set_busy(&dcfg->ep_cfg_in[i], false);
	}
	irq_unlock(key);
}

static void handle_xfer_complete_one(const struct device *dev, uint8_t ep_num,
				     bool dir_in)
{
	const struct udc_agm_config *dcfg = dev->config;
	struct udc_agm_data *priv = udc_get_private(dev);
	uint8_t ep_idx = dir_in ? EP_IDX_IN(ep_num) : EP_IDX_OUT(ep_num);
	uint8_t ep_addr = USB_EP_GET_ADDR(ep_num, dir_in ?
					  USB_EP_DIR_IN : USB_EP_DIR_OUT);
	struct udc_agm_dtd *dtd = &priv->dTD[ep_idx];
	struct udc_ep_config *cfg;
	struct net_buf *buf;
	unsigned int key;
	int err = 0;

	if (ep_num >= dcfg->num_endpoints) {
		return;
	}

	cfg = udc_get_ep_cfg(dev, ep_addr);
	if (cfg == NULL) {
		LOG_ERR("no ep_cfg for ep 0x%02x", ep_addr);
		return;
	}

	if (dtd->halted) {
		err = -EPIPE;
	} else if (dtd->tran_error || dtd->data_error) {
		err = -EIO;
	}

	/*
	 * Only one dTD is kept in flight per endpoint direction: kick()
	 * arms the FIFO head and marks the endpoint busy, so the buffer that
	 * just completed is the head. Pop it and clear busy while holding
	 * irq_lock so a concurrent enqueue/kick cannot observe a busy flag
	 * with an empty queue or arm a new dTD behind our back.
	 *
	 * A completion whose transfer was cancelled (new SETUP on EP0,
	 * dequeue, bus reset) may be processed after the queue was already
	 * drained by udc_setup_received()/udc_agm_ep_dequeue(); those paths
	 * clear the busy flag, so a NULL head here is simply dropped.
	 */
	key = irq_lock();
	buf = udc_buf_get(cfg);
	if (buf != NULL) {
		udc_ep_set_busy(cfg, false);
	}
	irq_unlock(key);

	if (buf == NULL) {
		LOG_DBG("xfer done ep 0x%02x: no queued buffer", ep_addr);
		return;
	}

	if (!dir_in && err == 0) {
		/* Report the number of bytes actually received.
		 * The controller writes back the remaining byte
		 * count into the dTD; expected_bytes holds the armed
		 * length (mirrors agrv2k_dcd.c xfer_complete).
		 */
		uint32_t rx = dtd->expected_bytes - dtd->total_bytes;

		if (rx > buf->size) {
			rx = buf->size;
		}

		net_buf_add(buf, rx);
		LOG_DBG("xfer done ep 0x%02x rx %u err %d", ep_addr, rx, err);
	} else {
		LOG_DBG("xfer done ep 0x%02x err %d", ep_addr, err);
	}

	udc_submit_ep_event(dev, buf, err);

	/* Feed the next buffer queued while this transfer was in flight. */
	udc_agm_ep_kick(dev, cfg);
}

/*
 * Process USB events deferred from the ISR on the UDC workqueue.
 *
 * Everything that touches the UDC mutex or the net_buf queues must run
 * in thread context: udc_setup_received() documents thread-context-only
 * (k_mutex_lock K_FOREVER), and handle_bus_reset() may spin for the
 * whole host-driven reset window. Processing setup before completion
 * snapshots in each drain preserves the ordering the hardware produces
 * (a SETUP aborts the in-flight EP0 transfer; its stale completion is
 * then drained by udc_setup_received() and dropped by the NULL check in
 * handle_xfer_complete_one()).
 */
static void udc_agm_evt_handler(struct k_work *item)
{
	struct udc_agm_data *priv = CONTAINER_OF(item, struct udc_agm_data,
						 evt_work);
	const struct device *dev = priv->dev;
	uint32_t flags;
	uint32_t complete;

	/*
	 * Loop: the ISR may accumulate new events while this handler runs
	 * (the atomic swaps below pick up every bit set so far).
	 */
	for (;;) {
		flags = atomic_and(&priv->evt_flags, 0);
		complete = atomic_and(&priv->pending_complete, 0);

		if ((flags | complete) == 0U) {
			break;
		}

		if ((flags & AGM_USB_EVT_RESET) != 0U) {
			/*
			 * A bus reset invalidates every pending transfer;
			 * drop stale completion snapshots before repainting
			 * the dQH table.
			 */
			atomic_set(&priv->pending_complete, 0);
			complete = 0U;
			handle_bus_reset(dev);
			udc_submit_event(dev, UDC_EVT_RESET, 0);
			LOG_DBG("work: bus reset done");
		}

		if ((flags & AGM_USB_EVT_SETUP) != 0U) {
			LOG_DBG("work: udc_setup_received");
			udc_setup_received(dev, priv->setup);
		}

		while (complete != 0U) {
			uint8_t bit = (uint8_t)(find_lsb_set(complete) - 1U);

			if (bit < 16U) {
				handle_xfer_complete_one(dev, bit, false);
			} else {
				handle_xfer_complete_one(dev,
							 bit - 16U, true);
			}
			complete &= ~BIT(bit);
		}
	}
}

/* Zephyr's ISR type -- see uart_agm.c for the full reason (the handler has to
 * be a bare identifier for IRQ_CONNECT()'s __isr_ ## name paste, and the
 * native/POSIX arch is the only one whose ARCH_IRQ_CONNECT() does not cast it).
 * This is the last driver in the tree that needed it. */
static void udc_agm_isr(const void *isr_arg)
{
	const struct device *dev = isr_arg;
	struct udc_agm_data *priv = udc_get_private(dev);
	const uint32_t enabled_ints =
		AGM_USB_STS_UI  | AGM_USB_STS_UEI | AGM_USB_STS_PCI |
		AGM_USB_STS_SEI | AGM_USB_STS_URI | AGM_USB_STS_SLI;
	uint32_t status = R(dev, USBSTS) & enabled_ints;
	bool evt = false;

	if (status == 0U) {
		return;
	}
	W(dev, USBSTS, status);

	if (status & AGM_USB_STS_URI) {
		LOG_DBG("ISR: bus reset");
		atomic_or(&priv->evt_flags, AGM_USB_EVT_RESET);
		evt = true;
	}
	if (status & AGM_USB_STS_SLI) {
		udc_submit_event(dev, UDC_EVT_SUSPEND, 0);
	}
	if (status & AGM_USB_STS_PCI) {
		if ((R(dev, PORTSC) & AGM_USB_PORTSC_SUSP) == 0U) {
			udc_submit_event(dev, UDC_EVT_RESUME, 0);
		}
	}
	if (status & AGM_USB_STS_UI) {
		uint32_t setup_stat = R(dev, ENDPTSETUPSTAT);

		if (setup_stat != 0U) {
			W(dev, ENDPTSETUPSTAT, setup_stat);
			/*
			 * Copy the SETUP out before the controller can
			 * overwrite dQH[0].setup_buffer with a later packet.
			 * The stack-facing udc_setup_received() is thread
			 * context only, so dispatch happens in the
			 * workqueue handler.
			 */
			memcpy(priv->setup,
			       (const uint8_t *)priv->dQH[0].setup_buffer,
			       sizeof(priv->setup));
			atomic_or(&priv->evt_flags, AGM_USB_EVT_SETUP);
			LOG_DBG("ISR: setup 0x%02x%02x%02x%02x%02x%02x%02x%02x",
				priv->setup[0], priv->setup[1], priv->setup[2],
				priv->setup[3], priv->setup[4], priv->setup[5],
				priv->setup[6], priv->setup[7]);
			evt = true;
		}

		uint32_t complete = R(dev, ENDPTCOMPLETE);

		if (complete != 0U) {
			W(dev, ENDPTCOMPLETE, complete);
			atomic_or(&priv->pending_complete, complete);
			LOG_DBG("ISR: complete 0x%08x", complete);
			evt = true;
		}
	}

	if (evt) {
		k_work_submit_to_queue(udc_get_work_q(), &priv->evt_work);
	}
}

/* ---- udc_api glue ---------------------------------------------------- */

static void udc_agm_lock(const struct device *dev)
{
	udc_lock_internal(dev, K_FOREVER);
}

static void udc_agm_unlock(const struct device *dev)
{
	udc_unlock_internal(dev);
}

static const struct udc_api udc_agm_api = {
	.lock = udc_agm_lock,
	.unlock = udc_agm_unlock,
	.device_speed = udc_agm_device_speed,
	.init = udc_agm_init,
	.enable = udc_agm_enable,
	.disable = udc_agm_disable,
	.shutdown = udc_agm_shutdown,
	.set_address = udc_agm_set_address,
	.host_wakeup = udc_agm_host_wakeup,
	.ep_enable = udc_agm_ep_enable,
	.ep_disable = udc_agm_ep_disable,
	.ep_set_halt = udc_agm_ep_set_halt,
	.ep_clear_halt = udc_agm_ep_clear_halt,
	.ep_enqueue = udc_agm_ep_enqueue,
	.ep_dequeue = udc_agm_ep_dequeue,
};

/* ---- per-instance bus ------------------------------------------------ */

#define UDC_AGM_DQH_SIZE (QH_MAX * sizeof(struct udc_agm_dqh))
#define UDC_AGM_DTD_SIZE (QH_MAX * sizeof(struct udc_agm_dtd))

/* Forward-declare per-instance IRQ config functions so they can be
 * referenced from udc_agm_config_##n before their full definition
 * appears further down in the same translation unit. */
#define UDC_AGM_DECLARE_IRQ_CONFIG(n) \
	static void udc_agm_irq_config_func_##n(const struct device *dev);

DT_INST_FOREACH_STATUS_OKAY(UDC_AGM_DECLARE_IRQ_CONFIG)
#define UDC_AGM_DEVICE_DEFINE(n)                                             \
	__aligned(4096) static struct udc_agm_dqh                               \
		udc_agm_dqh_pool_##n[QH_MAX];                                 \
	__aligned(32) static struct udc_agm_dtd                               \
		udc_agm_dtd_pool_##n[QH_MAX];                                 \
	static struct udc_ep_config                                           \
		udc_agm_ep_cfg_out_##n[DT_INST_PROP(n, num_bidir_endpoints)];\
	static struct udc_ep_config                                           \
		udc_agm_ep_cfg_in_##n[DT_INST_PROP(n, num_bidir_endpoints)]; \
                                                                            \
	static const struct udc_agm_config udc_agm_config_##n = {            \
		.regs = (volatile struct udc_agm_regs *)                      \
			DT_REG_ADDR(DT_DRV_INST(n)),                         \
		.num_endpoints = DT_INST_PROP(n, num_bidir_endpoints),        \
		.ep_cfg_out = udc_agm_ep_cfg_out_##n,                         \
		.ep_cfg_in = udc_agm_ep_cfg_in_##n,                           \
		.dqh_pool = udc_agm_dqh_pool_##n,                            \
		.dtd_pool = udc_agm_dtd_pool_##n,                            \
		.irq_config_func = udc_agm_irq_config_func_##n,               \
	};                                                                   \
                                                                            \
	static struct udc_agm_data udc_priv_##n = {                          \
		.dQH = udc_agm_dqh_pool_##n,                                 \
		.dTD = udc_agm_dtd_pool_##n,                                 \
	};                                                                   \
                                                                            \
	static struct udc_data udc_data_##n = {                              \
		.mutex = Z_MUTEX_INITIALIZER(udc_data_##n.mutex),            \
		.priv = &udc_priv_##n,                                       \
	};                                                                   \
                                                                            \
	DEVICE_DT_INST_DEFINE(n, udc_agm_preinit, NULL,                         \
			      &udc_data_##n, &udc_agm_config_##n,             \
			      POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEVICE,\
			      &udc_agm_api);                                  \
                                                                            \
	static void udc_agm_irq_config_func_##n(const struct device *dev)    \
	{                                                                    \
		ARG_UNUSED(dev);                                              \
		IRQ_CONNECT(DT_INST_IRQN(n), DT_INST_IRQ(n, priority),        \
			    udc_agm_isr, DEVICE_DT_INST_GET(n), 0);          \
		irq_enable(DT_INST_IRQN(n));                                  \
	}

DT_INST_FOREACH_STATUS_OKAY(UDC_AGM_DEVICE_DEFINE)
