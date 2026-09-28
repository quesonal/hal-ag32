# Flash paths for `agrv2k_407`

> **Which document is this?** The long-form background to the two runners
> (why the default is `agrv_openocd`, what each one erases, the probe J-Link /
> CMSIS-DAP differences). The **rules you have to follow on the dev board** live in
> [`FLASH-AND-CAPTURE.md`](FLASH-AND-CAPTURE.md); the boot/DFU end of it (record,
> A/B, DFU, production lock) is indexed in
> [`BOOT-DFU-STATUS.md`](BOOT-DFU-STATUS.md).

The HAL ships **two** flash runners. Both write to the same on-die FLASH
(0x80000000+, 1 MB on agrv2k_407); they differ only in the transport
used to talk to the chip.

| Runner | Transport | Default? | Default bitstream write? | Probe required? | Linux reliability |
|---|---|---|---|---|---|
| `agrv_openocd` | SWD via CMSIS-DAP / J-Link | ✅ yes (2026-09-17) | ✅ written (Plan A, 2026-09) | on-board CMSIS-DAP works (warmup bundled); J-Link works directly | ✅ stable; warmup never detaches `cdc_acm` (2026-09-07 fix, see [Why `agrv_openocd` not `openocd`](#why-agrv_openocd-not-vanilla-openocd)) |
| `agrv32flash` | UART → ROM bootloader | ❌ fallback (no probe / no openocd) | ❌ skipped | none (uses the CDC-ACM serial on the USB cable) | ✅ stable |

> **Plan A (2026-09-17) change**: `west flash` (default runner
> `agrv_openocd`) now writes **both** images; the bitstream comes from
> `<build_dir>/zephyr/board.bin` (produced by `west build -t bitstream`)
> or from `AGM_BITSTREAM_BIN=<path>` in the environment. `--skip-bitstream` writes firmware
> only, `--bitstream-only` writes the bitstream only; the two are mutually
> exclusive. `--write-bitstream` still works and is now redundant.
>
> The board-wide default runner is `agrv_openocd` (2026-09-17), which is why
> the flags above need no `--runner`. The ROM-bootloader runner stays
> available as `west flash --runner agrv32flash`; it keeps its own
> `--skip-bitstream` default (its `--bitstream` path points at
> `<board_dir>/board.bin`, which this repository never populates), so use it
> when the board has no probe or the fabric must be left alone.
>
> Quartus synthesis still happens out of band of west (west never invokes
> Quartus), so the bitstream can also be compiled on a workstation and
> pointed at directly with `AGM_BITSTREAM_BIN=<path> west flash ...`
> (`west flash` takes no `-D`; the runner rewrites the bitstream write
> command, so an already-configured build dir needs no reconfigure). See
> [Phase 4.0+ workflow](#phase-40-workflow-bitstream-and-firmware-flashed-separately)
> below.

Default runner is set by `board.cmake` via `AGRV_FLASH_RUNNER`.
`../boards/agm/agrv2k/shared/board_common.cmake` picks it from what is actually installed under the
AgRV SDK: `agrv_openocd` when the AgRV-patched openocd is present (the
Plan A default -- it is the only runner with the three-state bitstream
handling), otherwise `agrv32flash` (the UART bring-up path for a board
with no probe); if neither tool is found it keeps `agrv32flash` and prints
a warning naming the missing pieces. Override per-invocation with
`west flash --runner <name>` or project-wide with
`-DAGRV_FLASH_RUNNER=<name>` at `west build` time.

### SDK location and per-tool overrides

One environment variable configures everything — `west build`, both
runners, and every script in `tools/`:

| Variable | Default | Meaning |
|---|---|---|
| `AGRV_SDK_PATH` | `~/AgRV_pio` | root of the PlatformIO AgRV install |
| `AGRV32FLASH` | `$AGRV_SDK_PATH/packages/tool-agrv_flashloader/bin/agrv32flash` | ROM-bootloader tool |
| `AGRV_OPENOCD` | `$AGRV_SDK_PATH/packages/tool-agrv_openocd/bin/openocd_cmd` | AgRV-patched openocd |
| `AGRV32FLASH` / `AGRV_OPENOCD` may also be given as `-D...` at `west build` time | | |
| `AGRV_ADAPTER` | `cmsis-dap` | probe interface; both the runner and `tools/*.sh` export this default, and `AGRV_ADAPTER=jlink` overrides it |

---

## Phase 4.0+ workflow: bitstream and firmware flashed separately

The HAL follows the AgRV `af --prepare` / `af --setup` / `af --batch`
discipline: **Quartus synthesis and Supra bitstream compile happen
out of band of west**. West never invokes Quartus or Supra batch
synthesis. West only writes the MCU firmware image to FLASH.

End-to-end flow:

```bash
# Step 1: west build - firmware + the Quartus-ready logic/ directory
#         (logic/ lives in the build tree: <build>/logic/)
cd ~/zephyrproject
west build -b agrv2k_407 -d /tmp/b_x modules/hal_ag32/samples/hello_world

# Step 2 (optional): add your own fabric RTL, declared in the app's CMakeLists
#         (`set(AGM_LOGIC_IP "<name>")` with ip/<name>.v, or AGM_USER_RTL=<files>)
#         - see [CUSTOM-IP.md](CUSTOM-IP.md); then re-run the logic directory:
west build -d /tmp/b_x -t logic

# Step 3: synthesis + place & route, on a Quartus workstation.
#   Take /tmp/b_x/logic as generated (do NOT re-run an SDK "prepare logic"
#   over it: board.v and board.vex are two halves of one generation run).
cd /tmp/b_x/logic
quartus_sh -t af_quartus.tcl     # runs af_ip.tcl, then the flow -> simulation/modelsim/board.vo

# Step 4: Supra compile - back on this machine, from the same build dir
#   (copy the workstation's simulation/modelsim/board.vo back into
#   /tmp/b_x/logic/simulation/modelsim/ if you synthesized elsewhere)
west build -d /tmp/b_x -t bitstream     # -> /tmp/b_x/zephyr/board.bin

# Step 5: write both images from here (Plan A default: firmware + bitstream)
west flash -d /tmp/b_x                       # --skip-bitstream / --bitstream-only to split
```

For the full Quartus-ready directory layout and user-RTL
conventions, see
[`BOARD-VE-FROM-DTS.md`](BOARD-VE-FROM-DTS.md)(`board.ve` → `board.{vx,hx,vex,qsf}` +
`af_quartus.tcl` 的生成),用户 fabric RTL 见 [`CUSTOM-IP.md`](CUSTOM-IP.md),
烧录/抓取的规则见 [`FLASH-AND-CAPTURE.md`](FLASH-AND-CAPTURE.md)。

Splitting the two images, and the ROM-bootloader variant:

```bash
west flash --runner agrv_openocd            # Plan A: firmware + bitstream

# ROM-bootloader path, same idea (bitstream must be at $AGM_BITSTREAM_BIN
# or <board_dir>/board.bin -- the latter is not populated by this repo):
west flash --runner agrv32flash --write-bitstream
```

The runner then writes the bitstream at `$AGM_BITSTREAM_BIN`
(or `--bitstream=<path>`) to FLASH @ 0x800e7000, then writes `zephyr.bin`
to FLASH @ 0x80000000, then resets.

---

## Path 1 — `agrv32flash` (recommended)

Uses the AgRV2K **ROM bootloader** that ships in the SoC mask. The
bootloader listens on the chip's primary UART (57600 baud, 8e1) for a
framed init sequence; once acknowledged it accepts raw memory write
commands at any FLASH-aligned address.

The on-board debug probe on `agrv2k_407` bridges this UART to a
**CDC-ACM** USB endpoint (`/dev/ttyACM0`), so the same USB cable used
for power also carries the flash stream.

### What gets written

**Default**: firmware only (the ROM-bootloader runner skips the
bitstream). The board's default runner, `west flash` (agrv_openocd),
writes both.

| Step | Image | Address | Default | Override | Default behaviour |
|---|---|---|---|---|---|
| 1 | FPGA bitstream `board.bin` | FLASH end − 100 KB | `0x800e7000` | `--bitstream-addr 0x...` | **skipped** (`--skip-bitstream`). Pass `--write-bitstream` to write it in the same session. |
| 2 | MCU firmware `zephyr.bin` | FLASH base | `0x80000000` | `--firmware-addr 0x...` | written |
| 3 | jump to entry | — | `0x80000000` | `--firmware-addr 0x...` | executed |

### Invocation

```bash
# Default: writes firmware only, then jumps
west flash --runner agrv32flash

# Write both bitstream + firmware in one step (Plan A, SWD path)
west flash --runner agrv_openocd

# Explicit --skip-bitstream (alias of the default)
west flash --runner agrv32flash --skip-bitstream

# Write both through the ROM bootloader
west flash --runner agrv32flash --write-bitstream

# Custom serial device (the board's probe exposes CDC-ACM there)
west flash --serial /dev/ttyACM1

# Custom baud (default 57600; some clones accept 115200 / 460800)
west flash --baud 115200
```

### Tool location

`agrv32flash` ships with the PlatformIO `tool-agrv_flashloader`
package. The runner auto-discovers it at:

```
$HOME/AgRV_pio/packages/tool-agrv_flashloader/bin/agrv32flash
```

Override via `--agrv32flash /path/to/binary` or `AGRV32FLASH=/path`
environment variable.

### Hardware setup checklist

The board **will not respond** to bootloader INIT frames if any of
these is wrong:

- [ ] **PWR LED is on** — board is actually powered.
- [ ] **BOOT0 high *and* BOOT1 low at power-up** — the vendor's hardware
      notes are explicit: "串口下载时,注意 BOOT0(高),BOOT1(低)".
      On the agrv2k_407 dev board BOOT1 is pulled down to GND
      (permanently low) and BOOT0 is a pull-down with a **jumper to
      3.3 V** — fit that jumper, then power cycle the board, or pulse a
      real nRESET with `../tools/probe_reset_target.py` and start the
      tool **immediately** afterwards (the loader's window is short).
      The strap is latched on the 4th SYSCLK rising edge, so a *soft*
      system reset does not re-latch it — openocd's `reset run` in
      particular (measured: seven attempts, no answer), while the
      CMSIS-DAP nRESET pulse does. Pulling BOOT0 high stops the user
      program from running, which is what keeps the ROM loader
      listening.
- [ ] **UART bridge is wired to the ROM UART** — not all AgRV2K
      boards expose the bootloader UART on the same pins as the
      application UART.
- [ ] **`/dev/ttyACM0` is from this board** — unplug any other
      USB-TTL converters; run `dmesg -w` and look for `cdc_acm` lines
      on plug-in.
- [ ] **User is in the `dialout` group**, or file is `chmod 666`
      (`sudo chmod 666 /dev/ttyACM0` is a quick fix).

To see the bootloader handshake live:

```bash
stty -F /dev/ttyACM0 57600 cs8 -parenb -cstopb -echo -hupcl
( stty -F /dev/ttyACM0 57600 cs8 -parenb -cstopb; \
  sleep 1; printf '\x55' > /dev/ttyACM0 ) &
timeout 3 cat /dev/ttyACM0 | xxd
```

You should see `0x55` echoes back from the bootloader once the cable
is connected to the right UART pins.

---

## Path 2 — `agrv_openocd` (fallback for users with a known-good probe)

Uses the SWD/JTAG pins on the on-board debug probe (or an external
J-Link). Writes through OpenOCD's `agrv` flash driver, which speaks
the AgRV2K flash controller directly over the debug bus.

This runner is a **subclass of upstream `OpenOcdBinaryRunner`** that
adds a one-shot pyusb probe warmup before invoking openocd. See
[Why `agrv_openocd` not vanilla `openocd`](#why-agrv_openocd-not-vanilla-openocd)
below for the rationale.

### Invocation

```bash
# Default: agrv_openocd (SWD), firmware + bitstream in one session
west flash -d /tmp/b_x

# No probe: ROM bootloader over UART, firmware only
west flash -d /tmp/b_x --runner agrv32flash

# On-board CMSIS-DAP probe (the default; AGRV_ADAPTER=jlink for J-Link)
AGRV_ADAPTER=cmsis-dap west flash -d /tmp/b_x

# Firmware only, keeping the existing bitstream (sector-erase preserves it)
west flash -d /tmp/b_x --skip-bitstream

# Bitstream only, firmware region untouched
west flash -d /tmp/b_x --bitstream-only

# Redundant under the new default (kept for muscle memory / old scripts)
west flash -d /tmp/b_x --write-bitstream

# Project-wide old-runner escape hatch (set at build time)
west build -DAGRV_FLASH_RUNNER=agrv32flash ...
```

### Bitstream-write flags

`agrv_openocd` accepts three flags (`--skip-bitstream` and
`--bitstream-only` are mutually exclusive at the argparse level;
`--write-bitstream` is redundant under the Plan A default and loses to
`--skip-bitstream` if both are passed):

| Flag | Effect |
|---|---|
| `--skip-bitstream` | Do not write the FPGA bitstream; existing bitstream in FLASH is preserved. |
| `--bitstream-only` | Write only the FPGA bitstream (no erase/load/verify on the firmware region). |
| `--write-bitstream` | Write the bitstream (Plan A default); redundant, kept for compatibility. |

Implementation: `agrv_openocd` overrides `__init__` to drop the
`post_verify` list (which carries the `flash write_image erase
<bitstream.bin> <BITSTREAM_ADDR>` command) **before** the parent
`OpenOcdBinaryRunner.__init__` stores it. The `load_cmd` (firmware
write) and `verify_cmd` (firmware verify) are unaffected. Verified
working: `wrote 24576 bytes from file zephyr.bin` with **no**
bitstream write invocation when `--skip-bitstream` is active
(2026-09-05).

Note: `agrv32flash` uses the opposite flag naming convention for its own
bitstream flag (`--no-skip-bitstream` to opt in) — that's its standard
west-flash binary-runner style.

### What gets written

OpenOCD runs a single session that writes both images. The
`<build_dir>/logic/openocd.cfg` is a thin wrapper around the AgRV SDK's
`(vendor SDK config)` (see [OpenOCD cfg architecture](#openocd-cfg-architecture)
below), so all adapter/target/flash-bank setup comes from the SDK.
Our `board.cmake` supplies the rest as runner arguments:

```
openocd
  -f <build_dir>/logic/openocd.cfg      ← sources AgRV SDK's agrv2k.cfg
  -c init -c targets
  -c check_device_id 0x40200001:1024KB  ← --cmd-pre-load: safety check (SDK proc)
  -c reset init
  -c flash write_image erase zephyr.bin 0x80000000   ← --cmd-load: firmware
  -c verify_image zephyr.bin 0x80000000              ← --cmd-verify
  -c flash write_image erase board.bin 0x800e7000    ← --cmd-post-verify: bitstream
  -c reset run -c shutdown
```

The bitstream write runs *after* the firmware verify on purpose:
`flash write_image erase` can bank-erase, so the fabric has to be the last
thing written. `--skip-bitstream` drops that one command, `--bitstream-only`
drops the firmware ones instead (`do_verify_only` keeps `--cmd-load` out of
the session).

`check_logic` is deliberately **not** called: it reads the bitstream at
`AGM_BITSTREAM_ADDR` to compute `LOGIC_ALGO_SIZE_`, and at pre-load time the
bitstream is exactly what is about to be rewritten.

`AGM_BITSTREAM_ADDR` defaults to `0x800e7000` (1 MB FLASH end
− 100 KB reserved, matches AgRV_pio default); override via
`-DAGM_BITSTREAM_ADDR=0x...` at build time.

### Why `BIN` instead of `ELF`

Our `zephyr.elf` is linked **XIP**: `CONFIG_XIP=y`, entry `0x80000000`, and
the code segment sits at `0x80000000` in FLASH (measured 2026-09-17 on
`/tmp/b_default_runner`: `LOAD vaddr=0x80000000 filesz=0x5b20 flags=R+W+X`,
plus small SRAM segments for `.data`/`.bss`). The AgRV2K boot path just
jumps to the FLASH entry — it does **not** copy the image to SRAM. (An
older revision of this section claimed the opposite: no `CONFIG_XIP`,
SRAM-only image, ROM copy-then-execute. That was wrong; the vendor's
two-cores / two-bin note says the same thing we now measure:
"code.bin 则仍然在 flash 区域运行".)

Upstream `OpenOcdBinaryRunner` hard-codes `load_image` for ELF files
(`openocd.py:390`) and ignores `--cmd-load`. Since ELF segments carry their
own addresses, an ELF flash would scatter the image to *both* FLASH and
SRAM, which is not what we want for a raw flash image (and `load_image`
cannot express "write this file at 0x80000000").

So we force **`--file-type bin`** + **`--flash-address=0x80000000`**.
The runner then honours `--cmd-load "flash write_image erase"`, which
erases the firmware area and writes the raw image at the FLASH base — the
same bytes the ROM will jump into on the next reset.

### OpenOCD binary resolution

`board.cmake` resolves the OpenOCD binary in this order:

1. `-DAGRV_OPENOCD=<path>` (cmake override)
2. `$AGRV_OPENOCD` (env var)
3. `~/AgRV_pio/packages/tool-agrv_openocd/bin/openocd_cmd`
4. `~/.platformio/packages/tool-agrv_openocd/bin/openocd_cmd`

The AgRV-patched binary is required because it adds the `agrv` flash
driver; the stock Zephyr SDK's openocd does not have it.

### Required OpenOCD setup for the on-board probe

The on-board CMSIS-DAP v2 probe is a **bulk-only** device (no HID
fallback). The AgRV fork's cmsis-dap driver supports bulk transport
but defaults to the HID path, which fails with this probe's
firmware.

`<build_dir>/logic/openocd.cfg` injects `cmsis-dap backend usb_bulk` **before**
sourcing the AgRV SDK cfg (which runs `init` at line 360 of
`(vendor SDK config)`). openocd rejects `cmsis-dap backend` after `init`, so
the switch has to happen early. We source `../boards/agm/agrv2k/shared/support/openocd.cfg.in`
ourselves first (to register the `cmsis-dap` Tcl command), do the
switch, then let the SDK cfg re-source the interface cfg
(idempotent) and proceed.

The CMSIS-DAP bulk-only path requires `agrv_fpga_decomp.inc` to be
present in the board directory for `check_logic` to compute
`LOGIC_ALGO_SIZE_`. `../tools/build_bitstream.sh` stages a copy from
`AgRV_pio/platforms/AgRV/etc/agrv_fpga_decomp.inc` automatically.

---

## OpenOCD cfg architecture

`../boards/agm/agrv2k/shared/support/openocd.cfg` is intentionally a **thin wrapper** around the
AgRV SDK's `(vendor SDK config)` (`(vendor SDK path)`,
361 lines). We do **not** duplicate adapter/target/flash setup. We
just override two board-specific variables, force the CMSIS-DAP
backend to bulk when applicable, and let the SDK cfg own the rest.

Variables consumed by the SDK cfg:

| Variable | Default | Our override | Why |
|---|---|---|---|
| `ADAPTER` | `jlink` | (env `AGRV_ADAPTER` may override) | J-Link is the standard MVP choice |
| `FLASH_SIZE` | `0` (probe-detect) | `0x100000` | 1 MB on-die FLASH; explicit avoids probe-roundtrip |
| `ADAPTER_OFFLINE` | unset | (intentionally unset) | Tcl's `[info exists]` returns true even for value 0; we must `unset` it (with `catch`) to avoid SDK cfg's offline early-return |
| `AGRV_ADAPTER` env | unset | (user picks) | Convenience shim so users don't have to edit the cfg |

Why source the SDK cfg at all? Because:

- `check_device_id` / `check_logic` (the bitstream-safety procs we
  call in `--cmd-pre-load`) are **defined in the SDK cfg**, not
  ours.
- Flash bank metadata (`flash bank ... agrv 0x80000000 0x100000 ...`)
  is **defined in the SDK cfg**.
- Adapter/transport/DAP setup (especially the `adapter_speed_hi`
  HSI/PLL dance on the SYS controller) is **defined in the SDK cfg**.

If we re-implemented any of these, we'd drift from the AgRV_pio
mainline. Sourcing the SDK cfg means **docs / examples / known-bugs
written against AgRV_pio's openocd flow translate 1:1 to `west flash
--runner agrv_openocd`**.

The SDK cfg path is resolved from `$AGRV_SDK_PATH` (defaults to
`$HOME/AgRV_pio`); set the env var if your PlatformIO install lives
elsewhere.

---

## Why `agrv_openocd` not vanilla `openocd`

The agrv2k_407 on-board CMSIS-DAP probe (VID `cafe`, PID `1001`,
FW 2.1.0) has a Linux-only USB quirk that vanilla `west flash
--runner openocd` does not handle.

**Symptom**

```
Info : Using speed 10000 KHz
Warn : could not read product string for device 0xcafe:0x1001: Operation timed out
Error: unable to find a matching CMSIS-DAP device
```

**Root cause** — the probe's USB device firmware does **not**
respond to `GET_DESCRIPTOR(STRING)` control transfers on EP0 within
OpenOCD/libusb's hard-coded timeout of **1000 ms** once it has
been used at least once. The OpenOCD cmsis-dap driver uses this
call during probe enumeration as a sanity check; when it times out,
the driver bails.

The first invocation after a fresh re-plug succeeds because the FW
responds quickly right after power-up; subsequent invocations get
progressively slower responses until they exceed 1 s. Windows
masks this (USBD.sys handles EP0 stalls differently); Linux's
usbdevfs does not auto-clear EP0 halt on partial responses.

**Workaround — `agrv_openocd` runner**

`agrv_openocd` is a thin subclass of upstream
`OpenOcdBinaryRunner` whose only added behaviour is to invoke
`tools/openocd_warmup.py` before each `west flash`. The warmup:

1. Does one slow `GET_DESCRIPTOR(STRING)` control transfer via
   pyusb with `timeout=5000` ms (5 s, well above any sane probe
   response latency)

**The warmup never detaches any kernel driver** (fixed 2026-09-07).
Earlier versions ran `detach_kernel_driver()` on interfaces 0/1/2 —
that removed the `cdc_acm` binding on intf 1+2 (the UART bridge) and
nothing re-attached it after openocd shutdown, so `/dev/ttyACM0`
disappeared after every flash. openocd only claims the CMSIS-DAP
interface (intf 0, no kernel driver bound); EP0 control transfers
work without claiming or detaching anything, so the serial bridge
stays bound. After the fix, repeated `west flash` + openocd sessions
leave `/dev/ttyACM0` alive with zero USB disconnect/reset events
(kernel-log verified 2026-09-07). See
[CDC-ACM-FLAKE-SOLUTIONS.md](CDC-ACM-FLAKE-SOLUTIONS.md) for the full write-up.

After the warmup, the probe's response latency drops back into
openocd's 1 s libusb window and openocd can read its own strings.
**No physical re-plug needed.** Verified with 2+ consecutive
`west flash --runner agrv_openocd` runs on the same plugged-in
probe.

Internally:

```python
class AgrvOpenOcdBinaryRunner(OpenOcdBinaryRunner):
    @classmethod
    def name(cls):
        return "agrv_openocd"

    def do_run(self, command, **kwargs):
        self._run_warmup()                              # ← new
        return super().do_run(command, **kwargs)
```

The warmup script is `tools/openocd_warmup.py`. It is auto-located
relative to the runner module: west imports the runner from
`<module>/scripts/west_commands/runners/agrv_openocd.py`, so the
runner walks three levels up to `<module>/tools/openocd_warmup.py`
(with the legacy `zephyr/`-prefixed layout as a second candidate).
No environment variable is needed. `ZEPHYR_HAL_AGM_HOME` still
overrides the search when set — useful when running the runner from
a copied/patched tree. If the script is missing the runner logs
`openocd_warmup.py not found` and skips the warmup. Since the warmup
no longer detaches `cdc_acm`, a skipped warmup does **not** lose
`/dev/ttyACM0` — repeated flash/openocd sessions stay stable either
way (observed 2026-09-07). Override:

```bash
export ZEPHYR_HAL_AGM_HOME=/path/to/zephyrproject/modules/hal_ag32
# or your out-of-tree hal_ag32 checkout
```

The "search next to the runner module" fallback was a known TODO
until 2026-09-10: the parents[] indices assumed a `zephyr/` prefix
(`<module>/zephyr/scripts/west_commands/runners/`) and so missed
`<module>/tools/` after the Option 2 layout moved the runner to
`<module>/scripts/west_commands/runners/`. `_find_warmup_script()`
now checks `here.parents[3]` (current layout, module root) first and
`here.parents[4]` (legacy `zephyr/`-prefixed layout) second, so
`west flash --runner agrv_openocd` runs the warmup with no env var.

Cleanup note: `zephyr/scripts/west_commands/runners/openocd_warmup.py`
in the Zephyr fork was **stale residue** left behind by
`c298f971b1e` (the in-tree-removal commit deleted the runners but
missed this file). It still contained the old `detach_kernel_driver()`
loop, which is the known way to lose `/dev/ttyACM0` for good. Nothing
referenced or executed it — west only loads the `tools/` copy via
`agrv_openocd.py`. **Deleted 2026-09-10**; verify with
`rg -n "runners/openocd_warmup" <zephyr>/` (must be empty).

To skip the warmup (e.g. for J-Link users), use the vanilla runner:

```bash
# Force upstream runner (no warmup; useful with J-Link)
west flash --runner openocd
```

### Subclassing `OpenOcdBinaryRunner` gotcha

Upstream `OpenOcdBinaryRunner.do_create()` hard-codes
`OpenOcdBinaryRunner(cfg, ...)` instead of `cls(cfg, ...)`. That
silently bypasses any subclass `__init__` / `do_run` overrides —
your subclass methods will never be called.

We override `do_create()` to mirror the upstream logic verbatim
but use `cls(...)`. The override is a known footprint of this
runner; if upstream Zephyr ever fixes the bug, this override can
be deleted.

---

## Known issues

### 1. `ttyACM0` disappears after flash(已修复 2026-09-07)

旧版 `../tools/openocd_warmup.py` detach 了 CDC-ACM intf 1+2 且无人 rebind,导致
`/dev/ttyACM0` 在每次 flash 后消失。根因修复: warmup 不再 detach 任何内核驱动
(openocd 只用 CMSIS-DAP intf 0,EP0 warmup 不需要 detach),`/dev/ttyACM0`
全程保持绑定。见 [CDC-ACM-FLAKE-SOLUTIONS.md](CDC-ACM-FLAKE-SOLUTIONS.md)。

异常兜底(仅当仍出现时):

```bash
# Re-bind cdc_acm
sudo usb_modeswitch -v 0xcafe -p 0x1001 --reset-usb
# or simpler:
echo 0 > /sys/bus/usb/devices/1-2.1/authorized
echo 1 > /sys/bus/usb/devices/1-2.1/authorized
```

`/dev/ttyACM0` 会在设备 rebind 后重新出现。

### 2. udev rules for unprivileged access

Without a udev rule, libusb can find the probe but cannot open
`/dev/bus/usb/BBB/DDD` for write. Install:

```udev
# /etc/udev/rules.d/99-agm-cmsis-dap.rules
SUBSYSTEM=="hidraw", ATTRS{idVendor}=="cafe", ATTRS{idProduct}=="1001", \
    MODE="0660", GROUP="plugdev", TAG+="uaccess"
SUBSYSTEM=="usb", ATTR{idVendor}=="cafe", ATTRS{idProduct}=="1001", \
    MODE="0660", GROUP="plugdev", TAG+="uaccess"
```

Then:

```bash
sudo cp 99-agm-cmsis-dap.rules /etc/udev/rules.d/
sudo udevadm control --reload-rules
sudo udevadm trigger
# Re-plug the probe
```

For the CDC-ACM serial (`/dev/ttyACM0`), either add yourself to
`dialout`:

```bash
sudo usermod -aG dialout $USER  # requires logout
```

…or temporarily `sudo chmod 666 /dev/ttyACM0`.

### 3. PyYAML hex literal parsing in `runners.yaml`

Zephyr's west serialises runner args through `runners.yaml`, which is
parsed by **PyYAML 1.1**. YAML 1.1 interprets an unquoted bare token
starting with `0x` as an integer literal (not a string):

```yaml
- --flash-address        # argparse expects str
- 0x80000000             # PyYAML parses as int 2147483648
```

Then argparse chokes on the int:

```
TypeError: 'int' object is not subscriptable
```

**Workaround** — keep the value attached to the flag in a single
token. `--flag=value` survives the round-trip because the leading
`--` puts the whole string in the "looks like a flag" bucket, so
PyYAML doesn't try to interpret the `0x...` suffix as a literal:

```cmake
# ✅ works — single token, no separate bare hex literal
board_runner_args(agrv_openocd "--flash-address=0x80000000")

# ❌ crashes — bare hex literal in the next list element
board_runner_args(agrv_openocd "--flash-address" "0x80000000")
```

The `agrv32flash` runner avoids the same trap by keeping
`--bitstream-addr` as a Python-side default and **not** passing it
through `board_runner_args()`; users who want to override set it on
the `west flash` CLI (which parses natively, not through YAML).

### 4. AgRV_pio's `oo` `-a` is an offset for ELF, not an absolute address

When invoking `oo` directly (without PIO's wrapper) to write an
**ELF** file, do **NOT** pass `-a 0x80000000`. `oo`'s
`--address-offset` is added to each segment's `p_paddr`, but AgRV
ELFs already have `p_paddr = 0x80000000` for text (because FLASH is
the load region per `AgRV2K_FLASH_SRAM.ld`). Adding another
`0x80000000` causes `0x80000000 + 0x80000000 = 0x100000000`
overflow, and openocd bails with:

```
Warn : no flash bank found for address 0x100000000
Warn : no flash bank found for address 0x10000167c
wrote 0 bytes
```

For ELF files, **omit `-a`** entirely (let openocd use each segment's
`p_paddr` directly). For raw **BIN** files, `-a 0x80000000` *is* the
correct absolute write address.

PIO happens to work because its build emits `.bin`, not `.elf` —
and for BIN, `-a` is the absolute address as expected. If you
re-use the PIO `oo_args` template verbatim with an ELF, you will
hit this overflow.

Verified working ELF invocation:

```bash
python3 "$OO" -V -d "$OPENOCD_DIR" -I agrv2k -A cmsis-dap \
    -s 10000 -g 1 -J 0x40200001 -w firmware.elf
#           ^^ NO -a  ^^
```

Output:

```
wrote 8192 bytes from file firmware.elf in 0.187s (42.811 KiB/s)
verified 6828 bytes in 0.038s (171.682 KiB/s)
```

### 5. `cmsis-dap backend` rejected after `init`

If you write your own openocd cfg and put `cmsis-dap backend
usb_bulk` after `init`, openocd errors with:

```
Error: The 'cmsis-dap backend' command must be used before 'init'.
```

The backend switch must happen **before** the first `init`. The
AgRV SDK cfg (`(vendor SDK config)`) calls `init` at line 360, so any
backend switch must run **before** `source`ing the SDK cfg.

Our `<build_dir>/logic/openocd.cfg` sources `../boards/agm/agrv2k/shared/support/openocd.cfg.in`
itself first (to register the `cmsis-dap` Tcl command), does the
backend switch, then sources the SDK cfg.

### 6. `check_logic` needs `agrv_fpga_decomp.inc` staged in BOARD_DIR

The SDK cfg's `check_logic` proc (`(vendor SDK config)` line 270) opens
`agrv_fpga_decomp.inc` from `script_path` to compute
`LOGIC_ALGO_SIZE_`. Without it, you get:

```
Error: .../agrv_fpga_decomp.inc: No such file or directory
in procedure 'check_logic'
```

`../tools/build_bitstream.sh` stages a copy of this file from
`AgRV_pio/platforms/AgRV/etc/agrv_fpga_decomp.inc` into the logic directory
(`<build>/logic/`) as part of the bitstream build, so it's ready when
`west flash --runner agrv_openocd` runs `check_logic`.

If you skip the bitstream build (e.g. only firmware iteration) you
can stage the file manually:

```bash
cp $AGRV_SDK_PATH/platforms/AgRV/etc/agrv_fpga_decomp.inc \
   <build>/logic/
```


---

## Appendix: how the default came to be (Plan A, 2026-09-17)

> 实施过程的完整台账(动机 / 三改动 / 阶段 1–7 / 不在 scope / 风险)已在 `git log 8238aad`("Plan A" 提交的 commit message + 系列后续 fix);不再以单独的文档形式保留 ——
> 留在仓库里只会让用户看到一段已经过时的对账过程。当前规则的 *为什么*,
> 在 §0 *Phase 4.0+ workflow* 的动机段已经说过,要看 *怎么落到代码里* 直接看 git log。

## 11. F5 — 默认 runner 改为 `agrv_openocd`(2026-09-17,用户要求"减少参数输入")

**改动**:`../boards/agm/agrv2k/shared/board_common.cmake` 的 runner 选择顺序反过来 —— 有 AgRV-patched openocd
就用它(它是唯一有三态模型的 runner),没有才回落 `agrv32flash`;两者都没有时保持
`agrv32flash` 并警告缺什么。因此 `west flash` / `west flash --skip-bitstream` /
`west flash --bitstream-only` 都不再需要 `--runner`。

**保留的覆盖**:`west flash --runner agrv32flash`(没探针时)、
`-DAGRV_FLASH_RUNNER=<runner>`、`-DBOARD_FLASH_RUNNER=<runner>` 都仍然优先。
`BOARD_FLASH_RUNNER` 仍是普通 cache 变量(不用 FORCE),所以**用户显式设过的值不会
被覆盖**;代价是**这次改动不会自动作用到旧 build dir** —— configure 时会打一行
STATUS 说明缓存值与新默认不同、以及怎么切(`-DBOARD_FLASH_RUNNER=agrv_openocd`
或 pristine 重建),避免"改了默认却还是老 runner"的静默困惑。

**取舍(写下来,别只留在聊天里)**:默认走 SWD 意味着**必须有探针**;
`agrv32flash` 那条 UART ROM bootloader 通路(无探针 / fabric 未配置时的 bring-up)
降级为显式选项。另外探针会话在本开发板有已知的间歇性 examine 失败(见
[FLASH-AND-CAPTURE.md](FLASH-AND-CAPTURE.md) §10),`agrv32flash` 没有这个问题 —— 遇到连不上探针时
用 `--runner agrv32flash` 当作旁路。

**板级证据**(2026-09-17,agrv2k_407,canonical 比特流):

- 新 build dir(`west build -d /tmp/b_default_runner -b agrv2k_407 --pristine=always …`)
  → `zephyr/runners.yaml` 里 `flash-runner: agrv_openocd`
- `west flash -d /tmp/b_default_runner`(无 `--runner`、无 flag)
  → `-- west flash: using runner agrv_openocd`,同一会话写两个镜像
  (`wrote 24576 bytes … zephyr.bin` + `wrote 102400 bytes … board.bin`)
- `AGM_BITSTREAM_BIN=$HOME/spi_full_bitstream/example_board.bin west flash -d /tmp/b_default_runner --bitstream-only`
  → 不带 `--runner` 就被接受,日志
  `AGM: bitstream write retargeted: …/board.bin -> …/spi_full_bitstream/example_board.bin`
- `west flash -d /tmp/b_default_runner --bitstream-only` → 只写比特流
- 回读 `0x800e7000` 99944 B == canonical(md5 `6378549f…`);
  `test_uart_capture.sh -t 8` → `printable_ratio = 100%`,`RESULT: OK`
- 旧 build dir(`/tmp/b_plan_a4`,缓存里还是 `agrv32flash`)reconfigure 时打印
  `hal_ag32: BOARD_FLASH_RUNNER is cached as 'agrv32flash' but the board default is now 'agrv_openocd' …`
