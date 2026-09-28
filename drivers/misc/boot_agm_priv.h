/*
 * Copyright (c) 2026 AgRV Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief Internals shared between the boot driver's translation units.
 *
 * Every unit that touches flash has to agree on the layout, which is one
 * devicetree node (dts/bindings/misc/agm,agrv2k-boot.yaml), so the geometry and
 * the two flash devices live here rather than in any one .c. The rest is the
 * small set of helpers more than one unit needs; the public surface stays in
 * include/zephyr/drivers/misc/boot_agm.h.
 *
 * The BUILD_ASSERTs that police this layout live in boot_agm.c, next to the code
 * that would break if one failed.
 */

#ifndef ZEPHYR_DRIVERS_MISC_BOOT_AGM_PRIV_H_
#define ZEPHYR_DRIVERS_MISC_BOOT_AGM_PRIV_H_

#define DT_DRV_COMPAT agm_agrv2k_boot

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/drivers/misc/boot_agm.h>
#include <zephyr/sys/util.h>

#if defined(CONFIG_SOC_AGM_AGRV2K)
/*
 * What the FCB bring-up (soc/agm/agrv2k/fcb.c, PRE_KERNEL_1) did in this boot.
 * Declared as a small API rather than read as bare `extern` variables: two of
 * the three are __noinit and therefore meaningless until that code has run, and
 * agm_fcb_program_ran() is how the boot driver tells "nothing was reported"
 * apart from "this ran too early".
 */
extern bool agm_fcb_program_ran(void);
extern uint32_t agm_fcb_bitstream_slot_get(void);
extern int32_t agm_fcb_bitstream_refused_get(void);
#endif

/* The signed profiles and the command authorization all verify with MCUboot's
 * vendored tinycrypt: ECDSA signs and hashes with it, the RSA profile hashes
 * with PSA but its *fabric* is still ECDSA, and the command-authorization check needs the
 * curve without any image profile at all. boot_agm_verify.c is the only user of
 * the hash, but both units compile against this switch. */
#if defined(CONFIG_BOOT_AGM_SIG_ECDSA_P256) || \
	defined(CONFIG_BOOT_AGM_BITSTREAM_SIGNED) || \
	defined(CONFIG_BOOT_AGM_LOCK_PRODUCTION)
#define AGM_HAVE_TINYCRYPT_ECDSA 1
#endif

/* ---- layout: entirely from the devicetree node ---------------------- */


#define STORE_FLASH DEVICE_DT_GET(DT_INST_PHANDLE(0, store_flash))
#define INT_FLASH DEVICE_DT_GET(DT_INST_PHANDLE(0, on_die_flash))

/* The on-die flash is mapped at an absolute address while the driver works in
 * device offsets, so the base is subtracted (0x80000000 on this SoC). It
 * normally comes from the flash node's `reg`; a node without one (a simulated
 * flash, for instance) states it with `on-die-flash-base` instead. */
#if DT_INST_NODE_HAS_PROP(0, on_die_flash_base)
#define INT_FLASH_BASE ((uint32_t)DT_INST_PROP(0, on_die_flash_base))
#else
#define INT_FLASH_BASE ((uint32_t)DT_REG_ADDR(DT_INST_PHANDLE(0, on_die_flash)))
#endif

#define BOOT_CFG_SECTOR ((uint32_t)DT_INST_PROP(0, record_size))
/* The record spans BOOT_CFG_RECORDS sectors of that size -- see the "boot
 * record" section for why it is more than one. */
#define BOOT_CFG_RECORDS 2U

/* The fabric (bitstream) side. `bitstream-address` is the *factory* slot --
 * what the flash option byte points at, never written by this driver; updates
 * go into the inactive one of the two slots the SoC header defines, and a
 * record commits them (soc/agm/agrv2k/agm_bitstream.h explains why: rewriting
 * the region the running fabric came from freezes the core). */
#define BS_ON_DIE      ((uint32_t)DT_INST_PROP(0, bitstream_address))
#define BS_ON_DIE_OFF  (BS_ON_DIE - (uint32_t)INT_FLASH_BASE)
#define BS_ON_DIE_MAX  ((uint32_t)DT_INST_PROP(0, bitstream_size))

#define BS_SLOT1_ADDR  ((uint32_t)AGM_BITSTREAM_SLOT1_ADDR)
#define BS_SLOT2_ADDR  ((uint32_t)AGM_BITSTREAM_SLOT2_ADDR)
#define BS_RECORD_ADDR ((uint32_t)AGM_BITSTREAM_RECORD_ADDR)
#define BS_SLOT1_OFF   (BS_SLOT1_ADDR - (uint32_t)INT_FLASH_BASE)
#define BS_RECORD_OFF  (BS_RECORD_ADDR - (uint32_t)INT_FLASH_BASE)
#define BS_SLOT_OFF(a) ((a) - (uint32_t)INT_FLASH_BASE)

/* Which family this node describes -- see the binding. The on-die family is
 * the one where the images live in the same flash as the execution slot; it
 * takes sizes and derives the offsets, so one number (app-size) moves the
 * whole layout. The two-flash family spells every offset out, because the NOR
 * and the on-die flash are independent address spaces. */
#define BOOT_ON_DIE_AB \
	DT_SAME_NODE(DT_INST_PHANDLE(0, store_flash), DT_INST_PHANDLE(0, on_die_flash))

#if BOOT_ON_DIE_AB

/* The chain is packed from the loader up and the *metadata* sits at its top,
 * just below the fabric area:
 *
 *     loader | side A | side B | slot | boot record (2 sectors) | bind salt
 *
 * One 12 KiB metadata block, and every position derived from the three sizes
 * below (loader-size, app-size, record-size) -- the salt used to be a Kconfig
 * address pinned to the gap between the two fabric slots, which made the gap
 * look like a design decision instead of the leftover it was. The invariants
 * section asserts that the chain still ends below fabric slot 1. */
#define BOOT_LOADER_SIZE ((uint32_t)DT_INST_PROP(0, loader_size))
#define BOOT_APP_SIZE    ((uint32_t)DT_INST_PROP(0, app_size))

#define BOOT_STORE_A   BOOT_LOADER_SIZE
#define BOOT_STORE_B   (BOOT_STORE_A + BOOT_APP_SIZE)
#define SLOT_OFF       (BOOT_STORE_B + BOOT_APP_SIZE)
#define SLOT_BASE      ((uint32_t)INT_FLASH_BASE + SLOT_OFF)
#define SLOT_MAX       BOOT_APP_SIZE
#define SLOT_STRIDE    BOOT_APP_SIZE
#define BOOT_CFG_OFF   (SLOT_OFF + BOOT_APP_SIZE)
#define BOOT_SALT_OFF  (BOOT_CFG_OFF + BOOT_CFG_RECORDS * BOOT_CFG_SECTOR)

#else /* two flashes: the NOR holds the record and the images */

#define BOOT_CFG_OFF   ((uint32_t)DT_INST_PROP(0, record_offset))
#define BOOT_STORE_A   ((uint32_t)DT_INST_PROP(0, store_a_offset))
#define BOOT_STORE_B   ((uint32_t)DT_INST_PROP(0, store_b_offset))
#define SLOT_STRIDE    ((uint32_t)DT_INST_PROP(0, store_max_size))
#define SLOT_BASE      ((uint32_t)DT_INST_PROP(0, slot_address))
#define SLOT_OFF       (SLOT_BASE - (uint32_t)INT_FLASH_BASE)
#define SLOT_MAX       ((uint32_t)DT_INST_PROP(0, slot_size))

/* The salt is still a sector of the *on-die* flash (it has to outlive a
 * reflash of the NOR-resident images, and the on-die flash is the only place
 * the loader can read before it has anywhere to copy to), so this family
 * spells its address out instead of deriving it from a chain it does not
 * have. */
#if defined(CONFIG_BOOT_AGM_BIND)
#define BOOT_SALT_OFF \
	((uint32_t)CONFIG_BOOT_AGM_BIND_SALT_OFFSET - (uint32_t)INT_FLASH_BASE)
#endif

#endif /* BOOT_ON_DIE_AB */

/* Chunk for the external<->on-die copies: the SPI NOR driver's RX bounce
 * buffer is 256 B deep and 256 is a multiple of the on-die flash's 4-byte
 * programming unit. */
#define SLOT_CHUNK      256U

/* ---- MCUboot container constants ------------------------------------- */

/*
 * The container the verifier walks is `imgtool sign`'s output and these are
 * MCUboot's own numbers; they are copied rather than included so the driver
 * does not depend on MCUboot's headers (the layout is frozen, MCUboot changes
 * it only with a new magic). They live here because two units need them: the
 * verifier reads the container and the command-authorization check builds the same
 * ECDSA signature encoding.
 */
#define MCUBOOT_IMAGE_MAGIC    0x96f3b83dU
#define MCUBOOT_TLV_INFO_MAGIC 0x6907U
#define MCUBOOT_TLV_KEYHASH    0x01U
#define MCUBOOT_TLV_SHA256     0x10U
#define MCUBOOT_TLV_ECDSA_SIG  0x22U
#define MCUBOOT_TLV_RSA2048_PSS 0x20U
/* The per-chip binding tag (tools/agm_bind.py `embed`). MCUboot reserves
 * 0x00a0..0x00ff for vendors -- "vendor reserved TLVs at xxA0-xxFF",
 * boot/bootutil/include/bootutil/image.h -- so this cannot collide with a TLV
 * imgtool writes; 0x30, the obvious first pick, is IMAGE_TLV_ENC_RSA2048. */
#define MCUBOOT_TLV_BIND       0x00A0U
#define MCUBOOT_HDR_SIZE       32U

#define MCUBOOT_DER_MAX        80U  /* the ECDSA TLV carries DER, ~70-72 B */
#define MCUBOOT_ECDSA_SIG_LEN  64U  /* raw r||s, what tinycrypt takes */
#define MCUBOOT_RSA_SIG_LEN    256U /* the raw signature, as it is verified */
#define MCUBOOT_ECDSA_KEY_LEN  64U  /* raw X||Y */
#define MCUBOOT_RSA_KEY_LEN    270U /* PKCS#1 RSAPublicKey DER */
/* A signature TLV never exceeds this, so the walk reads it in one go. */
#define MCUBOOT_SIG_TLV_MAX    MCUBOOT_RSA_SIG_LEN
#define MCUBOOT_KEY_LEN        AGM_BOOT_PUBKEY_LEN
#define MCUBOOT_BS_KEY_LEN     AGM_BOOT_BITSTREAM_PUBKEY_LEN

/** MCUboot's DER ECDSA signature (r||s) as tinycrypt's raw 64-byte pair. */
int boot_agm_sig_der_to_raw(const uint8_t *der, uint32_t len, uint8_t raw[64]);

/* ---- helpers shared across units ------------------------------------- */

/*
 * `boot_agm_target_read()` is the read side of an upload target (the verifier
 * walks a container through it, and the upload machinery shares it);
 * `boot_agm_key_present()` is the driver's "is a trusted key configured at
 * all" test, since the weak default key is all zeroes; the two container
 * helpers are shared by the verifier and the publish path.
 */

/** Read @a len bytes at @a off of @a target. */
int boot_agm_target_read(enum agm_boot_target target, uint32_t off, uint8_t *buf,
			 uint32_t len);

/** Is any byte of @a key non-zero? (an all-zero key means "none configured") */
bool boot_agm_key_present(const uint8_t *key, uint32_t len);

/** Packed `major << 24 | minor << 16 | rev` of the container in @a target. */
int boot_agm_image_version_read(enum agm_boot_target target, uint32_t *ver);

/** The `ih_load_addr` the container in @a target was linked for. */
int boot_agm_image_container_entry(enum agm_boot_target target, uint32_t *entry);

/*
 * The record's sequence number and the authorization gate: the record
 * itself stays
 * private to boot_agm.c, which is why the authorization (boot_agm_auth.c) asks
 * for the one field it uses; the gate is what upload/publish/erase call before
 * they touch anything.
 */

/** The boot record's sequence number (0 when there is no usable record). */
uint32_t boot_agm_record_seq(void);

/** Take the grant for @a cmd (the locked profile's check). */
bool boot_agm_auth_take_locked(uint8_t cmd, const char *verb, const char *what);

/** Take the grant for @a cmd; see @ref boot_agm_auth_take_locked. */
static inline bool boot_agm_auth_take(uint8_t cmd, const char *verb, const char *what)
{
	/* A build without the lock has no gate at all, and that is the common
	 * build: spelling the test here lets the call sites in boot_agm.c compile
	 * the call away, instead of paying for a cross-unit call whose body
	 * starts with exactly this check. */
	if (!IS_ENABLED(CONFIG_BOOT_AGM_LOCK_PRODUCTION)) {
		ARG_UNUSED(cmd);
		ARG_UNUSED(verb);
		ARG_UNUSED(what);
		return true;
	}
	return boot_agm_auth_take_locked(cmd, verb, what);
}

/** Is there a trusted P-256 key to verify commands with? */
bool boot_agm_auth_key_ready(void);

#if defined(CONFIG_BOOT_AGM_BIND)
/*
 * The HMAC half of the per-chip tag: tag = HMAC(key, version ‖ digest), where
 * the verifier passes in the SHA-256 it has *already* computed over
 * header ‖ image for the signature check and the header's own `ih_ver`. Both
 * sides of the tag therefore walk the same bytes, and the container is hashed
 * once (drivers/misc/boot_agm_bind.c has the KDF).
 */
int boot_agm_bind_tag_digest(const uint8_t version[8],
			     const uint8_t digest[32], uint8_t tag[32]);
#endif

#endif /* ZEPHYR_DRIVERS_MISC_BOOT_AGM_PRIV_H_ */
