/*
 * Copyright (c) 2026 AgRV Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief Signed images, RSA flavour: does the loader accept exactly the right
 *        ones?
 *
 * The same five cases as tests/drivers/misc/boot_agm_ecdsa, but against a
 * driver built with CONFIG_BOOT_AGM_SIG_RSA2048_PSS and against RSA fixtures.
 * They differ from the ECDSA ones in exactly the three places the driver has
 * profile-specific code -- the signature TLV type (0x20), the raw 256-byte
 * signature, and the KEYHASH covering the PKCS#1 DER rather than the
 * SubjectPublicKeyInfo -- so running both suites is what keeps those from
 * silently drifting apart:
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
 *     writing an unsigned one.
 */

#include <zephyr/device.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/drivers/misc/boot_agm.h>
#include <zephyr/drivers/serial/uart_emul.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/crc.h>
#include <zephyr/ztest.h>

#include <string.h>

#define BOOT_DEV DEVICE_DT_GET(DT_NODELABEL(boot))
#define EMUL     DEVICE_DT_GET(DT_NODELABEL(emul_uart))

extern const unsigned char agm_signed_ok[];
extern const unsigned char agm_signed_other_key[];
extern const unsigned char agm_signed_other_slot[];
extern const unsigned char agm_image_raw[];
extern const unsigned char agm_ecdsa_signed[];
extern const size_t agm_signed_ok_len;
extern const size_t agm_signed_other_key_len;
extern const size_t agm_signed_other_slot_len;
extern const size_t agm_image_raw_len;
extern const size_t agm_ecdsa_signed_len;

/* The fixtures have to be the profile this build verifies with, or the
 * "accepted" case below would be testing nothing. 270 is the PKCS#1 DER of an
 * RSA-2048 key; the ECDSA fixture is 64. */
BUILD_ASSERT(sizeof(agm_boot_pubkey) == 270,
	     "the RSA profile keeps the 270-byte PKCS#1 DER public key");

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

ZTEST(boot_agm_rsa, test_accepts_a_properly_signed_image)
{
	place(AGM_BOOT_TARGET_STORE_A, agm_signed_ok, agm_signed_ok_len);

	{
		int ret = agm_boot_image_verify(BOOT_DEV, AGM_BOOT_TARGET_STORE_A,
						(uint32_t)agm_signed_ok_len);

		zassert_equal(ret, 0, "verify returned %d", ret);
	}
}

ZTEST(boot_agm_rsa, test_rejects_a_tampered_payload)
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

ZTEST(boot_agm_rsa, test_rejects_another_key)
{
	place(AGM_BOOT_TARGET_STORE_A, agm_signed_other_key, agm_signed_other_key_len);

	zassert_equal(agm_boot_image_verify(BOOT_DEV, AGM_BOOT_TARGET_STORE_A,
					    (uint32_t)agm_signed_other_key_len),
		      -EACCES, "the keyhash TLV has to name a trusted key");
}

ZTEST(boot_agm_rsa, test_rejects_a_raw_image)
{
	place(AGM_BOOT_TARGET_STORE_A, agm_image_raw, agm_image_raw_len);

	zassert_equal(agm_boot_image_verify(BOOT_DEV, AGM_BOOT_TARGET_STORE_A,
					    (uint32_t)agm_image_raw_len),
		      -EINVAL, "a raw image has no MCUboot header");
}

/*
 * Rewrite the signature TLV type to the *other* profile's (RSA2048_PSS 0x20
 * <-> ECDSA_SIG 0x22), leaving the digest, the KEYHASH and the signature bytes
 * as the trusted key signed them: the profile check is the only thing left
 * that can refuse it, from the RSA side.
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

ZTEST(boot_agm_rsa, test_rejects_a_container_of_the_other_profile)
{
	place(AGM_BOOT_TARGET_STORE_A, agm_ecdsa_signed, agm_ecdsa_signed_len);

	zassert_equal(agm_boot_image_verify(BOOT_DEV, AGM_BOOT_TARGET_STORE_A,
					    (uint32_t)agm_ecdsa_signed_len),
		      -EACCES, "an ECDSA-signed container is not signed by this key");
}

ZTEST(boot_agm_rsa, test_rejects_a_relabelled_signature_tlv)
{
	static uint8_t patched[2048];

	zassert_true(agm_signed_ok_len <= sizeof(patched));
	stamp_signature_tlv_as(agm_signed_ok, (uint32_t)agm_signed_ok_len, patched,
			       0x20U /* RSA2048_PSS */, 0x22U /* ECDSA_SIG */);
	place(AGM_BOOT_TARGET_STORE_A, patched, agm_signed_ok_len);

	zassert_equal(agm_boot_image_verify(BOOT_DEV, AGM_BOOT_TARGET_STORE_A,
					    (uint32_t)agm_signed_ok_len),
		      -EINVAL, "this build takes RSA TLVs, not ECDSA ones");
}

/* The enforcement points: publish_upload() and store_boot() both call the
 * verifier, so a bad image neither reaches the record nor gets booted. */
ZTEST(boot_agm_rsa, test_publish_accepts_a_signed_image)
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

ZTEST(boot_agm_rsa, test_publish_refuses_a_raw_image)
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
ZTEST(boot_agm_rsa, test_publish_refuses_a_container_for_another_slot)
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
ZTEST(boot_agm_rsa, test_boot_refuses_a_store_rewritten_afterwards)
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

/* ---- the console upload protocol, and what it says when it says no ----
 *
 * Everything above calls the driver API directly, which is one layer below
 * what tools/agm_upload.py speaks. That layer has no native coverage in this
 * tree at all, and the one thing it is responsible for that the API is not is
 * *how a refusal is reported*: the bytes are in the store by the time the
 * verifier runs, so "the image was refused" and "the flash write failed" used
 * to come back as the same NAK code (3), which sent a dev board session chasing
 * the flash instead of the signature.
 *
 * The frames here are the host tool's, byte for byte (frames(): a 16-byte
 * header -- magic, command, wire target, reserved, then offset/length/CRC as
 * little-endian 32-bit fields -- followed by the payload).
 */

#define UP_MAGIC      0xA5U
#define UP_ACK        0x79U
#define UP_NAK        0x1FU
#define UP_CMD_DATA   0x01U
#define UP_CMD_FINISH 0x02U
#define UP_ERR_FLASH  3U
#define UP_ERR_REJECT 4U

static uint8_t up_rx[4096];
static uint32_t up_rx_len;
static uint8_t up_tx[4096];
static uint32_t up_tx_len;

static void up_put(const uint8_t *data, uint32_t len)
{
	zassert_true(up_rx_len + len <= sizeof(up_rx), "uart-emul RX is too small");
	memcpy(up_rx + up_rx_len, data, len);
	up_rx_len += len;
}

static void up_put32(uint32_t v)
{
	uint8_t b[4] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16),
			 (uint8_t)(v >> 24) };

	up_put(b, sizeof(b));
}

static void up_frame(uint8_t cmd, uint8_t target, uint32_t off,
		     const uint8_t *data, uint32_t len)
{
	uint8_t hdr[4] = { UP_MAGIC, cmd, target, 0U };

	up_put(hdr, sizeof(hdr));
	up_put32(off);
	up_put32(len);
	up_put32(crc32_ieee(data, len));
	up_put(data, len);
}

/* Upload @a len bytes and finish, which is where the verifier runs; the phase's
 * whole console output is left in up_tx (the console prints through the same
 * UART, so the caller looks at the tail). The DATA payloads are capped at the
 * driver's UP_MAX_PAYLOAD (AGM_BOOT_UPLOAD_CHUNK) -- a signed container is
 * bigger than one frame. */
static void up_upload(const uint8_t *data, uint32_t len)
{
	static uint8_t fin[16];
	uint32_t crc = crc32_ieee(data, len);
	uint32_t off;

	up_rx_len = 0U;
	for (off = 0U; off < len; off += AGM_BOOT_UPLOAD_CHUNK) {
		uint32_t n = MIN(AGM_BOOT_UPLOAD_CHUNK, len - off);

		up_frame(UP_CMD_DATA, 0U /* wire target 'a' */, off, data + off, n);
	}

	/* FINISH payload: total length, the host's CRC over the whole image, and
	 * the load/entry the host would like (the loader derives a container's
	 * entry from its header and overrules these). */
	memset(fin, 0, sizeof(fin));
	sys_put_le32(len, &fin[0]);
	sys_put_le32(crc, &fin[4]);
	up_frame(UP_CMD_FINISH, 0U, 0U, fin, sizeof(fin));

	uart_emul_flush_tx_data(EMUL);
	zassert_equal(uart_emul_put_rx_data(EMUL, up_rx, up_rx_len), up_rx_len,
		      "uart-emul RX fifo");
	agm_boot_upload_console(BOOT_DEV, AGM_BOOT_TARGET_STORE_A);

	memset(up_tx, 0, sizeof(up_tx));
	up_tx_len = uart_emul_get_tx_data(EMUL, up_tx, sizeof(up_tx));
	uart_emul_flush_tx_data(EMUL);
}

ZTEST(boot_agm_rsa, test_console_phase_acks_a_signed_container)
{
	up_upload(agm_signed_ok, (uint32_t)agm_signed_ok_len);

	/* The last frame's reply is the last byte the phase wrote. */
	zassert_true(up_tx_len > 0U);
	zassert_equal(up_tx[up_tx_len - 1U], UP_ACK,
		      "a properly signed container has to end on ACK");
}

ZTEST(boot_agm_rsa, test_console_phase_says_rejected_not_flash_error)
{
	/* A raw image: the bytes upload and CRC-check fine, so the only thing
	 * that can fail is the verifier -- and the code has to say so instead of
	 * reusing "flash write/verify failed" (UP_ERR_FLASH). */
	up_upload(agm_image_raw, (uint32_t)agm_image_raw_len);

	zassert_true(up_tx_len >= 2U);
	zassert_equal(up_tx[up_tx_len - 2U], UP_NAK, "a rejected image has to NAK");
	zassert_equal(up_tx[up_tx_len - 1U], UP_ERR_REJECT,
		      "and the code has to be the verifier's, not the flash's");
}

ZTEST_SUITE(boot_agm_rsa, NULL, NULL, NULL, NULL, NULL);
