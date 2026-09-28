/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Copyright (c) 2026 AgRV Contributors
 *
 * AgRV2K fabric (bitstream) slots and the record that says which one to load.
 *
 * Why this exists: the region the FCB configured the running fabric from must
 * never be rewritten while the CPU runs. The CPU is clocked by that fabric
 * (CLKOUT[0] -> sys_clk), so touching its source region freezes the core and
 * takes the console and SWD with it.: an in-place update
 * stopped 15.9 KB into the write, the whole board dropped off USB, and the
 * next boot streamed the *partial* image into the fabric -- misconfigured
 * pins, high current, heat. (The update had worked once before, which is
 * exactly what made it look safe.)
 *
 * So updates go into the slot that is *not* live, and a record decides what
 * the next boot streams:
 *
 *   factory | record (one 4 KiB sector, two copies) | slot 2 | slot 1
 *
 * packed downwards from the factory slot -- what the flash option byte points
 * at, written only by an external tool, never by this device. On agrv2k_407
 * (factory 0x800e7000, slot 100 KiB) that is 0x800e6000 / 0x800cd000 /
 * 0x800b4000; the two inputs are CONFIG_AGM_BITSTREAM_FACTORY_ADDR and
 * CONFIG_AGM_BITSTREAM_SLOT_SIZE, so the numbers follow the configuration
 * instead of being pinned here.
 *
 * The order is what makes it safe: the image is written and verified in the
 * inactive slot *first*, and only then does the record point at it. A reset
 * (or a freeze) anywhere before that record write leaves the board on the
 * image it is already running; when the record is missing or torn, the
 * factory slot is what gets streamed.
 *
 * The addresses are fixed by the SoC rather than by the devicetree layout,
 * because the record has to be findable before any driver runs: the FCB
 * bring-up happens in soc.c, in every image, including one without the
 * bootloader driver (drivers/misc/boot_agm.c). The driver asserts that its own (devicetree-derived)
 * regions do not collide with these.
 */

#ifndef ZEPHYR_SOC_AGM_AGRV2K_AGM_BITSTREAM_H_
#define ZEPHYR_SOC_AGM_AGRV2K_AGM_BITSTREAM_H_

#include <stdbool.h>
#include <stdint.h>

/* The fabric image the FCB streams: 99944 bytes, the vendor SDK's
 * FCB_AUTO_WORDS = 24986 words. The slot size below has to be at least one
 * image rounded up to the erase sector (100 KiB today), and that is asserted
 * at the bottom of this header. */
#define AGM_BITSTREAM_IMAGE_LEN   99944U

/*
 * Where the *config* the FCB streams starts inside that region -- i.e. what
 * the flash option byte points at.
 *
 * For a raw bitstream that is the base itself. For a compressed one the vendor
 * tool writes its decompression algorithm first (the RISC-V code the ROM runs
 * at power-up) and the config after it, and points the option byte at the
 * config:  with the SDK's write_fpga_config, "wrote fpga
 * decompression algorithm at 0x800e7000 / wrote fpga configuration at
 * 0x800e8100", option byte "0x800e8100 (compressed)". The offset is the size
 * of *that* algorithm, which is why it is a Kconfig value and not a constant
 * here.
 */
#if defined(CONFIG_AGM_FCB_BITSTREAM_COMPRESSED)
#define AGM_BITSTREAM_CONFIG_ADDR \
	(AGM_BITSTREAM_FACTORY_ADDR + CONFIG_AGM_FCB_BITSTREAM_ALGO_SIZE)
#else
#define AGM_BITSTREAM_CONFIG_ADDR AGM_BITSTREAM_FACTORY_ADDR
#endif

/*
 * The fabric area is *derived*, not spelled out. It used to be four literals,
 * which made the 16 KiB between the slots look like a design decision instead
 * of the leftover of two hand-picked numbers.
 *
 * The two inputs are configuration because this header is compiled into
 * *every* image -- including ones with no boot node, whose fcb.c still has to
 * find the record on its own:
 *
 *   CONFIG_AGM_BITSTREAM_FACTORY_ADDR  what the FLASH option byte points at,
 *                                      where an external tool writes and what
 *                                      the ROM starts from (0x800e7000 on
 *                                      agrv2k_407's 1 MiB, 0x80027000 on the
 *                                      256 KiB parts);
 *   CONFIG_AGM_BITSTREAM_SLOT_SIZE     one fabric image rounded up to the
 *                                      erase sector (99944 B -> 100 KiB).
 *
 * Below the factory region the area is *packed*, top down:
 *
 *     factory | record (1 sector) | slot 2 | slot 1 | ... the A/B layout
 *
 * so the record and both slots follow the anchor and the sizes instead of
 * being three more numbers to keep in step. Where this area ends is where the
 * app-side chain's BUILD_ASSERTs take over (drivers/misc/boot_agm.c).
 */
#define AGM_BITSTREAM_FACTORY_ADDR \
	((uint32_t)CONFIG_AGM_BITSTREAM_FACTORY_ADDR)
#define AGM_BITSTREAM_SLOT_SIZE \
	((uint32_t)CONFIG_AGM_BITSTREAM_SLOT_SIZE)
/* The on-die erase sector (4 KiB, the SoC's FLASH_SECTOR_SIZE): the record
 * takes exactly one, and it is what the slot size is rounded up to. */
#define AGM_BITSTREAM_RECORD_SECTOR 0x1000U

#define AGM_BITSTREAM_RECORD_ADDR \
	(AGM_BITSTREAM_FACTORY_ADDR - AGM_BITSTREAM_RECORD_SECTOR)
#define AGM_BITSTREAM_SLOT2_ADDR \
	(AGM_BITSTREAM_RECORD_ADDR - AGM_BITSTREAM_SLOT_SIZE)
#define AGM_BITSTREAM_SLOT1_ADDR \
	(AGM_BITSTREAM_SLOT2_ADDR - AGM_BITSTREAM_SLOT_SIZE)

/*
 * Signed slots (CONFIG_BOOT_AGM_BITSTREAM_SIGNED).
 *
 * With that option the two update slots hold MCUboot containers -- the same
 * format the application images use -- around the same 99944-byte fabric, so
 * the fabric is authenticated exactly like an application image is:
 * `tools/sign_image.py --bitstream` produces the container, the boot driver
 * verifies it before the record is committed *and* again before the boot path
 * streams it (soc/agm/agrv2k/fcb.c), and a slot that does not verify is
 * refused -- the board stays on the factory fabric instead.
 *
 * The factory region is *not* covered by this: the ROM streams whatever the
 * option byte points at, and only an external tool ever writes that region.
 *
 * The profile is ECDSA P-256 rather than whatever the application uses,
 * because the check has to run in the boot path: `agrv2k_fcb_program()` runs
 * at PRE_KERNEL_1, before any driver and before Zephyr's heap exists
 * (`malloc_prepare()` is a POST_KERNEL init), so PSA's RSA verification --
 * which allocates -- cannot be used there. tinycrypt's P-256 verify is
 * allocation-free: a `malloc` from PRE_KERNEL_1 traps with
 * mcause=5/mtval=8 and the board never reaches the console.
 */
#if defined(CONFIG_BOOT_AGM_BITSTREAM_SIGNED)

/* The MCUboot header the fabric payload sits behind. */
#define AGM_BITSTREAM_HDR_SIZE 32U

/*
 * Where a fabric container says it runs (`ih_load_addr`). The two update
 * slots are interchangeable -- the same fabric is written to whichever one is
 * not live -- so there is exactly one stamp, taken from slot 1, instead of
 * one per slot. The boot driver refuses a container whose header does not say
 * this, which is what keeps an *application* container (or one built for
 * another board) from being uploaded as a fabric image.
 */
#define AGM_BITSTREAM_CONTAINER_ADDR \
	(AGM_BITSTREAM_SLOT1_ADDR + AGM_BITSTREAM_HDR_SIZE)

#define AGM_BITSTREAM_SIGNED 1

#else /* CONFIG_BOOT_AGM_BITSTREAM_SIGNED */

#define AGM_BITSTREAM_HDR_SIZE 0U
#define AGM_BITSTREAM_SIGNED   0

#endif /* CONFIG_BOOT_AGM_BITSTREAM_SIGNED */

/* Two copies live in the record sector, 32 bytes apart. A writer always
 * writes copy 0 first and copy 1 second; a reader takes the valid copy with
 * the higher sequence, so a torn write costs at most the newer copy. */
#define AGM_BITSTREAM_RECORD_MAGIC 0x31525342U /* "BSR1" */
#define AGM_BITSTREAM_RECORD_COPIES 2U
#define AGM_BITSTREAM_RECORD_STRIDE 32U

struct agm_bitstream_record {
	uint32_t magic;   /* AGM_BITSTREAM_RECORD_MAGIC */
	uint32_t seq;     /* generation, strictly increasing */
	uint32_t slot;    /* absolute address of the slot to stream */
	uint32_t len;     /* image length in bytes */
	uint32_t crc;     /* CRC-32 (IEEE, zlib) over the image */
	uint32_t rec_crc; /* CRC-32 over the five words above */
	uint32_t pin[2];  /* keep the copy 32 bytes and word aligned */
};

/* Number of leading words `rec_crc` covers. */
#define AGM_BITSTREAM_RECORD_CRC_WORDS 5U

/**
 * @brief CRC-32 (IEEE, the same polynomial zlib and Zephyr's crc32_ieee use)
 *        over the record's leading words, byte by byte in flash order.
 *
 * Hand-rolled and table-free on purpose: this header is read by the FCB
 * bring-up in soc.c, which runs before any driver and in images that may not
 * have CONFIG_CRC at all. Five words is 160 bit-steps, i.e. nothing at boot.
 *
 * Without it a torn record write could look valid if the fields happened to
 * land on a plausible magic/slot/len/seq -- the pin words were never checked,
 * so 16 of the 32 bytes were free.
 */
static inline uint32_t agm_bitstream_record_crc(const volatile uint32_t *words)
{
	uint32_t crc = 0xffffffffU;

	for (uint32_t w = 0U; w < AGM_BITSTREAM_RECORD_CRC_WORDS; w++) {
		uint32_t word = words[w];

		/* Byte by byte, little endian: the CRC has to be over the bytes
		 * as they sit in flash, not over the host's word order. */
		for (uint32_t byte = 0U; byte < 4U; byte++) {
			crc ^= (word >> (8U * byte)) & 0xffU;
			for (uint32_t bit = 0U; bit < 8U; bit++) {
				crc = (crc >> 1) ^ (0xedb88320U & (0U - (crc & 1U)));
			}
		}
	}
	return crc ^ 0xffffffffU;
}

/**
 * @brief What the next boot should stream, and where that decision came from.
 *
 * `slot` is the address of an update slot, or @ref AGM_BITSTREAM_CONFIG_ADDR
 * when the record is missing/torn/not usable and the factory region is what
 * gets streamed (`from_record` is false then, and `len`/`crc` are zero).
 *
 * The boot path needs the length and the CRC, not just the address: with
 * CONFIG_BOOT_AGM_BITSTREAM_SIGNED the slot's content has to be checked
 * against the record *before* the fabric is taken down (a slot that does not
 * match falls back to the factory instead of killing the core), and the record
 * is the only place that says how long the image is.
 */
struct agm_bitstream_boot {
	uint32_t slot;        /* absolute address of the image to stream */
	uint32_t len;         /* image length in bytes (0 from the factory) */
	uint32_t crc;         /* CRC-32 over the image      (0 from the factory) */
	bool from_record;     /* false: the factory fallback */
};

/**
 * @brief Which image the next boot should stream.
 *
 * Reads the two record copies and takes the valid one with the higher
 * sequence; anything else (no record, torn record, a slot that is not one of
 * the two, a length that cannot fit, a record whose own CRC does not match)
 * falls back to the factory slot. Both the FCB bring-up
 * (soc/agm/agrv2k/fcb.c) and the bootloader driver call this, so there is one
 * reading of the record, not two.
 *
 * Safe to call before any driver exists: it only reads flash, and a record
 * that is not there reads back as erased (0xffffffff), which fails the magic
 * check.
 */
static inline struct agm_bitstream_boot agm_bitstream_boot_select(void)
{
	const volatile uint8_t *base = (const volatile uint8_t *)AGM_BITSTREAM_RECORD_ADDR;
	uint32_t best_seq = 0U;
	struct agm_bitstream_boot best = {
		.slot = AGM_BITSTREAM_CONFIG_ADDR,
		.len = 0U,
		.crc = 0U,
		.from_record = false,
	};

	for (uint32_t i = 0U; i < AGM_BITSTREAM_RECORD_COPIES; i++) {
		const volatile struct agm_bitstream_record *copy =
			(const volatile struct agm_bitstream_record *)
				(base + i * AGM_BITSTREAM_RECORD_STRIDE);

		if (copy->magic != AGM_BITSTREAM_RECORD_MAGIC) {
			continue;
		}
		if (copy->slot != AGM_BITSTREAM_SLOT1_ADDR &&
		    copy->slot != AGM_BITSTREAM_SLOT2_ADDR) {
			continue;
		}
		if (copy->len == 0U || copy->len > AGM_BITSTREAM_SLOT_SIZE) {
			continue;
		}
		if (copy->rec_crc !=
		    agm_bitstream_record_crc((const volatile uint32_t *)copy)) {
			continue;
		}
		if (copy->seq >= best_seq) {
			best_seq = copy->seq;
			best.slot = copy->slot;
			best.len = copy->len;
			best.crc = copy->crc;
			best.from_record = true;
		}
	}
	return best;
}

/**
 * @brief Which slot holds the image the next boot should stream.
 *
 * The address-only view of @ref agm_bitstream_boot_select(): an update slot,
 * or the factory region's config address. The factory fallback is the
 * *config* inside the factory region: for a compressed board that is where the
 * option byte points (and where the ROM streamed from), so it is what a caller
 * reporting "which image is live" should see.
 */
static inline uint32_t agm_bitstream_slot_in_use(void)
{
	return agm_bitstream_boot_select().slot;
}

/**
 * @brief Where the FCB has to start streaming the selected image from.
 *
 * The factory region is streamed as it sits there (raw, or in the vendor's
 * compressed form, which the ROM already knows how to decode); a signed update
 * slot carries an MCUboot header first, so the fabric payload starts
 * AGM_BITSTREAM_HDR_SIZE bytes in.
 */
static inline uint32_t agm_bitstream_stream_addr(const struct agm_bitstream_boot *sel)
{
	return sel->from_record ? (sel->slot + AGM_BITSTREAM_HDR_SIZE) : sel->slot;
}

/** The slot an update has to go into: the one that is not in use. */
static inline uint32_t agm_bitstream_inactive_slot(void)
{
	return (agm_bitstream_slot_in_use() == AGM_BITSTREAM_SLOT1_ADDR)
		       ? AGM_BITSTREAM_SLOT2_ADDR
		       : AGM_BITSTREAM_SLOT1_ADDR;
}

/**
 * @brief Is the fabric in @a slot_addr genuine? (boot driver)
 *
 * Implemented by drivers/misc/boot_agm.c (CONFIG_BOOT_AGM); the boot path in
 * soc/agm/agrv2k/fcb.c calls it before it streams a slot, and the upload path
 * calls it before it commits one. With CONFIG_BOOT_AGM_BITSTREAM_SIGNED it
 * checks the record's CRC-32 first -- a cheap read-only check that a damaged
 * slot fails without the fabric ever being taken down -- and then the MCUboot
 * container's SHA-256, KEYHASH and ECDSA P-256 signature. Without that option
 * the slots hold raw fabric and this returns 0.
 *
 * @param slot_addr  AGM_BITSTREAM_SLOT1_ADDR or AGM_BITSTREAM_SLOT2_ADDR
 * @param len        image length in bytes, as the record has it
 * @param crc        CRC-32 in the record over the same bytes
 *
 * @return 0 when the image may be streamed, a negative errno otherwise.
 */
int agm_boot_bitstream_verify(uint32_t slot_addr, uint32_t len, uint32_t crc);

/** Sequence for the next record write (strictly increasing). */
static inline uint32_t agm_bitstream_seq_next(void)
{
	const volatile uint8_t *base = (const volatile uint8_t *)AGM_BITSTREAM_RECORD_ADDR;
	uint32_t best = 0U;

	for (uint32_t i = 0U; i < AGM_BITSTREAM_RECORD_COPIES; i++) {
		const volatile struct agm_bitstream_record *copy =
			(const volatile struct agm_bitstream_record *)
				(base + i * AGM_BITSTREAM_RECORD_STRIDE);

		if (copy->magic == AGM_BITSTREAM_RECORD_MAGIC &&
		    copy->rec_crc ==
			    agm_bitstream_record_crc((const volatile uint32_t *)copy) &&
		    copy->seq > best) {
			best = copy->seq;
		}
	}
	return best + 1U;
}

#endif /* ZEPHYR_SOC_AGM_AGRV2K_AGM_BITSTREAM_H_ */
