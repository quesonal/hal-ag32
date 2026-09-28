/*
 * Copyright (c) 2026 AgRV Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief The loader's SMP (mcumgr) server: wire format and error mapping.
 *
 * Runs on native_sim against the same stand-in flashes as the api suite
 * (app.overlay) and drives the server through the entry point the sample's
 * console uses, `agm_boot_smp_rx()`, with requests this test frames itself
 * (0x0609 + base64 + CRLF, the serial transport's shape).
 *
 * Why this suite exists: the two groups below shipped without any automated
 * coverage, and the first bug in them -- a CBOR map opened inside the map the
 * mcumgr framework already owns, which no client can parse -- was found on the
 * dev board instead (fixed in 0fcf4ab). What is pinned
 * here is the layer a hand-written client depends on:
 *
 *   * `image state` answers a map with a three-entry "images" list;
 *   * group 0x40 cmd 0 answers a 16-byte "nonce";
 *   * the error mapping: malformed body -> EINVAL(3), unknown command ->
 *     ENOTSUP(8), a build without a verifier -> ENOTSUP(8), and the authorization gate's
 *     refusal -> EACCESSDENIED(11);
 *   * the locked profile's whole flow: erase refused, authorize, upload
 *     published, the grant spent, authorize again, erase through.
 *
 * Two scenarios run this file: the default profile (no gate) and
 * `api_locked` (CONFIG_BOOT_AGM_LOCK_PRODUCTION, which is also what makes the
 * test sign the device's nonce with the same tinycrypt the driver verifies
 * with). Where they differ, the case says so.
 */

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/drivers/misc/boot_agm.h>
#include <zephyr/drivers/serial/uart_emul.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/base64.h>
#include <zephyr/sys/crc.h>
#include <zephyr/sys/util.h>
#include <zephyr/ztest.h>

#include <zcbor_decode.h>
#include <zcbor_encode.h>

#include <string.h>

#define BOOT_DEV DEVICE_DT_GET(DT_NODELABEL(boot))
#define EMUL     DEVICE_DT_GET(DT_NODELABEL(emul_uart))

/* Only the `api_corrupt_record` scenario (testcase.yaml) adds this node, and
 * it means "boot with a boot record that is present but unusable".
 */
#define T_CORRUPT_RECORD DT_NODE_EXISTS(DT_NODELABEL(corrupt_record))

#define T_RECORD_OFF ((uint32_t)DT_PROP(DT_NODELABEL(boot), record_offset))
#define T_RECORD_SIZE ((uint32_t)DT_PROP(DT_NODELABEL(boot), record_size))

/* One notch under the driver's default (90). A literal, because SYS_INIT puts
 * the priority into the section name and an expression there ("90 - 1") ends
 * up in the assembler; the BUILD_ASSERT is what keeps the ordering honest if
 * CONFIG_BOOT_AGM_INIT_PRIORITY ever moves.
 */
#define T_PLANT_PRIORITY 89

#if T_CORRUPT_RECORD
BUILD_ASSERT(CONFIG_BOOT_AGM_INIT_PRIORITY > T_PLANT_PRIORITY,
	     "the corrupt-record planter has to run before boot_agm_init()");

/*
 * The driver reads the boot record once, in boot_agm_init() (POST_KERNEL,
 * CONFIG_BOOT_AGM_INIT_PRIORITY), and decides every later "is there a usable
 * record" question from that snapshot -- so the corrupt half of that branch
 * can only be reached by a boot whose flash already holds bad bytes. That is
 * what this scenario is for: one init priority ahead of the driver (and well
 * after the flash driver, whose priority CONFIG_BOOT_AGM_INIT_PRIORITY is
 * documented to sit above), write a recognisable-but-unusable record.
 */
static int plant_corrupt_record(void)
{
	/* "SBC1" (the little-endian magic) with a version nothing imports: a
	 * record that is *there* (not the all-ones blank sector) and unusable.
	 * The api suite plants the same shape at runtime, for the publish path.
	 */
	static const uint8_t bad[16] = {
		0x53U, 0x42U, 0x43U, 0x31U, /* magic */
		0x03U, 0x00U, 0x00U, 0x00U, /* version 3 */
		0x07U, 0x00U, 0x00U, 0x00U, /* a stale sequence number */
		0x00U, 0x00U, 0x00U, 0x00U,
	};
	const struct device *store = DEVICE_DT_GET(DT_NODELABEL(sim_store));

	/* Errors are not fatal here: if the plant does not take, the case that
	 * depends on it fails outright, which is the signal worth having.
	 */
	(void)flash_erase(store, T_RECORD_OFF, T_RECORD_SIZE);
	(void)flash_write(store, T_RECORD_OFF, bad, sizeof(bad));
	return 0;
}

SYS_INIT(plant_corrupt_record, POST_KERNEL, T_PLANT_PRIORITY);
#endif /* T_CORRUPT_RECORD */

/* The stock image group, as drivers/misc/boot_agm_smp.c registers it. */
#define SMP_GROUP_IMAGE 1U
#define IMG_STATE       0U
#define IMG_UPLOAD      1U
#define IMG_ERASE       5U

/* The loader's own group: mcumgr reserves ids >= 64 for user groups. */
#define SMP_GROUP_BOOT_AUTH 0x40U
#define BOOT_AUTH_NONCE     0U
#define BOOT_AUTH_AUTHORIZE 1U

/* mgmt_err codes, as the protocol numbers them. */
#define SMP_ERR_EOK          0
#define SMP_ERR_EINVAL       3
#define SMP_ERR_ENOTSUP      8
#define SMP_ERR_EACCESSDENIED 11

/* The serial transport's framing: the line header, and the newline that ends
 * one (exactly what smp_cli.py / the sample's console send). */
#define SMP_LINE_HDR_PKT     0x06U
#define SMP_LINE_HDR_PKT_2   0x09U
#define SMP_LINE_HDR_FRAG    0x04U
#define SMP_LINE_HDR_FRAG_2  0x14U

#define RSP_BODY_MAX 512U

/* One decoded response: the SMP header fields plus the CBOR body. */
static struct {
	uint8_t op;
	uint16_t group;
	uint8_t seq;
	uint8_t cmd;
	uint8_t body[RSP_BODY_MAX];
	uint32_t body_len;
} rsp;

/* The CBOR body of a command that takes no arguments: an *empty, definite*
 * map, i.e. what cbor2 (and so every real host) puts on the wire. It is not
 * the same as "no body at all": that one the server answers with
 * MGMT_ERR_ECORRUPT, because there is nothing to parse. */
static const uint8_t empty_map[] = { 0xa0U };

/* zcbor's encoder writes indefinite-length maps (`bf ... ff`); the mcumgr
 * handlers decode their body with zcbor_map_decode_bulk(), which finishes by
 * calling zcbor_map_end_decode() and does not accept them: every request
 * whose handler decodes a body comes back MGMT_ERR_EINVAL until the
 * request carries a definite map, the shape cbor2 sends. The bodies
 * here are flat, so turning one into the other is mechanical:
 * swap the header and drop the trailing break byte. */
static uint32_t as_definite_map(uint8_t *body, uint32_t len, uint32_t entries)
{
	zassert_true(len >= 2U && body[0] == 0xbfU && body[len - 1U] == 0xffU,
		     "the encoder produced an indefinite map");
	zassert_true(entries < 24U, "a one-byte map header is enough");
	body[0] = (uint8_t)(0xa0U + entries);
	return len - 1U;
}

/* ---- request framing ------------------------------------------------- */

static void put_be16(uint8_t *dst, uint16_t v)
{
	dst[0] = (uint8_t)(v >> 8);
	dst[1] = (uint8_t)v;
}

static uint16_t get_be16(const uint8_t *src)
{
	return (uint16_t)(((uint16_t)src[0] << 8) | src[1]);
}

/* Longest base64 text one line carries: the transport's 127-byte line minus
 * its 2-byte header and the newline, rounded down to a base64 quantum. A
 * request with a 64-byte signature needs two lines (that is what the
 * continuation header is for), which is exactly what the real hosts send. */
#define SMP_B64_PER_LINE 124U

/* header (8 B) + body -> [len][payload][crc], base64, split into lines behind
 * 0x0609/0x0414 and each ended by '\n': the byte strings
 * mcumgr_serial_process_frag() expects. */
static uint32_t frame_request(uint8_t op, uint16_t group, uint8_t cmd, uint16_t seq,
			      const uint8_t *body, uint32_t body_len,
			      uint8_t *out, uint32_t out_size)
{
	static uint8_t payload[8U + RSP_BODY_MAX];
	static uint8_t framed[2U + sizeof(payload) + 2U];
	uint8_t b64[2U * (sizeof(framed) / 3U + 4U)];
	size_t b64_len = 0U;
	uint32_t payload_len = 8U + body_len;
	uint32_t off = 0U;

	zassert_true(payload_len <= sizeof(payload), "payload fits");
	/* SMP v2: version 1 in the top bits, the op in the low two. */
	payload[0] = (uint8_t)((1U << 3) | (op & 0x03U));
	payload[1] = 0U;
	/* The header's length field counts the *body*; the transport's own
	 * leading field (below) counts the whole packet plus its CRC -- the
	 * two are different numbers and using the wrong one makes the server
	 * answer MGMT_ERR_ECORRUPT. */
	put_be16(&payload[2], (uint16_t)body_len);
	put_be16(&payload[4], group);
	payload[6] = (uint8_t)seq;
	payload[7] = cmd;
	if (body_len != 0U) {
		memcpy(&payload[8], body, body_len);
	}

	put_be16(&framed[0], (uint16_t)(payload_len + 2U));
	memcpy(&framed[2], payload, payload_len);
	put_be16(&framed[2U + payload_len], crc16_itu_t(0U, payload, payload_len));

	zassert_ok(base64_encode(b64, sizeof(b64), &b64_len, framed,
				 2U + payload_len + 2U),
		   "base64_encode");

	for (uint32_t done = 0U; done < b64_len; done += SMP_B64_PER_LINE) {
		uint32_t piece = MIN(SMP_B64_PER_LINE, (uint32_t)b64_len - done);

		zassert_true(off + piece + 3U <= out_size, "the frame has room");
		out[off++] = (done == 0U) ? SMP_LINE_HDR_PKT : SMP_LINE_HDR_FRAG;
		out[off++] = (done == 0U) ? SMP_LINE_HDR_PKT_2 : SMP_LINE_HDR_FRAG_2;
		memcpy(&out[off], &b64[done], piece);
		off += piece;
		out[off++] = '\n';
	}
	return off;
}

/* Decode the base64 text collected so far (the lines' payloads concatenated)
 * into rsp, checking the frame the way the host tool does: length, CRC,
 * header fields. False means "not the whole packet yet". */
static bool decode_reply(const char *b64, uint32_t b64_len, uint16_t want_seq)
{
	static uint8_t frame[2U + 8U + RSP_BODY_MAX + 2U];
	size_t frame_len = 0U;
	uint16_t framed;
	uint16_t payload_len;

	if (base64_decode(frame, sizeof(frame), &frame_len, (const uint8_t *)b64, b64_len) != 0) {
		return false;
	}
	if (frame_len < 10U) {
		return false;
	}
	/* The leading field counts the payload *and* the CRC (see the host
	 * tools' frame() / smp_request()). */
	framed = get_be16(&frame[0]);
	if ((uint32_t)framed + 2U != frame_len) {
		return false;
	}
	payload_len = framed - 2U;
	if (crc16_itu_t(0U, &frame[2], payload_len) != get_be16(&frame[2U + payload_len])) {
		return false;
	}

	rsp.op = frame[2];
	rsp.group = get_be16(&frame[4]);
	rsp.seq = frame[8];
	rsp.cmd = frame[9];
	rsp.body_len = payload_len - 8U;
	if (rsp.body_len > sizeof(rsp.body)) {
		return false;
	}
	memcpy(rsp.body, &frame[10], rsp.body_len);
	return rsp.seq == (uint8_t)want_seq;
}

/* ---- one request, one response --------------------------------------- */

/* The server answers from the mcumgr work queue, so the reply is not there
 * when agm_boot_smp_rx() returns: poll the emulated console's TX fifo. The
 * driver also prints on that same UART (the gate's refusal, the record
 * messages), exactly like the dev board -- so the reply is what follows the
 * transport's own line header, and it may span several lines (image state's
 * three-entry list does). */
static bool smp_exchange(uint8_t op, uint16_t group, uint8_t cmd, uint16_t seq,
			 const uint8_t *body, uint32_t body_len)
{
	static uint8_t out[512];
	/* Big enough for the whole TX fifo: `image state` answers a three-entry
	 * list, which is a few hundred bytes once base64'd. */
	static uint8_t raw[1024];
	static char b64[1024];
	uint32_t b64_len = 0U;
	uint32_t len;
	bool started = false;
	bool ok = false;

	len = frame_request(op, group, cmd, seq, body, body_len, out, sizeof(out));
	uart_emul_flush_tx_data(EMUL);
	/* Feed it one line at a time, exactly like the sample's console does
	 * (its reader hands each line to the server on its own). */
	for (uint32_t sent = 0U; sent < len;) {
		uint32_t end = sent;

		while (end < len && out[end] != '\n') {
			end++;
		}
		end++; /* include the newline */
		agm_boot_smp_rx(BOOT_DEV, &out[sent], end - sent);
		sent = end;
	}

	for (uint32_t waited = 0U; waited < 1000U && !ok; waited += 2U) {
		uint32_t raw_len;

		k_sleep(K_MSEC(2));
		raw_len = uart_emul_get_tx_data(EMUL, raw, sizeof(raw));

		/* Walk what the server wrote so far: console text is skipped,
		 * a 0x0609 line starts the packet and 0x0414 lines continue it
		 * (tools/smp_cli.py's decode_packet() does the same). */
		for (uint32_t i = 0U; i + 1U < raw_len; i++) {
			uint32_t end;

			if (raw[i] != SMP_LINE_HDR_PKT && raw[i] != SMP_LINE_HDR_FRAG) {
				continue;
			}
			if (raw[i + 1U] != SMP_LINE_HDR_PKT_2 && raw[i + 1U] != SMP_LINE_HDR_FRAG_2) {
				continue;
			}
			for (end = i + 2U; end < raw_len && raw[end] != '\n'; end++) {
			}
			if (end == raw_len) {
				break; /* the line is not complete yet */
			}
			if (raw[i] == SMP_LINE_HDR_PKT) {
				b64_len = 0U;
				started = true;
			}
			if (started) {
				zassert_true(b64_len + (end - i - 2U) < sizeof(b64),
					     "the reply fits the test buffer");
				memcpy(&b64[b64_len], &raw[i + 2U], end - i - 2U);
				b64_len += end - i - 2U;
				ok = decode_reply(b64, b64_len, seq);
			}
			i = end;
			if (ok) {
				break;
			}
		}
		if (!ok && !started && raw_len == sizeof(raw)) {
			/* Console chatter filled the fifo before the reply
			 * arrived: drop it so the next poll starts clean. */
			uart_emul_flush_tx_data(EMUL);
		}
		if (!ok && b64_len != 0U && raw_len == 0U && waited > 100U) {
			/* The packet never completes (a server bug): fail with the
			 * buffer as it stands instead of waiting the full second. */
			break;
		}
	}
	return ok;
}

/* ---- response body readers ------------------------------------------- *
 *
 * Every body is a map the framework opened; the readers below walk it with
 * zcbor_any_skip() for the fields they do not care about, so a new field in a
 * response does not break them.
 */
/* `zcbor_decoder_t` is a function *type* in this zcbor, so the readers below
 * take the pointer form and the callers cast their decoder to it. */
#define AS_DECODER(fn) ((bool (*)(zcbor_state_t *, void *))(fn))

static bool body_get(const char *key, size_t key_len,
		     bool (*decode)(zcbor_state_t *, void *), void *out)
{
	/* Four backups: the walk descends into the "images" list and its entry
	 * maps and still needs one to skip the entry's trailing fields. */
	ZCBOR_STATE_D(zsd, 4, rsp.body, rsp.body_len, 1, 0);

	if (rsp.body_len == 0U || !zcbor_map_start_decode(zsd)) {
		return false;
	}
	while (!zcbor_array_at_end(zsd)) {
		struct zcbor_string k = { 0 };

		if (!zcbor_tstr_decode(zsd, &k)) {
			return false;
		}
		if (k.len == key_len && memcmp(k.value, key, key_len) == 0) {
			return decode(zsd, out);
		}
		if (!zcbor_any_skip(zsd, NULL)) {
			return false;
		}
	}
	return false;
}

/* "rc", or 0 when the map has none (the mcumgr convention: an empty body is a
 * success -- what smp_cli.py's parse_smp_reply() also assumes). */
static int32_t rsp_rc(void)
{
	int32_t rc = 0;

	(void)body_get("rc", 2U, AS_DECODER(zcbor_int32_decode), &rc);
	return rc;
}

/* What a client reads out of `image state`: how many images are listed, and
 * the index of the first one. */
struct image_list {
	uint32_t count;
	uint32_t first_image;
};

/* The "images" list (Zephyr's own img_mgmt_state_read() answers the same
 * shape): a list of maps whose first three fields are fixed, so the first
 * entry is read strictly and the rest only counted. */
static bool decode_image_list(zcbor_state_t *zs, void *out)
{
	struct image_list *list = out;

	list->count = 0U;
	list->first_image = UINT32_MAX;
	if (!zcbor_list_start_decode(zs)) {
		return false;
	}
	while (!zcbor_array_at_end(zs)) {
		if (list->count == 0U) {
			/* The first entry, read strictly: a map whose first
			 * three fields are the ones the protocol fixes. */
			struct zcbor_string version = { 0 };
			uint32_t image = 0U;
			uint32_t slot = 0U;

			if (!zcbor_map_start_decode(zs) ||
			    !zcbor_tstr_expect_ptr(zs, "image", 5U) ||
			    !zcbor_uint32_decode(zs, &image) ||
			    !zcbor_tstr_expect_ptr(zs, "slot", 4U) ||
			    !zcbor_uint32_decode(zs, &slot) ||
			    !zcbor_tstr_expect_ptr(zs, "version", 7U) ||
			    !zcbor_tstr_decode(zs, &version)) {
				return false;
			}
			list->first_image = image;
			/* Whatever else the entry carries (bootable, active,
			 * confirmed, permanent) is not this test's business. */
			while (!zcbor_array_at_end(zs)) {
				if (!zcbor_any_skip(zs, NULL) || !zcbor_any_skip(zs, NULL)) {
					return false;
				}
			}
			if (!zcbor_map_end_decode(zs)) {
				return false;
			}
		} else if (!zcbor_any_skip(zs, NULL)) {
			return false;
		}
		list->count++;
	}
	if (!zcbor_list_end_decode(zs)) {
		return false;
	}
	return true;
}

/* ---- the locked profile's signer (only compiled when it has one) ------ *
 *
 * Same key material as the api suite's locked scenario: the private half is
 * the 32 bytes 0x01..0x20, the public half is this binary's `agm_boot_pubkey`.
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

/* Sign SHA-256(nonce || cmd || args_len || args), the payload
 * agm_boot_authorize() documents, in the raw r||s form tinycrypt takes. */
static void sign_command(const uint8_t *nonce, uint8_t cmd, uint8_t *sig)
{
	struct tc_sha256_state_struct sha;
	uint8_t payload[AGM_BOOT_NONCE_LEN + 3U];
	uint8_t hash[32];

	memcpy(payload, nonce, AGM_BOOT_NONCE_LEN);
	payload[AGM_BOOT_NONCE_LEN] = cmd;
	payload[AGM_BOOT_NONCE_LEN + 1U] = 0U; /* args_len, little endian */
	payload[AGM_BOOT_NONCE_LEN + 2U] = 0U;

	tc_sha256_init(&sha);
	tc_sha256_update(&sha, payload, sizeof(payload));
	tc_sha256_final(hash, &sha);

	zassert_equal(uECC_sign(test_priv, hash, sizeof(hash), sig, uECC_secp256r1()), 1,
		      "uECC_sign()");
}
#endif /* CONFIG_BOOT_AGM_LOCK_PRODUCTION */

/* ---- cases ----------------------------------------------------------- */

/* One upload request: `off` bytes into image 0 (store A), out of `total`.
 * The target and the total ride along on the first chunk, which is what
 * smp_boot_img_upload() reassembles; the reply carries the offset back. */
static bool smp_upload_chunk(uint16_t seq, uint32_t off, uint32_t count, uint32_t total)
{
	static const uint8_t image[16] = {
		0xaa, 0xbb, 0xcc, 0xdd, 0x11, 0x22, 0x33, 0x44,
		0x55, 0x66, 0x77, 0x88, 0x99, 0x00, 0xff, 0xee,
	};
	static uint8_t body[64];
	uint32_t got_off = UINT32_MAX;
	uint32_t len;

	zassert_true(off + count <= sizeof(image), "the chunk fits the test image");
	ZCBOR_STATE_E(z, 1, body, sizeof(body), 1);

	zassert_true(zcbor_map_start_encode(z, 4) &&
		     zcbor_tstr_put_lit(z, "image") && zcbor_uint32_put(z, 0U) &&
		     zcbor_tstr_put_lit(z, "len") && zcbor_uint32_put(z, total) &&
		     zcbor_tstr_put_lit(z, "off") && zcbor_uint32_put(z, off) &&
		     zcbor_tstr_put_lit(z, "data") &&
		     zcbor_bstr_encode_ptr(z, (const char *)&image[off], count) &&
		     zcbor_map_end_encode(z, 4),
		     "the upload request encodes");
	/* image, len, off, data */
	len = as_definite_map(body, (uint32_t)(z->payload - body), 4U);

	if (!smp_exchange(2U, SMP_GROUP_IMAGE, IMG_UPLOAD, seq, body, len)) {
		return false;
	}
	if (rsp_rc() != SMP_ERR_EOK) {
		return false;
	}
	return body_get("off", 3U, AS_DECODER(zcbor_uint32_decode), &got_off)
	       && got_off == off + count;
}

/* The whole 16 bytes in one request: begin + write + publish in a single
 * round trip, the shape the unlocked profile accepts as it is. */
static bool smp_upload_16(uint16_t seq)
{
	return smp_upload_chunk(seq, 0U, 16U, 16U);
}

/* One authorize round trip: ask for a nonce, sign the command, send it, and
 * assert the device accepted it. Only the locked profile has a key (and a
 * device that can verify it), so this is compiled with its signer. */
#if IS_ENABLED(CONFIG_BOOT_AGM_LOCK_PRODUCTION)
static void smp_authorize(uint8_t cmd, uint16_t seq_nonce, uint16_t seq_auth)
{
	uint8_t nonce[AGM_BOOT_NONCE_LEN] = { 0 };
	struct zcbor_string got = { 0 };
	static uint8_t body[128];
	uint8_t sig[64];

	ZCBOR_STATE_E(zse, 1, body, sizeof(body), 1);

	zassert_true(smp_exchange(0U, SMP_GROUP_BOOT_AUTH, BOOT_AUTH_NONCE, seq_nonce, empty_map,
				  sizeof(empty_map)),
		     "the nonce came back");
	zassert_true(body_get("nonce", 5U, AS_DECODER(zcbor_bstr_decode), &got), "\"nonce\" key");
	memcpy(nonce, got.value, sizeof(nonce));
	sign_command(nonce, cmd, sig);

	zassert_true(zcbor_map_start_encode(zse, 4) &&
		     zcbor_tstr_put_lit(zse, "nonce") &&
		     zcbor_bstr_encode_ptr(zse, (const char *)nonce, sizeof(nonce)) &&
		     zcbor_tstr_put_lit(zse, "cmd") && zcbor_uint32_put(zse, cmd) &&
		     zcbor_tstr_put_lit(zse, "sig") &&
		     zcbor_bstr_encode_ptr(zse, (const char *)sig, sizeof(sig)) &&
		     zcbor_map_end_encode(zse, 4),
		     "the authorize body encodes");
	zassert_true(smp_exchange(2U, SMP_GROUP_BOOT_AUTH, BOOT_AUTH_AUTHORIZE, seq_auth, body,
				  as_definite_map(body, (uint32_t)(zse->payload - body), 3U)),
		     "the authorize answered");
	zassert_equal(rsp_rc(), SMP_ERR_EOK, "the signed command is accepted");
}
#endif /* CONFIG_BOOT_AGM_LOCK_PRODUCTION */

ZTEST(boot_agm_smp, test_image_state_lists_the_three_targets)
{
	struct image_list list = { 0 };

	zassert_true(smp_exchange(0U /* read */, SMP_GROUP_IMAGE, IMG_STATE, 1U, empty_map,
				  sizeof(empty_map)),
		     "image state answered");
	zassert_equal(rsp_rc(), SMP_ERR_EOK, "rc");
	zassert_true(body_get("images", 6U, decode_image_list, &list), "\"images\" list");
	zassert_equal(list.count, 3U, "the two stores and the on-die slot");
	zassert_equal(list.first_image, 0U, "and the first entry is image 0");
}

ZTEST(boot_agm_smp, test_nonce_is_a_sixteen_byte_bstring)
{
	struct zcbor_string nonce = { 0 };

	zassert_true(smp_exchange(0U, SMP_GROUP_BOOT_AUTH, BOOT_AUTH_NONCE, 2U, empty_map,
				  sizeof(empty_map)),
		     "the nonce came back");
	zassert_equal(rsp_rc(), SMP_ERR_EOK, "rc");
	zassert_true(body_get("nonce", 5U,
			      AS_DECODER(zcbor_bstr_decode), &nonce),
		     "\"nonce\" key");
	zassert_equal(nonce.len, AGM_BOOT_NONCE_LEN, "16 bytes");
}

/* A body the handler cannot use: "cmd" is missing, so the request never gets
 * as far as the signature (which also means this case runs in both
 * scenarios). */
ZTEST(boot_agm_smp, test_malformed_authorize_is_einval)
{
	static uint8_t body[64];
	static const uint8_t nothing[1] = { 0 };

	ZCBOR_STATE_E(zse, 1, body, sizeof(body), 1);

	zassert_true(zcbor_map_start_encode(zse, 1) &&
		     zcbor_tstr_put_lit(zse, "nonce") &&
		     zcbor_bstr_encode_ptr(zse, (const char *)nothing, 0U) &&
		     zcbor_map_end_encode(zse, 1));

	zassert_true(smp_exchange(2U /* write */, SMP_GROUP_BOOT_AUTH, BOOT_AUTH_AUTHORIZE,
				   3U, body, as_definite_map(body, (uint32_t)(zse->payload - body), 1U)),
		     "the server answered");
	zassert_equal(rsp_rc(), SMP_ERR_EINVAL, "a body without \"cmd\" is invalid");
}

ZTEST(boot_agm_smp, test_unknown_command_is_notsup)
{
	zassert_true(smp_exchange(0U, SMP_GROUP_IMAGE, 63U, 4U, empty_map, sizeof(empty_map)),
		     "answered");
	zassert_equal(rsp_rc(), SMP_ERR_ENOTSUP, "no handler, no error of its own");
}

/* `image erase` before anything was ever published, in both flavours of "the
 * record is not usable": the two answers have to stay different, because they
 * mean different things to an operator -- the driver half and this
 * mapping are two different cases:
 *
 *   blank (erased sector)   -> EOK: there is no image to drop, and the call
 *                              writes nothing -- an erase on a freshly flashed
 *                              board is a no-op, not a failure;
 *   corrupt (bad magic)     -> EINVAL(3): "this request cannot be applied",
 *                              not EUNKNOWN(1), which reads like a device
 *                              fault.
 *
 * Which one this binary boots with is the scenario's business: a corrupt
 * record has to exist before boot_agm_init() reads it, so only
 * `api_corrupt_record` (testcase.yaml + corrupt_record.overlay) plants one.
 * This case has to run before test_zz_gate_flow, which publishes a record and
 * from then on the record is valid (the erase there therefore answers EOK).
 */
ZTEST(boot_agm_smp, test_zz0_erase_of_an_unusable_record)
{
	struct agm_boot_info info;
	static uint8_t body[64];

	zassert_ok(agm_boot_info_get(BOOT_DEV, &info));
	zassert_false(info.record_valid, "this case needs the post-init state");
	zassert_equal(info.record_blank, !T_CORRUPT_RECORD,
		      "blank in every other scenario, corrupt in api_corrupt_record");

#if IS_ENABLED(CONFIG_BOOT_AGM_LOCK_PRODUCTION)
	/* The gate answers before the record does, so ask first: without this
	 * the answer would be EACCESSDENIED and say nothing about the mapping.
	 */
	smp_authorize(AGM_BOOT_AUTH_CMD_ERASE, 22U, 23U);
#endif

	{
		ZCBOR_STATE_E(zse, 1, body, sizeof(body), 1);

		zassert_true(zcbor_map_start_encode(zse, 1) &&
			     zcbor_tstr_put_lit(zse, "slot") &&
			     zcbor_uint32_put(zse, 0U) && zcbor_map_end_encode(zse, 1));
		zassert_true(smp_exchange(2U, SMP_GROUP_IMAGE, IMG_ERASE, 22U, body,
					  as_definite_map(body, (uint32_t)(zse->payload - body), 1U)));
	}

#if T_CORRUPT_RECORD
	zassert_equal(rsp_rc(), SMP_ERR_EINVAL,
		      "a corrupt record is EINVAL, not EUNKNOWN");
#else
	zassert_equal(rsp_rc(), SMP_ERR_EOK,
		      "a blank record is a no-op, not an error");
#endif

	zassert_ok(agm_boot_info_get(BOOT_DEV, &info));
	zassert_equal(info.slot[0].state, AGM_BOOT_SLOT_EMPTY, "no image either way");
	zassert_equal(info.slot[0].len, 0U, "and nothing was written");
}

/* The gate, end to end, in whichever profile this binary was built for. */
ZTEST(boot_agm_smp, test_zz_gate_flow)
{
	struct agm_boot_info info;
	static uint8_t body[512];

	zassert_ok(agm_boot_info_get(BOOT_DEV, &info));
	zassert_equal(info.slot[0].state, AGM_BOOT_SLOT_EMPTY, "this case starts clean");

#if IS_ENABLED(CONFIG_BOOT_AGM_LOCK_PRODUCTION)
	/* 1. An unsigned erase is refused, and changes nothing. */
	{

		ZCBOR_STATE_E(zse, 1, body, sizeof(body), 1);

		zassert_true(zcbor_map_start_encode(zse, 1) &&
			     zcbor_tstr_put_lit(zse, "slot") &&
			     zcbor_uint32_put(zse, 0U) && zcbor_map_end_encode(zse, 1));
		zassert_true(smp_exchange(2U, SMP_GROUP_IMAGE, IMG_ERASE, 5U, body,
					  as_definite_map(body,
							  (uint32_t)(zse->payload - body), 1U)));
	}
	zassert_equal(rsp_rc(), SMP_ERR_EACCESSDENIED, "the unsigned erase is denied");
	zassert_ok(agm_boot_info_get(BOOT_DEV, &info));
	zassert_equal(info.slot[0].state, AGM_BOOT_SLOT_EMPTY, "and nothing changed");

	/* 2. A signature over something else is refused too. */
	{
		uint8_t nonce[AGM_BOOT_NONCE_LEN] = { 0 };
		struct zcbor_string got = { 0 };
		uint8_t sig[64];

		ZCBOR_STATE_E(zse, 1, body, sizeof(body), 1);

		zassert_true(smp_exchange(0U, SMP_GROUP_BOOT_AUTH, BOOT_AUTH_NONCE, 6U, empty_map,
					  sizeof(empty_map)));
		zassert_true(body_get("nonce", 5U, AS_DECODER(zcbor_bstr_decode), &got));
		memcpy(nonce, got.value, sizeof(nonce));

		sign_command(nonce, AGM_BOOT_AUTH_CMD_ERASE, sig);
		sig[0] ^= 0x01U; /* right shape, wrong signature */
		zassert_true(zcbor_map_start_encode(zse, 4) &&
			     zcbor_tstr_put_lit(zse, "nonce") &&
			     zcbor_bstr_encode_ptr(zse, (const char *)nonce, sizeof(nonce)) &&
			     zcbor_tstr_put_lit(zse, "cmd") &&
			     zcbor_uint32_put(zse, AGM_BOOT_AUTH_CMD_ERASE) &&
			     zcbor_tstr_put_lit(zse, "sig") &&
			     zcbor_bstr_encode_ptr(zse, (const char *)sig, sizeof(sig)) &&
			     zcbor_map_end_encode(zse, 4));
		zassert_true(smp_exchange(2U, SMP_GROUP_BOOT_AUTH, BOOT_AUTH_AUTHORIZE, 7U,
					  body, as_definite_map(body, (uint32_t)(zse->payload - body), 3U)));
		zassert_equal(rsp_rc(), SMP_ERR_EACCESSDENIED, "a bad signature is denied");
	}

	/* 3. An upload needs *two* grants in this profile :
	 * ERASE to open the session, whose first write is what erases the
	 * target, and PUBLISH for the chunk that finishes the image. */
	zassert_false(smp_upload_16(8U), "an unsigned upload cannot open a session");
	zassert_equal(rsp_rc(), SMP_ERR_EACCESSDENIED, "and the refusal says \"denied\"");
	zassert_ok(agm_boot_info_get(BOOT_DEV, &info));
	zassert_equal(info.slot[0].state, AGM_BOOT_SLOT_EMPTY, "nothing was published");

	smp_authorize(AGM_BOOT_AUTH_CMD_PUBLISH, 9U, 10U);
	zassert_false(smp_upload_16(11U),
		      "a publish grant does not open the session -- that wants ERASE");
	zassert_equal(rsp_rc(), SMP_ERR_EACCESSDENIED, "same refusal");

	smp_authorize(AGM_BOOT_AUTH_CMD_ERASE, 12U, 13U);
	zassert_true(smp_upload_chunk(14U, 0U, 8U, 16U),
		     "the session opens on the erase grant (half the image)");

	smp_authorize(AGM_BOOT_AUTH_CMD_PUBLISH, 15U, 16U);
	zassert_true(smp_upload_chunk(17U, 8U, 8U, 16U),
		     "the publish grant finishes and publishes it");

	zassert_ok(agm_boot_info_get(BOOT_DEV, &info));
	zassert_equal(info.slot[0].len, 16U, "store A holds the uploaded image");
	zassert_equal(info.slot[0].state, AGM_BOOT_SLOT_TRIAL, "published as TRIAL");

	/* 4. That grant is spent: an erase needs its own. */
	{
		ZCBOR_STATE_E(zse, 1, body, sizeof(body), 1);

		zassert_true(zcbor_map_start_encode(zse, 1) &&
			     zcbor_tstr_put_lit(zse, "slot") &&
			     zcbor_uint32_put(zse, 0U) && zcbor_map_end_encode(zse, 1));
		zassert_true(smp_exchange(2U, SMP_GROUP_IMAGE, IMG_ERASE, 18U, body,
					  as_definite_map(body,
							  (uint32_t)(zse->payload - body), 1U)));
	}
	zassert_equal(rsp_rc(), SMP_ERR_EACCESSDENIED, "the publish grant did not carry over");

	/* 5. Authorize the erase, then do it. */
	smp_authorize(AGM_BOOT_AUTH_CMD_ERASE, 19U, 20U);
	{
		ZCBOR_STATE_E(zse, 1, body, sizeof(body), 1);

		zassert_true(zcbor_map_start_encode(zse, 1) &&
			     zcbor_tstr_put_lit(zse, "slot") &&
			     zcbor_uint32_put(zse, 0U) && zcbor_map_end_encode(zse, 1));
		zassert_true(smp_exchange(2U, SMP_GROUP_IMAGE, IMG_ERASE, 21U, body,
					  as_definite_map(body,
							  (uint32_t)(zse->payload - body), 1U)));
	}
	zassert_equal(rsp_rc(), SMP_ERR_EOK, "the authorized erase runs");
	zassert_ok(agm_boot_info_get(BOOT_DEV, &info));
	zassert_equal(info.slot[0].state, AGM_BOOT_SLOT_EMPTY, "and it really erased");
#else
	/* No locked profile: the same commands need no authorization, and the
	 * authorize command has no verifier to reach -- what the wire says is
	 * ENOTSUP, not a denial. */
	{
		/* Well-formed enough to reach the driver (a 16-byte nonce, a
		 * signature-shaped blob) but signed by nobody: the answer is
		 * still "this build has nothing to verify with". */
		static const uint8_t nonce[AGM_BOOT_NONCE_LEN] = { 0 };
		static const uint8_t dummy[64] = { 0 };

		ZCBOR_STATE_E(zse, 1, body, sizeof(body), 1);

		zassert_true(zcbor_map_start_encode(zse, 4) &&
			     zcbor_tstr_put_lit(zse, "nonce") &&
			     zcbor_bstr_encode_ptr(zse, (const char *)nonce, sizeof(nonce)) &&
			     zcbor_tstr_put_lit(zse, "cmd") &&
			     zcbor_uint32_put(zse, AGM_BOOT_AUTH_CMD_ERASE) &&
			     zcbor_tstr_put_lit(zse, "sig") &&
			     zcbor_bstr_encode_ptr(zse, (const char *)dummy, sizeof(dummy)) &&
			     zcbor_map_end_encode(zse, 4));
		zassert_true(smp_exchange(2U, SMP_GROUP_BOOT_AUTH, BOOT_AUTH_AUTHORIZE, 5U,
					  body, as_definite_map(body, (uint32_t)(zse->payload - body), 3U)));
		zassert_equal(rsp_rc(), SMP_ERR_ENOTSUP,
			      "no P-256 verifier in this build, so nothing can be authorized");
	}

	/* An upload publishes without any authorization here, and the erase
	 * that follows runs for the same reason. */
	zassert_true(smp_upload_16(5U), "an unlocked build publishes without a grant");
	zassert_ok(agm_boot_info_get(BOOT_DEV, &info));
	zassert_equal(info.slot[0].len, 16U, "store A holds the uploaded image");

	{

		ZCBOR_STATE_E(zse, 1, body, sizeof(body), 1);

		zassert_true(zcbor_map_start_encode(zse, 1) &&
			     zcbor_tstr_put_lit(zse, "slot") &&
			     zcbor_uint32_put(zse, 0U) && zcbor_map_end_encode(zse, 1));
		zassert_true(smp_exchange(2U, SMP_GROUP_IMAGE, IMG_ERASE, 6U, body,
					  as_definite_map(body,
							  (uint32_t)(zse->payload - body), 1U)));
	}
	zassert_equal(rsp_rc(), SMP_ERR_EOK, "an unlocked build erases without a grant");
	zassert_ok(agm_boot_info_get(BOOT_DEV, &info));
	zassert_equal(info.slot[0].state, AGM_BOOT_SLOT_EMPTY, "and it really erased");
#endif
}

ZTEST_SUITE(boot_agm_smp, NULL, NULL, NULL, NULL, NULL);
