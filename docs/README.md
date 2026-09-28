# Documentation

What to read, by question. Everything listed here ships with the repository;
the per-peripheral dev board records and design diaries behind it are not part of
this publication.

## Working on the dev board

| Question | Document |
|---|---|
| Build, flash and debug a sample; the board's conventions | [SAMPLE-WORKFLOW.md](https://github.com/quesonal/hal-ag32-samples/blob/main/docs/SAMPLE-WORKFLOW.md) |
| Which flash runner writes what, and how to switch between them | [FLASH.md](FLASH.md) |
| **Where everything lives**: loader, A/B, execution slot, fabric slots, boot records, bind-salt sector, option area (both layout families, and who writes what) | [FLASH-LAYOUT.md](FLASH-LAYOUT.md) |
| Wiring your own logic into the fabric: the AHB window, the three pin-map forms, the electrical attributes, and what is still missing | [CUSTOM-IP.md](CUSTOM-IP.md) |
| Rules, symptom table and the ROM-bootloader recovery path | [FLASH-AND-CAPTURE.md](FLASH-AND-CAPTURE.md) |
| How the dtsi is organised; SDK concepts, mapped | [DTSI-GUIDE.md](DTSI-GUIDE.md) |
| Pin names per package | [AG32-PINOUT.md](AG32-PINOUT.md) |
| Regenerating `board.ve` / the Quartus inputs | [BOARD-VE-FROM-DTS.md](BOARD-VE-FROM-DTS.md) |

## Features

| Question | Document |
|---|---|
| Bootloader / DFU: A/B + TRIAL, upload paths, signed images, per-chip binding, production lock | [BOOT-DFU-STATUS.md](BOOT-DFU-STATUS.md) —— its **§0.1 is the current DFU + verification pipeline** (one page: what is on the chip, the per-boot sequence, the update flow, the Kconfig defaults, what is *not* done, and the per-boot cost) |
| Running the whole verification chain end to end (salt provisioning + bound app + signed fabric + anti-rollback + gated commands, with the refusal paths) | [`hal_ag32_samples/samples/verify_flow`](https://github.com/quesonal/hal-ag32-samples/blob/main/samples/verify_flow/README.rst) + [`verify_flow.py`](../tools/verify_flow.py) |
| Signed images: algorithm choice, container format, key handling | [SIGNED-IMAGES-PLAN.md](SIGNED-IMAGES-PLAN.md) |
| Production lock (R2): what is gated, how a command is authorized | [SIGNED-IMAGES-PLAN.md](SIGNED-IMAGES-PLAN.md) §11 |
| Per-chip binding: what a copy on another board does, and what protects the salt | [SIGNED-IMAGES-PLAN.md](SIGNED-IMAGES-PLAN.md) §12 |

The driver headers under `include/zephyr/drivers/` are the API reference; the
samples under
[`hal_ag32_samples/samples/`](https://github.com/quesonal/hal-ag32-samples/tree/main/samples)
are the usage examples, and each one ships a
`sample.yaml` that says which board it is meant for.

## Other material

The per-peripheral status chapters, dev board measurements and design diaries
that back these guides stay in the development tree, outside this publication.
