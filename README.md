# hal_ag32 — Zephyr module for the AGM AgRV2K

[English](README.md) | [简体中文](README.zh-CN.md)

Out-of-tree [Zephyr](https://zephyrproject.org) module for the **AGM AgRV2K** SoC
(RV32IMAFC core plus an on-die programmable FPGA fabric). It carries the SoC
glue, the four board definitions, the devicetree bindings, the drivers and the
west runners — and it is also the **west manifest** of this workspace, so one
command reproduces the whole tree (module + the companion
[`hal_ag32_samples`](https://github.com/quesonal/hal-ag32-samples) repo as a west
sub-module + Zephyr):

```sh
west init -m <this repo> --mr main ~/agrv-ws
cd ~/agrv-ws && west update
```

Zephyr itself is pinned to an upstream SHA and carries no AGM-specific lines.

## What is in here

| | |
|---|---|
| **Boards** | `agrv2k_407` (the board everything below is verified on), `agrv2k_103`, `agrv2k_303`, `agrv2k_test` (build-only) |
| **Drivers** | UART (PL011-register compatible), GPIO + pinctrl, PLIC, DMA, I2C, SPI (incl. dual/quad lines and full-duplex through the fabric), TIMER/GPTIMER/RTC counters, PWM, WDOG/IWDG, CAN, Ethernet MAC0 + RMII, USB device (CDC-ACM / HID), on-die flash, external SPI NOR, CRC, CPLD window |
| **Fabric (CPLD)** | user logic in the bitstream: the AHB window from the MCU, three pin-map forms, electrical attributes, custom IP integration — [CUSTOM-IP.md](docs/CUSTOM-IP.md) |
| **Bootloader / DFU** | stage-1 loader (`drivers/misc/boot_agm.c`): A/B image stores, an on-die execution slot and fabric-bitstream slots, three host upload paths (console, `agrv32flash`, mcumgr), signed images, per-chip binding, production lock — [BOOT-DFU-STATUS.md](docs/BOOT-DFU-STATUS.md); the address map is [FLASH-LAYOUT.md](docs/FLASH-LAYOUT.md) |
| **Samples** | 58 apps in the companion [`hal_ag32_samples`](https://github.com/quesonal/hal-ag32-samples) repo (pulled in as a west sub-module at `<ws>/modules/hal_ag32_samples/`); each README says what it proves and what hardware it needs |
| **Bitstream** | the samples are written against one 200 MHz reference image; it is *not* redistributed in either repo — `hal_ag32_samples/samples/bitstreams/README.md` points the reader at their own copy (via `AGM_BITSTREAM_BIN`) |

## Requirements

Building needs the Zephyr SDK (with the RV32IMAFC multilib) and `west`; see
Zephyr's own getting-started guide for those. The module's tools are plain
Python plus `west` invocations and read these variables:

| Variable | Value |
|---|---|
| `ZEPHYR_SDK_INSTALL_DIR` | the Zephyr SDK install |
| `ZEPHYR_TOOLCHAIN_VARIANT` | `zephyr` |
| `AGRV_SDK_PATH` | the AgRV PlatformIO install (default `~/AgRV_pio`). `tool-agrv_openocd` is enough for firmware work; `west build -t logic` additionally needs `tool-agrv_logic` |
| `SUPRA_HOME` | `$AGRV_SDK_PATH/packages/tool-agrv_logic` (bitstream rebuilds only) |

Only the bitstream rebuild and the ROM-bootloader path need the AgRV SDK at all.
The companion [`hal_ag32_samples`](https://github.com/quesonal/hal-ag32-samples) repo
is pulled in by this manifest; [SAMPLE-WORKFLOW.md](https://github.com/quesonal/hal-ag32-samples/blob/main/docs/SAMPLE-WORKFLOW.md)
is the step-by-step for writing and running a sample.

## Quick start

```sh
# in a west workspace built with west init -m <this repo> (above);
# `west update` has placed the companion samples repo at
# <ws>/modules/hal_ag32_samples/, so the build path is one directory deeper:

west build -b agrv2k_407 \
           modules/hal_ag32_samples/samples/hello_world   # canonical 200 MHz bitstream: no overlay
west flash                                              # firmware + bitstream (needs the on-board CMSIS-DAP probe)
tools/test_uart_capture.sh                              # open the console first, then reset -> you get the banner
```

`west flash` writes firmware *and* bitstream by default — `--skip-bitstream` and
`--bitstream-only` split the two, and `tools/flash_fw.sh` / `tools/flash_logic.sh`
do the same. `AGRV_ADAPTER` picks the debug probe (default `cmsis-dap`, the
on-board one).

No probe, or a board that will not attach over SWD? Put `BOOT0` high, power
cycle, and use the ROM bootloader:

```sh
west flash --runner agrv32flash                   # UART, firmware only
```

## Documentation

| Question | Document |
|---|---|
| How to write, build, flash and debug a sample | [SAMPLE-WORKFLOW.md](https://github.com/quesonal/hal-ag32-samples/blob/main/docs/SAMPLE-WORKFLOW.md) |
| Rules, symptom table and the ROM-bootloader recovery path for flashing and capturing | [FLASH-AND-CAPTURE.md](docs/FLASH-AND-CAPTURE.md) |
| Which flash runner writes what, and how to switch | [FLASH.md](docs/FLASH.md) |
| Where everything lives in flash: loader, A/B pair, execution slot, fabric slots, boot records, bind-salt, option area | [FLASH-LAYOUT.md](docs/FLASH-LAYOUT.md) |
| Bootloader / DFU: upload paths, signed images, per-chip binding, production lock | [BOOT-DFU-STATUS.md](docs/BOOT-DFU-STATUS.md) |
| Signing and key handling: algorithms, container format | [SIGNED-IMAGES-PLAN.md](docs/SIGNED-IMAGES-PLAN.md) |
| Wiring your own logic into the fabric (AHB window, pin-map forms, electrical attributes) | [CUSTOM-IP.md](docs/CUSTOM-IP.md) |
| How the dtsi is organised, SDK concepts mapped | [DTSI-GUIDE.md](docs/DTSI-GUIDE.md) |
| Regenerating `board.ve` and the Quartus inputs | [BOARD-VE-FROM-DTS.md](docs/BOARD-VE-FROM-DTS.md) |
| Pin names per package | [AG32-PINOUT.md](docs/AG32-PINOUT.md) |
| Host-side USB/CDC-ACM flakiness and what to do about it | [CDC-ACM-FLAKE-SOLUTIONS.md](docs/CDC-ACM-FLAKE-SOLUTIONS.md) |

The driver headers under `include/zephyr/drivers/` are the API reference; every
sample ships a `sample.yaml` naming the board it is meant for. The whole set is
indexed in
[`hal_ag32_samples/samples/README.md`](https://github.com/quesonal/hal-ag32-samples/blob/main/samples/README.md);
the docs in this repo (this table) cover the module-side questions.

## Common mistakes

* **Console is garbled but the board is alive** — the bitstream's clock and the
  firmware's clock disagree. The canonical image is 200 MHz and matches the
  board default (no overlay); a 100 MHz image needs
  `-- -DEXTRA_DTC_OVERLAY_FILE=<100mhz overlay>`, otherwise UART0 runs at the
  wrong divisor (115200 requested → 57600 actual).
* **`west flash` writes firmware *and* bitstream** by default. Use
  `--skip-bitstream` or `--bitstream-only` to split the two (they are mutually
  exclusive). A firmware-only flash keeps the fabric as it was.
* **The boot banner only appears at reset** — open the console *before*
  resetting (`tools/test_uart_capture.sh` does); `minicom`, `cat /dev/ttyACM0`
  or a late attach show only the periodic output.
* **`west build` is not the regression net.** twister additionally enables
  `CONFIG_COMPILER_WARNINGS_AS_ERRORS=y` and `--edtlib-Werror`, so a green build
  can still fail CI-shaped checks.
* **Never re-run an SDK "prepare logic" step inside a generated `logic/`
  directory.** `board.v` and `board.vex` are two halves of one generation run; a
  second run renames the pin constraints (`SPI0_SI_IO0 PIN_92` vs `si_io0`) and
  the tool that binds pins only warns before placing those IOs on other pins —
  the board then cannot reach the flash at all.
* **Dual/quad SPI reads need a bitstream that routes IO2/IO3 to the flash's
  WP#/HOLD# pins.** With any other bitstream those calls return `-ENOTSUP` or
  read noise; [`samples/spi_quad_read`](https://github.com/quesonal/hal-ag32-samples/blob/main/samples/spi_quad_read/README.md) says
  which images work.
* **Do not touch `/dev/ttyACM1`** — on the eval board that is the on-board
  ESP32-C3, not the AgRV2K console (`/dev/ttyACM0`).
* **Flashing the wrong clock domain?** `west flash` writes firmware and the
  bitstream at the fabric address; if the board looks dead afterwards, see
  [FLASH-AND-CAPTURE.md](docs/FLASH-AND-CAPTURE.md) §10 (recovery via the
  ROM bootloader).

## License

Apache-2.0 — see [LICENSE](LICENSE). [NOTICE](NOTICE) lists the vendor SDK
components this module builds against.
