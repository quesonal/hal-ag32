/*
 * Copyright (c) 2026 AgRV Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief mcumgr (SMP) image server for the AgRV2K bootloader driver.
 *
 * The second host path next to AN3155 (boot_agm_an3155.c), on the same console
 * UART: `smpmgr -p <port> image upload --format any app.bin` / `os reset`. Only
 * the "over console" framing (base64 lines behind 0x0609/0x0414, what Zephyr's
 * MCUMGR_TRANSPORT_UART sends) is used, so mcumgr/smpmgr work unmodified; the
 * transport is instantiated here because the stock UART transport wants the
 * port for itself through an IRQ, and the console and AN3155 need it too
 * (src/main.c demultiplexes by first byte).
 *
 * Two groups. `image` (0/1 = the stores, 2 = the on-die application slot,
 * programmed directly -- nothing is copied at boot) answers with the *driver's*
 * semantics, because the stock img_mgmt group expects MCUboot headers and a
 * flash map the stores are not; the end of an upload publishes TRIAL + active,
 * exactly like `install`, AN3155 GO and `upload slot`. The loader's own group
 * 0x40 is the signed-command pair the locked profile wants before
 * publishing or erasing:
 * cmd 0 NONCE (read) answers `{"nonce": bstr}`, cmd 1 AUTHORIZE (write) takes
 * `{"nonce","cmd","args"?,"sig"}` and leaves a device-wide, one-shot grant.
 * The signature covers `nonce[16] || cmd[1] || args_len[2] (LE) || args`, not
 * the CBOR (boot_agm_auth.c); with no trusted key it answers ENOTSUP rather
 * than accepting anything.
 */

#include <zephyr/kernel.h>
#include <zephyr/net_buf.h>
#include <zephyr/sys/util.h>
#include <zcbor_decode.h>
#include <zcbor_encode.h>
#include <zephyr/mgmt/mcumgr/mgmt/mgmt.h>
#include <zephyr/mgmt/mcumgr/mgmt/handlers.h>
#include <zephyr/mgmt/mcumgr/smp/smp.h>
#include <zephyr/mgmt/mcumgr/transport/smp.h>
#include <zephyr/mgmt/mcumgr/transport/serial.h>
#include <mgmt/mcumgr/util/zcbor_bulk.h>
#include <mgmt/mcumgr/transport/smp_internal.h>

#include <zephyr/drivers/misc/boot_agm.h>

#include <string.h>

/* SMP payload we accept per request. The host sizes its chunks from what the
 * transport_mgmt group reports, so this only has to be at least as large as
 * the netbuf the mcumgr transport layer allocates (CONFIG_MCUMGR_TRANSPORT_
 * NETBUF_SIZE). */
#define SMP_BOOT_MTU 256U

/* Upload targets: image 0 = store A, image 1 = store B, image 2 = the on-die
 * application slot (the on-die DFU), image 3 = the bitstream staging area.
 * The driver's enum uses the same order, so the request's `image`/`slot` value
 * is the target.
 *
 * `image state` lists images only: the three below. The bitstream is accepted
 * by upload and erase but not listed (the client's ImageState would have to
 * call a fabric bitstream "bootable"). */
#define SMP_BOOT_IMG_COUNT  (AGM_BOOT_TARGET_SLOT + 1U)
#define SMP_BOOT_TARGET_MAX AGM_BOOT_TARGET_BITSTREAM

/* The image group's error codes, as the protocol numbers them (Zephyr's
 * zephyr/mgmt/mcumgr/grp/img_mgmt/img_mgmt.h carries the same list, but that
 * header pulls in MCUboot's bootutil/image.h, which this build has no reason
 * to carry -- the image group here is hand-written). */
#define SMP_BOOT_IMG_ERR_OK             0U
#define SMP_BOOT_IMG_ERR_NO_IMAGE       3U
#define SMP_BOOT_IMG_ERR_HASH_NOT_FOUND 8U
#define SMP_BOOT_IMG_ERR_INVALID_SLOT  14U
#define SMP_BOOT_IMG_ERR_INVALID_HASH  24U

/* Store the SMP transport writes its responses through. */
static struct smp_transport smp_boot_transport;
static struct mcumgr_serial_rx_ctxt smp_boot_rx;

static int smp_boot_tx(const void *data, int len)
{
	agm_boot_console_write(NULL, data, (size_t)len);
	return 0;
}

static int smp_boot_out(struct net_buf *nb)
{
	int rc = mcumgr_serial_tx_pkt(nb->data, nb->len, smp_boot_tx);

	smp_packet_free(nb);
	return rc;
}

static uint16_t smp_boot_get_mtu(const struct net_buf *nb)
{
	ARG_UNUSED(nb);
	return SMP_BOOT_MTU;
}

/* Called by the console's line reader with one raw fragment (the bytes of a
 * base64 line, header included, newline included). */
void agm_boot_smp_rx(const struct device *dev, const uint8_t *frag, uint32_t len)
{
	struct net_buf *nb = mcumgr_serial_process_frag(&smp_boot_rx, frag, len);

	ARG_UNUSED(dev);

	if (nb != NULL) {
		smp_rx_req(&smp_boot_transport, nb);
	}
}

/* ---- image management group ---------------------------------------- */

/* Upload state for the request being reassembled by the host. */
static struct {
	uint32_t slot;
	uint32_t off;
	uint32_t size;
	bool active;
} img_up;

static int smp_boot_rsp_off(struct smp_streamer *ctxt, uint32_t off)
{
	zcbor_state_t *zse = ctxt->writer->zs;
	bool ok = zcbor_tstr_put_lit(zse, "off") && zcbor_uint32_put(zse, off);

	return ok ? MGMT_ERR_EOK : MGMT_ERR_EMSGSIZE;
}

static int smp_boot_img_upload(struct smp_streamer *ctxt)
{
	zcbor_state_t *zsd = ctxt->reader->zs;
	struct zcbor_string data = { 0 };
	struct zcbor_string sha = { 0 };
	/* "image" is only sent with the first chunk (off == 0); UINT32_MAX
	 * means "not present here", not "image 0" (getting that wrong made
	 * every continuation of a non-zero target fail with EBADSTATE). */
	uint32_t image = UINT32_MAX;
	uint32_t off = UINT32_MAX;
	uint32_t len = UINT32_MAX;
	bool upgrade = false;
	bool ok;
	size_t decoded = 0U;
	int rc;

	struct zcbor_map_decode_key_val upload_decode[] = {
		ZCBOR_MAP_DECODE_KEY_DECODER("image", zcbor_uint32_decode, &image),
		ZCBOR_MAP_DECODE_KEY_DECODER("data", zcbor_bstr_decode, &data),
		ZCBOR_MAP_DECODE_KEY_DECODER("len", zcbor_uint32_decode, &len),
		ZCBOR_MAP_DECODE_KEY_DECODER("off", zcbor_uint32_decode, &off),
		ZCBOR_MAP_DECODE_KEY_DECODER("sha", zcbor_bstr_decode, &sha),
		ZCBOR_MAP_DECODE_KEY_DECODER("upgrade", zcbor_bool_decode, &upgrade),
	};

	ok = zcbor_map_decode_bulk(zsd, upload_decode, ARRAY_SIZE(upload_decode),
				   &decoded) == 0;
	if (!ok || off == UINT32_MAX ||
	    (image != UINT32_MAX && image > SMP_BOOT_TARGET_MAX)) {
		return MGMT_ERR_EINVAL;
	}

	if (off == 0U) {
		if (image == UINT32_MAX) {
			image = AGM_BOOT_TARGET_STORE_A;   /* the protocol's default */
		}
		/* First chunk: the host also names the image (= our target)
		 * and its total length here. Its `sha` field is the SHA-256 of
		 * the whole image; the loader's own integrity check is the
		 * CRC32 in its boot record, so the hash is accepted and not
		 * verified -- which is what `smpmgr --format any` (no MCUboot
		 * header to check) expects of the device. */
		img_up.slot = image;
		img_up.size = (len == UINT32_MAX) ? 0U : len;
		img_up.off = 0U;   /* a new upload starts over, even if the
				    * previous one in this session ended here */
		img_up.active = true;
		rc = agm_boot_upload_begin(NULL, (enum agm_boot_target)img_up.slot);
		if (rc < 0) {
			/* A production-locked build refuses to open the session
			 * without an ERASE grant : report
			 * that as "denied" like every other gate refusal, and
			 * leave the session closed so the writes that follow
			 * cannot land either. Anything else here is a bad
			 * target or a flash error, which EINVAL covers. */
			img_up.active = false;
			return (rc == AGM_BOOT_E_UNAUTHORIZED) ? MGMT_ERR_EACCESSDENIED
							       : MGMT_ERR_EINVAL;
		}
	} else if (!img_up.active) {
		/* A continuation for something we are not uploading. */
		return MGMT_ERR_EBADSTATE;
	}

	if (off != img_up.off) {
		/* The host picks the offset up from our own replies, so this
		 * only trips when it restarts a session mid-flight. */
		return MGMT_ERR_EINVAL;
	}

	if (data.len != 0U) {
		rc = agm_boot_upload_write(NULL, (enum agm_boot_target)img_up.slot, off,
					      data.value, data.len);
		if (rc < 0) {
			img_up.active = false;
			return MGMT_ERR_EUNKNOWN;
		}
		img_up.off = off + (uint32_t)data.len;
	}

	if (img_up.size != 0U && img_up.off == img_up.size) {
		rc = agm_boot_upload_finish(NULL, (enum agm_boot_target)img_up.slot, img_up.off);
		img_up.active = false;

		/* The response still carries the offset: the host stops on
		 * off == len(image), and only then does it look at rc.
		 *
		 * A failure here is the verifier's verdict (bad digest/signature,
		 * a container for another slot, no trusted key in this build) or
		 * a record write that did not take: the image itself is the
		 * suspect, so say "access denied" rather than EUNKNOWN, which
		 * reads like an internal fault. Same split as the console
		 * phase's UP_ERR_REJECT (drivers/misc/boot_agm.c). */
		/* An older image gets its own code: mcumgr clients ask for a
		 * specific version, so "too old" is not "access denied". */
		if (rc == AGM_BOOT_E_OLD_VERSION) {
			return MGMT_ERR_EBADSTATE;
		}
		if (rc < 0) {
			return MGMT_ERR_EACCESSDENIED;
		}
	}

	return smp_boot_rsp_off(ctxt, img_up.off);
}

/* The image list both "image state" handlers answer with: the two external
 * stores plus the on-die target. Zephyr's own handler replies to a state
 * *write* with the list too (img_mgmt_state.c calls img_mgmt_state_read()), and
 * a client that does not get it reports "Frame could not be parsed". */
/* One target's state as `image state` reports it: a store counts only when its
 * record entry really points at the store, and the on-die target is the same
 * record entry when it carries AGM_BOOT_SRC_ON_DIE. */
static void smp_boot_target_state(uint32_t target, uint32_t *len, uint32_t *crc,
				  bool *active, bool *confirmed)
{
	struct agm_boot_info info;
	bool used;

	(void)agm_boot_info_get(NULL, &info);

	if (target == AGM_BOOT_TARGET_SLOT) {
		used = info.record_valid && info.slot[0].src == AGM_BOOT_SRC_ON_DIE &&
		       info.slot[0].len != 0U;
		*len = used ? info.slot[0].len : 0U;
		*crc = used ? info.slot[0].crc : 0U;
		*active = used && info.active == 0U;
		*confirmed = used && info.slot[0].state == AGM_BOOT_SLOT_CONFIRMED;
		return;
	}

	used = info.record_valid && info.slot[target].src == AGM_BOOT_SRC_STORE &&
	       info.slot[target].state != AGM_BOOT_SLOT_BAD && info.slot[target].len != 0U;
	*len = used ? info.slot[target].len : 0U;
	*crc = used ? info.slot[target].crc : 0U;
	*active = used && info.active == target;
	*confirmed = used && info.slot[target].state == AGM_BOOT_SLOT_CONFIRMED;
}

static bool smp_boot_images_put(zcbor_state_t *zse)
{
	bool ok = zcbor_tstr_put_lit(zse, "images") &&
		  zcbor_list_start_encode(zse, SMP_BOOT_IMG_COUNT);

	for (uint32_t slot = 0U; ok && slot < SMP_BOOT_IMG_COUNT; slot++) {
		uint32_t len = 0U;
		uint32_t crc = 0U;
		bool active = false;
		bool confirmed = false;

		smp_boot_target_state(slot, &len, &crc, &active, &confirmed);

		ok = zcbor_map_start_encode(zse, 8) &&
		     zcbor_tstr_put_lit(zse, "image") &&
		     zcbor_uint32_put(zse, slot) &&
		     zcbor_tstr_put_lit(zse, "slot") && zcbor_uint32_put(zse, 0U) &&
		     zcbor_tstr_put_lit(zse, "version") &&
		     zcbor_tstr_put_lit(zse, (len != 0U) ? "1.0.0" : "0.0.0");

		if (ok && len != 0U) {
			ok = zcbor_tstr_put_lit(zse, "bootable") &&
			     zcbor_bool_put(zse, true);
		}
		if (ok && active) {
			ok = zcbor_tstr_put_lit(zse, "active") && zcbor_bool_put(zse, true);
		}
		if (ok && confirmed) {
			ok = zcbor_tstr_put_lit(zse, "confirmed") &&
			     zcbor_bool_put(zse, true) &&
			     zcbor_tstr_put_lit(zse, "permanent") &&
			     zcbor_bool_put(zse, true);
		}
		ok = ok && zcbor_map_end_encode(zse, 8);
	}

	return ok && zcbor_list_end_encode(zse, SMP_BOOT_IMG_COUNT);
}

static int smp_boot_img_state_read(struct smp_streamer *ctxt)
{
	bool ok = smp_boot_images_put(ctxt->writer->zs);

	return ok ? MGMT_ERR_EOK : MGMT_ERR_EMSGSIZE;
}

/* "image state" (write): test/confirm an upload. Same rules as Zephyr's
 * handler -- a confirm without a hash means "the image that is running",
 * a hash we cannot match (we keep CRC32 records, not MCUboot SHA-256) is
 * only accepted at the protocol's 32-byte shape, and a bare "test" (no
 * hash, no confirm) is invalid. */
static int smp_boot_img_state_write(struct smp_streamer *ctxt)
{
	zcbor_state_t *zsd = ctxt->reader->zs;
	zcbor_state_t *zse = ctxt->writer->zs;
	struct zcbor_string hash = { 0 };
	bool confirm = false;
	bool ok;
	size_t decoded = 0U;
	int rc = 0;

	struct zcbor_map_decode_key_val state_decode[] = {
		ZCBOR_MAP_DECODE_KEY_DECODER("hash", zcbor_bstr_decode, &hash),
		ZCBOR_MAP_DECODE_KEY_DECODER("confirm", zcbor_bool_decode, &confirm),
	};

	ok = zcbor_map_decode_bulk(zsd, state_decode, ARRAY_SIZE(state_decode),
				   &decoded) == 0;
	if (!ok) {
		return MGMT_ERR_EINVAL;
	}

	if (!confirm && hash.len == 0U) {
		/* A test without a hash: the upload already published the
		 * image as TRIAL + active, but the protocol calls this
		 * invalid, so say so instead of pretending. */
		ok = smp_add_cmd_err(zse, MGMT_GROUP_ID_IMAGE,
				     SMP_BOOT_IMG_ERR_INVALID_HASH);
		return ok ? MGMT_ERR_EOK : MGMT_ERR_EMSGSIZE;
	}
	if (hash.len != 0U && hash.len != 32U) {
		ok = smp_add_cmd_err(zse, MGMT_GROUP_ID_IMAGE,
				     SMP_BOOT_IMG_ERR_INVALID_HASH);
		return ok ? MGMT_ERR_EOK : MGMT_ERR_EMSGSIZE;
	}

	if (confirm) {
		rc = agm_boot_confirm(NULL);
	}

	/* Answer with the updated list, like Zephyr's img_mgmt_state_write(). */
	if (rc == AGM_BOOT_E_UNAUTHORIZED) {
		/* `confirm` is A/B policy and never gated, but the mapping
		 * belongs next to the others: if that ever changes, the client
		 * has to see "denied" rather than a silent success. */
		return MGMT_ERR_EACCESSDENIED;
	}
	return smp_boot_img_state_read(ctxt);
}

/* "image erase": make the target stop holding an image (the record entry is
 * emptied, and the on-die target is also erased back to blank). The host names
 * it with "slot" ("image" is accepted as an alias); the spec's default when
 * neither is present is slot 1. */
static int smp_boot_img_erase(struct smp_streamer *ctxt)
{
	zcbor_state_t *zsd = ctxt->reader->zs;
	uint32_t slot = UINT32_MAX;
	uint32_t image = UINT32_MAX;
	bool ok;
	size_t decoded = 0U;
	int rc;

	struct zcbor_map_decode_key_val erase_decode[] = {
		ZCBOR_MAP_DECODE_KEY_DECODER("slot", zcbor_uint32_decode, &slot),
		ZCBOR_MAP_DECODE_KEY_DECODER("image", zcbor_uint32_decode, &image),
	};

	ok = zcbor_map_decode_bulk(zsd, erase_decode, ARRAY_SIZE(erase_decode),
				   &decoded) == 0;
	if (!ok) {
		return MGMT_ERR_EINVAL;
	}
	if (slot == UINT32_MAX) {
		slot = (image == UINT32_MAX) ? AGM_BOOT_TARGET_STORE_B : image;
	}
	if (slot > SMP_BOOT_TARGET_MAX) {
		return MGMT_ERR_EINVAL;
	}

	rc = agm_boot_erase(NULL, (enum agm_boot_target)slot);
	if (rc == 0) {
		return MGMT_ERR_EOK;
	}
	/* A production-locked build refuses this until the command has been
	 * authorized: "denied" is a different operator action from
	 * "something went wrong inside".
	 *
	 * `-EINVAL` is the driver's "there is a record and it is not usable"
	 * (a blank one is not an error at all -- see agm_boot_erase()); the
	 * host has to see "invalid request" there, because EUNKNOWN reads like
	 * a device fault. Flash-level failures keep EUNKNOWN, like the upload
	 * path above does for the same reason.
	 */
	if (rc == AGM_BOOT_E_UNAUTHORIZED) {
		return MGMT_ERR_EACCESSDENIED;
	}
	if (rc == -EINVAL) {
		return MGMT_ERR_EINVAL;
	}
	return MGMT_ERR_EUNKNOWN;
}

static const struct mgmt_handler smp_boot_img_handlers[] = {
	[0] = { /* IMG_MGMT_ID_STATE */
		.mh_read = smp_boot_img_state_read,
		.mh_write = smp_boot_img_state_write,
	},
	[1] = { /* IMG_MGMT_ID_UPLOAD */
		.mh_read = NULL,
		.mh_write = smp_boot_img_upload,
	},
	[5] = { /* IMG_MGMT_ID_ERASE */
		.mh_read = NULL,
		.mh_write = smp_boot_img_erase,
	},
};

static struct mgmt_group smp_boot_img_group = {
	.mg_handlers = (struct mgmt_handler *)smp_boot_img_handlers,
	.mg_handlers_count = ARRAY_SIZE(smp_boot_img_handlers),
	.mg_group_id = MGMT_GROUP_ID_IMAGE,
#ifdef CONFIG_MCUMGR_GRP_ENUM_DETAILS_NAME
	.mg_group_name = "boot loader image",
#endif
};

/* ---- the loader's own group: the signed commands --------------- */

/* mcumgr reserves group ids 0-63 for the standard groups; this is a user
 * group (the number is part of the command-authorization contract). */
#define SMP_BOOT_AUTH_GROUP_ID 0x40U
#define SMP_BOOT_AUTH_ID_NONCE 0U
#define SMP_BOOT_AUTH_ID_AUTHORIZE 1U

/* The nonce, as one CBOR byte string (a text form would double the length for
 * no gain, and the host has to feed the same bytes to SHA-256).
 *
 * The response map is the framework's (smp.c opens and closes it around every
 * handler, unless the group asks for a custom payload): a handler only writes
 * its own key/value pairs. Opening one here *nests* a map inside that map, and
 * the result is CBOR a client cannot parse -- where the
 * first `nonce` request was answered with `bf bf 65"nonce" 50...` and the host
 * died with "premature end of stream". */
static int smp_boot_auth_nonce(struct smp_streamer *ctxt)
{
	zcbor_state_t *zse = ctxt->writer->zs;
	uint8_t nonce[AGM_BOOT_NONCE_LEN];

	if (agm_boot_nonce_get(NULL, nonce) < 0) {
		return MGMT_ERR_EUNKNOWN;
	}

	bool ok = zcbor_tstr_put_lit(zse, "nonce") &&
		  zcbor_bstr_encode_ptr(zse, nonce, sizeof(nonce));

	return ok ? MGMT_ERR_EOK : MGMT_ERR_EMSGSIZE;
}

/* One signed command. The request carries the fields separately (that is what
 * CBOR is for); the device reassembles them into the byte string the host
 * signed, which is the one agm_boot_authorize() documents. */
static int smp_boot_auth_authorize(struct smp_streamer *ctxt)
{
	zcbor_state_t *zsd = ctxt->reader->zs;
	struct zcbor_string nonce = { 0 };
	struct zcbor_string args = { 0 };
	struct zcbor_string sig = { 0 };
	/* "args" is optional: absent and zero-length are the same thing here. */
	uint32_t cmd = UINT32_MAX;
	bool ok;
	size_t decoded = 0U;
	int rc;

	struct zcbor_map_decode_key_val auth_decode[] = {
		ZCBOR_MAP_DECODE_KEY_DECODER("nonce", zcbor_bstr_decode, &nonce),
		ZCBOR_MAP_DECODE_KEY_DECODER("cmd", zcbor_uint32_decode, &cmd),
		ZCBOR_MAP_DECODE_KEY_DECODER("args", zcbor_bstr_decode, &args),
		ZCBOR_MAP_DECODE_KEY_DECODER("sig", zcbor_bstr_decode, &sig),
	};

	ok = zcbor_map_decode_bulk(zsd, auth_decode, ARRAY_SIZE(auth_decode), &decoded) == 0;
	if (!ok || nonce.len != AGM_BOOT_NONCE_LEN || cmd == UINT32_MAX || sig.len == 0U) {
		return MGMT_ERR_EINVAL;
	}

	rc = agm_boot_authorize(NULL, nonce.value, (uint8_t)cmd, args.value,
				(uint32_t)args.len, sig.value, (uint32_t)sig.len);
	if (rc == 0) {
		/* Nothing to say beyond the header's return code (the framework
		 * owns the response map: see smp_boot_auth_nonce()). */
		return MGMT_ERR_EOK;
	}
	if (rc == -ENOTSUP) {
		/* No key (or no verifier) in this build: the command can never be
		 * authorized here, which the client has to be able to tell apart
		 * from "your signature is wrong". */
		return MGMT_ERR_ENOTSUP;
	}
	if (rc == -EACCES) {
		return MGMT_ERR_EACCESSDENIED;
	}
	return MGMT_ERR_EINVAL;
}

static const struct mgmt_handler smp_boot_auth_handlers[] = {
	[SMP_BOOT_AUTH_ID_NONCE] = {
		.mh_read = smp_boot_auth_nonce,
		.mh_write = NULL,
	},
	[SMP_BOOT_AUTH_ID_AUTHORIZE] = {
		.mh_read = NULL,
		.mh_write = smp_boot_auth_authorize,
	},
};

static struct mgmt_group smp_boot_auth_group = {
	.mg_handlers = (struct mgmt_handler *)smp_boot_auth_handlers,
	.mg_handlers_count = ARRAY_SIZE(smp_boot_auth_handlers),
	.mg_group_id = SMP_BOOT_AUTH_GROUP_ID,
#ifdef CONFIG_MCUMGR_GRP_ENUM_DETAILS_NAME
	.mg_group_name = "boot loader authorization",
#endif
};

static int smp_boot_init(void)
{
	smp_boot_transport.functions.output = smp_boot_out;
	smp_boot_transport.functions.get_mtu = smp_boot_get_mtu;

	if (smp_transport_init(&smp_boot_transport) != 0) {
		return -EINVAL;
	}

	mgmt_register_group(&smp_boot_img_group);
	mgmt_register_group(&smp_boot_auth_group);
	return 0;
}

SYS_INIT(smp_boot_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
