/*
 * Copyright (c) 2026 AgRV Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief AgRV2K bootloader / DFU driver -- storage, upload and boot.
 *
 * This unit owns the layout, the boot record, the upload targets (store A/B,
 * the on-die application slot, bitstream staging), the A/B policy with its
 * trial watchdog, and the console's binary upload protocol. The rest of the
 * driver is next to it: boot_agm_verify.c (may this image be published or
 * booted), boot_agm_auth.c (the command gate), boot_agm_an3155.c and
 * boot_agm_smp.c (the two protocol servers). What they share is
 * include/zephyr/drivers/misc/boot_agm.h and boot_agm_priv.h.
 *
 * Both the layout and the two flashes come from one devicetree node
 * (dts/bindings/misc/agm,agrv2k-boot.yaml): moving a region, or pointing
 * `store-flash` at the on-die flash instead of the external NOR, is a DT
 * change. The driver keeps one bootloader's worth of state file-static and
 * still takes the device, so callers stay in DEVICE_DT_GET() shape.
 *
 * Rationale, dev board records and the design history are not in this
 * tree; git log on this file is the record.
 */

#define DT_DRV_COMPAT agm_agrv2k_boot

#include <zephyr/arch/cpu.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/drivers/misc/boot_agm.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/crc.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/sys/util.h>
#include <zephyr/drivers/watchdog.h>
#include <zephyr/drivers/misc/agm_bitstream.h>

#include <string.h>

/* The RTC/backup-domain register map, shared with the counter and watchdog
 * drivers and with the applications that answer the trial handshake (see the
 * one-shot section below for why this driver talks to those registers itself). */
#include <zephyr/drivers/counter/agm_rtc_regs.h>

/* The two signed profiles hash through different libraries, and that is
 * deliberate rather than an accident of history: ECDSA P-256 verifies with
 * MCUboot's vendored tinycrypt, so its SHA-256 comes from the same library
 * (one implementation, no PSA pulled in at all), while the RSA profile
 * already links PSA for the signature and hashes with PSA's SHA-256 --
 * MCUboot's copy would be a *second* SHA-256 in the same image, ~1.4 KB of
 * ROM (rom_report shows tinycrypt/sha256.c at 1426 B while
 * tf-psa-crypto carries its own). */
/* Two profiles, two key encodings -- and the fabric profile is ECDSA even in
 * an RSA build, so "ECDSA" here is the union of "the application uses it" and
 * "a signed fabric does" (see CONFIG_BOOT_AGM_BITSTREAM_SIGNED). The command
 * authorization (CONFIG_BOOT_AGM_LOCK_PRODUCTION) is a third user of the
 * same verifier: it checks a host-supplied signature over the command, so it
 * needs the curve and the hash but no image profile. */
#include "boot_agm_priv.h" /* also defines AGM_HAVE_TINYCRYPT_ECDSA */

#if defined(AGM_HAVE_TINYCRYPT_ECDSA)
#include <tinycrypt/ecc.h>
#include <tinycrypt/ecc_dsa.h>
#include <tinycrypt/sha256.h>
#endif

#if !defined(CONFIG_BOOT_AGM_BIND)
/* The binding half is not built: the address still has to answer, so callers
 * (and the console) do not need an #ifdef of their own. */
uint32_t agm_boot_bind_salt_addr(void);
uint32_t agm_boot_bind_salt_addr(void)
{
	return 0U;
}
#endif
#if defined(CONFIG_BOOT_AGM_SIG_RSA2048_PSS)
#include <psa/crypto.h>
#endif

BUILD_ASSERT(DT_NUM_INST_STATUS_OKAY(DT_DRV_COMPAT) == 1,
	     "the bootloader driver is single-instance");

/* ---- layout invariants ---------------------------------------------- */

/*
 * The whole storage layout comes from the devicetree node, and a node whose
 * regions overlap is the one mistake this driver cannot catch at runtime: the
 * reads and writes all succeed, they just do the wrong thing. An on-die A/B
 * overlay once declared store B as 64 KiB at 0x21000 while the application
 * slot starts at 0x30000, so the last 4 KiB of store B *were* the slot's first
 * sector -- an upload into store B erased the running image, and
 * store_write_slot() (which erases the slot before it reads the store back)
 * destroyed the image it was about to copy. Every relationship the driver
 * relies on is checked here instead, so that mistake fails the build.
 */

/* The erase loops in store_write_slot()/bitstream_commit() step a whole
 * record sector at a time (`for (done = 0; done < len; done += sector)`), so
 * every region length must be a whole number of sectors: an unaligned length
 * would let them erase past the region they own.
 */
BUILD_ASSERT((BOOT_CFG_SECTOR & (BOOT_CFG_SECTOR - 1U)) == 0U,
	     "record-size is the erase granularity and must be a power of two");
BUILD_ASSERT((SLOT_STRIDE % BOOT_CFG_SECTOR) == 0U,
	     "store-max-size must be a multiple of record-size");
BUILD_ASSERT((SLOT_MAX % BOOT_CFG_SECTOR) == 0U,
	     "slot-size must be a multiple of record-size");
BUILD_ASSERT((BS_ON_DIE_MAX % BOOT_CFG_SECTOR) == 0U,
	     "bitstream-size must be a multiple of record-size");

#if BOOT_ON_DIE_AB
/* The 12 KiB the layout keeps for metadata is the boot record and, directly
 * after it, the bind-salt sector: one contiguous block at the top of the
 * chain. The salt is written once and each record write erases a whole
 * sector, so the two must never share one -- the assert is the layout's half
 * of that, the writers' half is that they only ever erase the sector they
 * own. */
BUILD_ASSERT(BOOT_CFG_OFF + BOOT_CFG_RECORDS * BOOT_CFG_SECTOR <= BOOT_SALT_OFF,
	     "the bind-salt sector overlaps the boot record");
#endif

/* The two stores are `<store-max-size>` bytes each, one after the other. */
BUILD_ASSERT(BOOT_STORE_A + SLOT_STRIDE <= BOOT_STORE_B,
	     "store-a-offset + store-max-size overlaps store-b-offset");

/* The chain must not reach into the fabric area. For the on-die family the
 * chain ends with the salt (slot, then the 12 KiB metadata block); for the
 * two-flash family the on-die chain is just the application slot, and the
 * salt has its own asserts below. */
#if BOOT_ON_DIE_AB
BUILD_ASSERT(BOOT_SALT_OFF + BOOT_CFG_SECTOR <= BS_SLOT1_OFF,
	     "the A/B layout runs into fabric slot 1: lower app-size (or "
	     "loader-size/record-size)");
#else
BUILD_ASSERT(SLOT_OFF + SLOT_MAX <= BS_SLOT1_OFF,
	     "slot-address + slot-size overlaps fabric slot 1");
#endif

/* The reservation must fit in the flash it lives in. That is only checkable
 * when the on-die flash node declares its size in `reg` (the SoC's on-die
 * flash does; a node that had to state its base with `on-die-flash-base` does
 * not, and the SPI NOR never does -- for those the staging-area check below is
 * what is left).
 */
#if !DT_INST_NODE_HAS_PROP(0, on_die_flash_base)
BUILD_ASSERT(BS_ON_DIE_OFF + BS_ON_DIE_MAX <=
		     DT_REG_SIZE(DT_INST_PHANDLE(0, on_die_flash)),
	     "bitstream-address + bitstream-size runs past the on-die flash");
#endif

/* The bitstream A/B geometry (agm_bitstream.h) has to agree with the node:
 * the factory slot is the node's `bitstream-address`, and the two update slots
 * plus the record have to sit below it with nothing of the A/B layout in the
 * way. The addresses are the SoC's, not the layout's, because the record has
 * to be findable before any driver runs (fcb.c reads it in every image).
 */
BUILD_ASSERT(BS_ON_DIE == (uint32_t)AGM_BITSTREAM_FACTORY_ADDR,
	     "bitstream-address must be the factory slot (agm_bitstream.h)");
BUILD_ASSERT(BS_ON_DIE_MAX == (uint32_t)AGM_BITSTREAM_SLOT_SIZE,
	     "bitstream-size must be the slot size (agm_bitstream.h)");
BUILD_ASSERT(AGM_BITSTREAM_SLOT_SIZE % BOOT_CFG_SECTOR == 0U,
	     "the fabric slot must be a whole number of erase sectors");
BUILD_ASSERT(AGM_BITSTREAM_SLOT_SIZE >= AGM_BITSTREAM_IMAGE_LEN,
	     "the fabric slot is smaller than one fabric image (99944 B)");
BUILD_ASSERT(BS_SLOT1_OFF + AGM_BITSTREAM_SLOT_SIZE <= BS_SLOT_OFF(BS_SLOT2_ADDR),
	     "the two bitstream slots overlap");
BUILD_ASSERT(BS_SLOT_OFF(BS_SLOT2_ADDR) + AGM_BITSTREAM_SLOT_SIZE <= BS_RECORD_OFF,
	     "bitstream slot 2 runs into the boot record sector");
BUILD_ASSERT(BS_RECORD_OFF + BOOT_CFG_SECTOR <= BS_ON_DIE_OFF,
	     "the bitstream boot record sector runs into the factory slot");
BUILD_ASSERT(SLOT_OFF + SLOT_MAX <= BS_SLOT1_OFF,
	     "the A/B layout runs into bitstream slot 1: lower app-size (or "
	     "slot-size/store-max-size in the two-flash family)");

/* Where the per-chip binding salt lives (CONFIG_BOOT_AGM_BIND): one erase
 * sector in the gap between the two fabric update slots. It is there rather
 * than in the firmware region because the salt has to outlive a reflash --
 * `west flash` erases and rewrites the loader region and the boot record, and
 * a salt that went with them would invalidate every image already bound to
 * this chip. The gap is what makes that possible; nothing else may use it.
 * (On agrv2k_407, a provisioned salt survives both `west flash
 * --skip-bitstream` and a full firmware + canonical-bitstream `west flash` --
 * the vendor flash driver's auto-erase only takes the sectors the image being
 * written covers. Verified at the salt's derived 0x800b0000 with the same
 * result.)
 */
#if defined(CONFIG_BOOT_AGM_BIND)
BUILD_ASSERT(BOOT_SALT_OFF % BOOT_CFG_SECTOR == 0U,
	     "the bind-salt sector must be erase-sector aligned");
BUILD_ASSERT(BOOT_SALT_OFF + BOOT_CFG_SECTOR <= BS_SLOT1_OFF,
	     "the bind-salt sector overlaps fabric slot 1");
/* The salt is a secret the *device* never erases by accident, so it also
 * wants to sit outside the region an image write or a reflash covers: on the
 * on-die family that is what the packing gives (writes cover a whole number
 * of sectors from the covered data's first sector, and the salt is the last
 * sector of the chain, above the slot's image). */
#endif /* CONFIG_BOOT_AGM_BIND */

/* The on-die family derives every offset from two sizes, so what is left to
 * check is that the sizes themselves are usable; the chain (loader -> record
 * -> A -> B -> slot) and its end are asserted with the bitstream geometry
 * above. This is the whole reason the layout can be one number.
 */
#if BOOT_ON_DIE_AB
BUILD_ASSERT((BOOT_LOADER_SIZE % BOOT_CFG_SECTOR) == 0U,
	     "loader-size must be a multiple of record-size");
BUILD_ASSERT((BOOT_APP_SIZE % BOOT_CFG_SECTOR) == 0U,
	     "app-size must be a multiple of record-size");
BUILD_ASSERT(BOOT_APP_SIZE >= BOOT_CFG_SECTOR, "app-size is too small to hold a sector");

#else
/* With the stores in the external NOR the two address spaces are independent,
 * so store B must not overlap the on-die application slot only when they in
 * fact share a flash -- and other than the on-die family they do not.
 */
BUILD_ASSERT(BOOT_STORE_B + SLOT_STRIDE <= SLOT_OFF ||
		     !DT_SAME_NODE(DT_INST_PHANDLE(0, store_flash),
				   DT_INST_PHANDLE(0, on_die_flash)),
	     "store-b-offset + store-max-size overlaps the application slot");
#endif /* BOOT_ON_DIE_AB */


/* The packed anti-rollback key (CONFIG_BOOT_AGM_ANTI_ROLLBACK):
 * `major << 24 | minor << 16 | revision`, formed from MCUboot's big-endian
 * `ih_ver`. The build number is deliberately not part of the ordering. */
#define IMG_VER_MAJOR(v) ((uint32_t)(v) >> 24)
#define IMG_VER_MINOR(v) (((uint32_t)(v) >> 16) & 0xffU)
#define IMG_VER_REV(v)   ((uint32_t)(v) & 0xffffU)

#define BOOT_CFG_MAGIC   0x31434253U /* "SBC1" */
#define BOOT_CFG_VERSION 4U

/* The record is two sectors used as an append log (newest entry wins by `seq`),
 * so a torn write costs at most the newest entry -- the previous generation is
 * still valid -- and a sector holds many commits before it has to be erased.
 * Wear and tear were.
 */

#define BOOT_SLOTS        2U
#define BOOT_MAX_ATTEMPTS AGM_BOOT_MAX_ATTEMPTS

/* Protocol constants under their historical names (the console protocol and
 * the AN3155 server read the same as before). */
#define UP_MAGIC      0xA5U
#define UP_ACK        0x79U
#define UP_NAK        0x1FU
#define UP_CMD_DATA   0x01U
#define UP_CMD_FINISH 0x02U
#define UP_MAX_PAYLOAD AGM_BOOT_UPLOAD_CHUNK
#define UP_ERR_ARG    1U
#define UP_ERR_CRC    2U
#define UP_ERR_FLASH  3U
/* The verifier said no (bad digest/signature/KEYHASH, a container linked for
 * another slot, or a signed build with no trusted key). Distinct from
 * UP_ERR_FLASH because the two have nothing to do with each other: reusing
 * "flash write/verify failed" for a rejected image sent every dev board session
 * down the wrong path -- an RSA-signed container with one
 * flipped payload byte came back as code 3. Hosts that predate this code see an
 * unknown code there, which is still recognisably "not a flash error". */
#define UP_ERR_REJECT 4U
/* The image is genuine but older than the floor the record carries
 * (CONFIG_BOOT_AGM_ANTI_ROLLBACK): a separate code, because "install a newer
 * one" is a different fix from "sign it properly". */
#define UP_ERR_OLD_VERSION 5U
/* The build is production-locked and the host has not authorized this
 * command: again its own code, because "get the key and sign it" is
 * neither "the image is bad" nor "the flash failed", and code 4 would send
 * the operator looking at the image. */
#define UP_ERR_UNAUTHORIZED 6U

/* The image is genuine but bound to another chip (AGM_BOOT_E_UNBOUND): its own
 * code for the same reason as UP_ERR_OLD_VERSION -- a production line that
 * mixed up two boards needs to see *that*, not "the image was rejected". */
#define UP_ERR_UNBOUND 7U

/* The API's enums under their historical names. */
#define BOOT_MODE_AUTO     AGM_BOOT_MODE_AUTO
#define BOOT_MODE_INTERNAL AGM_BOOT_MODE_INTERNAL
#define SLOT_EMPTY         AGM_BOOT_SLOT_EMPTY
#define SLOT_TRIAL         AGM_BOOT_SLOT_TRIAL
#define SLOT_CONFIRMED     AGM_BOOT_SLOT_CONFIRMED
#define SLOT_BAD           AGM_BOOT_SLOT_BAD
#define CFG_SRC_STORE      AGM_BOOT_SRC_STORE
#define CFG_SRC_ON_DIE     AGM_BOOT_SRC_ON_DIE

/* RTC backup domain: the one-shot override that survives a reset (the vendor
 * DFU's `mscratch` trick). The base comes from the RTC node and the register
 * layout from the header the counter driver also uses, so the two cannot drift
 * apart again -- they had, and this driver was waiting on a prescaler bit and
 * storing its magic in the alarm registers. The feature is
 * compiled out when the node does not exist or CONFIG_BOOT_AGM_ONESHOT=n -- a
 * target without a backup domain (or a native_sim test) then answers
 * "unavailable" instead of touching registers that are not there. */
#define ONESHOT_NODE DT_NODELABEL(rtc0)

#if DT_NODE_EXISTS(ONESHOT_NODE) && defined(CONFIG_BOOT_AGM_ONESHOT)
#define BOOT_AGM_ONESHOT 1
#define RTC_BASE      ((uint32_t)DT_REG_ADDR(ONESHOT_NODE))
#define RTC_BKP_DR(i) (*(volatile uint16_t *)(RTC_BASE + AGM_RTC_BKP_DR(i)))
#define RTC_CRL      (*(volatile uint16_t *)(RTC_BASE + AGM_RTC_CRL))
#define BKP_ONCE_MAGIC_IDX 0U
#define BKP_ONCE_MODE_IDX  1U
#define BKP_ONCE_MAGIC     0x5342U
#else
#define BOOT_AGM_ONESHOT 0
#endif

/* The trial-boot watchdog: the loader arms the backup-domain IWDG right before
 * it jumps into a TRIAL image, and clears it on every other path. The handshake
 * registers it writes live in the same domain and are shared with the
 * application through include/zephyr/drivers/misc/boot_agm.h -- the whole
 * rationale is written up there; this side is the caller.
 *
 * Nothing here is compiled when the SoC has no IWDG node (a native test, for
 * instance): the ticket then stays 0, the loader never arms anything, and the
 * A/B policy behaves exactly as it did before.
 */
#if defined(CONFIG_BOOT_AGM_TRIAL_WATCHDOG)
#define BOOT_TRIAL_WATCHDOG 1
/* The SYS controller's reset-cause register is a SoC thing: on a simulated
 * target (a native test that enables this feature to exercise the chain) the
 * header is not on the include path, and the hook below answers "no cause"
 * until the test overrides it. */
#if defined(CONFIG_SOC_AGM_AGRV2K)
#include <agm_sys.h>
#define BOOT_TRIAL_HAVE_RST_CAUSE 1
#else
#define BOOT_TRIAL_HAVE_RST_CAUSE 0
#endif
static const struct device *const trial_wdt = DEVICE_DT_GET(DT_NODELABEL(iwdg0));
#else
#define BOOT_TRIAL_WATCHDOG 0
#endif

/* One A/B slot as the record stores it. The field order is the on-flash order:
 * keep it stable, records are read back with it. */
struct boot_slot_cfg {
	uint32_t ext_off;  /* image store offset in the record's own flash */
	uint32_t ext_len;  /* image length */
	uint32_t ext_crc;  /* crc32_ieee of the image */
	uint32_t state;    /* enum agm_boot_slot_state */
	uint32_t attempts; /* trial boots spent */
	uint32_t load;     /* where the image must run */
	uint32_t entry;    /* where to jump */
	/* Where the image comes from: the store (copied into the on-die slot
	 * before it runs) or the on-die slot itself (on-die DFU). This field used
	 * to be `reserved[0]` and was written as zero, so records from before it
	 * existed read back as AGM_BOOT_SRC_STORE. */
	uint32_t src;      /* enum agm_boot_source */
};

struct boot_cfg {
	uint32_t magic;
	uint32_t version;
	uint32_t seq;   /* commit order; the newest valid entry wins */
	uint32_t mode;  /* enum agm_boot_mode (policy) */
	uint32_t active;/* which slot the policy boots */
	struct boot_slot_cfg slot[BOOT_SLOTS];
	/* The anti-rollback floor (CONFIG_BOOT_AGM_ANTI_ROLLBACK):
	 * `major << 24 | minor << 16 | revision` of the highest image version
	 * accepted so far. One word for the whole board, not per slot: a floor
	 * that a rollback to the other slot could lower would not be a floor.
	 * v4 added this field. */
	uint32_t sec_ver;
	uint32_t crc;   /* crc32_ieee over the bytes above (see cfg_entry_ok) */
};

/* The append log rolls to the next record when the entry would not fit, which
 * only makes sense while an entry is smaller than a whole sector (it is what
 * lets `BOOT_CFG_SECTOR - sizeof(struct boot_cfg)` be computed without
 * wrapping). */
BUILD_ASSERT(sizeof(struct boot_cfg) <= BOOT_CFG_SECTOR / 2U,
	     "a boot record entry must fit in half a record sector");

BUILD_ASSERT(sizeof(struct boot_cfg) <= BOOT_CFG_SECTOR, "an entry has to fit a sector");

/* Older record layouts (v2: one sector, struct-of-arrays, no seq/CRC; v3: the
 * shapes above minus `sec_ver`) are *not* imported any more. Nothing has been
 * released with them, and importing meant trusting fields nobody checked --
 * a corrupt v3 record was copied forward into a fresh v4 one, corruption and
 * all (; the alternative, a CRC check, would have had to carry
 * the v3 layout and its algorithm forever for a board population of zero).
 * An old record therefore reads as "invalid", which is the path a foreign
 * record already took: cfg_read() reports it, the boot policy finds nothing
 * bootable, and the first publish erases the sector and writes a fresh log
 * (test_00_publish_over_foreign_record). cfg_read()
 * prints what version it found, so an operator who downgraded firmware and
 * came back is not left guessing where the A/B state went. */

/* The moved bodies spell the mode type the way the sample did. */
#define boot_mode agm_boot_mode

/* Forward declarations: the moved sections refer to each other in the order
 * they were written in the sample. */
static int cfg_read(void);
static int cfg_write(struct boot_cfg *c);
static bool cfg_valid(void);
static char slot_letter(uint32_t slot);
static int store_write_slot(uint32_t slot);
static uint32_t slot_content_crc(uint32_t len);
static bool slot_is_blank(void);
static int payload_install_slot(void);
static int store_boot(uint32_t slot);
static bool boot_ab_policy(void);
static enum agm_boot_mode once_take(void);
static int once_arm(enum agm_boot_mode mode);
/* Trial-watchdog hooks: weak, so a native test can drive the chain instead of
 * the hardware it would otherwise talk to . */
int agm_boot_bkp_read(uint32_t idx, uint16_t *val);
int agm_boot_bkp_write(uint32_t idx, uint16_t val);
uint32_t agm_boot_reset_cause_take(void);
bool agm_boot_jump_hook(uint32_t entry);

/* ---- state (one bootloader per SoC) --------------------------------- */

static struct boot_cfg cfg;

/* Where the append log stands: `cfg_sector` holds the newest entry and
 * `cfg_used` is how many bytes of it are taken (entries are fixed size and
 * appended, so the used length is also the offset of the next one). */
static uint32_t cfg_seq;
static uint32_t cfg_sector;
static uint32_t cfg_used;
/* True while the current sector has not been appended to yet, i.e. its content
 * is unknown (a fresh board is erased, but a board that ran another tool -- or
 * an older record layout -- can hold anything there). The first append then
 * erases the sector: NOR can only clear bits, so appending over old bytes
 * would corrupt the entry.: the record sector
 * held 0xdeadbeef from a 3.26.22 test and the first publish failed with
 * "flash write/verify failed". */
static bool cfg_fresh;

/* Set when the record sector could not even be read (the init read failed):
 * the state below is then BSS zeros, not what the flash holds, and the
 * difference matters to whoever reads `info` on a board that misbehaves. */
static bool cfg_read_failed;

static const struct device *const console_uart =
	DEVICE_DT_GET(DT_CHOSEN(zephyr_console));

/* Per-target upload session. `erased_end` is the lazy-erase watermark: every
 * sector below it is known to be erased, so a chunk only erases what it
 * crosses (the flash drivers reject unaligned erase calls, and the bitstream
 * staging area is not sector aligned). `carry` holds the sub-word tail of a
 * chunk so a host may hand over any chunk size -- the SPI NOR driver programs
 * 4 bytes at a time. */
struct boot_agm_upload {
	uint32_t len;        /* highest end written: what finish/GO publishes */
	uint32_t erased_end; /* first sector known to be erased */
	uint8_t carry[4];
	uint32_t carry_len;
	uint32_t carry_off;
	/* Bitstream target only: the slot this session writes into. Pinned at
	 * upload_begin() so it cannot move under a session -- committing a
	 * record flips which slot is "inactive", and a window that moved
	 * mid-session would desynchronise the host's addressing. */
	uint32_t slot_off;
	bool active;
};

static struct boot_agm_upload ups[AGM_BOOT_TARGET_COUNT];
static uint8_t join[AGM_BOOT_UPLOAD_CHUNK + 4U];

/* ---- console UART --------------------------------------------------- */

static void console_uart_write(const uint8_t *buf, size_t len)
{
	for (size_t i = 0; i < len; i++) {
		uart_poll_out(console_uart, buf[i]);
	}
}

static bool console_poll_char(char *c)
{
	return uart_poll_in(console_uart, c) == 0;
}

/* ---- chip unique ID (binding) --------------------------------------- */

/*
 * The chip's unique ID: 0x4B through the flash controller's flex-read engine,
 * the same path the vendor SDK's FLASH_GetUniqueID() takes (four 32-bit reads
 * at byte offsets 4/8/12/16 with four dummy bytes). Two details are
 * load-bearing:
 *   - the controller has to be unlocked with the two *public* constants first,
 *     or every read answers 0x00000000 -- which is exactly
 *     why this is a serial number and not a secret;
 *   - this can run at PRE_KERNEL_1 (the loader), where there is no kernel
 *     timer, so the wait is a bounded nop/spin loop and a timeout returns
 *     instead of hanging a boot.
 */
/* The controller's register block (a separate address from the array at
 * 0x80000000): the SoC fixes it, and the dtsi documents it as
 * flash-controller@40001000. It only exists on the real SoC -- native_sim's
 * simulated flash has none -- so the read reports -ENOTSUP there instead of
 * failing to build (the native suite injects the UID through the binding's
 * weak hook anyway). */
#if defined(CONFIG_SOC_AGM_AGRV2K)
#define FLASH_CTL_BASE       0x40001000U
#define AGM_HAVE_FLASH_CTL   1
#else
#define AGM_HAVE_FLASH_CTL   0
#endif
#define FLASH_CTL_KEYR       0x04U
#define FLASH_CTL_SR         0x0cU
#define FLASH_CTL_CR         0x10U
#define FLASH_CTL_READ_CTRL  0x2cU
#define FLASH_CTL_READ_DATA  0x30U
#define FLASH_CTL_SR_BSY     BIT(0)
#define FLASH_CTL_CR_STRT    BIT(6)
#define FLASH_CTL_CR_LOCK    BIT(7)
#define FLASH_CTL_CR_READ    BIT(14)
#define FLASH_CTL_KEY1       0x45670123U
#define FLASH_CTL_KEY2       0xCDEF89ABU
#define UID_CMD              0x4BU
#define UID_DUMMY_BYTES      4U
#define UID_SPIN_LIMIT       100000U

int agm_boot_unique_id(uint8_t uid[AGM_BOOT_UID_LEN])
{
	if (uid == NULL) {
		return -EINVAL;
	}

#if !AGM_HAVE_FLASH_CTL
	return -ENOTSUP;
#else
	sys_write32(FLASH_CTL_KEY1, FLASH_CTL_BASE + FLASH_CTL_KEYR);
	sys_write32(FLASH_CTL_KEY2, FLASH_CTL_BASE + FLASH_CTL_KEYR);

	for (uint32_t i = 0U; i < AGM_BOOT_UID_LEN / 4U; i++) {
		uint32_t spins = 0U;
		uint32_t words;

		/* READ_CTRL = cmd | data_bytes << 8 | dummy_bytes << 16 |
		 * has_addr << 31 (SDK framework-agrv_sdk/src/flash.c:
		 * FLASH_FlexRead()). Verified against that source:
		 * FLASH_GetUniqueID() there is
		 *     FLASH_FlexRead(0x4b, 0, 0, 4/8/12/16, 4)
		 * i.e. has_addr = 0 and the *offset* in the data_bytes field --
		 * which is what this loop builds (the offset is data_bytes - 4
		 * because the first read must yield four bytes). This cross-check is here
		 * because that shape is easy to get wrong. */
		sys_write32(UID_CMD | ((4U + i * 4U) << 8) | (UID_DUMMY_BYTES << 16),
			    FLASH_CTL_BASE + FLASH_CTL_READ_CTRL);
		sys_write32(sys_read32(FLASH_CTL_BASE + FLASH_CTL_CR) | FLASH_CTL_CR_READ,
			    FLASH_CTL_BASE + FLASH_CTL_CR);
		/* The SDK idles a few hundred cycles before starting the engine. */
		for (volatile int nop = 0; nop < 64; nop++) {
			;
		}
		sys_write32(sys_read32(FLASH_CTL_BASE + FLASH_CTL_CR) | FLASH_CTL_CR_STRT,
			    FLASH_CTL_BASE + FLASH_CTL_CR);

		while ((sys_read32(FLASH_CTL_BASE + FLASH_CTL_SR) & FLASH_CTL_SR_BSY) != 0U) {
			if (++spins > UID_SPIN_LIMIT) {
				sys_write32(FLASH_CTL_CR_LOCK, FLASH_CTL_BASE + FLASH_CTL_CR);
				return -ETIMEDOUT;
			}
		}

		sys_write32(sys_read32(FLASH_CTL_BASE + FLASH_CTL_CR) & ~FLASH_CTL_CR_READ,
			    FLASH_CTL_BASE + FLASH_CTL_CR);
		words = sys_read32(FLASH_CTL_BASE + FLASH_CTL_READ_DATA);

		uid[i * 4U + 0U] = (uint8_t)(words & 0xffU);
		uid[i * 4U + 1U] = (uint8_t)((words >> 8) & 0xffU);
		uid[i * 4U + 2U] = (uint8_t)((words >> 16) & 0xffU);
		uid[i * 4U + 3U] = (uint8_t)((words >> 24) & 0xffU);
	}

	sys_write32(FLASH_CTL_CR_LOCK, FLASH_CTL_BASE + FLASH_CTL_CR);
	return 0;
#endif /* AGM_HAVE_FLASH_CTL */
}

/* ---- target geometry ------------------------------------------------ */

const char *agm_boot_target_name(enum agm_boot_target target)
{
	switch (target) {
	case AGM_BOOT_TARGET_STORE_A:
		return "a";
	case AGM_BOOT_TARGET_STORE_B:
		return "b";
	case AGM_BOOT_TARGET_SLOT:
		return "slot";
	case AGM_BOOT_TARGET_BITSTREAM:
		return "bitstream";
	default:
		return "?";
	}
}

const char *agm_boot_mode_name(enum agm_boot_mode mode)
{
	switch (mode) {
	case BOOT_MODE_AUTO:
		return "auto";
	case BOOT_MODE_INTERNAL:
		return "internal";
	default:
		return "?";
	}
}

const char *agm_boot_slot_state_name(enum agm_boot_slot_state state)
{
	switch (state) {
	case SLOT_EMPTY:
		return "empty";
	case SLOT_TRIAL:
		return "trial";
	case SLOT_CONFIRMED:
		return "confirmed";
	case SLOT_BAD:
		return "bad";
	default:
		return "?";
	}
}

/* Bitstream slot bookkeeping for the driver. fcb.c reads the same record with
 * the header's inline reader, which dereferences the memory-mapped address --
 * correct for the FCB bring-up (it runs on the real thing, before any driver
 * exists, so there is no flash API to call) but not for the simulated flash
 * this driver is also built against. The two cannot be merged -- one side has
 * no driver, the other has no memory map -- so the driver goes through
 * the flash API and keeps its own (small) reading. */
static uint32_t bs_live_slot(void)
{
	struct agm_bitstream_record rec;
	uint32_t best_seq = 0U;
	uint32_t best_slot = 0U;

	for (uint32_t i = 0U; i < AGM_BITSTREAM_RECORD_COPIES; i++) {
		if (flash_read(INT_FLASH,
			       BS_RECORD_OFF + i * AGM_BITSTREAM_RECORD_STRIDE,
			       &rec, sizeof(rec)) < 0) {
			continue;
		}
		if (rec.magic != AGM_BITSTREAM_RECORD_MAGIC) {
			continue;
		}
		if (rec.slot != BS_SLOT1_ADDR && rec.slot != BS_SLOT2_ADDR) {
			continue;
		}
		if (rec.len == 0U || rec.len > BS_ON_DIE_MAX) {
			continue;
		}
		if (rec.rec_crc != agm_bitstream_record_crc((const uint32_t *)&rec)) {
			continue;
		}
		if (rec.seq >= best_seq) {
			best_seq = rec.seq;
			best_slot = rec.slot;
		}
	}
	return (best_slot != 0U) ? best_slot : BS_ON_DIE;
}

static uint32_t bs_inactive_slot(void)
{
	return (bs_live_slot() == BS_SLOT1_ADDR) ? BS_SLOT2_ADDR : BS_SLOT1_ADDR;
}

static uint32_t bs_seq_next(void)
{
	struct agm_bitstream_record rec;
	uint32_t best = 0U;

	for (uint32_t i = 0U; i < AGM_BITSTREAM_RECORD_COPIES; i++) {
		if (flash_read(INT_FLASH,
			       BS_RECORD_OFF + i * AGM_BITSTREAM_RECORD_STRIDE,
			       &rec, sizeof(rec)) < 0) {
			continue;
		}
		if (rec.magic == AGM_BITSTREAM_RECORD_MAGIC &&
		    rec.rec_crc == agm_bitstream_record_crc((const uint32_t *)&rec) &&
		    rec.seq > best) {
			best = rec.seq;
		}
	}
	return best + 1U;
}

/* The bitstream slot a live session pinned, as an absolute address; the current
 * inactive one when no session is live. Everything that has to agree with the
 * host's addressing (the target offset, the AN3155 window, the commit) goes
 * through this. */
static uint32_t bs_session_slot_addr(void)
{
	return ups[AGM_BOOT_TARGET_BITSTREAM].active
		       ? (uint32_t)INT_FLASH_BASE + ups[AGM_BOOT_TARGET_BITSTREAM].slot_off
		       : bs_inactive_slot();
}

uint32_t agm_boot_target_offset(const struct device *dev, enum agm_boot_target target)
{
	ARG_UNUSED(dev);

	switch (target) {
	case AGM_BOOT_TARGET_STORE_A:
		return BOOT_STORE_A;
	case AGM_BOOT_TARGET_STORE_B:
		return BOOT_STORE_B;
	case AGM_BOOT_TARGET_SLOT:
		return SLOT_OFF;
	default:
		/* A bitstream update goes into the slot that is not running; the
		 * record then commits it. Once a session is live the slot is the
		 * one it pinned at begin -- see struct boot_agm_upload. */
		return ups[AGM_BOOT_TARGET_BITSTREAM].active
			       ? ups[AGM_BOOT_TARGET_BITSTREAM].slot_off
			       : BS_SLOT_OFF(bs_inactive_slot());
	}
}

uint32_t agm_boot_upload_max(const struct device *dev, enum agm_boot_target target)
{
	ARG_UNUSED(dev);

	switch (target) {
	case AGM_BOOT_TARGET_SLOT:
		return SLOT_MAX;
	case AGM_BOOT_TARGET_BITSTREAM:
		return BS_ON_DIE_MAX;
	default:
		return SLOT_STRIDE;
	}
}

uint32_t agm_boot_target_window_base(const struct device *dev, enum agm_boot_target target)
{
	ARG_UNUSED(dev);

	switch (target) {
	case AGM_BOOT_TARGET_STORE_A:
		/* The loader's own region doubles as store A's window. */
		return INT_FLASH_BASE;
	case AGM_BOOT_TARGET_STORE_B:
		return SLOT_BASE + SLOT_MAX;
	case AGM_BOOT_TARGET_SLOT:
		return SLOT_BASE;
	default:
		/* The AN3155 window follows the upload target. While a session is
		 * live it is the slot that session pinned (the window must not
		 * move under the host's addressing). */
		return bs_session_slot_addr();
	}
}

uint32_t agm_boot_target_window_size(const struct device *dev, enum agm_boot_target target)
{
	uint32_t window;

	switch (target) {
	case AGM_BOOT_TARGET_STORE_A:
		window = SLOT_BASE - INT_FLASH_BASE;
		break;
	case AGM_BOOT_TARGET_STORE_B:
		window = BS_ON_DIE - (SLOT_BASE + SLOT_MAX);
		break;
	case AGM_BOOT_TARGET_SLOT:
		window = SLOT_MAX;
		break;
	default:
		window = BS_ON_DIE_MAX;
		break;
	}

	/* A window is what a host may write through it, so it never advertises
	 * more than the target itself accepts: without this the store windows
	 * (the gap between the slot and the reservation, the loader's own region)
	 * were wider than store-max-size on the on-die layout, and a whole-file
	 * write failed half way through with a NACK per frame. */
	return MIN(window, agm_boot_upload_max(dev, target));
}

static const struct device *target_flash(enum agm_boot_target target)
{
	/* The bitstream target lives on-die too (its two slots are the SoC's,
	 * not the A/B layout's), so only the stores are in the other flash. */
	return (target == AGM_BOOT_TARGET_SLOT ||
		target == AGM_BOOT_TARGET_BITSTREAM) ? INT_FLASH : STORE_FLASH;
}

/* ---- RTC backup domain (one-shot override) ------------------------- */

#if BOOT_AGM_ONESHOT

/* Backup-domain writes wait for the RTC's RTOFF flag, which only comes up
 * while its clock runs. This build does not enable the RTC driver (the loader
 * needs neither the counter nor the LSE), so the wait is bounded and the
 * failure is reported -- otherwise the `once` command spins in the console
 * forever (after the boot driver moved out of the
 * sample: the command hung the loader and only a reset brought it back). The
 * bound is in wall-clock time rather than in loop iterations, so it does not
 * depend on the CPU or the optimisation level. */
#define BKP_READY_TIMEOUT_MS 50

static int bkp_write(uint32_t idx, uint16_t v)
{
	int64_t from = k_uptime_get();

	while ((RTC_CRL & AGM_RTC_CRL_RTOFF) == 0U) {
		if (k_uptime_get() - from > BKP_READY_TIMEOUT_MS) {
			return -EIO;
		}
	}
	RTC_BKP_DR(idx) = v;
	return 0;
}

static enum boot_mode once_take(void)
{
	if (RTC_BKP_DR(BKP_ONCE_MAGIC_IDX) != BKP_ONCE_MAGIC) {
		return BOOT_MODE_AUTO;
	}

	uint16_t mode = RTC_BKP_DR(BKP_ONCE_MODE_IDX);

	/* Clear the mode first and the magic last, the reverse of the order
	 * once_arm() writes them in: an interrupted consume then leaves "armed,
	 * mode 0", i.e. the "follow the record" override, never a stale mode.
	 *
	 * Both writes are checked. A failure must not look
	 * like a consumed override:
	 *  - if the *mode* could not be cleared, leave the magic alone and do not
	 *    honour the stale mode: the next boot retries the consume;
	 *  - if only the *magic* could not be cleared, the mode is already 0, so
	 *    the override now reads "follow the record" -- safe either way. */
	if (bkp_write(BKP_ONCE_MODE_IDX, 0) < 0) {
		printk("loader: could not clear the one-shot mode; leaving it armed\n");
		return BOOT_MODE_AUTO;
	}
	if (bkp_write(BKP_ONCE_MAGIC_IDX, 0) < 0) {
		printk("loader: could not clear the one-shot magic; it stays armed\n");
		return BOOT_MODE_AUTO;
	}

	/* 2 was `external`; a one-shot armed by such a build follows the record. */
	return (mode <= BOOT_MODE_INTERNAL) ? (enum boot_mode)mode : BOOT_MODE_AUTO;
}

/* AGM_BOOT_MODE_AUTO clears the one-shot: MODE 0 with the magic set is the
 * "follow the record" override, and once_take() consumes it either way. */
static int once_arm(enum boot_mode mode)
{
	int ret;

	/* Stale-proof order: clear the mode, set the magic, then
	 * the mode. A power loss in the middle therefore leaves either "not armed"
	 * or "armed AUTO" -- never a mode left over from an earlier arming, which
	 * is what the old mode-then-magic order could do. */
	ret = bkp_write(BKP_ONCE_MODE_IDX, 0);
	if (ret < 0) {
		return ret;
	}
	ret = bkp_write(BKP_ONCE_MAGIC_IDX, BKP_ONCE_MAGIC);
	if (ret < 0) {
		return ret;
	}
	ret = bkp_write(BKP_ONCE_MODE_IDX, (uint16_t)mode);
	if (ret < 0) {
		/* Do not leave it armed with the mode we just cleared. */
		(void)bkp_write(BKP_ONCE_MAGIC_IDX, 0);
	}
	return ret;
}

#else /* !BOOT_AGM_ONESHOT */

/* No backup domain to talk to: the interface stays honest and returns the
 * "nothing armed" answers, and the console's `once` reports it as
 * unavailable. */
static enum boot_mode once_take(void)
{
	return BOOT_MODE_AUTO;
}

static int once_arm(enum boot_mode mode)
{
	ARG_UNUSED(mode);
	return -ENOTSUP;
}

#endif /* BOOT_AGM_ONESHOT */

/* ---- boot record --------------------------------------------------- */

static uint32_t cfg_record_off(uint32_t record)
{
	return BOOT_CFG_OFF + record * BOOT_CFG_SECTOR;
}

/* The CRC covers everything before itself, so an entry is self-checking
 * without a length field. */
static uint32_t cfg_entry_crc(const struct boot_cfg *c)
{
	return crc32_ieee((const uint8_t *)c, offsetof(struct boot_cfg, crc));
}

static bool cfg_entry_ok(const struct boot_cfg *c)
{
	return c->magic == BOOT_CFG_MAGIC && c->version == BOOT_CFG_VERSION &&
	       c->seq != 0U && c->crc == cfg_entry_crc(c);
}

/* Read both sectors, take the newest CRC-valid entry (`seq` decides) and
 * remember where the log stands. Leaves `cfg` holding either the winner, the
 * raw first entry (so `info` can report a corrupt magic) or all 0xff (an
 * untouched record area). */
static int cfg_read(void)
{
	struct boot_cfg e;
	uint32_t used[BOOT_CFG_RECORDS] = { 0U, 0U };
	uint32_t best_seq = 0U;
	uint32_t best_sector = 0U;
	bool found = false;

	for (uint32_t r = 0U; r < BOOT_CFG_RECORDS; r++) {
		for (uint32_t off = 0U; off + sizeof(e) <= BOOT_CFG_SECTOR;
		     off += sizeof(e)) {
			int ret = flash_read(STORE_FLASH, cfg_record_off(r) + off, &e,
					     sizeof(e));

			if (ret < 0) {
				printk("loader: boot record read failed (%d)\n", ret);
				return ret;
			}
			if (!cfg_entry_ok(&e)) {
				break; /* entries are appended: the first bad one ends it */
			}
			used[r] = off + sizeof(e);
			if (!found || e.seq > best_seq) {
				cfg = e;
				best_seq = e.seq;
				best_sector = r;
				found = true;
			}
		}
	}

	if (found) {
		cfg_seq = best_seq;
		cfg_sector = best_sector;
		cfg_used = used[best_sector];
		cfg_fresh = false;
		return 0;
	}

	/* Nothing valid: an untouched record area, a corrupt one, or a record in
	 * an older layout. The last of those is worth a line of its own -- the
	 * application's A/B state is about to look like it was never there, and
	 * "version 3, not imported" is the whole explanation (see the note by
	 * struct boot_cfg). */
	if (flash_read(STORE_FLASH, BOOT_CFG_OFF, &e, sizeof(e)) < 0) {
		printk("loader: boot record read failed\n");
		return -EIO;
	}

	{
		uint32_t head[2] = { 0U, 0U };

		if (flash_read(STORE_FLASH, BOOT_CFG_OFF, head, sizeof(head)) == 0 &&
		    head[0] == BOOT_CFG_MAGIC && head[1] != BOOT_CFG_VERSION &&
		    head[1] != 0U && head[1] != 0xffffffffU) {
			printk("loader: boot record is version %u, which this build does not "
			       "import -- the next upload starts a fresh record\n",
			       (unsigned int)head[1]);
		}
	}

	if (e.magic == 0xffffffffU) {
		/* An erased area: `record_blank` is derived from this magic. */
		memset(&cfg, 0xff, sizeof(cfg));
	} else {
		cfg = e; /* keep the raw magic/version for the "invalid" report */
	}
	cfg_seq = 0U;
	cfg_sector = 0U;
	cfg_used = 0U;
	cfg_fresh = true;
	return 0;
}

/* Commit `c` as the next entry of the append log, recycling the other sector
 * when this one is full. The caller's copy gets the new seq/CRC so the live
 * state and the flash agree. */
static int cfg_write(struct boot_cfg *c)
{
	struct boot_cfg e;
	int ret;

	/* Nothing was appended to the current sector yet and what is there is
	 * unknown: start from erased flash (see cfg_fresh). */
	if (cfg_fresh) {
		ret = flash_erase(STORE_FLASH, cfg_record_off(cfg_sector), BOOT_CFG_SECTOR);
		if (ret < 0) {
			return ret;
		}
		cfg_fresh = false;
	}

	/* Written as a subtraction so it cannot overflow for any `cfg_used`;
	 * the assert pins the assumption that an entry never needs more than
	 * half a sector. */
	if (cfg_used > BOOT_CFG_SECTOR - sizeof(e)) {
		uint32_t next = 1U - cfg_sector;

		ret = flash_erase(STORE_FLASH, cfg_record_off(next), BOOT_CFG_SECTOR);
		if (ret < 0) {
			return ret;
		}
		cfg_sector = next;
		cfg_used = 0U;
	}

	c->seq = ++cfg_seq;
	c->crc = cfg_entry_crc(c);
	e = *c;

	ret = flash_write(STORE_FLASH, cfg_record_off(cfg_sector) + cfg_used, &e,
			  sizeof(e));
	if (ret < 0) {
		return ret;
	}
	cfg_used += sizeof(e);
	return 0;
}

/* Commit the in-memory record and report a failure. The boot policy must not
 * carry on with a record it could not write: the record is what every later
 * boot reads back, so a dropped write turns into "the rollback never
 * happened" (the rollback path ignored cfg_write()'s
 * result and kept booting the slot it had just decided was bad). */
static bool cfg_commit(const char *what)
{
	int ret = cfg_write(&cfg);

	if (ret < 0) {
		printk("loader: boot record write failed (%d) while %s -- staying in "
		       "the console\n", ret, what);
		return false;
	}
	return true;
}

static bool cfg_valid(void)
{
	return cfg.magic == BOOT_CFG_MAGIC && cfg.version == BOOT_CFG_VERSION;
}

/* The one field another unit asks the record for: the nonce mixer
 * (boot_agm_auth.c) hashes it in. Zero when there is no usable record, which is
 * what a freshly flashed board has. */
uint32_t boot_agm_record_seq(void)
{
	return cfg_valid() ? cfg.seq : 0U;
}

/* ---- on-die application slot --------------------------------------- */

static bool slot_is_blank(void)
{
	uint32_t head = 0;

	if (flash_read(INT_FLASH, SLOT_OFF, &head, sizeof(head)) < 0) {
		return true;
	}
	return head == 0xffffffffU;
}

/* Copy the payload stored in the external NOR into the on-die slot, so the
 * board can run it without the external flash in the boot path (XIP from the
 * slot). The external copy has already been CRC-checked by install(); this
 * re-checks it while streaming and then verifies the on-die content byte for
 * byte, because a silently bad program would only show up as a crash at the
 * next boot.
 */
static int payload_install_slot(void)
{
	if (!cfg_valid() || cfg.active >= BOOT_SLOTS) {
		printk("loader: no valid boot record -- run install first\n");
		return -ENOENT;
	}
	if (cfg.slot[cfg.active].load != SLOT_BASE) {
		printk("loader: store %c is a RAM image (load 0x%08x); the A/B path "
		       "uses on-die images\n", slot_letter(cfg.active),
		       cfg.slot[cfg.active].load);
		return -EINVAL;
	}
	/* An on-die DFU upload already runs from the slot: there is no store to
	 * copy from, and "copying" would erase the image and then program the
	 * *other* flash's store A over it before the CRC check noticed
	 * (`install-slot` after `upload slot` destroyed the
	 * image it was asked to install). */
	if (cfg.slot[cfg.active].src != CFG_SRC_STORE) {
		printk("loader: store %c already runs from the on-die slot -- "
		       "nothing to install\n", slot_letter(cfg.active));
		return -EINVAL;
	}

	int ret = store_write_slot(cfg.active);

	if (ret == 0) {
		printk("loader: on-die slot updated from store %c\n",
		       slot_letter(cfg.active));
	}
	return ret;
}

/* ---- jumping ------------------------------------------------------- */

static void jump_to(uint32_t entry)
{
	printk("loader: jumping to 0x%08x\n", entry);
	/* A test can claim the jump; a board never does. */
	if (agm_boot_jump_hook(entry)) {
		return;
	}
	/* No return path: interrupts off, then into the payload's entry. The
	 * payload is a Zephyr image linked for SRAM, so its own startup sets
	 * up the stack, trap vector and .data/.bss. */
	(void)irq_lock();
	((void (*)(void))entry)();
	CODE_UNREACHABLE;
}

/* Commit a bitstream that has been written into one of the two on-die slots:
 * write the boot record, which is what makes the *next* boot stream it. This
 * is the only thing that decides which fabric runs, and it is written last --
 * on purpose. A reset, a freeze or a power loss before this point leaves the
 * board on the image it is already running, and the factory slot is never
 * written at all (soc/agm/agrv2k/agm_bitstream.h has the incident that made
 * this the design). */
static int bitstream_commit(uint32_t slot, uint32_t len, uint32_t crc)
{
	struct agm_bitstream_record rec;
	uint32_t seq = bs_seq_next();
	/* Who is live *before* the record moves, so the console can say which
	 * slot was left alone -- reading it afterwards would name the slot that
	 * was just written (the message said the slot in
	 * use was untouched while pointing at the one the update had landed in). */
	uint32_t previous = bs_live_slot();
	int ret;

	if (slot != BS_SLOT1_ADDR && slot != BS_SLOT2_ADDR) {
		return -EINVAL;
	}
	if (!device_is_ready(INT_FLASH)) {
		return -ENODEV;
	}

	memset(&rec, 0, sizeof(rec));
	rec.magic = AGM_BITSTREAM_RECORD_MAGIC;
	rec.seq = seq;
	rec.slot = slot;
	rec.len = len;
	rec.crc = crc;
	rec.rec_crc = agm_bitstream_record_crc((const uint32_t *)&rec);

	ret = flash_erase(INT_FLASH, BS_RECORD_OFF, BOOT_CFG_SECTOR);
	if (ret < 0) {
		printk("loader: could not erase the bitstream record sector (%d) -- the "
		       "slot in use (0x%08x) still runs; re-run the same upload to "
		       "retry\n", ret, previous);
		return ret;
	}
	/* Two copies, 32 bytes apart: the reader takes the valid one with the
	 * higher sequence, so a torn write costs at most the newer copy. */
	for (uint32_t i = 0U; i < AGM_BITSTREAM_RECORD_COPIES; i++) {
		ret = flash_write(INT_FLASH,
				  BS_RECORD_OFF + i * AGM_BITSTREAM_RECORD_STRIDE,
				  &rec, sizeof(rec));
		if (ret < 0) {
			/* Copy 0 is the commit; copy 1 is the spare. Losing copy 1
			 * costs the newest entry's spare, losing copy 0 costs the
			 * update itself -- either way the slot that was running
			 * keeps running, so say what to do about it. */
			printk("loader: bitstream record copy %u failed (%d) -- %s; re-run "
			       "the same upload to retry\n", i, ret,
			       (i == 0U) ? "the update did not take effect"
					 : "the update is committed (spare copy lost)");
			return ret;
		}
	}

	printk("loader: bitstream committed to slot 0x%08x (%u B, crc 0x%08x, "
	       "seq %u)\r\n", slot, len, crc, seq);
	printk("loader: 'reboot' to load it -- the slot that was in use (0x%08x) and "
	       "the factory slot (0x%08x) were not touched\r\n",
	       previous, BS_ON_DIE);
	return 0;
}

/* Stage the embedded bitstream in the external flash (the upload stand-in),
 * then apply it. */

/* ---- A/B policy ---------------------------------------------------- */

static char slot_letter(uint32_t slot)
{
	return slot == 0U ? 'A' : 'B';
}

static const char *slot_state_name(uint32_t state)
{
	return agm_boot_slot_state_name((enum agm_boot_slot_state)state);
}

/* A store is bootable when it has metadata and its execution target is the
 * on-die slot (the RAM target is the legacy `mode external` path). */
static bool store_usable(uint32_t slot)
{
	return cfg_valid() && slot < BOOT_SLOTS &&
	       (cfg.slot[slot].state == SLOT_TRIAL || cfg.slot[slot].state == SLOT_CONFIRMED) &&
	       cfg.slot[slot].ext_len > 0U && cfg.slot[slot].ext_crc != 0U &&
	       cfg.slot[slot].load == SLOT_BASE;
}

/* Make the on-die slot hold store[slot], then report what it cost. */
static int store_write_slot(uint32_t slot)
{
	uint8_t buf[SLOT_CHUNK];
	uint32_t len = cfg.slot[slot].ext_len;
	uint32_t crc = 0;
	int ret;

	/* This is "store -> slot": an image the record marks as already being
	 * in the slot has nothing to copy, and erasing the slot for it would
	 * destroy it. */
	if (slot >= BOOT_SLOTS || cfg.slot[slot].src != CFG_SRC_STORE) {
		return -EINVAL;
	}
	if (len == 0U || len > SLOT_MAX) {
		printk("loader: store %c length %u does not fit the slot\n",
		       slot_letter(slot), len);
		return -EINVAL;
	}
	if (!device_is_ready(INT_FLASH)) {
		return -ENODEV;
	}

	printk("loader: store %c -> on-die slot 0x%08x (%u B, crc 0x%08x)\n",
	       slot_letter(slot), SLOT_BASE, len, cfg.slot[slot].ext_crc);

	for (uint32_t done = 0; done < len; done += BOOT_CFG_SECTOR) {
		ret = flash_erase(INT_FLASH, SLOT_OFF + done, BOOT_CFG_SECTOR);
		if (ret < 0) {
			return ret;
		}
	}
	for (uint32_t done = 0; done < len; done += SLOT_CHUNK) {
		uint32_t chunk = MIN(SLOT_CHUNK, len - done);
		uint32_t aligned = ROUND_UP(chunk, 4U);

		ret = flash_read(STORE_FLASH, cfg.slot[slot].ext_off + done, buf, chunk);
		if (ret < 0) {
			return ret;
		}
		crc = crc32_ieee_update(crc, buf, chunk);
		for (uint32_t i = chunk; i < aligned; i++) {
			buf[i] = 0xffU;
		}
		ret = flash_write(INT_FLASH, SLOT_OFF + done, buf, aligned);
		if (ret < 0) {
			return ret;
		}
	}
	if (crc != cfg.slot[slot].ext_crc) {
		printk("loader: store %c CRC mismatch (0x%08x vs 0x%08x)\n",
		       slot_letter(slot), crc, cfg.slot[slot].ext_crc);
		return -EIO;
	}
	return 0;
}

/* CRC of what currently sits in the on-die slot, so a store that is already
 * there does not get rewritten on every boot (flash wear + boot time). */
static uint32_t slot_content_crc(uint32_t len)
{
	uint8_t buf[SLOT_CHUNK];
	uint32_t crc = 0;

	for (uint32_t done = 0; done < len; done += SLOT_CHUNK) {
		uint32_t chunk = MIN(SLOT_CHUNK, len - done);

		if (flash_read(INT_FLASH, SLOT_OFF + done, buf, chunk) < 0) {
			return 0U;
		}
		crc = crc32_ieee_update(crc, buf, chunk);
	}
	return crc;
}

/* ---- trial-boot watchdog -------------------------------------------- */

/*
 * The handshake itself lives in the public header (the application has to be
 * able to answer it); this is the loader's half:
 *
 *   - boot: report a reset the watchdog caused, apply a confirmation that the
 *     previous image left behind, and make sure no watchdog is still counting
 *     from that boot (the backup domain survives a reset, so one is);
 *   - right before jumping into a TRIAL image: arm it and hand out the ticket.
 *
 * Arming is the only place that touches the WDT API, and it happens last, so
 * the console path can never be reset by a watchdog the loader itself armed.
 */
/* ---- trial-watchdog hooks ------------------------------------------- */

/*
 * The arm/confirm chain touches three things that do not exist under
 * native_sim: the backup-domain registers (through the header's inline
 * helpers), the IWDG driver, and the SoC reset-cause register. With the
 * accesses inlined, none of the chain could be covered by the native suite --
 * the only evidence was a dev board session .
 *
 * Each access therefore goes through a weak function. The defaults below are
 * exactly what the code did before, so a board build is unchanged; a test
 * overrides them with a RAM stand-in and can drive the whole chain. The
 * application side keeps the header's inline helpers -- an application must
 * not have to link the bootloader to answer the handshake -- and a test
 * stands in for it by writing the same register the helper would.
 */
__weak int agm_boot_bkp_read(uint32_t idx, uint16_t *val)
{
	*val = agm_boot_trial_reg_read(AGM_RTC_BKP_DR(idx));
	return 0;
}

__weak int agm_boot_bkp_write(uint32_t idx, uint16_t val)
{
	return agm_boot_trial_reg_write(AGM_RTC_BKP_DR(idx), val);
}

/* Last thing before control leaves the bootloader. Returning true means "the
 * jump was taken care of" -- only a test does that, so the code after it runs
 * there instead of executing the payload. */
__weak bool agm_boot_jump_hook(uint32_t entry)
{
	ARG_UNUSED(entry);
	return false;
}

#if BOOT_TRIAL_WATCHDOG

/* The reset cause of this boot, cleared as it is read (the flags describe the
 * reset that produced this boot, not the previous one). */
__weak uint32_t agm_boot_reset_cause_take(void)
{
#if BOOT_TRIAL_HAVE_RST_CAUSE
	uint32_t rst = sys_read32(AGM_SYS_BASE + AGM_SYS_RST_CNTL);

	sys_write32(rst | AGM_SYS_RST_REMOVE, AGM_SYS_BASE + AGM_SYS_RST_CNTL);
	return rst;
#else
	return 0U;
#endif
}

__weak int agm_boot_trial_watchdog_set(bool on)
{
	if (!on) {
		return agm_boot_trial_watchdog_off();
	}
	{
		struct wdt_timeout_cfg cfg = {
			.window.min = 0U,
			.window.max = CONFIG_BOOT_AGM_TRIAL_WATCHDOG_TIMEOUT_MS,
			.callback = NULL,
			.flags = WDT_FLAG_RESET_SOC,
		};

		if (!device_is_ready(trial_wdt)) {
			return -ENODEV;
		}
		if (wdt_install_timeout(trial_wdt, &cfg) != 0) {
			return -EIO;
		}
		return (wdt_setup(trial_wdt, 0) != 0) ? -EIO : 0;
	}
}

static void trial_watchdog_report_reset(void)
{
	/* The cause *bit* is the SoC's (agm_sys.h), so the reporting only exists
	 * where that header does; a simulated target still consumes the hook so
	 * a test can drive it. */
#if BOOT_TRIAL_HAVE_RST_CAUSE
	uint32_t rst = agm_boot_reset_cause_take();

	if ((rst & AGM_SYS_RSTF_IWDG) != 0U) {
		printk("loader: the previous boot was reset by the trial watchdog "
		       "(the image did not confirm in time)\r\n");
	}
#else
	(void)agm_boot_reset_cause_take();
#endif
}

static void trial_watchdog_arm(void)
{
	if (agm_boot_trial_watchdog_set(true) != 0) {
		printk("loader: no trial watchdog on this build -- a trial image that "
		       "hangs will need a reset by hand\r\n");
	}
	/* After arming: the IWDG setup path pulses the backup-domain reset latch,
	 * which clears the backup registers this has to land in. */
	if (agm_boot_bkp_write(AGM_BOOT_TRIAL_TICKET_DR,
			       agm_boot_trial_ticket_encode(cfg_seq)) < 0) {
		printk("loader: could not write the trial ticket\r\n");
	}
}

/* Did the image that ran since the last boot say it was fine? The ticket
 * carries the commit sequence the record had when the loader handed it out, so
 * a marker left over from an older image cannot confirm a newer upload (every
 * upload commits a new entry, hence a new sequence). */
static bool trial_confirm_pending(void)
{
	uint16_t ticket = 0U;
	uint16_t echo = 0U;

	if (agm_boot_bkp_read(AGM_BOOT_TRIAL_TICKET_DR, &ticket) < 0 ||
	    agm_boot_bkp_read(AGM_BOOT_TRIAL_CONFIRM_DR, &echo) < 0) {
		return false;
	}
	/* Both sides of this comparison go through the same encoder the arm path
	 * uses: the value that would collide with "no ticket is pending" (0) can
	 * therefore never be armed or accepted. */
	return ticket != 0U && agm_boot_trial_ticket_encode(cfg_seq) == ticket &&
	       echo == ticket;
}

static void trial_confirm_apply(void)
{
	if (!trial_confirm_pending()) {
		return;
	}
	if (cfg_valid() && cfg.active < BOOT_SLOTS &&
	    cfg.slot[cfg.active].state == SLOT_TRIAL) {
		cfg.slot[cfg.active].state = SLOT_CONFIRMED;
		cfg.slot[cfg.active].attempts = 0U;
		if (cfg_write(&cfg) == 0) {
			printk("loader: store %c confirmed itself on its last boot\r\n",
			       slot_letter(cfg.active));
		}
	}
	/* Either way the answer has been acted on: do not let it be read again. */
	(void)agm_boot_bkp_write(AGM_BOOT_TRIAL_CONFIRM_DR, 0U);
	(void)agm_boot_bkp_write(AGM_BOOT_TRIAL_TICKET_DR, 0U);
}

#else /* !BOOT_TRIAL_WATCHDOG */

/* Nothing to arm or silence: this build has no trial watchdog at all. */
static int agm_boot_trial_watchdog_set(bool on)
{
	ARG_UNUSED(on);
	return -ENODEV;
}

static void trial_watchdog_report_reset(void) { }
static void trial_confirm_apply(void) { }
static void trial_watchdog_arm(void) { }

#endif /* BOOT_TRIAL_WATCHDOG */

/* Everything that has to be settled before any boot path runs: a trial image
 * may have answered the handshake on its last boot, and a trial boot arms a
 * watchdog that outlives the reset. Both entries into the boot path -- the
 * console/policy one below and the raw `agm_boot_boot()` -- call this, so the
 * chain behaves the same either way (the native tests in
 * tests/drivers/misc/boot_agm cover exactly that).
 *
 * Clearing the handshake here also means no handshake is outstanding until an
 * arming below writes one: without it, the ticket a failed trial left behind
 * would still be readable by whatever boots next (dev board: the
 * confirmed slot's image answered a leftover ticket and reported "confirmed"
 * for a boot that was not a trial at all). The record is protected either way
 * -- a promotion needs the ticket to match the current commit sequence -- but
 * the application has to be able to trust `ticket == 0`.
 */
static void settle_before_boot(void)
{
	trial_watchdog_report_reset();
	trial_confirm_apply();
	(void)agm_boot_trial_watchdog_set(false);
	(void)agm_boot_bkp_write(AGM_BOOT_TRIAL_CONFIRM_DR, 0U);
	(void)agm_boot_bkp_write(AGM_BOOT_TRIAL_TICKET_DR, 0U);
}

/* Is the image slot[slot] points at still the one the record published?
 *
 * store_boot() asks right before the jump; boot_ab_policy() asks before it
 * spends a trial attempt, so media damage cannot burn a slot's whole trial
 * budget. Both paths need the same answer, so it lives
 * here rather than inline in either one. */
static int verify_slot_image(uint32_t slot)
{
	enum agm_boot_target src = (cfg.slot[slot].src == CFG_SRC_ON_DIE)
				   ? AGM_BOOT_TARGET_SLOT
				   : ((slot == 0U) ? AGM_BOOT_TARGET_STORE_A
						   : AGM_BOOT_TARGET_STORE_B);

	return agm_boot_image_verify(NULL, src, cfg.slot[slot].ext_len);
}

/* Boot store[slot]: copy it into the on-die slot if that is stale, then jump.
 * Never returns on success. */
static int store_boot(uint32_t slot)
{
	uint32_t len = cfg.slot[slot].ext_len;

	/* Re-verify what we are about to run: the store -- or the slot -- may have
	 * been rewritten after this image was published. */
	{
		int vret = verify_slot_image(slot);

		if (vret < 0) {
			printk("loader: store %c is no longer acceptable (%d) -- not booting it\n",
			       slot_letter(slot), vret);
			return vret;
		}
	}

	if (!device_is_ready(INT_FLASH)) {
		return -ENODEV;
	}

	if (cfg.slot[slot].src == CFG_SRC_ON_DIE) {
		/* The DFU wrote the on-die slot directly: there is nothing to
		 * copy, only the image to verify (which also catches a flash
		 * that lost its content since the upload). */
		uint32_t crc = 0U;
		int ret = agm_boot_upload_crc(NULL, AGM_BOOT_TARGET_SLOT, 0U, len, &crc);

		if (ret < 0) {
			return ret;
		}
		if (crc != cfg.slot[slot].ext_crc) {
			printk("loader: on-die image CRC mismatch (0x%08x vs 0x%08x)\n",
			       crc, cfg.slot[slot].ext_crc);
			return -EIO;
		}
		printk("loader: on-die image verified (%u B, crc 0x%08x)\n", len, crc);
	} else if (!device_is_ready(STORE_FLASH)) {
		return -ENODEV;
	} else if (slot_content_crc(len) != cfg.slot[slot].ext_crc) {
		int ret = store_write_slot(slot);

		if (ret < 0) {
			printk("loader: store %c could not be applied (%d)\n",
			       slot_letter(slot), ret);
			return ret;
		}
	} else {
		printk("loader: store %c already in the on-die slot\n",
		       slot_letter(slot));
	}

	printk("loader: entering store %c (%s, attempt %u/%u)\n", slot_letter(slot),
	       slot_state_name(cfg.slot[slot].state), cfg.slot[slot].attempts,
	       BOOT_MAX_ATTEMPTS);
	/* Last thing before the jump: a trial image gets a watchdog and the ticket
	 * its successor has to echo (see the trial-boot section above). Nothing
	 * after this point may print, or it would be lost with the jump. */
	if (cfg.slot[slot].state == SLOT_TRIAL) {
		trial_watchdog_arm();
	}
	jump_to(cfg.slot[slot].entry == 0U ? SLOT_BASE : cfg.slot[slot].entry);
	return 0;
}

/* store_boot() never returns on success (it jumps), so every return from it is
 * a failure -- say so instead of quietly carrying on. */
static void store_boot_or_report(uint32_t slot)
{
	int ret = store_boot(slot);

	if (ret < 0) {
		printk("loader: store %c could not be booted (%d)\n", slot_letter(slot), ret);
	}
}

/* The A/B policy: decide which store to boot, spend a trial attempt, roll back.
 * Returns false when nothing bootable was found (the console takes over). */
static bool boot_ab_policy(void)
{
	uint32_t slot = cfg_valid() ? cfg.active : 0U;

	if (slot >= BOOT_SLOTS) {
		slot = 0U;
	}

	/* A store the policy has already given up on must never be streamed or
	 * jumped into; that is what store_usable() encodes (SLOT_BAD is not
	 * usable), so the fallback below covers it in one place -- stated here
	 * because this used to depend on that side effect without saying so. */
	if (!store_usable(slot)) {
		uint32_t other = 1U - slot;

		if (!store_usable(other)) {
			printk("loader: no usable store (A: %s, B: %s)\n",
			       cfg_valid() ? slot_state_name(cfg.slot[0].state) : "invalid",
			       cfg_valid() ? slot_state_name(cfg.slot[1].state) : "invalid");
			return false;
		}
		printk("loader: active store %c is not usable, falling back to %c\n",
		       slot_letter(slot), slot_letter(other));
		slot = other;
		cfg.active = slot;
		if (!cfg_commit("switching the active store")) {
			return false;
		}
	}

	/* A trial has to earn its place: spend one attempt *before* the jump so
	 * a power cycle (not just a clean reset) counts. */
	if (cfg.slot[slot].state == SLOT_TRIAL) {
		/* ... but only for an image the flash can still deliver: a store
		 * that no longer verifies is media damage, not a failed trial,
		 * and spending an attempt on it would let the damage exhaust the
		 * slot's budget and mark a *good* record BAD.
		 * store_boot_or_report() prints why and returns to the console;
		 * the record is left untouched. */
		if (verify_slot_image(slot) < 0) {
			store_boot_or_report(slot);
			return false;
		}

		cfg.slot[slot].attempts++;

		/* The *last* attempt is committed together with the outcome it implies
		 * (BAD + rollback), not as its own "attempt N/M" record first: two
		 * records leave a window in which a failing record write keeps the
		 * store in TRIAL with attempts == MAX, so every later boot spends
		 * another attempt and writes again -- a cross-reboot increment nothing
		 * currently owns. */
		if (cfg.slot[slot].attempts >= BOOT_MAX_ATTEMPTS) {
			uint32_t other = 1U - slot;

			cfg.slot[slot].state = SLOT_BAD;
			printk("loader: store %c exceeded %u attempts -> BAD\n",
			       slot_letter(slot), BOOT_MAX_ATTEMPTS);

			if (store_usable(other)) {
				cfg.active = other;
				if (!cfg_commit("rolling back")) {
					return false;
				}
				printk("loader: rolling back to store %c\n",
				       slot_letter(other));
				store_boot_or_report(other);
				return false;
			}
			if (!cfg_commit("marking the active store bad")) {
				return false;
			}
			printk("loader: no fallback store; staying in the console\n");
			return false;
		}

		if (!cfg_commit("recording a trial attempt")) {
			return false;
		}
		printk("loader: store %c trial attempt %u/%u\n", slot_letter(slot),
		       cfg.slot[slot].attempts, BOOT_MAX_ATTEMPTS);
	}

	store_boot_or_report(slot);
	return false;
}

/* ---- upload core ---------------------------------------------------- */

/*
 * One generic write path for all four targets: the driver erases whatever of
 * the range is not known to be erased yet, writes a 4-byte-aligned prefix and
 * keeps the sub-word tail for the next chunk (the SPI NOR driver programs
 * 4 bytes at a time, hosts hand over any chunk size).
 */

static int target_write(enum agm_boot_target target, uint32_t off, const uint8_t *data,
			uint32_t len)
{
	struct boot_agm_upload *up = &ups[target];
	uint32_t max = agm_boot_upload_max(NULL, target);
	uint32_t base;
	uint32_t abs;
	uint32_t a;

	/* Callers validate, this one does not trust them: `off` reaches here
	 * from a host frame, and a wrapped `base + off` used to land the erase
	 * on sector 0 (the boot record) instead of failing. */
	if (off > max || len > max - off) {
		return -EINVAL;
	}
	base = agm_boot_target_offset(NULL, target);
	abs = base + off;
	a = abs & ~(BOOT_CFG_SECTOR - 1U);

	if (up->erased_end > a) {
		a = up->erased_end;
	}
	for (; a < abs + len; a += BOOT_CFG_SECTOR) {
		int ret = flash_erase(target_flash(target), a, BOOT_CFG_SECTOR);

		if (ret < 0) {
			return ret;
		}
	}
	up->erased_end = a;
	return flash_write(target_flash(target), abs, data, len);
}

int boot_agm_target_read(enum agm_boot_target target, uint32_t off, uint8_t *buf,
			 uint32_t len)
{
	return flash_read(target_flash(target), agm_boot_target_offset(NULL, target) + off,
			  buf, len);
}

/* Is [off, off + len) inside what @a target can hold? Spelled out so that a
 * host cannot pass the check by wrapping `off + len`: with off = 0xfffffffc
 * and len = 4 the old `off + len > max` was false, and the write then landed
 * on a wrapped flash offset (sector 0 = the boot record) instead of failing. */
static bool target_range_ok(const struct device *dev, enum agm_boot_target target,
			    uint32_t off, uint32_t len)
{
	uint32_t max;

	if (target >= AGM_BOOT_TARGET_COUNT) {
		return false;
	}
	max = agm_boot_upload_max(dev, target);
	return len <= max && off <= max - len;
}

int agm_boot_upload_begin(const struct device *dev, enum agm_boot_target target)
{
	ARG_UNUSED(dev);

	if (target >= AGM_BOOT_TARGET_COUNT) {
		return -EINVAL;
	}

	/* the signed-command gate: opening a write session is itself a
	 * state change, gate included.
	 * The first chunk erases the target's sectors from scratch (`erased_end`
	 * starts at zero), and for a store that destroys the image the record
	 * still points at -- the rollback copy. Gating only the publish would let
	 * an unauthenticated host wipe a published store *without* leaving a
	 * trace in the record, which is exactly what `erase` is gated against;
	 * the command that guards it is the same one (AGM_BOOT_AUTH_CMD_ERASE).
	 * Nothing has been touched when this refuses: the session never opens, so
	 * the writes that follow fail with -EINVAL and the flash keeps its
	 * content. */
	if (!boot_agm_auth_take(AGM_BOOT_AUTH_CMD_ERASE, "open an upload to",
		       agm_boot_target_name(target))) {
		return AGM_BOOT_E_UNAUTHORIZED;
	}

	/* Fresh upload: every sector is erased as it is written, and the write
	 * watermark restarts (finish()/GO publish it). */
	memset(&ups[target], 0, sizeof(ups[target]));
	ups[target].active = true;
	if (target == AGM_BOOT_TARGET_BITSTREAM) {
		ups[target].slot_off = BS_SLOT_OFF(bs_inactive_slot());
	}
	return 0;
}

void agm_boot_upload_abort(const struct device *dev, enum agm_boot_target target)
{
	ARG_UNUSED(dev);

	if (target < AGM_BOOT_TARGET_COUNT) {
		memset(&ups[target], 0, sizeof(ups[target]));
	}
}

/* One internal chunk: at most AGM_BOOT_UPLOAD_CHUNK bytes, so the join buffer
 * and the carry always fit. Public callers may hand over more. */
static int upload_write_step(const struct device *dev, enum agm_boot_target target,
			     uint32_t off, const void *data, uint32_t len)
{
	struct boot_agm_upload *up;
	const uint8_t *src = data;
	uint32_t total = len;
	uint32_t aligned;
	int ret;

	if (len > AGM_BOOT_UPLOAD_CHUNK || !target_range_ok(dev, target, off, len)) {
		return -EINVAL;
	}

	up = &ups[target];
	/* Writes have to move forward within an upload: a host that skips ahead
	 * would leave a hole that the read-back CRC then blesses (finish()
	 * stores the CRC of what it read, gaps included), and one that rewrites
	 * an earlier offset would program over already-programmed NOR, which
	 * only clears bits. A fresh upload starts with upload_begin(), which
	 * resets the watermark. */
	if (up->len != 0U && up->carry_len == 0U && off != up->len) {
		return -EINVAL;
	}
	if (up->carry_len != 0U) {
		/* Continue where the previous chunk stopped. */
		if (off != up->carry_off + up->carry_len) {
			return -EINVAL;
		}
		memcpy(join, up->carry, up->carry_len);
		memcpy(join + up->carry_len, data, len);
		off = up->carry_off;
		src = join;
		total = up->carry_len + len;
		up->carry_len = 0U;
	}

	aligned = total & ~3U;
	if (aligned != 0U) {
		ret = target_write(target, off, src, aligned);
		if (ret < 0) {
			return ret;
		}
	}
	if (total > aligned) {
		up->carry_len = total - aligned;
		up->carry_off = off + aligned;
		memcpy(up->carry, src + aligned, up->carry_len);
	}

	/* `total`, not `len`: with a carry consumed above, `off` is the start of
	 * the joined buffer, so the end of what has been written is off + total
	 * (the old off + len lagged up to 3 bytes behind, which nothing noticed
	 * until the forward-progress check started using this watermark). */
	if (off + total > up->len) {
		up->len = off + total;
	}
	return 0;
}

int agm_boot_upload_write(const struct device *dev, enum agm_boot_target target,
			  uint32_t off, const void *data, uint32_t len)
{
	const uint8_t *p = data;

	if (!target_range_ok(dev, target, off, len)) {
		return -EINVAL;
	}

	/* Hosts and the sample's own installers hand over whatever chunk size
	 * suits them; split it into the size the carry/join buffers handle. */
	while (len != 0U) {
		uint32_t step = MIN(len, AGM_BOOT_UPLOAD_CHUNK);
		int ret = upload_write_step(dev, target, off, p, step);

		if (ret < 0) {
			return ret;
		}
		p += step;
		off += step;
		len -= step;
	}
	return 0;
}

/* Flush a trailing partial word (the sector was just erased, so padding with
 * 0xff writes the same value back; the CRC below only covers `len`). */
static int upload_flush_carry(enum agm_boot_target target)
{
	uint8_t pin[4] = { 0xFFU, 0xFFU, 0xFFU, 0xFFU };
	struct boot_agm_upload *up = &ups[target];

	if (up->carry_len == 0U) {
		return 0;
	}
	memcpy(pin, up->carry, up->carry_len);
	up->carry_len = 0U;
	return target_write(target, up->carry_off, pin, sizeof(pin));
}

int agm_boot_read(const struct device *dev, enum agm_boot_target target, uint32_t off,
		  void *buf, uint32_t len)
{
	ARG_UNUSED(dev);

	if (!target_range_ok(dev, target, off, len)) {
		return -EINVAL;
	}
	return boot_agm_target_read(target, off, buf, len);
}

int agm_boot_upload_crc(const struct device *dev, enum agm_boot_target target,
			uint32_t off, uint32_t len, uint32_t *crc)
{
	uint8_t buf[SLOT_CHUNK];
	uint32_t acc = 0U;

	ARG_UNUSED(dev);

	if (!target_range_ok(dev, target, off, len)) {
		return -EINVAL;
	}

	for (uint32_t done = 0U; done < len; done += SLOT_CHUNK) {
		uint32_t chunk = MIN(SLOT_CHUNK, len - done);
		int ret = boot_agm_target_read(target, off + done, buf, chunk);

		if (ret < 0) {
			return ret;
		}
		acc = crc32_ieee_update(acc, buf, chunk);
	}
	*crc = acc;
	return 0;
}

/* Publish an uploaded image: the A/B stores and the on-die slot become TRIAL +
 * active, the bitstream target is staged (BSB1 header) and applied to the
 * on-die reservation right away. */
static int publish_upload(enum agm_boot_target target, uint32_t len, uint32_t crc,
			  uint32_t load, uint32_t entry)
{
	uint32_t slot = target;
	uint32_t src = CFG_SRC_STORE;

	/* the signed-command gate: in the locked profile publishing is a
	 * state change like any
	 * other, so it needs the signed command -- checked *before* the branch
	 * below, because the bitstream target commits the fabric from this
	 * same function and would otherwise be the one way past the gate.
	 * Nothing has been touched yet at this point: the bytes are in the
	 * store (the host put them there), the record is not. */
	if (!boot_agm_auth_take(AGM_BOOT_AUTH_CMD_PUBLISH, "publish",
		       agm_boot_target_name(target))) {
		return AGM_BOOT_E_UNAUTHORIZED;
	}

	if (target == AGM_BOOT_TARGET_BITSTREAM) {
		uint32_t slot = bs_session_slot_addr();

		if (len == 0U || len > BS_ON_DIE_MAX) {
			return -EINVAL;
		}
		/* The image is already in the inactive slot and the caller has
		 * read it back for its CRC (that is what FINISH does), so all
		 * that is left is to check it and commit it. Nothing here
		 * touches the slot that is running -- see agm_bitstream.h.
		 *
		 * With CONFIG_BOOT_AGM_BITSTREAM_SIGNED the check is the fabric
		 * profile: the record is what makes the next boot stream this
		 * slot, so a slot that is not a properly signed container must
		 * never reach it (the boot path verifies again, but an upload
		 * that could never boot should not be committed either). */
		{
			int vret = agm_boot_bitstream_verify(slot, len, crc);

			if (vret < 0) {
				printk("loader: refusing to publish the bitstream: "
				       "image not accepted (%d)\n", vret);
				return vret;
			}
		}
		return bitstream_commit(slot, len, crc);
	}

	/* A signed build refuses an image that is not a properly signed container
	 * before it touches the record: the bytes may stay in the store, but with no
	 * record entry they can never be booted. (The bitstream target returned
	 * above -- the fabric payload is not an application image.) */
	{
		int vret = agm_boot_image_verify(NULL, target, len);

		if (vret < 0) {
			printk("loader: refusing to publish %s: image not accepted (%d)\n",
			       agm_boot_target_name(target), vret);
			return vret;
		}
	}

#if defined(CONFIG_BOOT_AGM_ANTI_ROLLBACK)
	/* Anti-rollback: a genuine but *older* container is still a rollback, so
	 * compare its version against the floor the record carries (zero when
	 * nothing signed has been accepted yet). Equal is fine -- re-installing
	 * the same release is a normal recovery move. The floor itself is raised
	 * below, after the record has been made valid -- a first upload runs the
	 * memset-a-fresh-record path in between, which would drop it here.
	 *
	 * The comparison is gated on the *floor*, not on the candidate: 0.0.0 is
	 * a version like any other, and gating it on `cand_ver != 0U` (which is
	 * what the first cut did) left exactly that one version -- the oldest
	 * there is -- accepted over any floor . "Could
	 * not read the version" is not a case this has to carry:
	 * a build with the floor compiled in only ever gets here with a
	 * container whose header the verifier above has already accepted. */
	uint32_t cand_ver = 0U;

	{
		uint32_t ver = 0U;

		if (boot_agm_image_version_read(target, &ver) == 0) {
			cand_ver = ver;
		}
		if (cfg_valid() && cfg.sec_ver != 0U && cand_ver < cfg.sec_ver) {
			printk("loader: refusing an older image (v%u.%u.%u < v%u.%u.%u "
			       "already installed)\n", IMG_VER_MAJOR(cand_ver),
			       IMG_VER_MINOR(cand_ver), IMG_VER_REV(cand_ver),
			       IMG_VER_MAJOR(cfg.sec_ver),
			       IMG_VER_MINOR(cfg.sec_ver), IMG_VER_REV(cfg.sec_ver));
			return AGM_BOOT_E_OLD_VERSION;
		}
	}
#endif

	/* Containers run at slot + header size, and the header's own
	 * `ih_load_addr` has to say so (boot_agm_image_container_entry() checks that).
	 * Hosts that pass the slot base -- the default everywhere -- or nothing
	 * get the entry derived from the header; a host that insists on some
	 * other entry is overruled by the same check, because it can only be
	 * right for an image this layout would refuse anyway.
	 *
	 * This runs *before* the host's load/entry are validated, and that order
	 * is the point: for a container this layout would run, the header is
	 * what decides, so a host that sends zeroes -- the "or nothing" above --
	 * must not be refused by a rule that only exists for raw images.
	 * A host sending (0, 0) with a genuine `imgtool`-signed container is
	 * rejected with "refusing load 0x00000000 entry 0x00000000 ... (not a
	 * word-aligned image inside SRAM)" before the header was ever read,
	 * which is the opposite of what the paragraph above promises. Raw images
	 * (the NONE profile: -ENOTSUP) keep the old rule. */
	{
		uint32_t derived;
		int dret = boot_agm_image_container_entry(target, &derived);

		if (dret == 0) {
			if (entry != 0U && entry != SLOT_BASE && entry != derived) {
				printk("loader: the host asked for entry 0x%08x but the "
				       "image header says 0x%08x\r\n", entry, derived);
				return -EINVAL;
			}
			entry = derived;
			load = SLOT_BASE;
		} else if (dret != -ENOTSUP) {
			/* A container the loader cannot place: the load address
			 * does not match this layout (message printed above). */
			return dret;
		} else if (load == SLOT_BASE) {
			/* A raw image for the on-die slot: the record is what the
			 * boot path jumps to, so the host's entry has to make
			 * sense here. `entry == 0` keeps its record meaning of
			 * "the slot base" (see store_boot()). */
			if (entry != 0U &&
			    (!IS_ALIGNED(entry, 4U) || entry < SLOT_BASE ||
			     entry >= SLOT_BASE + len)) {
				printk("loader: refusing entry 0x%08x for a %u B slot image\n",
				       entry, len);
				return -EINVAL;
			}
		} else {
			/* Anything that is neither a container nor a raw image
			 * for the on-die slot has nowhere to run: the SRAM
			 * (RAM-image) path was removed with `mode external`. */
			printk("loader: refusing load 0x%08x entry 0x%08x len %u "
			       "(only the on-die slot 0x%08x is a valid target)\n",
			       load, entry, len, (uint32_t)SLOT_BASE);
			return -EINVAL;
		}
	}

	if (target == AGM_BOOT_TARGET_SLOT) {
		/* The on-die slot is the A side of the A/B pair: a bad on-die
		 * image still rolls back to whatever store B holds. */
		slot = AGM_BOOT_TARGET_STORE_A;
		src = CFG_SRC_ON_DIE;
	}

	if (slot >= BOOT_SLOTS || len == 0U ||
	    len > ((src == CFG_SRC_ON_DIE) ? SLOT_MAX : SLOT_STRIDE)) {
		return -EINVAL;
	}

	if (!cfg_valid()) {
		memset(&cfg, 0, sizeof(cfg));
		cfg.magic = BOOT_CFG_MAGIC;
		cfg.version = BOOT_CFG_VERSION;
		cfg.mode = BOOT_MODE_AUTO;
		cfg.slot[0].ext_off = BOOT_STORE_A;
		cfg.slot[1].ext_off = BOOT_STORE_B;
	}
	cfg.slot[slot].src = src;
	/* For an on-die upload the record entry points at the on-die slot, not at
	 * an external store: `info` reports this field, and handing it back
	 * BOOT_STORE_A made it name an address the image is not at. Everything that
	 * *reads* an image goes through `src`, so this
	 * only has to be truthful, not load-bearing. */
	cfg.slot[slot].ext_off = (src == CFG_SRC_ON_DIE)
					 ? SLOT_OFF
					 : ((slot == AGM_BOOT_TARGET_STORE_A) ? BOOT_STORE_A
									      : BOOT_STORE_B);
	cfg.slot[slot].ext_len = len;
	cfg.slot[slot].ext_crc = crc;
	cfg.slot[slot].load = load;
	cfg.slot[slot].entry = entry;
	cfg.slot[slot].state = SLOT_TRIAL;
	cfg.slot[slot].attempts = 0U;
	cfg.active = slot;

#if defined(CONFIG_BOOT_AGM_ANTI_ROLLBACK)
	/* The floor only rises: this image was accepted, so later uploads have to
	 * be at least this new (see the check above). */
	if (cand_ver > cfg.sec_ver) {
		cfg.sec_ver = cand_ver;
	}
#endif

	int ret = cfg_write(&cfg);

	if (ret < 0) {
		return ret;
	}
	if (src == CFG_SRC_ON_DIE) {
		printk("\r\nloader: on-die slot uploaded (%u B, crc 0x%08x) -> "
		       "TRIAL/active\r\n", len, crc);
	} else {
		printk("\r\nloader: store %c uploaded (%u B, crc 0x%08x) -> "
		       "TRIAL/active\r\n", slot_letter(slot), len, crc);
	}
	printk("loader: 'confirm' once it proves itself, else %u failed boots roll back\r\n",
	       BOOT_MAX_ATTEMPTS);
	return 0;
}

int agm_boot_upload_finish_at(const struct device *dev, enum agm_boot_target target,
			      uint32_t len, uint32_t load, uint32_t entry)
{
	uint32_t crc = 0U;
	int ret;

	if (target >= AGM_BOOT_TARGET_COUNT || len == 0U) {
		return -EINVAL;
	}

	ret = upload_flush_carry(target);
	if (ret < 0) {
		return ret;
	}
	ret = agm_boot_upload_crc(dev, target, 0U, len, &crc);
	if (ret < 0) {
		return ret;
	}
	return publish_upload(target, len, crc, load, entry);
}

int agm_boot_upload_finish(const struct device *dev, enum agm_boot_target target,
			   uint32_t len)
{
	return agm_boot_upload_finish_at(dev, target, len, SLOT_BASE, SLOT_BASE);
}

/* ---- record / state ------------------------------------------------- */

static enum agm_boot_mode once_peek(void)
{
#if BOOT_AGM_ONESHOT
	if (RTC_BKP_DR(BKP_ONCE_MAGIC_IDX) != BKP_ONCE_MAGIC) {
		return BOOT_MODE_AUTO;
	}
	uint16_t mode = RTC_BKP_DR(BKP_ONCE_MODE_IDX);

	/* As in once_take(): a leftover `external` (2) reads as AUTO. */
	return (mode <= BOOT_MODE_INTERNAL) ? (enum agm_boot_mode)mode : BOOT_MODE_AUTO;
#else
	return BOOT_MODE_AUTO;
#endif
}

int agm_boot_info_get(const struct device *dev, struct agm_boot_info *info)
{
	ARG_UNUSED(dev);

	memset(info, 0, sizeof(*info));
	info->record_valid = cfg_valid();
	info->record_read_failed = cfg_read_failed;
	info->record_magic = cfg.magic;
	info->record_version = cfg.version;
	/* An erased sector reads back as 0xff; a failed read leaves cfg's magic
	 * at whatever was there, so "unreadable" stays "invalid" rather than
	 * being mistaken for an empty store. */
	info->record_blank = !info->record_valid && cfg.magic == 0xFFFFFFFFU;
	info->once = once_peek();
	info->slot_programmed = !slot_is_blank();
	info->sec_ver = info->record_valid ? cfg.sec_ver : 0U;

	if (!cfg_valid()) {
		/* Nothing in the record yet (blank sector) or something that is
		 * not ours: report the *configured* geometry so the caller can
		 * print a useful block instead of a row of zeros. */
		for (uint32_t i = 0U; i < BOOT_SLOTS; i++) {
			info->slot[i].state = SLOT_EMPTY;
			info->slot[i].src = CFG_SRC_STORE;
			info->slot[i].offset = (i == 0U) ? BOOT_STORE_A : BOOT_STORE_B;
			info->slot[i].load = SLOT_BASE;
			info->slot[i].entry = SLOT_BASE;
		}
		return 0;
	}

	info->mode = (cfg.mode <= BOOT_MODE_INTERNAL) ? (enum agm_boot_mode)cfg.mode
						      : BOOT_MODE_AUTO;
	info->active = cfg.active;
	for (uint32_t i = 0U; i < BOOT_SLOTS; i++) {
		info->slot[i].state = (enum agm_boot_slot_state)cfg.slot[i].state;
		info->slot[i].src = (enum agm_boot_source)cfg.slot[i].src;
		info->slot[i].len = cfg.slot[i].ext_len;
		info->slot[i].crc = cfg.slot[i].ext_crc;
		info->slot[i].load = cfg.slot[i].load;
		info->slot[i].entry = cfg.slot[i].entry;
		info->slot[i].attempts = cfg.slot[i].attempts;
		info->slot[i].offset = cfg.slot[i].ext_off;
	}
	return 0;
}

int agm_boot_confirm(const struct device *dev)
{
	ARG_UNUSED(dev);

	if (!cfg_valid() || cfg.active >= BOOT_SLOTS) {
		return -ENOENT;
	}
	cfg.slot[cfg.active].state = SLOT_CONFIRMED;
	cfg.slot[cfg.active].attempts = 0U;
	if (cfg_write(&cfg) < 0) {
		return -EIO;
	}
	printk("loader: store %c confirmed\n", slot_letter(cfg.active));
	return 0;
}

int agm_boot_rollback(const struct device *dev)
{
	uint32_t slot;

	ARG_UNUSED(dev);

	if (!cfg_valid()) {
		return -ENOENT;
	}
	slot = 1U - (cfg.active < BOOT_SLOTS ? cfg.active : 0U);
	cfg.active = slot;
	cfg.slot[slot].state = SLOT_TRIAL;
	cfg.slot[slot].attempts = 0U;
	if (cfg_write(&cfg) < 0) {
		return -EIO;
	}
	printk("loader: active store is now %c\n", slot_letter(slot));
	return 0;
}

int agm_boot_mode_set(const struct device *dev, enum agm_boot_mode mode)
{
	ARG_UNUSED(dev);

	if (!cfg_valid()) {
		memset(&cfg, 0, sizeof(cfg));
		cfg.magic = BOOT_CFG_MAGIC;
		cfg.version = BOOT_CFG_VERSION;
		cfg.slot[0].ext_off = BOOT_STORE_A;
		cfg.slot[1].ext_off = BOOT_STORE_B;
	}
	cfg.mode = (uint32_t)mode;
	if (cfg_write(&cfg) < 0) {
		return -EIO;
	}
	printk("loader: mode is now %s (persistent)\n", agm_boot_mode_name(mode));
	return 0;
}

int agm_boot_once_arm(const struct device *dev, enum agm_boot_mode mode)
{
	ARG_UNUSED(dev);

	if (IS_ENABLED(CONFIG_BOOT_AGM_LOCK_PRODUCTION)) {
		/* A production image does not offer the operator escape hatch:
		 * arming a one-shot forces the next boot down a path the A/B
		 * policy did not choose. (erase/rollback stay available --
		 * they are A/B policy, not an upload path.) */
		printk("loader: one-shot is disabled in this (production) build\n");
		return -EPERM;
	}
	return once_arm(mode);
}

int agm_boot_install_slot(const struct device *dev)
{
	ARG_UNUSED(dev);

	return payload_install_slot();
}

/* ---- erase (mcumgr's image erase) ----------------------------------- */

int agm_boot_erase(const struct device *dev, enum agm_boot_target target)
{
	uint32_t slot = target;

	ARG_UNUSED(dev);

	if (target >= AGM_BOOT_TARGET_COUNT) {
		return -EINVAL;
	}
	/* the signed-command gate: erasing is a state change (it empties a
	 * record entry and, for
	 * the on-die and bitstream targets, erases flash), so the locked
	 * profile wants the signed command first. Checked before anything is
	 * touched, so a refused erase leaves the record exactly as it was. */
	if (!boot_agm_auth_take(AGM_BOOT_AUTH_CMD_ERASE, "erase", agm_boot_target_name(target))) {
		return AGM_BOOT_E_UNAUTHORIZED;
	}
	if (target == AGM_BOOT_TARGET_BITSTREAM) {
		/* Drop whatever is in the inactive slot *and* the record, so the
		 * next boot goes back to the factory slot: that is what "erase the
		 * pending bitstream" has to mean, otherwise the board would keep
		 * booting whichever update slot was last committed and a factory
		 * reflash would silently not take effect. */
		uint32_t slot_off = BS_SLOT_OFF(bs_inactive_slot());
		int ret;

		for (uint32_t off = 0U; off < BS_ON_DIE_MAX; off += BOOT_CFG_SECTOR) {
			ret = flash_erase(INT_FLASH, slot_off + off, BOOT_CFG_SECTOR);
			if (ret < 0) {
				return ret;
			}
		}
		ret = flash_erase(INT_FLASH, BS_RECORD_OFF, BOOT_CFG_SECTOR);
		if (ret < 0) {
			return ret;
		}
		printk("loader: bitstream slots and record erased -- the next boot "
		       "uses the factory slot (0x%08x)\r\n",
		       (uint32_t)AGM_BITSTREAM_CONFIG_ADDR);
		return 0;
	}

	/* Everything below edits the record (empty this slot's entry, and for the
	 * on-die target also its flash), so it needs one. "There is no record"
	 * is three different situations, though, and `cfg_read()` already tells
	 * them apart (it is what `info` reports as blank / invalid / unreadable):
	 *
	 *  - blank (nothing was ever published): the post-condition -- this
	 *    target holds no image -- already holds, so answer like the
	 *    "nothing was ever uploaded here" branches below do. Failing here is
	 *    what made `image erase` the first host command that breaks on a
	 *    freshly flashed board, and it also forced the operator to write a
	 *    record (an upload) just to be allowed to drop what was never there.
	 *    Nothing is written: creating a record would change what the next
	 *    boot reads for no reason.
	 *  - corrupt, or an older layout this build does not import: an error,
	 *    but one that says so instead of a generic failure.
	 *  - unreadable (the sector could not be read at init): also an error,
	 *    and the only one here that is not the caller's doing.
	 */
	if (!cfg_valid()) {
		if (cfg_read_failed) {
			printk("loader: erase %s: the boot record could not be read\n",
			       agm_boot_target_name(target));
			return -EIO;
		}
		if (cfg.magic == 0xffffffffU) {
			return 0; /* blank record area: nothing to drop */
		}
		printk("loader: erase %s: the boot record is invalid (magic 0x%08x, "
		       "version %u) -- nothing points at an image to drop\n",
		       agm_boot_target_name(target), cfg.magic, cfg.version);
		return -EINVAL;
	}

	if (target == AGM_BOOT_TARGET_SLOT) {
		slot = AGM_BOOT_TARGET_STORE_A;
		if (cfg.slot[slot].src != CFG_SRC_ON_DIE) {
			return 0; /* nothing was ever uploaded on-die */
		}
		for (uint32_t off = 0U; off < cfg.slot[slot].ext_len; off += BOOT_CFG_SECTOR) {
			int ret = flash_erase(INT_FLASH, SLOT_OFF + off, BOOT_CFG_SECTOR);

			if (ret < 0) {
				return ret;
			}
		}
	} else if (cfg.slot[slot].src != CFG_SRC_STORE) {
		return 0;
	}

	cfg.slot[slot].src = CFG_SRC_STORE;
	cfg.slot[slot].ext_off = (slot == AGM_BOOT_TARGET_STORE_A) ? BOOT_STORE_A : BOOT_STORE_B;
	cfg.slot[slot].ext_len = 0U;
	cfg.slot[slot].ext_crc = 0U;
	cfg.slot[slot].state = SLOT_EMPTY;
	cfg.slot[slot].attempts = 0U;
	if (cfg_write(&cfg) < 0) {
		return -EIO;
	}
	return 0;
}

/* ---- boot policy / boot --------------------------------------------- */

int agm_boot_policy_run(const struct device *dev, agm_boot_abort_cb_t abort_cb)
{
	enum agm_boot_mode once = once_take();
	enum agm_boot_mode mode = (once != BOOT_MODE_AUTO)
					  ? once
					  : (cfg_valid() ? (enum agm_boot_mode)cfg.mode
							 : BOOT_MODE_AUTO);
	ARG_UNUSED(dev);

	printk("loader: decision: one-shot %s, record %s -> %s\n", agm_boot_mode_name(once),
	       cfg_valid() ? agm_boot_mode_name((enum agm_boot_mode)cfg.mode) : "invalid",
	       agm_boot_mode_name(mode));

	settle_before_boot();

	/* `mode internal` never waits: it is the "always go to the slot" mode
	 * an operator picks when the abort window would otherwise swallow the
	 * first byte of an upload. */
	if (mode != BOOT_MODE_INTERNAL && abort_cb != NULL && abort_cb()) {
		return -EAGAIN;
	}

	/* Both remaining modes run the A/B policy; the only difference is the
	 * abort window above (`internal` never waits). A value left over from
	 * the removed `external` mode was already clamped to AUTO by
	 * once_take()/cfg_read(), so nothing else has to be handled here. */
	if (!device_is_ready(INT_FLASH)) {
		printk("loader: on-die flash driver missing\n");
		return -ENODEV;
	}
	return boot_ab_policy();
}

int agm_boot_boot(const struct device *dev, uint32_t slot)
{
	ARG_UNUSED(dev);

	settle_before_boot();
	return store_boot(slot);
}

/* ---- console protocol (`upload <a|b|slot|bitstream>`) --------------- */

static void up_reply(uint8_t byte)
{
	console_uart_write(&byte, 1);
}

/* Bounded byte read for the upload phase. The console `upload` path used a
 * bare `for (;;)`: a host that died mid-frame left the loader spinning in
 * agm_boot_upload_console() forever, with only a reset to get back (the same
 * failure the AN3155 server fixed in an_get()).
 *
 * Both bounds are needed, for the same reason as there: the wall-clock one is
 * the real limit on hardware, the spin count is the backstop on targets where
 * the clock does not advance while a busy loop runs (native_sim). */
#define UP_BYTE_TIMEOUT_MS 2000
#define UP_SPIN_LIMIT      5000000U

static bool up_aborted;

static bool up_get(uint8_t *b)
{
	int64_t from = k_uptime_get();

	if (up_aborted) {
		return false;
	}

	for (uint32_t spin = 0U;; spin++) {
		char c;

		if (console_poll_char(&c)) {
			*b = (uint8_t)c;
			return true;
		}
		if ((k_uptime_get() - from > UP_BYTE_TIMEOUT_MS) ||
		    (spin > UP_SPIN_LIMIT)) {
			up_aborted = true;
			return false;
		}
		arch_nop();
	}
}

static bool up_read_exact(uint8_t *dst, uint32_t len)
{
	for (uint32_t i = 0; i < len; i++) {
		if (!up_get(&dst[i])) {
			return false;
		}
	}
	return true;
}

static uint32_t up_rd32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
	       ((uint32_t)p[3] << 24);
}

/* The console protocol's target byte is its own numbering (0 = a, 1 = b,
 * 2 = bitstream, 3 = slot -- what tools/agm_upload.py sends); the API numbers
 * the targets A/B/slot/bitstream so that they line up with mcumgr's image
 * indices (0/1 = stores, 2 = on-die slot). */
static uint8_t up_wire_target(enum agm_boot_target target)
{
	switch (target) {
	case AGM_BOOT_TARGET_STORE_A:
		return 0U;
	case AGM_BOOT_TARGET_STORE_B:
		return 1U;
	case AGM_BOOT_TARGET_BITSTREAM:
		return 2U;
	default:
		return 3U;
	}
}

/* Which NAK code publish_upload()'s failure means to the host. By the time it
 * runs, the bytes are in the store, read back and CRC-checked, so the only
 * non-verdict failure left is writing the record (a flash error). Everything
 * the verifier or the placement checks say -- -EACCES for a bad digest, key or
 * signature, -EINVAL for a malformed container or a load address this layout
 * would not run, -ENOTSUP for a signed build with no trusted key -- is the
 * image's fault, and says so. */
static uint8_t upload_err_code(int ret)
{
	switch (ret) {
	case AGM_BOOT_E_OLD_VERSION:
		return UP_ERR_OLD_VERSION;
	case AGM_BOOT_E_UNBOUND:
		return UP_ERR_UNBOUND;
	case AGM_BOOT_E_UNAUTHORIZED:
		return UP_ERR_UNAUTHORIZED;
	case -EACCES:
	case -EINVAL:
	case -ENOTSUP:
		return UP_ERR_REJECT;
	default:
		return UP_ERR_FLASH;
	}
}

void agm_boot_upload_console(const struct device *dev, enum agm_boot_target target)
{
	static uint8_t payload[UP_MAX_PAYLOAD];
	uint8_t hdr[16];
	uint32_t total = 0U;
	uint32_t load = 0U;
	uint32_t entry = 0U;
	int ret;

	ARG_UNUSED(dev);

	ret = agm_boot_upload_begin(dev, target);
	if (ret < 0) {
		return;
	}

	up_aborted = false;

	printk("\r\nloader: READY target=%s -- send frames (magic 0x%02x)\r\n",
	       agm_boot_target_name(target), UP_MAGIC);

	for (;;) {
		/* Resynchronise on the frame magic. The console link drops a byte
		 * now and then (a CR/LF left by the console phase, a byte lost
		 * while the loader was erasing flash); scanning for UP_MAGIC means
		 * a host resend always lands, and the frame CRC then decides
		 * whether the frame is any good. Without this, one lost
		 * byte in FINISH aborted the phase, and the retry was swallowed by
		 * the console line reader instead. */
		do {
			if (!up_get(&hdr[0])) {
				printk("\r\nloader: upload dropped (host stopped sending)\r\n");
				return;
			}
		} while (hdr[0] != UP_MAGIC);
		if (!up_read_exact(&hdr[1], sizeof(hdr) - 1)) {
			printk("\r\nloader: upload dropped mid-header\r\n");
			return;
		}

		if (hdr[2] != up_wire_target(target)) {
			up_reply(UP_NAK);
			up_reply(UP_ERR_ARG);
			continue;
		}

		uint32_t cmd = hdr[1];
		uint32_t off = up_rd32(&hdr[4]);
		uint32_t len = up_rd32(&hdr[8]);
		uint32_t crc = up_rd32(&hdr[12]);

		if (len > UP_MAX_PAYLOAD) {
			up_reply(UP_NAK);
			up_reply(UP_ERR_ARG);
			continue;
		}
		if (len > 0U) {
			if (!up_read_exact(payload, len)) {
				printk("\r\nloader: upload dropped mid-payload\r\n");
				return;
			}
		}
		if (crc32_ieee(payload, len) != crc) {
			up_reply(UP_NAK);
			up_reply(UP_ERR_CRC);
			continue;   /* stay in phase: the host resends this frame */
		}

		if (cmd == UP_CMD_DATA) {
			ret = agm_boot_upload_write(dev, target, off, payload, len);
			if (ret < 0) {
				up_reply(UP_NAK);
				up_reply(UP_ERR_FLASH);
				return;
			}
			if (off + len > total) {
				total = off + len;
			}
			up_reply(UP_ACK);
			continue;
		}

		if (cmd == UP_CMD_FINISH && len == 16U) {
			uint32_t want = up_rd32(&payload[0]);
			uint32_t want_crc = up_rd32(&payload[4]);
			uint32_t got_crc = 0;

			load = up_rd32(&payload[8]);
			entry = up_rd32(&payload[12]);
			total = want;

			ret = upload_flush_carry(target);
			if (ret == 0) {
				ret = agm_boot_upload_crc(dev, target, 0U, total, &got_crc);
			}

			printk("\r\nloader: finish t=%u want=0x%08x got=0x%08x base=0x%06x\r\n",
			       total, want_crc, got_crc,
			       agm_boot_target_offset(dev, target));
			if (ret < 0 || got_crc != want_crc) {
				printk("\r\nloader: upload CRC mismatch (0x%08x vs 0x%08x)\r\n",
				       got_crc, want_crc);
				up_reply(UP_NAK);
				up_reply(UP_ERR_CRC);
				return;
			}
			printk("\r\nloader: received %u B, crc 0x%08x verified\r\n", total,
			       got_crc);
			printk("loader: publishing target=%s\r\n",
			       agm_boot_target_name(target));
			ret = publish_upload(target, total, got_crc, load, entry);
			if (ret < 0) {
				up_reply(UP_NAK);
				up_reply(upload_err_code(ret));
				return;
			}
			up_reply(UP_ACK);
			return;
		}

		up_reply(UP_NAK);
		up_reply(UP_ERR_ARG);
		continue;
	}
}

void agm_boot_console_write(const struct device *dev, const void *buf, size_t len)
{
	ARG_UNUSED(dev);

	console_uart_write(buf, len);
}

int agm_boot_console_poll(const struct device *dev, char *c)
{
	ARG_UNUSED(dev);

	return console_poll_char(c) ? 0 : -EAGAIN;
}

/* ---- init ------------------------------------------------------------ */

/*
 * A production-locked build with no key to verify with, or with no server that
 * could ever send a signed command, is a board nobody can update again through
 * any in-band path -- and it *looks* perfectly healthy (it boots, the console
 * answers, `info` is fine). Both are legal configurations (a sealed product, a
 * profile being brought up), so they are not build errors here; what they must
 * not be is silent, which is what this reports . The two lines
 * say what is wrong and what is left: SWD or the ROM bootloader.
 *
 * The sample's CMake turns the "no key" case into a build error for images it
 * builds (a shipped image always has one); this is what everybody else gets,
 * including applications that wire the driver up themselves.
 */
static void production_lock_report(void)
{
#if defined(CONFIG_BOOT_AGM_LOCK_PRODUCTION)
#if defined(AGM_HAVE_TINYCRYPT_ECDSA)
	if (!boot_agm_auth_key_ready()) {
		printk("loader: production lock is on but this build has no P-256 key "
		       "(none compiled in, or an RSA profile without a signed fabric) "
		       "-- every state change will be refused\n");
	}
#endif
	if (!IS_ENABLED(CONFIG_BOOT_AGM_SMP)) {
		printk("loader: production lock is on and no server can authorize "
		       "(CONFIG_BOOT_AGM_SMP=n) -- only SWD or the ROM bootloader can "
		       "update this board\n");
	}
#endif
}

static int boot_agm_init(const struct device *dev)
{
	ARG_UNUSED(dev);

#if defined(CONFIG_SOC_AGM_AGRV2K)
	/* What the FCB actually streamed this boot (soc/agm/agrv2k/fcb.c). Its
	 * own printk runs before the console exists, so this is the only place
	 * the choice becomes visible -- the dev board check that the record and the
	 * fabric agree.
	 *
	 * The "did it run at all" question comes first: the values it publishes
	 * are __noinit, so without this check an init-order regression would look
	 * exactly like a boot where the FCB had nothing to report. */
	/* Diagnostic only, and it lands on the early console before anything
	 * has had a chance to flush: keep it behind a switch. Nothing functional
	 * depends on these lines -- the *errors*
	 * below stay unconditional. */
	if (IS_ENABLED(CONFIG_BOOT_AGM_VERBOSE_LOG)) {
		if (!agm_fcb_program_ran()) {
			printk("loader: the FCB bring-up has not run -- init order "
			       "broken, fabric slot unknown\n");
		} else if (agm_fcb_bitstream_slot_get() == 0U) {
			printk("loader: fabric slot not reported by the FCB\n");
		} else {
			printk("loader: fabric came from %s (0x%08x)\n",
			       (agm_fcb_bitstream_slot_get() == AGM_BITSTREAM_CONFIG_ADDR)
				       ? "the factory slot"
				       : "an update slot",
			       agm_fcb_bitstream_slot_get());
		}
	}
	/* The verifier's own lines run before the console exists (PRE_KERNEL_1),
	 * so this is where the *reason* for a fallback to the factory fabric
	 * becomes visible: without it a refused slot and an empty record look
	 * the same from the console. */
	if (agm_fcb_program_ran() && agm_fcb_bitstream_refused_get() != 0) {
		printk("loader: the fabric slot in the record was refused (%s) "
		       "-- the next upload has to fix it\n",
		       (agm_fcb_bitstream_refused_get() == -EBADMSG)
			       ? "it does not match the record"
		       : (agm_fcb_bitstream_refused_get() == -EACCES)
			       ? "bad digest/signature"
		       : (agm_fcb_bitstream_refused_get() == -ENOTSUP)
			       ? "no trusted fabric key in this build"
			       : "not a fabric container");
	}
#endif

	if (!device_is_ready(STORE_FLASH) || !device_is_ready(INT_FLASH)) {
		printk("loader: boot driver: a flash device is not ready\n");
		return -ENODEV;
	}

	production_lock_report();

	/* Keep the failure: cfg stays BSS-zero then, and without this flag the
	 * console would report it as "record invalid", i.e. as if the sector
	 * held rubbish, instead of "could not read the record". */
	cfg_read_failed = (cfg_read() < 0);
	return 0;
}

DEVICE_DT_INST_DEFINE(0, boot_agm_init, NULL, NULL, NULL, POST_KERNEL,
		      CONFIG_BOOT_AGM_INIT_PRIORITY, NULL);

/* The trusted public key (raw X || Y, what tinycrypt's uECC_verify expects).
 * The application defines it; this weak default accepts nothing, so a build
 * that forgot to provide a key cannot silently accept unsigned images -- nor
 * authorize a command. It is defined in *every* build, not just the
 * signed ones: the header declares it unconditionally, and the command
 * authorization consults it in builds that carry no image profile at all.
 * MCUBOOT_KEY_LEN is AGM_BOOT_PUBKEY_LEN (64 B, or the 270-byte DER in an
 * RSA build) -- see include/zephyr/drivers/misc/boot_agm.h. */
__weak const uint8_t agm_boot_pubkey[AGM_BOOT_PUBKEY_LEN];

/* Is the key compiled in at all? An all-zero key is the weak default and means
 * "this build has no trusted key", so nothing signed can be accepted. Used by
 * the image verifier (boot_agm_verify.c), the fabric key fallback and the
 * command authorization. */
#if defined(AGM_HAVE_TINYCRYPT_ECDSA) || defined(CONFIG_BOOT_AGM_SIG_RSA2048_PSS)
bool boot_agm_key_present(const uint8_t *key, uint32_t len)
{
	for (uint32_t i = 0U; i < len; i++) {
		if (key[i] != 0U) {
			return true;
		}
	}
	return false;
}
#endif
