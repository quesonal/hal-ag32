/*
 * Copyright (c) 2026 AgRV Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * AgRV2K UART driver (native).
 *
 * The on-die uart0 IP is register-compatible with the ARM PL011
 * PrimeCell UART (DR / RSR / FR / IBRD / FBRD / LCR_H / CR / IMSC / RIS /
 * MIS / ICR — see dts/riscv/agm/agrv2k.dtsi for the address). This
 * driver is written from scratch against that public register map:
 *
 *   - AgRV2K has no vendor-specific power/clock callbacks (the
 *     FPGA bitstream gates the IP clock via SYS.APB_CLKENABLE, opened
 *     in soc.c from the node's agm,apb-clkenable-bit before the first
 *     register access — see soc/agm/agrv2k/soc.c);
 *   - the IRQ line is a PLIC source number; on CONFIG_MULTI_LEVEL_INTERRUPTS=y
 *     platforms (RISC-V + PLIC, which AgRV2K is) IRQ_CONNECT() and
 *     irq_enable() need the multi-level encoded IRQ number, returned by
 *     DT_INST_IRQN(). DT_INST_IRQ_BY_IDX(n, idx, irq) returns the raw
 *     interrupt cell and would route to the wrong handler — call sites
 *     use DT_INST_IRQN_BY_IDX(n, idx) and irq_enable(DT_INST_IRQN_BY_IDX(n, 0)).
 *
 * The UART API surface (uart_driver_api), the device_init level
 * (PRE_KERNEL_1, so the console is ready before the kernel prints) and
 * the public behaviour match the PL011 reference closely enough that
 * the standard console / poll_out / interrupt-driven drivers that link
 * against CONFIG_UART_PL011 do not need a special case here.
 */

#define DT_DRV_COMPAT agm_agrv2k_uart

#include <zephyr/kernel.h>
#include <zephyr/arch/cpu.h>
#include <zephyr/init.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/irq.h>
#include <zephyr/spinlock.h>
#include <zephyr/sys/barrier.h>

/* ---- register map (PL011-compatible) --------------------------------- */

/*
 * The register struct lives in this file (not a separate header) so this
 * driver is self-contained: a header shared with another .c would be a
 * fork of a definition that has to stay single-sourced, and here we
 * own the IP, the .c and the .h.
 */
struct agm_uart_regs {
	uint32_t dr;			/* 0x00 data register (RX on read, TX on write) */
	union {
		uint32_t rsr;		/* 0x04 receive status (read) */
		uint32_t ecr;		/* 0x04 error clear (write) */
	};
	uint32_t reserved_0[4];		/* 0x08..0x14 */
	uint32_t fr;			/* 0x18 flag register */
	uint32_t reserved_1;		/* 0x1C */
	uint32_t ilpr;			/* 0x20 (not used; IrDA low-power counter) */
	uint32_t ibrd;			/* 0x24 integer baud-rate divisor */
	uint32_t fbrd;			/* 0x28 fractional baud-rate divisor */
	uint32_t lcr_h;			/* 0x2C line control */
	uint32_t cr;			/* 0x30 control */
	uint32_t ifls;			/* 0x34 interrupt FIFO level select */
	uint32_t imsc;			/* 0x38 interrupt mask set/clear */
	uint32_t ris;			/* 0x3C raw interrupt status */
	uint32_t mis;			/* 0x40 masked interrupt status */
	uint32_t icr;			/* 0x44 interrupt clear (write-only) */
	uint32_t dmacr;			/* 0x48 DMA control */
};

/* Flag register (FR) bits */
#define AGM_FR_CTS		BIT(0)	/* clear to send (inverted) */
#define AGM_FR_DSR		BIT(1)	/* data set ready (inverted) */
#define AGM_FR_DCD		BIT(2)	/* data carrier detect (inverted) */
#define AGM_FR_BUSY		BIT(3)	/* UART busy transmitting */
#define AGM_FR_RXFE		BIT(4)	/* RX FIFO empty */
#define AGM_FR_TXFF		BIT(5)	/* TX FIFO full */
#define AGM_FR_RXFF		BIT(6)	/* RX FIFO full */
#define AGM_FR_TXFE		BIT(7)	/* TX FIFO empty */
#define AGM_FR_RI		BIT(8)	/* ring indicator (inverted) */

/* Receive status / error clear (RSR / ECR) bits */
#define AGM_RSR_FE		BIT(0)	/* framing error */
#define AGM_RSR_PE		BIT(1)	/* parity error */
#define AGM_RSR_BE		BIT(2)	/* break error */
#define AGM_RSR_OE		BIT(3)	/* overrun error */
#define AGM_RSR_ERROR_MASK	(AGM_RSR_FE | AGM_RSR_PE | \
				 AGM_RSR_BE | AGM_RSR_OE)

/* Line control (LCR_H) bits */
#define AGM_LCRH_BRK		BIT(0)	/* send break */
#define AGM_LCRH_PEN		BIT(1)	/* parity enable */
#define AGM_LCRH_EPS		BIT(2)	/* even parity select */
#define AGM_LCRH_STP2		BIT(3)	/* two stop bits */
#define AGM_LCRH_FEN		BIT(4)	/* enable FIFOs */
#define AGM_LCRH_WLEN_SHIFT	5	/* word length field */
#define AGM_LCRH_WLEN_WIDTH	2
#define AGM_LCRH_SPS		BIT(7)	/* stick parity */
#define AGM_LCRH_WLEN_SIZE(x)	((x) - 5)
#define AGM_LCRH_FORMAT_MASK	(AGM_LCRH_PEN | AGM_LCRH_EPS | \
				 AGM_LCRH_SPS | \
				 (BIT_MASK(AGM_LCRH_WLEN_WIDTH) << \
				  AGM_LCRH_WLEN_SHIFT))
#define AGM_LCRH_PARITY_EVEN	(AGM_LCRH_PEN | AGM_LCRH_EPS)
#define AGM_LCRH_PARITY_ODD	(AGM_LCRH_PEN)
#define AGM_LCRH_PARITY_NONE	(0)

/* Control register (CR) bits */
#define AGM_CR_UARTEN		BIT(0)	/* UART enable */
#define AGM_CR_SIREN		BIT(1)	/* IrDA SIR enable */
#define AGM_CR_SIRLP		BIT(2)	/* IrDA SIR low-power */
#define AGM_CR_LBE		BIT(7)	/* loopback enable (test only) */
#define AGM_CR_TXE		BIT(8)	/* transmitter enable */
#define AGM_CR_RXE		BIT(9)	/* receiver enable */
#define AGM_CR_DTR		BIT(10)
#define AGM_CR_RTS		BIT(11)
#define AGM_CR_Out1		BIT(12)
#define AGM_CR_Out2		BIT(13)
#define AGM_CR_RTSEn		BIT(14)	/* RTS hardware flow-control enable */
#define AGM_CR_CTSEn		BIT(15)	/* CTS hardware flow-control enable */

/* Interrupt FIFO level select (IFLS) bits */
#define AGM_IFLS_RXIFLSEL_M	GENMASK(5, 3)
#define AGM_IFLS_TXIFLSEL_M	GENMASK(2, 0)
#define AGM_IFLS_RX_1_2_FULL	2U	/* trigger RX IRQ at >= 1/2 FIFO full */
#define AGM_IFLS_TX_1_8_FULL	0U	/* trigger TX IRQ at <= 1/8 FIFO full */

/* Interrupt mask / status (IMSC / RIS / MIS / ICR) bits */
#define AGM_INT_RIMIM		BIT(0)	/* RTR modem interrupt mask */
#define AGM_INT_CTSMIM		BIT(1)	/* CTS modem interrupt mask */
#define AGM_INT_DCDMIM		BIT(2)	/* DCD modem interrupt mask */
#define AGM_INT_DSRMIM		BIT(3)	/* DSR modem interrupt mask */
#define AGM_INT_RXIM		BIT(4)	/* receive interrupt mask */
#define AGM_INT_TXIM		BIT(5)	/* transmit interrupt mask */
#define AGM_INT_RTIM		BIT(6)	/* receive timeout interrupt mask */
#define AGM_INT_FEIM		BIT(7)	/* framing error interrupt mask */
#define AGM_INT_PEIM		BIT(8)	/* parity error interrupt mask */
#define AGM_INT_BEIM		BIT(9)	/* break error interrupt mask */
#define AGM_INT_OEIM		BIT(10)	/* overrun error interrupt mask */
#define AGM_INT_ERROR_MASK	(AGM_INT_FEIM | AGM_INT_PEIM | \
				 AGM_INT_BEIM | AGM_INT_OEIM)
#define AGM_INT_MASK_ALL	(AGM_INT_OEIM | AGM_INT_BEIM | \
				 AGM_INT_PEIM | AGM_INT_FEIM | \
				 AGM_INT_RIMIM | AGM_INT_CTSMIM | \
				 AGM_INT_DCDMIM | AGM_INT_DSRMIM | \
				 AGM_INT_RXIM | AGM_INT_TXIM | \
				 AGM_INT_RTIM)

/* ---- driver structures ---------------------------------------------- */

struct agm_uart_config {
	DEVICE_MMIO_ROM;
#if defined(CONFIG_PINCTRL)
	const struct pinctrl_dev_config *pincfg;
#endif
#if defined(CONFIG_UART_INTERRUPT_DRIVEN) && defined(CONFIG_UART_AGM)
	uart_irq_config_func_t irq_config_func;
#endif
	/* Fixed-clock rate of this instance's clock domain (the node's
	 * `clocks` phandle). Per-instance on purpose: a board with a second
	 * UART gets its own rate here (see the AGM_UART_INIT() glue). */
	uint32_t clk_freq;
	bool fifo_disable;
};

struct agm_uart_data {
	DEVICE_MMIO_RAM;
	struct uart_config uart_cfg;
#if defined(CONFIG_UART_INTERRUPT_DRIVEN)
	volatile bool sw_call_txdrdy;
	uart_irq_callback_user_data_t irq_cb;
	struct k_spinlock irq_cb_lock;
	void *irq_cb_data;
#endif
};

static inline volatile struct agm_uart_regs *agm_uart_regs(const struct device *dev)
{
	return (volatile struct agm_uart_regs *)DEVICE_MMIO_GET(dev);
}

/* ---- low-level helpers --------------------------------------------- */

static void agm_uart_enable(const struct device *dev)
{
	volatile struct agm_uart_regs *uart = agm_uart_regs(dev);

	barrier_isync_fence_full();
	uart->cr |= AGM_CR_UARTEN;
	barrier_isync_fence_full();
}

static void agm_uart_disable(const struct device *dev)
{
	volatile struct agm_uart_regs *uart = agm_uart_regs(dev);

	barrier_isync_fence_full();
	uart->cr &= ~AGM_CR_UARTEN;
	barrier_isync_fence_full();
}

static void agm_uart_fifo_enable(const struct device *dev)
{
	volatile struct agm_uart_regs *uart = agm_uart_regs(dev);

	uart->lcr_h |= AGM_LCRH_FEN;
}

static void agm_uart_fifo_disable(const struct device *dev)
{
	volatile struct agm_uart_regs *uart = agm_uart_regs(dev);

	uart->lcr_h &= ~AGM_LCRH_FEN;
}

static void agm_uart_set_flow_control(const struct device *dev, bool rts, bool cts)
{
	volatile struct agm_uart_regs *uart = agm_uart_regs(dev);

	uart->cr &= ~(AGM_CR_RTSEn | AGM_CR_CTSEn | AGM_CR_RTS);
	if (rts) {
		uart->cr |= AGM_CR_RTSEn | AGM_CR_RTS;
	}
	if (cts) {
		uart->cr |= AGM_CR_CTSEn;
	}
}

/*
 * Baud-rate divisor math: the divisor is
 *
 *     bauddiv = uart_clk / (16 * baud_rate)
 *
 * shifted left by FBRD_WIDTH (6) so the lower 6 bits hold the fraction
 * without floating-point. The same shifted form goes into IBRD/FBRD.
 */
#define FBRD_WIDTH 6U

static int agm_uart_set_baudrate(const struct device *dev,
				 uint32_t clk, uint32_t baudrate)
{
	uint64_t bauddiv;
	volatile struct agm_uart_regs *uart = agm_uart_regs(dev);

	if (baudrate == 0U) {
		return -EINVAL;
	}

	bauddiv = ((uint64_t)clk << FBRD_WIDTH) / (baudrate * 16U);

	if ((bauddiv < (1U << FBRD_WIDTH)) ||
	    (bauddiv > (65535U << FBRD_WIDTH))) {
		return -EINVAL;
	}

	uart->ibrd = bauddiv >> FBRD_WIDTH;
	uart->fbrd = bauddiv & ((1U << FBRD_WIDTH) - 1U);

	barrier_dmem_fence_full();

	/* A write to LCR_H commits the new divisor. */
	uart->lcr_h = uart->lcr_h;

	return 0;
}

static bool agm_uart_is_readable(const struct device *dev)
{
	volatile struct agm_uart_regs *uart = agm_uart_regs(dev);

	if (!(uart->cr & AGM_CR_UARTEN) || !(uart->cr & AGM_CR_RXE)) {
		return false;
	}

	return (uart->fr & AGM_FR_RXFE) == 0U;
}

/* ---- public UART API: poll ------------------------------------------ */

static int agm_uart_poll_in(const struct device *dev, unsigned char *c)
{
	volatile struct agm_uart_regs *uart = agm_uart_regs(dev);

	if (!agm_uart_is_readable(dev)) {
		return -1;
	}

	*c = (unsigned char)uart->dr;
	return 0;
}

static void agm_uart_poll_out(const struct device *dev, unsigned char c)
{
	volatile struct agm_uart_regs *uart = agm_uart_regs(dev);

	while (uart->fr & AGM_FR_TXFF) {
		; /* spin until FIFO has room */
	}

	uart->dr = (uint32_t)c;
}

static int agm_uart_err_check(const struct device *dev)
{
	int errors = 0;
	volatile struct agm_uart_regs *uart = agm_uart_regs(dev);
	uint32_t rsr;

	/*
	 * RSR latches error flags from the last DR read; writing any value
	 * to ECR (same offset, write side) clears them. Read-then-clear so
	 * the driver leaves the line in a clean state for the next read.
	 */
	rsr = uart->rsr;
	uart->rsr = 0;

	if (rsr & AGM_RSR_OE) {
		errors |= UART_ERROR_OVERRUN;
	}
	if (rsr & AGM_RSR_BE) {
		errors |= UART_BREAK;
	}
	if (rsr & AGM_RSR_PE) {
		errors |= UART_ERROR_PARITY;
	}
	if (rsr & AGM_RSR_FE) {
		errors |= UART_ERROR_FRAMING;
	}

	return errors;
}

/* ---- public UART API: runtime configuration ------------------------- */

static int agm_uart_configure(const struct device *dev,
			      const struct uart_config *cfg, bool disable)
{
	const struct agm_uart_config *config = dev->config;
	struct agm_uart_data *data = dev->data;
	volatile struct agm_uart_regs *uart = agm_uart_regs(dev);
	uint32_t lcrh;
	int ret;

	if (disable) {
		agm_uart_disable(dev);
		agm_uart_fifo_disable(dev);
	}

	/* Preserve bits we never touch in this routine. */
	lcrh = uart->lcr_h & ~(AGM_LCRH_FORMAT_MASK | AGM_LCRH_STP2);

	switch (cfg->parity) {
	case UART_CFG_PARITY_NONE:
		lcrh &= ~AGM_LCRH_PEN;
		break;
	case UART_CFG_PARITY_ODD:
		lcrh &= ~AGM_LCRH_EPS;
		lcrh |= AGM_LCRH_PARITY_ODD;
		break;
	case UART_CFG_PARITY_EVEN:
		lcrh |= AGM_LCRH_PARITY_EVEN;
		break;
	default:
		ret = -ENOTSUP;
		goto enable;
	}

	switch (cfg->stop_bits) {
	case UART_CFG_STOP_BITS_1:
		lcrh &= ~AGM_LCRH_STP2;
		break;
	case UART_CFG_STOP_BITS_2:
		lcrh |= AGM_LCRH_STP2;
		break;
	default:
		ret = -ENOTSUP;
		goto enable;
	}

	switch (cfg->data_bits) {
	case UART_CFG_DATA_BITS_5:
		lcrh |= AGM_LCRH_WLEN_SIZE(5) << AGM_LCRH_WLEN_SHIFT;
		break;
	case UART_CFG_DATA_BITS_6:
		lcrh |= AGM_LCRH_WLEN_SIZE(6) << AGM_LCRH_WLEN_SHIFT;
		break;
	case UART_CFG_DATA_BITS_7:
		lcrh |= AGM_LCRH_WLEN_SIZE(7) << AGM_LCRH_WLEN_SHIFT;
		break;
	case UART_CFG_DATA_BITS_8:
		lcrh |= AGM_LCRH_WLEN_SIZE(8) << AGM_LCRH_WLEN_SHIFT;
		break;
	default:
		ret = -ENOTSUP;
		goto enable;
	}

	switch (cfg->flow_ctrl) {
	case UART_CFG_FLOW_CTRL_NONE:
		agm_uart_set_flow_control(dev, false, false);
		break;
	case UART_CFG_FLOW_CTRL_RTS_CTS:
		agm_uart_set_flow_control(dev, true, true);
		break;
	default:
		ret = -ENOTSUP;
		goto enable;
	}

	ret = agm_uart_set_baudrate(dev, config->clk_freq, cfg->baudrate);
	if (ret != 0) {
		goto enable;
	}

	uart->lcr_h = lcrh;
	memcpy(&data->uart_cfg, cfg, sizeof(data->uart_cfg));
	ret = 0;

enable:
	if (disable) {
		if (!config->fifo_disable) {
			agm_uart_fifo_enable(dev);
		}
		agm_uart_enable(dev);
	}
	return ret;
}

#ifdef CONFIG_UART_USE_RUNTIME_CONFIGURE
static int agm_uart_runtime_configure(const struct device *dev,
				     const struct uart_config *cfg)
{
	return agm_uart_configure(dev, cfg, true);
}

static int agm_uart_runtime_config_get(const struct device *dev,
				       struct uart_config *cfg)
{
	struct agm_uart_data *data = dev->data;

	*cfg = data->uart_cfg;
	return 0;
}
#endif /* CONFIG_UART_USE_RUNTIME_CONFIGURE */

/* ---- public UART API: FIFO fill / drain (interrupt-driven) ---------- */

#if defined(CONFIG_UART_INTERRUPT_DRIVEN)
static int agm_uart_fifo_fill(const struct device *dev,
			      const uint8_t *tx_data, int len)
{
	volatile struct agm_uart_regs *uart = agm_uart_regs(dev);
	int n = 0;

	while ((len-- > 0) && !(uart->fr & AGM_FR_TXFF)) {
		uart->dr = *tx_data++;
		n++;
	}

	return n;
}

static int agm_uart_fifo_read(const struct device *dev,
			     uint8_t *rx_data, const int len)
{
	volatile struct agm_uart_regs *uart = agm_uart_regs(dev);
	int n = 0;
	int remaining = len;

	while ((remaining-- > 0) && !(uart->fr & AGM_FR_RXFE)) {
		*rx_data++ = (uint8_t)uart->dr;
		n++;
	}

	return n;
}
#endif /* CONFIG_UART_INTERRUPT_DRIVEN */

#if defined(CONFIG_UART_INTERRUPT_DRIVEN)
static void agm_uart_irq_tx_enable(const struct device *dev)
{
	volatile struct agm_uart_regs *uart = agm_uart_regs(dev);

	uart->imsc |= AGM_INT_TXIM;
}

static void agm_uart_irq_tx_disable(const struct device *dev)
{
	volatile struct agm_uart_regs *uart = agm_uart_regs(dev);

	uart->imsc &= ~AGM_INT_TXIM;
}

static int agm_uart_irq_tx_complete(const struct device *dev)
{
	struct agm_uart_data *data = dev->data;
	volatile struct agm_uart_regs *uart = agm_uart_regs(dev);

	/* The PL011 TX interrupt fires when the FIFO drops below the
	 * threshold, so on the last byte it can fire before that byte has
	 * actually left the shift register -- sampling FR.BUSY is what tells
	 * the two apart (upstream pl011's predicate is the same).
	 *
	 * sw_call_txdrdy is armed by a TX IRQ (and at init) and consumed
	 * here: "a TX IRQ was seen and the line has now gone idle". Reading
	 * it must not *gate* the answer -- it used to, and because nothing
	 * ever cleared it the function returned -EAGAIN forever, even with
	 * an empty FIFO and BUSY low.
	 */
	if ((uart->fr & AGM_FR_BUSY) != 0U) {
		return -EAGAIN;
	}

	data->sw_call_txdrdy = false;

	return 0;
}

static int agm_uart_irq_tx_ready(const struct device *dev)
{
	volatile struct agm_uart_regs *uart = agm_uart_regs(dev);

	/* TX ready = the FIFO has room.  We don't gate on a threshold bit
	 * here because callers (console) re-fill as long as there's room.
	 */
	return (uart->fr & AGM_FR_TXFF) ? 0 : 1;
}

static void agm_uart_irq_rx_enable(const struct device *dev)
{
	volatile struct agm_uart_regs *uart = agm_uart_regs(dev);

	uart->imsc |= AGM_INT_RXIM | AGM_INT_RTIM;
}

static void agm_uart_irq_rx_disable(const struct device *dev)
{
	volatile struct agm_uart_regs *uart = agm_uart_regs(dev);

	uart->imsc &= ~(AGM_INT_RXIM | AGM_INT_RTIM);
}

static int agm_uart_irq_rx_ready(const struct device *dev)
{
	volatile struct agm_uart_regs *uart = agm_uart_regs(dev);

	return (uart->fr & AGM_FR_RXFE) ? 0 : 1;
}

static void agm_uart_irq_err_enable(const struct device *dev)
{
	volatile struct agm_uart_regs *uart = agm_uart_regs(dev);

	uart->imsc |= AGM_INT_ERROR_MASK;
}

static void agm_uart_irq_err_disable(const struct device *dev)
{
	volatile struct agm_uart_regs *uart = agm_uart_regs(dev);

	uart->imsc &= ~AGM_INT_ERROR_MASK;
}

static int agm_uart_irq_is_pending(const struct device *dev)
{
	volatile struct agm_uart_regs *uart = agm_uart_regs(dev);

	return (uart->mis & (AGM_INT_RXIM | AGM_INT_TXIM | AGM_INT_RTIM |
			     AGM_INT_ERROR_MASK)) ? 1 : 0;
}

static void agm_uart_irq_callback_set(const struct device *dev,
				      uart_irq_callback_user_data_t cb,
				      void *cb_data)
{
	struct agm_uart_data *data = dev->data;

	K_SPINLOCK(&data->irq_cb_lock) {
		data->irq_cb = cb;
		data->irq_cb_data = cb_data;
	}
}
#endif /* CONFIG_UART_INTERRUPT_DRIVEN */

/* ---- DEVICE_API ------------------------------------------------------ */

/*
 * Two tables: the full API when interrupts are enabled and the node has
 * an `interrupts` property, and a noirq table otherwise (just poll in/out
 * + err_check + optional runtime configuration).
 */
static DEVICE_API(uart, agm_uart_driver_api_noirq) = {
	.poll_in = agm_uart_poll_in,
	.poll_out = agm_uart_poll_out,
	.err_check = agm_uart_err_check,
#ifdef CONFIG_UART_USE_RUNTIME_CONFIGURE
	.configure = agm_uart_runtime_configure,
	.config_get = agm_uart_runtime_config_get,
#endif
};

static DEVICE_API(uart, agm_uart_driver_api) = {
	.poll_in = agm_uart_poll_in,
	.poll_out = agm_uart_poll_out,
	.err_check = agm_uart_err_check,
#if defined(CONFIG_UART_INTERRUPT_DRIVEN)
	.fifo_fill = agm_uart_fifo_fill,
	.fifo_read = agm_uart_fifo_read,
	.irq_tx_enable = agm_uart_irq_tx_enable,
	.irq_tx_disable = agm_uart_irq_tx_disable,
	.irq_tx_complete = agm_uart_irq_tx_complete,
	.irq_tx_ready = agm_uart_irq_tx_ready,
	.irq_rx_enable = agm_uart_irq_rx_enable,
	.irq_rx_disable = agm_uart_irq_rx_disable,
	.irq_rx_ready = agm_uart_irq_rx_ready,
	.irq_err_enable = agm_uart_irq_err_enable,
	.irq_err_disable = agm_uart_irq_err_disable,
	.irq_is_pending = agm_uart_irq_is_pending,
	.irq_update = NULL,
	.irq_callback_set = agm_uart_irq_callback_set,
#endif /* CONFIG_UART_INTERRUPT_DRIVEN */
#ifdef CONFIG_UART_USE_RUNTIME_CONFIGURE
	.configure = agm_uart_runtime_configure,
	.config_get = agm_uart_runtime_config_get,
#endif
};

/* ---- device init ---------------------------------------------------- */

static int agm_uart_init(const struct device *dev)
{
	const struct agm_uart_config *config = dev->config;
	struct agm_uart_data *data = dev->data;
	volatile struct agm_uart_regs *uart;
	int ret;

	DEVICE_MMIO_MAP(dev, K_MEM_CACHE_NONE);
	uart = agm_uart_regs(dev);

#if defined(CONFIG_PINCTRL)
	ret = pinctrl_apply_state(config->pincfg, PINCTRL_STATE_DEFAULT);
	if (ret) {
		return ret;
	}
#endif

	agm_uart_disable(dev);
	agm_uart_fifo_disable(dev);
	(void)agm_uart_configure(dev, &data->uart_cfg, false);

	uart->ifls = FIELD_PREP(AGM_IFLS_TXIFLSEL_M, AGM_IFLS_TX_1_8_FULL)
		   | FIELD_PREP(AGM_IFLS_RXIFLSEL_M, AGM_IFLS_RX_1_2_FULL);

	if (!config->fifo_disable) {
		agm_uart_fifo_enable(dev);
	}

	uart->imsc = 0U;
	uart->icr = AGM_INT_MASK_ALL;
	uart->dmacr = 0U;

	barrier_isync_fence_full();
	uart->cr &= ~AGM_CR_SIREN;
	uart->cr |= AGM_CR_RXE | AGM_CR_TXE;
	barrier_isync_fence_full();

#if defined(CONFIG_UART_INTERRUPT_DRIVEN)
	if (config->irq_config_func) {
		config->irq_config_func(dev);
		data->sw_call_txdrdy = true;
	}
#endif

	agm_uart_enable(dev);
	return 0;
}

#if defined(CONFIG_UART_INTERRUPT_DRIVEN)
/* Zephyr's ISR type is void (*)(const void *), and it has to be spelled that
 * way here: IRQ_CONNECT() builds the handler's table symbol by pasting
 * __isr_ ## <handler> (include/zephyr/sw_isr_table.h), so the argument has to
 * be a bare identifier -- a cast at the call site does not compile. Every
 * architecture's ARCH_IRQ_CONNECT() then casts the handler itself ... except
 * the native/POSIX one, whose boards/native/common/irq/board_irq.h passes it
 * through unchanged, which is why a `const struct device *` handler is
 * -Wincompatible-pointer-types there and keeps the driver out of the
 * native_sim test build (tests/drivers/serial/uart_agm). */
static void agm_uart_isr(const void *isr_arg)
{
	const struct device *dev = isr_arg;
	struct agm_uart_data *data = dev->data;
	volatile struct agm_uart_regs *uart = agm_uart_regs(dev);

	/* CTS modem edge: clear and re-mask, mirroring pl011's behaviour
	 * (the modem interrupt is level-triggered; without the re-mask
	 * the IRQ would re-fire every byte).
	 */
	if (uart->mis & AGM_INT_CTSMIM) {
		uart->icr = AGM_INT_CTSMIM;
		uart->imsc &= ~AGM_INT_CTSMIM;
	}

	/* Latch-clear the error flags so they don't keep the IRQ pinned;
	 * err_check() still reads RSR for the user-side report.
	 */
	if (uart->mis & AGM_INT_ERROR_MASK) {
		uart->icr = uart->mis & AGM_INT_ERROR_MASK;
	}

	/* TX IRQ observed -> tx_complete() will see BUSY=1 next time it
	 * is called and poll until BUSY clears. See irq_tx_complete.
	 */
	if (uart->mis & AGM_INT_TXIM) {
		data->sw_call_txdrdy = true;
	}

	if (data->irq_cb) {
		K_SPINLOCK(&data->irq_cb_lock) {
			data->irq_cb(dev, data->irq_cb_data);
		}
	}
}
#endif /* CONFIG_UART_INTERRUPT_DRIVEN */

/* ---- instantiation glue -------------------------------------------- */

#if defined(CONFIG_PINCTRL)
#define AGM_PINCTRL_DEFINE(n) PINCTRL_DT_INST_DEFINE(n);
#define AGM_PINCTRL_INIT(n)   .pincfg = PINCTRL_DT_INST_DEV_CONFIG_GET(n),
#else
#define AGM_PINCTRL_DEFINE(n)
#define AGM_PINCTRL_INIT(n)
#endif

/*
 * IRQ wiring for the user-group mcumgr/SMP path and the console-callback
 * path: one entry per `interrupt-names` cell.  AgRV2K's dtsi declares a
 * single "uart" name today (drivers/serial/uart_agm_smp.c / smp_cli.py
 * / smpmgr all use the un-named idx 0 path; naming is the escape hatch
 * if a future board adds a second IRQ).
 *
 * Use DT_FOREACH_PROP_ELEM on the node_id directly rather than
 * DT_INST_FOREACH_PROP_ELEM: the devicetree generator only emits
 * `<node_id>_IRQ_LEVEL` for the node_id form (not the DT_INST_<i>_<compat>
 * form), and DT_IRQN_BY_IDX() in the MULTI_LEVEL_INTERRUPTS=y branch
 * reads that macro. Using the INST form would silently drop down to
 * the bare cell path, which is exactly the upstream pl011 bug.
 *
 * DT_IRQN_BY_IDX(node_id, i) returns the multi-level encoded IRQ number
 * on CONFIG_MULTI_LEVEL_INTERRUPTS=y platforms (RISC-V + PLIC); the raw
 * cell otherwise. Using the bare cell here was the bug in upstream
 * pl011's PL011_IRQ_CONFIG_FUNC_BODY.
 */
#if defined(CONFIG_UART_INTERRUPT_DRIVEN)
#define AGM_IRQ_CONFIG_FUNC(n)                                                                  \
	static void agm_uart_irq_config_func_##n(const struct device *dev)                            \
	{                                                                                          \
		DT_FOREACH_PROP_ELEM(DT_DRV_INST(n), interrupt_names, AGM_IRQ_CONNECT_BODY)         \
	}

#define AGM_IRQ_CONNECT_BODY(node_id, prop, i)                                                   \
	{                                                                                          \
		IRQ_CONNECT(DT_IRQN_BY_IDX(node_id, i),                                            \
			   DT_IRQ_BY_IDX(node_id, i, priority),                                     \
			   agm_uart_isr, DEVICE_DT_GET(node_id), 0);                                \
		irq_enable(DT_IRQN_BY_IDX(node_id, i));                                             \
	}

#define AGM_IRQ_CONFIG_FUNC_INIT(n)                                                    \
	IF_ENABLED(DT_INST_NODE_HAS_PROP(n, interrupts),                                \
		   (.irq_config_func = agm_uart_irq_config_func_##n,))
#else
#define AGM_IRQ_CONFIG_FUNC(n)
#define AGM_IRQ_CONNECT_BODY(n, prop, i)
#define AGM_IRQ_CONFIG_FUNC_INIT(n)
#endif

#define AGM_DEVICE_API(n)                                                              \
	COND_CODE_1(CONFIG_UART_INTERRUPT_DRIVEN,                                       \
		    (COND_CODE_1(DT_INST_NODE_HAS_PROP(n, interrupts),                  \
				 (&agm_uart_driver_api),                                \
				 (&agm_uart_driver_api_noirq))),                        \
		    (&agm_uart_driver_api_noirq))

#define AGM_UART_INIT(n)                                                                \
	AGM_PINCTRL_DEFINE(n)                                                           \
	AGM_IRQ_CONFIG_FUNC(n)                                                          \
	static const struct agm_uart_config agm_uart_cfg_##n = {                       \
		DEVICE_MMIO_ROM_INIT(DT_DRV_INST(n)),                                  \
		AGM_PINCTRL_INIT(n)                                                     \
		AGM_IRQ_CONFIG_FUNC_INIT(n)                                              \
		/* Per-instance: gated by soc.c from agm,apb-clkenable-bit before     \
		 * init runs (PRE_KERNEL_1); the bitstream owns the clock domain, so   \
		 * the rate comes from this node's own fixed-clock phandle. */         \
		.clk_freq = DT_INST_PROP_BY_PHANDLE(n, clocks, clock_frequency),        \
		.fifo_disable = DT_INST_PROP(n, fifo_disable),                          \
	};                                                                             \
	static struct agm_uart_data agm_uart_data_##n = {                               \
		.uart_cfg = {                                                           \
			.baudrate = DT_INST_PROP(n, current_speed),                    \
			.parity = UART_CFG_PARITY_NONE,                                \
			.stop_bits = UART_CFG_STOP_BITS_1,                             \
			.data_bits = UART_CFG_DATA_BITS_8,                             \
			.flow_ctrl = DT_INST_PROP(n, hw_flow_control)                  \
					     ? UART_CFG_FLOW_CTRL_RTS_CTS              \
					     : UART_CFG_FLOW_CTRL_NONE,                \
		},                                                                       \
	};                                                                             \
	DEVICE_DT_INST_DEFINE(n, agm_uart_init, NULL, &agm_uart_data_##n,               \
			      &agm_uart_cfg_##n, PRE_KERNEL_1,                         \
			      CONFIG_SERIAL_INIT_PRIORITY, AGM_DEVICE_API(n));

DT_INST_FOREACH_STATUS_OKAY(AGM_UART_INIT)
