# Copyright (c) 2026 AgRV Contributors
# SPDX-License-Identifier: Apache-2.0

'''AgRV32flash runner for AGM AgRV2K boards.

Wraps the ``agrv32flash`` tool shipped in the PlatformIO
``tool-agrv_flashloader`` package. Talks to the on-die ROM bootloader
over UART — the same UART the integrated CMSIS-DAP probe on agrv2k_407
exposes via CDC-ACM (``/dev/ttyACM0``).

Two flash steps, both optional:

* ``-l bitstream.bin`` writes the FPGA bitstream to its FLASH address
* ``-w firmware.bin`` writes the firmware to its FLASH address

After both writes, ``-g`` jumps to the firmware entry.

The chip must already be in serial-download mode when this runs: BOOT0 high
and BOOT1 low **at power-up** (on the dev board BOOT1 is tied to GND and
BOOT0 is a pull-down with a jumper to 3.3V). The reset sequence
(``-i GPIO_string``) is intentionally left to the caller — the board's
BOOT0 wiring is the user's (jumper, button, ...).
'''

import os
from os import path
from pathlib import Path

from runners.core import MissingProgram, RunnerCaps, ZephyrBinaryRunner


def _sdk_root():
    '''AgRV PlatformIO SDK root: $AGRV_SDK_PATH, else ~/AgRV_pio.'''
    return os.environ.get('AGRV_SDK_PATH') or '~/AgRV_pio'


# Same knob every hal_ag32 tool uses: one SDK root, with
# $AGRV32FLASH / --agrv32flash as explicit overrides.
DEFAULT_AGRV32FLASH = _sdk_root() + '/packages/tool-agrv_flashloader/bin/agrv32flash'


def _expand_user(p):
    return os.path.expanduser(os.path.expandvars(p))


class Agrv32flashBinaryRunner(ZephyrBinaryRunner):
    '''Runner front-end for agrv32flash (AgRV2K ROM bootloader).'''

    # ---- ZephyrBinaryRunner hooks ------------------------------------

    @classmethod
    def name(cls):
        return 'agrv32flash'

    @classmethod
    def capabilities(cls):
        # 'reset' is supported: the tool's -g option starts execution.
        return RunnerCaps(commands={'flash'}, reset=True)

    @classmethod
    def do_add_parser(cls, parser):
        parser.add_argument(
            '--agrv32flash', default=None,
            help='path to agrv32flash (default: $AGRV32FLASH or '
                 '~/AgRV_pio/packages/tool-agrv_flashloader/bin/agrv32flash)')
        parser.add_argument(
            '--serial', default='/dev/ttyACM0',
            help='serial port for agrv32flash (default: %(default)s)')
        parser.add_argument(
            '--baud', type=int, default=57600,
            help='serial baud rate (default: %(default)s)')
        parser.add_argument(
            '--bitstream', default=None,
            help='FPGA bitstream path (default: board.bin in board_dir)')
        parser.add_argument(
            '--bitstream-addr', default='0x800e7000',
            help='bitstream load address (default: %(default)s — '
                 '1 MB FLASH end - 100 KB reserved, matches '
                 'AgRV_pio default for agrv2k_407)')
        parser.add_argument(
            '--firmware-addr', default='0x80000000',
            help='firmware load address (default: %(default)s)')
        parser.add_argument(
            '--skip-bitstream', action='store_true',
            help='do not write the FPGA bitstream')

    @classmethod
    def do_create(cls, cfg, args):
        return Agrv32flashBinaryRunner(cfg, args)

    # ---- construction ------------------------------------------------

    def __init__(self, cfg, args):
        super().__init__(cfg)

        # Locate the agrv32flash binary. Lookup order:
        #   1. --agrv32flash on the command line
        #   2. $AGRV32FLASH environment variable
        #   3. PlatformIO default under $HOME/AgRV_pio/packages/...
        candidates = [
            args.agrv32flash,
            os.environ.get('AGRV32FLASH'),
            _expand_user(DEFAULT_AGRV32FLASH),
        ]
        for c in candidates:
            if c and path.isfile(c):
                self.agrv32flash = c
                break
        else:
            # Will be caught by require() in do_run()
            self.agrv32flash = _expand_user(DEFAULT_AGRV32FLASH)

        self.serial = args.serial
        self.baud = args.baud
        self.skip_bitstream = args.skip_bitstream
        self.bitstream_addr = args.bitstream_addr
        self.firmware_addr = args.firmware_addr

        # Resolve the bitstream path: explicit > board_dir/board.bin
        bs = args.bitstream
        if bs is None:
            default_bs = Path(cfg.board_dir) / 'board.bin'
            if default_bs.is_file():
                bs = str(default_bs)
        self.bitstream = bs

    # ---- do_run ------------------------------------------------------

    def do_run(self, command, **kwargs):
        try:
            self.require(self.agrv32flash)
        except MissingProgram:
            self.logger.error('agrv32flash not found at %s', self.agrv32flash)
            self.logger.error('  * install the AgRV PlatformIO SDK '
                              '(tool-agrv_flashloader), or')
            self.logger.error('  * export AGRV_SDK_PATH=/path/to/AgRV_pio, or')
            self.logger.error('  * pass --agrv32flash=/path/to/agrv32flash '
                              '(or set AGRV32FLASH), or')
            self.logger.error('  * use the SWD path instead: '
                              'west flash --runner agrv_openocd')
            raise

        # Common agrv32flash argument prefix: baud rate (8e1 default
        # matches the AgRV2K ROM bootloader protocol).
        common = ['-b', str(self.baud)]

        # ---- 1. (optional) FPGA bitstream --------------------------
        if not self.skip_bitstream and self.bitstream:
            self.logger.info(
                f'Writing bitstream {self.bitstream} -> {self.bitstream_addr}')
            self.check_call(
                [self.agrv32flash, *common,
                 '-l', self.bitstream, '-v',
                 self.serial])
        elif not self.skip_bitstream:
            self.logger.info(
                'No bitstream found (pass --bitstream or build first); '
                'skipping FPGA write.')

        # ---- 2. firmware image -------------------------------------
        bin_file = self.cfg.bin_file
        if not bin_file:
            self.logger.error(
                'No .bin file produced by the build. '
                'Check that CONFIG_BUILD_OUTPUT_BIN=y.')
            return

        self.logger.info(
            f'Writing firmware {bin_file} -> {self.firmware_addr}')
        self.check_call(
            [self.agrv32flash, *common,
             '-w', bin_file, '-v',
             self.serial])

        # ---- 3. start execution ------------------------------------
        self.logger.info(f'Starting execution at {self.firmware_addr}')
        self.check_call(
            [self.agrv32flash, *common,
             '-g', self.firmware_addr,
             self.serial])
