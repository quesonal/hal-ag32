#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
#
# agm_rdp_tear_test.sh — put the chip's *option area* through a tear: a power
# loss, or a reset, in the middle of the RDP write.
#
# Why a tool is needed at all: writing the option area is a handful of register
# writes (a few milliseconds inside a session that starts in ~300 ms), so
# pulling the plug by hand cannot be aimed at it. Two ways to widen the target
# are built in:
#
#   --window N   keep ONE openocd session (no per-write startup) issuing
#                back-to-back `agrv lock 0` writes for N seconds. The option
#                write is then a large fraction of the wall time, so a cut
#                "sometime in the next N seconds" has a real chance of landing
#                inside one.
#   --reset-storm  while the window is open, pulse the target's nRESET from the
#                probe (tools/probe_reset_target.py) as fast as it answers --
#                the closest thing to a power cut that a host can trigger by
#                itself, and repeatable. A reset during the option write is
#                what the vendor warns about, and what the ROM-loader fallback
#                exists for.
#   --kill-after N  start the same session, then SIGKILL it (the whole process
#                group, so the wrapper's openocd child goes too) N seconds in.
#                openocd spends most of a session *inside* an option write, so
#                a random kill lands in one often -- a host-side stand-in for a
#                power cut, and the one that can be sampled repeatably.
#   --iwdg-reset  arm the *on-chip* watchdog (the backup-domain IWDG, 64 ms at
#                its shortest tap) from the host, inside the same session and
#                immediately before the option writes: it resets the chip ~64 ms
#                later, i.e. in the middle of the first write. This is the one
#                reset source that needs neither the BOOT0 jumper nor the probe
#                -- openocd owns the DAP for the whole session, which is why
#                --reset-storm (pulses from a second process) does nothing.
#   --iwdg-after N  arm it after N *completed* writes instead. Armed at the top
#                (N = 0) the reset lands in the host-overhead part of the first
#                write and changes nothing; moving the arming point
#                walks the deadline through the write, which is how to aim at
#                the erase/program pair inside it.
#
#                Two preconditions: the RTC/backup
#                domain has to be *clocked* (a build whose devicetree has
#                &rtc0/&iwdg0 okay, or a SYS.APB_CLKENABLE write first) and the
#                chip has to be *unlocked* -- otherwise the arm's register
#                writes fail with "Failed to write memory (addr=0x40000032)"
#                (RDP on) or are silently dropped and read back as 0x0000 (gate
#                off). The tool reads the IWDG register after arming and says
#                which of the two happened.
#   --kill-on-erase  the aimed version of --kill-after: watch the session's own
#                log and SIGKILL it the moment the driver reports "Option bytes
#                are erased", i.e. inside the erase -> program gap. That gap is
#                the only place an interruption tears the option area, and it is
#                a few ms out of the 206 ms one `agrv lock 0` takes, so sampling
#                by time hits it rarely (one hit in ~10 samples).
#
#   --rom-nreset [S]  aim the probe's nRESET into the ROM-side option write
#                (`agrv32flash -j` talks over UART, so the probe is free).
#                Kept for the record; it is NOT a reproduction: 15
#                attempts from a verified canonical option area gave 0
#                tears. Two reasons --
#                  * the ROM's own option erase+program is ~3 ms wide, and
#                    the ROM tool's handshake phase in front of it wanders
#                    over 0.02..1.0 s, so no sleep offset can aim; a nRESET
#                    that lands *before* the write is harmless (the ROM comes
#                    back up and takes the late 0x82 command anyway), and one
#                    that lands *inside* it does not stop the write either --
#                    the option program completes, and the chip reads back
#                    "read protection: on" with osc/pointer intact;
#                  * the one earlier hit cannot be told apart from a
#                    pre-erased option area (see --rom-erase-tear below).
#                So this mode now judges a *transition*: it refuses to run
#                unless the area was canonical first, and only calls a tear
#                when the same read comes back erased. It still needs BOOT0.
#   --rom-erase-tear  the deterministic way to the state under test, and the
#                mode to reach for: `agrv32flash -O` erases the option area
#                (and resets the chip). Read back that is the torn state --
#                option area erased, read protection reads *on*, osc config
#                0xff,0xff, no FPGA pointer -- with no reset timing involved.
#                Whether a *supply* drop inside the ROM's option write reaches
#                the same place is untested (a manual power cut cannot be aimed
#                at a 3 ms window); what holds is that a chip-side reset
#                (probe nRESET here, the backup-domain IWDG in --iwdg-*) does
#                not. Needs BOOT0 fitted.
#
# What it does NOT do: recover the board. After the window it inspects (read
# only) and prints the ladder, because which rung is safe depends on what the
# chip answers -- `agm_oo.sh unlock` erases the whole chip, and the BOOT0 route
# needs a jumper. The ladder is the numbered list above.
#
# Usage
#   tools/agm_rdp_tear_test.sh                       # 30 s window, no storm
#   tools/agm_rdp_tear_test.sh --window 120 --reset-storm
#   tools/agm_rdp_tear_test.sh --kill-after 0.4
#   tools/agm_rdp_tear_test.sh --iwdg-reset --window 20
#   tools/agm_rdp_tear_test.sh --dry-run             # print the session only
#   tools/agm_rdp_tear_test.sh --inspect-only        # what state is it in now
#   tools/agm_rdp_tear_test.sh --rom-erase-tear --recover \
#       --build-dir /tmp/b_hello3 --bitstream <canonical> \
#       --salt-file <salt> --uid <32 hex>                # tear -> recover
#   tools/agm_rdp_tear_test.sh --recover --build-dir /tmp/b_hello3 \
#       --bitstream ~/spi_full_mac_bitstream_200mhz/example_board.bin \
#       --salt-file /tmp/verify_flow/salt.bin --uid 41503436…
#
# --recover is the ladder of FLASH-AND-CAPTURE 11.5/11.7 as one command:
# unlock (full erase) -> put the FPGA pointer back -> firmware + bitstream ->
# salt -> verify. It works from *any* option state, including the torn one that
# does not boot, because the option area is only ever touched over the AP path.
# Steps whose inputs are missing are skipped and said to be skipped; the point
# is not to guess where a salt file lives.
#
# Environment: as tools/agm_oo.sh (AGRV_SDK_PATH, AGM_OO, AGM_OPENOCD_BIN,
# AGM_SWD_SPEED), plus AGM_LOADER_PORT for the console check.

set -eu

WINDOW=30
STORM=0
DRY=0
INSPECT_ONLY=0
KILL_AFTER=""
IWDG_RESET=0
IWDG_AFTER=0
IWDG_TAP=0
KILL_ON_ERASE=0
KILL_DELAY=0
RECOVER=0
ROM_NRESET=""
ROM_ERASE_TEAR=0
BUILD_DIR=""
BITSTREAM=${AGM_BITSTREAM_BIN:-$HOME/spi_full_mac_bitstream_200mhz/example_board.bin}
SALT_FILE=""
SALT_UID=""

usage() { sed -n '3,40p' "$0" | sed 's/^# \{0,1\}//'; exit 2; }

while [ $# -gt 0 ]; do
	case "$1" in
	--window) WINDOW=$2; shift 2 ;;
	--reset-storm) STORM=1; shift ;;
	--kill-after) KILL_AFTER=$2; shift 2 ;;
	--iwdg-reset) IWDG_RESET=1; shift ;;
	--iwdg-after) IWDG_RESET=1; IWDG_AFTER=$2; shift 2 ;;
	--iwdg-tap) IWDG_RESET=1; IWDG_TAP=$2; shift 2 ;;
	--kill-on-erase) KILL_ON_ERASE=1; shift ;;
	--kill-delay) KILL_DELAY=$2; shift 2 ;;
	--dry-run) DRY=1; shift ;;
	--inspect-only) INSPECT_ONLY=1; shift ;;
	--recover) RECOVER=1; shift ;;
	--rom-nreset) ROM_NRESET=${2:-0.080}
		case "$ROM_NRESET" in [[:digit:]]*) shift 2 ;; *) shift ;; esac ;;
	--rom-erase-tear) ROM_ERASE_TEAR=1; shift ;;
	--build-dir) BUILD_DIR=$2; shift 2 ;;
	--bitstream) BITSTREAM=$2; shift 2 ;;
	--salt-file) SALT_FILE=$2; shift 2 ;;
	--uid) SALT_UID=$2; shift 2 ;;
	-h|--help) usage ;;
	*) usage ;;
	esac
done

if [ -z "${VIRTUAL_ENV:-}" ]; then
	echo "FATAL: no Zephyr virtualenv active" >&2
	exit 3
fi

SELF_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
AGRV_SDK_PATH=${AGRV_SDK_PATH:-$HOME/AgRV_pio}
AGM_OPENOCD_BIN=${AGM_OPENOCD_BIN:-$AGRV_SDK_PATH/packages/tool-agrv_openocd/bin}
AGRV32FLASH=${AGM_AGRV32FLASH:-$AGRV_SDK_PATH/packages/tool-agrv_flashloader/bin/agrv32flash}
AGM_SWD_SPEED=${AGM_SWD_SPEED:-10000}
AGRV_ADAPTER=${AGRV_ADAPTER:-cmsis-dap}
PORT=${AGM_LOADER_PORT:-/dev/ttyACM0}
OPENOCD=$AGM_OPENOCD_BIN/openocd_cmd

# The board's own cfg, the way tools/probe_state.sh finds it: the bare SDK cfg
# defaults to J-Link and dies with "No J-Link device found".
BOARD=${BOARD:-agrv2k_407}
AGM_HOME=${ZEPHYR_HAL_AGM_HOME:-$(dirname -- "$SELF_DIR")}
OPENOCD_CFG=${AGM_OPENOCD_CFG:-${AGM_BUILD_DIR:-$HOME/zephyrproject/build}/logic/openocd.cfg}
[ -f "$OPENOCD_CFG" ] || OPENOCD_CFG=$AGM_HOME/boards/agm/$BOARD/support/openocd.cfg
[ -f "$OPENOCD_CFG" ] || { echo "FATAL: no openocd cfg at $OPENOCD_CFG" >&2; exit 3; }
export AGRV_ADAPTER AGM_SWD_SPEED

say() { echo ">>> $*" >&2; }

# One session, N back-to-back option writes. The count is derived by timing a
# short burst first, so --window means seconds on the wall rather than a guess
# at how long `agrv lock 0` takes.
# The session as a command string, so it can be run directly, in a child shell,
# or in its own process group (for --kill-after: the wrapper is a plain shell
# script that does not exec openocd, so the kill has to take the group). No
# `exec` here: the window branch appends a sentinel after it, which is the only
# reliable "the session is over" signal -- a dead child stays visible to
# kill -0 until something waits for it, so the first cut of the countdown loop
# span forever on a zombie.
session_command() {
	count=$1
	pre=$2
	cmds="init; reset init"
	[ -n "$pre" ] && cmds="$cmds; $pre"
	i=0
	while [ "$i" -lt "$count" ]; do
		cmds="$cmds; echo [agrv lock 0]"
		i=$((i + 1))
		[ -n "$ARM_AT" ] && [ "$i" = "$ARM_AT" ] && cmds="$cmds; $PRE_ARM"
	done
	cmds="$cmds; shutdown"
	printf '"%s" -f "%s" -c "%s"' "$OPENOCD" "$OPENOCD_CFG" "$cmds"
}

# The IWDG as drivers/watchdog/wdt_iwdg_agm.c arms it, spelled out as openocd
# commands: pulse BDRST (without it the backup-domain latch drops every IWDG
# write -- the driver's comment says so and the dev board agrees), enable RTCEN +
# RTCSEL = LSI, then touch the IWDG register by *read-modify-write*, because a
# bare write to it is silently dropped on this IP. Prescaler 0 = /2 = the 64 ms
# tap (LSI nominal 32 kHz); the ENABLE bit makes that the whole reset window.
iwdg_arm_cmds() {
	# ${IWDG_TAP} is the 3-bit prescaler field: 0 = /2 = the 64 ms tap, 7 =
	# /256 = 8192 ms (nominal, LSI 32 kHz). The tap is the phase knob for
	# aiming the reset into the erase->program gap.
	cat <<TCL
proc rtoff {} { while {([mrh 0x40000004] & 0x20) == 0} {} }
rtoff
mwh 0x40000032 1
sleep 10
mwh 0x40000032 0
sleep 10
rtoff
mwh 0x40000000 0
rtoff
mwh 0x40000004 0
sleep 10
rtoff
mwh 0x40000030 0x8200
sleep 10
rtoff
mwh 0x40000034 [expr {([mrh 0x40000034] & ~7) | ${IWDG_TAP}}]
rtoff
mwh 0x40000034 [expr {[mrh 0x40000034] | 0x100}]
rtoff
mwh 0x40000034 [expr {([mrh 0x40000034] & ~0xf000) | 0xa000}]
TCL
}

# Clear the ENABLE bit again, or the chip resets every 64 ms for ever (the
# backup domain survives the reset that the watchdog itself caused). Both
# writes wait for CRL.RTOFF first: the backup register file latches one write
# at a time and a write that arrives too early is dropped -- that is what
# drivers/watchdog/wdt_iwdg_agm.c::rtc_wait_write() polls for, and why a bare
# single write here could come back with ENABLE still set.
iwdg_disarm_cmds() {
	cat <<'TCL'
proc rtoff {} { while {([mrh 0x40000004] & 0x20) == 0} {} }
rtoff
mwh 0x40000034 [expr {[mrh 0x40000034] & ~0x100}]
rtoff
TCL
}

iwdg_disarm() {
	cmds="init; $(iwdg_disarm_cmds | tr '\n' ' '); shutdown"
	"$OPENOCD" -f "$OPENOCD_CFG" -c "$cmds" >/dev/null 2>&1 || true
}

# Can the arm take at all? Read the register first: with the backup-domain gate
# off the writes are dropped (and it reads 0x0000), and with RDP on the access
# fails outright. Either way the reset this mode is built around cannot happen,
# so say so up front instead of leaving a silent no-op.
iwdg_check() {
	out=$("$OPENOCD" -f "$OPENOCD_CFG" -c "init; reset init" \
		-c "echo IWDG; mrh 0x40000034" -c shutdown 2>&1 || true)
	value=$(printf '%s\n' "$out" | grep -A1 "IWDG" | tail -1 | tr -d ' \r')
	case "$value" in
	0x0000|0x0|"")
		say "the IWDG register reads ${value:-nothing}: the arm was dropped." \
		    "The backup domain is not clocked (no &rtc0/&iwdg0 in this build)" \
		    "or the chip is read-protected -- use a build with the RTC gate" \
		    "on and unlock first."
		;;
	*)
		say "IWDG register before the window: $value (backup domain is reachable)"
		;;
	esac
}

option_write_session() {
	sh -c "$(session_command "$1")"
}

timed_writes() {
	start=$(date +%s%N)
	option_write_session "$1" >/dev/null 2>&1 || true
	end=$(date +%s%N)
	echo $(( (end - start) / 1000000 ))	# ms
}

# Milliseconds per write *inside* a session, which is what the window is made
# of: a session's own startup (~300 ms) would otherwise be spread over the
# writes and the count comes out far too small (a "60 s" window ran 6 s
# of writes).
per_write_ms() {
	t50=$(timed_writes 50)
	t100=$(timed_writes 100)
	echo $(( (t100 - t50) / 50 ))
}

inspect() {
	say "state now (read-only)"
	sh "$SELF_DIR/probe_state.sh" 2>&1 | tail -12 || true
	say "option bytes"
	sh "$SELF_DIR/agm_oo.sh" info 2>&1 | grep -E "option byte reg|read protection|osc config|fpga" || true
	say "SWD read of the salt sector (0x800b0000)"
	tmp=$(mktemp)
	if sh "$SELF_DIR/agm_oo.sh" read "$tmp" 32 0x800b0000 >/dev/null 2>&1; then
		xxd -l 16 "$tmp"
	else
		echo "    read failed (that is what a protected chip looks like)"
	fi
	rm -f "$tmp"
	say "console"
	sh "$SELF_DIR/test_uart_capture.sh" -d "$PORT" -n -t 4 2>&1 | grep -aE "bytes =|RESULT|hello_world|loader" | head -4 || true
}

if [ "$INSPECT_ONLY" = "1" ]; then
	inspect
	exit 0
fi

# The ROM-side modes both run before the recovery block on purpose, so
# `--rom-erase-tear --recover` (or `--rom-nreset S --recover`) means "tear it,
# then put it back" in one command.
TROM_RC=""

# A tear verdict has to be a *transition*, not a snapshot. The erased option
# area reads as "read protection: on, osc config 0xff,0xff, no FPGA pointer",
# and so does a chip that was simply left that way by an earlier unlock or
# `agrv32flash -O` -- which is how an earlier single observation came to be
# written down as a reproduced tear. Every judging mode below therefore reads
# the area first, requires it to be canonical, and only then compares.
canonical_state() {
	case "$1" in
	*"read protection: off"*"0xff, 0x57"*"configuration address = 0x"*)
		return 0 ;;
	*)	return 1 ;;
	esac
}

erased_state() {
	case "$1" in
	*"0xff, 0xff"*|*"not valid"*) return 0 ;;
	*)	return 1 ;;
	esac
}

if [ -n "$ROM_NRESET" ] || [ "$ROM_ERASE_TEAR" = "1" ]; then
	rom_info=$("$AGRV32FLASH" -b 57600 "$PORT" 2>&1 || true)
	case "$rom_info" in
	*Device*)
		;;
	*)
		say "the ROM loader does not answer on $PORT -- fit the BOOT0 jumper"
		say "and power-cycle (or pulse nRESET); that is what puts the chip in"
		say "the ROM loader, which this mode needs. It said:"
		printf '%s\n' "$rom_info" | tail -3 >&2
		exit 4
		;;
	esac

	base_state=$(sh "$SELF_DIR/agm_oo.sh" info 2>&1 |
		grep -E "read protection|osc config|fpga config" | tr '\n' ' ')
	say "option area before the attempt:"
	printf '    %s\n' "$base_state" >&2
	if ! canonical_state "$base_state"; then
		say "that is not a canonical option area, and an already-erased one"
		say "reads exactly like a tear -- so this mode will not judge from the"
		say "end state alone. Put it back first:"
		say "    tools/agm_oo.sh bitstream <canonical>   (or the --recover ladder)"
		exit 4
	fi

	if [ "$ROM_ERASE_TEAR" = "1" ]; then
		say "tear: agrv32flash -O (erase the option area, then reset)"
		"$AGRV32FLASH" -b 57600 -O "$PORT" 2>&1 | tail -2
		sleep 1
		state=$(sh "$SELF_DIR/agm_oo.sh" info 2>&1 |
			grep -E "read protection|osc config|fpga config" | tr '\n' ' ')
		say "state after the option erase:"
		printf '    %s\n' "$state" >&2
		if erased_state "$state"; then
			say "TEAR STATE REACHED -- the option area went from canonical to"
			say "erased: read protection reads on, osc config 0xff,0xff, no"
			say "FPGA pointer, and the board does not boot (that is the state"
			say "under test). This is the deterministic route: no reset timing."
			TROM_RC=0
		else
			say "the option erase did not leave the erased state?"
			TROM_RC=1
		fi
		if [ "$RECOVER" != "1" ]; then
			exit "$TROM_RC"
		fi
	fi
fi

if [ -n "$ROM_NRESET" ]; then
	say "tear: agrv32flash -j in the background, probe nRESET ${ROM_NRESET}s in"
	"$AGRV32FLASH" -b 57600 -j "$PORT" > /tmp/agm_rdp_tear.rom 2>&1 &
	rom=$!
	sleep "$ROM_NRESET"
	"${PYTHON:-python3}" "$SELF_DIR/probe_reset_target.py" >/dev/null 2>&1 || true
	wait "$rom" 2>/dev/null || true
	sleep 1

	state=$(sh "$SELF_DIR/agm_oo.sh" info 2>&1 |
		grep -E "read protection|osc config|fpga config" | tr '\n' ' ')
	say "state after the aimed reset:"
	printf '    %s\n' "$state" >&2
	if erased_state "$state"; then
		say "TEAR -- the option area went canonical -> erased with a reset"
		say "inside the write. (: this has not reproduced"
		say "in 15 attempts from a canonical area -- see the header for why"
		say "the aim cannot work; a hit here is worth writing down with the"
		say "ROM tool's own timestamps.)"
		TROM_RC=0
	else
		say "no tear: the option write completed (read protection on with"
		say "osc/pointer intact). That is what the aimed reset does every"
		say "time here -- the ROM's option program is not stopped by it."
		TROM_RC=1
	fi
	if [ "$RECOVER" != "1" ]; then
		exit "$TROM_RC"
	fi
fi

if [ "$RECOVER" = "1" ]; then
	WS=${AGM_WORKSPACE:-$HOME/zephyrproject}

	say "recover 1/5: unlock (this is the full erase the ladder starts with)"
	# A chip that was just torn over the ROM path is in the ROM loader, so the
	# ROM's own unlock is the one that is certainly reachable; fall back to the
	# SWD one when the ROM does not answer (that is the ladder's own order).
	if "$AGRV32FLASH" -b 57600 "$PORT" 2>&1 | grep -q "Device"; then
		"$AGRV32FLASH" -b 57600 -k "$PORT" 2>&1 | tail -1
	else
		sh "$SELF_DIR/agm_oo.sh" unlock 2>&1 | grep -a "agrv unlocked" || true
	fi

	say "recover 2/5: put the FPGA pointer back (also erases the array)"
	if [ -f "$BITSTREAM" ]; then
		sh "$SELF_DIR/agm_oo.sh" bitstream "$BITSTREAM" 2>&1 | tail -1
	else
		say "    no bitstream at $BITSTREAM -- pass --bitstream; the option"
		say "    pointer stays invalid and the ROM will not configure the fabric"
	fi

	say "recover 3/5: firmware + bitstream over SWD"
	if [ -n "$BUILD_DIR" ] && [ -f "$BITSTREAM" ]; then
		( cd "$WS" && AGM_BITSTREAM_BIN="$BITSTREAM" west flash -d "$BUILD_DIR" ) 2>&1 | tail -2
	else
		say "    skipped (need --build-dir <hello_world build> and --bitstream)"
	fi

	say "recover 4/5: salt"
	if [ -n "$SALT_FILE" ] && [ -n "$SALT_UID" ]; then
		"${PYTHON:-python3}" "$SELF_DIR/agm_bind.py" provision \
			--salt-file "$SALT_FILE" --uid "$SALT_UID" 2>&1 | grep -E "wrote|key "|| true
	else
		say "    skipped (need --salt-file and --uid); a board that shipped"
		say "    bound cannot accept images until the salt is back"
	fi

	say "recover 5/5: verify"
	inspect
	# With --rom-nreset in front, the run's verdict is whether the tear was
	# produced (the recovery is the second half of the same test).
	exit "${TROM_RC:-0}"
fi

say "baseline"
sh "$SELF_DIR/agm_oo.sh" info 2>&1 | grep -E "option byte reg|read protection|fpga" || true

PRE=""
PRE_ARM=""
ARM_AT=""
if [ "$IWDG_RESET" = "1" ]; then
	PRE_ARM="$(iwdg_arm_cmds | tr '\n' ' ')"
	if [ "$IWDG_AFTER" -gt 0 ]; then
		ARM_AT=$IWDG_AFTER
		say "iwdg-reset: arming the 64 ms watchdog after ${ARM_AT} completed" \
		    "writes, so the reset lands inside write $((ARM_AT + 1))"
	else
		PRE="$PRE_ARM"
		say "iwdg-reset: arming the backup-domain watchdog (tap ${IWDG_TAP}) right before" \
		    "the writes, so a hardware reset lands inside the first one"
	fi
	iwdg_check
fi

say "timing the writes (a 50- and a 100-write session)..."
per_ms=$(per_write_ms)
[ "$per_ms" -gt 0 ] || per_ms=3
count=$(( WINDOW * 1000 / per_ms ))
[ "$count" -lt 50 ] && count=50
say "${per_ms} ms per write -> ${count} writes for a ~${WINDOW} s window"

if [ "$DRY" = "1" ]; then
	say "dry run: the session would be"
	cmds="init; reset init"
	i=0; while [ "$i" -lt 3 ]; do cmds="$cmds; echo [agrv lock 0]"; i=$((i+1)); done
	echo "    $OPENOCD ... -c '${cmds}; ... (${count} writes) ...; shutdown'"
	exit 0
fi

STORM_PID=""
if [ "$STORM" = "1" ]; then
	say "reset storm: pulsing nRESET from the probe while the window is open"
	(
		while :; do
			python3 "$SELF_DIR/probe_reset_target.py" >/dev/null 2>&1 || true
		done
	) &
	STORM_PID=$!
	echo "0000" > /dev/null	# keep set -e quiet about the subshell
fi

say "WINDOW IS OPEN for ~${WINDOW} s -- cut the power now if that is the test"
say "(the session below is ${count} option writes back to back)"
if [ "$KILL_ON_ERASE" = "1" ]; then
	# Aim at the gap: the driver prints "Option bytes are erased" after the
	# erase and only then triggers the program. The kill has to arrive within a
	# millisecond or two, which is why the watcher is Python and reads the pipe
	# rather than the log (the shell version missed it every time).
	say "kill-on-erase: killing the session inside the erase->program gap" \
	    "(delay ${KILL_DELAY}s)"
	"${PYTHON:-python3}" "$SELF_DIR/kill_on_pattern.py" \
		--pattern "Option bytes are erased" \
		--delay "$KILL_DELAY" \
		--log /tmp/agm_rdp_tear.session \
		--shell "$(session_command "$count" "$PRE")" || true
	sleep 1
elif [ -n "$KILL_AFTER" ]; then
	say "kill-after ${KILL_AFTER}s: SIGKILLing the session (process group) mid-write"
	setsid sh -c "$(session_command "$count" "$PRE")" > /tmp/agm_rdp_tear.session 2>&1 &
	sess=$!
	sleep "$KILL_AFTER"
	kill -9 -"$sess" 2>/dev/null || kill -9 "$sess" 2>/dev/null || true
	wait "$sess" 2>/dev/null || true
	sleep 1
elif [ "$WINDOW" -ge 30 ]; then
	# A window an operator is expected to cut: run it in the background and
	# report progress, so "cut now" has a visible clock (and a counter that
	# says the writes really are happening).
	setsid sh -c "$(session_command "$count" "$PRE"); echo AGM_SESSION_DONE" \
		> /tmp/agm_rdp_tear.session 2>&1 &
	sess=$!
	i=0
	while ! grep -q AGM_SESSION_DONE /tmp/agm_rdp_tear.session 2>/dev/null &&
		[ "$i" -lt $((WINDOW + 120)) ]; do
		done_writes=$(grep -c "agrv locked" /tmp/agm_rdp_tear.session 2>/dev/null || true)
		printf '    ... %ss elapsed, %s option writes done -- CUT THE POWER NOW\n' \
			"$((i * 5))" "${done_writes:-0}" >&2
		sleep 5
		i=$((i + 1))
	done
	kill -0 "$sess" 2>/dev/null && kill -9 -"$sess" 2>/dev/null
	wait "$sess" 2>/dev/null || true
else
	sh -c "$(session_command "$count" "$PRE")" > /tmp/agm_rdp_tear.session 2>&1 || true
fi

if [ -s /tmp/agm_rdp_tear.session ]; then
	grep -c "agrv locked" /tmp/agm_rdp_tear.session | sed 's/^/    option writes reported locked: /' >&2
	case "$(tail -1 /tmp/agm_rdp_tear.session)" in
	*"shutdown command invoked"*) ;;
	*) say "session did not reach its end -- last lines:"; tail -3 /tmp/agm_rdp_tear.session >&2 ;;
	esac
else
	say "session produced no output"
fi

if [ -n "$STORM_PID" ]; then
	kill "$STORM_PID" 2>/dev/null || true
	wait "$STORM_PID" 2>/dev/null || true
fi

if [ "$IWDG_RESET" = "1" ]; then
	say "disarming the IWDG (otherwise the chip resets every 64 ms)"
	iwdg_disarm
fi

say "window closed"
inspect
cat >&2 <<'EOF'
>>> recovery ladder (run by hand; the steps are above):
    1. SWD alive?  tools/agm_oo.sh unlock        (unlock = full erase, by design)
    2. SWD dead?   BOOT0 fitted + power cycle -> agrv32flash -k (or -L), then
                   tools/agm_oo.sh bitstream <canonical> to put the FPGA
                   pointer back (that write erases the array: order matters)
    3. restore     west flash (firmware + bitstream), then
                   tools/agm_bind.py provision --salt-file <salt> --uid <uid>
EOF
