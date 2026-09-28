/*
 * Copyright (c) 2026 AgRV Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_DRIVERS_USB_UHC_AGM_H
#define ZEPHYR_DRIVERS_USB_UHC_AGM_H

#include <stdint.h>

/*
 * The USB0 register window (base 0x4100_1000, operational registers from
 * CAPLENGTH at 0x100) and its USBCMD/USBSTS/PORTSC/USBMODE bit names are
 * shared with the device-mode driver: udc_agm.h is the single source of
 * truth for that map, including the host-only HCH/PS/AS status bits and the
 * PORTSC speed field. This driver includes it instead of carrying a second
 * copy: the AHB gate bit was already defined twice in this tree once, and the
 * two copies drifting apart is exactly the kind of bug that produces.
 */
#include "../udc/udc_agm.h"

/* ---- PORTSC bits the device driver never needed (EHCI Table 2-16) ---- */

/*
 * The device driver only needs CCS/CSC/PE/PEC/FPR/SUSP/PR/PP, the wake bits,
 * PHCD/PFSC and PSPD. Host mode additionally writes the change bits out and
 * reads the line status before a reset.
 */
#define AGM_USB_PORTSC_OCC		BIT(5)	/* over-current change, W1C */
#define AGM_USB_PORTSC_PO		BIT(13)	/* port owner (companion routing) */
#define AGM_USB_PORTSC_LS_SHIFT		10U	/* line status: D+/D- state */
#define AGM_USB_PORTSC_LS_MASK		(0x3U << AGM_USB_PORTSC_LS_SHIFT)

/* PORTSC change bits: written 1 to clear. A read-modify-write that keeps the
 * R/W bits must therefore mask them out first (writing 0 has no effect, but
 * writing 1 clears the event). */
#define AGM_USB_PORTSC_W1C						\
	(AGM_USB_PORTSC_CSC | AGM_USB_PORTSC_PEC | AGM_USB_PORTSC_OCC)

/* PORTSC.PSPD encoding, shared with the queue head's "endpoint speed" field:
 * 0 = full speed, 1 = low speed, 2 = high speed (EHCI/NXP). */
#define AGM_USB_SPEED_FS		0U
#define AGM_USB_SPEED_LS		1U
#define AGM_USB_SPEED_HS		2U

/* ---- EHCI host schedule ---------------------------------------------- */

/* Link pointer: bit 0 terminates the list, bits [2:1] are the structure type
 * (1 = queue head, 0 = qTD). Written as a whole word so the type bits land
 * where the controller expects them. */
union uhc_agm_link {
	uint32_t address;
	struct {
		uint32_t terminate : 1;
		uint32_t type : 2;
	};
};

/* Queue element transfer descriptor (EHCI 3.5): 32 bytes, 32-byte aligned.
 *
 * Word 1 is the "alternate next qTD pointer", which this driver does not
 * use. TinyUSB's EHCI port overlays a software "used" flag and the expected
 * transfer size on it, and this driver does the same: the controller never
 * writes word 1, so the expected size survives until the completion handler
 * subtracts the remaining count the controller *does* write back. */
struct uhc_agm_qtd {
	union uhc_agm_link next;
	union {
		union uhc_agm_link alternate;
		struct {
			uint32_t reserved0 : 5;
			uint32_t used : 1;
			uint32_t reserved1 : 10;
			uint32_t expected_bytes : 16;
		};
	};
	union {
		uint32_t token;
		struct {
			volatile uint32_t ping_err : 1;
			volatile uint32_t split_state : 1;
			volatile uint32_t missed_uframe : 1;
			volatile uint32_t xact_err : 1;
			volatile uint32_t babble_err : 1;
			volatile uint32_t buffer_err : 1;
			volatile uint32_t halted : 1;
			volatile uint32_t active : 1;
			uint32_t pid : 2;
			volatile uint32_t err_count : 2;
			volatile uint32_t current_page : 3;
			uint32_t int_on_complete : 1;
			volatile uint32_t total_bytes : 15;
			volatile uint32_t data_toggle : 1;
		};
	};
	uint32_t buffer[5];
};

/* Queue head (EHCI 3.6). The controller owns words 0-11; words 12-15 are
 * EHCI-reserved padding, kept so every instance is a multiple of 32 bytes
 * apart, which is what the link-pointer alignment (low 5 bits dropped)
 * requires. */
struct uhc_agm_qhd {
	union uhc_agm_link next;

	union {
		uint32_t ep_char;
		struct {
			uint32_t dev_addr : 7;
			uint32_t fl_inactive_next_xact : 1;
			uint32_t ep_number : 4;
			uint32_t ep_speed : 2;
			uint32_t data_toggle_control : 1;
			uint32_t head_list_flag : 1;
			uint32_t max_packet_size : 11;
			uint32_t fl_ctrl_ep_flag : 1;
			uint32_t nak_reload : 4;
		};
	};

	union {
		uint32_t ep_caps;
		struct {
			uint32_t int_smask : 8;
			uint32_t fl_int_cmask : 8;
			uint32_t fl_hub_addr : 7;
			uint32_t fl_hub_port : 7;
			uint32_t mult : 2;
		};
	};

	volatile uint32_t current;
	struct uhc_agm_qtd overlay;
	uint32_t reserved[4];
};

/* PID field of a qTD token (EHCI: 0 = OUT, 1 = IN, 2 = SETUP). */
enum {
	UHC_AGM_PID_OUT = 0,
	UHC_AGM_PID_IN = 1,
	UHC_AGM_PID_SETUP = 2,
};

/*
 * Frame list size. 8 entries is the smallest schedule TinyUSB's EHCI port
 * uses on this IP (ChipIdea encodes it as bits [3:2] plus the MSB in bit 15)
 * and still covers 1/2/4/8 ms interrupt intervals; the vendor hcd passes the
 * same shape to the same hardware.
 */
#define UHC_AGM_FRAMELIST_SIZE		8U
#define UHC_AGM_FRAMELIST_CMD					\
	(AGM_USB_CMD_FS0 | AGM_USB_CMD_FS1 | AGM_USB_CMD_FS2)
#define UHC_AGM_MICROFRAMES_PER_FRAME	8U

#endif /* ZEPHYR_DRIVERS_USB_UHC_AGM_H */
