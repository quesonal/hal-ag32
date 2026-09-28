# AgRV2K shared board infrastructure

This directory holds files that are **byte-identical across all AgRV2K
board variants** (`agrv2k_103`, `agrv2k_303`, `agrv2k_407`,
`agrv2k_test`). Per-board files in `boards/agm/agrv2k_<V>/` either
`include()` these or `rsource` them; per-board CMake variables
(`AGRV_FLASH_SIZE`, `AGRV_BITSTREAM_ADDR`) flow into the shared
build glue via the parent `board.cmake`'s `set()` calls before the
`include(shared/board_common.cmake)` line.

## Why not `boards/common/agm/`?

Zephyr's `boards/common/` directory is reserved for **runner
includes** (one file per runner: `openocd.board.cmake`,
`jlink.board.cmake`, `nrfjprog.board.cmake`, ...). It is the
canonical "this is a generic helper for a runner" location, not a
"vendor-shared board artefacts" location. Adding `boards/common/agm/`
would mix two conventions and create a precedent that other vendors
might not want to follow. The vendor-shared subdir lives next to
its users instead — `boards/agm/agrv2k/shared/` mirrors the natural
vendor grouping at the directory tree level.

## Files

| File | Purpose | Per-board usage |
|---|---|---|
| `agrv2k-board.dtsi` | symlink → `dts/riscv/agm/agrv2k-board.dtsi`;shared board-level devicetree (LEDs, buttons, `gpio4`/`gpio6` enables) | each `<board>.dts` does `#include <agm/agrv2k-board.dtsi>` |
| `agrv_fpga_decomp.inc` | 21 KB Supra FPGA config (byte-identical across boards) | referenced by each `board.cmake` |
| `support/openocd.cfg.in` | cmake template (substitutes `@FLASH_SIZE@` + `@BITSTREAM_ADDR@`) | rendered into per-board `support/openocd.cfg` |
| `agrv32flash.board.cmake` | 32-line runner default (`--skip-bitstream`) | each board's `agrv32flash.board.cmake` is a 1-line include |
| `Kconfig.defconfig.body` | 30-line board-level defaults (UART/GPIO/CLINT/pinctrl) | each board's `Kconfig.defconfig` is a 5-line guard + `rsource` |
| `board_common.cmake` | ~140-line build glue (bitstream + runners + `flash`/`flash-logic` targets + cfg generation) | included from each board's `board.cmake` |

## Adding a new AgRV2K variant

1. **Create** `boards/agm/agrv2k_<new>/`.
2. **Copy** the per-board files from `boards/agm/agrv2k_407/`:
   `board.yml`, `Kconfig.agrv2k_407`, `Kconfig.defconfig`,
   `agrv2k_407_defconfig`, `agrv2k_407.dts`, `board.cmake`,
   `agrv32flash.board.cmake`. Rename per-board (e.g.
   `Kconfig.agrv2k_407` → `Kconfig.agrv2k_<new>`,
   `agrv2k_407_defconfig` → `agrv2k_<new>_defconfig`, etc.).
3. **Edit** per-board content:
   - `board.yml`: `name`, `full_name`
   - `Kconfig.agrv2k_<new>`: `BOARD_AGM_AGRV2K_<NEW>` symbol + help
   - `Kconfig.defconfig`: `if BOARD_AGM_AGRV2K_<NEW>` (replacing 407)
   - `agrv2k_<new>_defconfig`: `CONFIG_BOARD_AGM_AGRV2K_<NEW>=y`
   - `agrv2k_<new>.dts`: model string, FLASH `reg`, `&cpu0 clock-frequency`, `compatible`
   - `board.cmake`: `AGRV_FLASH_SIZE` + `AGRV_BITSTREAM_ADDR`
   - `agrv2k_<new>.dts`: `#include <agm/agrv2k-board.dtsi>` after the
     `zephyr/dt-bindings/*` includes (that file needs `GPIO_ACTIVE_LOW`
     and `INPUT_KEY_*`), plus the usual `#include <agm/agrv2k-pins.dtsi>`
   - `board.ve` is **generated**, not copied: `west build -t logic` renders
     it from the merged devicetree via `tools/generate_board_ve.py`
     (see `../../docs/BOARD-VE-FROM-DTS.md`). The vendor tree's per-board
     `board.ve` is stale — do not copy it back in.
4. **Don't edit** the per-board `agrv32flash.board.cmake` or
   `support/openocd.cfg` — both are auto-generated/auto-included.

## Per-board values reference table

| Board | SYSCLK | FLASH size | `AGRV_FLASH_SIZE` | `AGRV_BITSTREAM_ADDR` | `AGRV_FLASH_SIZE_KB` |
|---|---|---|---|---|---|
| agrv2k_103 | 100 MHz | 256 KB | `0x40000` | `0x80027000` | `256` |
| agrv2k_303 | 200 MHz | 256 KB | `0x40000` | `0x80027000` | `256` |
| agrv2k_407 | 240 MHz | 1 MB | `0x100000` | `0x800e7000` | `1024` |
| agrv2k_test | 100 MHz | 256 KB | `0x40000` | `0x80027000` | `256` |

Source of truth: SDK `boards/agrv2k_<V>.json` (`f_cpu` and
`upload.maximum_size` fields).