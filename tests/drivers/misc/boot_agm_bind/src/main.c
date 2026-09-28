/*
 * Copyright (c) 2026 AgRV Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief Per-chip binding: does the loader accept exactly one chip's copy?
 *
 * Runs the driver built with CONFIG_BOOT_AGM_BIND on top of the signed
 * profile, against the signed-image suite's containers (fixtures/) with a BIND
 * TLV appended the way production appends one (tools/agm_bind.py embed), and
 * checks the cases that are the design:
 *
 *   - the chip's own bound container is accepted, and one bound to another
 *     chip is not (the copy-protection case the feature exists for);
 *   - a container bound to another *release* is not (the tag covers the
 *     header's version, so re-signing for a new version is not enough);
 *   - an image with no BIND TLV at all is not (a signed build that requires
 *     binding cannot be fed the unbound containers it shipped with);
 *   - a board whose salt was never provisioned refuses a bound image instead
 *     of falling back to a zero key (fail closed: the alternative would make
 *     "forgot to provision" look like "binding works");
 *   - the sector image the host tool writes is the one the device reads, and
 *     the KDF and the tag match the host's known-answer vectors
 *     (tools/tests/test_agm_bind.py asserts the same bytes from the other
 *     side, so a drift in either implementation fails one of the two).
 *
 * The last three of those are what the *enforcement* is: `publish` refuses
 * them and so does the boot path, because both call the same
 * agm_boot_image_verify(). The publish case below is asserted through the
 * upload API (a refused publish must leave the record alone); the boot path's
 * copy of the check is the same call, which is why it needs no separate case
 * (the signed-image suite makes the same argument for its own refusals).
 */

#include <zephyr/device.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/drivers/misc/boot_agm.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/crc.h>
#include <zephyr/ztest.h>

#include <string.h>

#define BOOT_DEV  DEVICE_DT_GET(DT_NODELABEL(boot))
#define INT_FLASH DEVICE_DT_GET(DT_NODELABEL(sim_ondie))

/* The containers, from tests/drivers/misc/boot_agm_ecdsa/fixtures (see that
 * directory's README for how each one was made). */
extern const unsigned char agm_signed_ok[];
extern const unsigned char agm_signed_bound[];
extern const unsigned char agm_signed_bound_other_chip[];
extern const unsigned char agm_signed_v2[];
extern const unsigned char agm_signed_v2_stale_bind[];
extern const unsigned char agm_bind_salt_sector[];
extern const size_t agm_signed_ok_len;
extern const size_t agm_signed_bound_len;
extern const size_t agm_signed_bound_other_chip_len;
extern const size_t agm_signed_v2_len;
extern const size_t agm_signed_v2_stale_bind_len;
extern const size_t agm_bind_salt_sector_len;

#define SALT_LEN   16U
#define SECTOR_LEN 4096U
#define SALT_SECTOR_OFF ((uint32_t)CONFIG_BOOT_AGM_BIND_SALT_OFFSET - 0x80000000U)

/*
 * The UID the fixtures are bound to, and the salt sectors are written for.
 *
 * This is the dev board chip's UID rather than a random one because the same
 * 16 bytes appear in tools/tests/test_agm_bind.py: one value pins the KDF,
 * the tag and the sector format on both
 * sides of the boundary. native_sim has no flash controller to read it from,
 * so the driver's weak hook is replaced here (the same pattern the trial-boot
 * hooks use).
 */
static const uint8_t bench_uid[AGM_BOOT_UID_LEN] = {
	0x41, 0x50, 0x34, 0x36, 0x33, 0x34, 0x31, 0x12,
	0x00, 0xd6, 0xb8, 0x36, 0x56, 0x06, 0x01, 0x78,
};

int agm_boot_bind_uid(uint8_t uid[AGM_BOOT_UID_LEN])
{
	memcpy(uid, bench_uid, sizeof(bench_uid));
	return 0;
}

/* The dev board salt (00..0f) and a second, valid one: "another chip" only has to
 * be a different salt, which is exactly what a different chip has. */
static void bench_salt(uint8_t salt[SALT_LEN])
{
	for (uint32_t i = 0U; i < SALT_LEN; i++) {
		salt[i] = (uint8_t)i;
	}
}

/* The sector image the production tool writes (tools/agm_bind.py salt-sector):
 * magic "AGMB", layout version 1, the salt, a CRC-32 over those 24 bytes, then
 * 0xff padding. Spelled out here so the test fails if the two formats drift --
 * test_the_device_reads_the_sector_the_host_tool_writes compares this against
 * the bytes the host tool actually produced. */
static void salt_sector_image(uint8_t *out, const uint8_t salt[SALT_LEN])
{
	memset(out, 0xff, SECTOR_LEN);
	memcpy(out, "AGMB", 4);
	sys_put_le32(1U, out + 4);
	memcpy(out + 8, salt, SALT_LEN);
	sys_put_le32(crc32_ieee(out, 24U), out + 24);
}

static void place_salt(const uint8_t salt[SALT_LEN])
{
	static uint8_t image[SECTOR_LEN];

	salt_sector_image(image, salt);
	zassert_ok(flash_erase(INT_FLASH, SALT_SECTOR_OFF, SECTOR_LEN));
	zassert_ok(flash_write(INT_FLASH, SALT_SECTOR_OFF, image, SECTOR_LEN));
}

static void clear_salt(void)
{
	zassert_ok(flash_erase(INT_FLASH, SALT_SECTOR_OFF, SECTOR_LEN));
}

/* Push bytes into a target through the same API a host uses, tail included:
 * the flash programs whole words, so a real upload finishes with the padded
 * last word (upload_finish() does the same). */
static void place(enum agm_boot_target target, const unsigned char *data,
		  size_t len)
{
	static unsigned char padded[2048];
	size_t total = ROUND_UP(len, 4U);

	zassert_true(total <= sizeof(padded));
	memcpy(padded, data, len);
	memset(padded + len, 0xff, total - len);

	zassert_ok(agm_boot_upload_begin(BOOT_DEV, target), "upload_begin");
	for (size_t off = 0U; off < total; off += 256U) {
		size_t n = MIN(256U, total - off);

		zassert_ok(agm_boot_upload_write(BOOT_DEV, target, (uint32_t)off,
						 padded + off, (uint32_t)n),
			   "upload_write at %u", (unsigned int)off);
	}
}

/* ---- the vectors ------------------------------------------------------- *
 *
 * Known answers, computed by tools/agm_bind.py and duplicated in
 * tools/tests/test_agm_bind.py: key = HKDF-SHA256(UID, salt = 00..0f) and the
 * tag over each fixture, which is what its BIND TLV carries.
 */
static const uint8_t t_key[AGM_BOOT_BIND_KEY_LEN] = {
	0xd3, 0x86, 0xff, 0x9a, 0x77, 0x3d, 0x41, 0xf2, 0x30, 0xd3, 0x69, 0x3f, 0x64, 0xe0,
	0x14, 0xc1, 0x99, 0x2d, 0x4b, 0x51, 0x7c, 0xb3, 0x84, 0xc5, 0x09, 0x80, 0x50, 0xf9,
	0x77, 0x1d, 0xf2, 0xe0,
};

static const uint8_t t_tag_ok[AGM_BOOT_BIND_KEY_LEN] = {
	0xcc, 0x1f, 0xfd, 0x58, 0xc8, 0x34, 0x71, 0xf0, 0xff, 0xa5, 0xae, 0xb3, 0x14, 0x99,
	0x96, 0x76, 0x4e, 0x22, 0x5d, 0x0a, 0x6f, 0xc4, 0xc0, 0xce, 0x67, 0xe5, 0x62, 0x84,
	0x61, 0x91, 0x1c, 0x20,
};

/* SHA-256(key)'s first four bytes: what the loader prints in `info` and what
 * the provisioning tool compares against. */
static const uint8_t t_fp[AGM_BOOT_BIND_FP_LEN] = { 0xf7, 0xb6, 0xf8, 0xa8 };

ZTEST(boot_agm_bind, test_the_device_reads_the_sector_the_host_tool_writes)
{
	uint8_t image[SECTOR_LEN];
	uint8_t salt[SALT_LEN];

	bench_salt(salt);
	salt_sector_image(image, salt);

	zassert_equal(agm_bind_salt_sector_len, SECTOR_LEN);
	zassert_mem_equal(image, agm_bind_salt_sector, SECTOR_LEN,
			  "the sector format is the host tool's");
}

ZTEST(boot_agm_bind, test_key_tag_and_fingerprint_vectors)
{
	uint8_t salt[SALT_LEN];
	uint8_t key[AGM_BOOT_BIND_KEY_LEN];
	uint8_t tag[AGM_BOOT_BIND_KEY_LEN];
	uint8_t fp[AGM_BOOT_BIND_FP_LEN];
	bool present = false;

	bench_salt(salt);
	place_salt(salt);

	zassert_ok(agm_boot_bind_key(key, &present), "the salt is provisioned");
	zassert_true(present);
	zassert_mem_equal(key, t_key, sizeof(key),
			  "the device KDF has to match tools/agm_bind.py");

	zassert_ok(agm_boot_bind_tag(agm_signed_ok, (uint32_t)agm_signed_ok_len, tag));
	zassert_mem_equal(tag, t_tag_ok, sizeof(tag),
			  "and so does the tag over the container's version and digest");
	zassert_mem_equal(tag, agm_signed_bound + agm_signed_bound_len - AGM_BOOT_BIND_KEY_LEN,
			  sizeof(tag), "which is the tag the fixture carries");

	zassert_ok(agm_boot_bind_fingerprint(fp));
	zassert_mem_equal(fp, t_fp, sizeof(fp), "and the fingerprint the console prints");

	clear_salt();
	zassert_equal(agm_boot_bind_key(key, &present), -ENOENT,
		      "an erased sector is not a salt");
	zassert_false(present);
}

ZTEST(boot_agm_bind, test_bound_image_is_accepted)
{
	uint8_t salt[SALT_LEN];

	bench_salt(salt);
	place_salt(salt);
	place(AGM_BOOT_TARGET_STORE_A, agm_signed_bound, agm_signed_bound_len);

	zassert_ok(agm_boot_image_verify(BOOT_DEV, AGM_BOOT_TARGET_STORE_A,
					 (uint32_t)agm_signed_bound_len),
		   "this chip's own copy verifies");
}

ZTEST(boot_agm_bind, test_unbound_image_is_refused)
{
	uint8_t salt[SALT_LEN];

	bench_salt(salt);
	place_salt(salt);
	/* signed_ok is the same image without the BIND TLV. */
	place(AGM_BOOT_TARGET_STORE_A, agm_signed_ok, agm_signed_ok_len);

	zassert_equal(agm_boot_image_verify(BOOT_DEV, AGM_BOOT_TARGET_STORE_A,
					    (uint32_t)agm_signed_ok_len),
		      AGM_BOOT_E_UNBOUND,
		      "a signed build that requires binding takes no unbound image");
}

ZTEST(boot_agm_bind, test_image_bound_to_another_chip_is_refused)
{
	uint8_t salt[SALT_LEN];

	bench_salt(salt);
	place_salt(salt);
	place(AGM_BOOT_TARGET_STORE_A, agm_signed_bound_other_chip,
	      agm_signed_bound_other_chip_len);

	zassert_equal(agm_boot_image_verify(BOOT_DEV, AGM_BOOT_TARGET_STORE_A,
					    (uint32_t)agm_signed_bound_other_chip_len),
		      AGM_BOOT_E_UNBOUND,
		      "another chip's tag does not verify here");
}

ZTEST(boot_agm_bind, test_image_bound_to_another_release_is_refused)
{
	uint8_t salt[SALT_LEN];

	bench_salt(salt);
	place_salt(salt);
	/* The 2.0.0 container carrying the tag that belongs to 1.0.0. */
	place(AGM_BOOT_TARGET_STORE_A, agm_signed_v2_stale_bind,
	      agm_signed_v2_stale_bind_len);

	zassert_equal(agm_boot_image_verify(BOOT_DEV, AGM_BOOT_TARGET_STORE_A,
					    (uint32_t)agm_signed_v2_stale_bind_len),
		      AGM_BOOT_E_UNBOUND,
		      "the version is part of what the tag commits to");
}

ZTEST(boot_agm_bind, test_a_board_without_a_salt_refuses_bound_images)
{
	uint8_t salt[SALT_LEN];

	/* The salt the fixture was bound to is *gone*: this is a board that
	 * never got provisioned (or one whose sector was erased), and the image
	 * has to be refused rather than verified against a zero key. */
	bench_salt(salt);
	place_salt(salt);
	clear_salt();
	place(AGM_BOOT_TARGET_STORE_A, agm_signed_bound, agm_signed_bound_len);

	zassert_equal(agm_boot_image_verify(BOOT_DEV, AGM_BOOT_TARGET_STORE_A,
					    (uint32_t)agm_signed_bound_len),
		      AGM_BOOT_E_UNBOUND,
		      "no salt means nothing can be bound to this chip");
}

ZTEST(boot_agm_bind, test_publish_refuses_an_unbound_image)
{
	struct agm_boot_info before;
	struct agm_boot_info after;
	uint8_t salt[SALT_LEN];

	bench_salt(salt);
	place_salt(salt);

	/* A good, bound image first: publish works, so the refusal below is the
	 * binding check and not something else about this layout. */
	place(AGM_BOOT_TARGET_STORE_A, agm_signed_bound, agm_signed_bound_len);
	zassert_ok(agm_boot_upload_finish(BOOT_DEV, AGM_BOOT_TARGET_STORE_A,
					  (uint32_t)agm_signed_bound_len));
	zassert_ok(agm_boot_info_get(BOOT_DEV, &before));
	zassert_true(before.record_valid, "the first publish wrote a record");

	/* Then the same image with no BIND TLV: refused, and the record -- the
	 * only thing that makes an uploaded image bootable -- is untouched. */
	place(AGM_BOOT_TARGET_STORE_B, agm_signed_ok, agm_signed_ok_len);
	zassert_equal(agm_boot_upload_finish(BOOT_DEV, AGM_BOOT_TARGET_STORE_B,
					     (uint32_t)agm_signed_ok_len),
		      AGM_BOOT_E_UNBOUND);
	zassert_ok(agm_boot_info_get(BOOT_DEV, &after));
	zassert_equal(after.active, before.active, "still the accepted image");
	zassert_equal(after.slot[1].state, AGM_BOOT_SLOT_EMPTY,
		      "and store B never reached the record");
	zassert_equal(after.slot[1].len, 0U);
}

ZTEST_SUITE(boot_agm_bind, NULL, NULL, NULL, NULL, NULL);
