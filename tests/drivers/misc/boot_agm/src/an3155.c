/*
 * Copyright (c) 2026 AgRV Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief Protocol-layer test for the AN3155 server (agrv32flash's protocol).
 *
 * The server (drivers/misc/boot_agm_an3155.c) is driven through the real
 * console device -- a zephyr,uart-emul in this build -- so the framing rules
 * the vendor tool depends on are pinned against the code that implements them,
 * not against a reimplementation: command + complement byte, big-endian
 * addresses with an XOR, the GID count byte, the read-length N/!N pair, the
 * GID/GET payload shapes, and "GO on the bitstream window publishes the staged
 * image and ends the session".
 *
 * The server's corner cases had no test that did not need a board and the
 * vendor tool; this suite is that test.
 */

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/drivers/misc/boot_agm.h>
#include <zephyr/drivers/serial/uart_emul.h>
#include <zephyr/sys/crc.h>
#include <zephyr/sys/util.h>
#include <zephyr/ztest.h>

#include <string.h>

#include "test_auth.h"

#define BOOT_DEV    DEVICE_DT_GET(DT_NODELABEL(boot))
#define EMUL        DEVICE_DT_GET(DT_NODELABEL(emul_uart))
#define STORE_FLASH DEVICE_DT_GET(DT_NODELABEL(sim_store))
#define INT_FLASH   DEVICE_DT_GET(DT_NODELABEL(sim_ondie))

#define BOOT_NODE   DT_NODELABEL(boot)
#define T_FLASH_BASE ((uint32_t)DT_PROP(BOOT_NODE, on_die_flash_base))
/* The bitstream window is the slot that is not in use, and it *moves* as
 * updates alternate (the factory slot the option byte points at is never
 * written). Ask the driver for it rather than assuming slot 1; a test that
 * assumed a fixed slot 1 hung for the idle timeout as soon as an earlier case had
 * committed. */
#define T_BS_ADDR (agm_boot_target_window_base(NULL, AGM_BOOT_TARGET_BITSTREAM))
#define T_BS_OFF  (agm_boot_target_offset(NULL, AGM_BOOT_TARGET_BITSTREAM))

/* AN3155 opcodes this test uses (the server's own names are in its file). */
#define AN_ACK   0x79U
#define AN_NACK  0x1FU
#define AN_GET   0x00U
#define AN_GVR   0x01U
#define AN_GID   0x02U
#define AN_RM    0x11U
#define AN_WM    0x31U
#define AN_CRC   0xA1U
#define AN_GO    0x21U
#define AN_EE    0x44U

/* What the test sends and what the server should answer. The session has to end
 * with a successful GO (the only early exit the server has besides its 10 s
 * idle timeout), and GO on the bitstream window is the one that publishes
 * without jumping into the image. */
static uint8_t rx[512];
static uint32_t rx_len;
static uint8_t tx[512];
static uint32_t tx_len;

static void an_tx(const uint8_t *data, uint32_t len)
{
	zassert_true(rx_len + len <= sizeof(rx), "test frame buffer is too small");
	memcpy(rx + rx_len, data, len);
	rx_len += len;
}

static void an_tx_byte(uint8_t b)
{
	an_tx(&b, 1U);
}

static uint8_t xor_of(const uint8_t *data, uint32_t len)
{
	uint8_t x = 0U;

	for (uint32_t i = 0U; i < len; i++) {
		x ^= data[i];
	}
	return x;
}

/* Command + its complement, which is what the vendor tool sends and what the
 * server skips when it shows up right after the command byte. */
static void an_tx_cmd(uint8_t cmd)
{
	an_tx_byte(cmd);
	an_tx_byte((uint8_t)~cmd);
}

/* 4-byte address, most significant first, plus the XOR that makes the sum 0. */
static void an_tx_addr(uint32_t addr)
{
	uint8_t b[5] = { (uint8_t)(addr >> 24), (uint8_t)(addr >> 16),
			 (uint8_t)(addr >> 8), (uint8_t)addr, 0U };

	b[4] = xor_of(b, 4U);
	an_tx(b, sizeof(b));
}

/* Write-memory frame: N (= len - 1), the data, and the XOR over both. */
static void an_tx_wm(uint32_t addr, const uint8_t *data, uint32_t len)
{
	uint8_t n = (uint8_t)(len - 1U);

	an_tx_cmd(AN_WM);
	an_tx_addr(addr);
	an_tx_byte(n);
	an_tx(data, len);
	an_tx_byte(n ^ xor_of(data, len));
}

/* Read-memory: address, then N and its complement. */
static void an_tx_rm(uint32_t addr, uint32_t len)
{
	uint8_t n = (uint8_t)(len - 1U);

	an_tx_cmd(AN_RM);
	an_tx_addr(addr);
	an_tx_byte(n);
	an_tx_byte((uint8_t)~n);
}

static void an_tx_crc(uint32_t addr, uint32_t len)
{
	/* The CRC command carries the address and the length as two address
	 * frames (the length one is not an address, it just has the same shape). */
	an_tx_cmd(AN_CRC);
	an_tx_addr(addr);
	an_tx_addr(len);
}

static void an_tx_go(uint32_t addr)
{
	an_tx_cmd(AN_GO);
	an_tx_addr(addr);
}

/* Run one session: push what was queued into the console's RX fifo, let the
 * server consume it (a successful GO returns), then collect what it wrote. */
static void an_session(void)
{
	/* The console device is shared with printk/log output (that is what the
	 * "no printing while the protocol is live" rule is about), so start from
	 * an empty TX fifo: only what the server writes during the session is
	 * part of the reply. The RX fifo gets the same treatment: a session
	 * that ended early (a refused frame, the idle timeout) can leave bytes
	 * behind, and the next one would then see them as its own commands: two
	 * stale NACKs left in the buffer are read back as commands.
	 * in front of the identity session's answers. */
	uart_emul_flush_tx_data(EMUL);
	uart_emul_flush_rx_data(EMUL);
	zassert_equal(uart_emul_put_rx_data(EMUL, rx, rx_len), rx_len, "rx fifo");
	rx_len = 0U;

	agm_boot_an3155_run(BOOT_DEV);

	memset(tx, 0, sizeof(tx));
	tx_len = uart_emul_get_tx_data(EMUL, tx, sizeof(tx));
	uart_emul_flush_tx_data(EMUL);
}

/* Walk the reply the server wrote, in order: every protocol reply this test
 * expects has to line up byte for byte. */
static uint32_t pos;

static void expect(uint8_t want)
{
	zassert_true(pos < tx_len, "reply ran out at %u (wanted 0x%02x)", pos, want);
	zassert_equal(tx[pos], want, "reply[%u]: got 0x%02x, wanted 0x%02x", pos,
		      tx[pos], want);
	pos++;
}

static void expect_mem(const uint8_t *want, uint32_t len)
{
	zassert_true(pos + len <= tx_len, "reply ran out at %u (+%u)", pos, len);
	zassert_mem_equal(&tx[pos], want, len, "reply[%u..]", pos);
	pos += len;
}

/* Bytes whose content is the flash's, not the protocol's (the window may hold
 * whatever an earlier case wrote there). */
#if IS_ENABLED(CONFIG_BOOT_AGM_AN3155_READ_FABRIC)
static void expect_any(uint32_t len)
{
	zassert_true(pos + len <= tx_len, "reply ran out at %u (+%u)", pos, len);
	pos += len;
}
#endif

static void run_session(void)
{
	an_session();
	pos = 0U;
}

/* GVR's version byte and RDP half-word. This is the *second* place the server
 * synthesises RDP (the option-byte window read is the first), so it follows
 * CONFIG_BOOT_AGM_AN3155_CLAIM_UNPROTECTED: with the switch off both answer
 * "unknown" rather than claiming a protection state the CPU cannot read
 * ( -- this expectation used to be 0xA5 unconditionally, which
 * is how the api_locked scenario "proved" the lie was still there). */
static void expect_gvr(void)
{
	expect(AN_ACK);
	expect(0x20U);
#if IS_ENABLED(CONFIG_BOOT_AGM_AN3155_CLAIM_UNPROTECTED)
	expect(0xA5U);
	expect(0x5AU);
#else
	expect(0x00U);
	expect(0x00U);
#endif
	expect(AN_ACK);
}

/* A finished bitstream upload leaves a live session behind, and the window
 * base follows that pin rather than the record -- so a test that starts from
 * a pin left by an earlier case would build its frames for an address the
 * server resolves differently once the first write re-pins -- the same
 * trap the API side hit. Start every bitstream session from the
 * record's current inactive slot. */
static void bs_session_reset(void)
{
	agm_boot_upload_abort(BOOT_DEV, AGM_BOOT_TARGET_BITSTREAM);
}

/* INIT (acked by run()), GET, GVR, GID (twice, as the tool does) -- the identity
 * handshake the vendor tool performs before it writes anything. */
/* A production-locked build that cannot be updated in-band has to say so at
 * boot, not only when an operator tries . This case reads what
 * the driver printed during POST_KERNEL, which is why it has to run before any
 * session flushes the console -- the `test_000_` prefix does that (ztest runs
 * cases in name order, and this suite runs before the api suite's). */
ZTEST(boot_agm_an3155, test_000_locked_build_reports_that_it_cannot_be_updated)
{
	static uint8_t boot[512];
	uint32_t len = uart_emul_get_tx_data(EMUL, boot, sizeof(boot) - 1U);
	bool reported = false;

	boot[len] = '\0';
	zassert_true(len > 0U, "the driver printed something at init");

	{
		static const char needle[] = "production lock is on and no server can authorize";

		for (uint32_t i = 0U; i + sizeof(needle) - 1U <= len; i++) {
			if (memcmp(&boot[i], needle, sizeof(needle) - 1U) == 0) {
				reported = true;
				break;
			}
		}
	}

#if IS_ENABLED(CONFIG_BOOT_AGM_LOCK_PRODUCTION) && !IS_ENABLED(CONFIG_BOOT_AGM_SMP)
	/* This suite's locked scenario has no mcumgr server: nothing can sign a
	 * command for it, and the boot log has to be where an operator finds out
	 * (the alternative -- discovering it when an upload is refused -- needs
	 * physical access to fix). */
	zassert_true(reported, "the locked build has to report that it cannot be updated:\n%s",
		     (char *)boot);
#else
	/* The default profiles are updatable and must not claim otherwise. */
	zassert_false(reported, "an unlocked build has nothing to warn about");
#endif

	/* Either way the other line must not fire here: this build does carry a
	 * P-256 key in the locked scenario (the test's own), and the unlocked
	 * profiles have no lock at all. */
	zassert_true(strstr((char *)boot, "this build has no P-256 key") == NULL,
		     "the key warning is for builds without one:\n%s", (char *)boot);
}

ZTEST(boot_agm_an3155, test_identity_commands)
{
	/* A production-locked build cannot complete an AN3155 fabric update, and
	 * that is the point rather than an accident: the session's first write
	 * needs an ERASE grant and the GO needs a PUBLISH one ,
	 * and there is no way to inject the second authorize *inside* the
	 * session -- the console demultiplexes by the first byte, so an mcumgr
	 * frame cannot interleave with AN3155 ones. The production profile
	 * therefore compiles this server out; what the locked scenario
	 * would show here is only that the gate makes the path useless,
	 * which the API/SMP cases already cover. */
#if IS_ENABLED(CONFIG_BOOT_AGM_LOCK_PRODUCTION)
	ztest_test_skip();
#endif

	/* Start from the record's current inactive slot: a previous case that
	 * committed a bitstream leaves a session pin behind, and the server
	 * resolves addresses against that pin rather than the record -- the
	 * trap bs_session_reset() documents. Without this the GO at the end
	 * was refused (an NACK the case never looked at, because it only
	 * walks the bytes it expects). */
	bs_session_reset();

	/* What the GET reply carries: N, then N + 1 bytes whose first one is the
	 * version and the rest is the full command table -- 14 entries, the last
	 * one being read-memory (0x11). The server used to report one byte less
	 * than it wrote, which dropped that entry; the tool
	 * infers "unprotected" from the list, so its exact shape is
	 * load-bearing. */
	static const uint8_t get_reply[15] = {
		0x20U, AN_GET, AN_GVR, AN_GID, AN_RM, AN_WM, AN_EE, AN_GO,
		0x63U, 0x73U, 0x82U, 0x92U, 0xA2U, 0xA3U, 0x11U,
	};

	an_tx_cmd(AN_GET);
	an_tx_cmd(AN_GVR);
	an_tx_cmd(AN_GID);
	an_tx_cmd(AN_GVR);
	auth_before_write();
	an_tx_wm(T_BS_ADDR, (const uint8_t *)"ab", 2U);
	/* This GO publishes too (the writes above gave the window a
	 * watermark), so the locked profile needs the grant here as well --
	 * without it the case would still "pass" (it only walks the bytes it
	 * expects) while the publish behind it was refused. */
	an_tx_go(T_BS_ADDR);
	run_session();

	/* INIT ack, then GET: N = count - 1 followed by the list, then an ACK. The
	 * tool infers "unprotected" from this list, so its shape is load-bearing. */
	expect(AN_ACK);
	expect(AN_ACK);
	expect(14U); /* N = count - 1, i.e. 15 bytes follow */
	expect_mem(get_reply, sizeof(get_reply));
	expect(AN_ACK);

	/* GVR: version and the RDP halfword (see expect_gvr()). */
	expect_gvr();

	/* GID: N = 4 - 1, then the four id bytes most significant first. */
	expect(AN_ACK);
	expect(3U);
	expect(0x40U);
	expect(0x20U);
	expect(0x00U);
	expect(0x01U);
	expect(AN_ACK);

	/* The second GVR of the session, then the "ab" write and the GO that
	 * ends it. Those two are not walked byte for byte here: the loader
	 * prints the commit summary between the protocol's replies (it is the
	 * same UART), so the count of ACK bytes is not stable -- and counting
	 * only the bytes one expects is exactly what used to hide a refused
	 * GO behind an unchecked 0x1F. What this case asserts instead is
	 * stronger: the whole session answered, and *nothing* was refused. */
	expect_gvr();
	for (uint32_t i = 0U; i < tx_len; i++) {
		zassert_not_equal(tx[i], AN_NACK,
				  "reply[%u] is a refusal; the write and the GO have to "
				  "succeed (the console text around them is printable, so "
				  "0x1F can only be the protocol's)", i);
	}
}

/* A write into the bitstream window, a read-back of it, a CRC over it and GO:
 * the staged image has to reach the on-die reservation, and the session has to
 * end without jumping anywhere. */
ZTEST(boot_agm_an3155, test_write_read_crc_go_bitstream)
{
	/* A production-locked build cannot complete an AN3155 fabric update, and
	 * that is the point rather than an accident: the session's first write
	 * needs an ERASE grant and the GO needs a PUBLISH one ,
	 * and there is no way to inject the second authorize *inside* the
	 * session -- the console demultiplexes by the first byte, so an mcumgr
	 * frame cannot interleave with AN3155 ones. The production profile
	 * therefore compiles this server out; what the locked scenario
	 * would show here is only that the gate makes the path useless,
	 * which the API/SMP cases already cover. */
#if IS_ENABLED(CONFIG_BOOT_AGM_LOCK_PRODUCTION)
	ztest_test_skip();
#endif

	bs_session_reset();
	static const uint8_t payload[64] = {
		0xde, 0xad, 0xbe, 0xef, 0x01, 0x23, 0x45, 0x67,
		0x89, 0xab, 0xcd, 0xef, 0x10, 0x32, 0x54, 0x76,
	};
	uint8_t readback[64];
	uint32_t crc = crc32_ieee(payload, sizeof(payload));

	auth_before_write();
	an_tx_wm(T_BS_ADDR, payload, sizeof(payload));
	an_tx_rm(T_BS_ADDR, sizeof(payload));
	an_tx_crc(T_BS_ADDR, sizeof(payload));
	/* The GO below publishes the fabric, which the locked profile only lets
	 * through with an authorized command. The grant is device-wide, so it
	 * covers this session whichever way it was obtained (src/test_auth.h). */
	an_tx_go(T_BS_ADDR);
	run_session();

	expect(AN_ACK);                       /* INIT */
	expect(AN_ACK);                       /* WM: command */
	expect(AN_ACK);                       /* WM: address */
	expect(AN_ACK);                       /* WM: payload */
	expect(AN_ACK);                       /* read: command */
	expect(AN_ACK);                       /* read: address */
	expect(AN_ACK);                       /* read: length */
	expect_mem(payload, sizeof(payload)); /* the bytes the server sent back */
	/* CRC acknowledges the command, the address and the length, then sends the
	 * four bytes of the device's own CRC32 over the window. */
	expect(AN_ACK);
	expect(AN_ACK);
	expect(AN_ACK);
	expect_mem((const uint8_t *)&crc, sizeof(crc));
	expect(AN_ACK);                       /* GO */
	expect(AN_ACK);                       /* GO address */
	/* The session then prints its diagnostics (the console is shared), so
	 * anything after the last ACK is ignored. */

	/* GO committed it into the bitstream slot the window maps to. */
	zassert_ok(flash_read(INT_FLASH, T_BS_OFF, readback, sizeof(payload)));
	zassert_mem_equal(readback, payload, sizeof(payload), "bitstream in the slot");
}

/* A write frame with a broken XOR and a write to an address no window covers:
 * both have to be NACKed rather than acted on, and the session has to stay in
 * sync afterwards. */
ZTEST(boot_agm_an3155, test_bad_frames_are_nacked)
{
	/* A production-locked build cannot complete an AN3155 fabric update, and
	 * that is the point rather than an accident: the session's first write
	 * needs an ERASE grant and the GO needs a PUBLISH one ,
	 * and there is no way to inject the second authorize *inside* the
	 * session -- the console demultiplexes by the first byte, so an mcumgr
	 * frame cannot interleave with AN3155 ones. The production profile
	 * therefore compiles this server out; what the locked scenario
	 * would show here is only that the gate makes the path useless,
	 * which the API/SMP cases already cover. */
#if IS_ENABLED(CONFIG_BOOT_AGM_LOCK_PRODUCTION)
	ztest_test_skip();
#endif

	bs_session_reset();
	static const uint8_t junk[3] = { 0x11U, 0x22U, 0x33U };
	uint8_t readback[4];
	uint32_t nacks = 0U;

	/* A write whose payload checksum is wrong (should be 3 ^ 0x11 ^ 0x22 ^ 0x33),
	 * then a write to an address no window covers (the server decides that at
	 * the address, so the frame carries no payload), then a good write and the
	 * GO that ends the session. */
	an_tx_cmd(AN_WM);
	an_tx_addr(T_BS_ADDR);
	an_tx_byte(3U);
	an_tx(junk, sizeof(junk));
	an_tx_byte(0x00U);

	an_tx_cmd(AN_WM);
	an_tx_addr(0x10000000U);

	auth_before_write();
	an_tx_wm(T_BS_ADDR, (const uint8_t *)"z", 1U);
	/* Same as above: the GO publishes the fabric, which the locked profile
	 * only lets through with an authorized command. */
	an_tx_go(T_BS_ADDR);
	run_session();

	/* The session answered something (INIT is always acknowledged first) and
	 * exactly two frames were refused. */
	zassert_true(tx_len > 0U, "no reply at all");
	zassert_equal(tx[0], AN_ACK, "INIT is acknowledged");
	for (uint32_t i = 0U; i < tx_len; i++) {
		if (tx[i] == AN_NACK) {
			nacks++;
		}
	}
	zassert_equal(nacks, 2U, "one NACK per bad frame (got %u)", nacks);

	/* And the good write still went through, i.e. the session stayed in sync:
	 * GO committed it into the bitstream slot. */
	zassert_ok(flash_read(INT_FLASH, T_BS_OFF, readback, 1U));
	zassert_equal(readback[0], 'z', "the write after the bad frames was published");
}

/* Reading the fabric window *without* having written it in this session: that
 * is the vendor-tool "dump the bitstream" path (`agrv32flash -S <slot 1> -r
 * file`), and it is refused -- the window is licensed IP. The read-back a
 * write-verify does is covered by test_write_read_crc_go_bitstream above,
 * which is exactly the line this case must not cross. */
ZTEST(boot_agm_an3155, test_fabric_window_read_needs_a_write_first)
{
	bs_session_reset();
	an_tx_rm(T_BS_ADDR, 8U);
	run_session();

	expect(AN_ACK); /* INIT */
	expect(AN_ACK); /* read: command */
	expect(AN_ACK); /* read: address */
#if IS_ENABLED(CONFIG_BOOT_AGM_AN3155_READ_FABRIC)
	/* The diagnostic switch serves it: the same three ACKs, then data. */
	expect(AN_ACK); /* read: length */
	expect_any(8U); /* whatever the window holds */
#else
	expect(AN_NACK); /* read: length -- nothing was written to read back */
#endif
}

/* ... and even in a session that *did* write the window, the read stops at the
 * watermark: everything past it is NACKed, so dumping the bitstream means
 * writing over it. */
ZTEST(boot_agm_an3155, test_fabric_window_read_stops_at_the_write_watermark)
{
	/* A production-locked build cannot complete an AN3155 fabric update, and
	 * that is the point rather than an accident: the session's first write
	 * needs an ERASE grant and the GO needs a PUBLISH one ,
	 * and there is no way to inject the second authorize *inside* the
	 * session -- the console demultiplexes by the first byte, so an mcumgr
	 * frame cannot interleave with AN3155 ones. The production profile
	 * therefore compiles this server out; what the locked scenario
	 * would show here is only that the gate makes the path useless,
	 * which the API/SMP cases already cover. */
#if IS_ENABLED(CONFIG_BOOT_AGM_LOCK_PRODUCTION)
	ztest_test_skip();
#endif

	bs_session_reset();
	static const uint8_t payload[64] = {
		0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88,
	};

	auth_before_write();
	an_tx_wm(T_BS_ADDR, payload, sizeof(payload));
	an_tx_rm(T_BS_ADDR, sizeof(payload));         /* inside: served */
	an_tx_rm(T_BS_ADDR + sizeof(payload), 8U);    /* past it: refused */
	an_tx_go(T_BS_ADDR);
	run_session();

	expect(AN_ACK);                       /* INIT */
	expect(AN_ACK);                       /* WM */
	expect(AN_ACK);
	expect(AN_ACK);
	expect(AN_ACK);                       /* read: command */
	expect(AN_ACK);                       /* read: address */
	expect(AN_ACK);                       /* read: length */
	expect_mem(payload, sizeof(payload)); /* what the session wrote */
	expect(AN_ACK);                       /* second read: command */
	expect(AN_ACK);                       /* second read: address */
#if IS_ENABLED(CONFIG_BOOT_AGM_AN3155_READ_FABRIC)
	expect(AN_ACK);                       /* served as well ... */
	expect_any(8U);                       /* ... bytes and all */
#else
	expect(AN_NACK);                      /* past the watermark */
#endif
	expect(AN_ACK);                       /* GO */
	expect(AN_ACK);
}

/* The allowance does not survive the session that earned it: a later,
 * read-only session may not read back what a previous one wrote. Without
 * this, one ordinary fabric DFU (which writes the whole window) would leave
 * the bitstream dumpable until the next write. */
ZTEST(boot_agm_an3155, test_fabric_read_allowance_is_session_scoped)
{
	static const uint8_t payload[64] = {
		0xaa, 0xbb, 0xcc, 0xdd,
	};

	/* Session 1: write (and stop there -- no GO, so the window is left with
	 * exactly the bytes this session wrote). */
	bs_session_reset();
	auth_before_write();
	an_tx_wm(T_BS_ADDR, payload, sizeof(payload));
	run_session();
	expect(AN_ACK); /* INIT */
	expect(AN_ACK); /* WM command */
	expect(AN_ACK); /* WM address */
	expect(AN_ACK); /* WM payload */

	/* Session 2: the same offsets, but nothing was written *here*. */
	rx_len = 0U;
	an_tx_rm(T_BS_ADDR, sizeof(payload));
	run_session();

	expect(AN_ACK); /* INIT */
	expect(AN_ACK); /* read command */
	expect(AN_ACK); /* read address */
#if IS_ENABLED(CONFIG_BOOT_AGM_AN3155_READ_FABRIC)
	expect(AN_ACK); /* the diagnostic switch does not care */
#else
	expect(AN_NACK); /* nothing written in this session */
#endif
}

ZTEST_SUITE(boot_agm_an3155, NULL, NULL, NULL, NULL, NULL);
