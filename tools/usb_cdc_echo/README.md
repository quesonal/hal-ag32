# USB0 CDC-ACM echo — on-target verification

This directory hosts the **on-target** end-to-end test for it.

## What this validates

* `drivers/usb/udc/udc_agm.c` (in-tree Zephyr, commit `a3034b5cced`)
  enumerates as a CDC-ACM device on USB0 (PIN_70/71, second USB port).
* The host sees `/dev/ttyACMx`; chars sent down that tty are echoed back.

## How the "echo" actually works

`samples/subsys/usb/cdc_acm` is a **pure USB echo at the CDC-ACM class
driver level** — no physical UART is involved:

```
host /dev/ttyACMx  ──USB OUT──>  rx_ringbuf  ──uart_fifo_read──┐
                                                                  │
                              ┌──uart_fifo_fill──  tx_ringbuf  ──USB IN──>
                              │       (sample main loop)
                              └──────────────────────────────────────┘
```

Concretely:

* `zephyr,cdc-acm-uart` (compatible in app.overlay) is a **virtual UART
  controller** on the USB bus — it implements `uart_driver_api` with
  two Zephyr ring buffers (`rx_ringbuf`, `tx_ringbuf`) backing
  `uart_fifo_read`/`uart_fifo_fill`. There is **no** binding to any
  PL011 UART peripheral, so UART0/UART1 are not touched.
* The sample main loop reads from `uart_fifo_read` (drains
  `rx_ringbuf`, which is fed by the USB OUT bulk endpoint callback)
  and writes back via `uart_fifo_fill` (fills `tx_ringbuf`, which is
  drained to the USB IN bulk endpoint by `tx_work_handler`).
* Consequently **no physical pin shorting is required** — the echo
  round-trips entirely inside the USB controller.

## Hardware prerequisites

* `agrv2k_407` board (the LQFP-100 / 1 MB FLASH variant).
* The shipped bitstream (`example_board.bin`) flashed at `0x800e7000`
  — it provides USB0 in OTG mode + 60 MHz `PLLCLK1` clock.
* **Two** USB cables:
  1. The CMSIS-DAP / CDC-ACM debug cable (already used for flashing +
     `minicom -D /dev/ttyACM0 -b 115200` Zephyr console). This
     enumerates as the **first** `/dev/ttyACM0` — that's the Zephyr
     console, NOT the echo tty.
  2. A second USB cable on **PIN_70/71 (USBD-/USBD+)**. This is the
     port that enumerates as the CDC-ACM echo device, typically
     `/dev/ttyACM1`.

## Build

```bash
source <your-venv>/bin/activate
cd ~/zephyrproject/zephyr
west build -b agrv2k_407 samples/subsys/usb/cdc_acm
```

Expected: `ROM: ~62 KB / RAM: ~20 KB`, `zephyr.bin` at
`build/zephyr/zephyr.bin`.

## Flash + test (one-shot)

```bash
~/zephyr-hal-ag32/tools/usb_cdc_echo/flash_and_test.sh
```

The script does:

1. `../flash_fw.sh` (surgical, keeps the bitstream intact; ends with
   `reset run` + `shutdown`, so the new firmware is already executing
   when the script resumes).
2. Polls `/dev/ttyACM{3,2,1}` (preferring higher-numbered to skip the
   debug tty at ACM0) for ≤ 10 s. The new CDC-ACM tty enumerates ~1 s
   after the reset.
3. Sets 115200 baud, sends `hello agm HHMMSS\r`, reads the echo.
4. PASS if the response contains `hello agm`; FAIL otherwise.

## Known caveats

* **OTG role flip**: the bitstream is OTG-mode (`USB0_ID PIN_78`).
  The driver writes `USBMODE.CM = device (0x2)` at init time, so
  enumeration works regardless of the ID-pin level. If you want
  rock-solid role stability, rebuild a `USB0 device` bitstream and
  re-flash (`tools/flash_logic.sh example_board.bin`).
* **CMSIS-DAP warmup**: the on-board probe is occasionally flaky
  on Linux. The script inherits the warmup via `../flash_fw.sh`. If
  it still drops the probe, replug once.
* **No RC SOF calibration**: cdc_msc-style examples call `osc_cal()`
  to trim the internal RC against USB SOF. The 407 bitstream uses
  HSE → PLL → 60 MHz for USB, so this is not needed.

## Manual debug

If `flash_and_test.sh` fails, the next thing to look at:

* `dmesg | tail -30` — does `cdc_acm` show up in `lsusb`?
* `lsusb -v -d <VID:PID>` — check the descriptor dump matches CDC-ACM
  (bInterfaceClass = 2 Communications, bInterfaceSubClass = 2 ACM).
* `minicom -D /dev/ttyACM0 -b 115200` — that's the Zephyr console,
  not the echo. The echo tty is the next one enumerated
  (`/dev/ttyACM1` typically).
* `openocd ... -c "mdw 0x41001000 0x10"` — check USBCMD/USBSTS/PORTSC
  values against the FSL/ChipIdea register spec (PORTSC.CCS = 1
  after enumeration, PORTSC.PSPD = 0 for FS).
