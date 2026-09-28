/*
 * Copyright (c) 2026 AgRV Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief AN3155 server for the AgRV2K bootloader driver (agrv32flash).
 *
 * Answers the vendor's `agrv32flash` (ST AN3155 protocol) on the console UART,
 * on windows that mirror the real flash, *derived* from the layout rather than
 * pinned (default on-die layout: `0x80000000` store A, `0x800ae000` store B,
 * `0x8007c000` the on-die slot, and the fabric staging slot the session pins
 * -- see agm_boot_target_window_base()). The
 * vendor tool only writes a whole file when the address lands inside a flash
 * range its device table knows; outside one it does a single 4-byte write and
 * stops (protocol quirks and native tests
 * tests/drivers/misc/boot_agm*).
 *
 * Storage lives in boot_agm.c; this file is framing, checksums, address decoding
 * and the tool's quirks. Two rules: every command carries its complement byte,
 * and nothing may printk while a session is live (a stray line between two ACKs
 * is read as the reply and the host aborts) -- diagnostics are buffered and
 * flushed afterwards.
 */

#include <zephyr/arch/cpu.h>
#include <zephyr/device.h>
#include <zephyr/drivers/misc/boot_agm.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

#include <string.h>

/* How many A/B stores the protocol's window map knows about (the record has
 * two; the window indexes below are the protocol's own). */
#define BOOT_SLOTS 2U

/* Window indexes: 0/1 are the stores, BOOT_SLOTS (2) is the bitstream staging
 * area and the last one is the on-die application slot. */
#define AN_WIN_ON_DIE (BOOT_SLOTS + 1U)
#define AN_WIN_COUNT  (BOOT_SLOTS + 2U)

#define AN_CMD_INIT 0x7FU

/* How long one byte of a frame may take before the session is dropped. A real
 * host sends the bytes of a frame back to back, so this only has to be wider
 * than the USB CDC / FIFO latency. */
#define AN_BYTE_TIMEOUT_MS 2000

/* Backstop for the idle timeout above; see the loop. Generous on purpose: a
 * real session only spins while it waits for the host's next byte. */
#define AN_SPIN_LIMIT 5000000U

/* The flash window the vendor tool writes into, per target. These are the
 * addresses the targets are exposed at; the driver knows the layout, this is
 * only the protocol's view of it. */
#define AN_WIN_A      (agm_boot_target_window_base(NULL, AGM_BOOT_TARGET_STORE_A))
#define AN_WIN_A_MAX  (agm_boot_target_window_size(NULL, AGM_BOOT_TARGET_STORE_A))
#define AN_WIN_SLOT   (agm_boot_target_window_base(NULL, AGM_BOOT_TARGET_SLOT))
#define AN_WIN_SLOT_MAX (agm_boot_target_window_size(NULL, AGM_BOOT_TARGET_SLOT))
#define AN_WIN_B      (agm_boot_target_window_base(NULL, AGM_BOOT_TARGET_STORE_B))
#define AN_WIN_B_MAX  (agm_boot_target_window_size(NULL, AGM_BOOT_TARGET_STORE_B))
#define AN_WIN_BS     (agm_boot_target_window_base(NULL, AGM_BOOT_TARGET_BITSTREAM))
#define AN_WIN_BS_MAX (agm_boot_target_window_size(NULL, AGM_BOOT_TARGET_BITSTREAM))

/* ---- AN3155 subset: let the vendor `agrv32flash` drive the stores ---- */

/*
 * Address windows sit inside the on-die flash range the tool knows, because
 * that is the only place `agrv32flash -w` writes a whole file (anything else is
 * a single 4-byte "register" write):
 *   0x80000000  store A (192 KiB)          0x8007c000  on-die application slot
 *   0x800ae000  store B (up to 0x800b3fff) 0x800b4000  bitstream staging
 * The windows follow the real flash (store A starts where the loader lives, the
 * slot window *is* the application slot -- writes go there directly, nothing is
 * copied at boot) and are clamped to `store-max-size`
 * (agm_boot_target_window_size()), so the sizes above are the default layout's.
 *
 * The bitstream window is readable only as far as this session wrote it: the
 * fabric is licensed IP, and that read path is how `-r` would dump it -- but it
 * is also how the tool verifies the write it just made, so refusing reads
 * outright would break fabric DFU . Past the watermark reads NACK;
 * `CONFIG_BOOT_AGM_AN3155_READ_FABRIC=y` opens the window for dev board work.
 *
 * Commands: 0x7F INIT, 0x00 GET, 0x01 GVR, 0x02 GID, 0x11 read, 0x31 write,
 * 0x44 erase (accepted; writes erase lazily), 0xA1 CRC, 0x21 GO (publish: the
 * stores go TRIAL+active and boot, the bitstream window is applied and the
 * board reboots into it). Drive it with `-m 8n1` -- the console is 8N1, while
 * agrv32flash defaults to the ROM bootloader's 8e1.
 */
/* Highest byte the host has written through AN3155 since boot, per window
 * (A, B, bitstream, on-die). GO carries an address but no length, so this is
 * how the loader knows how much of the window to publish. */
static uint32_t an_written_end[AN_WIN_COUNT];

/* Windows the previous session used, so the first write into a window starts
 * a fresh erase run. Both the per-window write watermark and the upload
 * channel's erase watermark have to be dropped then: they are otherwise left
 * over from an earlier session, and a stale erase watermark means the sector
 * is never erased, so the write fails ("Failed to write memory at address
 * ..."). The write watermark additionally *is* the length GO publishes. */
static uint32_t an_window_used;   /* bit per window (A, B, bitstream) */

/* Last write attempt the AN3155 path made, for an-status: the error code and
 * the parameters it used. */
static int an_last_wm_ret;
static uint32_t an_last_wm_off;
static uint32_t an_last_wm_len;

/* Log of the commands the host sent (opcode plus the address/length each one
 * carried), so the console can show exactly what a host tool did -- including
 * the reads a tool does right after a write to verify it. */
#define AN_LOG_MAX 64U
static uint8_t an_log_cmd[AN_LOG_MAX];
static uint32_t an_log_addr[AN_LOG_MAX];
static uint32_t an_log_len[AN_LOG_MAX];
static uint32_t an_log_n;

static void an_log(uint8_t cmd, uint32_t addr, uint32_t len)
{
	if (an_log_n < AN_LOG_MAX) {
		an_log_cmd[an_log_n] = cmd;
		an_log_addr[an_log_n] = addr;
		an_log_len[an_log_n] = len;
		an_log_n++;
	}
}

/* Session diagnostics. Nothing in the AN3155 loop may print while the
 * protocol is live: a printk between two ACKs goes out on the same UART and
 * the host tool reads it as the reply ("Got byte 0x0d instead of ACK", then
 * "Failed to write memory at address 0x10000000" -- the
 * only write block it ever sent was rejected that way). The loop collects its
 * findings here, and an3155_run() prints them once the console is back. */
static uint32_t an_diag_bad_addr;
static uint32_t an_diag_bad_frame;
static uint32_t an_diag_wm_fail;
static uint32_t an_diag_read_refused;
static uint32_t an_diag_marks;
static uint32_t an_diag_last_mark;
static uint32_t an_diag_go_addr;
static bool an_diag_goed;
static bool an_diag_timed_out;
static bool an_diag_half_frame;
static bool an_diag_reset;

/* Set by an_get() when a byte does not turn up in time: the frame that was
 * being read is dead, and the session has to end instead of waiting for the
 * rest of it. */
static bool an_aborted;

static void an_diag_flush(void)
{
	if (an_diag_reset) {
		printk("loader: AN3155 RESET (0xa2): console resumed, "
		       "use GO to publish+boot\n");
	}
	if (an_diag_timed_out) {
		printk("loader: AN3155 idle timeout\n");
	}
	if (an_diag_half_frame) {
		printk("loader: AN3155 session dropped mid-frame "
		       "(the host stopped sending)\n");
	}
	if (an_diag_goed) {
		printk("loader: AN3155 GO 0x%08x\n", an_diag_go_addr);
	}
	if (an_log_n != 0U) {
		printk("loader: AN3155 %u commands, %u write marks (last %u B), "
		       "%u bad addr, %u bad frames, %u write failures (last ret %d), "
		       "%u refused reads"
		       "\n",
		       an_log_n, an_diag_marks, an_diag_last_mark, an_diag_bad_addr,
		       an_diag_bad_frame, an_diag_wm_fail, an_last_wm_ret,
		       an_diag_read_refused);
	}
}

#define AN_ACK     0x79U
#define AN_NACK    0x1FU

#define AN_CMD_GET  0x00U
#define AN_CMD_GVR  0x01U
#define AN_CMD_GID  0x02U
#define AN_CMD_RM   0x11U
#define AN_CMD_WM   0x31U
#define AN_CMD_EE   0x44U
#define AN_CMD_GO   0x21U
#define AN_CMD_CRC  0xA1U

static void an_put(uint8_t b)
{
	agm_boot_console_write(NULL, &b, 1);
}

static uint8_t an_get(void)
{
	char c;
	int64_t from;

	/* A host that stops in the middle of a frame must not be able to hang
	 * the loader: the 10 s idle check only runs between commands, so a
	 * half-sent frame used to leave this loop spinning forever and only a
	 * reset brought the console back. Bound every
	 * byte, remember the abort and return 0xff so the frame parsers finish
	 * immediately (their checksums then fail, which is harmless: nobody is
	 * listening any more). */
	if (an_aborted) {
		return 0xffU;
	}
	from = k_uptime_get();
	/* Both bounds are needed. The ms check is the real one on hardware; the
	 * spin count is the backstop for targets where the clock does not advance
	 * while a busy loop runs (native_sim), which is exactly where the session
	 * loop's own AN_SPIN_LIMIT cannot help: while this function spins, control
	 * never gets back there . */
	for (uint32_t spin = 0U; agm_boot_console_poll(NULL, &c) != 0; spin++) {
		if (k_uptime_get() - from > AN_BYTE_TIMEOUT_MS ||
		    spin > AN_SPIN_LIMIT) {
			an_aborted = true;
			return 0xffU;
		}
		arch_nop();
	}
	return (uint8_t)c;
}

static void an_ack(void)
{
	an_put(AN_ACK);
}

static void an_nack(void)
{
	an_put(AN_NACK);
}

/* AN3155 sends a READ's byte count as N followed by its complement (~N).
 *
 * The option-byte window (0x81000000) branch below used to require the
 * *repeated* byte instead (n1 ^ nx == 0) -- the only frame shape in this file
 * that did, and a NACK there is exactly what makes the vendor tool report
 * "Flash: Protected" and refuse to write (see the comment at that branch).
 * Accept both shapes: the count is range-checked either way
 * and the trailing XOR over the payload still guards the frame, so a
 * desynchronised stream does not survive. Which shape the real tool sends is
 * worth capturing once (tools/test_uart_capture.sh + the `an-status` log);
 * after that this can go back to a single predicate. */
static bool an_count_ok(uint8_t n1, uint8_t nx)
{
	return ((uint8_t)(n1 ^ nx) == 0xFFU) || (nx == n1);
}

/* 4-byte address + XOR checksum. The address travels most-significant byte
 * first (the SDK converts it with CONVERT_TO_WORD, like the device id we
 * answer with): reading it little-endian turned the tool's 0x10000000 into
 * 0x00000010, which is what made every write fail the window check. */
static bool an_get_addr(uint32_t *addr)
{
	uint8_t b[5];
	uint8_t xor = 0;

	for (int i = 0; i < 5; i++) {
		b[i] = an_get();
		xor ^= b[i];
	}
	*addr = ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) |
		((uint32_t)b[2] << 8) | (uint32_t)b[3];
	return xor == 0U;
}

/* Payload length byte (N-1) + data + XOR over both. Returns false (and NACKs)
 * when the checksum does not match. */
static bool an_get_data(uint8_t *dst, uint32_t max, uint32_t *len)
{
	uint8_t n1 = an_get();
	uint32_t n = (uint32_t)n1 + 1U;
	uint8_t xor = n1;

	if (n > max) {
		an_nack();
		return false;
	}
	for (uint32_t i = 0; i < n; i++) {
		dst[i] = an_get();
		xor ^= dst[i];
	}
	if (xor != an_get()) {
		an_nack();
		return false;
	}
	*len = n;
	return true;
}

/* The flash window the vendor tool writes into (see the AN3155 section at the
 * top of the file). Both stores and the bitstream reservation live inside the
 * 1 MiB the tool knows as "Flash", so a plain `-w image -S <window>` transfers
 * a whole file. */
/* Resolve a host address to (store window, offset inside it). The windows tile
 * the on-die flash's address range in the order A, slot, B, bitstream (see the
 * driver's agm_boot_target_window_base()) and do not overlap, so the order
 * below only decides which one an address belongs to; the bitstream window is
 * checked first because it sits at the top of that range. Its size (and the
 * stores') is agm_boot_target_window_size(), i.e. the geometry clamped to what
 * the target accepts -- on the on-die layout that is store-max-size, not the
 * whole gap. */
/* AN3155 window index -> upload target. The two orders differ on purpose: the
 * windows are laid out like the real flash (A, slot, B, bitstream) while the
 * API numbers the targets A, B, slot, bitstream. */
static enum agm_boot_target an_target(uint32_t window)
{
	switch (window) {
	case 0U:
		return AGM_BOOT_TARGET_STORE_A;
	case 1U:
		return AGM_BOOT_TARGET_STORE_B;
	case BOOT_SLOTS:
		return AGM_BOOT_TARGET_BITSTREAM;
	default:
		return AGM_BOOT_TARGET_SLOT;
	}
}

static bool an_window(uint32_t addr, uint32_t *slot, uint32_t *off)
{
	if (addr >= AN_WIN_BS && addr < AN_WIN_BS + AN_WIN_BS_MAX) {
		*slot = BOOT_SLOTS; /* sentinel: bitstream staging */
		*off = addr - AN_WIN_BS;
		return true;
	}
	if (addr >= AN_WIN_SLOT && addr < AN_WIN_SLOT + AN_WIN_SLOT_MAX) {
		*slot = AN_WIN_ON_DIE; /* the internal flash itself */
		*off = addr - AN_WIN_SLOT;
		return true;
	}
	if (addr >= AN_WIN_A && addr < AN_WIN_A + AN_WIN_A_MAX) {
		*slot = 0U;
		*off = addr - AN_WIN_A;
		return true;
	}
	if (addr >= AN_WIN_B && addr < AN_WIN_B + AN_WIN_B_MAX) {
		*slot = 1U;
		*off = addr - AN_WIN_B;
		return true;
	}
	return false;
}

static int an_read(uint32_t slot, uint32_t off, uint8_t *buf, uint32_t n)
{
	return agm_boot_read(NULL, an_target(slot), off, buf, n);
}

static int an_write(uint32_t slot, uint32_t off, const uint8_t *buf, uint32_t n)
{
	return agm_boot_upload_write(NULL, an_target(slot), off, buf, n);
}

static int an_crc32(uint32_t slot, uint32_t off, uint32_t len, uint32_t *out)
{
	return agm_boot_upload_crc(NULL, an_target(slot), off, len, out);
}

static void an_get_info(uint8_t *vals, size_t *len)
{
	/* Same list the vendor bootloader advertises (boot.c
	 * getInfomationCmd()); the tool infers the protection state from it --
	 * a shorter list made it print "Flash: Protected" and refuse to
	 * write. */
	static const uint8_t cmds[] = {
		AN_CMD_GET, AN_CMD_GVR, AN_CMD_GID, AN_CMD_RM,  AN_CMD_WM,
		AN_CMD_EE,  AN_CMD_GO,	0x63U,	    0x73U,	0x82U,
		0x92U,	    0xA2U,	0xA3U,	    0x11U,
	};

	vals[0] = 0x20U; /* version, like the ROM loader */
	memcpy(&vals[1], cmds, sizeof(cmds));
	/* Version byte + the command list -- the old `sizeof(cmds)` dropped the
	 * last entry from the GET reply, which is 15 bytes written but 14
	 * reported. */
	*len = sizeof(cmds) + 1U;
}

static int an_go(uint32_t addr)
{
	uint32_t win;
	uint32_t off;
	uint32_t written;
	enum agm_boot_target target;
	int ret;

	if (!an_window(addr, &win, &off)) {
		return -EINVAL;
	}
	target = an_target(win);
	written = an_written_end[win];

	printk("\r\nloader: GO window=%u watermark=%u\r\n", win, written);

	if (written == 0U) {
		return -EINVAL;
	}

	/* Publishing is what the other hosts call `upload_finish`: the A/B
	 * stores and the on-die slot become TRIAL + active, the bitstream is
	 * staged (BSB1 header) and applied to the on-die reservation. */
	ret = agm_boot_upload_finish(NULL, target, written);
	if (ret < 0) {
		return ret;
	}

	if (target == AGM_BOOT_TARGET_BITSTREAM) {
		/* Applied; the new fabric only takes effect on the next boot. */
		return 0;
	}
	if (target == AGM_BOOT_TARGET_SLOT) {
		return agm_boot_boot(NULL, AGM_BOOT_TARGET_STORE_A);
	}
	return agm_boot_boot(NULL, win);
}

void agm_boot_an3155_run(const struct device *dev)
{
	static uint8_t buf[AGM_BOOT_UPLOAD_CHUNK];
	static uint8_t trace[64];
	static uint32_t trace_len;
	int64_t idle_from = k_uptime_get();
	uint32_t spins = 0U;

	ARG_UNUSED(dev);

	trace_len = 0U;
	trace[trace_len++] = AN_CMD_INIT;   /* the console consumed it */
	an_window_used = 0U;
	an_diag_bad_addr = 0U;
	an_diag_bad_frame = 0U;
	an_diag_wm_fail = 0U;
	an_diag_read_refused = 0U;
	an_diag_marks = 0U;
	an_diag_last_mark = 0U;
	an_diag_go_addr = 0U;
	an_diag_goed = false;
	an_diag_timed_out = false;
	an_diag_half_frame = false;
	an_diag_reset = false;
	an_aborted = false;
	an_log_n = 0U;

	/* The per-window write watermarks are session state: GO publishes them,
	 * and the fabric read allowance below ("only what *this* session wrote")
	 * is derived from them. Leaving them sticky would mean a session that
	 * only reads could read back whatever the previous session wrote -- and
	 * one fabric DFU would then leave the whole bitstream dumpable. */
	for (uint32_t i = 0U; i < AN_WIN_COUNT; i++) {
		an_written_end[i] = 0U;
	}

	an_ack(); /* the INIT byte is acknowledged by the command handler below */

	for (;;) {
		uint8_t cmd;

		/* The host tool abandons a session on error, and this loop has no
		 * other exit before GO, so a stale session would swallow the next
		 * invocation's traffic (the vendor tool reported a NACK
		 * on GET when a previous run had left the loader in this loop).
		 * Drop back to the console after a quiet period. */
		if (k_uptime_get() - idle_from > 10000) {
			an_diag_timed_out = true;
			break;
		}
		/* ... and the same on a spin count, because the clock above is only
		 * a wall clock: under native_sim nothing sleeps in this loop, the
		 * simulated time does not advance, and a session that misses its GO
		 * would spin there for ever (the native suite
		 * hung instead of timing out). */
		if (++spins > AN_SPIN_LIMIT) {
			an_diag_timed_out = true;
			break;
		}
		/* A frame that stopped in the middle is not an idle session: the
		 * host is gone, so drop back to the console now instead of
		 * waiting for the idle timeout with the parsers spinning. */
		if (an_aborted) {
			an_diag_half_frame = true;
			break;
		}
		if (agm_boot_console_poll(NULL, (char *)&cmd) != 0) {
			arch_nop();
			continue;
		}
		if (trace_len < sizeof(trace)) {
			trace[trace_len++] = cmd;
		}
		an_log(cmd, 0U, 0U);
		idle_from = k_uptime_get();

		/* stm32flash sends some command bytes together with their
		 * complement (a trace reads "01 fe 00 ff" = GVR 0x01 + ~0x01,
		 * then GET 0x00 + ~0x00). Consume it when it shows up right away;
		 * a byte that is not the complement is left for the next
		 * iteration. Not doing this made the complement look like an
		 * unknown command, which is the "NACK on command 0x00" the vendor
		 * tool reported. */
		{
			uint8_t want = (uint8_t)~cmd;
			int64_t pair_from = k_uptime_get();

			while (k_uptime_get() - pair_from < 20) {
				char nb;

				if (agm_boot_console_poll(NULL, &nb) == 0) {
					if ((uint8_t)nb == want) {
						if (trace_len < sizeof(trace)) {
							trace[trace_len++] = (uint8_t)nb;
						}
					}
					break;
				}
				arch_nop();
			}
		}
		/* slot/off stay uninitialised in the paths that reject an address
		 * before an_window() runs (the compiler cannot prove otherwise),
		 * so start them at zero. */
		uint32_t addr = 0U, len = 0U, slot = 0U, off = 0U;

		switch (cmd) {
		case AN_CMD_INIT:
			an_ack();
			break;

		case AN_CMD_GET: {
			uint8_t vals[32];
			size_t n;

			an_ack();
			an_get_info(vals, &n);
			an_put((uint8_t)(n - 1U));      /* N = count - 1 */
			agm_boot_console_write(NULL, vals, n);
			an_ack();
			break;
		}

		case AN_CMD_GVR: {
			/* version, then the RDP half-word as the SDK sends it
			 * (FLASH_OB->RDP, unprotected = RDP_WORD 0x5AA5); sending
			 * zeros made the tool print "Flash: Protected".
			 *
			 * This is the second place RDP is synthesised (the
			 * option-byte window read below is the first), so it
			 * honours CONFIG_BOOT_AGM_AN3155_CLAIM_UNPROTECTED too:
			 * with the switch off both answer "unknown" (0x0000)
			 * instead of claiming a protection state this CPU cannot
			 * read ( -- the claim used to be unconditional
			 * here, and the api_locked scenario's identity case proved
			 * it by expecting 0xA5 in the "honest" profile). */
			uint8_t rdp_lo = IS_ENABLED(CONFIG_BOOT_AGM_AN3155_CLAIM_UNPROTECTED)
						 ? 0xA5U
						 : 0x00U;
			uint8_t rdp_hi = IS_ENABLED(CONFIG_BOOT_AGM_AN3155_CLAIM_UNPROTECTED)
						 ? 0x5AU
						 : 0x00U;

			an_ack();
			an_put(0x20U);
			an_put(rdp_lo);   /* RDP word 0x5AA5, low byte first */
			an_put(rdp_hi);
			an_ack();
			break;
		}

		case AN_CMD_GID: {
			/* Same device id the on-die flash controller reports. The
			 * vendor's getDeviceIDCmd() uses sendValues(..., true), i.e.
			 * a leading N = count-1 before the data; without it the tool
			 * reads the first id byte as N and then trips over the last
			 * one ("Got byte 0x40 instead of ACK"). */
			/* The tool reads the four id bytes most significant first
			 * (the SDK's CONVERT_FROM_WORD): sending them in memory
			 * order made it report "Device ID: 0x1002040". */
			static const uint8_t id_be[4] = { 0x40U, 0x20U, 0x00U, 0x01U };

			an_ack();
			an_put(3U);   /* N = 4 - 1 */
			agm_boot_console_write(NULL, id_be, sizeof(id_be));
			an_ack();
			break;
		}

		case AN_CMD_RM:
			an_ack();
			if (!an_get_addr(&addr)) {
				an_nack();
				break;
			}
			an_log_addr[an_log_n - 1U] = addr;
			/* The option-byte window (FLASH_OPTION_BASE = 0x81000000,
			 * 128 B) is synthesised: the tool reads RDP from it and
			 * declares the part "Protected" when the read fails -- which
			 * is exactly what made it stop after a 4-byte probe write.
			 * Report an unprotected part (RDP 0xA5) plus the FPGA
			 * configuration pointer the bitstream lives at. */
			if (addr >= 0x81000000U && addr < 0x81000000U + 128U) {
				static uint8_t ob[128];
				uint32_t o = addr - 0x81000000U;
				uint32_t fpga = 0x800E7000U;
				uint32_t fpga_inv = ~fpga;

				memset(ob, 0, sizeof(ob));
				/* RDP. The CPU cannot read the real value (the
				 * option bytes are behind the AP/flash-controller
				 * path), so the honest answer is 0x00 = unknown:
				 * the vendor tool then stops before writing
				 * instead of getting a green light on a locked
				 * part. The dev board default keeps pretending
				 * 0xA5 ("unprotected") because that tool refuses
				 * to write at all otherwise -- see
				 * CONFIG_BOOT_AGM_AN3155_CLAIM_UNPROTECTED. */
				ob[0] = IS_ENABLED(CONFIG_BOOT_AGM_AN3155_CLAIM_UNPROTECTED)
						? 0xA5U
						: 0x00U;
				memcpy(&ob[0x30], &fpga, 4);
				memcpy(&ob[0x34], &fpga_inv, 4);

				an_ack();
				{
					uint8_t n1 = an_get();
					uint8_t nx = an_get();
					uint32_t n = (uint32_t)n1 + 1U;

					if (!an_count_ok(n1, nx) || n > sizeof(buf)) {
						an_nack();
						break;
					}
					an_ack();
					an_log_len[an_log_n - 1U] = n;
					for (uint32_t i = 0; i < n; i++) {
						buf[i] = (o + i < sizeof(ob)) ? ob[o + i] : 0U;
					}
					agm_boot_console_write(NULL, buf, n);
				}
				break;
			}
			if (!an_window(addr, &slot, &off)) {
				an_nack();
				break;
			}
			an_ack();
			{
				uint8_t n1 = an_get();
				uint8_t nx = an_get();
				uint32_t n = (uint32_t)n1 + 1U;
				int rc;

				an_log_len[an_log_n - 1U] = n;
				/* AN3155 sends the byte count as N followed by its
				 * complement; requiring N == ~N here rejected every
				 * read the tool asked for (the tool's
				 * "03 fc" = 4 bytes came back as NACK). */
				if (!an_count_ok(n1, nx) || n > sizeof(buf)) {
					an_nack();
					break;
				}
				/* The fabric window answers only for the bytes this
				 * session wrote: that is what the vendor tool's
				 * write-verify reads back, while a dump of the
				 * (licensed) bitstream would have to write over it
				 * first. See CONFIG_BOOT_AGM_AN3155_READ_FABRIC. */
				if (slot == BOOT_SLOTS &&
				    !IS_ENABLED(CONFIG_BOOT_AGM_AN3155_READ_FABRIC) &&
				    (off >= an_written_end[slot] ||
				     n > an_written_end[slot] - off)) {
					an_diag_read_refused++;
					an_nack();
					break;
				}
				an_ack();
				rc = an_read(slot, off, buf, n);
				if (rc < 0) {
					an_nack();
					break;
				}
				agm_boot_console_write(NULL, buf, n);
			}
			break;

		case AN_CMD_WM: {
			int ret;

			an_ack();
			bool addr_ok = an_get_addr(&addr);

			if (!addr_ok || !an_window(addr, &slot, &off)) {
				an_diag_bad_addr++;
				an_nack();
				break;
			}
			an_ack();
			if (!an_get_data(buf, AGM_BOOT_UPLOAD_CHUNK, &len)) {
				an_diag_bad_frame++;
				break;
			}
			an_log_addr[an_log_n - 1U] = addr;
			an_log_len[an_log_n - 1U] = len;
			/* An image always starts at offset 0: treat that as the
			 * start of a fresh upload, so a second image in the same
			 * session (no board reset in between) does not inherit the
			 * previous one's length -- GO publishes the watermark. */
			if (off == 0U) {
				an_window_used &= ~BIT(slot);
			}
			if ((an_window_used & BIT(slot)) == 0U) {
				/* First write into this window: erase from
				 * scratch and forget any earlier session's
				 * watermark (that one is GO's image length). */
				int begin_ret;

				an_window_used |= BIT(slot);
				an_written_end[slot] = 0U;
				/* Fresh upload into this window: the driver
				 * forgets its erase watermark and starts over.
				 *
				 * In a production-locked build this is gated: opening
				 * the session is itself the command that guards an
				 * erase, so the refusal has to be reported -- ignoring
				 * the return used to leave the frame looking accepted
				 * while every write behind it failed. */
				begin_ret = agm_boot_upload_begin(NULL, an_target(slot));
				if (begin_ret < 0) {
					an_window_used &= ~BIT(slot);
					an_diag_wm_fail++;
					an_last_wm_ret = begin_ret;
					an_last_wm_off = off;
					an_last_wm_len = len;
					an_nack();
					break;
				}
			}
			if (off + len > an_written_end[slot]) {
				an_written_end[slot] = off + len;
				if ((an_written_end[slot] & 0x3FFFU) < 256U) {
					an_diag_marks++;
					an_diag_last_mark = an_written_end[slot];
				}
			}
			ret = an_write(slot, off, buf, len);
			an_last_wm_ret = ret;
			an_last_wm_off = off;
			an_last_wm_len = len;
			if (ret < 0) {
				an_diag_wm_fail++;
				an_nack();
				break;
			}
			if (trace_len < sizeof(trace) - 1U) {
				/* remember the last accepted write for the trace */
			}
			an_ack();
			break;
		}

		case 0x63U: /* write protect   */
		case 0x73U: /* write unprotect */
		case 0x82U: /* read protect    */
		case 0x92U: /* read unprotect  */
		case 0xA3U: /* erase option bytes */
			/* Nothing to protect here (the images live in the external
			 * NOR behind our own records), so these are accepted no-ops:
			 * the tool needs them to consider the device writable. */
			an_ack();
			an_ack();
			break;

		case 0x43U: /* ERASE: N pages, 2 bytes each, plus XOR */
			an_ack();
			{
				uint8_t n1 = an_get();
				uint32_t bytes = ((uint32_t)n1 + 1U) * 2U + 1U;

				for (uint32_t i = 0; i < bytes; i++) {
					(void)an_get();
				}
				an_ack();
			}
			break;

		case 0xA2U: /* RESET */
			/* Acknowledge and stay in the session -- do *not* reboot:
			 * the tool sends this at the end of every session, and a
			 * real reboot would start the published image, after which
			 * the next AN3155 session has nothing to talk to (it
			 * the write succeeded but the follow-up read could not even
			 * initialise). Booting is explicit: GO, or the console's
			 * `reboot`.
			 *
			 * The session also must not print anything after this ACK:
			 * some tool flows (-O, for one) keep sending commands after
			 * the reset, and the first byte of a "loader: ..." line came
			 * back as "Got byte 0x6c instead of ACK". The diagnostics are flushed
			 * when the session
			 * really ends, i.e. on the idle timeout or on GO. */
			an_ack();
			an_diag_reset = true;
			break;

		case AN_CMD_EE:
			/* Extended erase. The frame is N (two bytes, most
			 * significant first), then N+1 page numbers (two bytes
			 * each) and the XOR checksum; N = 0xFFFF is the global
			 * erase and carries only the checksum. The tool erases
			 * pages 0x8000_0000.. before it writes, and a NACK here
			 * aborts the whole operation ("Sector-by-sector erase
			 * failed. It may be in protection mode."). Erasing stays lazy:
			 * up_store_write()
			 * erases each 4 KiB sector once before programming it, so
			 * the frame only has to be consumed. */
			an_ack();
			{
				uint8_t n_hi = an_get();
				uint8_t n_lo = an_get();
				uint32_t npages =
					((uint32_t)n_hi << 8 | n_lo) + 1U;
				uint8_t xor = n_hi ^ n_lo;
				uint32_t bytes;

				if (n_hi == 0xFFU && n_lo == 0xFFU) {
					(void)an_get();   /* checksum */
					an_ack();
					break;
				}
				/* Guard against a desynchronised stream: the
				 * whole 1 MiB window is 128 8 KiB pages. */
				if (npages > 256U) {
					an_nack();
					break;
				}
				bytes = npages * 2U + 1U;
				for (uint32_t i = 0; i < bytes; i++) {
					xor ^= an_get();
				}
				if (xor != 0U) {
					an_nack();
					break;
				}
				an_ack();
			}
			break;

		case AN_CMD_CRC:
			an_ack();
			if (!an_get_addr(&addr) || !an_window(addr, &slot, &off)) {
				an_nack();
				break;
			}
			an_ack();
			if (!an_get_addr(&len)) {
				an_nack();
				break;
			}
			{
				uint32_t crc = 0;

				if (an_crc32(slot, off, len, &crc) < 0) {
					an_nack();
					break;
				}
				an_ack();
				agm_boot_console_write(NULL, &crc, sizeof(crc));
			}
			break;

		case AN_CMD_GO:
			an_ack();
			if (!an_get_addr(&addr)) {
				an_nack();
				break;
			}
			an_ack();
			an_diag_go_addr = addr;
			an_diag_goed = true;
			if (an_go(addr) < 0) {
				an_diag_goed = false;
				an_nack();
				break;
			}
			an_diag_flush();
			return;

		default:
			an_nack();
			break;
		}
	}

	an_diag_flush();
}


void agm_boot_an3155_status_print(const struct device *dev)
{
	ARG_UNUSED(dev);

	for (uint32_t i = 0; i < AN_WIN_COUNT; i++) {
		printk("an-status: window %u watermark %u\n", i, an_written_end[i]);
	}
	printk("an-status: last WM ret=%d off=0x%x len=%u\n", an_last_wm_ret, an_last_wm_off,
	       an_last_wm_len);
	printk("an-status: %u commands:", an_log_n);
	for (uint32_t i = 0; i < an_log_n; i++) {
		printk(" %02x:%x+%x", an_log_cmd[i], an_log_addr[i], an_log_len[i]);
	}
	printk("\n");
}
