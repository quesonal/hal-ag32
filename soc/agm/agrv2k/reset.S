/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Copyright (c) 2026 AgRV Contributors
 *
 * AgRV2K-specific __reset, replacing arch/riscv/core/reset.S's default.
 *
 * HOW WE'RE WIRED INTO ZEPHYR
 * ---------------------------
 * Zephyr's INCLUDE_RESET_VECTOR default implementation jumps
 * directly to __initialize. We provide our own __reset in this file
 * and rely on Zephyr's existing mechanism — once it sees a __reset
 * symbol defined by our SoC sources (linked via CMakeLists.txt's
 * zephyr_sources(reset.S ...)), it auto-clears INCLUDE_RESET_VECTOR
 * and we own the boot path entirely. No shared-source edits, no
 * arch/riscv/core/reset.S patch, no `--gc-sections` fragility.
 *
 * PROBLEM 1 — FLASH prefetch stale bytes
 * --------------------------------------
 * AgRV2K's openocd warm-reset (`reset run`) does NOT invalidate the
 * FLASH instruction prefetch buffer. After `flash write_image` rewrites
 * FLASH in place, the CPU may execute stale bytes from the previous
 * firmware, surfacing as mcause=2 illegal-instruction traps at
 * arbitrary .text addresses. Empirically (see
 * memory/agmv2k-flash-icache-prefetch-trap.md):
 *   - 1x fence.i : trap site shifts 4 B, still illegal
 *   - 5x fence.i : trap site shifts ~20 B, still illegal
 *   - 12x nop linear pad (48 B) + tail : still see banner truncation
 *     at 27 bytes and boot loop on warm-reset
 *
 * AgRV SDK's `agm_example.bin` works because its `_start` (at FLASH
 * 0x80000000) is a single tight LINEAR block of ~244 B:
 *   fence.i + GPR zero + FPU zero + mtvec set + `j _init`.
 * No branches until the very last `tail` / `j`. Every cache-line
 * fill after the first `fence.i` is from fresh FLASH bytes because
 * the linear sequence is long enough that the prefetch buffer's
 * depth (≤ 128 B on AgRV2K) cannot hold stale entries from before.
 *
 * We mirror that pattern: ONE `fence.i`, then a long LINEAR sequence
 * of GPR-zero + mtimecmp-saturate + mtvec-set, then the single far
 * jump. No branches until the final `tail` — by then the prefetch
 * buffer is full of fresh FLASH bytes from this preamble.
 *
 * PROBLEM 2 — MTIP preempt printk / XIP prefetch stall
 * -----------------------------------------------------
 * Zephyr's shared arch/riscv/core/reset.S `__initialize` enables
 * `mie.MTIE` right before mret (so the kernel timer driver can take
 * MTIP and forward as STIP for sys_clock ticks). On AgRV2K that MTIP
 * fires every 50 µs and preempts `main()` mid-`printk` →
 * `pl011_poll_out`'s busy-wait polling FR.TXFF. Each preemption
 * + XIP prefetch refill drops individual TX bytes; banner truncates
 * mid-line at byte 27 ("(FCB PLL,") and the board boot-loops.
 * CPU is alive (mcause=0x80000007, mepc=0x8000163c in
 * pl011_poll_out) but the host only sees the first 27 bytes.
 *
 * AgRV SDK never sees this: SDK's INT_Init does `csrwi mie, 0` and
 * never re-enables MTIE; SDK has no kernel timer driver to service
 * MTIP anyway.
 *
 * We can't patch the shared `__initialize` — that breaks every
 * other RISC-V SoC. Instead we DON'T call `__initialize` at all.
 * We replicate the small slice of `__initialize` we actually need
 * for M-mode boot: FPU init, sp, TLS, then `tail z_prep_c` directly
 * (AgRV2K's DTS declares only "m","u" privilege modes, so the
 * S-mode `mret`/`s_mode_entry`/`csrs mie, MIP_MTIP` block in
 * __initialize is irrelevant anyway). mie.MTIE is `csrc`'d here;
 * it stays off through boot and is only set later by the timer
 * driver's own init (see below).
 *
 * mtimecmp is also saturated to 0xFFFFFFFFFFFFFFFF in our preamble
 * as defense-in-depth: MTIP cannot assert before the timer driver
 * (PRE_KERNEL_2) programs a finite deadline. RV32 split: mtimecmp
 * is at MTIMECMP (low half) + MTIMECMP+4 (high half). SiFive CLINT
 * spec mandates write-high-then-low.
 *
 * System timer: the upstream riscv_machine_timer driver is enabled via
 * the timer0 DT node. Its PRE_KERNEL_2 init
 * (drivers/timer/riscv_machine_timer.c) writes
 * mtimecmp = last_count + CYC_PER_TICK and then irq_enable(7) →
 * mie.MTIE. That ordering is the whole point: MTIE is never set while
 * mtimecmp is still 0 or saturated, so the early-boot MTIP storm
 * cannot recur.
 *
 * A polling-mode timer (timer_poll.c + custom arch_cpu_idle) was tried
 * and removed — it only announced from the idle path, freezing ticks
 * whenever a thread ran without sleeping.
 *
 * ENTRY STATE
 * -----------
 * No assumptions about register / CSR state on entry. vector.S::__start
 * has already done `csrw mtvec, _isr_wrapper` and `tail __reset`'d us.
 *
 * SIZE
 * ----
 * 1 fence.i (4 B) + 31 li rN,0 (124 B) + 4 store mtimecmp (16 B) +
 * 1 la mtvec (8 B) + 1 csrw mtvec (4 B) + csrr mhartid / sp setup /
 * fs init / tls init / csrc mie / mstatus.MPP=S / mret = roughly
 * +30 instructions. Total ~190 B. Larger than the prefetch-buffer
 * depth (~64–128 B on AgRV2K) so stale entries cannot survive.
 *
 * If testing shows prefetch-buffer drain is still incomplete, pad
 * with additional linear `nop`s between the last li and the
 * mtimecmp stores.
 */

#include <zephyr/toolchain.h>
#include <zephyr/linker/sections.h>
#include <zephyr/offsets.h>           /* __z_interrupt_stack_SIZEOF etc. */
#include <zephyr/arch/riscv/csr.h>
#include <zephyr/arch/riscv/arch.h>

/* exports */
GTEXT(__reset)

/* imports */
GTEXT(_isr_wrapper)
GTEXT(z_prep_c)      /* C: zeros BSS, copies .data, calls z_cstart */

/*
 * CLINT mtimecmp on AgRV2K. AgRV2K is a stand-alone port — there's
 * no <soc/memory.h> to consume, so we hard-code the address.
 * Matches the SiFive CLINT layout.
 */
#define MTIMECMP 0x02004000

SECTION_FUNC(reset, __reset)
	/*
	 * SDK uses ONE fence.i. AgRV2K hardware docs note that one
	 * fence.i takes "an extra cycle to drain the prefetch buffer".
	 * We mirror SDK's one-fence.i choice.
	 */
	fence.i

	/*
	 * Linear GPR zero — 31 instructions, 124 B of fresh-fetch window.
	 * No branches, so prefetch buffer never gets a chance to skip
	 * ahead and refetch stale FLASH bytes.
	 *
	 * t0 / t2 used below for mtimecmp writes, so they need to remain
	 * zero — already done by the li rN, 0 block.
	 */
	li	ra, 0
	li	sp, 0
	li	gp, 0
	li	tp, 0
	li	t0, 0
	li	t1, 0
	li	t2, 0
	li	s0, 0
	li	s1, 0
	li	a0, 0
	li	a1, 0
	li	a2, 0
	li	a3, 0
	li	a4, 0
	li	a5, 0
	li	a6, 0
	li	a7, 0
	li	s2, 0
	li	s3, 0
	li	s4, 0
	li	s5, 0
	li	s6, 0
	li	s7, 0
	li	s8, 0
	li	s9, 0
	li	s10, 0
	li	s11, 0
	li	t3, 0
	li	t4, 0
	li	t5, 0
	li	t6, 0

	/*
	 * Saturate mtimecmp to 0xFFFFFFFFFFFFFFFF so MTIP can never
	 * fire regardless of what `mie.MTIE` Zephyr later enables.
	 * RV32 split: write HIGH half first (SiFive CLINT spec).
	 */
	li	t0, -1
	li	t2, MTIMECMP
	sw	t0, 4(t2)            /* mtimecmp high half */
	sw	t0, 0(t2)            /* mtimecmp low half */

	/*
	 * mtvec → _isr_wrapper. Use `la` (assembler pseudo) which the
	 * Zephyr toolchain lowers to auipc + addi (PIC, no GOT relocs).
	 * This is the same pattern SDK uses at 0x800000d8.
	 *
	 * We have to do this HERE (not rely on vector.S) because if
	 * vector.S::mtvec setting code itself was prefetched stale,
	 * a timer interrupt fired during the preamble above would jump
	 * to garbage. Setting mtvec again at the end of the preamble
	 * — with prefetch guaranteed fresh — gives us a safe landing.
	 */
	la	t0, _isr_wrapper
	csrw	mtvec, t0

	/*
	 * --- Skip Zephyr's shared __initialize ---
	 *
	 * We replicate the M-mode boot work `__initialize` does, but with
	 * `csrc mie, MIP_MTIP` instead of `csrs`, and we `tail z_prep_c`
	 * directly. AgRV2K's DTS declares only `"m", "u"` privilege modes
	 * so `RISCV_S_MODE` = n — the kernel runs in M-mode, and there
	 * is no s_mode_entry / mret dance to perform. __initialize's
	 * tail for M-mode is just `call z_prep_c`, which is exactly
	 * what we do here.
	 *
	 * Replicated:
	 *   - mhartid → a0 (only meaningful on SMP; AgRV2K is single-
	 *     hart so a0 stays 0 from the GPR-zero block)
	 *   - FPU enable: csrs mstatus, MSTATUS_FS | MSTATUS_XS
	 *   - FPU csr clear (fscsr) and zero all 32 FP regs (fmv.s.x fN, x0)
	 *   - Stack pointer: la sp, z_interrupt_stacks + size
	 *   - TLS init (skipped — single-hart M-mode, see below)
	 *   - mie.MTIE = 0   (instead of __initialize's csrs mie, MIP_MTIP)
	 *   - tail z_prep_c  (skips __initialize entirely)
	 */
#if defined(CONFIG_FPU)
	/* Mirror SDK crt.S:45-46. FS=01 (Initial), XS=01 (Initial). */
	li	t0, MSTATUS_FS | MSTATUS_XS
	csrs	mstatus, t0

	/* fscsr clears FP exception flags; SDK also clears rounding mode
	 * back to IEEE-754 default (RNE). Zephyr's __initialize only
	 * does fscsr zero. Same effect — fscsr with x0 = default state.
	 */
	fscsr	zero

	/* SDK crt.S:54-85 — zero all 32 FP regs with `fmv.s.x fN, x0`
	 * so first-use by main() sees clean values. Costs 32
	 * instructions (128 B), still well within the prefetch-buffer
	 * depth, so no XIP-stale risk. hello_world doesn't use FPU so
	 * this is dead-on-arrival, but if a future sample adds math
	 * it's already correct.
	 */
	fmv.s.x	f0,  zero
	fmv.s.x	f1,  zero
	fmv.s.x	f2,  zero
	fmv.s.x	f3,  zero
	fmv.s.x	f4,  zero
	fmv.s.x	f5,  zero
	fmv.s.x	f6,  zero
	fmv.s.x	f7,  zero
	fmv.s.x	f8,  zero
	fmv.s.x	f9,  zero
	fmv.s.x	f10, zero
	fmv.s.x	f11, zero
	fmv.s.x	f12, zero
	fmv.s.x	f13, zero
	fmv.s.x	f14, zero
	fmv.s.x	f15, zero
	fmv.s.x	f16, zero
	fmv.s.x	f17, zero
	fmv.s.x	f18, zero
	fmv.s.x	f19, zero
	fmv.s.x	f20, zero
	fmv.s.x	f21, zero
	fmv.s.x	f22, zero
	fmv.s.x	f23, zero
	fmv.s.x	f24, zero
	fmv.s.x	f25, zero
	fmv.s.x	f26, zero
	fmv.s.x	f27, zero
	fmv.s.x	f28, zero
	fmv.s.x	f29, zero
	fmv.s.x	f30, zero
	fmv.s.x	f31, zero
#endif

	la	sp, z_interrupt_stacks
	li	t0, __z_interrupt_stack_SIZEOF
	add	sp, sp, t0

	/*
	 * TLS init: SKIPPED for AgRV2K.
	 *
	 * AgRV2K is single-hart M-mode; the arch/riscv/core/reset.S
	 * `riscv_tls_init_early` macro is designed for S-mode SBI
	 * trampolines that hand off to multi-hart C entry, neither of
	 * which apply here. tp is left at 0 from the GPR-zero block,
	 * which is fine for hello_world (no CONFIG_STACK_CANARY, no
	 * __thread / thread_local variables). If a future sample needs
	 * proper TLS, port the macro body here or call a C helper from
	 * the M-mode init path (soc.c, PRE_KERNEL_1) — tp is writable
	 * from M-mode via `mv tp, ...`.
	 */

	/*
	 * mie.MTIE = 0 (before z_prep_c / any driver runs). The upstream
	 * riscv_machine_timer driver sets mie.MTIE itself at
	 * PRE_KERNEL_2 — after it has armed a finite mtimecmp — so
	 * MTIP can never fire during the early boot window.
	 */
	li	t0, MIP_MTIP
	csrc	mie, t0

	/*
	 * Jump straight into C domain. z_prep_c zeros BSS, copies rw
	 * data into RAM, then enters kernel z_cstart. `tail` is a
	 * pseudo-op equivalent to `auipc + addi + jr` (PIC, far-call
	 * capable).
	 */
	tail	z_prep_c