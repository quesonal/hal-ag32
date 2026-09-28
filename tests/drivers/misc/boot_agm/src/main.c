/*
 * Copyright (c) 2026 AgRV Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief Public-API tests for the AgRV2K bootloader / DFU driver.
 *
 * Runs on native_sim against two zephyr,sim-flash devices (app.overlay), so
 * the storage paths of drivers/misc/boot_agm.c are covered without a board, a
 * probe or a bitstream: upload (any chunk size, including the sub-word carry),
 * CRC, publish into each of the four targets, the boot record, info, confirm,
 * rollback, erase, the store -> slot copy and the bitstream staging ->
 * apply -> read-back chain.
 *
 * What is deliberately *not* here, and is covered on the dev board instead
 * (samples/spi_boot_loader):
 *
 *   - the *jump* itself: agm_boot_policy_run() and agm_boot_boot() end in
 *     jump_to(), which executes the image, and there is nothing sensible to
 *     jump to here. The weak agm_boot_jump_hook() stops them right before
 *     that, so everything up to the jump *is* covered -- including the A/B
 *     attempt counting and the BAD/rollback transition in boot_ab_policy()
 *     (test_trial_attempts_exhaust_to_bad_in_one_commit);
 *   - the one-shot override (RTC backup registers, CONFIG_BOOT_AGM_ONESHOT=n
 *     in prj.conf);
 *   - the AN3155 / mcumgr servers (CONFIG_BOOT_AGM_AN3155=n here; the SMP
 *     group needs MCUmgr, which this test does not pull in);
 *   - the corrupt half of the "no usable record" branch: `cfg` is read once,
 *     at init, so reaching it would need a boot whose record is already
 *     corrupt -- that one is covered on the dev board instead. The blank
 *     half is this suite's first case,
 *     which is also why it has to stay first.
 */

/*
 * Harness note: the zephyr,sim-flash devices keep their contents in
 * `flash.bin` / `flash1.bin` *in the current working directory* on
 * native_sim, so a run has to start from a clean one. A leftover file holds a
 * published record, and the two cases that need the record to still be blank
 * (test_00_erase_on_a_virgin_record_is_a_no_op, test_00_publish_over_foreign_
 * record) fail -- which says nothing about the driver. twister gives every
 * configuration its own directory, so the CI gate is unaffected.
 */

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/drivers/misc/agm_bitstream.h>
#include <zephyr/drivers/misc/boot_agm.h>
#include <zephyr/sys/crc.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>
#include <zephyr/ztest.h>

#include <string.h>

#include "test_auth.h"

#define BOOT_DEV    DEVICE_DT_GET(DT_NODELABEL(boot))

/* ---- trial-boot chain: the driver's weak hooks, stubbed -------------- *
 *
 * The arm -> confirm -> promote chain talks to the backup domain, the IWDG
 * driver and the SoC reset-cause register. None of those exists under
 * native_sim, which is why it had no automated coverage at all. These
 * strong definitions replace all three with RAM. The
 * application's side of the handshake is the header's inline helper; the test
 * stands in for it by writing the echo the same way the helper does
 * (CONFIRM = TICKET).
 */
static uint16_t fake_bkp[AGM_RTC_BKP_DR_COUNT];
static bool fake_watchdog_on;
static uint32_t fake_jump_entry;
static uint32_t fake_jumps;

int agm_boot_bkp_read(uint32_t idx, uint16_t *val)
{
	if (idx >= AGM_RTC_BKP_DR_COUNT) {
		return -EINVAL;
	}
	*val = fake_bkp[idx];
	return 0;
}

int agm_boot_bkp_write(uint32_t idx, uint16_t val)
{
	if (idx >= AGM_RTC_BKP_DR_COUNT) {
		return -EINVAL;
	}
	fake_bkp[idx] = val;
	return 0;
}

int agm_boot_trial_watchdog_set(bool on)
{
	fake_watchdog_on = on;
	return 0;
}

/* The record's commit sequence, so the trial assertions can check that the
 * *encoded* value is what landed in the ticket register (boot_agm.c; internal,
 * like the hooks above). */
extern uint32_t boot_agm_record_seq(void);

uint32_t agm_boot_reset_cause_take(void)
{
	return 0U;
}

bool agm_boot_jump_hook(uint32_t entry)
{
	fake_jump_entry = entry;
	fake_jumps++;
	return true; /* a test never executes the payload */
}
#define STORE_FLASH DEVICE_DT_GET(DT_NODELABEL(sim_store))
#define INT_FLASH   DEVICE_DT_GET(DT_NODELABEL(sim_ondie))

/* The layout under test is the one app.overlay describes, read back from the
 * same node the driver uses, so editing the overlay keeps this in step. */
#define BOOT_NODE   DT_NODELABEL(boot)
#define T_STORE_A   ((uint32_t)DT_PROP(BOOT_NODE, store_a_offset))
#define T_STORE_B   ((uint32_t)DT_PROP(BOOT_NODE, store_b_offset))
#define T_STORE_MAX ((uint32_t)DT_PROP(BOOT_NODE, store_max_size))
#define T_SLOT_ADDR ((uint32_t)DT_PROP(BOOT_NODE, slot_address))
#define T_SLOT_SIZE ((uint32_t)DT_PROP(BOOT_NODE, slot_size))
#define T_FLASH_BASE ((uint32_t)DT_PROP(BOOT_NODE, on_die_flash_base))
#define T_SLOT_OFF  (T_SLOT_ADDR - T_FLASH_BASE)
#define T_BS_ADDR   ((uint32_t)DT_PROP(BOOT_NODE, bitstream_address))
#define T_BS_OFF    (T_BS_ADDR - T_FLASH_BASE)
/* Bitstream updates go into the slot that is not in use and are committed by
 * a boot record (include/zephyr/drivers/misc/agm_bitstream.h). The slot alternates as
 * updates commit, so ask the driver where the current window is; the record
 * sits at the SoC's fixed address. */
#define T_BITSTREAM_WIN_ADDR (agm_boot_target_window_base(NULL, AGM_BOOT_TARGET_BITSTREAM))
#define T_BITSTREAM_WIN_OFF  (agm_boot_target_offset(NULL, AGM_BOOT_TARGET_BITSTREAM))
#define T_BITSTREAM_RECORD_OFF (0x800e6000U - T_FLASH_BASE)
#define T_RECORD_OFF ((uint32_t)DT_PROP(BOOT_NODE, record_offset))
#define T_BS_REC_MAGIC 0x31525342U /* "BSR1" in the bitstream boot record */

/* Large enough to cross several 256 B copy chunks and the 4 KiB erase sector
 * of the store, small enough to keep the test fast. */
#define IMG_LEN 5000U

static uint8_t pattern[IMG_LEN];
static uint8_t readback[IMG_LEN];

static void fill_pattern(uint8_t seed)
{
	for (size_t i = 0; i < sizeof(pattern); i++) {
		pattern[i] = (uint8_t)(seed + i * 7U + (i >> 3) + (i >> 8));
	}
}

static void upload(enum agm_boot_target target, uint32_t len, uint32_t chunk)
{
	/* Opening the session is itself gated in the locked profile (the first
	 * write erases the target), so ask before the begin. A no-op elsewhere. */
	auth_before_write();
	zassert_ok(agm_boot_upload_begin(BOOT_DEV, target), "upload_begin(%s)",
		   agm_boot_target_name(target));

	for (uint32_t off = 0; off < len; off += chunk) {
		uint32_t n = MIN(chunk, len - off);

		zassert_ok(agm_boot_upload_write(BOOT_DEV, target, off, pattern + off, n),
			   "upload_write(%s, %u, %u)", agm_boot_target_name(target), off, n);
	}
}

/* ---- the signed commands of the locked profile --------------------- */
 *
 * CONFIG_BOOT_AGM_LOCK_PRODUCTION refuses to publish an upload or erase a
 * target until the host has authorized that command: it hands out a nonce
 * (agm_boot_nonce_get()) and the host returns an ECDSA P-256 signature over
 * `nonce || cmd || args_len || args` (agm_boot_authorize()). The three
 * profiles this suite runs in differ exactly here, so that the storage cases
 * below keep testing what they were written for:
 *
 *   api / api_fabric_read: no authorization exists, the calls are what they
 *                          always were (and authorize() says -ENOTSUP,
 *                          which the last case in this file checks);
 *   api_locked:            every publish and erase asks for a grant first.
 *
 * The key is a fixed P-256 pair: the private half is the 32 bytes 0x01..0x20
 * (a value no one would use for anything real), the public half is the
 * `agm_boot_pubkey` this binary is built with. The pair is checked against
 * itself at run time -- uECC_compute_public_key() from the private half has to
 * reproduce the compiled key -- so a pair that drifted apart fails as itself
 * instead of as a mysterious -EACCES.
 */
#if IS_ENABLED(CONFIG_BOOT_AGM_LOCK_PRODUCTION)
#include <tinycrypt/ecc.h>
#include <tinycrypt/ecc_dsa.h>
#include <tinycrypt/sha256.h>

static const uint8_t test_priv[32] = {
	0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
	0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10,
	0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18,
	0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f, 0x20,
};

/* X||Y of the curve point `test_priv * G`, computed once (the driver's weak
 * default is all zeroes, i.e. "no trusted key at all"). */
const uint8_t agm_boot_pubkey[64] = {
	0x51, 0x5c, 0x3d, 0x6e, 0xb9, 0xe3, 0x96, 0xb9,
	0x04, 0xd3, 0xfe, 0xca, 0x7f, 0x54, 0xfd, 0xcd,
	0x0c, 0xc1, 0xe9, 0x97, 0xbf, 0x37, 0x5d, 0xca,
	0x51, 0x5a, 0xd0, 0xa6, 0xc3, 0xb4, 0x03, 0x5f,
	0x45, 0x36, 0xbe, 0x3a, 0x50, 0xf3, 0x18, 0xfb,
	0xf9, 0xa5, 0x47, 0x59, 0x02, 0xa2, 0x21, 0x50,
	0x2b, 0xef, 0x0d, 0x57, 0xe0, 0x8c, 0x53, 0xb2,
	0xcc, 0x0a, 0x56, 0xf1, 0x7d, 0x9f, 0x93, 0x54,
};

/* The signed blob the device has to accept, built exactly as the header (and
 * the SMP group) describe it: the nonce it just handed out, the command byte,
 * the argument length little endian, then the arguments. */
static uint32_t auth_payload(const uint8_t *nonce, uint8_t cmd, const uint8_t *args,
			     uint32_t args_len, uint8_t *out, uint32_t out_size)
{
	uint32_t off = 0U;

	zassert_true(AGM_BOOT_NONCE_LEN + 3U + args_len <= out_size, "payload fits");
	memcpy(out + off, nonce, AGM_BOOT_NONCE_LEN);
	off += AGM_BOOT_NONCE_LEN;
	out[off++] = cmd;
	out[off++] = (uint8_t)args_len;
	out[off++] = (uint8_t)(args_len >> 8);
	if (args_len != 0U) {
		memcpy(out + off, args, args_len);
		off += args_len;
	}
	return off;
}

/* Sign `payload` with the test key, in the raw r||s form tinycrypt takes (the
 * device also accepts DER; the DER path is covered by the ecdsa suite's
 * containers). The host's RNG is tinycrypt's own /dev/urandom default. */
static void auth_sign_payload(const uint8_t *payload, uint32_t len, uint8_t sig[64])
{
	struct tc_sha256_state_struct sha;
	uint8_t hash[32];

	tc_sha256_init(&sha);
	tc_sha256_update(&sha, payload, len);
	tc_sha256_final(hash, &sha);

	zassert_equal(uECC_sign(test_priv, hash, sizeof(hash), sig, uECC_secp256r1()), 1,
		      "uECC_sign() for the test key");
}

/* Keep the two halves of the test key honest (see the section comment): the
 * private half above has to reproduce the `agm_boot_pubkey` this binary was
 * built with, so a pair that drifted apart fails here instead of showing up as
 * an unexplained AUTHORIZE -EACCES. Runs once. */
static void auth_check_key_pair(void)
{
	static bool checked;
	uint8_t pub[64];

	if (checked) {
		return;
	}
	zassert_equal(uECC_compute_public_key(test_priv, pub, uECC_secp256r1()), 1,
		      "uECC_compute_public_key()");
	zassert_mem_equal(pub, agm_boot_pubkey, sizeof(pub),
			  "agm_boot_pubkey is the test private key's public half");
	checked = true;
}

/* The general form: sign a command *with arguments* (the payload layout the
 * device rebuilds is `nonce || cmd || args_len || args`, so the arguments are
 * covered by the signature). boot_agm_test_authorize() below is the common
 * no-arguments case. */
void boot_agm_test_authorize_args(uint8_t cmd, const uint8_t *args, uint32_t args_len)
{
	uint8_t nonce[AGM_BOOT_NONCE_LEN];
	uint8_t payload[AGM_BOOT_NONCE_LEN + 3U + 64U];
	uint8_t sig[64];
	uint32_t len;

	zassert_true(args_len <= 64U, "the test's payload buffer is big enough");
	auth_check_key_pair();
	zassert_ok(agm_boot_nonce_get(BOOT_DEV, nonce), "nonce_get()");
	len = auth_payload(nonce, cmd, args, args_len, payload, sizeof(payload));
	auth_sign_payload(payload, len, sig);
	zassert_ok(agm_boot_authorize(BOOT_DEV, nonce, cmd, args, args_len, sig, sizeof(sig)),
		   "authorize(0x%02x, %u args)", cmd, args_len);
}

void boot_agm_test_authorize(uint8_t cmd)
{
	boot_agm_test_authorize_args(cmd, NULL, 0U);
}

/* The two state-changing calls the rest of the suite makes: the grant first in
 * the locked profile, the plain call in the other two. */
static int test_finish(enum agm_boot_target target, uint32_t len)
{
	boot_agm_test_authorize(AGM_BOOT_AUTH_CMD_PUBLISH);
	return agm_boot_upload_finish(BOOT_DEV, target, len);
}

static int test_finish_at(enum agm_boot_target target, uint32_t len, uint32_t load,
			  uint32_t entry)
{
	boot_agm_test_authorize(AGM_BOOT_AUTH_CMD_PUBLISH);
	return agm_boot_upload_finish_at(BOOT_DEV, target, len, load, entry);
}

static int test_erase(enum agm_boot_target target)
{
	boot_agm_test_authorize(AGM_BOOT_AUTH_CMD_ERASE);
	return agm_boot_erase(BOOT_DEV, target);
}
#else
static int test_finish(enum agm_boot_target target, uint32_t len)
{
	return agm_boot_upload_finish(BOOT_DEV, target, len);
}

static int test_finish_at(enum agm_boot_target target, uint32_t len, uint32_t load,
			  uint32_t entry)
{
	return agm_boot_upload_finish_at(BOOT_DEV, target, len, load, entry);
}

static int test_erase(enum agm_boot_target target)
{
	return agm_boot_erase(BOOT_DEV, target);
}
#endif

static void info_of(struct agm_boot_info *info)
{
	memset(info, 0, sizeof(*info));
	zassert_ok(agm_boot_info_get(BOOT_DEV, info), "info_get");
	zassert_true(info->record_valid, "the record should be valid here");
}

/* A board that has never published an image -- the record sector is still
 * erased -- is where every freshly flashed loader starts, and it used to be
 * the one state `image erase` answered with -EINVAL. "Drop this target's
 * image" is already true there, so all three targets have to answer "done"
 * and leave the sector as blank as they found it: writing a record just to
 * be allowed to drop what was never installed would change what the next
 * boot reads.
 *
 * This case has to run first: it is the only one in the suite that needs the
 * record to still be blank (the record is read once, at init). It is also
 * why `info_of()` (which insists on a valid record) is not used below.
 *
 * The corrupt-record half of the same branch cannot be reached from here --
 * `cfg` is read at init, so the record would have had to be corrupt before
 * the tests started. That one is covered on the dev board (write a bad magic
 * into 0x80018000, reset, erase -> -EINVAL / mgmt EINVAL).
 */
ZTEST(boot_agm_api, test_00_erase_on_a_virgin_record_is_a_no_op)
{
	struct agm_boot_info info;

	memset(&info, 0, sizeof(info));
	zassert_ok(agm_boot_info_get(BOOT_DEV, &info), "info_get");
	zassert_false(info.record_valid, "this case starts from a blank record");
	zassert_true(info.record_blank, "and blank is not the same as corrupt");

	zassert_ok(test_erase(AGM_BOOT_TARGET_STORE_A), "erase store A");
	zassert_ok(test_erase(AGM_BOOT_TARGET_STORE_B), "erase store B");
	zassert_ok(test_erase(AGM_BOOT_TARGET_SLOT), "erase the on-die slot");

	memset(&info, 0, sizeof(info));
	zassert_ok(agm_boot_info_get(BOOT_DEV, &info), "info_get");
	zassert_false(info.record_valid, "a no-op erase must not create a record");
	zassert_true(info.record_blank, "the record sector has to stay erased");
	zassert_equal(info.slot[0].state, AGM_BOOT_SLOT_EMPTY, "store A is empty");
	zassert_equal(info.slot[0].len, 0U, "and holds nothing");
}

/* Odd chunk sizes exercise the driver's 4-byte carry across writes: the SPI
 * NOR programs whole words while hosts (and this test) hand over anything. */
ZTEST(boot_agm_api, test_upload_store_a_odd_chunks)
{
	struct agm_boot_info info;
	uint32_t crc = crc32_ieee(pattern, IMG_LEN);

	fill_pattern(0x11);
	crc = crc32_ieee(pattern, IMG_LEN);

	upload(AGM_BOOT_TARGET_STORE_A, IMG_LEN, 7);
	zassert_ok(test_finish(AGM_BOOT_TARGET_STORE_A, IMG_LEN));

	info_of(&info);
	zassert_equal(info.active, 0U, "store A should be the active one");
	zassert_equal(info.slot[0].len, IMG_LEN);
	zassert_equal(info.slot[0].crc, crc);
	zassert_equal(info.slot[0].state, AGM_BOOT_SLOT_TRIAL);
	zassert_equal(info.slot[0].src, AGM_BOOT_SRC_STORE);
	zassert_equal(info.slot[0].offset, T_STORE_A);

	zassert_ok(flash_read(STORE_FLASH, T_STORE_A, readback, IMG_LEN));
	zassert_mem_equal(readback, pattern, IMG_LEN, "store A content");
}

/* The store -> slot copy: the record whose load is the slot base gets copied
 * into the application slot (this is what the boot path does before it
 * jumps). */
ZTEST(boot_agm_api, test_install_slot_copies_the_store)
{
	fill_pattern(0x22);

	upload(AGM_BOOT_TARGET_STORE_B, IMG_LEN, 1024);
	zassert_ok(test_finish(AGM_BOOT_TARGET_STORE_B, IMG_LEN));
	zassert_ok(agm_boot_install_slot(BOOT_DEV));

	zassert_ok(flash_read(INT_FLASH, T_SLOT_OFF, readback, IMG_LEN));
	zassert_mem_equal(readback, pattern, IMG_LEN, "on-die slot content");
}

/* On-die DFU: the image is already in the slot, so the record says
 * src = ON_DIE and install-slot() must refuse to "copy" over it (that used to
 * erase the image and program store A of the other flash on top). */
ZTEST(boot_agm_api, test_on_die_upload_and_install_slot_refusal)
{
	struct agm_boot_info info;

	fill_pattern(0x33);

	upload(AGM_BOOT_TARGET_SLOT, IMG_LEN, 256);
	zassert_ok(test_finish(AGM_BOOT_TARGET_SLOT, IMG_LEN));

	info_of(&info);
	zassert_equal(info.slot[0].src, AGM_BOOT_SRC_ON_DIE);
	zassert_equal(info.slot[0].state, AGM_BOOT_SLOT_TRIAL);
	zassert_equal(info.active, 0U);

	zassert_ok(flash_read(INT_FLASH, T_SLOT_OFF, readback, IMG_LEN));
	zassert_mem_equal(readback, pattern, IMG_LEN, "on-die slot content");

	zassert_equal(agm_boot_install_slot(BOOT_DEV), -EINVAL,
		      "install-slot must refuse an image that already runs from the slot");

	zassert_ok(flash_read(INT_FLASH, T_SLOT_OFF, readback, IMG_LEN));
	zassert_mem_equal(readback, pattern, IMG_LEN, "the image must still be there");
}

/* confirm/rollback move the record, not the flash. */
ZTEST(boot_agm_api, test_confirm_and_rollback)
{
	struct agm_boot_info info;

	fill_pattern(0x44);
	upload(AGM_BOOT_TARGET_STORE_B, IMG_LEN, 512);
	zassert_ok(test_finish(AGM_BOOT_TARGET_STORE_B, IMG_LEN));

	info_of(&info);
	zassert_equal(info.active, 1U);

	zassert_ok(agm_boot_confirm(BOOT_DEV));
	info_of(&info);
	zassert_equal(info.slot[1].state, AGM_BOOT_SLOT_CONFIRMED);
	zassert_equal(info.slot[1].attempts, 0U);

	zassert_ok(agm_boot_rollback(BOOT_DEV));
	info_of(&info);
	zassert_equal(info.active, 0U, "rollback should move to the other store");
	zassert_equal(info.slot[0].state, AGM_BOOT_SLOT_TRIAL);
}

ZTEST(boot_agm_api, test_erase_empties_the_record_and_the_flash)
{
	struct agm_boot_info info;

	fill_pattern(0x55);
	upload(AGM_BOOT_TARGET_SLOT, IMG_LEN, 1024);
	zassert_ok(test_finish(AGM_BOOT_TARGET_SLOT, IMG_LEN));
	info_of(&info);
	zassert_equal(info.slot[0].src, AGM_BOOT_SRC_ON_DIE);

	zassert_ok(test_erase(AGM_BOOT_TARGET_SLOT));
	info_of(&info);
	zassert_equal(info.slot[0].state, AGM_BOOT_SLOT_EMPTY);
	zassert_equal(info.slot[0].src, AGM_BOOT_SRC_STORE);
	zassert_equal(info.slot[0].len, 0U);

	zassert_ok(flash_read(INT_FLASH, T_SLOT_OFF, readback, IMG_LEN));
	for (size_t i = 0; i < IMG_LEN; i++) {
		zassert_equal(readback[i], 0xffU, "slot byte %u should be erased", i);
	}
}

/* A bitstream upload lands in the slot that is not running and commits it with
 * a boot record. The window alternates between updates, so the record is what
 * the assertions follow: it has to name the slot that holds exactly the bytes
 * the host sent, and the factory slot -- what an in-place update used to
 * overwrite, and what the flash option byte points at -- has to be untouched. */
ZTEST(boot_agm_api, test_bitstream_lands_in_the_inactive_slot)
{
	uint32_t rec[8];
	uint32_t factory_before[4];
	uint32_t factory_after[4];
	uint32_t crc;

	fill_pattern(0x66);
	crc = crc32_ieee(pattern, IMG_LEN);

	zassert_ok(flash_read(INT_FLASH, T_BS_OFF, factory_before, sizeof(factory_before)));

	upload(AGM_BOOT_TARGET_BITSTREAM, IMG_LEN, 256);
	zassert_ok(test_finish(AGM_BOOT_TARGET_BITSTREAM, IMG_LEN));

	zassert_ok(flash_read(INT_FLASH, T_BITSTREAM_RECORD_OFF, rec, sizeof(rec)));
	zassert_equal(rec[0], T_BS_REC_MAGIC, "record magic");
	zassert_equal(rec[3], IMG_LEN, "record length");
	zassert_equal(rec[4], crc, "record CRC");
	zassert_true(rec[2] == AGM_BITSTREAM_SLOT1_ADDR ||
			     rec[2] == AGM_BITSTREAM_SLOT2_ADDR,
		     "the record has to name one of the two update slots (got 0x%08x)",
		     rec[2]);

	/* The slot the record names holds the payload, byte for byte. */
	zassert_ok(flash_read(INT_FLASH, rec[2] - T_FLASH_BASE, readback, IMG_LEN));
	zassert_mem_equal(readback, pattern, IMG_LEN, "image in the committed slot");

	/* And the factory slot was not touched. */
	zassert_ok(flash_read(INT_FLASH, T_BS_OFF, factory_after, sizeof(factory_after)));
	zassert_mem_equal(factory_after, factory_before, sizeof(factory_after),
			  "the factory slot must never be written");
}

/* A TRIAL boot arms the watchdog and hands the application a ticket; the
 * application's echo on the next boot is what promotes the slot. */
ZTEST(boot_agm_api, test_trial_boot_arms_tickets_and_promotes)
{
	struct agm_boot_info info;

	memset(fake_bkp, 0, sizeof(fake_bkp));
	fake_watchdog_on = false;
	fake_jumps = 0U;

	fill_pattern(0x77);
	upload(AGM_BOOT_TARGET_STORE_A, IMG_LEN, 256);
	zassert_ok(test_finish(AGM_BOOT_TARGET_STORE_A, IMG_LEN));

	zassert_ok(agm_boot_boot(BOOT_DEV, 0U), "the hooked jump returns");
	zassert_true(fake_watchdog_on, "a TRIAL boot arms the watchdog");
	zassert_not_equal(fake_bkp[AGM_BOOT_TRIAL_TICKET_DR], 0U,
			  "a TRIAL boot hands out a ticket");
	/* ... and it is the *encoded* sequence, which is what makes "never 0"
	 * hold for every sequence instead of all-but-one. */
	zassert_equal(fake_bkp[AGM_BOOT_TRIAL_TICKET_DR],
		      agm_boot_trial_ticket_encode(boot_agm_record_seq()),
		      "the ticket is the encoded record sequence");
	zassert_equal(fake_jumps, 1U, "the payload was entered once");
	zassert_equal(fake_jump_entry, T_SLOT_ADDR, "the entry comes from the record");

	/* The application answers: echo the ticket (what the header's inline
	 * helper does on a board). */
	fake_bkp[AGM_BOOT_TRIAL_CONFIRM_DR] = fake_bkp[AGM_BOOT_TRIAL_TICKET_DR];

	/* Next boot: the promotion happens before anything is booted. */
	zassert_ok(agm_boot_boot(BOOT_DEV, 0U));
	zassert_ok(agm_boot_info_get(BOOT_DEV, &info));
	zassert_equal(info.slot[0].state, AGM_BOOT_SLOT_CONFIRMED,
		      "an image that confirmed itself becomes permanent");
	zassert_equal(fake_bkp[AGM_BOOT_TRIAL_CONFIRM_DR], 0U, "the echo is consumed");
	zassert_equal(fake_bkp[AGM_BOOT_TRIAL_TICKET_DR], 0U, "the ticket is consumed");
	zassert_false(fake_watchdog_on, "a CONFIRMED boot is not put on a watchdog");
}

/* An echo that does not match the ticket on the table must not promote
 * anything -- that is the property the ticket's sequence buys. */
ZTEST(boot_agm_api, test_stale_confirm_does_not_promote)
{
	struct agm_boot_info info;

	fill_pattern(0x78);
	upload(AGM_BOOT_TARGET_STORE_B, IMG_LEN, 256);
	zassert_ok(test_finish(AGM_BOOT_TARGET_STORE_B, IMG_LEN));

	/* Somebody else's answer: the pair agrees with itself, not with the
	 * ticket the loader is about to hand out. */
	fake_bkp[AGM_BOOT_TRIAL_TICKET_DR] = 0x1234U;
	fake_bkp[AGM_BOOT_TRIAL_CONFIRM_DR] = 0x1234U;

	zassert_ok(agm_boot_boot(BOOT_DEV, 1U));
	zassert_ok(agm_boot_info_get(BOOT_DEV, &info));
	zassert_equal(info.slot[1].state, AGM_BOOT_SLOT_TRIAL,
		      "a stale echo must not confirm the image that just booted");
}

/* Trial attempts are spent one per boot until the store is marked BAD and the
 * loader rolls back to the other one (include/zephyr/drivers/misc/boot_agm.h
 * AGM_BOOT_MAX_ATTEMPTS).
 *
 * The last attempt has to be committed *together with* that outcome: writing
 * "attempt N/M" first and "BAD"/"rollback" after leaves a window in which a
 * failing record write keeps the store in TRIAL with attempts == MAX, and
 * every later boot then spends another attempt and writes again -- a
 * cross-reboot increment nothing currently owns. The record's commit
 * sequence is the observable that tells the two implementations apart: one
 * entry for the last attempt, not two.
 *
 * Also pinned: a store the policy has marked BAD is never entered again -- the
 * next policy run streams the *other* store, which the slot contents show. */
ZTEST(boot_agm_api, test_trial_attempts_exhaust_to_bad_in_one_commit)
{
	struct agm_boot_info info;
	uint32_t seq;

	/* Store B first (with its own pattern), then store A, so the record ends
	 * up pointing at A -- the trial the loop below spends. */
	fill_pattern(0x21);
	upload(AGM_BOOT_TARGET_STORE_B, IMG_LEN, 512);
	zassert_ok(test_finish(AGM_BOOT_TARGET_STORE_B, IMG_LEN));

	fill_pattern(0x11);
	upload(AGM_BOOT_TARGET_STORE_A, IMG_LEN, 512);
	zassert_ok(test_finish(AGM_BOOT_TARGET_STORE_A, IMG_LEN));

	info_of(&info);
	zassert_equal(info.active, 0U, "the fresh trial is the active store");
	zassert_equal(info.slot[0].state, AGM_BOOT_SLOT_TRIAL, "A is a trial");
	zassert_equal(info.slot[0].attempts, 0U, "with no attempts spent yet");

	/* Every attempt but the last is one record entry. */
	for (uint32_t i = 1U; i < AGM_BOOT_MAX_ATTEMPTS; i++) {
		seq = boot_agm_record_seq();
		zassert_ok(agm_boot_policy_run(BOOT_DEV, NULL), "policy run %u", i);

		info_of(&info);
		zassert_equal(info.slot[0].attempts, i, "attempt %u recorded", i);
		zassert_equal(info.slot[0].state, AGM_BOOT_SLOT_TRIAL, "still on trial");
		zassert_equal(boot_agm_record_seq(), seq + 1U,
			      "one attempt is one record entry (attempt %u)", i);
	}

	/* The last attempt: attempts -> MAX, store -> BAD, active -> B, in ONE
	 * commit. */
	seq = boot_agm_record_seq();
	zassert_ok(agm_boot_policy_run(BOOT_DEV, NULL), "the last policy run");

	info_of(&info);
	zassert_equal(info.slot[0].attempts, AGM_BOOT_MAX_ATTEMPTS,
		      "the last attempt is recorded");
	zassert_equal(info.slot[0].state, AGM_BOOT_SLOT_BAD,
		      "exhausting the budget marks the store bad");
	zassert_equal(info.active, 1U, "and the loader rolls back to the other store");
	zassert_equal(boot_agm_record_seq(), seq + 1U,
		      "the last attempt and the BAD/rollback are a single record entry");

	/* A BAD store is never entered again: the next run has to stream store B,
	 * which is what the on-die slot ends up holding. */
	fill_pattern(0x21); /* B's payload, the one a rollback boot has to deliver */
	zassert_ok(agm_boot_policy_run(BOOT_DEV, NULL), "boot after the rollback");
	zassert_ok(flash_read(INT_FLASH, T_SLOT_OFF, readback, IMG_LEN));
	zassert_mem_equal(readback, pattern, IMG_LEN,
			  "the rollback streamed store B, not the bad store A");
	info_of(&info);
	zassert_equal(info.slot[0].state, AGM_BOOT_SLOT_BAD, "A stays bad");
}

/* The ticket is the record's commit sequence in 16 bits, and 0 is the
 * protocol's "nothing is pending": the application's agm_boot_trial_confirm()
 * answers -ENOENT on it and the loader requires a non-zero ticket before it
 * promotes anything. The low half of a 32-bit counter is 0 once every 65536
 * commits, so without an encoding that one boot could never be confirmed:
 * a healthy trial image would burn its attempts and be marked bad (the risk
 * runs the other way from what one might first suspect: a false negative
 * rather than a stale echo promoting the wrong image). */
ZTEST(boot_agm_api, test_trial_ticket_never_encodes_to_zero)
{
	/* The colliding sequences: the low 16 bits are 0. */
	zassert_equal(agm_boot_trial_ticket_encode(0x00000000U), 0xFFFFU);
	zassert_equal(agm_boot_trial_ticket_encode(0x00010000U), 0xFFFFU);
	zassert_equal(agm_boot_trial_ticket_encode(0x12340000U), 0xFFFFU);

	/* Everything else is the low half, unchanged: the application only
	 * echoes what it reads, so the wire value must not move for the
	 * sequences that are already unambiguous. */
	zassert_equal(agm_boot_trial_ticket_encode(0x00000001U), 0x0001U);
	zassert_equal(agm_boot_trial_ticket_encode(0x0000FFFFU), 0xFFFFU);
	zassert_equal(agm_boot_trial_ticket_encode(0xDEADBEEFU), 0xBEEFU);
}

/* The record carries a CRC over its own fields, so a torn write that happens
 * to land on a plausible magic/slot/len/seq is still ignored.
 * The record names which slot to stream, and the only public observable of
 * that is which slot the *window* points at: a record naming slot 1 must move
 * the window to slot 2, and the same record with a broken CRC must leave the
 * window on slot 1 (i.e. the reader fell back to the factory slot). */
ZTEST(boot_agm_api, test_bitstream_record_crc_gates_the_slot)
{
	struct agm_bitstream_record rec;
	uint32_t window;

	/* No upload session may be pinned: a live session makes the window
	 * follow the session, not the record. */
	agm_boot_upload_abort(BOOT_DEV, AGM_BOOT_TARGET_BITSTREAM);

	memset(&rec, 0, sizeof(rec));
	rec.magic = 0x31525342U;        /* AGM_BITSTREAM_RECORD_MAGIC */
	rec.seq = 1U;
	rec.slot = AGM_BITSTREAM_SLOT1_ADDR;    /* names slot 1 */
	rec.len = 1000U;
	rec.rec_crc = agm_bitstream_record_crc((const volatile uint32_t *)&rec);

	zassert_ok(flash_erase(INT_FLASH, T_BITSTREAM_RECORD_OFF, 4096U));
	zassert_ok(flash_write(INT_FLASH, T_BITSTREAM_RECORD_OFF, &rec, sizeof(rec)));
	zassert_ok(flash_write(INT_FLASH, T_BITSTREAM_RECORD_OFF + 32U, &rec, sizeof(rec)));
	window = agm_boot_target_window_base(NULL, AGM_BOOT_TARGET_BITSTREAM);
	zassert_equal(window, AGM_BITSTREAM_SLOT2_ADDR,
		      "a whole record naming slot 1 must be believed (window 0x%08x)", window);

	/* Same record, one bit flipped in its own CRC. */
	rec.rec_crc ^= 1U;
	zassert_ok(flash_erase(INT_FLASH, T_BITSTREAM_RECORD_OFF, 4096U));
	zassert_ok(flash_write(INT_FLASH, T_BITSTREAM_RECORD_OFF, &rec, sizeof(rec)));
	zassert_ok(flash_write(INT_FLASH, T_BITSTREAM_RECORD_OFF + 32U, &rec, sizeof(rec)));
	window = agm_boot_target_window_base(NULL, AGM_BOOT_TARGET_BITSTREAM);
	zassert_equal(window, AGM_BITSTREAM_SLOT1_ADDR,
		      "a record with a bad CRC must be ignored (window 0x%08x)", window);

	/* Leave the record erased: the rest of the suite starts from the factory. */
	zassert_ok(flash_erase(INT_FLASH, T_BITSTREAM_RECORD_OFF, 4096U));
}

/* The bounds the review turned up: a wrapped offset used to pass the
 * `off + len > max` check and erase the record sector, a gap in the write
 * stream, a length that does not fit, and a misaligned entry. */
ZTEST(boot_agm_api, test_rejects_out_of_range_and_misaligned)
{
	struct agm_boot_info before;
	struct agm_boot_info after;
	uint32_t crc = 0U;

	info_of(&before);

	auth_before_write();
	zassert_ok(agm_boot_upload_begin(BOOT_DEV, AGM_BOOT_TARGET_STORE_A));

	zassert_equal(agm_boot_upload_write(BOOT_DEV, AGM_BOOT_TARGET_STORE_A,
					    0xfffffffcU, pattern, 4U),
		      -EINVAL, "a wrapped offset must be rejected");
	zassert_equal(agm_boot_upload_write(BOOT_DEV, AGM_BOOT_TARGET_STORE_A, 0U,
					    pattern, T_STORE_MAX + 1U),
		      -EINVAL, "a write past the store must be rejected");
	zassert_ok(agm_boot_upload_write(BOOT_DEV, AGM_BOOT_TARGET_STORE_A, 0U,
					 pattern, 16U));
	zassert_equal(agm_boot_upload_write(BOOT_DEV, AGM_BOOT_TARGET_STORE_A, 32U,
					    pattern, 16U),
		      -EINVAL, "a gap in the write stream must be rejected");
	zassert_equal(test_finish(AGM_BOOT_TARGET_STORE_A, 0U),
		      -EINVAL, "finishing nothing must be rejected");
	zassert_equal(test_finish_at(AGM_BOOT_TARGET_SLOT, 16U,
						T_SLOT_ADDR, T_SLOT_ADDR + 2U),
		      -EINVAL, "a misaligned entry must be rejected");
	zassert_equal(agm_boot_upload_begin(BOOT_DEV, AGM_BOOT_TARGET_COUNT), -EINVAL);
	zassert_equal(agm_boot_read(BOOT_DEV, AGM_BOOT_TARGET_SLOT, 0U, readback,
				    T_SLOT_SIZE + 1U),
		      -EINVAL, "a read past the target must be rejected");
	zassert_equal(agm_boot_upload_crc(BOOT_DEV, AGM_BOOT_TARGET_SLOT, 0U,
					  T_SLOT_SIZE + 1U, &crc),
		      -EINVAL, "a CRC past the target must be rejected");

	agm_boot_upload_abort(BOOT_DEV, AGM_BOOT_TARGET_STORE_A);

	/* None of that may have touched the record (the wrapped offset used to
	 * land the erase on sector 0). */
	info_of(&after);
	zassert_equal(after.active, before.active);
	zassert_equal(after.slot[0].crc, before.slot[0].crc);
	zassert_equal(after.slot[1].crc, before.slot[1].crc);
}

/* The record is an append log in two sectors: each commit appends an entry
 * and the sector is only erased once it
 * is full, which is what makes a torn write survivable and the per-sector wear
 * ~1/46th of "erase + write per commit". Push it across that boundary several
 * times and check the record is still consistent -- the free function here is
 * agm_boot_confirm(), which commits on every call. */
ZTEST(boot_agm_api, test_record_log_recycles_sectors)
{
	struct agm_boot_info info;
	const uint32_t commits = 200U; /* 4 KiB / sizeof(entry) = 46 per sector */

	fill_pattern(0x77);
	upload(AGM_BOOT_TARGET_STORE_A, IMG_LEN, 512);
	zassert_ok(test_finish(AGM_BOOT_TARGET_STORE_A, IMG_LEN));

	for (uint32_t i = 0U; i < commits; i++) {
		zassert_ok(agm_boot_confirm(BOOT_DEV), "commit %u", i);
	}

	info_of(&info);
	zassert_equal(info.slot[0].len, IMG_LEN, "the image record survived the log");
	zassert_equal(info.slot[0].crc, crc32_ieee(pattern, IMG_LEN));
	zassert_equal(info.slot[0].state, AGM_BOOT_SLOT_CONFIRMED);

	zassert_ok(flash_read(STORE_FLASH, T_STORE_A, readback, 64U));
	zassert_mem_equal(readback, pattern, 64U, "store A is still where the record says");

	/* And the log still works afterwards. */
	fill_pattern(0x78);
	upload(AGM_BOOT_TARGET_STORE_B, IMG_LEN, 512);
	zassert_ok(test_finish(AGM_BOOT_TARGET_STORE_B, IMG_LEN));
	info_of(&info);
	zassert_equal(info.active, 1U);
	zassert_equal(info.slot[1].crc, crc32_ieee(pattern, IMG_LEN));
}

/* A board whose record sector holds something that is not a record (another
 * tool wrote there, or a record in an older layout) must still be publishable:
 * the first append erases the sector instead of writing over whatever is there
 * -- NOR can only clear bits. Found on the dev board, where the record
 * sector still held 0xdeadbeef from a 3.26.22 test and the first upload failed
 * with "flash write/verify failed".
 *
 * The bytes below are a *version 3* header followed by garbage: this build
 * does not import older layouts any more, so
 * an old record has to behave exactly like any other foreign one: report
 * invalid, boot nothing, and let the next upload start a fresh log. The
 * common parts of that (magic, version) are what a stale loader's record
 * would really carry, so a future import that half-parses it would show up
 * here as a failed publish instead of as a silent migration.
 *
 * This has to be the suite's first case: the driver reads the record once at
 * init, so only then is its log "fresh" (nothing valid, position 0), which is
 * the state the dev board had. The precondition is asserted rather than assumed, so
 * a change in test order fails here instead of passing without testing. */
ZTEST(boot_agm_api, test_00_publish_over_foreign_record)
{
	/* "SBC1" (little-endian magic) + version 3 + whatever a v3 record held
	 * where v4 keeps its sequence number. */
	static const uint8_t old_record[16] = {
		0x53U, 0x42U, 0x43U, 0x31U, /* magic */
		0x03U, 0x00U, 0x00U, 0x00U, /* version 3 */
		0x07U, 0x00U, 0x00U, 0x00U, /* a stale sequence number */
		0x00U, 0x00U, 0x00U, 0x00U,
	};
	struct agm_boot_info info;

	zassert_ok(agm_boot_info_get(BOOT_DEV, &info));
	zassert_false(info.record_valid, "this case needs the untouched post-init state");

	/* Put the stale bytes where a record used to live (its first sector) and
	 * publish over them, as the dev board did. */
	zassert_ok(flash_erase(STORE_FLASH, T_RECORD_OFF,
			       (uint32_t)DT_PROP(BOOT_NODE, record_size)));
	zassert_ok(flash_write(STORE_FLASH, T_RECORD_OFF, old_record, sizeof(old_record)));

	fill_pattern(0x99);
	upload(AGM_BOOT_TARGET_STORE_A, IMG_LEN, 512);
	zassert_ok(test_finish(AGM_BOOT_TARGET_STORE_A, IMG_LEN),
		   "publishing over a corrupt record has to work");

	info_of(&info);
	zassert_equal(info.slot[0].len, IMG_LEN);
	zassert_equal(info.slot[0].crc, crc32_ieee(pattern, IMG_LEN));
}

/* ---- the gate itself (four cases, run in every profile) ------------- */
 *
 * What the locked profile changes for a host: a publish or an erase without a
 * signature is refused *and changes nothing*, the same call with one is
 * allowed, and the nonce that authorized it cannot be replayed. The unlocked
 * profiles assert the opposite half of the same contract -- no authorization
 * exists there, so `erase` just runs and `authorize()` has no verifier to
 * reach (-ENOTSUP) -- which is what "the default profile is unchanged" means
 * in a native test.
 */
/* Opening the session is gated as well: without it, an
 * unauthenticated host could wipe a published store -- the first write of an
 * upload erases the target's sectors, and the record would keep pointing at
 * the image that is no longer there, so the `erase` gate would be trivially
 * bypassed by "uploading" less. This case pins both halves: the refusal, and
 * that the flash really is untouched. */
ZTEST(boot_agm_api, test_zz0_unsigned_upload_cannot_wipe_a_store)
{
	uint8_t before[IMG_LEN];

	/* A published image in store B, read back from the flash. */
	fill_pattern(0x4d);
	upload(AGM_BOOT_TARGET_STORE_B, IMG_LEN, 512);
	zassert_ok(test_finish(AGM_BOOT_TARGET_STORE_B, IMG_LEN));
	zassert_ok(flash_read(STORE_FLASH, T_STORE_B, before, sizeof(before)));

#if IS_ENABLED(CONFIG_BOOT_AGM_LOCK_PRODUCTION)
	struct agm_boot_info info;

	zassert_equal(agm_boot_upload_begin(BOOT_DEV, AGM_BOOT_TARGET_STORE_B),
		      AGM_BOOT_E_UNAUTHORIZED, "an unsigned upload session has to be refused");
	zassert_equal(agm_boot_upload_write(BOOT_DEV, AGM_BOOT_TARGET_STORE_B, 0U,
					    pattern, 64U),
		      -EINVAL, "and its writes have nowhere to go");
	zassert_ok(flash_read(STORE_FLASH, T_STORE_B, readback, sizeof(readback)));
	zassert_mem_equal(readback, before, sizeof(before),
			  "store B still holds the published image, byte for byte");
	info_of(&info);
	zassert_equal(info.slot[1].len, IMG_LEN, "and the record still points at it");
	zassert_equal(info.slot[1].crc, crc32_ieee(before, sizeof(before)));
#else
	/* The default profile is unchanged: the same call opens and writes. */
	zassert_ok(agm_boot_upload_begin(BOOT_DEV, AGM_BOOT_TARGET_STORE_B),
		   "an unlocked build opens without a grant");
	zassert_ok(agm_boot_upload_write(BOOT_DEV, AGM_BOOT_TARGET_STORE_B, 0U, pattern, 64U));
#endif
}

ZTEST(boot_agm_api, test_zz1_unsigned_state_change_is_refused)
{
	struct agm_boot_info before;
	struct agm_boot_info after;

	info_of(&before);
	zassert_true(before.slot[0].len != 0U, "this case needs a live store A entry");

#if IS_ENABLED(CONFIG_BOOT_AGM_LOCK_PRODUCTION)
	zassert_equal(agm_boot_erase(BOOT_DEV, AGM_BOOT_TARGET_STORE_A),
		      AGM_BOOT_E_UNAUTHORIZED, "an unsigned erase has to be refused");
	info_of(&after);
	zassert_equal(after.slot[0].len, before.slot[0].len, "the entry is untouched");
	zassert_equal(after.slot[0].crc, before.slot[0].crc, "and still the same image");
	zassert_equal(after.slot[0].state, before.slot[0].state, "and in the same state");

	/* The publish side of the same gate, on a fresh upload: the bytes may
	 * reach the store (that is just flash), but the record must not move --
	 * an unpublished image cannot be booted, which is the point. */
	fill_pattern(0x5a);
	upload(AGM_BOOT_TARGET_STORE_B, IMG_LEN, 512);
	zassert_equal(agm_boot_upload_finish(BOOT_DEV, AGM_BOOT_TARGET_STORE_B, IMG_LEN),
		      AGM_BOOT_E_UNAUTHORIZED, "an unsigned publish has to be refused");
	info_of(&after);
	zassert_equal(after.slot[1].len, before.slot[1].len,
		      "store B's record entry is untouched");
	zassert_equal(after.slot[1].crc, before.slot[1].crc, "and still the old image");
	zassert_equal(after.slot[1].state, before.slot[1].state, "and in the same state");
	zassert_equal(after.active, before.active, "and nothing became active");
#else
	/* No lock: the same call is allowed without any authorization. */
	zassert_ok(agm_boot_erase(BOOT_DEV, AGM_BOOT_TARGET_STORE_A),
		   "an unlocked build erases without one");
	info_of(&after);
	zassert_equal(after.slot[0].state, AGM_BOOT_SLOT_EMPTY, "the entry was emptied");

	/* Put it back for the cases below (the suite shares one driver). */
	fill_pattern(0x99);
	upload(AGM_BOOT_TARGET_STORE_A, IMG_LEN, 512);
	zassert_ok(test_finish(AGM_BOOT_TARGET_STORE_A, IMG_LEN));
#endif
}

ZTEST(boot_agm_api, test_zz2_authorized_state_change_passes)
{
	uint8_t nonce[AGM_BOOT_NONCE_LEN];
	static const uint8_t zeroes[AGM_BOOT_NONCE_LEN];

	zassert_ok(agm_boot_nonce_get(BOOT_DEV, nonce), "nonce_get()");
	zassert_true(memcmp(nonce, zeroes, sizeof(nonce)) != 0,
		     "the nonce is not all zeroes");

#if IS_ENABLED(CONFIG_BOOT_AGM_LOCK_PRODUCTION)
	struct agm_boot_info after;

	/* The signed erase, through the same helper the storage cases use. */
	boot_agm_test_authorize(AGM_BOOT_AUTH_CMD_ERASE);
	zassert_ok(agm_boot_erase(BOOT_DEV, AGM_BOOT_TARGET_STORE_A),
		   "an authorized erase has to pass");
	info_of(&after);
	zassert_equal(after.slot[0].state, AGM_BOOT_SLOT_EMPTY, "and it really erased");

	/* A grant is one command, one shot: it was spent above, so the *same*
	 * type of call needs a new one. */
	zassert_equal(agm_boot_erase(BOOT_DEV, AGM_BOOT_TARGET_STORE_A),
		      AGM_BOOT_E_UNAUTHORIZED, "the grant was spent by the call it authorized");
#else
	/* Nothing to authorize with in this build: the API still answers, and
	 * the answer is "there is no verifier here" rather than "no". */
	zassert_equal(agm_boot_authorize(BOOT_DEV, nonce, AGM_BOOT_AUTH_CMD_ERASE, NULL, 0U,
					  nonce, AGM_BOOT_NONCE_LEN),
		      -ENOTSUP, "an unsigned build has no command verifier");
#endif
}

ZTEST(boot_agm_api, test_zz3_authorization_is_bound_to_the_command_and_the_nonce)
{
#if IS_ENABLED(CONFIG_BOOT_AGM_LOCK_PRODUCTION)
	uint8_t nonce[AGM_BOOT_NONCE_LEN];
	uint8_t payload[AGM_BOOT_NONCE_LEN + 3U];
	uint8_t sig[64];
	uint32_t len;

	/* The grant names a command: authorizing a publish does not authorize
	 * an erase. */
	boot_agm_test_authorize(AGM_BOOT_AUTH_CMD_PUBLISH);
	zassert_equal(agm_boot_erase(BOOT_DEV, AGM_BOOT_TARGET_STORE_B),
		      AGM_BOOT_E_UNAUTHORIZED,
		      "a publish grant must not open an erase");

	/* A nonce nobody handed out is refused... */
	zassert_ok(agm_boot_nonce_get(BOOT_DEV, nonce), "nonce_get()");
	len = auth_payload(nonce, AGM_BOOT_AUTH_CMD_ERASE, NULL, 0U, payload, sizeof(payload));
	auth_sign_payload(payload, len, sig);
	nonce[0] ^= 0x01U;
	zassert_equal(agm_boot_authorize(BOOT_DEV, nonce, AGM_BOOT_AUTH_CMD_ERASE, NULL, 0U,
					  sig, sizeof(sig)),
		      -EACCES, "a stale nonce must be refused");
	nonce[0] ^= 0x01U;
	/* ... and so is a signature over something else: the same nonce with
	 * one byte of the covered command flipped. */
	payload[AGM_BOOT_NONCE_LEN] = AGM_BOOT_AUTH_CMD_PUBLISH;
	auth_sign_payload(payload, len, sig);
	zassert_equal(agm_boot_authorize(BOOT_DEV, nonce, AGM_BOOT_AUTH_CMD_ERASE, NULL, 0U,
					  sig, sizeof(sig)),
		      -EACCES, "a signature over another command must not authorize this one");

	/* Now the genuine one. Its grant is spent by the call it authorizes,
	 * and the nonce is spent by the authorize() that carried it: both the
	 * second erase and a replay of the signature have to be refused. */
	payload[AGM_BOOT_NONCE_LEN] = AGM_BOOT_AUTH_CMD_ERASE;
	auth_sign_payload(payload, len, sig);
	zassert_ok(agm_boot_authorize(BOOT_DEV, nonce, AGM_BOOT_AUTH_CMD_ERASE, NULL, 0U,
				       sig, sizeof(sig)),
		   "the signed command is accepted");
	zassert_ok(agm_boot_erase(BOOT_DEV, AGM_BOOT_TARGET_STORE_B),
		   "and the erase it authorized runs");
	zassert_equal(agm_boot_erase(BOOT_DEV, AGM_BOOT_TARGET_STORE_B),
		      AGM_BOOT_E_UNAUTHORIZED, "a grant is good for exactly one call");
	zassert_equal(agm_boot_authorize(BOOT_DEV, nonce, AGM_BOOT_AUTH_CMD_ERASE, NULL, 0U,
					  sig, sizeof(sig)),
		      -EACCES, "the same nonce must not be usable twice");
#else
	ztest_test_skip();
#endif
}

/* A fresh NONCE drops a grant that has not been used yet: it
 * opens a new session, and the old authorization belongs to the previous one.
 * The point of pinning it is that the alternative -- carrying the grant over --
 * is silently tempting for a host that fetches its nonce early, and the gate's
 * refusal line ("no grant armed ... a fresh nonce voids an unused grant") is
 * what makes the failure legible. */
ZTEST(boot_agm_api, test_zz4_a_fresh_nonce_voids_the_grant)
{
	uint8_t nonce[AGM_BOOT_NONCE_LEN];

	zassert_ok(agm_boot_nonce_get(BOOT_DEV, nonce), "nonce_get()");

#if IS_ENABLED(CONFIG_BOOT_AGM_LOCK_PRODUCTION)
	/* Authorize an erase, then ask for another nonce before using it: the
	 * grant from the first round trip must be gone. */
	boot_agm_test_authorize(AGM_BOOT_AUTH_CMD_ERASE);
	zassert_ok(agm_boot_nonce_get(BOOT_DEV, nonce), "a second nonce (new session)");
	zassert_equal(agm_boot_erase(BOOT_DEV, AGM_BOOT_TARGET_STORE_A),
		      AGM_BOOT_E_UNAUTHORIZED,
		      "the grant armed before the new nonce is not carried into the new session");
#else
	/* Without a gate the same sequence is simply an erase. */
	zassert_ok(agm_boot_erase(BOOT_DEV, AGM_BOOT_TARGET_STORE_A),
		   "an unlocked build needs no grant at all");
#endif
}

/* The signature covers the command's *arguments*, not just the command id and
 * the nonce: the device rebuilds the payload from the bytes it
 * is handed and hashes those, so signing one argument set and presenting
 * another has to fail. Without this case the packing (`args_len` little
 * endian, then the args) was only exercised with an empty argument list -- the
 * host side has it pinned in tools/tests, the device side had nothing.
 *
 * The refusal also has to leave the nonce alone, which is why the
 * *same* nonce and signature are used for the good call right after it. */
ZTEST(boot_agm_api, test_zz5_signed_command_covers_its_arguments)
{
#if IS_ENABLED(CONFIG_BOOT_AGM_LOCK_PRODUCTION)
	/* A plausible argument blob: "target 2, length 0x10", the shape a host
	 * would sign for a command that names a target. */
	static const uint8_t args[4] = { 0x02U, 0x00U, 0x10U, 0x00U };
	uint8_t tampered[sizeof(args)];
	uint8_t nonce[AGM_BOOT_NONCE_LEN];
	uint8_t payload[AGM_BOOT_NONCE_LEN + 3U + sizeof(args)];
	uint8_t sig[64];
	uint32_t len;

	auth_check_key_pair();
	zassert_ok(agm_boot_nonce_get(BOOT_DEV, nonce), "nonce_get()");
	len = auth_payload(nonce, AGM_BOOT_AUTH_CMD_ERASE, args, sizeof(args), payload,
			   sizeof(payload));
	auth_sign_payload(payload, len, sig);

	memcpy(tampered, args, sizeof(args));
	tampered[0] ^= 0x01U;
	zassert_equal(agm_boot_authorize(BOOT_DEV, nonce, AGM_BOOT_AUTH_CMD_ERASE, tampered,
					 sizeof(tampered), sig, sizeof(sig)),
		      -EACCES, "the signature has to cover the arguments");

	/* Same nonce, same signature, the arguments it was signed over: this is
	 * the call that has to work -- and it also shows the refused attempt
	 * above did not spend the nonce. */
	zassert_ok(agm_boot_authorize(BOOT_DEV, nonce, AGM_BOOT_AUTH_CMD_ERASE, args,
				      sizeof(args), sig, sizeof(sig)),
		   "the signed arguments are accepted");
	zassert_ok(agm_boot_erase(BOOT_DEV, AGM_BOOT_TARGET_STORE_B),
		   "and the grant it armed runs the command it authorized");
#else
	ztest_test_skip();
#endif
}

ZTEST_SUITE(boot_agm_api, NULL, NULL, NULL, NULL, NULL);
