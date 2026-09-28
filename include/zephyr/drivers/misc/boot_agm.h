/*
 * Copyright (c) 2026 AgRV Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief AgRV2K bootloader / DFU driver.
 *
 * Storage and DFU for a bootloader that lives in the low part of the on-die
 * flash and keeps its images in the external SPI NOR (see the binding
 * dts/bindings/misc/agm,agrv2k-boot.yaml for the layout):
 *
 *   boot record (external flash sector 0)
 *     magic/version/mode, per-slot {offset, length, CRC32, state, attempts},
 *     which slot is active, where each image comes from
 *   store A / store B (external flash)
 *     the two image stores; whatever is published here is copied into the
 *     application slot before it runs
 *   application slot (on-die flash)
 *     where images execute. A store is copied here at boot; an image uploaded
 *     straight into it ("on-die DFU") is verified and jumped to instead
 *   bitstream staging (external flash) + reservation (on-die flash)
 *     the fabric update path
 *
 * Everything a host protocol needs is one API: `upload_begin/write/crc/
 * finish` publish an image into any of the four targets, `info_get` reports
 * the record, `decide`/`boot` run the A/B policy (attempt-counted, with
 * rollback), `erase`/`confirm` are the image-management operations, and
 * `bitstream_upload` puts a bitstream into the slot that is not running and
 * commits it with a boot record (the slot in use is never rewritten; see
 * soc/agm/agrv2k/agm_bitstream.h).
 *
 * The protocol servers themselves live next to the driver
 * (drivers/misc/boot_agm_an3155.c for the vendor's agrv32flash, CONFIG_
 * BOOT_AGM_AN3155; drivers/misc/boot_agm_smp.c for mcumgr, CONFIG_BOOT_
 * AGM_SMP) and call this API -- so the sample is only the console.
 */

#ifndef ZEPHYR_INCLUDE_DRIVERS_MISC_BOOT_AGM_H_
#define ZEPHYR_INCLUDE_DRIVERS_MISC_BOOT_AGM_H_

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/sys/util.h>
/* The RTC / backup-domain map, shared with the drivers that touch the same
 * registers (see the trial-boot handshake below and the header itself). */
#include <zephyr/drivers/counter/agm_rtc_regs.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Where an upload goes. */
enum agm_boot_target {
	/** Image store A in the external flash ("slot 0" of mcumgr's image group). */
	AGM_BOOT_TARGET_STORE_A = 0,
	/** Image store B in the external flash ("slot 1"). */
	AGM_BOOT_TARGET_STORE_B = 1,
	/** The application slot in the on-die flash itself ("slot 2", on-die DFU). */
	AGM_BOOT_TARGET_SLOT = 2,
	/** Bitstream staging area; `upload_finish` applies it to the on-die copy. */
	AGM_BOOT_TARGET_BITSTREAM = 3,
	AGM_BOOT_TARGET_COUNT = 4,
};

/** Persistent boot mode (the record's policy).
 *
 *  Value 2 used to mean `external` -- an image copied into SRAM and run from
 *  there. That path is gone (the A/B slot is the only place this driver runs
 *  an image from), and a record or one-shot left over from it reads as AUTO.
 */
enum agm_boot_mode {
	/** A/B policy: pick the usable slot, with the console abort window. */
	AGM_BOOT_MODE_AUTO = 0,
	/** A/B policy, but never wait in the abort window (upload-friendly). */
	AGM_BOOT_MODE_INTERNAL = 1,
};

/** State of a slot in the record. */
enum agm_boot_slot_state {
	AGM_BOOT_SLOT_EMPTY = 0,
	AGM_BOOT_SLOT_TRIAL = 1,     /**< installed, not yet confirmed */
	AGM_BOOT_SLOT_CONFIRMED = 2, /**< known good */
	AGM_BOOT_SLOT_BAD = 3,       /**< used up its trial attempts */
};

/** Where a slot's image lives. */
enum agm_boot_source {
	AGM_BOOT_SRC_STORE = 0,  /**< external store, copied into the slot at boot */
	AGM_BOOT_SRC_ON_DIE = 1, /**< already in the application slot */
};

/** One slot as the record describes it. */
struct agm_boot_slot_info {
	enum agm_boot_slot_state state;
	enum agm_boot_source src;
	uint32_t len;
	uint32_t crc;
	uint32_t load;
	uint32_t entry;
	uint32_t attempts;
	/** Where the image lives: the external store offset when @ref src is
	 *  AGM_BOOT_SLOT_SRC_STORE, the on-die slot offset when it is
	 *  AGM_BOOT_SLOT_SRC_ON_DIE (this used to report the store-A offset for
	 *  an on-die image as well). */
	uint32_t offset;
};

/** Everything the console's `info` and the hosts' `image state` need. */
struct agm_boot_info {
	bool record_valid;
	/** The record sector reads back as erased, i.e. no record has ever been
	 *  written there. That is the normal state of a freshly flashed board --
	 *  in particular with the on-die A/B layout, where the record sector is
	 *  blank until the first upload -- and is not the same as a corrupt
	 *  record (magic/version mismatch, or an unreadable store). */
	bool record_blank;
	/** Raw magic/version the record read gave (invalid records only). */
	uint32_t record_magic;
	uint32_t record_version;
	/** The record sector could not be read at init, so everything above is
	 *  the driver's zeroed state rather than what the flash holds. Distinct
	 *  from @ref record_blank (a readable, erased sector) and from a corrupt
	 *  record (readable, wrong magic): with this set the record was never
	 *  looked at. */
	bool record_read_failed;
	enum agm_boot_mode mode;
	uint32_t active;
	struct agm_boot_slot_info slot[2];
	/** RTC-backed one-shot override for the next boot (AGM_BOOT_MODE_AUTO
	 *  when none is armed). This is the "is a one-shot armed?" query: the
	 *  value comes from the backup domain rather than from the record, and
	 *  AUTO covers both "nothing armed" and "armed as auto" -- the policy
	 *  treats those the same way. The next boot consumes (and clears) it. */
	enum agm_boot_mode once;
	/** True while the on-die application slot holds something. */
	bool slot_programmed;
	/** The anti-rollback floor the record carries: the highest image version
	 *  accepted so far, packed `major << 24 | minor << 16 | revision`
	 *  (CONFIG_BOOT_AGM_ANTI_ROLLBACK; zero when no signed image has been
	 *  accepted yet, and always zero in builds without it). */
	uint32_t sec_ver;
};

/** Largest payload `upload_write()` accepts in one call (bytes). */
#define AGM_BOOT_UPLOAD_CHUNK 1024U

/** Trial boots an image gets before the policy calls it bad and rolls back. */
#define AGM_BOOT_MAX_ATTEMPTS 3U

/**
 * @brief Start a fresh upload into @a target.
 *
 * Forgets the watermarks of whatever was written before, so the first write
 * erases its sectors from scratch. `upload_finish()` publishes what was
 * written; uploads that are never finished publish nothing.
 *
 * With CONFIG_BOOT_AGM_LOCK_PRODUCTION this is gated too: opening the session
 * is the same state change `agm_boot_erase()` asks authorization for, because
 * that first write erases the target -- for a store, the image the record
 * still points at. The gate therefore wants an ERASE grant
 * (@ref AGM_BOOT_AUTH_CMD_ERASE) before it opens; without one nothing is
 * erased and the writes that follow fail.
 *
 * @return 0, AGM_BOOT_E_UNAUTHORIZED when the locked profile has no grant, or
 *         another negative errno.
 */
int agm_boot_upload_begin(const struct device *dev, enum agm_boot_target target);

/**
 * @brief Write @a len bytes of an image at @a off of @a target.
 *
 * Chunks may be any size and any alignment (the driver keeps the sub-word
 * tail of a chunk for the next one); offsets have to move forward.
 */
int agm_boot_upload_write(const struct device *dev, enum agm_boot_target target,
			  uint32_t off, const void *data, uint32_t len);

/**
 * @brief CRC32 over what was uploaded to @a target (read back from the flash).
 *
 * The boot record stores a CRC32 (Zephyr's `crc32_ieee`), so a host that
 * wants its own integrity check can compare against this.
 */
int agm_boot_upload_crc(const struct device *dev, enum agm_boot_target target,
			uint32_t off, uint32_t len, uint32_t *crc);

/**
 * @brief Publish the uploaded image as TRIAL + active (the A/B policy takes
 * over from there).
 *
 * For AGM_BOOT_TARGET_BITSTREAM this also writes the staging header and
 * applies the image to the on-die reservation (effective after a reset).
 */
int agm_boot_upload_finish(const struct device *dev, enum agm_boot_target target,
			   uint32_t len);

/**
 * @brief Same as @ref agm_boot_upload_finish, but recording where the image
 *        must run (the legacy `mode external` RAM payload, whose load address
 *        is not the application slot).
 */
int agm_boot_upload_finish_at(const struct device *dev, enum agm_boot_target target,
			      uint32_t len, uint32_t load, uint32_t entry);

/**
 * @brief Forget an upload that will not be finished.
 *
 * Only clears the session state (watermarks); whatever was written stays in
 * the flash until the next upload into the same target erases it.
 */
void agm_boot_upload_abort(const struct device *dev, enum agm_boot_target target);

/** Largest image the target accepts. */
uint32_t agm_boot_upload_max(const struct device *dev, enum agm_boot_target target);

/** "a" / "b" / "slot" / "bitstream" (what the console commands print). */
const char *agm_boot_target_name(enum agm_boot_target target);

/** Store offset of an external target (0 for the on-die / bitstream targets). */
uint32_t agm_boot_target_offset(const struct device *dev,
				enum agm_boot_target target);

/** @brief Read the whole record state (for `info`, `image state`). */
int agm_boot_info_get(const struct device *dev, struct agm_boot_info *info);

/** Promote the active slot to CONFIRMED (the console's `confirm`). */
int agm_boot_confirm(const struct device *dev);

/** Make the other slot the active one (the console's `rollback`). */
int agm_boot_rollback(const struct device *dev);

/** Drop a target's image: the record entry is emptied, and the on-die target
 *  is also erased back to blank (mcumgr's `image erase`).
 *
 *  A board whose record is still blank (nothing was ever published) answers 0
 *  and writes nothing: there is no image to drop, so the call is a no-op
 *  rather than an error -- and writing a record just to be allowed to drop
 *  what was never installed would change what the next boot reads. A record
 *  that is present but unusable (-EINVAL) or unreadable (-EIO) does fail, and
 *  leaves the record untouched.
 */
int agm_boot_erase(const struct device *dev, enum agm_boot_target target);

/** Set the persistent boot mode (written to the record). */
int agm_boot_mode_set(const struct device *dev, enum agm_boot_mode mode);

/** Arm the RTC-backed one-shot override for the next boot. */
int agm_boot_once_arm(const struct device *dev, enum agm_boot_mode mode);

/** Copy the active store's image into the on-die application slot (the
 *  console's `install-slot`). */
int agm_boot_install_slot(const struct device *dev);

/**
 * @typedef agm_boot_abort_cb_t
 * @brief Asked once, before an attempt is spent, whether to stay in the
 *        caller's console instead of booting (the loader's "send any
 *        character to stay in the console" window).
 *
 * @return true to abort the boot.
 */
typedef bool (*agm_boot_abort_cb_t)(void);

/**
 * @brief Run the boot policy: the RTC one-shot override, then the record's
 *        mode, then the A/B policy (attempt counting, rollback, copy into the
 *        application slot) or the legacy RAM-image path for `mode external`.
 *
 * @param abort_cb  Optional; ignored for `mode internal`, which never waits.
 *
 * @return -EAGAIN when @a abort_cb asked to stay, -ENOENT when nothing is
 *         bootable, or a negative errno. Never returns when it boots.
 */
int agm_boot_policy_run(const struct device *dev, agm_boot_abort_cb_t abort_cb);

/**
 * @brief Verify (and copy, if the image comes from a store) then jump to the
 *        slot's entry point.
 *
 * @warning Never returns on success.
 */
int agm_boot_boot(const struct device *dev, uint32_t slot);

/** Apply the staged bitstream to the on-die reservation (reboot to load it). */

/**
 * @brief The image is older than the one already installed.
 *
 * With CONFIG_BOOT_AGM_ANTI_ROLLBACK an uploaded or booted container whose
 * version is below the floor the boot record carries is refused with this
 * error: the console phase reports it as NAK code 5, the SMP server as
 * MGMT_ERR_EBADSTATE. Its own code (rather than -EACCES) because "too old" is
 * a different operator action from "not signed by us".
 */
#define AGM_BOOT_E_OLD_VERSION (-1000)

/** No (valid) authorization for a state-changing command. */
#define AGM_BOOT_E_UNAUTHORIZED (-1001)

/**
 * @brief The image is genuine, but not bound to this chip.
 *
 * With CONFIG_BOOT_AGM_BIND the container has to carry this chip's tag: the
 * HMAC over its version and digest, keyed by `KDF(UID, salt)`. A copy of
 * another board's flash recomputes a different tag here, and a board whose
 * salt was never provisioned has no key to recompute with -- both are this
 * code (the console reports it as NAK code 7). It is its own code for the
 * same reason @ref AGM_BOOT_E_OLD_VERSION is: "this image is for another
 * chip" is a different operator action from "this image is not ours".
 */
#define AGM_BOOT_E_UNBOUND (-1002)

/** Length of the session nonce `agm_boot_nonce_get()` hands out.
 *
 * Sixteen bytes because the nonce only has to be *unique per session*: it
 * is a replay counter, the signature over it is what binds the command, and
 * the source is the record's sequence number plus the cycle counter plus a
 * per-session count rather than an entropy driver (see
 * @ref agm_boot_nonce_get). A longer value would not make a forged command
 * any harder -- the verifier would still refuse it -- it would only cost
 * bytes on the wire and in the hash. */
#define AGM_BOOT_NONCE_LEN 16U

/** Length of the chip's unique ID (`agm_boot_unique_id()`): 128 bits. */
#define AGM_BOOT_UID_LEN 16U

/**
 * @brief Read the chip's 128-bit unique ID (the on-die flash's 0x4B ID).
 *
 * This is the "serial number" half of per-chip binding: it identifies *this*
 * die. It is deliberately not treated as a secret anywhere in this driver --
 * the controller is unlocked with two public constants to read it (see the
 * ), so anything that must stay unknown to an attacker
 * who can run code on the chip has to come from somewhere else (the
 * provisioned salt, protected by RDP; tools/agm_bind.py is the host half).
 *
 * @param uid  receives AGM_BOOT_UID_LEN bytes, in read order
 *
 * @return 0 on success, -ETIMEDOUT if the controller never answered.
 */
int agm_boot_unique_id(uint8_t uid[AGM_BOOT_UID_LEN]);

/** Length of the per-chip binding key (`agm_boot_bind_key()`): SHA-256 sized. */
#define AGM_BOOT_BIND_KEY_LEN 32U

/**
 * @brief Derive this chip's binding key (CONFIG_BOOT_AGM_BIND).
 *
 * `HKDF-SHA256(ikm = UID, salt = the provisioned salt, info = "agm-bind-v1")`.
 * The UID alone is a serial number (see @ref agm_boot_unique_id); the salt is
 * the secret half and lives in the layout's bind-salt sector, which only RDP
 * protects.
 *
 * @param key      receives AGM_BOOT_BIND_KEY_LEN bytes
 * @param present  optional: set to false unless a valid salt sector was found
 *
 * @return 0 on success, -ENOENT when no salt is provisioned (the caller
 *         decides whether that is fatal), other errno on failure.
 */
int agm_boot_bind_key(uint8_t key[AGM_BOOT_BIND_KEY_LEN], bool *present);

/**
 * @brief Absolute address of the bind-salt sector in this build's layout.
 *
 * The salt is the last sector of the A/B chain, so
 * its address is *derived* from loader-size / app-size / record-size rather
 * than pinned -- which is why the host half asks the device for it instead of
 * carrying a copy that has to be kept in step (`tools/agm_bind.py` reads the
 * `bind-salt:` line the loader's `info` prints). Returns 0 when the build has
 * no per-chip binding (`CONFIG_BOOT_AGM_BIND=n`).
 *
 * @return the sector's absolute address, or 0 without CONFIG_BOOT_AGM_BIND.
 */
uint32_t agm_boot_bind_salt_addr(void);

/** Length of the key fingerprint `agm_boot_bind_fingerprint()` returns. */
#define AGM_BOOT_BIND_FP_LEN 4U

/**
 * @brief A short fingerprint of this chip's binding key (CONFIG_BOOT_AGM_BIND).
 *
 * `SHA-256(key)`'s first @ref AGM_BOOT_BIND_FP_LEN bytes. Provisioning prints
 * the same value from the salt it just wrote (tools/agm_bind.py `provision`),
 * so comparing the two is how a production line checks that the board really
 * read the salt it was given -- and that it is talking to the chip whose UID
 * went into the host-side KDF. The key itself is never printed, and a hash of a
 * 128-bit secret gives nothing away.
 *
 * @return 0 on success, -ENOENT when no salt is provisioned, other errno.
 */
int agm_boot_bind_fingerprint(uint8_t fp[AGM_BOOT_BIND_FP_LEN]);

/**
 * @brief The tag one bound image has to carry, for this chip (see the host's
 *        tools/agm_bind.py).
 *
 * `HMAC-SHA256(key, ih_ver ‖ SHA-256(header ‖ image))`, with the version taken
 * from the container's own header and the key from @ref agm_boot_bind_key. The
 * loader compares it against the container's BIND TLV on every publish and
 * every boot; this entry point is the host-parity one (the verifier feeds the
 * digest it already has to `boot_agm_bind_tag_digest()` instead of hashing the
 * container twice).
 *
 * @return 0 on success, -EINVAL for a malformed container, -ENOENT when no
 *         salt is provisioned.
 */
int agm_boot_bind_tag(const uint8_t *container, uint32_t len,
		      uint8_t tag[AGM_BOOT_BIND_KEY_LEN]);

/**
 * @brief Hand out a fresh, one-shot session nonce.
 *
 * The nonce only has to be unique per session -- the signature binds the
 * *command* -- so its source is the record's sequence, the cycle counter and a
 * per-session counter rather than an entropy driver. Every successful
 * @ref agm_boot_authorize() consumes the nonce it was signed over, so a
 * replayed AUTHORIZE is refused.
 *
 * A new nonce also drops a grant that was armed but not used yet: it starts a
 * new session, and the old authorization belongs to the previous one. (A
 * failed @ref agm_boot_authorize() does *not* spend the nonce -- otherwise one
 * bogus signature could burn a legitimate host's challenge.)
 *
 * @return 0 and @a nonce filled, or a negative errno.
 */
int agm_boot_nonce_get(const struct device *dev, uint8_t nonce[AGM_BOOT_NONCE_LEN]);

/**
 * @brief Authorize one state-changing command.
 *
 * The host signs SHA-256(@a nonce || @a cmd || @a args_len (little endian) ||
 * @a args) with the same key that signs images: the application's
 * @ref agm_boot_pubkey, or the fabric key in an RSA build (whose application
 * key is DER rather than a point on the curve). Either way it is *one* key for
 * the whole command set -- the grant names neither a target nor a length, so
 * the authorization is not per-target, and a build that provisions a separate
 * fabric key still authorizes commands with the application key
 * . Verification is the ECDSA path the
 * image verifier already uses, so no second crypto backend. On success the
 * nonce is spent and the grant is armed for exactly one call of @a cmd, which
 * is what lets @ref agm_boot_upload_begin, the publish step of an upload and
 * @ref agm_boot_erase through when CONFIG_BOOT_AGM_LOCK_PRODUCTION is on.
 *
 * Read-only commands (`info`, `status`, `image state` reads, `reboot`) and the
 * A/B policy commands (`confirm`, `rollback`) never require this: they are
 * policy, not an upload path.
 *
 * @param nonce     the value @ref agm_boot_nonce_get returned for this session
 * @param cmd       the command id being authorized (see AGM_BOOT_AUTH_CMD_*)
 * @param args      serialized arguments the signature covers (may be NULL)
 * @param args_len  length of @a args
 * @param sig       ECDSA P-256 signature, DER or raw r||s
 * @param sig_len   length of @a sig
 *
 * @return 0 when the command may run, -EACCES for a bad signature or a spent
 *         nonce, -ENOTSUP in a build without a trusted key, or another
 *         negative errno.
 */
int agm_boot_authorize(const struct device *dev, const uint8_t *nonce, uint8_t cmd,
		       const uint8_t *args, uint32_t args_len, const uint8_t *sig,
		       uint32_t sig_len);

/** Command ids the authorization covers (the `cmd` byte above). */
#define AGM_BOOT_AUTH_CMD_ERASE   1U
#define AGM_BOOT_AUTH_CMD_PUBLISH 2U

/** Read back what a target holds (the AN3155/mcumgr read-back paths). */
int agm_boot_read(const struct device *dev, enum agm_boot_target target, uint32_t off,
		  void *buf, uint32_t len);

/**
 * @brief Would this loader accept what was uploaded/installed into @a target?
 *
 * With `CONFIG_BOOT_AGM_SIG_NONE` (the default) any bytes are accepted: the
 * record's CRC32 catches accidental corruption, which is explicitly not a
 * security boundary.
 *
 * With `CONFIG_BOOT_AGM_SIG_ECDSA_P256` or `CONFIG_BOOT_AGM_SIG_RSA2048_PSS`
 * the @a len bytes have to be an MCUboot container (`imgtool sign` output)
 * whose SHA-256 TLV matches the bytes, whose KEYHASH TLV names the key in
 * `agm_boot_pubkey[]`, and whose ECDSA P-256 / RSA-2048-PSS signature
 * verifies. Raw images are refused, so a signed build cannot be downgraded by
 * writing an unsigned image. With CONFIG_BOOT_AGM_BIND the container also has
 * to carry a BIND TLV matching this chip's tag (@ref agm_boot_bind_tag): a
 * genuine image bound to another board, or one with no tag at all, is refused
 * with @ref AGM_BOOT_E_UNBOUND.
 *
 * @return 0 when the image is acceptable, -EACCES for a bad digest, key or
 *         signature, @ref AGM_BOOT_E_UNBOUND for an image bound to another
 *         chip, -EINVAL for a malformed container, -ENOTSUP when the build has
 *         no trusted key, or another negative errno on a read error.
 */
int agm_boot_image_verify(const struct device *dev, enum agm_boot_target target,
			  uint32_t len);

/**
 * @brief Length of @ref agm_boot_pubkey, in bytes, for the selected profile.
 *
 * Both signed profiles carry the key in the encoding MCUboot's own key
 * handling uses, which is also the encoding this driver needs to recompute the
 * container's KEYHASH TLV from:
 *
 *  - `BOOT_AGM_SIG_ECDSA_P256`: 64 bytes, the raw point X||Y (the KEYHASH is
 *    taken over the SubjectPublicKeyInfo the loader rebuilds around it);
 *  - `BOOT_AGM_SIG_RSA2048_PSS`: 270 bytes, the PKCS#1 `RSAPublicKey` DER
 *    (`imgtool` hashes those bytes directly, and `psa_import_key()` takes the
 *    same blob).
 */
#if defined(CONFIG_BOOT_AGM_SIG_RSA2048_PSS) || defined(__DOXYGEN__)
#define AGM_BOOT_PUBKEY_LEN 270
#else
#define AGM_BOOT_PUBKEY_LEN 64
#endif

/** The trusted public key a signed build verifies with, in the encoding
 *  @ref AGM_BOOT_PUBKEY_LEN describes for the selected profile. The
 *  application defines it; the driver's weak default is all zeroes, which
 *  makes @ref agm_boot_image_verify refuse everything. */
extern const uint8_t agm_boot_pubkey[AGM_BOOT_PUBKEY_LEN];

/**
 * @brief Length of @ref agm_boot_bitstream_pubkey, in bytes.
 *
 * The fabric profile is always ECDSA P-256, whatever the application images
 * use (see CONFIG_BOOT_AGM_BITSTREAM_SIGNED): the fabric check runs in the
 * boot path, before the kernel's heap exists, and only the tinycrypt P-256
 * verifier is allocation-free. So the blob is the same 64-byte raw X||Y an
 * ECDSA application build keeps.
 */
#define AGM_BOOT_BITSTREAM_PUBKEY_LEN 64

/**
 * @brief The trusted key fabric (bitstream) images are verified with.
 *
 * Only meaningful with CONFIG_BOOT_AGM_BITSTREAM_SIGNED. The application
 * defines it (`-DSPI_BOOT_BITSTREAM_PUBKEY=...`); the driver's weak default is
 * all zeroes, and an all-zero fabric key falls back to @ref agm_boot_pubkey
 * when that key is a P-256 one -- signing the application and the fabric with
 * the same key then needs no second blob. In an RSA build there is nothing to
 * fall back to (the RSA key is 270 bytes of DER), so a fabric key has to be
 * provided or nothing signed will be accepted.
 */
extern const uint8_t agm_boot_bitstream_pubkey[AGM_BOOT_BITSTREAM_PUBKEY_LEN];

/**
 * @brief Flash address a target is exposed at (the window the AN3155 hosts
 *        write to).
 *
 * Store A is the loader's own region, the slot target is the application slot
 * itself, store B takes the gap above it and the bitstream target is the
 * on-die reservation.
 */
uint32_t agm_boot_target_window_base(const struct device *dev,
				     enum agm_boot_target target);

/** Size of the window @ref agm_boot_target_window_base returns. */
uint32_t agm_boot_target_window_size(const struct device *dev,
				     enum agm_boot_target target);

/** "auto" / "internal" / "external". */
const char *agm_boot_mode_name(enum agm_boot_mode mode);

/** "empty" / "trial" / "confirmed" / "bad". */
const char *agm_boot_slot_state_name(enum agm_boot_slot_state state);

/**
 * @brief Run the console's `upload <target>` binary phase until the image is
 *        finished or the host stops.
 */
void agm_boot_upload_console(const struct device *dev, enum agm_boot_target target);

/** Bytes the console upload protocol (and the AN3155 server) drain from the
 *  UART one at a time. */
int agm_boot_console_poll(const struct device *dev, char *c);

/** Write to the console UART (protocol replies). */
void agm_boot_console_write(const struct device *dev, const void *buf, size_t len);

#if defined(CONFIG_BOOT_AGM_AN3155) || defined(__DOXYGEN__)
/**
 * @brief Serve one AN3155 session on the console UART until the host sends
 *        RESET/GO or the session times out.
 */
void agm_boot_an3155_run(const struct device *dev);

/** Print the last session's diagnostics (the console's `an-status`). */
void agm_boot_an3155_status_print(const struct device *dev);
#endif

#if defined(CONFIG_BOOT_AGM_SMP) || defined(__DOXYGEN__)
/** Feed one mcumgr (SMP) fragment -- one base64 line, header included. */
void agm_boot_smp_rx(const struct device *dev, const uint8_t *frag, uint32_t len);
#endif

/**
 * @name Trial-boot handshake
 *
 * A trial image is one that has never been seen to run. The A/B policy gives
 * it a limited number of boots and rolls back to the other slot after that
 * (see @ref AGM_BOOT_SLOT_TRIAL), but a count only advances when the board
 * *resets* -- and nothing resets a board whose new image hangs. So the loader
 * does two things right before it jumps into a trial image:
 *
 *   1. it arms the backup-domain watchdog (CONFIG_BOOT_AGM_TRIAL_WATCHDOG,
 *      CONFIG_BOOT_AGM_TRIAL_WATCHDOG_TIMEOUT_MS), and
 *   2. it writes @ref AGM_BOOT_TRIAL_TICKET_DR with the **A/B** boot record's
 *      commit sequence -- the record that says which *application* slot is
 *      active (drivers/misc/boot_agm.c), not the bitstream record the fabric
 *      boot path uses (include/zephyr/drivers/misc/agm_bitstream.h). The two
 *      are unrelated, even though a review once read this as the
 *      bitstream one.
 *
 * The application answers by calling agm_boot_trial_confirm() once it is
 * satisfied with itself -- it echoes the ticket into
 * @ref AGM_BOOT_TRIAL_CONFIRM_DR and silences the watchdog. On the next boot
 * the loader makes the trial slot permanent only if the echo matches the
 * ticket *and* the record still carries that sequence; a marker left behind by
 * an earlier image therefore cannot confirm one that was uploaded after it
 * (every upload commits a new record entry, hence a new sequence).
 *
 * If nothing confirms -- the image hung, crashed, or simply never got that far
 * -- the watchdog resets the board, the next boot spends another attempt, and
 * after AGM_BOOT_MAX_ATTEMPTS the slot goes bad and the other one boots.
 *
 * Both sides talk to the backup domain directly: those registers survive a SoC
 * reset (which is the lifetime that matters here) and the offsets come from the
 * map the drivers use too (include/zephyr/drivers/counter/agm_rtc_regs.h), so
 * the two cannot drift apart.
 *
 * @{
 */

/** Backup register the loader writes the trial ticket into. */
#define AGM_BOOT_TRIAL_TICKET_DR  2U
/** Backup register the application echoes it into. */
#define AGM_BOOT_TRIAL_CONFIRM_DR 3U

/**
 * @brief Encode the A/B record's commit sequence into a trial ticket.
 *
 * The ticket a trial boot hands out is the A/B boot record's commit sequence
 * in 16 bits, and 0 is the protocol's "nothing is pending": the application's
 * agm_boot_trial_confirm() answers -ENOENT on it, and the loader requires a
 * non-zero ticket before it will promote anything. The sequence is a 32-bit
 * counter though, so its low half is 0 once every 65536 commits -- on that
 * boot the loader would hand out a ticket the application cannot answer and
 * the loader would never accept, and a healthy trial image could never be
 * promoted: it would burn its attempts and be marked bad.
 *
 * The loader writes this encoding of the current sequence and compares
 * against the same encoding, so the one colliding value is mapped out of the
 * way. An application never needs to call it: it reads the ticket and echoes
 * it back unchanged (see agm_boot_trial_confirm()).
 *
 * @param seq The boot record's commit sequence (boot_agm_record_seq()).
 *
 * @return A non-zero 16-bit ticket for that sequence.
 */
static inline uint16_t agm_boot_trial_ticket_encode(uint32_t seq)
{
	uint16_t ticket = (uint16_t)seq;

	/* 0 means "no ticket is pending", so it cannot also be a ticket. */
	return (ticket == 0U) ? 0xFFFFU : ticket;
}

/* Bounded spin for the CRL.RTOFF wait, same shape as the counter driver's
 * (a backup-domain write does not latch until the previous one has settled). */
#define AGM_BOOT_TRIAL_WAIT_SPINS 100000U

#if DT_NODE_EXISTS(DT_NODELABEL(rtc0)) || defined(__DOXYGEN__)

#define AGM_BOOT_TRIAL_RTC_BASE ((uintptr_t)DT_REG_ADDR(DT_NODELABEL(rtc0)))

/** Wait for CRL.RTOFF, then write a 16-bit backup-domain register. */
static inline int agm_boot_trial_reg_write(uintptr_t offset, uint16_t value)
{
	volatile uint16_t *crl = (volatile uint16_t *)(AGM_BOOT_TRIAL_RTC_BASE +
						       AGM_RTC_CRL);

	for (unsigned int spin = 0U; spin < AGM_BOOT_TRIAL_WAIT_SPINS; spin++) {
		if ((*crl & AGM_RTC_CRL_RTOFF) != 0U) {
			*(volatile uint16_t *)(AGM_BOOT_TRIAL_RTC_BASE + offset) = value;
			return 0;
		}
	}
	return -ETIMEDOUT;
}

/** Read a 16-bit backup-domain register. */
static inline uint16_t agm_boot_trial_reg_read(uintptr_t offset)
{
	return *(volatile uint16_t *)(AGM_BOOT_TRIAL_RTC_BASE + offset);
}

/** Read the ticket the loader wrote for the boot that is running now. */
static inline uint16_t agm_boot_trial_ticket_get(void)
{
	return agm_boot_trial_reg_read(AGM_RTC_BKP_DR(AGM_BOOT_TRIAL_TICKET_DR));
}

/**
 * @brief Write the trial ticket (the bootloader's side of the handshake).
 *
 * The loader writes the boot record's commit sequence here right before it
 * jumps into a trial image, so that the application can echo it back. A zero
 * ticket means "nothing to answer"; the loader never writes one, because it
 * encodes the sequence with agm_boot_trial_ticket_encode() first.
 */
static inline int agm_boot_trial_ticket_set(uint16_t ticket)
{
	return agm_boot_trial_reg_write(AGM_RTC_BKP_DR(AGM_BOOT_TRIAL_TICKET_DR),
					ticket);
}

/** Read what the application echoed back (the loader's side). */
static inline uint16_t agm_boot_trial_confirm_get(void)
{
	return agm_boot_trial_reg_read(AGM_RTC_BKP_DR(AGM_BOOT_TRIAL_CONFIRM_DR));
}

/**
 * @brief Leave the trial watchdog off.
 *
 * The IWDG's enable bit lives in the backup domain and survives a reset, so the
 * watchdog a trial boot armed is still counting when the loader comes back --
 * including on the paths where it decides to fall back to the other slot or to
 * sit in the console. This clears it.
 */
static inline int agm_boot_trial_watchdog_off(void)
{
	uint16_t reg = agm_boot_trial_reg_read(AGM_RTC_IWDG);

	return agm_boot_trial_reg_write(AGM_RTC_IWDG,
					(uint16_t)(reg & ~AGM_RTC_IWDG_ENABLE));
}

/**
 * @brief Tell the bootloader that the image running now is good.
 *
 * Call this once the application's own self-test has passed: it echoes the
 * loader's ticket and silences the trial watchdog, and the next boot promotes
 * the slot from trial to confirmed.
 *
 * @retval 0        answered; the next boot promotes this slot
 * @retval -ENOENT  no ticket is pending (this image was not started as a trial)
 * @retval -ETIMEDOUT the backup domain did not go ready
 */
static inline int agm_boot_trial_confirm(void)
{
	uint16_t ticket = agm_boot_trial_ticket_get();
	int ret;

	if (ticket == 0U) {
		return -ENOENT;
	}
	ret = agm_boot_trial_reg_write(AGM_RTC_BKP_DR(AGM_BOOT_TRIAL_CONFIRM_DR),
				       ticket);
	if (ret < 0) {
		return ret;
	}
	return agm_boot_trial_watchdog_off();
}

/** Forget both registers (the loader does this once it has acted on them). */
static inline int agm_boot_trial_clear(void)
{
	int ret = agm_boot_trial_reg_write(AGM_RTC_BKP_DR(AGM_BOOT_TRIAL_CONFIRM_DR), 0U);

	if (ret < 0) {
		return ret;
	}
	return agm_boot_trial_reg_write(AGM_RTC_BKP_DR(AGM_BOOT_TRIAL_TICKET_DR), 0U);
}

#else /* no backup domain on this target */

/* The register primitives the driver's default hooks call. They exist here so
 * a build without an RTC node still compiles; a target like that answers
 * "no backup domain" and a native test overrides the hooks anyway. */
static inline uint16_t agm_boot_trial_reg_read(uintptr_t offset)
{
	ARG_UNUSED(offset);
	return 0U;
}

static inline int agm_boot_trial_reg_write(uintptr_t offset, uint16_t value)
{
	ARG_UNUSED(offset);
	ARG_UNUSED(value);
	return -ENOTSUP;
}

static inline uint16_t agm_boot_trial_ticket_get(void)
{
	return 0U;
}

static inline int agm_boot_trial_ticket_set(uint16_t ticket)
{
	ARG_UNUSED(ticket);
	return -ENOTSUP;
}

static inline uint16_t agm_boot_trial_confirm_get(void)
{
	return 0U;
}

static inline int agm_boot_trial_watchdog_off(void)
{
	return -ENOTSUP;
}

static inline int agm_boot_trial_confirm(void)
{
	return -ENOTSUP;
}

static inline int agm_boot_trial_clear(void)
{
	return -ENOTSUP;
}

#endif /* DT_NODE_EXISTS(DT_NODELABEL(rtc0)) */

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_INCLUDE_DRIVERS_MISC_BOOT_AGM_H_ */
