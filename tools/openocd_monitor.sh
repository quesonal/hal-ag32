#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# openocd_monitor.sh — Foreground OpenOCD session with parallel
#                      semihosting + UART capture.
#
# Use case
#   Run OpenOCD as a foreground process so semihosting output (SDK
#   `printstr` magic-memory writes — see agmv2k-sdk-semihosting-vs-pl011)
#   shows up directly in the terminal where OpenOCD runs. Simultaneously
#   captures /dev/ttyACM0 (PL011 → USB-UART bridge) so Zephyr printk is
#   also visible, side by side.
#
#   This is the script the user was running when they said
#   "sdk example uart 我可以使用 openocd monitor 查看到数据".
#
# Usage
#   ./openocd_monitor.sh [board]
#   ./openocd_monitor.sh agrv2k_407
#
# Bounded duration: wrap with `timeout`
#   timeout 30 ./openocd_monitor.sh agrv2k_407
#   timeout --foreground 60 ./openocd_monitor.sh
#
# What it does
#   1. (Re-)bind cdc_acm so /dev/ttyACM0 is fresh
#   2. Print clear banner with tagged-line legend
#   3. Background cat /dev/ttyACM0 → tagged UART log file
#   4. Foreground openocd_cmd with -c "init" -c "reset"
#      - OpenOCD's stdout is tee'd into the terminal (with [OCD] tag)
#        AND into a stable log file
#      - We `exec` into openocd_cmd so this bash script becomes
#        openocd; SIGINT/Ctrl-C lands cleanly on openocd
#   5. On exit (openocd terminated, Ctrl-C, or outer timeout):
#      cleanup trap kills the UART reader and prints capture summary
#
# Notes
#   - OpenOCD's `-c "init"` examines the target but does NOT run it.
#     After flash the CPU is halted; this script auto-issues
#     `-c "reset run"` (note the `run` — without it the CPU stays
#     halted after reset and no code executes, hence no UART).
#   - OpenOCD listens on telnet 4444 (tcl: mdw / mww / reset / etc.)
#     and gdb 3333 (riscv32-elf-gdb attach).
#   - While this script runs, CMSIS-DAP Vendor If 0 is held by
#     openocd for SWD. CDC-ACM If 1+2 remain bound to cdc_acm so
#     PL011 → host UART keeps flowing in parallel.
#
# Requires
#   $AGRV_SDK_PATH/packages/tool-agrv_openocd/bin/openocd_cmd
#   (AGRV_SDK_PATH defaults to $HOME/AgRV_pio)
#   AGRV_ADAPTER env var (default cmsis-dap)

set -u
set -o pipefail

# ---------- knobs -----------------------------------------------------------
BOARD="${1:-agrv2k_407}"
AGRV_SDK_PATH="${AGRV_SDK_PATH:-$HOME/AgRV_pio}"
AGRV_ADAPTER="${AGRV_ADAPTER:-cmsis-dap}"
OPENOCD_CMD="$AGRV_SDK_PATH/packages/tool-agrv_openocd/bin/openocd_cmd"

# This script lives in <module>/tools/, so the module root is its parent.
SELF_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ZEPHYR_HAL_AGM_HOME="${ZEPHYR_HAL_AGM_HOME:-$(dirname -- "$SELF_DIR")}"
# Workspace first -- see the note in openocd_reset_run.sh.
SUPPORT_CFG=""
for _cand in "${AGM_WORKSPACE:-$HOME/zephyrproject}/modules/hal_ag32" \
	     "$ZEPHYR_HAL_AGM_HOME" "$HOME/zephyr-hal-ag32"; do
	if [ -f "$_cand/zephyr/module.yml" ]; then
		SUPPORT_CFG="${AGM_OPENOCD_CFG:-}"
		[ -n "$SUPPORT_CFG" ] || SUPPORT_CFG="${AGM_BUILD_DIR:+${AGM_BUILD_DIR}/logic/openocd.cfg}"
		[ -n "$SUPPORT_CFG" ] || SUPPORT_CFG=$(ls -t "${AGM_WORKSPACE:-$HOME/zephyrproject}"/build*/logic/openocd.cfg /tmp/b_*/logic/openocd.cfg 2>/dev/null | head -1)
		[ -n "$SUPPORT_CFG" ] || SUPPORT_CFG="$_cand/boards/agm/${BOARD}/support/openocd.cfg"
		break
	fi
done

TS="$(date +%Y%m%d_%H%M%S)"
UART_LOG="/tmp/uart_monitor_${BOARD}_${TS}.bin"
SEMI_LOG="/tmp/semihost_${BOARD}_${TS}.log"

# ---------- preflight -------------------------------------------------------
echo "=== openocd_monitor.sh ==="
echo "board        = $BOARD"
echo "uart_log     = $UART_LOG"
echo "semihost_log = $SEMI_LOG"
echo "adapter      = $AGRV_ADAPTER"
echo "support_cfg  = $SUPPORT_CFG"
echo "(wrap with 'timeout N' for bounded duration)"
echo ""

if [ ! -x "$OPENOCD_CMD" ]; then
    echo "FATAL: openocd_cmd not found at $OPENOCD_CMD" >&2
    exit 1
fi
if [ ! -f "$SUPPORT_CFG" ]; then
    echo "FATAL: openocd support cfg not found at $SUPPORT_CFG" >&2
    exit 1
fi

# ---------- 1. ensure cdc_acm is bound -------------------------------------
if ! lsusb -t 2>/dev/null | grep -E 'cafe|Class=Communications.*cdc_acm' >/dev/null; then
    echo "[1] cdc_acm NOT bound, attempting rebind..."
    SUDO=$([ "$(id -u)" -ne 0 ] && echo sudo || echo)
    $SUDO modprobe cdc_acm 2>&1 | tail -3
    for devdir in /sys/bus/usb/devices/*; do
        [ -f "$devdir/idVendor" ] || continue
        if [ "$(cat "$devdir/idVendor" 2>/dev/null)" = "cafe" ]; then
            for intf in "$devdir"/*/intf:*:1.*; do
                [ -e "$intf" ] || continue
                intf_name="$(basename "$(dirname "$intf")")"
                echo "$intf_name" | $SUDO tee /sys/bus/usb/drivers/cdc_acm/bind \
                    >/dev/null 2>&1 && echo "  bound $intf_name"
            done
        fi
    done
    sleep 0.3
fi

if [ ! -e /dev/ttyACM0 ]; then
    echo "FATAL: /dev/ttyACM0 missing after rebind" >&2
    exit 1
fi

# ---------- 2. serial port setup -------------------------------------------
stty -F /dev/ttyACM0 115200 cs8 -cstopb -parenb -crtscts raw 2>&1 || {
    echo "FATAL: stty failed" >&2; exit 1; }

# ---------- 3. clear terminal banner ----------------------------------------
cat <<'EOF'
=================================================================
  openocd_monitor.sh running
=================================================================
  Each line tagged so you know its source:
    [OCD ]  OpenOCD log line (Info / Error / target state)
    [UART]  PL011 → AFSEL → PIN_68 → /dev/ttyACM0 (Zephyr printk)
  Telnet 4444 = OpenOCD tcl monitor (mdw / mww / reset / etc.)
  GDB    3333 = gdbserver for riscv32-unknown-elf-attach
  Ctrl-C = clean shutdown (also kills openocd)
=================================================================

EOF

# ---------- 4. cleanup trap -------------------------------------------------
# Final cleanup is done in the main flow (after `wait $OCD_PID` returns).
# The trap just forwards signals to openocd so it exits cleanly and
# triggers the natural cleanup path.
OCD_PID=""
forward_signal() {
    if [ -n "$OCD_PID" ] && kill -0 "$OCD_PID" 2>/dev/null; then
        kill -TERM "$OCD_PID" 2>/dev/null
    fi
}
trap forward_signal INT TERM

# ---------- 5. UART reader (background) ------------------------------------
echo "[UART-MON] starting /dev/ttyACM0 capture → $UART_LOG"
( cat /dev/ttyACM0 2>/dev/null | \
  stdbuf -oL awk '{printf("[UART %s] %s\n", strftime("%H:%M:%S"), $0); fflush()}' \
  >> "$UART_LOG" ) &
UART_PID=$!

# ---------- 6. openocd as foreground child (background for clean signal handling) -
# We background openocd, capture its PID, then `wait` for it. SIGINT (Ctrl-C)
# is caught by the trap, which forwards TERM to openocd, drains, and exits.
# The pipeline tee'ing output uses process substitution so the tee+awk live
# as a side channel that dies when openocd's stdout closes.
echo "[OCD-MON] launching openocd (init + reset run, listens on 4444/3333)..."
export AGRV_ADAPTER
"$OPENOCD_CMD" \
    -f "$SUPPORT_CFG" \
    -c "init" \
    -c "reset run" \
    > >(stdbuf -oL tee >(stdbuf -oL awk '{ "date +%H:%M:%S" | getline ts; close("date +%H:%M:%S"); printf("[OCD  %s] %s\n", ts, $0); fflush()}') \
                        > "$SEMI_LOG") \
    2>&1 &
OCD_PID=$!
echo "[OCD-MON] openocd PID=$OCD_PID"
# Now that OCD_PID is known, the INT/TERM trap can find it

# Wait for openocd. Trap (set above) handles INT/TERM/EXIT cleanup.
wait "$OCD_PID"
OCD_RC=$?

# ---------- 7. final cleanup (also reached via trap on Ctrl-C) --------------
echo ""
echo "[$(date +%H:%M:%S)] openocd exited rc=$OCD_RC"

if [ -n "$UART_PID" ]; then
    kill "$UART_PID" 2>/dev/null
    wait "$UART_PID" 2>/dev/null
fi
echo ""
echo "================================================================="
echo " Captures saved:"
echo "   semihost : $SEMI_LOG    ($(wc -c < "$SEMI_LOG" 2>/dev/null || echo 0) bytes)"
echo "   uart     : $UART_LOG    ($(wc -c < "$UART_LOG" 2>/dev/null || echo 0) bytes)"
echo "================================================================="

exit $OCD_RC
