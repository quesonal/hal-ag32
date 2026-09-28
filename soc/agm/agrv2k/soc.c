/*
 * Copyright (c) 2026 AgRV Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * AgRV2K SoC-level orchestration.
 *
 * This file is the **top-level** SoC init. The actual work lives in:
 *   - fcb.c     : FCB bitstream injection (FCB_AutoConfig + Activate)
 *   - clk.c     : SYS controller clock tree (HSI → PLL switch)
 *   - pinctrl.c : GPIO AFSEL routing for UART0 TX/RX
 *
 * The split mirrors AgRV SDK's framework-agrv_sdk/src layout
 * (fcb.c / system.c / board.c). Why split: every chunk is a distinct
 * concern (FCB ↔ clock ↔ pinctrl), the SDK keeps them apart, and
 * keeping soc.c as a thin orchestrator makes the boot sequence
 * readable in one screen.
 *
 * Boot flow (mirrors AgRV SDK framework-agrv_sdk/src/board.c::board_init):
 *   1. agrv2k_fcb_program()       — stream bitstream → FCB → activate
 *   2. agrv2k_clk_switch_hsi()    — force HSI source, drop HSE/PLL
 *   3. agrv2k_clk_switch_pll()    — HSE → PLL switch + SCLK divider
 *   4. APB clocks, derived from devicetree (agrv2k_apb_gates()): every
 *      enabled GPIO bank, plus each enabled UART's own gate and the gates
 *      of the banks its pinctrl state routes through
 *
 * The pin route itself (AFSEL/DIR for PIN_68/69) is NOT done here: it is
 * the pinctrl driver's job, driven by uart0's pinctrl state, and happens
 * from uart_agm_init() at PRE_KERNEL_1 like any other consumer's pinctrl.
 *
 * This init deliberately does NOT touch the mie CSR. The boot-time
 * suppression (csrc mie.MTIE + mtimecmp saturation) happens once in
 * soc/agm/agrv2k/reset.S; the MTIP-driven system timer
 * (riscv_machine_timer, enabled via the timer0 DT node)
 * arms a finite mtimecmp and sets mie.MTIE itself at PRE_KERNEL_2.
 * Wiping mie in a SYS_INIT would also clear the MEIP bit that the
 * PLIC driver enables during its own PRE_KERNEL_1 init (irq_enable()
 * of CPU-local irq 11), silently killing every external interrupt on
 * the board. Leave mie alone.
 *
 * MMIO access: raw volatile loads/stores — see "MMIO access" note in
 * the original soc.c. AgRV2K does not provide z_soc_sys_read32/
 * z_soc_sys_write32, so the upstream helpers trap illegal-instruction.
 */

#include <errno.h>
#include <zephyr/init.h>
#include <zephyr/devicetree.h>
#include <zephyr/irq.h>
#include <zephyr/sys/util.h>

#include "agm_sys.h"

/* Public entry points implemented in the split files. */
extern int agrv2k_fcb_program(void);
extern void agrv2k_fcb_log_status(const char *tag);
extern void agrv2k_clk_switch_hsi(void);
extern void agrv2k_clk_switch_pll(uint32_t pll_hz, uint32_t flash_max_hz);

/* SYS_BASE / register offsets / APB bits live in agm_sys.h (shared with
 * the peripheral drivers). Aliases below keep the boot
 * sequence readable. */
#define SYS_BASE              AGM_SYS_BASE
#define SYS_APB_CLKENABLE     AGM_SYS_APB_CLKENABLE
#define SYS_AHB_CLKENABLE     AGM_SYS_AHB_CLKENABLE
#define SYS_AHB_RESET         AGM_SYS_AHB_RESET
#define SYS_RST_CNTL          AGM_SYS_RST_CNTL
#define SYS_RST_SFT           AGM_SYS_RST_SFT
#define APB_CLKENABLE_FCB0    AGM_APB_CLK_FCB0     /* fcb.c streams bitstream via this */

/* Clock configuration is taken from DTS (per-board .dts → dtsi default).
 * The two properties live on sys-controller@3000000 (compatible
 * "agm,agrv2k-sys"); see dts/bindings/clock/agm,agrv2k-sys.yaml.
 *
 *   clock-frequency       = SYSCLK after FCB PLL switch (Hz).
 *   flash-max-frequency   = Maximum FLASH controller clock (Hz).
 */
static const uint32_t board_pll_frequency =
	DT_PROP_BY_PHANDLE(DT_NODELABEL(sys), clocks, clock_frequency);
static const uint32_t flash_max_freq =
	DT_PROP(DT_NODELABEL(sys), flash_max_frequency);

static inline uint32_t sys_read(uint32_t off)
{
	return *((volatile uint32_t *)(SYS_BASE + off));
}

static inline void sys_write(uint32_t off, uint32_t val)
{
	*((volatile uint32_t *)(SYS_BASE + off)) = val;
}

/*
 * APB clock gates, derived from devicetree instead of a hand-written list.
 *
 * This used to be GPIO4|GPIO6|GPIO7|UART0, copied from the SDK's
 * board_init., SYS.APB_CLKENABLE reads back as
 * 0x00200d01 at runtime -- exactly those five bits -- so every *other*
 * enabled peripheral sat behind a closed gate and its registers accepted
 * writes that silently vanished (GPIO5, WDOG0, I2C0/1, CAN0, GPTIMER0-4
 * all read 0 until their driver opened them).
 *
 * The set is now derived:
 *   - every enabled (status="okay") GPIO bank. The in-tree stellaris driver
 *     owns the bank registers but knows nothing about the AgRV APB gate, so
 *     the SoC is the only place that can open it;
 *   - every enabled peripheral that declares a pinctrl state (agm,agrv2k-uart,
 *     agm,agrv2k-can, agm,agrv2k-i2c): its own gate bit, plus the gates of the
 *     banks its pinctrl state routes through. Those routes are AGM_PINCTRL()
 *     cells in the state (see the pinctrl binding), and a bank named there may
 *     deliberately not be a GPIO device in DT (uart0's TX bank GPIO7 and
 *     can0's pins in GPIO7/GPIO8 are exactly that), so devicetree is the only
 *     correct source for it.
 *   - every enabled peripheral that only needs its own gate bit
 *     (counter_agm_timer, counter_agm_gptimer, pwm_agm_gptimer, wdt_agm):
 *     read agm,apb-clkenable-bit from the node. None of them derives the bit
 *     from its register address any more;
 *   - AHB-domain peripherals (agm,agrv2k-usb0, agm,agrv2k-crc, agm,agrv2k-dma): their
 *     agm,ahb-clkenable-bit is set in SYS.AHB_CLKENABLE and the matching
 *     agm,ahb-reset-bit is cleared in SYS.AHB_RESET (write 1 = assert).
 *
 * FCB0 is not a devicetree node and stays explicit. Drivers do not open their
 * own gates during bring-up -- everything a device needs is on by the time it
 * initialises, so "which clocks are on at boot" can be read off the
 * devicetree. One deliberate exception: a driver's runtime power management
 * action may gate *its own* bit later (spi_agm does exactly that, with the bit
 * taken from DT).
 */
#define AGM_GATE_GPIO_BANK(node)                                                            \
	bits |= AGM_APB_CLK_GPIO((DT_REG_ADDR(node) - AGM_GPIO0_BASE) / AGM_APB_STRIDE);

/* Guard the bank range: a cell naming a non-existent bank would otherwise
 * enable an unrelated peripheral's clock (bank 15 maps to APB bit 19). The
 * pin controller rejects such a cell with -EINVAL at configure time; here it
 * simply contributes nothing. */
#define AGM_GATE_PIN(node, prop, idx)                                                       \
	bits |= (((DT_PROP_BY_IDX(node, prop, idx) & 0xfU) < AGM_GPIO_BANK_COUNT)           \
		 ? AGM_APB_CLK_GPIO(DT_PROP_BY_IDX(node, prop, idx) & 0xfU)                 \
		 : 0U);

/* A state whose agm,pins is empty is a build-time bug, not a no-op.
 *
 * The generated <agm/pinctrl-*.dtsi> fragments are what fill these states
 * in; dts/riscv/agm/agrv2k.dtsi declares them with an `agm,pins = <>;`
 * placeholder and dts/riscv/agm/agrv2k-board.dtsi includes the board
 * default one (a sample overlay adds its own, e.g.
 * <agm/pinctrl-devmac.dtsi>). If that include is ever dropped, the
 * placeholder stays behind -- a *valid* empty array, so nothing else
 * complains: this macro gates no bank, pinctrl_configure_pins() sees
 * pin_cnt == 0 and returns 0, the peripheral's init succeeds, and the pins
 * are simply never AFSEL'd. The symptom is the UART0 console going silent
 * with no build error and no runtime error. Fail the build instead.
 */
#define AGM_GATE_PINCTRL(group)                                                             \
	BUILD_ASSERT(DT_PROP_LEN(group, agm_pins) > 0,                                      \
		     "pinctrl state has no agm,pins cells -- the generated "                \
		     "<agm/pinctrl-*.dtsi> fragment is not included");                      \
	DT_FOREACH_PROP_ELEM(group, agm_pins, AGM_GATE_PIN)

/* Peripheral whose only requirement is its own APB gate. */
#define AGM_GATE_APB(node) bits |= BIT(DT_PROP(node, agm_apb_clkenable_bit));

/* Peripheral with pins: own APB gate + the banks its pinctrl state uses. */
#define AGM_GATE_PERIPHERAL(node)                                                           \
	AGM_GATE_APB(node)                                                                  \
	AGM_GATE_PINCTRL(DT_PHANDLE(node, pinctrl_0))

/* Peripheral whose pins are AFSEL'd but which has no own
 * apb-clkenable-bit (eth0 sits on AHB; its pins live on
 * GPIO banks gated separately). */
#define AGM_GATE_PADS_ONLY(node)                                                           \
	AGM_GATE_PINCTRL(DT_PHANDLE(node, pinctrl_0))

static uint32_t agrv2k_apb_gates(void)
{
	uint32_t bits = APB_CLKENABLE_FCB0;

	DT_FOREACH_STATUS_OKAY(ti_stellaris_gpio, AGM_GATE_GPIO_BANK)
	DT_FOREACH_STATUS_OKAY(agm_agrv2k_uart, AGM_GATE_PERIPHERAL)
	DT_FOREACH_STATUS_OKAY(agm_agrv2k_can, AGM_GATE_PERIPHERAL)
	DT_FOREACH_STATUS_OKAY(agm_agrv2k_i2c, AGM_GATE_PERIPHERAL)
	DT_FOREACH_STATUS_OKAY(agm_agrv2k_spi, AGM_GATE_PERIPHERAL)
	DT_FOREACH_STATUS_OKAY(agm_agrv2k_timer, AGM_GATE_APB)
	DT_FOREACH_STATUS_OKAY(agm_agrv2k_gptimer, AGM_GATE_APB)
	DT_FOREACH_STATUS_OKAY(agm_agrv2k_gptimer_pwm, AGM_GATE_PERIPHERAL)
	DT_FOREACH_STATUS_OKAY(agm_agrv2k_wdt, AGM_GATE_APB)
	/* eth0 sits on AHB (handled in agrv2k_ahb_clocks() below) but
	 * the pins it routes through AFSEL live on GPIO banks 4 / 7 /
	 * 8 / 9, which are gated by APB_CLKENABLE like any other
	 * peripheral. Emac has no apb-clkenable-bit of its own -- the
	 * GPIO bank gates are what we need; without them AFSEL writes
	 * silently no-op and the MAC pins stay in GPIO mode. */
	DT_FOREACH_STATUS_OKAY(agm_agrv2k_emac, AGM_GATE_PADS_ONLY)

	return bits;
}

/*
 * AHB-domain peripherals: clock gate + reset release, both from devicetree.
 *
 * Called from *both* bring-up paths: the CRC_AGM build opens the gates before
 * step 1 (its fabric verifier runs inside agrv2k_fcb_program()), every other
 * build opens them at step 4b. Leaving the call out of one of them is not a
 * cosmetic difference -- DMAC0, USB0 and EMAC are AHB devices, so a gated
 * DMAC0 makes every DMA-backed transfer fail (:
 * samples/dma_memcpy FAIL and the SPI driver's long RX -ETIMEDOUT, while
 * register-only paths kept working).
 */
#define AGM_GATE_AHB(node)                                                                  \
	BUILD_ASSERT(DT_PROP(node, agm_ahb_clkenable_bit) ==                                \
			     DT_PROP(node, agm_ahb_reset_bit),                              \
		     "AHB clock-enable and reset bit must match");                          \
	ahb_add |= BIT(DT_PROP(node, agm_ahb_clkenable_bit));                               \
	ahb_clear |= BIT(DT_PROP(node, agm_ahb_reset_bit));

/*
 * USB0 is one register block with two devicetree nodes: &usb0 for the device
 * role (udc_agm) and &uhc0 for the host role (uhc_agm). Both write the same
 * registers and both claim the same PLIC line (USB0_IRQn = 35), so an overlay
 * that enables both builds two drivers that fight over one controller and
 * fails in a way that looks like a hardware fault. The role is chosen by
 * which node is "okay"; make the mistake impossible to build.
 */
BUILD_ASSERT(!(DT_NODE_HAS_STATUS(DT_NODELABEL(usb0), okay) &&
	       DT_NODE_HAS_STATUS(DT_NODELABEL(uhc0), okay)),
	     "enable either &usb0 (device) or &uhc0 (host), not both: one "
	     "register block, one IRQ");

static void agrv2k_ahb_clocks(void)
{
	uint32_t ahb_add = 0U;
	uint32_t ahb_clear = 0U;

	DT_FOREACH_STATUS_OKAY(agm_agrv2k_usb0, AGM_GATE_AHB)
	/* Same AHB block and the same gate/reset bit as agm,agrv2k-usb0, but
	 * a separate node: the host role (drivers/usb/uhc/uhc_agm.c) is
	 * chosen by enabling &uhc0 instead of &usb0, and whichever node is
	 * enabled has to open the gate here. */
	DT_FOREACH_STATUS_OKAY(agm_agrv2k_uhc, AGM_GATE_AHB)
	DT_FOREACH_STATUS_OKAY(agm_agrv2k_crc, AGM_GATE_AHB)
	DT_FOREACH_STATUS_OKAY(agm_agrv2k_dma, AGM_GATE_AHB)
	DT_FOREACH_STATUS_OKAY(agm_agrv2k_emac, AGM_GATE_AHB)

	if (ahb_add != 0U) {
		sys_write(SYS_AHB_CLKENABLE, sys_read(SYS_AHB_CLKENABLE) | ahb_add);
	}
	if (ahb_clear != 0U) {
		/* Write 1 = assert, so clear the bits to release reset. Only
		 * these peripherals' bits are touched. */
		sys_write(SYS_AHB_RESET, sys_read(SYS_AHB_RESET) & ~ahb_clear);
	}
}

/*
 * Top-level SoC init. Runs at PRE_KERNEL_1, before any device driver.
 *
 * Order matters (same as AgRV SDK board_init):
 *   1. FCB first — without it the PLL doesn't know what to lock to
 *   2. Force HSI  — provide a deterministic clock source before
 *                   touching the PLL (matches SYS_SwitchHSIClock)
 *   3. Switch to PLL — only after FCB has programmed the silicon's
 *                     PLL config bits
 *   4. APB clocks for GPIO + UART — peripherals are gated by default
 *   5. UART AFSEL — alternate function routing for TX/RX pins
 *
 * Interrupt state is intentionally left untouched (see file header).
 */
static int agrv2k_clock_init(void)
{
	uint32_t apb;

	if (IS_ENABLED(CONFIG_AGM_SOC_SKIP_CLOCK_INIT)) {
		/* Chain-loaded image (samples/spi_boot_loader payload): the
		 * loader already programmed the fabric, switched the CPU to
		 * the bitstream's PLL and set up UART0/pin routing. Doing it
		 * again would deactivate the fabric this image is clocked
		 * from: a silent core and SWD reporting "stalled AP operation".
		 */
		return 0;
	}

	/* Step 0: FCB0 APB clock. Must be on before fcb.c writes CTRL/AUTO,
	 * otherwise the writes are silently dropped (FCB register interface
	 * is gated). Mirror SDK board.c::SYS_EnableAPBClock(APB_MASK_FCB0)
	 * which fires before FCB_AutoConfig. */
	apb  = sys_read(SYS_APB_CLKENABLE);
	apb |= APB_CLKENABLE_FCB0;
	sys_write(SYS_APB_CLKENABLE, apb);

#if defined(CONFIG_CRC_AGM)
	/* Step 0b: AHB clocks + reset release, *before* step 1 -- but only when
	 * the CRC driver is in the build. The fabric verifier CRCs the slot from
	 * inside agrv2k_fcb_program(), and with a CRC driver present Zephyr
	 * routes crc32_ieee() to that unit (see drivers/crc/Kconfig.agm); with
	 * the gate still closed the unit would read garbage. USB0 / DMAC0 /
	 * EMAC are opened here too (one place owns the AHB domain) and step 4b
	 * repeats the write harmlessly. Builds without the CRC keep the original
	 * order: gates at step 4b, SoC init at priority 0. */
	agrv2k_ahb_clocks();
#endif

	/* Step 1: FCB bitstream injection. */
	int fcb_rc = agrv2k_fcb_program();
	agrv2k_fcb_log_status("boot");
	if (fcb_rc != 0) {
		/* Don't keep going with a half-loaded fabric: clock tree
		 * and pin mux are tuned for the bitstream that just
		 * failed to come up. Halt before UART prints anything.
		 * The printk is the dev board-script's grep target — "FATAL
		 * FCB" should appear exactly once in the capture. */
		printk("FATAL FCB: program failed (rc=%d), halting\n", fcb_rc);
		for (;;) {
			/* spin */
		}
	}

	/* Step 2: Force HSI source. */
	agrv2k_clk_switch_hsi();

	/* Step 3: HSE → PLL switch (with SCLK divider for FLASH). */
	agrv2k_clk_switch_pll(board_pll_frequency, flash_max_freq);

	/* Step 4: APB clocks for the peripherals devicetree enables. */
	apb  = sys_read(SYS_APB_CLKENABLE);
	sys_write(SYS_APB_CLKENABLE, apb | agrv2k_apb_gates());

	/* Step 4b: AHB clocks + reset release for DMAC0 / USB0 / EMAC. The
	 * CRC_AGM build already did this before step 1 and the repeat is
	 * harmless; the other builds have to do it here, and used not to. */
	agrv2k_ahb_clocks();

	return 0;
}

/*
 * PRE_KERNEL_1 priority: 1 when the CRC driver is in the build, 0 otherwise.
 *
 * With CONFIG_CRC_AGM the CRC device registers itself at priority 0 and this
 * bring-up has to run *after* it: the fabric verifier CRCs a slot from inside
 * agrv2k_fcb_program() below, and Zephyr routes crc32_ieee() to any CRC driver
 * that exists (drivers/crc/Kconfig.agm). Nothing else depends on 0 -- the
 * console (CONFIG_SERIAL_INIT_PRIORITY, 50) and every other driver initialize
 * later at this level, and they all need this bring-up first.
 *
 * The two priority literals are not stylistic: the priority is pasted into the
 * init-entry section name, so it cannot be an expression.
 */
#if defined(CONFIG_CRC_AGM)
SYS_INIT(agrv2k_clock_init, PRE_KERNEL_1, 1);
#else
SYS_INIT(agrv2k_clock_init, PRE_KERNEL_1, 0);
#endif

/*
 * sys_reboot() support.
 *
 * Zephyr's fallback for a platform without this symbol prints "Failed to
 * reboot: spinning endlessly..." (: samples/spi_boot_loader's
 * `reboot` command and mcumgr's `os reset` both landed there instead of
 * restarting the board). The SoC resets itself through SYS.RST_CNTL, the same
 * register the vendor SDK writes (`SYS->RST_CNTL |= SYS_RST_SFT`,
 * framework-agrv_sdk/src/system.h). The reset re-runs the whole boot path,
 * FCB bitstream programming included.
 */
void sys_arch_reboot(int type)
{
	ARG_UNUSED(type);

	sys_write(SYS_RST_CNTL, sys_read(SYS_RST_CNTL) | SYS_RST_SFT);

	/* The core is gone once the bit lands; keep the compiler from
	 * falling through if it ever does not. */
	for (;;) {
		arch_nop();
	}
}
