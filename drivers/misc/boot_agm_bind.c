/*
 * Copyright (c) 2026 AgRV Contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Per-chip binding, device side (CONFIG_BOOT_AGM_BIND).
 *
 * The host half is tools/agm_bind.py. In short: the chip's UID is a
 * serial number (two public constants read it), the provisioned salt is
 * the secret half, and this file derives
 *
 *     key = HKDF-SHA256(ikm = UID, salt = salt, info = "agm-bind-v1", L = 32)
 *     tag = HMAC-SHA256(key, ih_ver ‖ SHA-256(header ‖ image))
 *
 * from both, over the same bytes the container's signature covers. The
 * known-answer test in tests/drivers/misc/boot_agm (and the matching vectors
 * in tools/tests/test_agm_bind.py) pin this file and the host tool to the same
 * bytes; the enforcement itself -- the BIND TLV the tag travels in, and the
 * two refusals -- is in boot_agm_verify.c (one place, both the publish and the
 * boot path).
 *
 * HMAC is built on tinycrypt's SHA-256 (the same library the image verifier
 * uses) rather than pulling in mbedTLS for one construction.
 */

#include <errno.h>

#include <zephyr/drivers/flash.h>
#include <zephyr/drivers/misc/boot_agm.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/crc.h>
#include <zephyr/sys/util.h>

#include <string.h>

#include <tinycrypt/sha256.h>

#include "boot_agm_priv.h"

#define BIND_SALT_MAGIC   0x424D4741U /* "AGMB" */
#define BIND_SALT_VERSION 1U
#define BIND_SALT_LEN     16U
#define BIND_KEY_LEN      32U
#define BIND_SECTOR_LEN   4096U

/* The container header is MCUboot's `image_header`
 * (bootloader/mcuboot/boot/bootutil/include/bootutil/image.h), little endian:
 *
 *   off  size  field
 *     0     4  ih_magic            0x96F3B83D
 *     4     4  ih_load_addr
 *     8     2  ih_hdr_size         <- read below
 *    10     2  ih_protect_tlv_size
 *    12     4  ih_img_size         <- read below
 *    16     4  ih_flags
 *    20     8  ih_ver              `{u8 major, u8 minor, u16 revision, u32 build}`
 *    28     4  _pad1
 *
 * The tag commits to ih_ver's own bytes rather than to a version string an
 * operator typed, so a "2.0" that imgtool padded to 2.0.0 cannot produce an
 * image this device refuses. (Written out field by field because the earlier
 * prose -- "load_addr (4+4), ... flags (4+4)" -- read as if the pairs covered
 * different fields than the offsets the code uses; verified against the header
 * above.) */
#define BIND_VERSION_OFF 20U
#define BIND_VERSION_LEN 8U

/* Where the salt is: the last sector of the on-die layout's chain, right
 * after the boot record (boot_agm_priv.h derives both from the sizes; the
 * two-flash family keeps a spelled-out address because its images live in the
 * NOR). No Kconfig address to keep in step with the layout any more. */
#define BIND_SALT_OFFSET  BOOT_SALT_OFF

uint32_t agm_boot_bind_salt_addr(void)
{
	return (uint32_t)INT_FLASH_BASE + BIND_SALT_OFFSET;
}

/*
 * The binding check runs on whatever stack the calling path has, and on the
 * upload path that is mcumgr's transport work queue: the CBOR/framing chain,
 * the container hash and the signature verification are already on it before
 * this file adds an HMAC. That stack has to be big enough for all of them
 * together -- on a bound loader with the Zephyr default
 * (2048): the overflow corrupted a saved return address and the board faulted
 * with an illegal instruction whose `mepc` was in RAM below the stack, right
 * after the (correctly) printed refusal; 4096 runs the same upload cleanly.
 * Failing at build time is the point: this is silent memory corruption, and
 * only the path that is deepest hits it.
 */
#if defined(CONFIG_BOOT_AGM_SMP)
BUILD_ASSERT(CONFIG_MCUMGR_TRANSPORT_WORKQUEUE_STACK_SIZE >= 4096,
	     "CONFIG_BOOT_AGM_BIND needs a bigger mcumgr transport workqueue "
	     "stack (the verification chain runs on it; see boot_agm_bind.c)");
#endif

/*
 * Where the binding code gets the UID. Weak on purpose (the same pattern the
 * trial-boot hooks use): on a board it is the real flash-controller read, and
 * a native test can override it with the dev board chip's ID instead of poking
 * registers that do not exist on native_sim.
 */
__weak int agm_boot_bind_uid(uint8_t uid[AGM_BOOT_UID_LEN])
{
	return agm_boot_unique_id(uid);
}

#define SHA256_LEN 32U
#define SHA256_BLOCK 64U

static void hmac_sha256(const uint8_t *key, uint32_t key_len,
			const uint8_t *msg, uint32_t msg_len, uint8_t out[SHA256_LEN])
{
	uint8_t k[SHA256_BLOCK];
	uint8_t ipad[SHA256_BLOCK];
	uint8_t opad[SHA256_BLOCK];
	uint8_t inner[SHA256_LEN];
	struct tc_sha256_state_struct sha;

	memset(k, 0, sizeof(k));
	if (key_len > SHA256_BLOCK) {
		tc_sha256_init(&sha);
		tc_sha256_update(&sha, key, key_len);
		tc_sha256_final(k, &sha);
	} else {
		memcpy(k, key, key_len);
	}
	for (uint32_t i = 0U; i < SHA256_BLOCK; i++) {
		ipad[i] = k[i] ^ 0x36U;
		opad[i] = k[i] ^ 0x5cU;
	}

	tc_sha256_init(&sha);
	tc_sha256_update(&sha, ipad, sizeof(ipad));
	tc_sha256_update(&sha, msg, msg_len);
	tc_sha256_final(inner, &sha);

	tc_sha256_init(&sha);
	tc_sha256_update(&sha, opad, sizeof(opad));
	tc_sha256_update(&sha, inner, sizeof(inner));
	tc_sha256_final(out, &sha);
}

static int read_salt(uint8_t salt[BIND_SALT_LEN])
{
	uint8_t head[28];
	uint32_t magic, version, crc;
	int ret = flash_read(INT_FLASH, BIND_SALT_OFFSET, head, sizeof(head));

	if (ret < 0) {
		return ret;
	}
	magic = sys_get_le32(&head[0]);
	version = sys_get_le32(&head[4]);
	crc = sys_get_le32(&head[24]);
	if (magic != BIND_SALT_MAGIC || version != BIND_SALT_VERSION) {
		return -ENOENT;
	}
	if (crc != crc32_ieee(head, 24U)) {
		return -EBADMSG;
	}
	memcpy(salt, &head[8], BIND_SALT_LEN);
	return 0;
}

int agm_boot_bind_key(uint8_t key[BIND_KEY_LEN], bool *present)
{
	uint8_t uid[AGM_BOOT_UID_LEN];
	uint8_t salt[BIND_SALT_LEN];
	uint8_t prk[SHA256_LEN];
	uint8_t info[] = "agm-bind-v1";
	/* sizeof() - 1: the C string's NUL is not part of the HKDF info string
	 * (the host signs over b"agm-bind-v1" and this has to match). */
	uint32_t info_len = sizeof(info) - 1U;
	/* One expand block: T(1) = HMAC(prk, T(0) || info || 0x01), and T(0) for
	 * the first block is the *empty* string -- not 32 zero bytes (that was
	 * the first cut, and the known-answer test caught it). */
	uint8_t block[sizeof(info) + 1U];
	uint32_t block_len = info_len + 1U;
	int ret;

	if (present != NULL) {
		*present = false;
	}
	ret = agm_boot_bind_uid(uid);
	if (ret < 0) {
		return ret;
	}
	ret = read_salt(salt);
	if (ret < 0) {
		return ret;
	}

	/* HKDF: extract with the salt, expand one block (32 B needs exactly one). */
	hmac_sha256(salt, sizeof(salt), uid, sizeof(uid), prk);
	memcpy(&block[0], info, info_len);
	block[info_len] = 1U; /* the T(1) counter */
	hmac_sha256(prk, sizeof(prk), block, block_len, key);

	if (present != NULL) {
		*present = true;
	}
	return 0;
}

int boot_agm_bind_tag_digest(const uint8_t version[BIND_VERSION_LEN],
			     const uint8_t digest[SHA256_LEN],
			     uint8_t tag[SHA256_LEN])
{
	uint8_t key[BIND_KEY_LEN];
	uint8_t msg[BIND_VERSION_LEN + SHA256_LEN];
	int ret;

	memcpy(msg, version, BIND_VERSION_LEN);
	memcpy(&msg[BIND_VERSION_LEN], digest, SHA256_LEN);

	ret = agm_boot_bind_key(key, NULL);
	if (ret < 0) {
		/* -ENOENT (no salt provisioned) is the one a caller has to tell
		 * apart from "the tag does not match": nothing on this chip can
		 * be bound until production has written the salt, so the
		 * verifier refuses the image instead of trusting a zero key. */
		return ret;
	}
	hmac_sha256(key, sizeof(key), msg, sizeof(msg), tag);
	return 0;
}

int agm_boot_bind_fingerprint(uint8_t fp[AGM_BOOT_BIND_FP_LEN])
{
	uint8_t key[BIND_KEY_LEN];
	uint8_t digest[SHA256_LEN];
	struct tc_sha256_state_struct sha;
	int ret = agm_boot_bind_key(key, NULL);

	if (ret < 0) {
		return ret;
	}
	tc_sha256_init(&sha);
	tc_sha256_update(&sha, key, sizeof(key));
	tc_sha256_final(digest, &sha);
	memcpy(fp, digest, AGM_BOOT_BIND_FP_LEN);
	return 0;
}

int agm_boot_bind_tag(const uint8_t *container, uint32_t len, uint8_t tag[SHA256_LEN])
{
	uint8_t digest[SHA256_LEN];
	uint32_t hdr_size, img_size, body;
	struct tc_sha256_state_struct sha;

	if (container == NULL || len < 32U) {
		return -EINVAL;
	}
	if (sys_get_le32(&container[0]) != 0x96F3B83DU) {
		return -EINVAL;
	}
	hdr_size = sys_get_le16(&container[8]);
	img_size = sys_get_le32(&container[12]);
	body = hdr_size + img_size;
	if (body > len) {
		return -EINVAL;
	}

	tc_sha256_init(&sha);
	tc_sha256_update(&sha, container, body);
	tc_sha256_final(digest, &sha);

	return boot_agm_bind_tag_digest(&container[BIND_VERSION_OFF], digest, tag);
}
