/*
 * Copyright (c) 2026 AgRV Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_DRIVERS_USB_UDC_AGM_H
#define ZEPHYR_DRIVERS_USB_UDC_AGM_H

#include <stdint.h>
#include <zephyr/sys/sys_io.h>

/*
 * AgRV2K USB0 — FSL/ChipIdea-like dual-role controller (device-only driver).
 *
 * Register layout and bit definitions extracted from the SDK reference:
 *   framework-agrv_sdk/src/usb.h (USB_TypeDef + USB_* macros)
 *   framework-agrv_tinyusb/hw/mcu/agm/agrv2k.h  (USB_DataTypeDef, dQH/dTD)
 *
 * Clocking: PLLCLK1 = 60 MHz feeds USB0.
 *
 * Every register-level finding from the bring-up is in the commit
 * messages for this file; this header is the single source of truth.
 */

/* ---- register block ---------------------------------------------------- */

struct udc_agm_regs {
	uint8_t  RESERVED0[0x80];
	uint32_t GPTIMER0LD;             /* 0x080 */
	uint32_t GPTIMER0CTRL;           /* 0x084 */
	uint32_t GPTIMER1LD;             /* 0x088 */
	uint32_t GPTIMER1CTRL;           /* 0x08C */
	uint8_t  RESERVED1[0x70];        /* 0x090..0x0FF */
	uint8_t  CAPLENGTH;              /* 0x100 */
	uint8_t  RESERVED2;              /* 0x101 */
	uint16_t HCIVERSION;             /* 0x102 */
	uint32_t HCSPARAMS;              /* 0x104 */
	uint32_t HCCPARAMS;              /* 0x108 */
	uint32_t RESERVED3[13];          /* 0x10C..0x13C */
	uint32_t USBCMD;                 /* 0x140 */
	uint32_t USBSTS;                 /* 0x144 */
	uint32_t USBINTR;                /* 0x148 */
	uint32_t FRINDEX;                /* 0x14C */
	uint32_t RESERVED4;              /* 0x150 */
	union {
		uint32_t PERIODICLISTBASE;
		uint32_t DEVICEADDR;
	};                               /* 0x154 */
	union {
		uint32_t ASYNCLISTADDR;
		uint32_t ENDPOINTLISTADDR;
	};                               /* 0x158 */
	uint32_t RESERVED5;              /* 0x15C */
	uint32_t BURSTSIZE;              /* 0x160 */
	uint32_t TXFILLTUNING;           /* 0x164 */
	uint32_t RESERVED6[4];           /* 0x168..0x174 */
	uint32_t ENDPTNAK;               /* 0x178 */
	uint32_t ENDPTNAKEN;             /* 0x17C */
	uint32_t CONFIGFLAG;             /* 0x180 */
	uint32_t PORTSC;                 /* 0x184 */
	uint32_t RESERVED7[7];           /* 0x188..0x1A0 */
	uint32_t OTGSC;                  /* 0x1A4 */
	uint32_t USBMODE;                /* 0x1A8 */
	uint32_t ENDPTSETUPSTAT;         /* 0x1AC */
	uint32_t ENDPTPRIME;             /* 0x1B0 */
	uint32_t ENDPTFLUSH;             /* 0x1B4 */
	uint32_t ENDPTSTATUS;            /* 0x1B8 */
	uint32_t ENDPTCOMPLETE;          /* 0x1BC */
	uint32_t ENDPTCTRL[4];           /* 0x1C0..0x1CF */
};

/* ---- USBCMD bits ------------------------------------------------------ */
#define AGM_USB_CMD_RS              BIT(0)
#define AGM_USB_CMD_RST             BIT(1)
#define AGM_USB_CMD_FS0             BIT(2)
#define AGM_USB_CMD_FS1             BIT(3)
#define AGM_USB_CMD_PSE             BIT(4)
#define AGM_USB_CMD_ASE             BIT(5)
#define AGM_USB_CMD_IAA             BIT(6)
#define AGM_USB_CMD_SUTW            BIT(13)
#define AGM_USB_CMD_ATDTW           BIT(14)
#define AGM_USB_CMD_FS2             BIT(15)
#define AGM_USB_CMD_ITC             (0xFFU << 16)
#define AGM_USB_CMD_ITC_SHIFT       16

/* ---- USBSTS bits (write-1-to-clear) ---------------------------------- */
#define AGM_USB_STS_UI              BIT(0)
#define AGM_USB_STS_UEI             BIT(1)
#define AGM_USB_STS_PCI             BIT(2)
#define AGM_USB_STS_FRI             BIT(3)
#define AGM_USB_STS_SEI             BIT(4)
#define AGM_USB_STS_AAI             BIT(5)
#define AGM_USB_STS_URI             BIT(6)
#define AGM_USB_STS_SRI             BIT(7)
#define AGM_USB_STS_SLI             BIT(8)
#define AGM_USB_STS_HCH             BIT(12)
#define AGM_USB_STS_RCL             BIT(13)
#define AGM_USB_STS_PS              BIT(14)
#define AGM_USB_STS_AS              BIT(15)
#define AGM_USB_STS_NAKI            BIT(16)
#define AGM_USB_STS_UAI             BIT(18)
#define AGM_USB_STS_UPI             BIT(19)
#define AGM_USB_STS_TI0             BIT(24)
#define AGM_USB_STS_TI1             BIT(25)

/* ---- PORTSC bits ------------------------------------------------------ */
#define AGM_USB_PORTSC_CCS          BIT(0)
#define AGM_USB_PORTSC_CSC          BIT(1)
#define AGM_USB_PORTSC_PE           BIT(2)
#define AGM_USB_PORTSC_PEC          BIT(3)
#define AGM_USB_PORTSC_FPR          BIT(6)
#define AGM_USB_PORTSC_SUSP         BIT(7)
#define AGM_USB_PORTSC_PR           BIT(8)
#define AGM_USB_PORTSC_PP           BIT(12)
#define AGM_USB_PORTSC_WKCN         BIT(20)
#define AGM_USB_PORTSC_WKDS         BIT(21)
#define AGM_USB_PORTSC_WKOC         BIT(22)
#define AGM_USB_PORTSC_PHCD         BIT(23)
#define AGM_USB_PORTSC_PFSC         BIT(24)
#define AGM_USB_PORTSC_PSPD_SHIFT   26
#define AGM_USB_PORTSC_PSPD_MASK    (0x3U << AGM_USB_PORTSC_PSPD_SHIFT)

/* ---- OTGSC bits (subset, ID only) ------------------------------------- */
#define AGM_USB_OTGSC_ID            BIT(8)   /* 0 = A-device (host), 1 = B-device */
#define AGM_USB_OTGSC_IDIS          BIT(16)
#define AGM_USB_OTGSC_IDIE          BIT(24)

/* ---- USBMODE bits ---------------------------------------------------- */
#define AGM_USB_MODE_CM_MASK        0x3U     /* 00=idle, 01=reserved, 10=device, 11=host */
#define AGM_USB_MODE_CM_DEVICE      0x2U
#define AGM_USB_MODE_CM_HOST        0x3U
#define AGM_USB_MODE_SLOM           BIT(3)

/* ---- ENDPTCTRL bits (per-endpoint, RX in [15:8], TX in [31:16]) ------ */
#define AGM_USB_EPCTRL_RX_S         BIT(0)
#define AGM_USB_EPCTRL_RX_TYPE_SHIFT 2
#define AGM_USB_EPCTRL_RX_TYPE_MASK (0x3U << AGM_USB_EPCTRL_RX_TYPE_SHIFT)
#define AGM_USB_EPCTRL_RX_TI        BIT(5)
#define AGM_USB_EPCTRL_RX_TR        BIT(6)
#define AGM_USB_EPCTRL_RX_E         BIT(7)
#define AGM_USB_EPCTRL_TX_S         BIT(16)
#define AGM_USB_EPCTRL_TX_TYPE_SHIFT 18
#define AGM_USB_EPCTRL_TX_TYPE_MASK (0x3U << AGM_USB_EPCTRL_TX_TYPE_SHIFT)
#define AGM_USB_EPCTRL_TX_TI        BIT(21)
#define AGM_USB_EPCTRL_TX_TR        BIT(22)
#define AGM_USB_EPCTRL_TX_E         BIT(23)

/* Endpoint type values (2 bits, shifted into TYPE field) */
#define AGM_USB_EP_TYPE_CTRL        0U
#define AGM_USB_EP_TYPE_ISO         1U
#define AGM_USB_EP_TYPE_BULK        2U
#define AGM_USB_EP_TYPE_INT         3U
/* ---- dTD (device transfer descriptor, 32-byte aligned) --------------- */
struct udc_agm_dtd {
	uint32_t next;
	union {
		uint32_t status;
		struct {
			uint32_t reserved0           : 3;
			uint32_t tran_error          : 1;
			uint32_t reserved1           : 1;
			uint32_t data_error          : 1;
			uint32_t halted              : 1;
			uint32_t active              : 1;
			uint32_t reserved2           : 2;
			uint32_t mult_override       : 2;
			uint32_t reserved3           : 3;
			uint32_t int_on_complete     : 1;
			uint32_t total_bytes         : 15;
			uint32_t reserved4           : 1;
		};
	};
	uint32_t buffer_pointer[5];
	uint16_t expected_bytes;
	uint16_t reserved5;
} __packed;

/* ---- dQH (device queue head, 64-byte aligned) ----------------------- */
struct udc_agm_dqh {
	union {
		uint32_t info;
		struct {
			uint32_t reserved0           : 15;
			uint32_t int_on_setup        : 1;
			uint32_t max_packet_length   : 11;
			uint32_t reserved1           : 2;
			uint32_t zlt_disable         : 1;
			uint32_t mult                : 2;
		};
	};
	uint32_t current;                  /* current dTD pointer */
	struct udc_agm_dtd overlay;        /* transfer overlay (embedded dTD) */
	uint32_t setup_buffer[2];          /* 8-byte setup packet */
	uint32_t reserved[2];
	struct udc_agm_dtd *head;
	struct udc_agm_dtd *tail;
};
#define AGM_USB_DTD_STATUS_ACTIVE           BIT(7)
#define AGM_USB_DTD_STATUS_HALTED           BIT(6)
#define AGM_USB_DTD_STATUS_DATA_ERROR       BIT(5)
#define AGM_USB_DTD_STATUS_TRAN_ERROR       BIT(3)
#define AGM_USB_DTD_STATUS_IOC              BIT(15)

/* dTD next-pointer TERMINATE */
#define AGM_USB_TD_TERMINATE                0x1U

/* ENDPTCOMPLETE / ENDPTPRIME / ENDPTFLUSH bit indexing.
 *   PERBn (OUT) sits at bit n, PETBn (IN) sits at bit (16+n).
 *   Matches SDK USB_SetEndPtPrime(TU_BIT(ep_idx/2 + 16*(ep_idx&1)))
 *   because ep_idx/2 == ep when dQH[2*ep]=OUT, dQH[2*ep+1]=IN.
 */
#define AGM_USB_EP_BIT_OUT(ep)  ((ep) & 0x1FU)
#define AGM_USB_EP_BIT_IN(ep)   (16U + ((ep) & 0x1FU))

/* Endpoint count (FSL-style: 4 bi-directional EPs). */
#define AGM_USB_NUM_EPS              4U
#define AGM_USB_NUM_QH               (AGM_USB_NUM_EPS * 2U)
#define AGM_USB_EP0_MPS              64U
#define AGM_USB_FS_MPS               64U

#endif /* ZEPHYR_DRIVERS_USB_UDC_AGM_H */
