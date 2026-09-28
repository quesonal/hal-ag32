#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
#
# DEPRECATED (Plan A): `west flash` is the unified
# entry point now. Use:
#   west flash                       # write both (agrv_openocd is the default)
#   west flash --skip-bitstream      # this script's job
#   west flash --bitstream-only      # flash_logic.sh's job
# (all three belong to the default agrv_openocd runner; the ROM-bootloader
#  runner --runner agrv32flash has no --bitstream-only)
# Direct invocation of this file keeps running so muscle-memory
# CI hooks don't break, but the `west build -t flash` /
# `-t flash-logic` CMake targets are gone -- call west flash
# with the flag above. Slated for removal once no external
# scripts reference this entry point.
#
# Flash the Zephyr firmware image to FLASH @ 0x80000000, erasing only the
# pages the image occupies so the FPGA bitstream above
# ${AGM_BITSTREAM_ADDR} survives. Then `reset run` so the CPU boots the
# new firmware.
#
# This is the surgical counterpart to `west flash --runner agrv_openocd`
# (which does `flash write_image erase`, bank-erase collateral-damaging
# the bitstream). Use this during firmware-iteration development.
#
# Usage:
#   flash_fw.sh [-n] <firmware.{elf,bin,hex,ihex,s19,srec}>
#
#   -n, --no-erase   program-only: skip the erase. Only valid when the
#                    target pages are already 0xFF; otherwise the write
#                    verifies dirty and the script fails.
#
# Supported formats (auto-detected from file extension):
#   .elf  / .axf   ELF — segments written at their virtual addresses
#                  (XIP firmware, where segments are in FLASH @ 0x8xxxxxxx)
#   .bin           raw binary — written at FW_LOAD_ADDR
#                  (typical for non-XIP Zephyr, where the AgRV ROM
#                  bootloader copies BIN from FLASH to SRAM and jumps)
#   .hex / .ihex   Intel HEX — records written at their embedded addresses
#   .s19 / .srec   Motorola S-record — records written at their addresses
#
# Why format matters:
#   Zephyr's default linker script places .text / .rodata in SRAM
#   (CONFIG_XIP is not enabled in our MVP), so the ELF program headers
#   put segments at 0x20000000 (SRAM), NOT 0x80000000 (FLASH). When
#   openocd gets an ELF whose segments are outside any flash bank, it
#   falls back to `load_image` which writes to virtual addresses — that
#   puts firmware in SRAM, not FLASH, and the CPU can't boot from SRAM
#   on reset. The Zephyr build emits BOTH zephyr.elf (with SRAM vaddrs)
#   AND zephyr.bin (the post-link raw image intended for FLASH at
#   0x80000000). Pass the .bin and you get the boot path the SDK's
#   ROM bootloader expects.
#
# Environment overrides:
#   AGRV_ADAPTER         cmsis-dap | jlink         default: cmsis-dap
#   AGRV_PLATFORM_ETC    platforms/AgRV/etc path   default: ~/AgRV_pio/platforms/AgRV/etc
#   AGRV_OPENOCD         openocd binary path       default: ~/AgRV_pio/packages/tool-agrv_openocd/bin/openocd_cmd
#   ZEPHYR_HAL_AGM_HOME  hal_ag32 checkout path     default: the tree this script lives in
#   ZEPHYR_HAL_AGM_TOOLS tools subdir              default: $ZEPHYR_HAL_AGM_HOME/tools
#   AGM_OPENOCD_ATTEMPTS whole-session retries     default: 3
#   FW_LOAD_ADDR         FLASH load address for    default: 0x80000000
#                        non-ELF formats. Ignored
#                        for ELF (uses program
#                        headers).
#   AGM_BITSTREAM_ADDR   first byte after the      default: 0x800e7000
#                        firmware region (never
#                        erased)
#   AGM_FW_ERASE_LEN     erase span override       default: image size for
#                        (hex bytes)              .bin, else 0xe7000
#
# Erase behaviour:
#   By default the script erases just the pages the image occupies
#   (4 KB granules at FW_LOAD_ADDR) and then writes + verifies. It never
#   erases at or above AGM_BITSTREAM_ADDR, so the bitstream loaded by
#   tools/flash_logic.sh survives -- do NOT reach for a mass/bank erase to
#   "clean up", that erases the bitstream too.
#
# Exit codes:
#   1  bad args
#   2  missing file or unknown format
#   3  tool not found
#   *  openocd exit code

set -eu

# --- venv guard  ----
# A bare `python3` here silently resolves to /usr/bin/python3, which lacks
# pyusb -> openocd_warmup.py fails in ways the log does not attribute to the
# interpreter. Fail fast with an actionable message instead.
if [ -z "${VIRTUAL_ENV:-}" ]; then
	echo "FATAL: no Zephyr virtualenv active - activate the repo venv first" >&2
	exit 3
fi
PYTHON=${PYTHON:-$VIRTUAL_ENV/bin/python3}

NO_ERASE=0
case "${1:-}" in
	-n | --no-erase)
		NO_ERASE=1
		shift
		;;
esac

if [ $# -ne 1 ]; then
	echo "Usage: $0 [-n] <firmware.{elf,bin,hex,ihex,s19,srec}>" >&2
	echo "  -n  skip the erase (only if the target region is already 0xFF)" >&2
	exit 1
fi

FW_FILE=$1
if [ ! -f "$FW_FILE" ]; then
	echo "Error: firmware image not found at $FW_FILE" >&2
	exit 2
fi

# --- Auto-detect format from extension -----------------------------
# Each format needs a different openocd `flash write_image` type tag,
# and ELF ignores the load address (uses program headers) while the
# others use FW_LOAD_ADDR (default 0x80000000 = FLASH base, where the
# AgRV ROM bootloader expects to find raw firmware).
FW_LOAD_ADDR=${FW_LOAD_ADDR:-0x80000000}
BS_ADDR=${AGM_BITSTREAM_ADDR:-0x800e7000}

# Erase span: 4 KB granules covering exactly this image, so nothing at or
# above BS_ADDR (the bitstream) is ever touched. A .bin size is known; for
# ELF/HEX/S-record the extent would have to be parsed, so use the whole
# firmware region.
FW_ERASE_LEN=${AGM_FW_ERASE_LEN:-}
if [ -z "$FW_ERASE_LEN" ]; then
	if [ "${FW_FILE##*.}" = "bin" ]; then
		fw_bytes=$(wc -c < "$FW_FILE" | tr -d ' ')
		FW_ERASE_LEN=$(awk -v s="$fw_bytes" 'BEGIN { printf "0x%x", int((s + 4095) / 4096) * 4096 }')
	else
		FW_ERASE_LEN=0xe7000
	fi
fi

case "$FW_FILE" in
	*.elf|*.axf)
		FMT="elf"
		WRITE_ADDR="0x0"      # ignored; openocd uses ELF program headers
		;;
	*.bin)
		FMT="bin"
		WRITE_ADDR="$FW_LOAD_ADDR"
		;;
	*.hex|*.ihex)
		FMT="ihex"
		WRITE_ADDR="$FW_LOAD_ADDR"   # used as default if record has no addr
		;;
	*.s19|*.srec)
		FMT="s19"
		WRITE_ADDR="$FW_LOAD_ADDR"
		;;
	*)
		echo "Error: cannot detect firmware format from extension: $FW_FILE" >&2
		echo "Supported: .elf .axf .bin .hex .ihex .s19 .srec" >&2
		exit 2
		;;
esac

# --- Locate tools --------------------------------------------------
# This script lives in <module>/tools/, so the module root is its parent.
SELF_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ZEPHYR_HAL_AGM_HOME=${ZEPHYR_HAL_AGM_HOME:-$(dirname -- "$SELF_DIR")}
ZEPHYR_HAL_AGM_TOOLS=${ZEPHYR_HAL_AGM_TOOLS:-$ZEPHYR_HAL_AGM_HOME/tools}
AGRV_SDK_PATH=${AGRV_SDK_PATH:-$HOME/AgRV_pio}
AGRV_PLATFORM_ETC=${AGRV_PLATFORM_ETC:-$AGRV_SDK_PATH/platforms/AgRV/etc}
AGRV_OPENOCD=${AGRV_OPENOCD:-$AGRV_SDK_PATH/packages/tool-agrv_openocd/bin/openocd_cmd}
AGRV_ADAPTER=${AGRV_ADAPTER:-cmsis-dap}

# Probe setup comes from the board's support/openocd.cfg, not from the SDK's
# agrv2k.cfg directly: the board cfg is what forces the CMSIS-DAP v2 (bulk)
# backend and the vendor's 10 MHz SWD clock. Driven straight from agrv2k.cfg
# openocd falls back to the v1 HID backend at 15 MHz (14285 kHz on this
# probe), and on this board that combination fails AP accesses
# ("stalled AP operation", dmstatus=0x0), where the vendor's
# PlatformIO flow passes ADAPTER_SPEED=10000.
#
# The rendered cfg lives in the *workspace* module checkout by default (that
# is the tree every `west build` configures); the dev tree keeps a copy only
# as a fallback, so prefer the workspace one when it exists.
BOARD=${BOARD:-agrv2k_407}
SUPPORT_CFG=""
for _cand in "${AGM_WORKSPACE:-$HOME/zephyrproject}/modules/hal_ag32" \
	     "$ZEPHYR_HAL_AGM_HOME"; do
	if [ -f "$_cand/zephyr/module.yml" ]; then
		SUPPORT_CFG="${AGM_OPENOCD_CFG:-}"
		[ -n "$SUPPORT_CFG" ] || SUPPORT_CFG="${AGM_BUILD_DIR:+${AGM_BUILD_DIR}/logic/openocd.cfg}"
		[ -n "$SUPPORT_CFG" ] || SUPPORT_CFG=$(ls -t "${AGM_WORKSPACE:-$HOME/zephyrproject}"/build*/logic/openocd.cfg /tmp/b_*/logic/openocd.cfg 2>/dev/null | head -1)
		[ -n "$SUPPORT_CFG" ] || SUPPORT_CFG="$_cand/boards/agm/$BOARD/support/openocd.cfg"
		break
	fi
done
if [ -z "$SUPPORT_CFG" ]; then
	echo "Error: board support cfg not found for $BOARD" >&2
	echo "Run a west build once so the module renders support/openocd.cfg." >&2
	exit 3
fi

if [ ! -x "$AGRV_OPENOCD" ]; then
	echo "Error: openocd not found at $AGRV_OPENOCD" >&2
	echo "Set AGRV_OPENOCD or install tool-agrv_openocd." >&2
	exit 3
fi
if [ ! -x "$ZEPHYR_HAL_AGM_TOOLS/openocd_warmup.py" ]; then
	echo "Error: openocd_warmup.py not found at $ZEPHYR_HAL_AGM_TOOLS/openocd_warmup.py" >&2
	echo "Set ZEPHYR_HAL_AGM_HOME." >&2
	exit 3
fi

# --- Warmup probe + flash firmware + reset -------------------------
# Sequence:
#   1. warmup:    slow pyusb GET_DESCRIPTOR for the CMSIS-DAP probe
#                 (FW 2.1.0 string-read timeout workaround)
#   2. init:      AgRV SDK cfg sources interface/cmsis-dap.cfg,
#                 switches to usb_bulk backend, examines RISC-V core
#   3. halt:      stop CPU so flash writes don't race
#   4. erase +    flash erase_address (unless -n), then
#      write:     flash write_image $FW_FILE $WRITE_ADDR $FMT
#                 (never at or above 0x800e7000, so the bitstream
#                 survives)
#   5. verify:    readback + CRC check
#   6. reset run: FPGA loads bitstream (from OPTBY pointer, set when
#                 bitstream was last flashed), CPU boots firmware
#                 (ROM bootloader copies BIN from FLASH to SRAM, jumps)
#   7. shutdown:  close openocd session, release probe

echo ">>> firmware: $FW_FILE"
echo ">>> format:   $FMT"
echo ">>> write at: $WRITE_ADDR"
if [ "$NO_ERASE" = 1 ]; then
	echo ">>> erase:    skipped (-n): the target region must already be 0xFF"
else
	echo ">>> erase:    $FW_ERASE_LEN bytes at $FW_LOAD_ADDR (bitstream at $BS_ADDR survives)"
fi
echo ">>> [1/6] warmup probe (AGM CMSIS-DAP FW 2.1.0 string-read workaround)"
"$PYTHON" "$ZEPHYR_HAL_AGM_TOOLS/openocd_warmup.py"

# The SDK flash driver intermittently reports
#   "Flash write data timed out after NNN ms."
# right after an erase (seen roughly 1 in 6 runs), and the SWD examine
# itself flakes (2 runs in 6).
# In both cases the region is left erased, so re-running the whole session
# recovers; anything else is reported with a hint instead of a wall of diffs.
# AGM_OPENOCD_ATTEMPTS overrides the retry budget (shared with the other tools).
export AGRV_ADAPTER	# openocd.cfg.in takes the interface from this env var

ATTEMPTS=${AGM_OPENOCD_ATTEMPTS:-3}
attempt=1
while :; do
	echo ">>> [2/6] openocd: init/halt/$( [ "$NO_ERASE" = 1 ] && echo write || echo erase+write )/verify/reset/shutdown (attempt $attempt/$ATTEMPTS)"
	if [ "$NO_ERASE" = 1 ]; then
		set --
	else
		set -- -c "flash erase_address $FW_LOAD_ADDR $FW_ERASE_LEN"
	fi

	OCD_RC=0
	OCD_OUT=$("$AGRV_OPENOCD" \
		-s "$AGRV_PLATFORM_ETC" \
		-c "variable ADAPTER $AGRV_ADAPTER" \
		-c "variable ADAPTER_SPEED ${AGM_SWD_SPEED:-10000}" \
		-f "$SUPPORT_CFG" \
		-c "init" \
		-c "reset init" \
		"$@" \
		-c "flash write_image $FW_FILE $WRITE_ADDR $FMT" \
		-c "verify_image $FW_FILE $WRITE_ADDR $FMT" \
		-c "reset run" \
		-c "shutdown" 2>&1) || OCD_RC=$?
	printf '%s\n' "$OCD_OUT" | tail -8

	if printf '%s' "$OCD_OUT" | grep -qE "Flash write data timed out|error writing to flash|Examination failed|stalled AP|dmstatus=0x0|Couldn't determine state|examine-end failed"; then
		if [ "$attempt" -lt "$ATTEMPTS" ]; then
			echo "WARN: openocd session failed (attempt $attempt/$ATTEMPTS) - retrying" >&2
			attempt=$((attempt + 1))
			sleep 2
			continue
		fi
		echo "FATAL: openocd session failed after $ATTEMPTS attempts (raise AGM_OPENOCD_ATTEMPTS to retry more)" >&2
		exit 3
	fi

	# Success is judged by evidence, not by the absence of "Error" lines:
	# a transient timeout can print one and still recover, and openocd
	# itself prints "Error:" for some harmless shutdown paths.
	if printf '%s' "$OCD_OUT" | grep -qE "verified [0-9]+ bytes"; then
		if [ "$OCD_RC" -ne 0 ] && ! printf '%s' "$OCD_OUT" | grep -q "shutdown command invoked"; then
			echo "WARN: openocd rc=$OCD_RC (verify succeeded)" >&2
		fi
		break
	fi

	if printf '%s' "$OCD_OUT" | grep -qE "^Error|checksum mismatch"; then
		echo "FATAL: programming failed (openocd rc=$OCD_RC)." >&2
		echo "       Most common cause: the pages were not erased (i.e. this" >&2
		echo "       ran with -n, or a previous image is still there)." >&2
		echo "       Re-run without -n: it erases $FW_ERASE_LEN bytes at" >&2
		echo "       $FW_LOAD_ADDR and leaves the bitstream at $BS_ADDR alone." >&2
		exit 3
	fi

	echo "FATAL: no verify evidence in the openocd output (rc=$OCD_RC)" >&2
	exit 3
done

echo ">>> [6/6] done (CPU running; capture the console with tools/test_uart_capture.sh -n)"
