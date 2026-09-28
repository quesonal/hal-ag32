/*
 * Copyright (c) 2026 AgRV Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief AgRV2K bootloader / DFU driver -- command authorization.
 *
 * The one part of the driver that exists only for a locked build, and the only
 * one that verifies a host-supplied signature. The driver asks it two questions
 * -- "hand out a nonce" and "may this state change through" -- and it answers
 * from the record's sequence number and cycle counters, never from a secret the
 * device holds.
 */

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

#include <string.h>

#include "boot_agm_priv.h"

#if defined(AGM_HAVE_TINYCRYPT_ECDSA)
#include <tinycrypt/ecc.h>
#include <tinycrypt/ecc_dsa.h>
#include <tinycrypt/sha256.h>
#endif

/* ---- command authorization ------------------------------------ */

/*
 * The locked profile (CONFIG_BOOT_AGM_LOCK_PRODUCTION) does not let a state
 * change through just because a host can reach the console: publishing an
 * upload and erasing a target need a *signed command* first. What
 * matters here:
 *
 *   NONCE     -> a fresh 16-byte value (record seq ^ cycle counter ^ session
 *                counter: it only has to be unique per session, the signature
 *                is what binds the command, and this SoC has no entropy driver
 *                at this point in the boot);
 *   AUTHORIZE -> the host returns that nonce with
 *                nonce[16] || cmd[1] || args_len[2] (LE) || args and an ECDSA
 *                P-256 signature over its SHA-256, verified by the same
 *                tinycrypt path the image verifier uses.
 *
 * A grant is one command, one shot: it remembers the command it was signed for
 * and is spent by the call it authorizes (whoever authorized an erase has not
 * authorized an upload), and an unused grant dies with its nonce. The
 * arguments are inside the signature, but a grant is deliberately not bound to
 * a target or a length.
 */

#if defined(AGM_HAVE_TINYCRYPT_ECDSA)
static uint8_t auth_nonce[AGM_BOOT_NONCE_LEN];
static bool auth_nonce_live;
#endif
/* The session counter is in the nonce mix whatever else is compiled in: it is
 * what makes two requests in the same cycle differ. */
static uint32_t auth_nonces;
static uint8_t auth_grant_cmd;
static bool auth_granted;

/* The P-256 key the commands are authorized with -- a *per-profile* answer,
 * so the matrix is written out instead of hiding behind nested #ifs:
 *
 *   application profile | key the authorization uses
 *   --------------------+-----------------------------------------------
 *   ECDSA P-256         | agm_boot_pubkey (64 B raw X||Y)
 *   BOOT_AGM_SIG_NONE   | agm_boot_pubkey (same weak default; this build's
 *                       | AGM_BOOT_PUBKEY_LEN is 64, not the RSA DER)
 *   RSA-2048-PSS        | agm_boot_bitstream_pubkey -- the *fabric* key,
 *                       | because this build's own key is DER rather than a
 *                       | point on the curve, and only if the fabric is
 *                       | signed at all (BOOT_AGM_BITSTREAM_SIGNED)
 *
 * An all-zero key (the weak default, or a fabric key the application never
 * set) means "this build has none": NULL comes back and the caller refuses
 * every command rather than accepting any. The key is deliberately *not*
 * per-target: the grant hands out neither a target nor a length, so one
 * key has to cover the whole command set -- an operator who provisions a
 * separate fabric key still authorizes commands with the application key,
 * and that is the documented policy. */
#if defined(AGM_HAVE_TINYCRYPT_ECDSA)
static const uint8_t *auth_key(void)
{
#if defined(CONFIG_BOOT_AGM_SIG_RSA2048_PSS)
#if defined(CONFIG_BOOT_AGM_BITSTREAM_SIGNED)
	if (!boot_agm_key_present(agm_boot_bitstream_pubkey, AGM_BOOT_BITSTREAM_PUBKEY_LEN)) {
		return NULL;
	}
	return agm_boot_bitstream_pubkey;
#else
	return NULL; /* an RSA-only build carries no P-256 key at all */
#endif
#else
	if (!boot_agm_key_present(agm_boot_pubkey, AGM_BOOT_PUBKEY_LEN)) {
		return NULL;
	}
	return agm_boot_pubkey;
#endif
}
#endif /* AGM_HAVE_TINYCRYPT_ECDSA */

/* The boot report (boot_agm.c) warns when the locked profile has nothing to
 * verify with; it only needs the yes/no, not the key. */
bool boot_agm_auth_key_ready(void)
{
#if defined(AGM_HAVE_TINYCRYPT_ECDSA)
	return auth_key() != NULL;
#else
	return false;
#endif
}

int agm_boot_nonce_get(const struct device *dev, uint8_t nonce[AGM_BOOT_NONCE_LEN])
{
	uint32_t s;

	ARG_UNUSED(dev);

	if (nonce == NULL) {
		return -EINVAL;
	}

	/* A mixer, not a CSPRNG (see the section comment). k_cycle_get_32() is
	 * read once: two requests in the same cycle still differ, because the
	 * session counter goes into the mix too. */
	auth_nonces++;
	s = (boot_agm_record_seq() * 0x9e3779b9U) ^ k_cycle_get_32() ^
	    (auth_nonces * 0x85ebca6bU);
	s |= 1U;
	for (uint32_t i = 0U; i < AGM_BOOT_NONCE_LEN; i++) {
		s ^= s << 13;
		s ^= s >> 17;
		s ^= s << 5;
		nonce[i] = (uint8_t)(s >> 24);
	}

#if defined(AGM_HAVE_TINYCRYPT_ECDSA)
	memcpy(auth_nonce, nonce, AGM_BOOT_NONCE_LEN);
	auth_nonce_live = true;
#endif
	/* A fresh nonce starts a new session, so a grant armed for a command
	 * the host has not run yet is dropped here rather than carried over --
	 * "authorize, then ask for another nonce, then run the command" is a
	 * host bug, and it fails with the gate's line rather than silently
	 * working . Any *used* grant was spent at the call it
	 * authorized, so only an unused one can be lost this way. */
	auth_granted = false;
	return 0;
}

int agm_boot_authorize(const struct device *dev, const uint8_t *nonce, uint8_t cmd,
		       const uint8_t *args, uint32_t args_len, const uint8_t *sig,
		       uint32_t sig_len)
{
	ARG_UNUSED(dev);

	if (args_len > 0xFFFFU || (args_len != 0U && args == NULL)) {
		return -EINVAL;
	}
	if (cmd != AGM_BOOT_AUTH_CMD_ERASE && cmd != AGM_BOOT_AUTH_CMD_PUBLISH) {
		return -EINVAL;
	}

#if defined(AGM_HAVE_TINYCRYPT_ECDSA)
	const uint8_t *key = auth_key();
	uint8_t hash[32];
	uint8_t raw[MCUBOOT_ECDSA_SIG_LEN];
	uint8_t len_le[2] = { (uint8_t)args_len, (uint8_t)(args_len >> 8) };
	struct tc_sha256_state_struct sha;

	if (key == NULL) {
		/* No P-256 key compiled in: nothing can ever be authorized in
		 * this build, which is what "fail closed" has to look like. */
		return -ENOTSUP;
	}
	if (sig == NULL || sig_len == 0U) {
		return -EINVAL;
	}
	if (nonce == NULL || !auth_nonce_live ||
	    memcmp(nonce, auth_nonce, AGM_BOOT_NONCE_LEN) != 0) {
		return -EACCES; /* never handed out, or already spent */
	}

	tc_sha256_init(&sha);
	tc_sha256_update(&sha, nonce, AGM_BOOT_NONCE_LEN);
	tc_sha256_update(&sha, &cmd, 1U);
	tc_sha256_update(&sha, len_le, sizeof(len_le));
	if (args_len != 0U) {
		tc_sha256_update(&sha, args, args_len);
	}
	tc_sha256_final(hash, &sha);

	/* The signature may arrive in either shape the header documents: the
	 * raw r||s pair tinycrypt takes, or the DER form (what imgtool's
	 * containers and most hosts carry). Everything after this is the same
	 * call the image verifier makes. */
	if (sig_len == MCUBOOT_ECDSA_SIG_LEN) {
		memcpy(raw, sig, MCUBOOT_ECDSA_SIG_LEN);
	} else if (boot_agm_sig_der_to_raw(sig, sig_len, raw) < 0) {
		printk("loader: command signature is neither raw r||s nor valid DER\n");
		return -EINVAL;
	}
	if (uECC_verify(key, hash, 32U, raw, uECC_secp256r1()) != 1) {
		printk("loader: command signature is not valid\n");
		return -EACCES;
	}

	/* One nonce, one command -- and note *where* this sits: after the
	 * verify, not before it. A bad signature therefore does not burn the
	 * nonce, which is the only sane direction (an attacker who could spend a
	 * legitimate host's challenge with one garbage signature would have a
	 * denial of service). The next NONCE replaces this one anyway; what the
	 * flag stops is a *replay* of this one . */
	auth_nonce_live = false;
	auth_grant_cmd = cmd;
	auth_granted = true;
	return 0;
#else
	ARG_UNUSED(nonce);
	ARG_UNUSED(args);
	ARG_UNUSED(sig);
	ARG_UNUSED(sig_len);
	/* No signed profile and not LOCK_PRODUCTION: this build links no curve
	 * (CONFIG_BOOT_AGM_SIG_NONE), so there is nothing that could verify a
	 * command signature. */
	return -ENOTSUP;
#endif
}

/* The gate the three state-changing entry points call first (opening an upload,
 * publishing one, erasing a target): true when the command may run. Outside the
 * locked profile it always may (the gate leaves the dev board behaviour
 * unchanged); inside it the grant has to have been signed for *this*
 * command, and it is spent here.
 *
 * It reports its own refusal, because the interesting part is not "denied" but
 * *why*: the two ways a host gets here with a fresh authorization in hand are
 * a grant armed for a different command ("I authorized an erase, why does the
 * publish fail?" -- one grant covers one command) and a NONCE fetched after
 * that authorization, which starts a new session and drops the old grant (see
 * agm_boot_nonce_get()). Either line makes that obvious instead of looking
 * like a signature problem. */
bool boot_agm_auth_take_locked(uint8_t cmd, const char *verb, const char *what)
{
	if (auth_granted && auth_grant_cmd == cmd) {
		auth_granted = false;
		return true;
	}

	if (auth_granted) {
		printk("loader: refusing to %s %s: command 0x%02x is armed, not 0x%02x "
		       "(one grant covers one command)\n", verb, what, auth_grant_cmd, cmd);
	} else {
		printk("loader: refusing to %s %s: no grant armed for command 0x%02x "
		       "(a fresh nonce voids an unused grant)\n", verb, what, cmd);
	}
	return false;
}
