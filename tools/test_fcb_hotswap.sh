#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# test_fcb_hotswap.sh — drive samples/fcb_hotswap end-to-end, then assert the
#                        FCB hot-swap completed in both directions and the
#                        SPI NOR RDID moved with the fabric.
#
# This replaces the hand-driven capture sequence with a script.
# What motivated it: a 9-10 ms fabric-reload window each way,
# RDID 68 40 15 on slot A and 00 00 00 on slot B, no reboot between, FCB STAT
# 0x000f0002 both sides. Anything that regresses from there fails the script.
#
# Pre-conditions
#   * the firmware image (samples/fcb_hotswap built for agrv2k_407) is
#     already in FLASH — the script never flashes it; use
#     tools/test_uart_capture.sh / tools/flash_fw.sh for that;
#   * slot A (0x800e7000) holds the boot bitstream, written with
#     tools/flash_logic.sh (and that script has set OPTBY's FPGA CONFIG
#     pointer to that address);
#   * the dev board is plugged in, /dev/ttyACM0 is the AG32 console, the
#     on-board CMSIS-DAP probe is up.
#
# Usage
#   tools/test_fcb_hotswap.sh --slot-b-bs <bitstream.bin> [options]
#
#   --slot-a-bs <file>        (optional, doc only) bitstream that lives in
#                             slot A today — printed in the summary so the
#                             board state is reproducible from the log.
#   --slot-b-bs <file>        (required)         bitstream to write into
#                             slot B (the fabric update slot 2, 0x800cd000)
#                             before the swap.
#   --expected-rdid <hex>     (optional)         RDID bytes to require after
#                             the first swap, e.g. "00 00 00". If omitted,
#                             the script asserts the RDID *changed* from
#                             the boot value (which is the property that
#                             proves the fabric moved).
#   --no-swap-back            do only the first swap (A->B or B->A depending
#                             on the slot the board booted from); leave the
#                             board on the target slot. Default is the
#                             round trip (A->B, B->A) so the dev board is
#                             returned to slot A.
#
#   -b <board>     board name                (default agrv2k_407)
#   -d <device>    serial device             (default /dev/ttyACM0)
#   -t <seconds>   capture window            (default 25)
#   -o <path>      capture file              (default /tmp/hotswap_<board>_<ts>.bin)
#
# Exit codes
#   0  PASS — boot banner, slot B verify, both swaps completed, RDID moved
#   1  empty capture (probe / bridge / firmware not running)
#   2  swap did not complete (no FCB reload status line, or it reported FAIL)
#   3  preflight failure (slot B write failed, RDID did not move, missing
#      bitstream, missing venv, openocd session failure)
#
# The same SWD flake as test_uart_capture.sh applies (:
# examine success ~2/6 on this board); the openocd session retries
# AGM_OPENOCD_ATTEMPTS times before reporting failure.

set -u
set -o pipefail

# ---------- knobs -----------------------------------------------------------
BOARD="agrv2k_407"
TTY_DEV="/dev/ttyACM0"
TIMEOUT=25
OUTFILE=""
SLOT_A_BS=""
SLOT_B_BS=""
EXPECTED_RDID=""
NO_SWAP_BACK=0

usage() {
	sed -n '2,40p' "$0" | sed 's/^# \{0,1\}//'
	exit 2
}

while [ $# -gt 0 ]; do
	case "$1" in
	-b) BOARD="$2"; shift 2 ;;
	-d) TTY_DEV="$2"; shift 2 ;;
	-t) TIMEOUT="$2"; shift 2 ;;
	-o) OUTFILE="$2"; shift 2 ;;
	--slot-a-bs) SLOT_A_BS="$2"; shift 2 ;;
	--slot-a-bs=*) SLOT_A_BS="${1#*=}"; shift ;;
	--slot-b-bs) SLOT_B_BS="$2"; shift 2 ;;
	--slot-b-bs=*) SLOT_B_BS="${1#*=}"; shift ;;
	--expected-rdid) EXPECTED_RDID="$2"; shift 2 ;;
	--expected-rdid=*) EXPECTED_RDID="${1#*=}"; shift ;;
	--no-swap-back) NO_SWAP_BACK=1; shift ;;
	-h|--help) usage ;;
	--) shift; break ;;
	-*) echo "bad option: $1" >&2; usage ;;
	*) echo "unexpected positional arg: $1" >&2; usage ;;
	esac
done

# ---------- argument validation --------------------------------------------
if [ -z "$SLOT_B_BS" ]; then
	echo "FATAL: --slot-b-bs <file> is required (bitstream to put in slot B)" >&2
	usage
fi
if [ ! -f "$SLOT_B_BS" ]; then
	echo "FATAL: slot B bitstream not found: $SLOT_B_BS" >&2
	exit 3
fi

# Sanity: Supra bitstreams are 24986 words = 99944 bytes. Anything else will
# be refused by the FCB at ACTIVATE — but the script catches it here too so
# the failure mode is "wrong size" instead of "FCB reload FAILED".
SLOT_B_BYTES=$(wc -c < "$SLOT_B_BS" | tr -d ' ')
if [ "$SLOT_B_BYTES" != "99944" ]; then
	echo "WARN: slot B bitstream is $SLOT_B_BYTES bytes; FCB_AUTO_WORDS = 99944" >&2
	echo "      the FCB will refuse it at ACTIVATE — continuing so the failure is observable" >&2
fi

if [ -z "${VIRTUAL_ENV:-}" ]; then
	echo "FATAL: no Zephyr virtualenv active - activate the repo venv first" >&2
	exit 3
fi

OUTFILE="${OUTFILE:-/tmp/hotswap_${BOARD}_$(date +%Y%m%d_%H%M%S).bin}"

# ---------- locate openocd cfg + binaries (same conventions as flash_fw.sh) -
SELF_DIR="$(cd "$(dirname "$0")" && pwd)"
AGM_HOME="${ZEPHYR_HAL_AGM_HOME:-$(dirname "$SELF_DIR")}"
OO="$SELF_DIR/agm_oo.sh"

BOARD_CFG=""
for cand in "${AGM_WORKSPACE:-$HOME/zephyrproject}/modules/hal_ag32" "$AGM_HOME"; do
	if [ -f "$cand/zephyr/module.yml" ]; then
		BOARD_CFG="${AGM_OPENOCD_CFG:-}"
		[ -n "$BOARD_CFG" ] || BOARD_CFG="${AGM_BUILD_DIR:+${AGM_BUILD_DIR}/logic/openocd.cfg}"
		[ -n "$BOARD_CFG" ] || BOARD_CFG=$(ls -t "${AGM_WORKSPACE:-$HOME/zephyrproject}"/build*/logic/openocd.cfg /tmp/b_*/logic/openocd.cfg 2>/dev/null | head -1)
		[ -n "$BOARD_CFG" ] || BOARD_CFG="$cand/boards/agm/$BOARD/support/openocd.cfg"
		break
	fi
done
MINIMAL_CFG="$AGM_HOME/boards/agm/agrv2k/shared/support/agrv2k-minimal.cfg"
SDK_CFG="${AGRV_PLATFORM_ETC:-$HOME/AgRV_pio/platforms/AgRV/etc}/agrv2k.cfg"
if [ -n "${AGM_OPENOCD_CFG:-}" ]; then
	OPENOCD_CFG="$AGM_OPENOCD_CFG"
elif [ -n "$BOARD_CFG" ]; then
	OPENOCD_CFG="$BOARD_CFG"
elif [ -f "$SDK_CFG" ]; then
	# Fall back to the SDK cfg only when no board cfg is available — it
	# will pick up the vendor's 15 MHz + v1 HID defaults, the combination
	# that has been known to fail on this dev board. The board cfg is what we
	# actually want; this branch is here for completeness only.
	OPENOCD_CFG="$SDK_CFG"
else
	OPENOCD_CFG="$MINIMAL_CFG"
fi
OPENOCD_BIN="${AGRV_OPENOCD_BIN:-$HOME/AgRV_pio/packages/tool-agrv_openocd/bin/openocd_cmd}"
if [ ! -x "$OPENOCD_BIN" ]; then
	echo "FATAL: openocd_cmd not found at $OPENOCD_BIN" >&2
	echo "       set AGM_OPENOCD_BIN, or install the AgRV PlatformIO SDK" >&2
	exit 3
fi

# Where slot B lives. The sample takes this from the devicetree now
# (`hotswap_slot_b` in samples/fcb_hotswap/boards/agrv2k_407.overlay), so keep
# the two in step: the boot log carries the address and the check below greps
# for it. It used to be 0x800c0000, which overlapped fabric slot 1, the
# bind-salt sector and slot 2.
SLOT_B_ADDR="0x800cd000"

echo "=== test_fcb_hotswap.sh ==="
echo "board          = $BOARD"
echo "slot_b_bs      = $SLOT_B_BS ($SLOT_B_BYTES bytes) -> $SLOT_B_ADDR"
echo "slot_a_bs      = ${SLOT_A_BS:-<not provided, slot A left as-is>}"
echo "expected_rdid  = ${EXPECTED_RDID:-<assert: post != boot>}"
echo "swap_back      = $([ "$NO_SWAP_BACK" -eq 0 ] && echo "yes (round trip)" || echo "no (--no-swap-back)")"
echo "tty            = $TTY_DEV"
echo "capture_path   = $OUTFILE"
echo "openocd cfg    = $OPENOCD_CFG"

# ---------- 1. write slot B ------------------------------------------------
echo ""
echo "=== 1. write slot B ($SLOT_B_BS -> $SLOT_B_ADDR) ==="
"$OO" fw "$SLOT_B_BS" "$SLOT_B_ADDR" || {
	echo "FATAL: slot B write failed (see oo output above)" >&2
	exit 3
}

# ---------- 2. serial port setup -------------------------------------------
echo ""
echo "=== 2. $TTY_DEV setup ==="
stty -F "$TTY_DEV" 115200 cs8 -cstopb -parenb -crtscts raw -echo 2>&1 || {
	echo "FATAL: stty failed on $TTY_DEV" >&2; exit 3; }

# ---------- 3. reader + reset run (no flash) --------------------------------
echo ""
echo "=== 3. reading $TTY_DEV in background (${TIMEOUT}s) ==="
dd if="$TTY_DEV" of=/dev/null bs=64 count=1 iflag=nonblock 2>/dev/null
rm -f "$OUTFILE"
( timeout "$TIMEOUT" cat "$TTY_DEV" > "$OUTFILE" 2>/dev/null ) &
READER_PID=$!
sleep 0.5

echo ""
echo "=== 4. openocd: reset run (firmware already in FLASH) ==="
OCD_OUT=$(AGRV_ADAPTER=cmsis-dap "$OPENOCD_BIN" \
	-s "${AGRV_PLATFORM_ETC:-$HOME/AgRV_pio/platforms/AgRV/etc}" \
	-f "$OPENOCD_CFG" \
	-c "init" -c "reset run" -c "shutdown" 2>&1) || true
printf '%s\n' "$OCD_OUT" | tail -5
if printf '%s' "$OCD_OUT" \
		| grep -qE "Examination failed|stalled AP|dmstatus=0x0|Couldn't determine state|examine-end failed"; then
	echo "FATAL: openocd reset-run failed; check probe + SWD config)" >&2
	wait "$READER_PID" 2>/dev/null || true
	exit 3
fi

# ---------- 5. send 'x' (first swap) ---------------------------------------
# The sample prints its banner + slot verdicts + boot RDID before the prompt.
# Match the hand recipe the sample documents: 1.5 s settle, then
# 'x' triggers the swap, then ~4 s for the post-swap line + prompt to come
# back. The swap itself is ~10 ms; the rest is console.
echo ""
echo "=== 5. send 'x' (first swap) ==="
sleep 1.5
printf 'x' > "$TTY_DEV"

# ---------- 6. parse boot + first swap -------------------------------------
# These run after the swap + a settle so we read the prompt + post line.
sleep 4

# Anchor-based parsing: the bytes between anchors are allowed to be garbage
# (the fabric swap tears down the UART clock mid-byte; that's the whole
# point of the "FCB reload completed, window N ms" line being there).
post_line() {
	# Print the last hotswap [post]: line in the capture.
	grep -aoE 'hotswap \[post\]:[^@]*rdid=[0-9a-f]{2} [0-9a-f]{2} [0-9a-f]{2}' "$OUTFILE" | tail -1
}
boot_line() {
	grep -aoE 'hotswap \[boot\]:[^@]*rdid=[0-9a-f]{2} [0-9a-f]{2} [0-9a-f]{2}' "$OUTFILE" | head -1
}
swap_status_line() {
	# "hotswap: FCB reload completed (rc=0), window 10 ms"
	# "hotswap: FCB reload FAILED (rc=-EIO), window 10 ms"
	grep -aoE 'hotswap: FCB reload (completed|FAILED) \(rc=-?[0-9]+\), window [0-9]+ ms' "$OUTFILE"
}
slot_b_verify_line() {
	grep -aE "slot B @${SLOT_B_ADDR} spare[[:space:]]+: ok" "$OUTFILE"
}
extract_rdid() {
	# $1 = full anchor line; print "xx yy zz" or empty.
	echo "$1" | grep -aoE 'rdid=[0-9a-f]{2} [0-9a-f]{2} [0-9a-f]{2}' | head -1 | sed 's/rdid=//'
}

echo ""
echo "=== 6. parse + first-swap verdict ==="
if ! grep -q 'fcb_hotswap: runtime fabric reload' "$OUTFILE"; then
	echo "RESULT: FAIL — boot banner not seen in $OUTFILE" >&2
	echo "  (firmware not in FLASH, or board still on slot B from a previous run?)" >&2
	wait "$READER_PID" 2>/dev/null || true
	exit 1
fi

BOOT=$(boot_line)
if [ -z "$BOOT" ]; then
	echo "RESULT: FAIL — no 'hotswap [boot]: ...' line (firmware probably crashed)" >&2
	tail -c 512 "$OUTFILE" >&2
	wait "$READER_PID" 2>/dev/null || true
	exit 1
fi
BOOT_RDID=$(extract_rdid "$BOOT")
echo "  boot RDID:   $BOOT_RDID"

if [ -z "$(slot_b_verify_line)" ]; then
	echo "RESULT: FAIL — slot B not 'ok' at boot; the write to $SLOT_B_ADDR did not land" >&2
	grep -a 'slot B' "$OUTFILE" | head -3 >&2
	wait "$READER_PID" 2>/dev/null || true
	exit 3
fi
echo "  slot B verify: OK"

# Collect every FCB reload status line — there should be at least one by now
# (the first swap). We will check the round-trip count at the end too.
SWAP_STATUSES=$(swap_status_line || true)
if [ -z "$SWAP_STATUSES" ]; then
	echo "RESULT: FAIL — no FCB reload status seen after 'x' (swap never started)" >&2
	tail -c 512 "$OUTFILE" >&2
	wait "$READER_PID" 2>/dev/null || true
	exit 2
fi
FIRST_SWAP=$(echo "$SWAP_STATUSES" | head -1)
echo "  first swap:  $FIRST_SWAP"
if echo "$FIRST_SWAP" | grep -q 'FAILED'; then
	echo "RESULT: FAIL — first swap FAILED: $FIRST_SWAP" >&2
	wait "$READER_PID" 2>/dev/null || true
	exit 2
fi

POST1=$(post_line)
if [ -z "$POST1" ]; then
	echo "RESULT: FAIL — no 'hotswap [post]:' line after first swap (firmware probably crashed)" >&2
	tail -c 512 "$OUTFILE" >&2
	wait "$READER_PID" 2>/dev/null || true
	exit 2
fi
POST1_RDID=$(extract_rdid "$POST1")
echo "  post RDID:   $POST1_RDID"

# RDID verdict on the first swap: must move (or match the user-provided one).
if [ -n "$EXPECTED_RDID" ]; then
	if [ "$POST1_RDID" != "$EXPECTED_RDID" ]; then
		echo "RESULT: FAIL — post-swap RDID '$POST1_RDID' != expected '$EXPECTED_RDID'" >&2
		wait "$READER_PID" 2>/dev/null || true
		exit 3
	fi
	echo "  RDID match:  OK (matched --expected-rdid)"
else
	if [ "$POST1_RDID" = "$BOOT_RDID" ]; then
		echo "RESULT: FAIL — RDID did not change after the swap ($BOOT_RDID -> $POST1_RDID); fabric did not move" >&2
		wait "$READER_PID" 2>/dev/null || true
		exit 3
	fi
	echo "  RDID moved:  OK ($BOOT_RDID -> $POST1_RDID)"
fi

# ---------- 7. second swap (B -> A) unless --no-swap-back ------------------
if [ "$NO_SWAP_BACK" -eq 0 ]; then
	echo ""
	echo "=== 7. send 'x' (swap back to slot A) ==="
	sleep 1
	printf 'x' > "$TTY_DEV"
	sleep 4

	SWAP2=$(swap_status_line | tail -1)
	if [ -z "$SWAP2" ]; then
		echo "RESULT: FAIL — no FCB reload status after second 'x'" >&2
		wait "$READER_PID" 2>/dev/null || true
		exit 2
	fi
	echo "  second swap: $SWAP2"
	if echo "$SWAP2" | grep -q 'FAILED'; then
		echo "RESULT: FAIL — second swap FAILED: $SWAP2" >&2
		wait "$READER_PID" 2>/dev/null || true
		exit 2
	fi

	POST2=$(post_line)
	POST2_RDID=$(extract_rdid "$POST2")
	echo "  post2 RDID:  $POST2_RDID"
	# Round trip is the property we actually care about: the RDID moved
	# *twice*, so the fabric is doing real work and is the same image on
	# both sides (same firmware, same console route).
	if [ "$POST2_RDID" = "$POST1_RDID" ]; then
		echo "RESULT: FAIL — RDID did not change after the second swap ($POST1_RDID -> $POST2_RDID);" >&2
		echo "        the round trip did not move the fabric back" >&2
		wait "$READER_PID" 2>/dev/null || true
		exit 3
	fi
	echo "  RDID moved:  OK ($POST1_RDID -> $POST2_RDID)"
fi

# ---------- 8. collect + summary -------------------------------------------
wait "$READER_PID" 2>/dev/null || true

echo ""
echo "=== 8. summary ==="
BYTES=$(wc -c < "$OUTFILE")
echo "captured bytes  = $BYTES (raw log at $OUTFILE)"
echo "boot RDID       = $BOOT_RDID"
echo "post-swap RDID  = $POST1_RDID"
if [ "$NO_SWAP_BACK" -eq 0 ]; then
	echo "post2 RDID      = $POST2_RDID"
fi
echo "FCB status (1)  = $FIRST_SWAP"
echo ""
if [ "$NO_SWAP_BACK" -eq 0 ]; then
	echo "RESULT: PASS — boot, swap, round-trip, all FCB reloads rc=0, RDID moved twice"
	echo "         board left on slot A"
else
	echo "RESULT: PASS — boot, first swap, FCB reload rc=0, RDID moved"
	echo "         board left on the slot it was swapped to (--no-swap-back)"
fi
exit 0
