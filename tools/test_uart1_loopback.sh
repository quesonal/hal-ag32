#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# test_uart1_loopback.sh — register-level UART1 loopback test for AgRV2K.
#
# Runs three checks with the CPU halted through openocd, then leaves the
# target running again (`reset run`):
#
#   A. identity   — read the PL011 PeriphID/CellID of UART0 and UART1.
#                   A real PL011 answers 11 10 04 00 / 0d f0 05 b1.
#                   An all-zero answer means no peripheral at that address
#                   (UART2..4 on this bitstream behave that way).
#
#   B. fabric/jumper route — drive GPIO8_0 as a *plain* GPIO and read
#                   GPIO6_3 as a *plain* GPIO. This bypasses the UART
#                   entirely, so it only proves the bitstream wiring
#                   (PIN_67 <- GPIO8_0, PIN_66 -> GPIO6_3) plus the
#                   external jumper. Failure here = bitstream or jumper.
#
#   C. loopback    — program PL011 #1 (0x40026000) and send one byte:
#                     lbe : internal loopback (CR.LBE) — no pins involved;
#                           isolates the UART IP + baud clock.
#                     ext : AF on GPIO8_0 (TX) / GPIO6_3 (RX) — the byte
#                           leaves on PIN_67, crosses the jumper to PIN_66
#                           and comes back into the RX FIFO.
#
# Wiring
#   `ext` needs a jumper between PIN_66 and PIN_67. board.ve maps
#       UART1_UARTTXD PIN_67   (MCU side GPIO8_0 / 0x4001C420 bit 0)
#       UART1_UARTRXD PIN_66   (MCU side GPIO6_3 / 0x4001A420 bit 3)
#   These pins are *not* direct GPIO pins: the signals reach them through
#   the FPGA fabric, so check B is what validates the route.
#
# Usage
#   ./test_uart1_loopback.sh [-b board] [-m lbe|ext|both] [-k]
#
#   -b <board>   board name            (default agrv2k_407)
#   -m <mode>    lbe | ext | both      (default both)
#   -k           keep the CPU halted at the end instead of `reset run`
#
# Exit codes
#   0  all requested checks passed
#   1  UART1 loopback failed
#   2  fabric/jumper route failed (bitstream or wiring)
#   3  preflight failure
#
# Prerequisites
#   * an active Zephyr virtualenv (repo venv; no system python)
#   * probe plugged in (cmsis-dap), not wedged by a killed openocd
#
# Note: this script never writes FLASH, so the FPGA bitstream region
# (0x800E7000..) is untouched by definition.

set -u
set -o pipefail

BOARD="agrv2k_407"
MODE="both"
KEEP_HALTED=0

while getopts "b:m:kh" opt; do
	case "$opt" in
	b) BOARD="$OPTARG" ;;
	m) MODE="$OPTARG" ;;
	k) KEEP_HALTED=1 ;;
	h) sed -n '2,60p' "$0"; exit 0 ;;
	*) echo "bad option; try -h" >&2; exit 3 ;;
	esac
done

case "$MODE" in
lbe | ext | both) ;;
*) echo "FATAL: -m must be lbe, ext or both" >&2; exit 3 ;;
esac

AGRV_SDK_PATH="${AGRV_SDK_PATH:-$HOME/AgRV_pio}"
AGRV_ADAPTER="${AGRV_ADAPTER:-cmsis-dap}"
OPENOCD_CMD="$AGRV_SDK_PATH/packages/tool-agrv_openocd/bin/openocd_cmd"
PLATFORM_ETC="${AGRV_PLATFORM_ETC:-$AGRV_SDK_PATH/platforms/AgRV/etc}"

# ---------- locate hal_ag32 tree (board support cfg) -------------------------
SELF_DIR="$(cd "$(dirname "$0")" && pwd)"
AGM_HOME="${ZEPHYR_HAL_AGM_HOME:-}"
if [ -z "$AGM_HOME" ]; then
	# Workspace first -- see the same note in test_uart_capture.sh.
	for cand in "${AGM_WORKSPACE:-$HOME/zephyrproject}/modules/hal_ag32" \
		    "$HOME/zephyr-hal-ag32" "$SELF_DIR/.."; do
		# The module tree, found by a file that is always in it: the rendered
		# cfg is a build artifact now ( <build>/logic/openocd.cfg).
		if [ -f "$cand/zephyr/module.yml" ]; then
			AGM_HOME="$(cd "$cand" && pwd)"
			break
		fi
	done
fi
SUPPORT_CFG="${AGM_OPENOCD_CFG:-}"
[ -n "$SUPPORT_CFG" ] || SUPPORT_CFG="${AGM_BUILD_DIR:+${AGM_BUILD_DIR}/logic/openocd.cfg}"
[ -n "$SUPPORT_CFG" ] || SUPPORT_CFG=$(ls -t "${AGM_WORKSPACE:-$HOME/zephyrproject}"/build*/logic/openocd.cfg /tmp/b_*/logic/openocd.cfg 2>/dev/null | head -1)
[ -n "$SUPPORT_CFG" ] || SUPPORT_CFG="$AGM_HOME/boards/agm/$BOARD/support/openocd.cfg"

echo "=== test_uart1_loopback.sh ==="
echo "board        = $BOARD"
echo "mode         = $MODE"
echo "adapter      = $AGRV_ADAPTER"
echo "hal_ag32 home = ${AGM_HOME:-<not found>}"
echo "support_cfg  = $SUPPORT_CFG"

# ---------- preflight -------------------------------------------------------
if [ -z "${VIRTUAL_ENV:-}" ]; then
	echo "FATAL: no Zephyr virtualenv active - activate the repo venv first" >&2
	exit 3
fi
[ -x "$OPENOCD_CMD" ] || { echo "FATAL: openocd_cmd not found at $OPENOCD_CMD" >&2; exit 3; }
[ -f "$SUPPORT_CFG" ] || { echo "FATAL: board support cfg not found at $SUPPORT_CFG" >&2; exit 3; }

# ---------- register map ----------------------------------------------------
# SYS   0x03000000  APB_CLKENABLE +0x60 (UART1 = bit22, GPIO8 = bit12)
# GPIO6 0x4001A000  DIR +0x400, AFSEL +0x420, DATA[bit] = +4*bit
# GPIO8 0x4001C000  DIR +0x400, AFSEL +0x420, DATA[bit] = +4*bit
# UART1 0x40026000  DR +0x00, FR +0x18, IBRD +0x24, FBRD +0x28,
#                   LCR_H +0x2C, CR +0x30
#   CR 0x301 = UARTEN|TXE|RXE        0x381 = same + LBE
#   FR bit4 = RXFE (0 = byte arrived)  bit3 = BUSY  bit7 = TXFE
#   Divisor 108.5 (IBRD 0x6c / FBRD 0x20) matches the UART0 console; the
#   loopback TX/RX share it, so the absolute baud rate does not matter.

TCL="$(mktemp /tmp/uart1_loopback_XXXXXX.tcl)"
trap 'rm -f "$TCL"' EXIT

cat > "$TCL" <<'TCL_EOF'
proc r {a} { return [mrw $a] }

# ---------- 0. APB clocks ---------------------------------------------------
# Mandatory first step. The GPIO bank and UART register files are gated by
# APB_CLKENABLE; without the bank clock the AFSEL write silently no-ops, and
# without the UART clock the register file reads back all zero (UART2..4 on
# this bitstream read 0 for exactly that reason).
set apb [r 0x03000060]
mww 0x03000060 [expr {$apb | 0x1000 | 0x400000}]   ;# GPIO8 bit12, UART1 bit22
sleep 20
echo "CLOCKS APB_CLKENABLE=[r 0x03000060]"

proc ident {name base} {
    set p0 [r [expr {$base + 0xfe0}]]
    set p1 [r [expr {$base + 0xfe4}]]
    set p2 [r [expr {$base + 0xfe8}]]
    set p3 [r [expr {$base + 0xfec}]]
    set c0 [r [expr {$base + 0xff0}]]
    set c3 [r [expr {$base + 0xffc}]]
    echo "IDENT $name PeriphID=$p0 $p1 $p2 $p3 CellID=$c0 .. $c3"
    if {$p0 == 0x11 && $p1 == 0x10 && $p2 == 0x04 && $c0 == 0x0d && $c3 == 0xb1} {
        echo "IDENT $name PL011=yes"
    } else {
        echo "IDENT $name PL011=no"
    }
}

# ---------- A. identity ----------------------------------------------------
ident UART0 0x40025000
ident UART1 0x40026000

# ---------- B. fabric route + jumper, plain GPIO ---------------------------
mww 0x4001C400 [expr {[r 0x4001C400] | 0x01}]     ;# GPIO8_0 -> output
mww 0x4001C420 [expr {[r 0x4001C420] & ~0x01}]    ;# GPIO8_0 -> software mode
mww 0x4001A400 [expr {[r 0x4001A400] & ~0x08}]    ;# GPIO6_3 -> input
mww 0x4001A420 [expr {[r 0x4001A420] & ~0x08}]    ;# GPIO6_3 -> software mode

set route_ok 1
for {set i 0} {$i < 3} {incr i} {
    mww 0x4001C004 0x01
    sleep 20
    set hi [r 0x4001A020]
    mww 0x4001C004 0x00
    sleep 20
    set lo [r 0x4001A020]
    echo "ROUTE drive1 GPIO6_3=$hi drive0 GPIO6_3=$lo"
    if {$hi != 0x08 || $lo != 0x00} { set route_ok 0 }
}
if {$route_ok} { echo "ROUTE PASS" } else { echo "ROUTE FAIL" }

# ---------- C1. internal loopback (LBE) ------------------------------------
proc lbe {name base} {
    mww [expr {$base + 0x30}] 0x000
    mww [expr {$base + 0x2c}] 0x070
    mww [expr {$base + 0x24}] 0x06c
    mww [expr {$base + 0x28}] 0x020
    mww [expr {$base + 0x30}] 0x381
    mww [expr {$base + 0x00}] 0x41
    for {set i 0} {$i < 40} {incr i} {
        if { [expr {[r [expr {$base + 0x18}]] & 0x10}] == 0 } {
            set dr [r [expr {$base + 0x00}]]
            echo "LBE $name DR=$dr"
            if {$dr == 0x41} { echo "LBE $name PASS" } else { echo "LBE $name FAIL" }
            return
        }
        sleep 5
    }
    echo "LBE $name FAIL FR=[r [expr {$base + 0x18}]]"
}

# ---------- C2. external loopback through PIN_66/67 ------------------------
proc ext {base} {
    mww [expr {$base + 0x30}] 0x000
    mww 0x4001C420 [expr {[r 0x4001C420] | 0x01}]   ;# GPIO8_0 -> UART1 TX
    mww 0x4001A420 [expr {[r 0x4001A420] | 0x08}]   ;# GPIO6_3 -> UART1 RX
    mww [expr {$base + 0x2c}] 0x070
    mww [expr {$base + 0x24}] 0x06c
    mww [expr {$base + 0x28}] 0x020
    mww [expr {$base + 0x30}] 0x301
    echo "EXT CR=[r [expr {$base + 0x30}]] FR=[r [expr {$base + 0x18}]]"
    mww [expr {$base + 0x00}] 0x41
    for {set i 0} {$i < 100} {incr i} {
        if { [expr {[r [expr {$base + 0x18}]] & 0x10}] == 0 } {
            set dr [r [expr {$base + 0x00}]]
            echo "EXT DR=$dr at attempt $i"
            if {$dr == 0x41} { echo "EXT PASS" } else { echo "EXT FAIL" }
            return
        }
        sleep 5
    }
    echo "EXT FAIL FR=[r [expr {$base + 0x18}]]"
}

if {$MODE == "lbe" || $MODE == "both"} { lbe UART1 0x40026000 }
if {$MODE == "ext" || $MODE == "both"} { ext 0x40026000 }

# leave the UART in a sane state for a subsequent `reset run`
mww 0x40026030 0x000
TCL_EOF

# ---------- run -------------------------------------------------------------
export AGRV_ADAPTER
END_ARGS=(-c "reset run" -c "shutdown")
if [ "$KEEP_HALTED" -eq 1 ]; then
	END_ARGS=(-c "shutdown")
fi

# `set MODE` runs in the same Tcl interpreter as the sourced script.
OUT="$(timeout 300 "$OPENOCD_CMD" -s "$PLATFORM_ETC" -f "$SUPPORT_CFG" \
	-c "init" -c "halt" -c "set MODE {$MODE}" -c "source $TCL" "${END_ARGS[@]}" 2>&1)"
RC=$?
printf '%s\n' "$OUT" | sed -n '/^CLOCKS /p;/^IDENT /p;/^ROUTE /p;/^LBE /p;/^EXT /p'

if [ $RC -ne 0 ] && ! printf '%s' "$OUT" | grep -q "shutdown command invoked"; then
	echo ""
	echo "WARN: openocd rc=$RC" >&2
	if printf '%s' "$OUT" | grep -q "error submitting USB"; then
		echo "The probe is wedged (usb read I/O error). Do NOT pkill openocd;" >&2
		echo "recover with pyusb dev.reset() + attach_kernel_driver(1)(2)," >&2
		echo "the probe did not examine; retry." >&2
	fi
fi

# ---------- verdict ---------------------------------------------------------
echo ""
if printf '%s' "$OUT" | grep -q "^ROUTE FAIL"; then
	echo "RESULT: fabric/jumper route FAILED — reflash the bitstream or reseat the PIN_66/67 jumper" >&2
	exit 2
fi
if printf '%s' "$OUT" | grep -q "^IDENT UART1 PL011=no"; then
	echo "RESULT: UART1 has no PL011 register file in this bitstream" >&2
	exit 1
fi
if printf '%s' "$OUT" | grep -q "^LBE UART1 FAIL" ||
	printf '%s' "$OUT" | grep -q "^EXT FAIL"; then
	echo "RESULT: UART1 loopback FAILED" >&2
	exit 1
fi
echo "RESULT: OK — UART1 $MODE loopback passed"
exit 0
