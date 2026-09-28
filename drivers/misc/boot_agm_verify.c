/*
 * Copyright (c) 2026 AgRV Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief AgRV2K bootloader / DFU driver -- image and fabric verification.
 *
 * Everything that decides whether an image may be published or booted. Both
 * paths call in here, which is what makes the verdict the same on both: a bad
 * image never reaches the record, and one tampered with afterwards is refused
 * at the next boot. The container walk is the driver's own (no MCUboot headers)
 * and the two signed profiles share it -- see the comment below for the layout
 * and the three places they differ.
 */

#include <zephyr/device.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/drivers/misc/agm_bitstream.h>
#include <zephyr/sys/crc.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

#include <string.h>

#include "boot_agm_priv.h"

#if defined(AGM_HAVE_TINYCRYPT_ECDSA)
#include <tinycrypt/ecc.h>
#include <tinycrypt/ecc_dsa.h>
#include <tinycrypt/sha256.h>
#endif
#if defined(CONFIG_BOOT_AGM_SIG_RSA2048_PSS)
#include <psa/crypto.h>
#endif

#if defined(AGM_HAVE_TINYCRYPT_ECDSA)
/* tinycrypt wants the raw r||s pair, MCUboot's signature TLV carries the DER
 * (SEQUENCE of two INTEGERs, either of which may have a leading 0x00 because
 * DER keeps integers positive). This is the conversion bootutil's ECDSA glue
 * performs, written out here with the bounds checks a host-supplied blob
 * needs. It lives outside the image-verification section because the command
 * authorization uses it in builds that carry no image profile at all. */
static int der_integer(const uint8_t *der, uint32_t len, uint32_t *off,
			     uint8_t out[32])
{
	uint32_t n;

	if (*off + 2U > len || der[*off] != 0x02U) {
		return -EINVAL;
	}
	*off += 1U;
	n = der[*off];
	*off += 1U;
	if (n == 0U || n > 33U || *off + n > len) {
		return -EINVAL;
	}
	if (n == 33U) {
		if (der[*off] != 0x00U) {
			return -EINVAL;
		}
		*off += 1U;
		n = 32U;
	}
	memset(out, 0, 32U);
	memcpy(out + (32U - n), der + *off, n);
	*off += n;
	return 0;
}

int boot_agm_sig_der_to_raw(const uint8_t *der, uint32_t len, uint8_t raw[64])
{
	uint32_t off;
	uint32_t seq;

	if (len < 8U || der[0] != 0x30U || (der[1] & 0x80U) != 0U) {
		return -EINVAL;
	}
	seq = der[1];
	if (2U + seq > len) {
		return -EINVAL;
	}
	off = 2U;
	if (der_integer(der, 2U + seq, &off, raw) < 0 ||
	    der_integer(der, 2U + seq, &off, raw + 32) < 0) {
		return -EINVAL;
	}
	return off == 2U + seq ? 0 : -EINVAL;
}
#endif /* AGM_HAVE_TINYCRYPT_ECDSA */

/* ---- image verification --------------------------------------------- */

/*
 * With a signature profile set (CONFIG_BOOT_AGM_SIG_ECDSA_P256 /
 * _RSA2048_PSS) or with CONFIG_BOOT_AGM_BITSTREAM_SIGNED, an image has to be
 * an MCUboot container and is checked twice: on publish (so a bad image never
 * reaches the record) and again before every boot (so an image that was
 * tampered with after publishing is refused).
 *
 * The container is the `imgtool sign` output -- header + image + TLV area --
 * and the layout below is MCUboot's (boot/bootutil/include/bootutil/image.h,
 * all fields little endian):
 *
 *   image_header  { ih_magic 0x96f3b83d, ih_load_addr, ih_hdr_size,
 *                   ih_protect_tlv_size, ih_img_size, ih_flags, ih_ver, pin }
 *   <image, ih_img_size bytes>
 *   image_tlv_info { it_magic 0x6907, it_tlv_tot }     <- 8-byte aligned
 *   image_tlv      { it_type, it_len } + data          <- repeated
 *
 * The three TLVs that matter here are KEYHASH (0x01), SHA256 (0x10) and the
 * profile's signature TLV. Verification computes SHA-256 over header + image,
 * checks the SHA256 TLV against it, checks the KEYHASH TLV against the trusted
 * key, and then verifies the signature over that hash. The constants are copied
 * rather than included so the driver does not depend on MCUboot's headers (the
 * layout is frozen; MCUboot changes it only with a new magic).
 *
 * The two profiles differ in three places, and *only* in three places -- the
 * rest of the walk is shared:
 *
 *  - the signature TLV type (0x22 ECDSA_SIG vs 0x20 RSA2048_PSS) and its
 *    encoding (DER r||s, ~70 B, vs the raw 256-byte big-endian signature);
 *  - the trusted key blob (raw X||Y, 64 B, vs PKCS#1 RSAPublicKey DER, 270 B);
 *  - therefore the bytes KEYHASH is taken over. imgtool hashes the
 *    SubjectPublicKeyInfo for ECDSA but the PKCS#1 DER for RSA -- both are the
 *    encoding its own `getpubhash` uses for that key type, and they are *not*
 *    the same encoding (a key that verifies under one is rejected by the
 *    other).
 */

/* The numbers above are defined outside the profile guard on purpose: the
 * command authorization needs the ECDSA signature sizes in a build that
 * carries no image profile at all (LOCK_PRODUCTION over BOOT_AGM_SIG_NONE). */
#if defined(CONFIG_BOOT_AGM_SIG_ECDSA_P256) || \
	defined(CONFIG_BOOT_AGM_SIG_RSA2048_PSS) || \
	defined(CONFIG_BOOT_AGM_BITSTREAM_SIGNED)

/*
 * A verification profile: which signature TLV it accepts, how the trusted key
 * blob hashes into the container's KEYHASH TLV, how a signature is checked,
 * and (for a fabric container) the size and run address the header has to
 * declare. The application profile comes from CONFIG_BOOT_AGM_SIGNATURE; the
 * fabric profile is always ECDSA P-256 and independent of it.
 */
struct img_src;

struct img_profile {
	const char *name;
	const char *key_name; /* the symbol a missing key is reported as */
	uint16_t sig_tlv;
	uint32_t key_len;
	int (*hash)(const struct img_src *src, uint32_t len, uint8_t hash[32]);
	int (*key_hash)(const uint8_t *key, uint8_t hash[32]);
	int (*sig_check)(const uint8_t *key, const uint8_t hash[32],
			 const uint8_t *sig, uint32_t len);
	uint32_t exact_img_size;    /* 0: any non-zero size is acceptable */
	uint32_t require_load_addr; /* 0: any run address is acceptable */
	/* CONFIG_BOOT_AGM_BIND: the container has to carry this chip's binding
	 * tag (the application profile only -- binding a fabric image is a
	 * different product decision, and the bitstream is not bound). */
	bool bound;
};

struct mcuboot_image_header {
	uint32_t ih_magic;
	uint32_t ih_load_addr;
	uint16_t ih_hdr_size;
	uint16_t ih_protect_tlv_size;
	uint32_t ih_img_size;
	uint32_t ih_flags;
	uint8_t ih_ver[8];
	uint32_t pin;
} __packed;

struct mcuboot_tlv_info {
	uint16_t it_magic;
	uint16_t it_tlv_tot;
} __packed;

struct mcuboot_tlv {
	uint16_t it_type;
	uint16_t it_len;
} __packed;

/* The trusted fabric key: same 64-byte blob, but for the bitstream slots. Its
 * weak default is all zeroes too, and an all-zero one falls back to a P-256
 * application key (bs_trusted_key() below), so the common "one keypair for
 * both" setup needs no second -D. */
#if defined(CONFIG_BOOT_AGM_BITSTREAM_SIGNED)
__weak const uint8_t agm_boot_bitstream_pubkey[MCUBOOT_BS_KEY_LEN];
#endif

/* What the last verification looked at, for the console's diagnostics. */
static uint32_t img_verify_len;
static bool img_verify_ok;

/* Profile pieces, defined further down; the profiles themselves reference
 * them, and the TLV walk takes a profile. */
#if defined(CONFIG_BOOT_AGM_SIG_RSA2048_PSS)
static int img_hash_psa(const struct img_src *src, uint32_t len, uint8_t hash[32]);
static int img_key_hash_rsa(const uint8_t *key, uint8_t hash[32]);
static int img_sig_check_rsa(const uint8_t *key, const uint8_t hash[32],
			     const uint8_t *sig, uint32_t len);
#endif
#if defined(CONFIG_BOOT_AGM_SIG_ECDSA_P256) || defined(CONFIG_BOOT_AGM_BITSTREAM_SIGNED)
static int img_hash_tc(const struct img_src *src, uint32_t len, uint8_t hash[32]);
static int img_key_hash_ecdsa(const uint8_t *key, uint8_t hash[32]);
static int img_sig_check_ecdsa(const uint8_t *key, const uint8_t hash[32],
			       const uint8_t *sig, uint32_t len);
#endif

#if defined(CONFIG_BOOT_AGM_SIG_ECDSA_P256)
static const struct img_profile app_profile = {
	.name = "ECDSA P-256",
	.key_name = "agm_boot_pubkey",
	.sig_tlv = MCUBOOT_TLV_ECDSA_SIG,
	.key_len = MCUBOOT_ECDSA_KEY_LEN,
	.hash = img_hash_tc,
	.key_hash = img_key_hash_ecdsa,
	.sig_check = img_sig_check_ecdsa,
	.bound = IS_ENABLED(CONFIG_BOOT_AGM_BIND),
};
#elif defined(CONFIG_BOOT_AGM_SIG_RSA2048_PSS)
static const struct img_profile app_profile = {
	.name = "RSA-2048-PSS",
	.key_name = "agm_boot_pubkey",
	.sig_tlv = MCUBOOT_TLV_RSA2048_PSS,
	.key_len = MCUBOOT_RSA_KEY_LEN,
	.hash = img_hash_psa,
	.key_hash = img_key_hash_rsa,
	.sig_check = img_sig_check_rsa,
	.bound = IS_ENABLED(CONFIG_BOOT_AGM_BIND),
};
#endif

#if defined(CONFIG_BOOT_AGM_BITSTREAM_SIGNED)
/* The fabric profile: ECDSA P-256 whatever the application uses, because the
 * check runs in the boot path (see the header). Its container has to declare
 * the full 99944-byte fabric and the one canonical run address. */
static const struct img_profile bs_profile = {
	.name = "ECDSA P-256",
	.key_name = "agm_boot_bitstream_pubkey",
	.sig_tlv = MCUBOOT_TLV_ECDSA_SIG,
	.key_len = MCUBOOT_ECDSA_KEY_LEN,
	.hash = img_hash_tc,
	.key_hash = img_key_hash_ecdsa,
	.sig_check = img_sig_check_ecdsa,
	.exact_img_size = AGM_BITSTREAM_IMAGE_LEN,
	.require_load_addr = AGM_BITSTREAM_CONTAINER_ADDR,
};
#endif

/*
 * Where the bytes under verification are.
 *
 * Application images live in a target (a store, the on-die slot) and are read
 * through the driver's own window, i.e. through the flash driver. Fabric
 * images are named by their absolute address instead, and have to be readable
 * *before* that driver exists: `agrv2k_fcb_program()` verifies the fabric at
 * PRE_KERNEL_1, where the on-die flash is only reachable through the SoC's
 * memory map (0x80000000). Once the driver is up -- the upload path -- the
 * same address is read through it, so there is one verifier and one set of
 * bytes, not two.
 */
struct img_src {
	enum agm_boot_target target; /* valid when !by_addr */
	uint32_t addr;               /* absolute address, when by_addr */
	bool by_addr;
	bool direct;                 /* read memory, not the flash device */
};

static struct img_src img_src_target(enum agm_boot_target target)
{
	return (struct img_src){ .target = target };
}

#if defined(CONFIG_BOOT_AGM_BITSTREAM_SIGNED)
static struct img_src img_src_slot(uint32_t addr)
{
	return (struct img_src){
		.addr = addr,
		.by_addr = true,
		.direct = !device_is_ready(INT_FLASH),
	};
}
#endif

static int img_read(const struct img_src *src, uint32_t off, uint8_t *buf,
		    uint32_t len)
{
	if (src->direct) {
		memcpy(buf, (const void *)(uintptr_t)(src->addr + off), len);
		return 0;
	}
	if (src->by_addr) {
		return flash_read(INT_FLASH, src->addr - INT_FLASH_BASE + off,
				  buf, len);
	}
	return boot_agm_target_read(src->target, off, buf, len);
}

/* SHA-256 over [0, len) of a source, in SLOT_CHUNK reads. ECDSA hashes with
 * tinycrypt, RSA with PSA (see the include block above for why they differ);
 * a build that links both -- an RSA application with a signed fabric -- has
 * both, and the profile decides which one runs. */
#if defined(CONFIG_BOOT_AGM_SIG_RSA2048_PSS)
static int img_hash_psa(const struct img_src *src, uint32_t len, uint8_t hash[32])
{
	psa_hash_operation_t op = PSA_HASH_OPERATION_INIT;
	uint8_t buf[SLOT_CHUNK];
	psa_status_t st;
	size_t out_len = 0U;

	st = psa_crypto_init();
	if (st != PSA_SUCCESS) {
		printk("loader: PSA crypto init failed (%d)\n", (int)st);
		return -EIO;
	}
	st = psa_hash_setup(&op, PSA_ALG_SHA_256);
	if (st != PSA_SUCCESS) {
		return -EIO;
	}
	for (uint32_t done = 0U; done < len; done += SLOT_CHUNK) {
		uint32_t chunk = MIN(SLOT_CHUNK, len - done);
		int ret = img_read(src, done, buf, chunk);

		if (ret < 0) {
			(void)psa_hash_abort(&op);
			return ret;
		}
		if (psa_hash_update(&op, buf, chunk) != PSA_SUCCESS) {
			(void)psa_hash_abort(&op);
			return -EIO;
		}
	}
	st = psa_hash_finish(&op, hash, 32U, &out_len);
	if (st != PSA_SUCCESS || out_len != 32U) {
		printk("loader: PSA SHA-256 failed (%d)\n", (int)st);
		return -EIO;
	}
	return 0;
}
#endif /* CONFIG_BOOT_AGM_SIG_RSA2048_PSS */

#if defined(CONFIG_BOOT_AGM_SIG_ECDSA_P256) || defined(CONFIG_BOOT_AGM_BITSTREAM_SIGNED)
static int img_hash_tc(const struct img_src *src, uint32_t len, uint8_t hash[32])
{
	struct tc_sha256_state_struct sha;
	uint8_t buf[SLOT_CHUNK];

	tc_sha256_init(&sha);
	for (uint32_t done = 0U; done < len; done += SLOT_CHUNK) {
		uint32_t chunk = MIN(SLOT_CHUNK, len - done);
		int ret = img_read(src, done, buf, chunk);

		if (ret < 0) {
			return ret;
		}
		tc_sha256_update(&sha, buf, chunk);
	}
	tc_sha256_final(hash, &sha);
	return 0;
}
#endif /* ECDSA || BITSTREAM_SIGNED */

/* Check the container's signature TLV (already located by the TLV walk) over
 * the image hash, against @a key. Returns 0 when the signature is genuine,
 * -EINVAL when the TLV is not a well-formed signature of this profile, -EACCES
 * when it is well-formed but does not verify.
 *
 * The key is a parameter rather than `agm_boot_pubkey` because the fabric has
 * its own trust anchor (agm_boot_bitstream_pubkey) and the same code has to
 * serve both -- including in a build whose *application* profile is RSA, where
 * this ECDSA verifier exists only for the fabric. The command authorization
 * passes the same key in, which is why the guard here is "the curve is
 * linked" rather than the two profile options. */
#if defined(AGM_HAVE_TINYCRYPT_ECDSA)
static int img_sig_check_ecdsa(const uint8_t *key, const uint8_t hash[32],
			       const uint8_t *sig, uint32_t len)
{
	uint8_t raw[MCUBOOT_ECDSA_SIG_LEN];

	/* MCUboot stores the DER form (typically 70-72 B), while tinycrypt
	 * verifies the raw r||s pair. */
	if (len < 8U || len > MCUBOOT_DER_MAX) {
		printk("loader: signature TLV has %u bytes\n", len);
		return -EINVAL;
	}
	if (boot_agm_sig_der_to_raw(sig, len, raw) < 0) {
		printk("loader: signature TLV is not valid DER\n");
		return -EINVAL;
	}
	if (uECC_verify(key, hash, 32U, raw, uECC_secp256r1()) != 1) {
		printk("loader: signature is not valid\n");
		return -EACCES;
	}
	return 0;
}
#endif /* ECDSA || BITSTREAM_SIGNED */

#if defined(CONFIG_BOOT_AGM_SIG_RSA2048_PSS)
static int img_sig_check_rsa(const uint8_t *key, const uint8_t hash[32],
			     const uint8_t *sig, uint32_t len)
{
	psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
	psa_key_id_t id = 0;
	psa_status_t st;

	/* RSA-2048-PSS: the TLV carries the raw 256-byte signature. It maps
	 * one-to-one onto what PSA wants, so there is no DER to unwrap here
	 * (the *key* is DER, but that is `psa_import_key`'s business below). */
	if (len != MCUBOOT_RSA_SIG_LEN) {
		printk("loader: signature TLV has %u bytes, RSA-2048 needs %u\n",
		       len, MCUBOOT_RSA_SIG_LEN);
		return -EINVAL;
	}
	st = psa_crypto_init();
	if (st != PSA_SUCCESS) {
		printk("loader: PSA crypto init failed (%d)\n", (int)st);
		return -EIO;
	}
	psa_set_key_type(&attr, PSA_KEY_TYPE_RSA_PUBLIC_KEY);
	psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_VERIFY_HASH);
	psa_set_key_algorithm(&attr, PSA_ALG_RSA_PSS(PSA_ALG_SHA_256));
	st = psa_import_key(&attr, key, MCUBOOT_RSA_KEY_LEN, &id);
	if (st != PSA_SUCCESS) {
		printk("loader: the trusted key is not an RSA public key (%d)\n",
		       (int)st);
		psa_reset_key_attributes(&attr);
		return -EINVAL;
	}
	psa_reset_key_attributes(&attr);
	st = psa_verify_hash(id, PSA_ALG_RSA_PSS(PSA_ALG_SHA_256), hash, 32U,
			     sig, len);
	psa_destroy_key(id);
	if (st != PSA_SUCCESS) {
		printk("loader: signature is not valid (psa %d)\n", (int)st);
		return st == PSA_ERROR_INVALID_SIGNATURE ? -EACCES : -EIO;
	}
	return 0;
}
#endif /* CONFIG_BOOT_AGM_SIG_RSA2048_PSS */

/* Walk the TLV area and check the three TLVs that matter. */
static int img_check_tlvs(const struct img_src *src, uint32_t tlv_off,
			  uint32_t file_len, const uint8_t hash[32],
			  const uint8_t version[8], const struct img_profile *prof,
			  const uint8_t *key)
{
	struct mcuboot_tlv_info info;
	uint8_t keyhash[32];
	bool have_hash = false;
	bool have_sig = false;
#if defined(CONFIG_BOOT_AGM_BIND)
	bool have_bind = false;
#endif
	uint32_t off;
	int ret;

#if !defined(CONFIG_BOOT_AGM_BIND)
	/* Nothing else in the walk reads the header's `ih_ver`; the binding
	 * check below is the one caller that does. */
	ARG_UNUSED(version);
#endif

	if (tlv_off + sizeof(info) > file_len) {
		printk("loader: image has no TLV area\n");
		return -EINVAL;
	}
	ret = img_read(src, tlv_off, (uint8_t *)&info, sizeof(info));
	if (ret < 0) {
		return ret;
	}
	if (info.it_magic != MCUBOOT_TLV_INFO_MAGIC ||
	    info.it_tlv_tot < sizeof(info) ||
	    tlv_off + info.it_tlv_tot > file_len) {
		printk("loader: bad TLV area (magic 0x%04x, %u B)\n",
		       info.it_magic, info.it_tlv_tot);
		return -EINVAL;
	}

	/* The keyhash is SHA-256 of the raw public key, exactly like imgtool's
	 * `getpubhash`, so a build with the wrong key fails here and not only in
	 * the signature check. */
	ret = prof->key_hash(key, keyhash);
	if (ret < 0) {
		return ret;
	}

	for (off = tlv_off + sizeof(info); off + sizeof(struct mcuboot_tlv) <=
	     tlv_off + info.it_tlv_tot;) {
		struct mcuboot_tlv tlv;

		ret = img_read(src, off, (uint8_t *)&tlv, sizeof(tlv));
		if (ret < 0) {
			return ret;
		}
		off += sizeof(tlv);
		if (off + tlv.it_len > tlv_off + info.it_tlv_tot) {
			printk("loader: TLV 0x%02x runs past the TLV area\n", tlv.it_type);
			return -EINVAL;
		}

		/* The signature TLV is the profile's, so this is a chain of
		 * comparisons rather than a switch. */
		if (tlv.it_type == MCUBOOT_TLV_SHA256) {
			if (tlv.it_len != sizeof(keyhash)) {
				printk("loader: SHA256 TLV has %u bytes\n", tlv.it_len);
				return -EINVAL;
			}
			ret = img_read(src, off, keyhash, tlv.it_len);
			if (ret < 0) {
				return ret;
			}
			if (memcmp(keyhash, hash, sizeof(keyhash)) != 0) {
				printk("loader: image digest does not match its SHA256 TLV\n");
				return -EACCES;
			}
			have_hash = true;
		} else if (tlv.it_type == MCUBOOT_TLV_KEYHASH) {
			if (tlv.it_len != sizeof(keyhash)) {
				printk("loader: KEYHASH TLV has %u bytes\n", tlv.it_len);
				return -EINVAL;
			}
			ret = img_read(src, off, keyhash, tlv.it_len);
			if (ret < 0) {
				return ret;
			}
			{
				uint8_t want[32];

				ret = prof->key_hash(key, want);
				if (ret < 0) {
					return ret;
				}
				if (memcmp(keyhash, want, sizeof(want)) != 0) {
					printk("loader: image was signed by an unknown key\n");
					return -EACCES;
				}
			}
		} else if (tlv.it_type == prof->sig_tlv) {
			/* The ECDSA TLV carries the DER signature (~70-72 B) and
			 * the RSA one the raw 256-byte signature; either fits in
			 * one read. */
			uint8_t sig[MCUBOOT_SIG_TLV_MAX];

			if (tlv.it_len > sizeof(sig)) {
				printk("loader: signature TLV has %u bytes\n", tlv.it_len);
				return -EINVAL;
			}
			ret = img_read(src, off, sig, tlv.it_len);
			if (ret < 0) {
				return ret;
			}
			ret = prof->sig_check(key, hash, sig, tlv.it_len);
			if (ret < 0) {
				return ret;
			}
			have_sig = true;
#if defined(CONFIG_BOOT_AGM_BIND)
		} else if (prof->bound && tlv.it_type == MCUBOOT_TLV_BIND) {
			/* The per-chip half: the same tag production embedded
			 * (tools/agm_bind.py `embed`) over the header's version
			 * and the digest the signature above just covered. A
			 * copy of another chip's flash carries another chip's
			 * salt, so it recomputes a different tag here -- and
			 * the salt is the secret half, so it cannot be
			 * recomputed without it.
			 *
			 * -ENOENT from the KDF (no salt provisioned) is
			 * reported as "not bound" too: an unprovisioned board
			 * has nothing that could match this tag. */
			uint8_t want[AGM_BOOT_BIND_KEY_LEN];
			uint8_t bind[AGM_BOOT_BIND_KEY_LEN];

			if (tlv.it_len != AGM_BOOT_BIND_KEY_LEN) {
				printk("loader: BIND TLV has %u bytes\n", tlv.it_len);
				return -EINVAL;
			}
			ret = img_read(src, off, bind, tlv.it_len);
			if (ret < 0) {
				return ret;
			}
			if (boot_agm_bind_tag_digest(version, hash, want) < 0) {
				printk("loader: this chip has no binding salt, so it "
				       "cannot run an image bound to it\n");
				return AGM_BOOT_E_UNBOUND;
			}
			if (memcmp(bind, want, sizeof(want)) != 0) {
				printk("loader: the image is bound to a different "
				       "chip (or another release)\n");
				return AGM_BOOT_E_UNBOUND;
			}
			have_bind = true;
#endif /* CONFIG_BOOT_AGM_BIND */
		}
		/* Any other TLV is none of our business. */
		off += tlv.it_len;
	}

	if (!have_hash) {
		printk("loader: image has no SHA256 TLV\n");
		return -EINVAL;
	}
	if (!have_sig) {
		printk("loader: image has no signature TLV\n");
		return -EINVAL;
	}
#if defined(CONFIG_BOOT_AGM_BIND)
	if (prof->bound && !have_bind) {
		printk("loader: image carries no binding tag (this build runs only "
		       "images bound to this chip)\n");
		return AGM_BOOT_E_UNBOUND;
	}
#endif
	return 0;
}

/* The bytes IMAGE_TLV_KEYHASH is the SHA-256 of. imgtool hashes a *different*
 * encoding per key type -- the SubjectPublicKeyInfo DER for ECDSA
 * (imgtool/keys/ecdsa.py: get_public_bytes()) and the PKCS#1 RSAPublicKey DER
 * for RSA (imgtool/keys/rsa.py) -- so the loader has to reproduce that
 * encoding, not just hash the blob it keeps the key in. Both are checked
 * against imgtool's own output in tests/drivers/misc/boot_agm_{ecdsa,rsa}. */
#if defined(CONFIG_BOOT_AGM_SIG_ECDSA_P256) || defined(CONFIG_BOOT_AGM_BITSTREAM_SIGNED)
/* The SPKI around a P-256 point is a fixed prefix followed by 0x04||X||Y, so
 * the loader can build it from the 64 bytes it keeps. */
static const uint8_t mcuboot_p256_spki_prefix[] = {
	0x30, 0x59, 0x30, 0x13, 0x06, 0x07, 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x02, 0x01,
	0x06, 0x08, 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x03, 0x01, 0x07, 0x03, 0x42, 0x00,
};

static int img_key_hash_ecdsa(const uint8_t *key, uint8_t hash[32])
{
	static const uint8_t point[1] = { 0x04U };
	struct tc_sha256_state_struct sha;

	tc_sha256_init(&sha);
	tc_sha256_update(&sha, mcuboot_p256_spki_prefix, sizeof(mcuboot_p256_spki_prefix));
	tc_sha256_update(&sha, point, sizeof(point));
	tc_sha256_update(&sha, key, MCUBOOT_ECDSA_KEY_LEN);
	tc_sha256_final(hash, &sha);
	return 0;
}
#endif /* ECDSA || BITSTREAM_SIGNED */

#if defined(CONFIG_BOOT_AGM_SIG_RSA2048_PSS)
/* RSA: the 270-byte PKCS#1 DER is already exactly what imgtool hashes, and it
 * is also what `psa_import_key()` below takes, so one blob serves both. */
static int img_key_hash_rsa(const uint8_t *key, uint8_t hash[32])
{
	psa_status_t st;
	size_t out_len = 0U;

	st = psa_crypto_init();
	if (st != PSA_SUCCESS) {
		return -EIO;
	}
	st = psa_hash_compute(PSA_ALG_SHA_256, key, MCUBOOT_RSA_KEY_LEN,
			      hash, 32U, &out_len);
	if (st != PSA_SUCCESS || out_len != 32U) {
		return -EIO;
	}
	return 0;
}
#endif /* CONFIG_BOOT_AGM_SIG_RSA2048_PSS */

/* Verify the container @a src points at against @a prof and @a key. @a what
 * only names the thing in the success line ("image", "fabric image"). */
static int img_verify(const struct img_src *src, uint32_t len,
		      const struct img_profile *prof, const uint8_t *key,
		      const char *what)
{
	struct mcuboot_image_header hdr;
	uint8_t hash[32];
	uint32_t body;
	int ret;

	img_verify_len = len;
	img_verify_ok = false;

	if (!boot_agm_key_present(key, prof->key_len)) {
		printk("loader: this build has no trusted public key (%s)\n",
		       prof->key_name);
		return -ENOTSUP;
	}
	if (len < MCUBOOT_HDR_SIZE) {
		printk("loader: image is shorter than an MCUboot header\n");
		return -EINVAL;
	}
	ret = img_read(src, 0U, (uint8_t *)&hdr, sizeof(hdr));
	if (ret < 0) {
		return ret;
	}
	if (hdr.ih_magic != MCUBOOT_IMAGE_MAGIC ||
	    hdr.ih_hdr_size != MCUBOOT_HDR_SIZE || hdr.ih_img_size == 0U) {
		printk("loader: not a signed MCUboot image (magic 0x%08x, hdr %u, "
		       "img %u); this build requires one\n", hdr.ih_magic,
		       hdr.ih_hdr_size, hdr.ih_img_size);
		return -EINVAL;
	}
	/* A fabric container has to carry the whole fabric and be stamped for
	 * the fabric: an application container, or one built for another board,
	 * is refused here (it would stream garbage into the FCB otherwise). */
	if (prof->exact_img_size != 0U && hdr.ih_img_size != prof->exact_img_size) {
		printk("loader: container carries %u B, this slot needs %u\n",
		       hdr.ih_img_size, prof->exact_img_size);
		return -EINVAL;
	}
	if (prof->require_load_addr != 0U &&
	    hdr.ih_load_addr != prof->require_load_addr) {
		printk("loader: container is stamped for 0x%08x, this slot needs "
		       "0x%08x\n", hdr.ih_load_addr, prof->require_load_addr);
		return -EINVAL;
	}
	body = (uint32_t)hdr.ih_hdr_size + hdr.ih_img_size;
	if (body > len) {
		printk("loader: image header claims %u B, only %u uploaded\n",
		       body, len);
		return -EINVAL;
	}

	/* 1. The digest covers header + image, exactly like MCUboot's. */
	ret = prof->hash(src, body, hash);
	if (ret < 0) {
		return ret;
	}

	/* 2. The TLV area follows the image exactly: imgtool keeps the padding to
	 * the alignment *inside* img_size, so an image whose length is not a
	 * multiple of the alignment still puts the TLV info right after it (seen
	 * e.g. img_size 22172 puts it at 0x20+22172, four bytes before the
	 * 8-byte boundary -- rounding up here skips its magic). */
	ret = img_check_tlvs(src, body, len, hash, hdr.ih_ver, prof, key);
	if (ret < 0) {
		return ret;
	}

	img_verify_ok = true;
	printk("loader: %s verified (MCUboot container, %u B, %s)\n", what, len,
	       prof->name);
	return 0;
}

/* The container's version, packed for comparison: MCUboot's `ih_ver` is an
 * `image_version { major, minor, revision(u16), build(u32) }` whose multi-byte
 * fields sit in the image's byte order (little-endian -- imgtool
 * `--version 1.2.4660` puts `34 12` for revision 0x1234). The build number is
 * deliberately *not* part of the ordering -- a rebuild of the same release
 * must not look newer than the release it rebuilt -- so the key is
 * `major << 24 | minor << 16 | revision`. */
int boot_agm_image_version_read(enum agm_boot_target target, uint32_t *ver)
{
	const struct img_src src = img_src_target(target);
	struct mcuboot_image_header hdr;
	int ret = img_read(&src, 0U, (uint8_t *)&hdr, sizeof(hdr));

	if (ret < 0) {
		return ret;
	}
	if (hdr.ih_magic != MCUBOOT_IMAGE_MAGIC) {
		return -EINVAL;
	}
	*ver = ((uint32_t)hdr.ih_ver[0] << 24) | ((uint32_t)hdr.ih_ver[1] << 16) |
	       ((uint32_t)hdr.ih_ver[3] << 8) | (uint32_t)hdr.ih_ver[2];
	return 0;
}

/* An MCUboot container is linked to run at `slot + header size` (what
 * `imgtool sign --pad-header` produces), so the record's entry point has to
 * point past the header. Deriving it here means hosts keep passing the slot
 * base -- agm_upload.py's default -- instead of having to know the offset.
 *
 * The header also *declares* that address (`ih_load_addr`, what
 * `sign_image.py --slot-base` writes), and the two have to agree: an image
 * built for another slot -- or for another board -- would otherwise be
 * accepted, published and then jumped into at an address it was not linked
 * for. Refusing it here means a wrong-slot upload never reaches the record.
 */
int boot_agm_image_container_entry(enum agm_boot_target target, uint32_t *entry)
{
	const struct img_src src = img_src_target(target);
	struct mcuboot_image_header hdr;
	uint32_t runs_at;
	int ret = img_read(&src, 0U, (uint8_t *)&hdr, sizeof(hdr));

	if (ret < 0) {
		return ret;
	}
	if (hdr.ih_magic != MCUBOOT_IMAGE_MAGIC || hdr.ih_hdr_size == 0U) {
		return -EINVAL;
	}
	runs_at = SLOT_BASE + hdr.ih_hdr_size;
	if (hdr.ih_load_addr != runs_at) {
		printk("loader: image is linked for 0x%08x but this layout runs a "
		       "container at 0x%08x -- refusing it\r\n",
		       hdr.ih_load_addr, runs_at);
		return -EINVAL;
	}
	*entry = runs_at;
	return 0;
}
#else
int boot_agm_image_container_entry(enum agm_boot_target target, uint32_t *entry)
{
	ARG_UNUSED(target);
	ARG_UNUSED(entry);
	return -ENOTSUP; /* raw images run from the slot base itself */
}
#endif

int agm_boot_image_verify(const struct device *dev, enum agm_boot_target target,
			  uint32_t len)
{
	ARG_UNUSED(dev);

	if (target >= AGM_BOOT_TARGET_COUNT) {
		return -EINVAL;
	}
#if defined(CONFIG_BOOT_AGM_SIG_ECDSA_P256) || defined(CONFIG_BOOT_AGM_SIG_RSA2048_PSS)
	const struct img_src src = img_src_target(target);

	return img_verify(&src, len, &app_profile, agm_boot_pubkey, "image");
#else
	/* BOOT_AGM_SIG_NONE: raw images, integrity left to the CRC32 in the
	 * record. Explicitly not a security boundary (see the Kconfig help). */
	return 0;
#endif
}

#if defined(CONFIG_BOOT_AGM_BITSTREAM_SIGNED)
/* CRC-32 (the record's own field) over the slot, in SLOT_CHUNK reads. */
static int img_crc_range(const struct img_src *src, uint32_t len, uint32_t *crc)
{
	uint8_t buf[SLOT_CHUNK];
	uint32_t acc = 0U;

	for (uint32_t done = 0U; done < len; done += SLOT_CHUNK) {
		uint32_t chunk = MIN(SLOT_CHUNK, len - done);
		int ret = img_read(src, done, buf, chunk);

		if (ret < 0) {
			return ret;
		}
		acc = crc32_ieee_update(acc, buf, chunk);
	}
	*crc = acc;
	return 0;
}

/*
 * The fabric key: the application's own `agm_boot_bitstream_pubkey` unless it
 * is all zeroes *and* the application profile is a P-256 one, in which case the
 * application key stands in. Signing the application and the fabric with one
 * keypair is the common case, and this is what makes it need no second -D --
 * while an RSA build (nothing to fall back to: its key is 270 bytes of DER)
 * still requires the fabric key to be given explicitly.
 */
static const uint8_t *bs_trusted_key(void)
{
	if (boot_agm_key_present(agm_boot_bitstream_pubkey, MCUBOOT_BS_KEY_LEN)) {
		return agm_boot_bitstream_pubkey;
	}
#if defined(CONFIG_BOOT_AGM_SIG_ECDSA_P256)
	return agm_boot_pubkey;
#else
	return agm_boot_bitstream_pubkey;
#endif
}

int agm_boot_bitstream_verify(uint32_t slot_addr, uint32_t len, uint32_t crc)
{
	struct img_src src;
	uint32_t want_crc = 0U;
	int ret;

	if (slot_addr != (uint32_t)AGM_BITSTREAM_SLOT1_ADDR &&
	    slot_addr != (uint32_t)AGM_BITSTREAM_SLOT2_ADDR) {
		printk("loader: 0x%08x is not a fabric slot\n", slot_addr);
		return -EINVAL;
	}
	if (len == 0U || len > (uint32_t)AGM_BITSTREAM_SLOT_SIZE) {
		printk("loader: %u B does not fit a fabric slot\n", len);
		return -EINVAL;
	}

	src = img_src_slot(slot_addr);

	/* The record's CRC first: it is the cheap read-only check that keeps a
	 * damaged slot from taking the fabric down, and it is also what says
	 * "the bytes here are not the ones the record committed". The boot path
	 * runs this before it touches the FCB, so a slot that fails here costs
	 * the board nothing. */
	ret = img_crc_range(&src, len, &want_crc);
	if (ret < 0) {
		return ret;
	}
	if (want_crc != crc) {
		printk("loader: the fabric slot does not match its record "
		       "(crc 0x%08x, record says 0x%08x)\n", want_crc, crc);
		return -EBADMSG;
	}

	return img_verify(&src, len, &bs_profile, bs_trusted_key(), "fabric image");
}
#else
int agm_boot_bitstream_verify(uint32_t slot_addr, uint32_t len, uint32_t crc)
{
	ARG_UNUSED(slot_addr);
	ARG_UNUSED(len);
	ARG_UNUSED(crc);
	/* CONFIG_BOOT_AGM_BITSTREAM_SIGNED off: the slots hold raw fabric and
	 * the boot path streams them as they are (the pre-signing behaviour,
	 * which is explicitly not a security boundary). */
	return 0;
}
#endif /* CONFIG_BOOT_AGM_BITSTREAM_SIGNED */
