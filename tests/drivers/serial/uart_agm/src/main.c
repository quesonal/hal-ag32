/*
 * Copyright (c) 2026 AgRV Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief native_sim suite for drivers/serial/uart_agm.c.
 *
 * Two things this suite has to provide, neither of which exists on native_sim:
 *
 *  1. **The register window**, which tests/drivers/common/agm_native/
 *     agm_fake_mmio.c reserves with mmap(MAP_FIXED) before any driver init.
 *     This suite only has to link it and keep
 *     the *real* UART address (0x40025000) in its overlay.
 *
 *  2. **Something for the status bits to mean.** RAM answers reads with
 *     whatever was last written, so the test plays the hardware for the bits
 *     the driver *polls* (FR.TXFF/RXFE/BUSY, SR.TIP-style flags, RSR error
 *     bits). That is the other approach in play: a small register
 *     model, not a behaviour model of the IP.
 *
 * What that buys, concretely, is coverage of the paths the board cannot
 * reach: irq_tx_complete() (the status flag that never cleared was
 * invisible to a 62-config build gate *and* to the on-board samples, because
 * nothing there calls it), the baud-divisor arithmetic, the FIFO gate checks,
 * and the AFSEL/DIR writes the driver's pinctrl state decodes to.
 */

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>
#include <zephyr/ztest.h>

#define UART_DEV  DEVICE_DT_GET(DT_NODELABEL(uart_agm0))

/* ---- the register model ---------------------------------------------- */

/* Offsets from drivers/serial/uart_agm.c's struct agm_uart_regs. Duplicated on
 * purpose: if the driver's map moves, this suite should fail loudly. */
#define R_DR     0x00U
#define R_RSR    0x04U
#define R_FR     0x18U
#define R_IBRD   0x24U
#define R_FBRD   0x28U
#define R_LCR_H  0x2cU
#define R_CR     0x30U
#define R_IMSC   0x38U
#define R_MIS    0x40U

#define UART_BASE 0x40025000UL

static uint32_t reg_rd(uint32_t off)
{
	return *(volatile uint32_t *)(UART_BASE + off);
}

static void reg_wr(uint32_t off, uint32_t val)
{
	*(volatile uint32_t *)(UART_BASE + off) = val;
}

/* FR bits the driver polls. */
#define FR_BUSY   BIT(3)
#define FR_RXFE   BIT(4)
#define FR_TXFF   BIT(5)

/* RSR/ECR bits. */
#define RSR_FE    BIT(0)
#define RSR_PE    BIT(1)
#define RSR_BE    BIT(2)
#define RSR_OE    BIT(3)

/* IMSC/MIS bits. */
#define IMSC_RXIM BIT(4)
#define IMSC_TXIM BIT(5)

/* CR bits (what init leaves behind). */
#define CR_UARTEN BIT(0)
#define CR_TXE    BIT(8)
#define CR_RXE    BIT(9)

/* GPIO bank registers the SoC pinctrl writes (soc/agm/agrv2k/agm_sys.h). */
#define GPIO0_BASE 0x40014000UL
#define GPIO_STRIDE 0x1000UL
#define GPIO_DIR_OFF 0x400UL
#define GPIO_AFSEL_OFF 0x420UL

static uint32_t gpio_rd(uint32_t bank, uint32_t off)
{
	return *(volatile uint32_t *)(GPIO0_BASE + (bank * GPIO_STRIDE) + off);
}

static void suite_reset_fake_regs(void *fixture)
{
	ARG_UNUSED(fixture);

	/* A clean device: FIFOs empty, line idle, no stale flags. The driver's
	 * own init already ran (device init happens before the suite), so CR /
	 * LCR_H / IBRD / FBRD keep whatever it programmed -- only the bits the
	 * test plays with are reset here. */
	reg_wr(R_FR, FR_RXFE);
	reg_wr(R_RSR, 0U);
	reg_wr(R_MIS, 0U);
	reg_wr(R_IMSC, 0U);
}

ZTEST_SUITE(uart_agm, NULL, NULL, suite_reset_fake_regs, NULL, NULL);

/* ---- init: the window is mapped, the state was applied ---------------- */

ZTEST(uart_agm, test_01_init_maps_registers_and_applies_pinctrl)
{
	zassert_true(device_is_ready(UART_DEV), "the UART device came up against the fake window");

	/* agm_uart_init() leaves UARTEN|TXE|RXE, enables the FIFOs and programs
	 * 115200 off the 200 MHz fixed clock. Reading it back through the fake
	 * window is what proves the driver's MMIO reached a backing store. */
	zassert_equal(reg_rd(R_CR) & (CR_UARTEN | CR_TXE | CR_RXE),
		      CR_UARTEN | CR_TXE | CR_RXE, "the UART is enabled with both directions");

	/* 200 MHz / (16 * 115200) = 108.5069 -> IBRD 108, FBRD 32 (truncated). */
	zassert_equal(reg_rd(R_IBRD), 108U, "IBRD for 115200 Bd at 200 MHz");
	zassert_equal(reg_rd(R_FBRD), 32U, "FBRD for 115200 Bd at 200 MHz");

	/* 8 data bits, no parity, one stop: WLEN = 3 << 5, in the format field
	 * (PEN|EPS|SPS|WLEN). The FIFO enable is a separate bit and the driver
	 * turns it on by default (CONFIG_UART_AGM, fifo_disable = 0). */
	zassert_equal(reg_rd(R_LCR_H) & 0xe6U, 0x60U, "8N1 line control");
	zassert_equal(reg_rd(R_LCR_H) & BIT(4), BIT(4), "the FIFOs are enabled");

	/* The consumer's pinctrl state is the real AGM encoding, decoded by the
	 * real pinctrl_configure_pins(): TX0 = GPIO7 bit 6 as OUTPUT, RX0 =
	 * GPIO6 bit 1 as INPUT. */
	zassert_equal(gpio_rd(7U, GPIO_AFSEL_OFF) & BIT(6), BIT(6),
		      "TX pin handed to the peripheral (AFSEL)");
	zassert_equal(gpio_rd(7U, GPIO_DIR_OFF) & BIT(6), BIT(6),
		      "TX pin set to drive (DIR), or the line stays silent");
	zassert_equal(gpio_rd(6U, GPIO_AFSEL_OFF) & BIT(1), BIT(1),
		      "RX pin handed to the peripheral (AFSEL)");
	zassert_equal(gpio_rd(6U, GPIO_DIR_OFF) & BIT(1), 0U,
		      "RX pin left as an input");
}

/* ---- irq_tx_complete() ----------------------------------------------- */

/* The regression this pins: irq_tx_complete() used to AND the hardware's
 * FR.BUSY with a flag that was only ever set, so it answered -EAGAIN forever
 * -- on an idle line, with nothing queued. Neither the build gate nor any
 * sample called it, so nothing noticed. */
ZTEST(uart_agm, test_10_irq_tx_complete_tracks_the_shifter)
{
	/* Still shifting: not complete yet. */
	reg_wr(R_FR, FR_RXFE | FR_BUSY);
	zassert_equal(uart_irq_tx_complete(UART_DEV), -EAGAIN,
		      "a busy shifter is not 'complete'");

	/* Shift register drained: complete, *without* needing a TX interrupt
	 * to have been observed first. */
	reg_wr(R_FR, FR_RXFE);
	zassert_equal(uart_irq_tx_complete(UART_DEV), 0,
		      "an idle line is complete");

	/* ... and it stays a true answer: the old code consumed a latch here and
	 * would flip back to -EAGAIN on the next call. */
	zassert_equal(uart_irq_tx_complete(UART_DEV), 0,
		      "asking twice does not change the answer");

	/* Busy again after a new byte: -EAGAIN again. */
	reg_wr(R_FR, FR_RXFE | FR_BUSY);
	zassert_equal(uart_irq_tx_complete(UART_DEV), -EAGAIN,
		      "a new transfer is not complete either");
}

/* ---- poll_in / poll_out and the FIFO gates --------------------------- */

ZTEST(uart_agm, test_20_poll_out_writes_the_data_register)
{
	reg_wr(R_FR, FR_RXFE);
	reg_wr(R_DR, 0U);

	uart_poll_out(UART_DEV, 0x5aU);
	zassert_equal(reg_rd(R_DR), 0x5aU, "poll_out writes the byte to DR");
}

ZTEST(uart_agm, test_21_poll_in_reads_only_when_the_fifo_has_data)
{
	unsigned char c = 0U;

	/* RXFE set: nothing there. */
	reg_wr(R_FR, FR_RXFE);
	reg_wr(R_DR, 0xa5U);
	zassert_equal(uart_poll_in(UART_DEV, &c), -1, "an empty FIFO is not readable");

	/* RXFE clear: the byte is there. */
	reg_wr(R_FR, 0U);
	zassert_equal(uart_poll_in(UART_DEV, &c), 0, "a full FIFO is readable");
	zassert_equal(c, 0xa5U, "the byte comes from DR");

	/* The receiver must also be enabled: a UART with RXE cleared reads
	 * nothing even when the FIFO flag says otherwise. */
	reg_wr(R_CR, CR_UARTEN | CR_TXE);
	zassert_equal(uart_poll_in(UART_DEV, &c), -1, "RXE=0 means no reception");
	reg_wr(R_CR, CR_UARTEN | CR_TXE | CR_RXE);
}

ZTEST(uart_agm, test_22_irq_tx_ready_and_rx_ready_follow_the_fifo_flags)
{
	reg_wr(R_FR, FR_RXFE | FR_TXFF);
	zassert_equal(uart_irq_tx_ready(UART_DEV), 0, "TXFF set: the FIFO has no room");
	zassert_equal(uart_irq_rx_ready(UART_DEV), 0, "RXFE set: nothing to read");

	reg_wr(R_FR, 0U);
	zassert_equal(uart_irq_tx_ready(UART_DEV), 1, "TXFF clear: room");
	zassert_equal(uart_irq_rx_ready(UART_DEV), 1, "RXFE clear: data waiting");
}

ZTEST(uart_agm, test_23_fifo_fill_stops_at_the_full_flag_and_fifo_read_at_empty)
{
	static const uint8_t tx[4] = { 1U, 2U, 3U, 4U };
	uint8_t rx[4] = { 0U };

	/* Room for everything. */
	reg_wr(R_FR, FR_RXFE);
	reg_wr(R_DR, 0U);
	zassert_equal(uart_fifo_fill(UART_DEV, tx, sizeof(tx)), sizeof(tx),
		      "an empty FIFO takes the whole buffer");
	zassert_equal(reg_rd(R_DR), 4U, "the last byte written is in DR");

	/* Full: nothing is taken. */
	reg_wr(R_FR, FR_RXFE | FR_TXFF);
	zassert_equal(uart_fifo_fill(UART_DEV, tx, sizeof(tx)), 0,
		      "a full FIFO takes nothing");

	/* Empty: nothing is read. */
	reg_wr(R_FR, FR_RXFE);
	zassert_equal(uart_fifo_read(UART_DEV, rx, sizeof(rx)), 0, "empty FIFO yields nothing");

	/* One byte waiting (RXFE clear): one byte comes back when one is asked
	 * for. The model cannot shrink a FIFO -- FR is driven by the test -- so
	 * the count assertion is sized to the request, and the "stop on empty"
	 * half is the case above. */
	reg_wr(R_DR, 0x42U);
	reg_wr(R_FR, 0U);
	zassert_equal(uart_fifo_read(UART_DEV, rx, 1U), 1, "one byte available");
	zassert_equal(rx[0], 0x42U, "the byte comes from DR");
}

/* ---- err_check ------------------------------------------------------- */

ZTEST(uart_agm, test_30_err_check_decodes_and_clears_rsr)
{
	reg_wr(R_RSR, RSR_OE | RSR_FE);
	zassert_equal(uart_err_check(UART_DEV),
		      UART_ERROR_OVERRUN | UART_ERROR_FRAMING,
		      "overrun + framing are reported");
	zassert_equal(reg_rd(R_RSR), 0U, "reading the errors clears them (ECR)");
	zassert_equal(uart_err_check(UART_DEV), 0, "and they do not come back");

	reg_wr(R_RSR, RSR_PE | RSR_BE);
	zassert_equal(uart_err_check(UART_DEV), UART_ERROR_PARITY | UART_BREAK,
		      "parity + break are reported");
	zassert_equal(uart_err_check(UART_DEV), 0, "clean line again");
}

/* ---- runtime configuration ------------------------------------------- */

#ifdef CONFIG_UART_USE_RUNTIME_CONFIGURE
ZTEST(uart_agm, test_40_configure_programs_the_divisors_and_reports_them)
{
	struct uart_config cfg = {
		.baudrate = 9600U,
		.parity = UART_CFG_PARITY_NONE,
		.stop_bits = UART_CFG_STOP_BITS_1,
		.data_bits = UART_CFG_DATA_BITS_8,
		.flow_ctrl = UART_CFG_FLOW_CTRL_NONE,
	};
	struct uart_config back = { 0 };

	zassert_ok(uart_configure(UART_DEV, &cfg), "9600 Bd off 200 MHz is in range");

	/* (200 MHz / (16 * 9600)) = 1302.08 -> IBRD 1302, FBRD 5. */
	zassert_equal(reg_rd(R_IBRD), 1302U, "IBRD for 9600 Bd");
	zassert_equal(reg_rd(R_FBRD), 5U, "FBRD for 9600 Bd");

	zassert_ok(uart_config_get(UART_DEV, &back));
	zassert_equal(back.baudrate, cfg.baudrate, "the driver remembers the rate");
	zassert_equal(back.data_bits, UART_CFG_DATA_BITS_8, "and the frame");

	/* A rate the divisors cannot express is refused, not silently clamped. */
	cfg.baudrate = 100000000U;
	zassert_equal(uart_configure(UART_DEV, &cfg), -EINVAL,
		      "a divisor below 1 is out of range");

	/* Unsupported frame shapes are refused too. */
	cfg.baudrate = 115200U;
	cfg.parity = UART_CFG_PARITY_MARK;
	zassert_equal(uart_configure(UART_DEV, &cfg), -ENOTSUP, "mark parity is not supported");
	cfg.parity = UART_CFG_PARITY_NONE;
	cfg.stop_bits = UART_CFG_STOP_BITS_1_5;
	zassert_equal(uart_configure(UART_DEV, &cfg), -ENOTSUP, "1.5 stop bits are not supported");

	/* Back to the board default for the other cases. */
	cfg.stop_bits = UART_CFG_STOP_BITS_1;
	zassert_ok(uart_configure(UART_DEV, &cfg));
}
#endif /* CONFIG_UART_USE_RUNTIME_CONFIGURE */

/* ---- interrupt-enable plumbing --------------------------------------- */

#if defined(CONFIG_UART_INTERRUPT_DRIVEN)
ZTEST(uart_agm, test_50_irq_enables_touch_the_mask_and_pending_reads_mis)
{
	uart_irq_tx_enable(UART_DEV);
	zassert_equal(reg_rd(R_IMSC) & IMSC_TXIM, IMSC_TXIM, "TX interrupt enabled");
	uart_irq_tx_disable(UART_DEV);
	zassert_equal(reg_rd(R_IMSC) & IMSC_TXIM, 0U, "TX interrupt disabled");

	uart_irq_rx_enable(UART_DEV);
	zassert_equal(reg_rd(R_IMSC) & IMSC_RXIM, IMSC_RXIM, "RX interrupt enabled");
	uart_irq_rx_disable(UART_DEV);
	zassert_equal(reg_rd(R_IMSC) & IMSC_RXIM, 0U, "RX interrupt disabled");

	reg_wr(R_MIS, 0U);
	zassert_equal(uart_irq_is_pending(UART_DEV), 0, "nothing pending");
	reg_wr(R_MIS, IMSC_RXIM);
	zassert_equal(uart_irq_is_pending(UART_DEV), 1, "a masked-in RX status is pending");
	reg_wr(R_MIS, 0U);
}
#endif /* CONFIG_UART_INTERRUPT_DRIVEN */
