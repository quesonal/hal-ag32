# hal_ag32 tools

Helper scripts that bridge Zephyr's build to the Supra FPGA toolchain
(`af_cmd`, `gen_vlog`, `pre_logic.tcl`, `gen_logic.tcl`) shipped with
the `AgRV_pio` PlatformIO platform package.

The tools are intentionally minimal — they do not introduce a new build
system on top of SCons/CMake; they just translate Zephyr's "I want a
bitstream" custom command into a sequence of Supra invocations.

Everything is standalone (no shared shell library), but every script
follows the same environment convention: nothing is hard-coded to a
particular machine, and each variable can be overridden.

| Variable | Default | Meaning |
| --- | --- | --- |
| (锁定档) | — | **生产锁定的 build 只有一条带内升级路径**:`smp_cli.py <port> upload <image> <file> --authorize <key.pem>`(它替宿主花掉两次授权:开会话 + publish)。console 的 `upload` 相与 `agrv32flash` 在锁定档**完不成**上传(它们拿不到流中间那条授权),`agm_upload.py` 会在读到 loader 的拒绝行时直接给出这条提示;没有 SMP 服务端的 build 只能靠 SWD / BOOT0 + ROM bootloader 恢复。见 [`docs/SIGNED-IMAGES-PLAN.md`](../docs/SIGNED-IMAGES-PLAN.md) §11 |
| `AGRV_SDK_PATH` | `$HOME/AgRV_pio` | root of the AgRV PlatformIO platform install |
| `AGRV_OPENOCD` | `$AGRV_SDK_PATH/packages/tool-agrv_openocd/bin/openocd_cmd` | SDK OpenOCD launcher |
| `AGRV_PLATFORM_ETC` | `$AGRV_SDK_PATH/platforms/AgRV/etc` | `gen_vlog`, `pre_logic.tcl`, `gen_logic.tcl` |
| `SUPRA_HOME` | `$AGRV_SDK_PATH/packages/tool-agrv_logic` | dir with `bin/af_cmd` (Supra) |
| `AGRV_ADAPTER` | `cmsis-dap` | probe type handed to OpenOCD |
| `ZEPHYR_HAL_AGM_HOME` | the tree the script lives in | module root (board `support/*.cfg`, warmup, decoders) |
| `AGM_WORKSPACE` | — | west workspace root, for the optional `devsync.sh` helper |
| `AGM_DEV_TREE` | the tree `devsync.sh` lives in | tree to mirror into the workspace, for `devsync.sh` |
| `AGM_OO` | `$AGRV_SDK_PATH/platforms/AgRV/etc/oo` | the SDK's OpenOCD runner (used by `agm_oo.sh`) |
| `AGM_OPENOCD_BIN` | `$AGRV_SDK_PATH/packages/tool-agrv_openocd/bin` | dir holding `openocd_cmd` |
| `AGM_SWD_SPEED` | `10000` | SWD clock in kHz for every OpenOCD path (the vendor uses 10 MHz) |
| `AGM_OPENOCD_ATTEMPTS` | `3` (`4` for `flash_logic.sh`) | retry budget for the whole OpenOCD session when the examine/write flakes |

The Python-invoking scripts additionally require an **active Zephyr
virtualenv** (`VIRTUAL_ENV`): the system `python3` lacks `pyusb` and
`devicetree`, and the failure mode is silent.

## Scripts

| Script | Purpose |
| --- | --- |
| `build_bitstream.sh <board_dir> <logic_dir>` | dts (or an existing `board.ve`) → Quartus-ready logic dir via Supra; behind `west build -t logic`. `AGM_DTS=auto` renders the dts here with cpp+dtc, so no Zephyr build is needed first |
| `compile_bitstream.sh <logic_dir> [out.bin]` | **last step**: after Quartus has routed the design (`simulation/modelsim/<design>.vo`), run Supra's batch compile to place+route on the architecture and write `<design>.bin`; copies it to `$AGM_BITSTREAM_BIN` when asked |
| `generate_board_ve.py` | render `<board>/board.ve` from the `agm,agrv2k-pins` node of a dts (the preprocessed `zephyr.dts`, or the one `AGM_DTS=auto` builds from `<board>/*.dts`); called by the above |
| `flash_fw.sh <firmware>` | firmware → FLASH @0x80000000, leaving the bitstream region alone |
| `flash_logic.sh [bitstream.bin]` | bitstream → FLASH @0x800e7000 (sector-erase of that region only) |
| `generate_pinctrl_dtsi.py --dts <zephyr.dts> --out <dtsi> [--check]` | render the pinctrl states' `agm,pins` cells from a board/sample overlay's pin list + the SoC AF table (`tools/agm_af_pins.yaml`); `--check` is the freshness gate. Refuses an `--out` under a west workspace's `modules/hal_ag32` (exit 7) — that tree is a `devsync.sh` mirror, generate into the tree the script lives in |
| `check_pinctrl.py --dts <zephyr.dts> [--netlist <board.vx>] [--ve <board.ve>]` | assert the pin list matches the bitstream's netlist and that each state's effective `agm,pins` is exactly what the AF table implies (missing/extra/mislabelled cells fail). `--netlist` must be the netlist of the bitstream this pin list describes (the pre-route wrapper: `logic/board.vx`, or the vendor pair in `tools/tests/fixtures/`); `--ve` defaults to the netlist's sibling `<stem>.ve` and is what lets a function routed through the CPLD cross-bar (a Case C row, e.g. `SPI0_SI_IO0 si_io0`) be resolved instead of reported as "not routed" |
| `check_af_table.py [--af <agm_af_pins.yaml>] [--sdk-header <AltaRiscv.h>]` | verify the AF table against the vendor header (all 114 `*_AF_GPIO` triples: missing, extra and mismatched rows fail) and against the dts (`state:` must exist, `dir:` must fit the header's direction marker). Skips the header part when the SDK is absent |
| `check_pin_routing.sh [--dts <zephyr.dts>]... [--netlist <board.vx>]` | the pin-routing gate in one command: AF table + `tools/tests/` (which re-derive the committed fragments from their pin lists) + (given a merged dts) fragment freshness + (given a netlist) the fabric comparison. `.gitea/workflows/pin-routing.yml` runs it without arguments on every push. |
| `tests/` | `python3 -m unittest discover -s tools/tests` — regression tests for the three tools above (no board, no SDK, no build tree needed), driven partly by the vendor fixture pair in `tests/fixtures/` |
| `make_agm100_overlay.sh [<out>]` | write the (deliberately uncommitted) `/tmp/agm100.overlay` that puts the DT clocks at the 100 MHz dev board bitstream's rate — it is a temp file, so recreate it after a reboot; confirm the board's actual rate first ([FLASH-AND-CAPTURE.md](../docs/FLASH-AND-CAPTURE.md) §0.3) |
| `devsync.sh [status\|push\|restore] [--apply]` | mirror this working tree into `<ws>/modules/hal_ag32` so the build reads uncommitted edits, and put the workspace back on its HEAD afterwards (see below) |
| `agm_oo.sh info\|options-erase\|lock\|read-protect\|unlock\|fw\|bitstream\|read\|erase-all` | the SDK's own OpenOCD runner (`oo`): option-byte readback/erase, **read protection on (`lock`, `read-protect`) and off (`unlock`)**, firmware/bitstream write, flash read. The only path that reaches the flash controller's option registers. `lock` does *not* erase the images (the console keeps running) but `unlock` erases the whole chip -- read [FLASH-AND-CAPTURE.md](../docs/FLASH-AND-CAPTURE.md) §11 before using either; the no-probe side is `agrv32flash -j/-k/-L` (BOOT0) |
| `agm_rdp_tear_test.sh [--rom-erase-tear] [--rom-nreset [S]] [--window N] [--kill-after S] [--kill-on-erase] [--iwdg-reset\|--iwdg-after N] [--inspect-only] [--recover …]` | rehearse a **tear in the option write** (the one window R2c could not answer for), and recover from it. **`--rom-erase-tear` is the mode to reach for**: `agrv32flash -O` erases the option area, which reads back exactly as the torn state (option area erased, read protection reads *on*, `osc 0xff,0xff`, no FPGA pointer, board stops booting) with no reset timing involved; the script verifies the area was canonical first and judges the **transition**, not the end state. `--rom-nreset [S]` (aim the probe's nRESET into `agrv32flash -j`'s option write; needs BOOT0) is kept but is **not** a reproduction: 15 attempts from a verified canonical area, 0 tears -- the ROM's option erase+program is ~3 ms wide while the tool's handshake in front of it wanders over 0.02..1.0 s, and a chip-side reset that does land inside it does not stop the write. The other modes interrupt an openocd session instead (`--window N` keeps ONE session of back-to-back `agrv lock 0` writes; `--kill-after S` SIGKILLs it S seconds in; `--kill-on-erase` kills it the moment the driver reports the option erase; `--iwdg-reset`/`--iwdg-after N` arm the on-chip 64 ms watchdog) -- all of those land in host time, which, across 8 delays, *cannot* tear the option area. `--inspect-only` reports the state; `--recover --build-dir … --bitstream … --salt-file … --uid …` runs the recovery ladder as one command (ROM or SWD unlock -> FPGA pointer -> firmware+bitstream -> salt -> verify), which works from any option state because that area is only ever touched over the AP path. What is torn, what a reboot does and does not fix, and the transcripts: [FLASH-AND-CAPTURE.md](../docs/FLASH-AND-CAPTURE.md) §11.5--§11.9 |
| `test_hil.sh [<scenario>...]` | the **on-board** twister run: six `*.hil` device-testing cases (hello_world canary, dma_memcpy, spi_flash_rw, rtc_alarm, wdt_feed, iwdg_basic) built, flashed, run and asserted on the console by twister itself, one at a time (`-j 1` dodges the documented parallel `configure_file` race). Use it whenever a **driver or the SoC bring-up** (clocks, gates, resets, DMA) changes: the commit gate `west twister ... --build-only` never runs anything, which is how the AHB-gate regression stayed green in every configuration while DMAC0 was gated on the dev board. Needs a free probe + console (`AGM_HIL_PORT`, default `/dev/ttyACM0`) and a bitstream (`AGM_BITSTREAM_BIN`, default the canonical 200 MHz one); `spi_flash_rw` erases NOR sector 0. 6/6 in ~107 s on the dev board. See the sample workflow, §2.1 |
| `clean_twister.sh [--apply] [--keep N] [root...]` | reclaim what test runs leave behind: `/tmp/tw_*` (twister `-O` trees, including the `.1/.2` copies twister makes because its default "clean" *renames* the old output) and `/tmp/b_*` (west build dirs). Dry run unless `--apply`; `--keep N` keeps the N newest of each. One session of leftovers filled a 97 GB volume to 100 %, after which builds failed with "No space left on device" -- which surfaces as a bogus twister CMake failure or a silently stale firmware. `tools/test_hil.sh` passes `--clobber-output`, so its own `-O` dir can be reused |
| `rom_opt.py show\|save\|restore\|set-fpga <addr>` | the **ROM bootloader** path to the same option area (BOOT0 high, UART0, no probe): `show` decodes it, `save`/`restore` round-trip an image, `set-fpga` points the FPGA config address at an address (with `--compressed --algo …` for the compressed layout). It does what the vendor's custom-programmer reference does -- erase the options (0xA3) and write the image (0x31) in *one* session -- because `agrv32flash` cannot: its `-O` resets the device, after which the ROM reads the erased options as protected and NACKs everything. **Writing the option area triggers a full chip erase**: re-flash code and logic afterwards. See [FLASH-AND-CAPTURE.md](../docs/FLASH-AND-CAPTURE.md) §10.0.1 |
| `probe_state.sh` | one-shot readback: USB/tty, device + option bytes (WRPR, read protection, FPGA address), the SWD setup in use, and FCB STAT / APB_CLKENABLE / RST_CNTL through the debugger |
| `probe_recover.sh [--options-erase]` | the ladder for "the probe looks dead": clear a stale usbfs claim + warmup, optionally erase option bytes, then print the ROM-bootloader rung (needs BOOT0 high + BOOT1 low latched by **power-up or a real nRESET**: dev board = BOOT0 jumper to 3.3V, BOOT1 already at GND) |
| `probe_reset_target.py` | pulse the target's nRESET through the CMSIS-DAP probe (`DAP_ResetTarget`) without openocd, so the BOOT0/BOOT1 strap is re-sampled. This is what brings the ROM UART loader up on a power-fed dev board: openocd's `reset run` is a soft system reset and does **not** re-latch the strap. Start `agrv32flash` right after the pulse (the loader's window is short). |
| `test_uart_capture.sh [-t N] [firmware]` | reset (optionally flash first) while capturing `/dev/ttyACM0`, then analyse the capture |
| `boot_timing.py [--runs N] [--window S]` | timestamp the console around a `reset run` (through a **running** openocd's telnet port, so the trigger is one command rather than a 1 s process spawn) and print every line's offset plus the `fabric / window / verify / jump / app` marks of `samples/spi_boot_loader`. This is the loop behind the signed-boot numbers in [BOOT-DFU-STATUS.md](../docs/BOOT-DFU-STATUS.md) §5.5: the signed-fabric verify runs in `PRE_KERNEL_1`, before the kernel timer exists, so no device-side clock can see it. Start openocd yourself, keep it running, then call the tool (usage in its header) |
| `test_uart1_loopback.sh` | register-level UART1 loopback check, CPU halted through OpenOCD |
| `test_fcb_hotswap.sh --slot-b-bs <bin>` | end-to-end run of `samples/fcb_hotswap`: write slot B, `reset run`, send `x` twice, assert FCB reload rc=0 in both directions and that the SPI NOR RDID moved with the fabric. Default does the round trip (board ends on slot A); `--no-swap-back` keeps it on the target slot |
| `openocd_reset_run.sh [board]` | minimal `init / reset run / shutdown` |
| `loader_session.sh [--port P] [--window S] -- <cmd...>` | reset the board, land in the `samples/spi_boot_loader` console (send CRLF inside its 1.5 s boot-abort window) and then hand the serial port to `<cmd>`; a no-op variant with no command just leaves the console at its prompt. This is the step in front of both standard upload paths (`agrv32flash` / `smpmgr`). |
| `smp_cli.py <port> {state\|upload\|erase\|nonce\|authorize} …` | a second, dependency-light SMP (mcumgr) host: it implements the 0x0609/0x0414 base64 framing and the CBOR body itself (pyserial + cbor2 only), so a framing bug shows up against two independent hosts — `smpmgr` and this one. `upload`/`erase` take the bootloader's image number (0/1 stores, 2 on-die slot, 3 bitstream staging). `nonce` + `authorize <key.pem> <erase\|publish>` are the R2b signed commands a production-locked build wants; `upload … --authorize <key.pem>` spends both grants for you (ERASE before the first chunk, PUBLISH before the one that publishes) |
| `verify_flow.py [--only N,…] [--keep] [--key K] [--salt S] [--workdir D]` | the **end-to-end verification flow**: builds `samples/verify_flow` (the application half) + a production-profile loader (with `CONFIG_BOOT_AGM_BIND`), provisions the board's per-chip salt over SWD, signs + binds the payload containers with a key/salt it keeps in the workdir, then runs twelve steps on the board and asserts each one — salt provisioning + the read-back fingerprint, signed application, signed bitstream slot, anti-rollback floor, R2b command grants, and the six refusal paths (unbound, another chip's copy, tampered container, older version, unauthorized publish, forged bitstream slot). Restores `hello_world` at the end unless `--keep`. |
| `agm_upload.py <port> <a\|b\|bitstream\|slot> <image.bin> [--load 0x… --entry 0x…]` | drive `samples/spi_boot_loader`'s own `upload <target>` binary phase: `a`/`b` are the two A/B sides, `bitstream` the fabric staging area, and `slot` the on-die flash DFU (programs the application slot directly). `--load`/`--entry` default to that slot -- `0x8007c000` in the default (on-die) layout, `0x80030000` with `boards/agrv2k_407_ext_nor.overlay` -- and a signed image has to carry the same address in its header, so zeroes work too (the container decides). Rejections come back as NAK **code 4** ("image rejected", not code 3 "flash write/verify failed") or **code 5** ("older than the one installed", the anti-rollback floor); the loader's own explanation is echoed with `--verbose`. |
| `sign_image.py <image.bin> <key.pem> -o <out.signed.bin> [--pubkey-out <pub>] [--slot-base 0x…]` | package a `zephyr.bin` into the MCUboot container the signed loader accepts, and emit the key blob to build it with (`-DSPI_BOOT_PUBKEY=`). The **key file picks the profile**: ECDSA P-256 → 64 B raw X\|\|Y pubkey + SPKI-based KEYHASH, RSA-2048 → 270 B PKCS#1 DER + PKCS#1-based KEYHASH (`-DCONFIG_BOOT_AGM_SIG_{ECDSA_P256,RSA2048_PSS}=y`). `--slot-base` must be the slot the image will run in, because it becomes the header's `ih_load_addr` and the loader refuses a container that disagrees. Self-checks its own output (imgtool verify + KEYHASH) before the dev board sees it; `tests/test_sign_image.py` pins both profiles |
| `sign_image.py --bitstream <example_board.bin> <key.pem> -o <bs.signed.bin> [--pubkey-out <pub>]` | the **fabric** flavour (`CONFIG_BOOT_AGM_BITSTREAM_SIGNED`): the whole 99944 B bitstream in an MCUboot container stamped for the bitstream slots. Fixes `--slot-base 0x800b4000` (`ih_load_addr 0x800b4020`: the two slots are interchangeable, so there is one stamp) and `--slot-size 0x19000`, refuses an input that is not exactly 99944 B, and passes imgtool `--overwrite-only` to drop its 3 KB swap-trailer reservation (this loader never swaps). Upload the result with `agm_upload.py <port> bitstream bs.signed.bin`; build the loader with `-DSPI_BOOT_BITSTREAM_PUBKEY=` (or, in a P-256 build, with the same key in `-DSPI_BOOT_PUBKEY=` -- the fabric key falls back to it). |
| `compress_bitstream.py <config.bin> -o <out.bin> [--check]` | LZW-compress a Supra bitstream for `CONFIG_AGM_FCB_BITSTREAM_COMPRESSED` (the SDK's `logic_compress = true` form, which Supra only emits inside a Quartus flow) `--check` decodes the result with a built-in copy of the decoder and compares byte for byte. Format and the three things that have to line up on the board are in the tool's header. |
| `agm_logic_crypt.py selftest \| keys --uid <32 hex digits> \| crypt <in> -o <out> [--uid/--uid-file] [--offset 8] [--length N]` | the vendor's per-chip **bitstream** cipher (`board_logic.encrypt`), read out of the unstripped `agrv32flash` and validated against its own output. `keys` derives the flash key from the **on-die flash's 128-bit unique ID** — the 16 bytes `FLASH_GetUniqueID` reads with `0x4B` after `FLASH_Unlock` (not the external SPI NOR's 8-byte ID) — via `[u0^u1, u1^u2, u2^u3, u3^(u0^u1)]`; `crypt`/`decrypt` transform an image from `--offset` (default 8: the vendor leaves IDCODE/USERID raw). It is a **compatibility** tool, not a security boundary (two-round Salsa20 over three XOR combinations of an ID any probe can read). Read only if you need the whole picture. |
| `agm_bind.py {gen-salt\|key\|tag\|embed\|check\|salt-sector\|uid-from-words} …` | the **per-chip binding** host half (`CONFIG_BOOT_AGM_BIND`): `key = HKDF-SHA256(ikm = the chip's UID, salt = the provisioned salt, info = "agm-bind-v1")`, and the tag a container has to carry is `HMAC-SHA256(key, ih_ver ‖ SHA-256(header ‖ image))`. `embed` appends that tag as a `BIND` TLV (`0x00a0`, MCUboot's vendor range) to a container `sign_image.py` just produced; `check` recomputes and compares. The tag covers the container's *own* version field, so re-signing for a new release invalidates it without anyone having to remember a flag. Design and threat model: [SIGNED-IMAGES-PLAN.md](../docs/SIGNED-IMAGES-PLAN.md) §12 |
| `agm_bind.py provision --salt-file S [--uid H \| --uid-log F \| --port P]` | write the 4 KiB **salt sector** (`0x800b0000` in the default on-die layout, the last sector of the A/B chain) into a board over SWD: reads the sector first and refuses to overwrite anything that is not erased (and not the same salt) unless `--force`, then reads it back and verifies. `--read-only` just reports what is there plus the key fingerprint; `--uid-log` takes the UID out of a captured loader `info`, `--port` resets the board and reads it live. Orders that matter: firmware + bitstream → salt → bound images → RDP. A provisioned salt survives both `west flash --skip-bitstream` and a full firmware + bitstream flash |
| `check_bitstream_clock.py <bin> --config <build>/zephyr/.config [--dts <build>/zephyr/zephyr.dts]` | does the bitstream about to be written run the fabric at the clock this firmware was built for? It reads the bitstream's own `.ve` (`SYSCLK 200` / `BUSCLK 100` / `HSECLK 8`, the file `Supra gen_logic` consumes) or, when there is no `.ve`, computes SYSCLK from the generated Verilog's PLL parameters (`CLKIN_FREQ x (CLKFB_HIGH+1) / (CLKOUT0_HIGH+CLKOUT0_LOW+2)`, cross-checked on the three dev board bitstreams), and compares it with `CONFIG_SYS_CLOCK_HW_CYCLES_PER_SEC` plus the devicetree's `hseclk-frequency`/`flash-max-frequency`. Exit 1 = mismatch, 0 = agree (or nothing to compare — pass `--require-ve` to make that fatal). `west flash` runs it before writing a bitstream and refuses on a mismatch; `AGM_SKIP_CLOCK_CHECK=1` skips it once. It exists because a bitstream whose clock disagrees with the firmware leaves the board unbootable until it is reflashed. |
| `openocd_monitor.sh [board]` | foreground OpenOCD with parallel semihosting + UART capture |
| `openocd_warmup.py` | pre-flash CMSIS-DAP warmup (CDC-ACM flake workaround) |
| `usb_forensic_dump.sh` + `usb_forensic_decode.py` | dump and decode USB0 controller / dQH / dTD state |
| `usb_cdc_echo/` | helper app + script for the CDC-ACM echo dev board |

Background: [FLASH-AND-CAPTURE.md](../docs/FLASH-AND-CAPTURE.md) (rules + symptom → fix table for
flashing and UART capture), [FLASH.md](../docs/FLASH.md) (probe + flash flow) and
[CDC-ACM-FLAKE-SOLUTIONS.md](../docs/CDC-ACM-FLAKE-SOLUTIONS.md) (why the warmup exists).

## Probe state, recovery, and the SDK's own runner

Three tools were added, each one earned by a lost hour:

* **`agm_oo.sh`** wraps `<AgRV_pio>/platforms/AgRV/etc/oo` — the runner
  behind PlatformIO's upload / lock / unlock / wipe / opterase targets. Use it
  instead of hand-rolling `openocd -f agrv2k.cfg`: `oo` is the only path that
  reaches the AgRV2K flash controller's option-byte and write-protection
  registers (0x8100_0020+), which are **not** on the system bus — raw
  `flash protect` and system-bus reads fail on them, which is what makes
  openocd claim "device protected" on an unlocked chip. `agm_oo.sh info`
  reads the state back, `options-erase` clears it (and takes the FPGA
  configuration pointer with it, so write the bitstream again).
* **`probe_state.sh`** prints everything a debug session can tell you in one
  shot: the probe and console nodes, the option-byte readback, the SWD setup
  the board cfg is about to use, and FCB STAT / APB_CLKENABLE / RST_CNTL from
  the SoC. It is read-only; start any "the board is dead" investigation here.
* **`probe_recover.sh`** is the recovery ladder: clear a stale host-side
  usbfs claim (`usb.util.dev.reset()`) and warm the probe, optionally erase
  the option bytes, and print the `agrv32flash` rung for the case where the
  CPU is not running at all. That rung needs the chip in serial-download
  mode -- BOOT0 high **and** BOOT1 low **at power-up** (dev board: BOOT1
  is tied to GND, BOOT0 is a pull-down with a jumper to 3.3V).

The SWD settings those tools rely on (10 MHz, CMSIS-DAP **v2 bulk**, `reset
init` before flashing) and the symptoms they fix — `stalled AP operation`,
`dmstatus=0x0`, a bogus "protected" flash, a wedged CDC-ACM bridge — are
documented in [FLASH-AND-CAPTURE.md](../docs/FLASH-AND-CAPTURE.md) §10
and [§10.1](../docs/FLASH-AND-CAPTURE.md#101).

### What needs the SDK, and what does not

The tools were deliberately kept usable without the AgRV SDK wherever that is
possible. The split is:

| Tool | Needs |
| --- | --- |
| `devsync.sh`, `probe_recover.sh` (step 1), `openocd_warmup.py` | the repo venv (pyusb) only — no SDK |
| `probe_state.sh`, `openocd_reset_run.sh`, `test_uart_capture.sh` (capture-only), `openocd_monitor.sh`, `test_uart1_loopback.sh`, `usb_forensic_*` | an OpenOCD **binary** with RISC-V support; the SDK's *files* are optional — see the fallback below |
| `flash_fw.sh`, `flash_logic.sh` | the SDK's `agrv` flash driver (it lives in the vendor OpenOCD build) |
| `agm_oo.sh` | the SDK's `oo` runner — it is the only path to the flash controller's option-byte registers |
| `build_bitstream.sh`, `compile_bitstream.sh` | the Supra / Quartus toolchain (`tool-agrv_logic`) |

**The fallback.** The board's `support/openocd.cfg` gets its adapter, target and
flash driver by sourcing the SDK's `platforms/AgRV/etc/agrv2k.cfg`. When that
file is not installed, the debug-only tools (`probe_state.sh`,
`openocd_reset_run.sh`, and `test_uart_capture.sh` without a firmware
argument) fall back to this module's own
`boards/agm/agrv2k/shared/support/agrv2k-minimal.cfg`: the same CMSIS-DAP v2 +
SWD + RISC-V target, without the SDK helpers or the flash driver. Verified
equivalent to the SDK path (FCB STAT / APB_CLKENABLE / RST_CNTL read back
identical, and the same examine reliability
— see below), and it needs no `-s <sdk>/platforms/AgRV/etc` on the command
line. `AGM_OPENOCD_CFG=<path>` overrides either choice. Flashing still needs
the vendor driver, or the UART ROM bootloader.

A stock OpenOCD 0.12 with RISC-V support is expected to load the minimal cfg
too (it uses only vanilla commands: `interface/cmsis-dap.cfg`, `target/swj-dp.tcl`,
`target create … riscv`); that combination was **not** verified on this machine
(no stock OpenOCD installed), so it is stated as an expectation, not a result.

### The SWD link is retried automatically

`probe_state.sh` and `openocd_reset_run.sh` retry a failed session up to four
times before reporting it. When a tool does report one, re-running it is
usually the whole fix.

The flashing paths retry the same way, because the two failures arrive through
the same window: `flash_fw.sh` and `test_uart_capture.sh` re-run the whole
session when the output carries `Examination failed` / `stalled AP` /
`dmstatus=0x0` / `Flash write data timed out`, and `flash_logic.sh` does the
same around `agrv write_fpga_config` (success is judged by the
`agrv wrote fpga configuration` line, not by the exit code). The budget is
`AGM_OPENOCD_ATTEMPTS` (default 3, `flash_logic.sh` 4); each retry is logged as
`WARN: openocd session failed (attempt N/M) - retrying`.

A *failed* session can leave the CPU halted, which looks like a dead board on
the next capture (0 bytes) even though `reset run` says nothing is wrong.
Re-flashing firmware (`flash_fw.sh`, or the vendor `agm_oo.sh fw`) brings it
back.

## `devsync.sh` — optional helper for a two-checkout setup

`west build` compiles the module checkout inside the workspace
(`<ws>/modules/hal_ag32`). If you keep a *second* checkout as your editing tree,
this helper mirrors it into the workspace without a commit per iteration
(push/pull does the same thing, one commit at a time):

```sh
tools/devsync.sh status          # what a push would change (writes nothing)
tools/devsync.sh push --apply    # mirror the dev tree into the workspace
#   ... build / flash / capture from <ws> ...
tools/devsync.sh restore --apply # workspace back on origin/main
```

It works because `hal_ag32` is the *manifest* repository of the workspace and
`west update` never touches the manifest repository (west 1.5: "This command
does not alter the manifest repository's contents"). Before the manifest
reversal this trick would have been undone by the next `west update`.

Rules while a mirror is in place:

* do not `git pull` / `git checkout` / `git clean` in the workspace
  checkout — they either fail (the tree is dirty) or silently discard the
  mirror; use `restore` instead;
* do not hand-edit the workspace checkout — `push` overwrites it;
* do not run `west update` until you have run `restore` (the mirrored
  `west.yml` can carry a different pin);
* **a build off a mirrored tree is not a result "at `<sha>`"** — only
  `restore` plus a clean rebuild is. Anything that goes into `docs/` as
  "实测" has to come from the clean tree.

---

## `build_bitstream.sh`

Wrapper that produces a Quartus-ready logic directory from the board's
devicetree. The bitstream itself comes from `compile_bitstream.sh` after
Quartus has routed the design.

### Whole flow and who does what

```
dtsi ──build_bitstream.sh (us, this machine)──► <build>/logic/
                                                 │
                                                 │  quartus_sh -t af_quartus.tcl
                                                 │  (user: any Quartus install, Windows is
                                                 │   what the vendor flow uses)
                                                 ▼
                                    simulation/modelsim/<design>.vo
                                                 │
                                                 │  compile_bitstream.sh (us, this machine:
                                                 │  the Linux Supra package is enough)
                                                 ▼
                                          <design>.bin ──► $AGM_BITSTREAM_BIN ──► west flash
```

With the vendor's own Supra 2026.03.b0 the Linux step
reproduces the Windows result (same `0 fatals, 0 errors, 1 warnings,
88 infos`, same 99944-byte image) and the image it writes boots the board.
It is **not** byte-reproducible -- the placer/routing is seeded per run, so
two runs differ by a few hundred bytes spread over the image (and the
Windows artifact differs from a Linux one the same way). Judge the result by
"it boots and behaves", not by md5.

The pinout lives in `dts/riscv/agm/agrv2k-pins.dtsi` (the
`agm,agrv2k-pins` node). `board.ve` is **generated**, not a source:

* `AGM_DTS=auto` — `build_bitstream.sh` runs cpp + dtc on
  `<board_dir>/*.dts` (plus `AGM_DTS_OVERLAY`) and feeds the result to
  `generate_board_ve.py`. One command, no Zephyr cmake build. The
  `board.ve` and the whole logic directory this produces
  are byte-identical to the ones the build-first path produces.
* `AGM_DTS=<path>` — use a dts that a Zephyr build already preprocessed
  (`${PROJECT_BINARY_DIR}/zephyr.dts`); this is what `west build -t logic`
  passes. Still the path to use when a sample overlay changes the pins.
* neither — reuse the `board.ve` already on disk.

The `.ve` step itself cannot be removed: it is the input format of the
vendor's `gen_vlog` (MCU macro instantiation, pin capability checks, the
`.hx/.vx/.vex/.asf/.qsf` generation and `pre_logic.tcl`), not something of
ours. See [BOARD-VE-FROM-DTS.md](../docs/BOARD-VE-FROM-DTS.md).

### Pipeline

| Stage | Who | Tool | Input | Output |
| --- | --- | --- | --- | --- |
| 1. netlist | us | `python3 gen_vlog -d <device>` | `board.ve` | `board.{hx,vx,vex}` |
| 2. skeleton | us | `af_cmd -F pre_logic.tcl` | `board.{hx,vx,vex}` | `board.{asf,sdc,qpf,qsf}`, `af_quartus.tcl`, `af_ip.tcl`, `*.pre.asf`, `*.post.asf` |
| 3. synthesis + place & route | **you, on a Quartus machine** | `quartus_sh -t af_quartus.tcl` | `board.qsf` | `simulation/modelsim/board.vo` |
| 4. bitstream | us | `compile_bitstream.sh` (`af_cmd -B --batch --mode QUARTUS`) | `board.vo` | `board.bin` + `board_batch.bin` |

### Usage

```sh
# normal path: from a Zephyr build (regenerates logic/ whenever the dts or
# the pins change, and runs the package check below)
west build -d <build_dir> -t logic

# firmware only (no AgRV SDK on this machine): keep the logic target out of
# the default build, then use `-t logic` (or build_bitstream.sh) elsewhere
west build -d <build_dir> -- -DAGM_LOGIC_TARGET=OFF

# standalone: no Zephyr build needed with AGM_DTS=auto
build_bitstream.sh <board_dir> <logic_dir>

# stage 4, after Quartus has produced simulation/modelsim/<design>.vo
compile_bitstream.sh <logic_dir> [<bitstream.bin>]
```

`<logic_dir>` ends up with everything Quartus needs. On the Quartus machine
keep `simulation/modelsim/<design>.vo` where it is (copy the whole
`logic/` directory back when in doubt) -- `compile_bitstream.sh` looks for
exactly that path and tells you to run `quartus_sh -t af_quartus.tcl` when it
is missing.

### The package (logic_device) is checked before gen_vlog

Which pins exist depends on the package, so `build_bitstream.sh` takes
`gen_vlog -d` from the pins node's **`agm,logic-device`** property
(`AGM_LOGIC_DEVICE` overrides) and fails if `board.ve` names a pin that
device does not have:

```
>>> [0/2] logic device from dts: AGRV2KL100
>>> [0/2] package check: 24 pin(s), all present on AGRV2KL100
```

That check exists because `gen_vlog` itself only warns
(`IP pin PIN_x is ignored because no IP macro is specified`) and still
exits 0, which would leave those pins unconnected without saying so.
The per-package numbers are in the table above.

### Environment overrides

| Variable | Default | Purpose |
| --- | --- | --- |
| `AGRV_SDK_PATH` | `$HOME/AgRV_pio` | root of the AgRV PlatformIO install |
| `SUPRA_HOME` | `$AGRV_SDK_PATH/packages/tool-agrv_logic` | dir with `bin/af_cmd` |
| `AGRV_PLATFORM_ETC` | `$AGRV_SDK_PATH/platforms/AgRV/etc` | dir with `gen_vlog`, `pre_logic.tcl`, `gen_logic.tcl` |
| `agm,logic-device` (dts) | `AGRV2KL100` | package the pin map targets; read from the pins node |
| `AGM_LOGIC_DEVICE` | (the dts property) | override the package (L100/L100H/L64/L64H/L48/Q32) |
| `AGM_LOGIC_DESIGN` | `board` | top-level design name (must match `${LOGIC_DESIGN}.{hx,vx,vex}`) |
| `AGM_LOGIC_MODULE` | same as `AGM_LOGIC_DESIGN` | verilog top module name |
| `AGM_LOGIC_TOPPIN` | `false` | whether board.ve controls top pins |
| `AGM_DTS` | unset | preprocessed dts to render `board.ve` from, or `auto` (cpp+dtc here) |
| `AGM_BOARD_DTS` / `AGM_DTS_OVERLAY` | single `<board_dir>/*.dts` / empty | inputs for `AGM_DTS=auto` |
| `AGM_SKIP_BOARD_VE_GEN` | unset | use the `board.ve` already on disk |
| `AGM_ALLOW_PIN_CONFLICTS` | unset | downgrade duplicate-pin errors to warnings |
| `AGM_IP_DIR` | (empty) | optional IP search path |
| `AGM_BITSTREAM_BIN` | unset | where `compile_bitstream.sh` copies the result, and the bitstream path `west flash` (via `board_common.cmake`) resolves to. Plan A: the build default is `<build_dir>/zephyr/board.bin`; with nothing set the Supra output stays in `<logic_dir>/` |
| `AGM_SUPRA_QUARTUS_SDC` / `_FITTING` / `_FITTER` / `_EFFORT` / `_HOLDX` / `_SKEW` / `_X` | vendor values | Supra's own place & route settings |

### Standalone debugging

If a build is failing, run the stages manually to localise the problem:

```sh
cd boards/agm/agrv2k_407
SUPRA_HOME=...  AGRV_PLATFORM_ETC=...  \
    build_bitstream.sh . logic
```

Or invoke each tool directly to inspect intermediate files:

```sh
cd boards/agm/agrv2k_407

# Stage 1
python3 $AGRV_PLATFORM_ETC/gen_vlog \
    -c board.hx -d AGRV2KL100 board.ve board.vx -x board.vex

# Stage 2
$SUPRA_HOME/bin/af_cmd \
    -L pre_logic.log \
    -X 'set LOGIC_DEVICE {AGRV2KL100}' \
    -X 'set LOGIC_DESIGN {board}' \
    -X 'set LOGIC_MODULE  {board}' \
    -X 'set LOGIC_TOPPIN {false}' \
    -X 'set LOGIC_DIR    {.}' \
    -X 'set LOGIC_VV     {board.vx}' \
    -X 'set BOARD_ASF    {board.asf}' \
    -X 'set VEX_FILE     {board.vex}' \
    -F $AGRV_PLATFORM_ETC/pre_logic.tcl

# Stage 3 — do NOT run this by hand: it needs Quartus's post-route netlist
# first (quartus_sh -t af_quartus.tcl → simulation/modelsim/board.vo),
# and compile_bitstream.sh supplies the fitting parameters the vendor flow uses.
compile_bitstream.sh . board.bin
```

### Outputs

After stage 2 the build tree's `logic/` directory contains the Quartus inputs;
stages 3+4 add the netlist and the image:

| File | Purpose |
| --- | --- |
| `board.ve` | function→pin text, rendered from the dts (gitignored, regenerated) |
| `board.hx` | header with `HSE_FREQ`, `SYS_FREQ`, pin map |
| `board.vx` | verilog netlist (post-vlog, Quartus input) |
| `board.vex` | constraint file consumed by `gen_logic.tcl` |
| `board.asf` / `board.pre.asf` / `board.post.asf` | pre/post-synthesis hooks |
| `board.sdc` | timing constraints |
| **`board.qsf` / `board.qpf`** | **the Quartus project** (`TOP_LEVEL_ENTITY = board`) |
| `simulation/modelsim/board.vo` | Quartus post-route netlist (stage 4 input) |
| `board.bin` / `board_batch.bin` | the bitstream / the same with option bytes |
| `pre_logic.log` | af_cmd log |

None of this is consumed by the firmware build; the whole `logic/`
directory (and `board.ve`) is gitignored and regenerated on demand.

### Troubleshooting

| Symptom | Likely cause | Fix |
| --- | --- | --- |
| `Error: af_cmd not found` | `SUPRA_HOME` not set or wrong | `export AGRV_SDK_PATH=~/AgRV_pio` (or set `SUPRA_HOME` directly) |
| `Error: gen_vlog not found` | `AGRV_PLATFORM_ETC` not set | `export AGRV_SDK_PATH=~/AgRV_pio` (or set `AGRV_PLATFORM_ETC` directly) |
| `Error: couldn't open "top.hx"` | `LOGIC_DESIGN` does not match `board.{hx,vx}` | make sure `AGM_LOGIC_DESIGN=board` (the default) and `gen_vlog` step wrote `board.hx` |
| `Error: license` | Supra license server not reachable | check `af_cmd --version` once interactively |
| `User constraints coverage is too low` | warning only, build succeeds | add `SDC_FILE` via `AGM_LOGIC_TOPPIN=true` and a richer `.ve` |
| `Error: ... names N pin(s) that <device> does not have` | `agm,logic-device`/`AGM_LOGIC_DEVICE` does not match the pin map | fix the package, or prune `mcu-pins`/`cpld-pins` to that package |
| `Error: .../simulation/modelsim/board.vo not found` | Quartus never ran (or its output was not copied back) | `quartus_sh -t af_quartus.tcl` on the Quartus machine |

---

## Adding new boards

The step-by-step recipe (in-tree or from your own `BOARD_ROOT`, with the
Quartus stage) is [DTSI-GUIDE.md](../docs/DTSI-GUIDE.md) §3. In short: copy
`boards/agm/agrv2k_407/`, rename the board in `board.yml` / `Kconfig.*` /
`<board>_defconfig`, point its `board.cmake` at this module's
`boards/agm/agrv2k/shared/board_common.cmake`, replace the pin map in
`<board>.dts` with your own `agrv2k_pins` node (including
`agm,logic-device`), and let the generator produce the board's
`<board>-pinctrl.dtsi`. The board's `Kconfig.defconfig` should `rsource`
the shared `Kconfig.defconfig.body` (console/UART/GPIO/PINCTRL defaults)
and its `<board>_defconfig` selects `SOC_AGM_AGRV2K`.

The Supra toolchain does not need to be re-configured for a new board —
only the pin map (and therefore `board.ve`) changes. A board on a smaller
package additionally needs `agm,logic-device` to match and its pin list
pruned; the package check stops the build otherwise.
