/*
 * Copyright (c) 2026 AgRV Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * AgRV2K FCB (Flash Config Block) — bitstream injection.
 *
 * Mirrors AgRV SDK's framework-agrv_sdk/src/fcb.c, ported to Zephyr.
 *
 * Why this is its own file:
 *   FCB programming is part of the **user RTL flow**, not part of the
 *   SoC clock tree. It reads a Supra-generated bitstream from FLASH
 *   and streams it into the FCB's AUTO register so the silicon's PLL
 *   and IO get reconfigured from cold-boot defaults to whatever rate
 *   the bitstream was synthesized for.
 *
 * SDK reference: framework-agrv_sdk/src/fcb.c::FCB_AutoConfig (the
 * non-DMA, non-LZW path — what SDK board.c:101 picks for the
 * uncompressed 99944-byte example_board.bin we use).
 *
 * What is deliberately NOT ported:
 *
 *  - FCB_AutoConfigDma: AgRV2K's DMAC is not in the Zephyr driver tree, and
 *    the CPU path takes only a few ms at HSI for a one-time boot init —
 *    acceptable cost.
 *  - the per-chip encryption callback (FCB_AutoDecompressEncrypt, the
 *    `board_logic.encrypt` form): a config written that way cannot be
 *    re-streamed by this port, so a board flashed like that does not boot
 *    here at all. Not a cryptographic limitation — the key schedule was read
 *    out of the host tool and the 128-bit flash UID is readable by any code on
 *    the chip — but that form buys
 *    per-chip *binding* rather than any confidentiality, and the vendor's own
 *    documentation has an encrypted logic that disables remote updates.
 *
 * The compressed form, on the other hand, IS supported: enable
 * CONFIG_AGM_FCB_BITSTREAM_COMPRESSED and this file calls the LZW decoder in
 * fcb_lzw.c instead of copying raw words. The SDK exposes that variant as
 * `board_logic.compress = true` (the vendor's own reference boot embeds a
 * *compressed* logic.bin, per the vendor's boot-loader note), it is
 * verified on hardware, and producing it from an existing config is
 * tools/compress_bitstream.py.
 *
 * Getting the choice wrong is not symmetric and not diagnosable from the
 * stream: both forms start with IDCODE/USERID, so a compressed image cannot be
 * told apart by looking at its first words — and streaming one as raw words
 * produces a dead fabric and takes the CPU clock down with it. What we kept is
 * the guard, since the failure is unrecoverable without a cable: the west
 * runner and tools/agm_oo.sh refuse a bitstream that is not 99944 B unless
 * CONFIG_AGM_BITSTREAM_ANY_SIZE is set, so this cannot be reached by accident.
 *
 * STAT readback discipline:
 *   AgRV FCB STAT error bits must be **read** after Activate and **written
 *   with 1 to clear**. SDK examples just check and bail; we go further
 *   and print every error bit, because "FCB_AutoConfig returned RET_OK"
 *   does not mean the fabric came up clean. The CTRL register accepts
 *   plain writes (no RMW needed — bit0..bit7 are independent).
 */

#include <errno.h>
#include <zephyr/init.h>
#include <zephyr/sys/printk.h>
#include <zephyr/arch/cpu.h>

#include <zephyr/drivers/misc/agm_bitstream.h>

#include "fcb_lzw.h"

/* FCB0 register base (0x40010000). */
#define FCB0_BASE              0x40010000UL

/* Register offsets (AltaRiscv.h FCB_TypeDef). */
#define FCB_REG_CTRL           0x00U
#define FCB_REG_ADDR           0x04U
#define FCB_REG_DATA           0x08U
#define FCB_REG_AUTO           0x0CU
#define FCB_REG_STAT           0x10U
#define FCB_REG_INT            0x14U

/* CTRL bits. */
#define FCB_CTRL_INIT          (1U << 0)
#define FCB_CTRL_WRITE         (1U << 1)
#define FCB_CTRL_READ          (1U << 2)
#define FCB_CTRL_UPDATE        (1U << 3)
#define FCB_CTRL_ACTIVATE      (1U << 4)
#define FCB_CTRL_DEACTIVATE    (1U << 5)
#define FCB_CTRL_AUTO          (1U << 6)
#define FCB_CTRL_DMA           (1U << 7)

/* STAT bits. INIT/ACTIVE are status, ERR_* are write-1-to-clear. */
#define FCB_STAT_INIT          (1U << 0)
#define FCB_STAT_ACTIVE        (1U << 1)
#define FCB_STAT_ERR_ID        (1U << 4)
#define FCB_STAT_ERR_HEADER    (1U << 5)
#define FCB_STAT_ERR_CRC       (1U << 6)
#define FCB_STAT_ERR_ALL       (FCB_STAT_ERR_ID | FCB_STAT_ERR_HEADER | \
				 FCB_STAT_ERR_CRC)

/* Number of 32-bit words in the embedded bitstream. Matches
 * AgRV SDK framework-agrv_sdk/src/fcb.h::FCB_AUTO_WORDS = 99944/4
 * = 24986. */
#define FCB_AUTO_WORDS         24986U

/* Bitstream location in FLASH (set by west flash --runner agrv32flash's
 * --bitstream-addr default, or by examples/flash_logic.sh). This is the
 * factory slot: the bitstream A/B record below can point at an update slot
 * instead, but nothing on the device ever rewrites this one. */
/* The config inside the factory region: same as the region base for a raw
 * bitstream, + the ROM's decompression algorithm for a compressed one (which
 * is what the option byte points at, and so what the ROM streamed). */
#define FCB_BITSTREAM_ADDR     AGM_BITSTREAM_CONFIG_ADDR

/*
 * A signed fabric is verified right here, at PRE_KERNEL_1 -- and until
 * z_cstart() switches to the main thread, that runs on the interrupt stack.
 * SHA-256 over the fabric plus a tinycrypt P-256 verify needs more than the
 * 2048-byte default leaves, and the failure mode would be a board that never
 * reaches the console: make it a build error instead. (The verification is
 * allocation-free, so the *heap* is not the constraint here; the stack is.)
 */
#if AGM_BITSTREAM_SIGNED && defined(CONFIG_ISR_STACK_SIZE)
BUILD_ASSERT(CONFIG_ISR_STACK_SIZE >= 3072,
	     "CONFIG_BOOT_AGM_BITSTREAM_SIGNED verifies the fabric on the "
	     "interrupt stack: set CONFIG_ISR_STACK_SIZE >= 3072");
#endif

/* Bound on how long we wait for FCB_STAT_ACTIVE to come up after
 * Activate. Vendor SDK does not document a timeout; 1ms is generous
 * given the silicon programs in microseconds at HSI.
 *
 *  with example_board.bin: ACTIVE comes up
 * within ~50 us after ACTIVATE, so 1ms gives 20x headroom for
 * future bitstreams. */
#define FCB_ACTIVE_TIMEOUT_US  1000U

/* Each fcb_wait_active iteration reads STAT once. With arch_nop the
 * loop body cannot be optimised away, but we still calibrate by
 * counting iterations rather than wall-clock cycles — 100 reads is
 * ~1us at 100 MHz HSI (~10 cycle per STAT read + 1 cycle nop). */
#define FCB_ITER_PER_US        100U

static inline uint32_t fcb_read(uint32_t off)
{
	return *((volatile uint32_t *)(FCB0_BASE + off));
}

/* Plain write: CTRL bits are independent (no RMW needed). */
static inline void fcb_write(uint32_t off, uint32_t val)
{
	*((volatile uint32_t *)(FCB0_BASE + off)) = val;
}

/*
 * FCB_AutoConfig: stream the bitstream from FLASH to FCB->AUTO.
 * Mirrors AgRV SDK framework-agrv_sdk/src/fcb.c::FCB_AutoConfig.
 *
 * FLASH is XIP-able at this point (we run from it), so CPU reads
 * from flash_addr are direct. The 24986-word stream takes a few ms
 * at HSI; fine for one-time boot init.
 *
 * The stream is just data — vendor IP does not validate the bitstream
 * header until ACTIVATE, so this function always returns 0; error
 * bits are reported by the caller via fcb_wait_active() + STAT read.
 */
static int fcb_auto_config(uint32_t flash_addr)
{
	volatile uint32_t *auto_reg =
		(volatile uint32_t *)(FCB0_BASE + FCB_REG_AUTO);
	const volatile uint32_t *flash =
		(const volatile uint32_t *)flash_addr;
	uint32_t stat;

	/* Clear any stale error bits from a previous boot before we
	 * start — STAT is sticky on the error side. */
	stat = fcb_read(FCB_REG_STAT);
	if (stat & FCB_STAT_ERR_ALL) {
		fcb_write(FCB_REG_STAT, FCB_STAT_ERR_ALL);
	}

	fcb_write(FCB_REG_CTRL, FCB_CTRL_AUTO);
	for (uint32_t i = 0; i < FCB_AUTO_WORDS; i++) {
		*auto_reg = flash[i];
	}
	fcb_write(FCB_REG_CTRL, 0);
	return 0;
}

#if defined(CONFIG_AGM_FCB_BITSTREAM_COMPRESSED)
/* Where the decoded words go: the same AUTO register the raw path streams
 * into. Nothing else needs a context, hence the unused parameter. */
static void fcb_auto_emit(void *ctx, uint32_t word)
{
	ARG_UNUSED(ctx);

	*((volatile uint32_t *)(FCB0_BASE + FCB_REG_AUTO)) = word;
}

/*
 * FCB_AutoDecompress: same thing for the compressed build of a Supra config
 * (`logic_compress = true`), which is what the vendor SDK's
 * FCB_AutoDecompress() does -- see fcb_lzw.c for the decoder and
 * tools/compress_bitstream.py for how to make the image.
 *
 * The config's first two words (IDCODE, USERID) are stored raw; the LZW stream
 * follows them and decodes to the remaining FCB_AUTO_WORDS - 2 words, exactly
 * like the SDK's decoder (which writes those two words out before decoding).
 *
 * A stream that is truncated or corrupt cannot be detected here -- a config in
 * flash carries no length -- so it decodes whatever follows; the FCB then
 * reports ERR_HEADER/ERR_CRC at ACTIVATE and the caller halts. That is the same
 * failure mode the raw path has with a wrong address.
 */
static int fcb_auto_decompress(uint32_t flash_addr)
{
	const volatile uint32_t *flash = (const volatile uint32_t *)flash_addr;
	uint32_t stat;

	stat = fcb_read(FCB_REG_STAT);
	if (stat & FCB_STAT_ERR_ALL) {
		fcb_write(FCB_REG_STAT, FCB_STAT_ERR_ALL);
	}

	fcb_write(FCB_REG_CTRL, FCB_CTRL_AUTO);
	*((volatile uint32_t *)(FCB0_BASE + FCB_REG_AUTO)) = flash[0];
	*((volatile uint32_t *)(FCB0_BASE + FCB_REG_AUTO)) = flash[1];

	if (fcb_lzw_decode((const uint8_t *)flash_addr + 8U,
			   FCB_AUTO_WORDS - 2U, fcb_auto_emit, NULL) != 0) {
		fcb_write(FCB_REG_CTRL, 0);
		printk("FATAL FCB: compressed bitstream is not decodable\n");
		return -EIO;
	}

	fcb_write(FCB_REG_CTRL, 0);
	return 0;
}
#endif /* CONFIG_AGM_FCB_BITSTREAM_COMPRESSED */

static inline void fcb_activate(void)
{
	/* CTRL accepts plain writes; ACTIVATE is bit4. */
	fcb_write(FCB_REG_CTRL, FCB_CTRL_ACTIVATE);
}

/*
 * Poll STAT until ACTIVE comes up, or until timeout.
 *
 * Returns the STAT register value (caller decides what counts as
 * success — at minimum ACTIVE must be set, ERR_* must be clear).
 *
 * Calibration: one iteration is one STAT read + one arch_nop. At
 * 100 MHz HSI that is ~10 cycles = 100 ns, so FCB_ITER_PER_US=100
 * is a safe lower bound. FCB_ACTIVE_TIMEOUT_US=1000 gives a 1ms
 * wall-clock ceiling.
 */
static uint32_t fcb_wait_active(void)
{
	uint32_t stat;
	const uint32_t max_iter =
		FCB_ACTIVE_TIMEOUT_US * FCB_ITER_PER_US;

	for (uint32_t i = 0; i < max_iter; i++) {
		stat = fcb_read(FCB_REG_STAT);
		if (stat & FCB_STAT_ACTIVE) {
			return stat;
		}
		arch_nop();
	}

	return fcb_read(FCB_REG_STAT);
}

/*
 * Decode STAT to a short tag for printk.
 *
 * Reused by both boot-time and runtime paths so the banner format
 * is identical: "FCB ACTIVE/INIT/ERR_ID/ERR_HEADER/ERR_CRC".
 */
static const char *fcb_stat_tag(uint32_t stat)
{
	if (stat & FCB_STAT_ERR_ID) {
		return "ERR_ID";
	}
	if (stat & FCB_STAT_ERR_HEADER) {
		return "ERR_HEADER";
	}
	if (stat & FCB_STAT_ERR_CRC) {
		return "ERR_CRC";
	}
	if ((stat & FCB_STAT_ACTIVE) && (stat & FCB_STAT_INIT)) {
		return "ACTIVE+INIT";
	}
	if (stat & FCB_STAT_ACTIVE) {
		return "ACTIVE";
	}
	if (stat & FCB_STAT_INIT) {
		return "INIT";
	}
	return "IDLE";
}

/*
 * Public: called by soc.c during PRE_KERNEL_1 to inject the bitstream
 * and re-program the silicon PLL/IO from whatever was flashed.
 *
 * Always (re)configures the FCB on every boot. The bitstream may have
 * been rewritten by a previous flash step; unconditionally run
 * AutoConfig + Activate so the FCB is in sync with what's actually in
 * FLASH, even on warm resets where the FCB might otherwise carry
 * stale state.
 *
 * Returns 0 on success (ACTIVE set, no errors), negative errno on
 * any FCB_STAT_ERR_* — caller is expected to dump & halt.
 */
/* Which slot this boot actually streamed, published for whoever runs after
 * (the console is not up yet here, so the printk below is lost --
 *). Not zeroed at boot on purpose: a warm reset keeps it, which is
 * what makes "the record said X and the FCB used X" checkable on the dev board. */
__noinit uint32_t agm_fcb_bitstream_slot;

/* Why the record's slot was *not* streamed, published the same way: the
 * verifier's detailed lines are printed here, at PRE_KERNEL_1, where the
 * console does not exist yet, so without this a board that fell back to the
 * factory fabric would look identical to one whose record was empty. The boot
 * driver prints it (with the reason spelled out) once the console is up. */
__noinit int32_t agm_fcb_bitstream_refused;

/* Did the FCB bring-up run in *this* boot? Deliberately NOT __noinit: unlike
 * the two values above (which survive a warm reset on purpose so the dev board
 * can compare them), this one has to be clearable, because it is what tells a
 * later init level whether reading them means anything. Without it a boot
 * driver that ran before this one -- an init-order regression -- would read the
 * BSS defaults and report "the FCB reported nothing" instead of saying that it
 * asked too early. */
static bool fcb_program_ran;

bool agm_fcb_program_ran(void)
{
	return fcb_program_ran;
}

uint32_t agm_fcb_bitstream_slot_get(void)
{
	return agm_fcb_bitstream_slot;
}

int32_t agm_fcb_bitstream_refused_get(void)
{
	return agm_fcb_bitstream_refused;
}

int agrv2k_fcb_program(void)
{
	/* The bitstream A/B record decides this; see agm_bitstream.h for why an
	 * update never lands in the slot that is running. */
	struct agm_bitstream_boot sel = agm_bitstream_boot_select();
	uint32_t stream = agm_bitstream_stream_addr(&sel);
	uint32_t stat;

	fcb_program_ran = true;
	agm_fcb_bitstream_refused = 0;
#if AGM_BITSTREAM_SIGNED
	/* Authenticate the fabric *before* anything in the FCB is touched: a
	 * slot whose bytes do not match the record, or whose MCUboot container
	 * does not verify against the trusted key, is not streamed at all and
	 * the board stays on the fabric the ROM loaded from the factory
	 * region. This runs in every image that owns the boot path, at
	 * PRE_KERNEL_1, which is why the fabric profile is the allocation-free
	 * ECDSA one (see agm_bitstream.h). */
	if (sel.from_record) {
		int vret = agm_boot_bitstream_verify(sel.slot, sel.len, sel.crc);

		if (vret != 0) {
			agm_fcb_bitstream_refused = vret;
			/* This printk is one of the few that fit here: the whole
			 * PRE_KERNEL_1 verifier runs on the interrupt stack, and
			 * adding a second one *after* agm_boot_bitstream_verify()
			 * -- i.e. on top of the ECDSA frame -- panicked the boot
			 * with the ISR stack at 4096
			 * ("ZEPHYR FATAL ERROR 4: Kernel panic", and the line never
			 * reached the console). Diagnostics in this window cost stack;
			 * size the window before keeping one. */
			printk("FCB bitstream: 0x%08x did not verify (%d) -- streaming "
			       "the factory slot (0x%08x) instead\n", sel.slot, vret,
			       (uint32_t)FCB_BITSTREAM_ADDR);
			stream = FCB_BITSTREAM_ADDR;
		}
	}
#endif

	/* What the FCB streams is the *stream* address: for a signed slot that
	 * is the fabric behind the container header, for the factory region the
	 * config the option byte points at. The upload path reads this through
	 * the console (`status`) and the dev board scripts through SWD. */
	agm_fcb_bitstream_slot = stream;

	printk("FCB bitstream: 0x%08x (%s)\n", stream,
	       (stream == FCB_BITSTREAM_ADDR) ? "factory slot" : "update slot");

	/* Which decoder: the two forms are the same fabric description in
	 * different containers, and only the board knows which one it has in
	 * flash (the option bytes carry the flag, but those are reachable only
	 * through the debug/flash-controller path, not from the CPU). */
	int rc;

#if defined(CONFIG_AGM_FCB_BITSTREAM_COMPRESSED)
	/* A compressed build streams the vendor's compressed form from either
	 * source -- except from a signed update slot, which holds the *raw*
	 * fabric: the vendor's algorithm+config blob is not something we can
	 * put a container around (the ROM runs that algorithm, we do not). */
	if (sel.from_record && AGM_BITSTREAM_SIGNED) {
		rc = fcb_auto_config(stream);
	} else {
		rc = fcb_auto_decompress(stream);
	}
#else
	rc = fcb_auto_config(stream);
#endif
	if (rc != 0) {
		return -EIO;
	}
	fcb_activate();
	stat = fcb_wait_active();

	/* Clear error bits so the next boot starts clean. */
	if (stat & FCB_STAT_ERR_ALL) {
		fcb_write(FCB_REG_STAT, FCB_STAT_ERR_ALL);
		return -EIO;
	}
	if (!(stat & FCB_STAT_ACTIVE)) {
		return -ETIMEDOUT;
	}
	return 0;
}

/*
 * Public: print the current FCB STAT as a single banner line.
 * Used by soc.c right after agrv2k_fcb_program() (so a boot log always
 * carries the fabric-load verdict) and by samples/fcb_reload, which probes
 * which bitstream is live after a FLASH-side reload + reboot.
 */
void agrv2k_fcb_log_status(const char *tag)
{
	uint32_t stat = fcb_read(FCB_REG_STAT);

	printk("FCB %s: STAT=0x%08x (%s)\n",
	       tag ? tag : "status", stat, fcb_stat_tag(stat));
}

/*
 * Runtime (hot) reload — `agrv2k_fcb_reload()`.
 *
 * This is the same DEACTIVATE / AutoConfig / ACTIVATE sequence the boot path
 * above uses, but applied to a fabric that is already live, so it is *not* a
 * drop-in call: on AgRV2K the CPU's own sys_clk/bus_clk are produced inside
 * the fabric (example_board.bin carries the PLL and the clock switch that
 * drive `rv32.sys_clk` — `gclksw_inst|gclk_switch__alta_gclksw__clkout ->
 * rv32|sys_clk` in the Quartus log, see
 * Kconfig), so a reload that runs with the CPU
 * still clocked by the old fabric stops the core mid-instruction and takes
 * the console, the SWD AP and the ROM bootloader with it.
 *
 * The caller therefore has to move the CPU onto a clock that does not come
 * from the fabric first:
 *
 *   agrv2k_clk_switch_hsi();                  // CPU off the fabric PLL
 *   agrv2k_fcb_reload(new_bitstream_addr);
 *   agrv2k_clk_switch_pll(pll_hz, flash_hz);  // hand it to the new fabric
 *   <re-apply pinctrl / peripheral init>
 *
 * samples/fcb_hotswap does exactly that, validates the target image before
 * touching the fabric, and prints the evidence. The same-class constraint is
 * real: the new bitstream has to keep this board's SYSCLK, FLASH clock class
 * and console pin routing, or the console comes back as garbage.
 *
 * A reload is still the *last* resort for a plain "swap the fabric" dev board:
 * putting the image at FCB_BITSTREAM_ADDR and rebooting (agrv2k_fcb_program()
 * + agrv2k_clk_switch_pll(), i.e. the boot path) does the same thing with the
 * CPU never at risk.
 */
int agrv2k_fcb_reload(uint32_t flash_addr)
{
	uint32_t stat;

	/* Drop the live configuration before streaming the replacement:
	 * AutoConfig writes the same shift register that built it. */
	fcb_write(FCB_REG_CTRL, FCB_CTRL_DEACTIVATE);

	if (fcb_auto_config(flash_addr) != 0) {
		return -EIO;
	}
	fcb_activate();
	stat = fcb_wait_active();

	if (stat & FCB_STAT_ERR_ALL) {
		fcb_write(FCB_REG_STAT, FCB_STAT_ERR_ALL);
		return -EIO;
	}
	if (!(stat & FCB_STAT_ACTIVE)) {
		return -ETIMEDOUT;
	}
	return 0;
}
