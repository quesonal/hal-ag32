/*
 * Copyright (c) 2026 AgRV Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief Signed images: does the loader accept exactly the right ones?
 *
 * Runs the driver built with CONFIG_BOOT_AGM_SIG_ECDSA_P256 against real
 * `imgtool sign` output (in fixtures/) and checks the
 * four cases that matter:
 *
 *   - a properly signed MCUboot container is accepted;
 *   - one byte of payload flipped is refused (the SHA-256 TLV no longer
 *     matches);
 *   - a container signed by another key is refused (the KEYHASH TLV names a key
 *     this build does not trust);
 *   - a container that is signed by the trusted key but linked for another
 *     address is refused (the header's `ih_load_addr` is not where this layout
 *     would run it);
 *   - a raw image is refused, so a signed build cannot be downgraded by
 *     writing an unsigned one (the mirror image of BOOT_AGM_SIG_NONE, which
 *     accepts raw images and is explicitly not a security boundary).
 *
 * Since CONFIG_BOOT_AGM_BITSTREAM_SIGNED is on as well, the second half of the
 * file does the same for the *fabric*: the bitstream slots have to hold a
 * container stamped for them, with the whole 99944-byte fabric as its payload
 * and the fabric key as its KEYHASH. The boot path that calls that verifier
 * runs before the kernel (soc/agm/agrv2k/fcb.c), so what is exercised here is
 * the verifier itself, exactly as it is called there.
 */

#include <zephyr/device.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/drivers/misc/agm_bitstream.h>
#include <zephyr/drivers/misc/boot_agm.h>
#include <zephyr/ztest.h>

#include <string.h>

#define BOOT_DEV DEVICE_DT_GET(DT_NODELABEL(boot))

extern const unsigned char agm_signed_ok[];
extern const unsigned char agm_signed_other_key[];
extern const unsigned char agm_signed_other_slot[];
extern const unsigned char agm_image_raw[];
extern const unsigned char agm_bs_signed[];
extern const unsigned char agm_rsa_signed[];
extern const unsigned char agm_signed_v2[];
extern const unsigned char agm_signed_v0[];
extern const size_t agm_signed_ok_len;
extern const size_t agm_signed_other_key_len;
extern const size_t agm_signed_other_slot_len;
extern const size_t agm_image_raw_len;
extern const size_t agm_bs_signed_len;
extern const size_t agm_rsa_signed_len;
extern const size_t agm_signed_v2_len;
extern const size_t agm_signed_v0_len;

/* ---- board-side hooks, stubbed --------------------------------------- *
 *
 * The anti-rollback cases below include one that *boots* an image (to show
 * which checks the boot path does and does not make), and the boot path talks
 * to the backup domain, the IWDG and the SoC reset-cause register before it
 * jumps. None of those exists under native_sim: these strong definitions
 * replace all three with RAM, exactly like the api suite does for the trial
 * chain -- a stub here.
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

/*
 * Rewrite the container's signature TLV type to the *other* profile's
 * (ECDSA_SIG 0x22 <-> RSA2048_PSS 0x20), leaving the digest, the KEYHASH and
 * the signature bytes exactly as the trusted key signed them. That is the one
 * field the profile check looks at, so a container patched this way can only
 * be refused by "this build does not take that algorithm" -- the
 * anti-cross-profile (and anti-downgrade) rule.
 */
static void stamp_signature_tlv_as(const uint8_t *src, uint32_t len, uint8_t *dst,
				   uint16_t want_type, uint16_t other_type)
{
	uint16_t hdr_size;
	uint16_t tlv_tot;
	uint32_t img_size;
	uint32_t off;
	uint32_t end;

	zassert_true(len >= 32U, "a container is at least a header");
	memcpy(dst, src, len);
	memcpy(&hdr_size, dst + 8, sizeof(hdr_size));
	memcpy(&img_size, dst + 12, sizeof(img_size));
	off = (uint32_t)hdr_size + img_size; /* the TLV area */

	zassert_true(off + 4U <= len, "the fixture has a TLV area");
	memcpy(&tlv_tot, dst + off + 2, sizeof(tlv_tot));
	end = off + tlv_tot;
	zassert_true(end <= len, "the TLV area fits the container");
	off += 4U;

	while (off + 4U <= end) {
		uint16_t type;
		uint16_t tlen;

		memcpy(&type, dst + off, sizeof(type));
		memcpy(&tlen, dst + off + 2, sizeof(tlen));
		if (type == want_type) {
			memcpy(dst + off, &other_type, sizeof(other_type));
			return;
		}
		off += 4U + tlen;
	}
	zassert_true(false, "no signature TLV in the fixture");
}

ZTEST(boot_agm_ecdsa, test_accepts_a_properly_signed_image)
{
	place(AGM_BOOT_TARGET_STORE_A, agm_signed_ok, agm_signed_ok_len);

	{
		int ret = agm_boot_image_verify(BOOT_DEV, AGM_BOOT_TARGET_STORE_A,
						(uint32_t)agm_signed_ok_len);

		zassert_equal(ret, 0, "verify returned %d", ret);
	}
}

ZTEST(boot_agm_ecdsa, test_rejects_a_tampered_payload)
{
	static unsigned char tampered[2048];

	zassert_true(agm_signed_ok_len <= sizeof(tampered));
	memcpy(tampered, agm_signed_ok, agm_signed_ok_len);
	tampered[0x40] ^= 0xffU; /* inside the payload; the header is 0x20 bytes */

	place(AGM_BOOT_TARGET_STORE_A, tampered, agm_signed_ok_len);
	zassert_equal(agm_boot_image_verify(BOOT_DEV, AGM_BOOT_TARGET_STORE_A,
					    (uint32_t)agm_signed_ok_len),
		      -EACCES, "a flipped payload byte has to be refused");
}

ZTEST(boot_agm_ecdsa, test_rejects_another_key)
{
	place(AGM_BOOT_TARGET_STORE_A, agm_signed_other_key, agm_signed_other_key_len);

	zassert_equal(agm_boot_image_verify(BOOT_DEV, AGM_BOOT_TARGET_STORE_A,
					    (uint32_t)agm_signed_other_key_len),
		      -EACCES, "the keyhash TLV has to name a trusted key");
}

ZTEST(boot_agm_ecdsa, test_rejects_a_raw_image)
{
	place(AGM_BOOT_TARGET_STORE_A, agm_image_raw, agm_image_raw_len);

	zassert_equal(agm_boot_image_verify(BOOT_DEV, AGM_BOOT_TARGET_STORE_A,
					    (uint32_t)agm_image_raw_len),
		      -EINVAL, "a raw image has no MCUboot header");
}

/* The two rows that cross profiles: an RSA container must not
 * install on an ECDSA build, whatever else about it is genuine, and the same
 * container with only its signature TLV *relabelled* as ECDSA's must not
 * either (the digest and the KEYHASH would still be the RSA key's, so this is
 * also the "no downgrade by relabelling" case). */
ZTEST(boot_agm_ecdsa, test_rejects_a_container_of_the_other_profile)
{
	place(AGM_BOOT_TARGET_STORE_A, agm_rsa_signed, agm_rsa_signed_len);

	zassert_equal(agm_boot_image_verify(BOOT_DEV, AGM_BOOT_TARGET_STORE_A,
					    (uint32_t)agm_rsa_signed_len),
		      -EACCES, "an RSA-signed container is not signed by this key");
}

ZTEST(boot_agm_ecdsa, test_rejects_a_relabelled_signature_tlv)
{
	static uint8_t patched[2048];

	zassert_true(agm_signed_ok_len <= sizeof(patched));
	stamp_signature_tlv_as(agm_signed_ok, (uint32_t)agm_signed_ok_len, patched,
			       0x22U /* ECDSA_SIG */, 0x20U /* RSA2048_PSS */);
	place(AGM_BOOT_TARGET_STORE_A, patched, agm_signed_ok_len);

	zassert_equal(agm_boot_image_verify(BOOT_DEV, AGM_BOOT_TARGET_STORE_A,
					    (uint32_t)agm_signed_ok_len),
		      -EINVAL, "this build takes ECDSA TLVs, not RSA ones");
}


/* The enforcement points: publish_upload() and store_boot() both call the
 * verifier, so a bad image neither reaches the record nor gets booted. */
ZTEST(boot_agm_ecdsa, test_publish_accepts_a_signed_image)
{
	struct agm_boot_info info;

	place(AGM_BOOT_TARGET_STORE_A, agm_signed_ok, agm_signed_ok_len);
	zassert_ok(agm_boot_upload_finish(BOOT_DEV, AGM_BOOT_TARGET_STORE_A,
					  (uint32_t)agm_signed_ok_len),
		   "a properly signed container has to publish");

	zassert_ok(agm_boot_info_get(BOOT_DEV, &info));
	zassert_true(info.record_valid);
	zassert_equal(info.slot[0].len, (uint32_t)agm_signed_ok_len);
	/* The entry point comes from the container's header, not from the host:
	 * the fixture is linked at slot + 0x20. */
	zassert_equal(info.slot[0].entry,
		      (uint32_t)DT_PROP(DT_NODELABEL(boot), slot_address) + 0x20U,
		      "entry should be derived from the MCUboot header size");
}

ZTEST(boot_agm_ecdsa, test_publish_refuses_a_raw_image)
{
	struct agm_boot_info before;
	struct agm_boot_info after;

	/* The suite shares one bootloader, so compare against the state this test
	 * starts with rather than assuming an empty record. */
	zassert_ok(agm_boot_info_get(BOOT_DEV, &before));

	place(AGM_BOOT_TARGET_STORE_A, agm_image_raw, agm_image_raw_len);
	zassert_true(agm_boot_upload_finish(BOOT_DEV, AGM_BOOT_TARGET_STORE_A,
					    (uint32_t)agm_image_raw_len) < 0,
		     "a raw image must not publish in a signed build");

	zassert_ok(agm_boot_info_get(BOOT_DEV, &after));
	zassert_equal(after.record_valid, before.record_valid);
	zassert_equal(after.active, before.active);
	zassert_equal(after.slot[0].len, before.slot[0].len);
	zassert_equal(after.slot[0].crc, before.slot[0].crc);
	zassert_equal(after.slot[0].state, before.slot[0].state);
	zassert_equal(after.slot[0].entry, before.slot[0].entry);
}

/* A container the trusted key really signed, but linked for another address:
 * the bytes are fine and the KEYHASH matches, so only the header's
 * `ih_load_addr` can catch it. This is the check that keeps a wrong-slot
 * upload from ever reaching the record. */
ZTEST(boot_agm_ecdsa, test_publish_refuses_a_container_for_another_slot)
{
	struct agm_boot_info before;
	struct agm_boot_info after;

	zassert_ok(agm_boot_info_get(BOOT_DEV, &before));

	/* It verifies as an image (signature and digest are genuine) ... */
	place(AGM_BOOT_TARGET_STORE_A, agm_signed_other_slot,
	      agm_signed_other_slot_len);
	zassert_ok(agm_boot_image_verify(BOOT_DEV, AGM_BOOT_TARGET_STORE_A,
					 (uint32_t)agm_signed_other_slot_len),
		   "the fixture has to be genuinely signed, or this tests nothing");

	/* ... but it cannot be published, because the loader would run it at
	 * slot + header size while the header says 0x80020020. */
	zassert_equal(agm_boot_upload_finish(BOOT_DEV, AGM_BOOT_TARGET_STORE_A,
					     (uint32_t)agm_signed_other_slot_len),
		      -EINVAL, "a container linked for another address must not publish");

	zassert_ok(agm_boot_info_get(BOOT_DEV, &after));
	zassert_equal(after.record_valid, before.record_valid);
	zassert_equal(after.active, before.active);
	zassert_equal(after.slot[0].len, before.slot[0].len);
	zassert_equal(after.slot[0].crc, before.slot[0].crc);
	zassert_equal(after.slot[0].state, before.slot[0].state);
	zassert_equal(after.slot[0].entry, before.slot[0].entry);
}

/* Tamper with the store *after* publishing, then ask the boot path to run it:
 * it has to refuse before copying or jumping anywhere. */
ZTEST(boot_agm_ecdsa, test_boot_refuses_a_store_rewritten_afterwards)
{
	static unsigned char tampered[2048];
	struct agm_boot_info info;
	uint32_t sym;
	uint32_t erase_pad;
	const struct device *store = DEVICE_DT_GET(DT_NODELABEL(sim_store));

	place(AGM_BOOT_TARGET_STORE_A, agm_signed_ok, agm_signed_ok_len);
	zassert_ok(agm_boot_upload_finish(BOOT_DEV, AGM_BOOT_TARGET_STORE_A,
					  (uint32_t)agm_signed_ok_len));
	zassert_ok(agm_boot_info_get(BOOT_DEV, &info));
	zassert_true(info.slot[0].state == AGM_BOOT_SLOT_TRIAL);

	/* Rewrite the store with a tampered copy, behind the record's back. */
	zassert_true(agm_signed_ok_len <= sizeof(tampered));
	memcpy(tampered, agm_signed_ok, agm_signed_ok_len);
	tampered[0x40] ^= 0xffU;
	sym = (uint32_t)DT_PROP(DT_NODELABEL(boot), store_a_offset);
	erase_pad = ROUND_UP((uint32_t)agm_signed_ok_len, 4096U);
	for (uint32_t off = 0U; off < erase_pad; off += 4096U) {
		zassert_ok(flash_erase(store, sym + off, 4096U));
	}
	zassert_ok(flash_write(store, sym, tampered,
			       ROUND_UP((uint32_t)agm_signed_ok_len, 4U)));

	zassert_equal(agm_boot_boot(BOOT_DEV, 0U), -EACCES,
		      "the boot path has to refuse the tampered store");
}

/* ---- the signed fabric (bitstream) slots --------------------------------
 *
 * CONFIG_BOOT_AGM_BITSTREAM_SIGNED puts MCUboot containers in the two fabric
 * update slots, around the whole 99944-byte bitstream, verified with the
 * *fabric* key (agm_boot_bitstream_pubkey -- a different one from the
 * application's, on purpose). The verifier is the same code the boot path
 * calls at PRE_KERNEL_1, so these cases are the boot path's guarantees:
 *
 *   - a genuine fabric container verifies, and publishing it commits a record
 *     that names the slot and the image;
 *   - a flipped payload byte is refused (the digest TLV no longer matches),
 *     even when the record's CRC matches the tampered bytes -- i.e. the CRC
 *     check cannot be used as a bypass;
 *   - a slot whose bytes are not what the record says is refused before
 *     anything else happens (the pre-check that keeps a damaged slot from
 *     taking the fabric down);
 *   - an application container is refused: it is not stamped for the fabric
 *     and does not carry a whole bitstream.
 */
#define BS_SLOT1_ADDR  ((uint32_t)AGM_BITSTREAM_SLOT1_ADDR)
#define BS_FLASH_BASE  ((uint32_t)DT_PROP(DT_NODELABEL(boot), on_die_flash_base))
#define BS_RECORD_OFF  ((uint32_t)0x800e6000U - BS_FLASH_BASE)
#define BS_REC_MAGIC   0x31525342U /* "BSR1" */

/* The fabric fixture is ~100 KB: too big for place()'s staging buffer, so it
 * goes into the bitstream slot straight from the (flash-resident) fixture
 * array. Two details are what a real host does too: the tail is padded out to
 * a word (the flash programs whole words and the driver holds a partial one
 * back until the upload is finished), and `flip_at` replaces one 4-byte word
 * inside the payload -- which is how a "genuinely signed bytes, later
 * rewritten" slot is built without a second 100 KB fixture. */
static uint32_t place_fabric(size_t flip_at)
{
	static const unsigned char pin = 0xffU;
	static const uint32_t flipped = 0xdeadbeefU;
	size_t len = agm_bs_signed_len;
	size_t total = ROUND_UP(len, 4U);
	size_t off = 0U;

	zassert_ok(agm_boot_upload_begin(BOOT_DEV, AGM_BOOT_TARGET_BITSTREAM),
		   "upload_begin");
	if (flip_at != 0U) {
		zassert_ok(agm_boot_upload_write(BOOT_DEV, AGM_BOOT_TARGET_BITSTREAM,
						 0U, agm_bs_signed, (uint32_t)flip_at),
			   "upload_write (head)");
		zassert_ok(agm_boot_upload_write(BOOT_DEV, AGM_BOOT_TARGET_BITSTREAM,
						 (uint32_t)flip_at, &flipped,
						 sizeof(flipped)),
			   "upload_write (flipped word)");
		off = flip_at + sizeof(flipped);
	}
	if (off < len) {
		zassert_ok(agm_boot_upload_write(BOOT_DEV, AGM_BOOT_TARGET_BITSTREAM,
						 (uint32_t)off, agm_bs_signed + off,
						 (uint32_t)(len - off)),
			   "upload_write (rest)");
	}
	for (off = len; off < total; off++) {
		zassert_ok(agm_boot_upload_write(BOOT_DEV, AGM_BOOT_TARGET_BITSTREAM,
						 (uint32_t)off, &pin, 1U),
			   "upload_write (tail padding) at %u", (unsigned int)off);
	}
	/* The slot this upload pinned -- asked for *after* begin(), the way a
	 * host sees it: a live session's window does not move under it. */
	return agm_boot_target_window_base(NULL, AGM_BOOT_TARGET_BITSTREAM);
}

/* The CRC the record would carry for what is in the slot right now, read back
 * through the driver: the boot path's CRC pre-check is against exactly this. */
static uint32_t slot_crc(void)
{
	uint32_t crc = 0U;

	zassert_ok(agm_boot_upload_crc(BOOT_DEV, AGM_BOOT_TARGET_BITSTREAM, 0U,
				       (uint32_t)agm_bs_signed_len, &crc),
		   "reading the slot back for its CRC");
	return crc;
}

ZTEST(boot_agm_ecdsa, test_fabric_container_verifies_and_publishes)
{
	uint32_t rec[8];
	uint32_t crc;
	uint32_t slot = place_fabric(0U);

	/* A fresh bitstream record means the update goes into slot 1. */
	zassert_equal(slot, BS_SLOT1_ADDR, "the fixture is verified at its slot");

	crc = slot_crc();

	zassert_ok(agm_boot_bitstream_verify(slot, (uint32_t)agm_bs_signed_len, crc),
		   "a genuine fabric container has to verify");

	zassert_ok(agm_boot_upload_finish(BOOT_DEV, AGM_BOOT_TARGET_BITSTREAM,
					  (uint32_t)agm_bs_signed_len),
		   "and it has to publish");

	zassert_ok(flash_read(DEVICE_DT_GET(DT_NODELABEL(sim_ondie)), BS_RECORD_OFF,
			      rec, sizeof(rec)));
	zassert_equal(rec[0], BS_REC_MAGIC, "the record has to be committed");
	zassert_equal(rec[2], BS_SLOT1_ADDR, "record slot");
	zassert_equal(rec[3], (uint32_t)agm_bs_signed_len, "record length");
	zassert_equal(rec[4], crc, "record CRC");
}

ZTEST(boot_agm_ecdsa, test_fabric_container_with_a_flipped_byte_is_refused)
{
	/* Inside the fabric payload (the container header is 0x20 bytes). */
	uint32_t slot = place_fabric(0x40U);

	/* The CRC matches what is in the slot now -- so this cannot be caught by
	 * the pre-check, only by the digest/signature. */
	zassert_equal(agm_boot_bitstream_verify(slot, (uint32_t)agm_bs_signed_len,
						slot_crc()),
		      -EACCES, "a rewritten fabric has to fail verification");
}

ZTEST(boot_agm_ecdsa, test_fabric_slot_disagreeing_with_its_record_is_refused)
{
	uint32_t crc;
	uint32_t slot = place_fabric(0U);

	crc = slot_crc();

	/* The boot path gets (len, crc) from the record: a slot that does not
	 * match it has to be refused before the fabric is touched. */
	zassert_equal(agm_boot_bitstream_verify(slot, (uint32_t)agm_bs_signed_len,
						crc ^ 0x1U),
		      -EBADMSG, "the record's CRC has to gate the slot");
}

ZTEST(boot_agm_ecdsa, test_fabric_slot_refuses_an_application_container)
{
	uint32_t slot;
	uint32_t crc = 0U;

	/* Genuinely signed, but an application image: 1004 bytes and stamped for
	 * the application slot. It must not be usable as a fabric. */
	place(AGM_BOOT_TARGET_BITSTREAM, agm_signed_ok, agm_signed_ok_len);
	slot = agm_boot_target_window_base(NULL, AGM_BOOT_TARGET_BITSTREAM);
	zassert_ok(agm_boot_upload_crc(BOOT_DEV, AGM_BOOT_TARGET_BITSTREAM, 0U,
				       (uint32_t)agm_signed_ok_len, &crc));

	zassert_equal(agm_boot_bitstream_verify(slot, (uint32_t)agm_signed_ok_len, crc),
		      -EINVAL, "an application container is not a fabric image");
}

/* ---- anti-rollback (CONFIG_BOOT_AGM_ANTI_ROLLBACK) ---------------------- *
 *
 * Signing decides *who* may install an image; the version floor decides
 * *which* one. Both fixtures here are genuinely signed by the trusted key and
 * differ only in `ih_ver` (1.0.0 vs 2.0.0), so what these cases pin is the
 * ordering and the floor's behaviour -- not a reimplementation of them.
 *
 * The floor is checked when an image is *installed*, never when one is booted:
 * refusing an older image at boot would also block the A/B rollback that is
 * this loader's safety net (and an attacker who can hand-edit the record can
 * clear the floor anyway, which is the RDP half).
 *
 * Named `test_zz_*` on purpose: ztest runs the suite in name order, and the
 * floor only ever rises -- the rest of the suite publishes 1.0.0, so these
 * cases have to come after it (the api suite's `test_00_` does the mirror
 * image of this for "the untouched record").
 */
ZTEST(boot_agm_ecdsa, test_zz_anti_rollback_refuses_an_older_image)
{
	struct agm_boot_info before;
	struct agm_boot_info after;

	/* v2 installs and raises the floor ... */
	place(AGM_BOOT_TARGET_STORE_A, agm_signed_v2, agm_signed_v2_len);
	zassert_ok(agm_boot_upload_finish(BOOT_DEV, AGM_BOOT_TARGET_STORE_A,
					  (uint32_t)agm_signed_v2_len));
	zassert_ok(agm_boot_info_get(BOOT_DEV, &before));
	zassert_equal(before.sec_ver, 0x02000000U,
		      "the floor follows the accepted image (2.0.0)");

	/* ... re-installing the *same* release is a normal recovery move ... */
	place(AGM_BOOT_TARGET_STORE_A, agm_signed_v2, agm_signed_v2_len);
	zassert_ok(agm_boot_upload_finish(BOOT_DEV, AGM_BOOT_TARGET_STORE_A,
					  (uint32_t)agm_signed_v2_len),
		   "an image of the same version has to be accepted");

	/* ... and v1 is refused, with the record left exactly as it was. */
	zassert_ok(agm_boot_info_get(BOOT_DEV, &before));
	place(AGM_BOOT_TARGET_STORE_B, agm_signed_ok, agm_signed_ok_len);
	zassert_equal(agm_boot_upload_finish(BOOT_DEV, AGM_BOOT_TARGET_STORE_B,
					     (uint32_t)agm_signed_ok_len),
		      AGM_BOOT_E_OLD_VERSION, "the older image has to be refused");
	zassert_ok(agm_boot_info_get(BOOT_DEV, &after));
	zassert_equal(after.active, before.active);
	zassert_equal(after.slot[0].len, before.slot[0].len);
	zassert_equal(after.slot[1].state, before.slot[1].state,
		      "the refused image must not have become TRIAL");
	zassert_equal(after.sec_ver, 0x02000000U, "and the floor stays");
}

ZTEST(boot_agm_ecdsa, test_zz_anti_rollback_floor_survives_erase)
{
	struct agm_boot_info info;

	/* Keep the floor at 2.0.0 whatever ran before ... */
	place(AGM_BOOT_TARGET_STORE_A, agm_signed_v2, agm_signed_v2_len);
	zassert_ok(agm_boot_upload_finish(BOOT_DEV, AGM_BOOT_TARGET_STORE_A,
					  (uint32_t)agm_signed_v2_len));

	/* ... wipe the slot the way the console's `erase` does (it rewrites the
	 * record from the live state, which is what keeps the floor) ... */
	zassert_ok(agm_boot_erase(BOOT_DEV, AGM_BOOT_TARGET_STORE_A));
	zassert_ok(agm_boot_info_get(BOOT_DEV, &info));
	zassert_equal(info.sec_ver, 0x02000000U,
		      "erase must not reset the anti-rollback floor");
	zassert_true(info.slot[0].state == AGM_BOOT_SLOT_EMPTY, "slot A was erased");

	/* ... so the old image is still refused afterwards. */
	place(AGM_BOOT_TARGET_STORE_A, agm_signed_ok, agm_signed_ok_len);
	zassert_equal(agm_boot_upload_finish(BOOT_DEV, AGM_BOOT_TARGET_STORE_A,
					     (uint32_t)agm_signed_ok_len),
		      AGM_BOOT_E_OLD_VERSION, "erase-then-downgrade has to fail too");
}

/* ---- anti-rollback: the two contracts the review found mis-stated ------- *
 *
 * `test_zz*` on purpose, like the cases below: the floor only rises, so the
 * order is load-bearing. These two come first among them because the first
 * one publishes a 1.0.0 image, which needs the floor to still be <= 1.0.0
 * (it is: the rest of the suite installs 1.0.0 too).
 */

/* Where the floor *is* checked, and where it deliberately is not.
 *
 * The boot path re-verifies the signature but must keep A/B rollback working:
 * an image that was published before the floor rose (the other slot, i.e. the
 * normal A/B situation) still boots. That is the contract
 * CONFIG_BOOT_AGM_ANTI_ROLLBACK used to mis-describe as checking "again
 * before it is booted" -- this case is what makes a future
 * "fix" that adds a boot-time version check fail loudly instead of silently
 * disabling rollback.
 */
ZTEST(boot_agm_ecdsa, test_zz1_anti_rollback_is_install_only)
{
	struct agm_boot_info info;

	/* 1.0.0 goes into store A while that is still allowed ... */
	place(AGM_BOOT_TARGET_STORE_A, agm_signed_ok, agm_signed_ok_len);
	zassert_ok(agm_boot_upload_finish(BOOT_DEV, AGM_BOOT_TARGET_STORE_A,
					  (uint32_t)agm_signed_ok_len));

	/* ... then 2.0.0 into store B raises the floor above store A's image. */
	place(AGM_BOOT_TARGET_STORE_B, agm_signed_v2, agm_signed_v2_len);
	zassert_ok(agm_boot_upload_finish(BOOT_DEV, AGM_BOOT_TARGET_STORE_B,
					  (uint32_t)agm_signed_v2_len));
	zassert_ok(agm_boot_info_get(BOOT_DEV, &info));
	zassert_equal(info.sec_ver, 0x02000000U, "the floor is 2.0.0");
	zassert_equal(info.slot[0].len, (uint32_t)agm_signed_ok_len,
		      "and store A still holds the older, published image");

	/* Store A is now *below* the floor -- and the boot path still runs it:
	 * the signature is what it checks, the version is not. (Uninstalling it
	 * is refused; booting what is already installed is the A/B safety net.) */
	fake_jumps = 0U;
	fake_jump_entry = 0U;
	zassert_ok(agm_boot_boot(BOOT_DEV, 0U), "in-rules image: store A boots");
	zassert_equal(fake_jumps, 1U, "the payload was entered");
	zassert_equal(fake_jump_entry, 0x80030020U,
		      "at the container's run address (slot base + MCUboot header)");
}

/* The comparison is gated on the *floor*, not on the candidate, so 0.0.0 --
 * the oldest version there is -- is refused like any other value below it.
 * (The first cut short-circuited on `cand_ver != 0U`, which left exactly
 * that one version installable over any floor.) The floor is non-zero
 * here because the case above raised it. */
ZTEST(boot_agm_ecdsa, test_zz2_anti_rollback_refuses_version_zero)
{
	struct agm_boot_info before;
	struct agm_boot_info after;

	zassert_ok(agm_boot_info_get(BOOT_DEV, &before));
	zassert_equal(before.sec_ver, 0x02000000U, "this case needs a raised floor");

	place(AGM_BOOT_TARGET_STORE_A, agm_signed_v0, agm_signed_v0_len);
	zassert_equal(agm_boot_upload_finish(BOOT_DEV, AGM_BOOT_TARGET_STORE_A,
					     (uint32_t)agm_signed_v0_len),
		      AGM_BOOT_E_OLD_VERSION, "0.0.0 is below the floor like any version");

	zassert_ok(agm_boot_info_get(BOOT_DEV, &after));
	zassert_equal(after.sec_ver, before.sec_ver, "the refused image did not move it");
	zassert_equal(after.active, before.active);
	zassert_equal(after.slot[0].len, before.slot[0].len,
		      "and the record entry was not rewritten");
	zassert_equal(after.slot[0].state, before.slot[0].state);
}

ZTEST_SUITE(boot_agm_ecdsa, NULL, NULL, NULL, NULL, NULL);
