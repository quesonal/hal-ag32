# Copyright (c) 2026 AgRV Contributors
# SPDX-License-Identifier: Apache-2.0

'''AgRV-openocd runner — openocd with on-board CMSIS-DAP probe warmup.

Subclass of upstream ``OpenOcdBinaryRunner`` that runs a one-shot
pyusb warmup before invoking openocd, bypassing the AgRV2K
on-board probe FW 2.1.0 "slow string response" bug.

Why this is needed
------------------
The on-board CMSIS-DAP probe on agrv2k_407 (VID=cafe, PID=1001,
FW 2.1.0) does not respond to ``GET_DESCRIPTOR(STRING)`` within
OpenOCD's hard-coded libusb timeout of 1000 ms once it has been
used at least once. After the first successful openocd session,
all subsequent invocations time out on the string read and
openocd bails with::

    Warn : could not read product string for device 0xcafe:0x1001:
          Operation timed out
    Error: unable to find a matching CMSIS-DAP device

The fix is one slow control transfer via pyusb (5 s timeout)
BEFORE openocd starts. After the warmup the probe is responsive
enough that openocd's 1 s libusb window can read its strings.

No physical re-plug is needed. Verified with 3 consecutive
invocations on $HOME/agm_example.elf (wrote 8192 bytes
+ verified 6828 bytes each time, end-to-end).

Usage
-----
Same CLI as ``west flash --runner openocd``::

    west flash --runner agrv_openocd

All ``openocd_*`` flags accepted by the upstream runner are
forwarded unchanged.

Note: this runner only adds the warmup. The AgRV-patched openocd
binary (with the ``agrv`` flash driver) and the agrv2k.cfg setup
remain configured via ``board.cmake`` + ``support/openocd.cfg``
exactly as for the upstream runner.

Why we override ``do_create``
-----------------------------
Upstream ``OpenOcdBinaryRunner.do_create`` hard-codes
``OpenOcdBinaryRunner(cfg, ...)`` instead of ``cls(cfg, ...)``.
That silently bypasses any subclass __init__ / do_run overrides.
We replicate the do_create logic verbatim but use ``cls`` so
our methods actually run.
'''

import logging
import os
import subprocess
import sys
from pathlib import Path

from zephyr_ext_common import ZEPHYR_BASE  # noqa: F401  (kept for parity)

from runners.core import MissingProgram
from runners.openocd import OpenOcdBinaryRunner, FileType

_logger = logging.getLogger('runners.agrv_openocd')


def _sdk_root():
    '''AgRV PlatformIO SDK root: $AGRV_SDK_PATH, else ~/AgRV_pio.'''
    return os.environ.get('AGRV_SDK_PATH') or os.path.expanduser('~/AgRV_pio')


def _detect_sdk_openocd():
    '''Path to the AgRV-patched openocd, or None.

    Order: $AGRV_OPENOCD, then the SDK default
    (<AGRV_SDK_PATH>/packages/tool-agrv_openocd/bin/openocd_cmd).
    '''
    for cand in (os.environ.get('AGRV_OPENOCD'),
                 os.path.join(_sdk_root(),
                              'packages/tool-agrv_openocd/bin/openocd_cmd')):
        if cand and os.path.isfile(cand):
            return cand
    return None


def _find_warmup_script():
    """Locate openocd_warmup.py in the hal_ag32 tools directory.

    Search order:
      1. ``$ZEPHYR_HAL_AGM_HOME/tools/openocd_warmup.py``
      2. ``<module>/tools/openocd_warmup.py`` relative to this file
         (works for both the hal_ag32 source tree and the west-managed
         modules/hal_ag32 checkout).
    """
    env_path = os.environ.get("ZEPHYR_HAL_AGM_HOME")
    if env_path:
        p = Path(env_path) / "tools" / "openocd_warmup.py"
        if p.exists():
            return str(p)

    # west imports this file from its real module path, so ``__file__``
    # is <module>/scripts/west_commands/runners/agrv_openocd.py -- the
    # module root (and hence tools/) is 3 levels up. Older checkouts
    # nested the runner under a ``zephyr/`` prefix (module root 4 levels
    # up); keep that as a fallback.
    here = Path(__file__).resolve()
    candidates = [
        here.parents[3] / "tools" / "openocd_warmup.py",
        here.parents[4] / "tools" / "openocd_warmup.py",
    ]
    for c in candidates:
        if c.exists():
            return str(c)

    return None


class AgrvOpenOcdBinaryRunner(OpenOcdBinaryRunner):
    """OpenOcdBinaryRunner + pre-flash CMSIS-DAP probe warmup."""

    # ---- ZephyrBinaryRunner hooks ------------------------------------

    @classmethod
    def name(cls):
        return "agrv_openocd"

    @staticmethod
    def _check_bitstream_clock(cfg, bs_path):
        """Refuse a bitstream the fabric would clock at the wrong rate.

        Both sides of the comparison are files that already exist: the
        bitstream's own `.ve`/`.v` (what it was compiled for) and this build's
        `zephyr/.config` + `zephyr.dts` (what the firmware believes). The tool
        that compares them lives in the module's `tools/`, next to the rest of
        the dev board tooling; AGM_SKIP_CLOCK_CHECK=1 skips the gate for one run.
        """
        if os.environ.get('AGM_SKIP_CLOCK_CHECK') == '1':
            _logger.warning('AGM: bitstream clock check skipped '
                            '(AGM_SKIP_CLOCK_CHECK=1)')
            return

        module = os.path.dirname(os.path.dirname(os.path.dirname(
            os.path.dirname(os.path.abspath(__file__)))))
        tool = os.path.join(module, 'tools', 'check_bitstream_clock.py')
        if not os.path.isfile(tool):
            _logger.warning('AGM: %s is missing; the bitstream clock is '
                            'unverified', tool)
            return

        cmd = [sys.executable, tool, bs_path,
               '--config', os.path.join(cfg.build_dir, 'zephyr', '.config'),
               '--dts', os.path.join(cfg.build_dir, 'zephyr', 'zephyr.dts')]
        proc = subprocess.run(cmd, stdout=subprocess.PIPE,
                              stderr=subprocess.PIPE, text=True)
        for line in (proc.stdout or '').strip().splitlines():
            _logger.info('AGM: %s', line)

        if proc.returncode == 1:
            # A stated mismatch: the one outcome that must stop the flash.
            raise RuntimeError(
                'AGM: refusing to write %s:\n%s' % (bs_path,
                                                    (proc.stderr or '').strip()))
        if proc.returncode != 0:
            # Unverifiable (no .ve/.v next to the bitstream, an unreadable
            # build config, ...). Warn rather than block: the operator may be
            # flashing a bitstream from somewhere this tree cannot see.
            _logger.warning('AGM: bitstream clock could not be checked '
                            '(rc=%d): %s', proc.returncode,
                            (proc.stderr or '').strip())

    @classmethod
    def do_add_parser(cls, parser):
        # Inherit openocd's CLI args (--cmd-load, --cmd-pre-load,
        # --file-type, --flash-address, ...).
        OpenOcdBinaryRunner.do_add_parser(parser)
        # Opt out from the FPGA bitstream write. Mirror the
        # agrv32flash runner's flags. The board.cmake sets
        # --skip-bitstream by default; user overrides with
        # --write-bitstream to opt back in.
        # --skip-bitstream and --bitstream-only are mutually exclusive:
        # one means "firmware only", the other means "bitstream only",
        # and there is no sane "neither" interpretation.
        bs_group = parser.add_mutually_exclusive_group()
        bs_group.add_argument(
            '--skip-bitstream', action='store_true',
            help='do not write the FPGA bitstream to its FLASH region '
                 '(sector-erase preserves existing bitstream).')
        bs_group.add_argument(
            '--bitstream-only', action='store_true',
            help='write ONLY the FPGA bitstream -- firmware region is '
                 'untouched. Goes through the same openocd session as '
                 'a regular flash, but skips erase/load/verify on the '
                 'firmware region. Useful when the bitstream changed '
                 'and the firmware did not.')
        parser.add_argument(
            '--write-bitstream', action='store_true',
            help='write the FPGA bitstream even when board.cmake '
                 'would default to --skip-bitstream. (Compatibility '
                 'flag; the new default already writes the bitstream.)')
        # Plan A: board.cmake resolves the bitstream path
        # ($AGM_BITSTREAM_BIN / -DAGM_BITSTREAM_BIN / build dir) and bakes
        # it into --cmd-post-verify. Hand the runner the same value so the
        # missing-file check below can never disagree with the command
        # openocd is actually given -- without this, a bitstream living
        # outside the build dir was reported as missing and the flash was
        # silently downgraded to firmware-only.
        parser.add_argument(
            '--agm-bitstream-bin', default=None,
            help='FPGA bitstream path as resolved by board.cmake '
                 '(AGM_BITSTREAM_BIN). Used for the missing-file check; '
                 'falls back to $AGM_BITSTREAM_BIN and then '
                 '<build_dir>/zephyr/board.bin.')

    @classmethod
    def do_create(cls, cfg, args):
        # Mirror upstream OpenOcdBinaryRunner.do_create verbatim, but
        # use ``cls`` so __init__ / do_run overrides actually run.
        if args.use_image_type:
            _logger.warning('--use-hex/--use-elf/--use-bin are deprecated, '
                            'use --file-type instead')
            if cfg.file_type == FileType.OTHER or cfg.file_type is None:
                type_map = {'hex': FileType.HEX, 'elf': FileType.ELF, 'bin': FileType.BIN}
                image_type = type_map.get(args.use_image_type)
            else:
                image_type = cfg.file_type
        else:
            image_type = cfg.file_type

        return cls(
            cfg,
            pre_init=args.cmd_pre_init, pre_init_flash=args.cmd_pre_init_flash,
            reset_halt_cmd=args.cmd_reset_halt,
            pre_load=args.cmd_pre_load, erase_cmd=args.cmd_erase, load_cmd=args.cmd_load,
            verify_cmd=args.cmd_verify, post_verify=args.cmd_post_verify,
            do_verify=args.verify, do_verify_only=args.verify_only, do_erase=args.erase,
            tui=args.tui, config=args.config, serial=args.dev_id,
            image_type=image_type,
            flash_address=args.flash_address, no_halt=args.no_halt, no_init=args.no_init,
            no_targets=args.no_targets, tcl_port=args.telnet_port,
            telnet_port=args.telnet_port, log_file=args.log_file, gdb_port=args.gdb_port,
            gdb_client_port=args.gdb_client_port, gdb_init=args.gdb_init,
            load=args.load, target_handle=args.target_handle,
            rtt_port=args.rtt_port, rtt_server=args.rtt_server,
            gdb_pre_debug=args.gdb_pre_debug,
            skip_bitstream=args.skip_bitstream,
            write_bitstream=args.write_bitstream,
            bitstream_only=args.bitstream_only,
            bitstream_bin=args.agm_bitstream_bin)

    def __init__(self, cfg, pre_init=None, pre_init_flash=None,
                 reset_halt_cmd=None,
                 pre_load=None, erase_cmd=None, load_cmd=None,
                 verify_cmd=None, post_verify=None, do_verify=False,
                 do_verify_only=False, do_erase=False, tui=False, config=None,
                 serial=None, image_type=FileType.HEX, flash_address=None,
                 no_halt=False, no_init=False, no_targets=False, tcl_port=None,
                 telnet_port=None, log_file=None, gdb_port=None,
                 gdb_client_port=None, gdb_init=None, load=None,
                 target_handle=None, rtt_port=None, rtt_server=None,
                 gdb_pre_debug=None,
                 skip_bitstream=False, write_bitstream=False,
                 bitstream_only=False, bitstream_bin=None):
        # Plan A: resolve the three orthogonal flash modes:
        #
        #   default         -> write firmware + bitstream
        #   --skip-bitstream -> write firmware only (sector erase)
        #   --bitstream-only  -> write bitstream only (firmware region untouched)
        #   --write-bitstream -> redundant under the new default
        #
        # argparse has already enforced that --skip-bitstream and
        # --bitstream-only are mutually exclusive.
        if skip_bitstream and write_bitstream:
            _logger.warning(
                '--skip-bitstream and --write-bitstream both passed; '
                '--skip-bitstream wins (--write-bitstream is redundant '
                'under the new default).')

        # Default / --bitstream-only / --write-bitstream all write the
        # bitstream; only --skip-bitstream skips it.
        should_write_bitstream = not skip_bitstream
        if skip_bitstream and write_bitstream:
            should_write_bitstream = False

        # --bitstream-only: skip firmware region entirely. We use
        # do_verify_only=True (upstream OpenOcdBinaryRunner skips
        # load_image when this is set), force erase_cmd/verify_cmd to
        # None, and set load_cmd to a placeholder so BIN validation
        # passes without ever being executed.
        if bitstream_only:
            erase_cmd = None
            verify_cmd = None
            do_erase = False
            do_verify = False
            do_verify_only = True
            # load_cmd must be non-None for BIN validation, but it
            # will never run because do_verify_only skips load_image.
            load_cmd = load_cmd or "echo"

        # Plan A: bitstream missing -> warn + degrade (but the right way
        # for each mode).
        #
        # Resolution order: $AGM_BITSTREAM_BIN (a live override -- west
        # flash rebuilds but does not re-run cmake, so an env var set for
        # this invocation has to be honoured here, not only at configure
        # time) > the path board.cmake baked into --agm-bitstream-bin and
        # --cmd-post-verify > ${CMAKE_BINARY_DIR}/zephyr/board.bin.
        # Keeping the check and the write on the same path matters: the
        # check gates the "degrade to firmware-only" fallback.
        if should_write_bitstream:
            bs_path = (os.environ.get('AGM_BITSTREAM_BIN')
                       or bitstream_bin
                       or os.path.join(cfg.build_dir, 'zephyr', 'board.bin'))
            post_verify = self._retarget_bitstream(post_verify, bs_path)
            if not os.path.isfile(bs_path):
                if bitstream_only:
                    # --bitstream-only has nothing to write at all if the
                    # bitstream file is missing -- error out instead of
                    # silently upgrading to firmware-only (which is what
                    # --skip-bitstream means).
                    raise RuntimeError(
                        'AGM: --bitstream-only but %s not found. Run '
                        '"west build -t bitstream" to generate it '
                        'first, or drop --bitstream-only.' % bs_path)
                _logger.warning(
                    'AGM: %s not found; this west flash writes firmware '
                    'only (bitstream region survives via sector-erase). '
                    'Run "west build -t bitstream" to generate the '
                    'bitstream, then retry west flash for the full '
                    'write.', bs_path)
                should_write_bitstream = False
            else:
                # FCB_AUTO_WORDS * 4 -- the byte count agrv2k_fcb_program()
                # streams into FCB->AUTO. A Supra image in any other form
                # starts with the same IDCODE/USERID words, so the file
                # cannot be told apart by looking at its head; the size is
                # what gives it away. The SDK's `board_logic.compress` and
                # `board_logic.encrypt` both produce a smaller image, and
                # this driver streams raw words only (soc/agm/agrv2k/fcb.c):
                # writing one would configure the fabric with garbage and
                # take the CPU's clock down with it -- recovery is BOOT0 +
                # agrv32flash, not another west flash. See

                bs_size = os.path.getsize(bs_path)
                allow_any = os.environ.get('AGM_BITSTREAM_ANY_SIZE') == '1'
                if bs_size != 99944 and not allow_any:
                    raise RuntimeError(
                        'AGM: %s is %d bytes; an uncompressed Supra bitstream '
                        'is exactly 99944 bytes (FCB_AUTO_WORDS = 24986 '
                        'words). A smaller image is compressed or encrypted '
                        '(the SDK\'s board_logic.compress / board_logic.'
                        'encrypt, or the vendor boot\'s embedded logic): that '
                        'needs CONFIG_AGM_FCB_BITSTREAM_COMPRESSED=y on the '
                        'device -- and for a compressed one the factory slot '
                        'moves to where the flashing tool put the config '
                        '(AGM_FCB_BITSTREAM_ALGO_SIZE, 0x1100 with the SDK\'s '
                        'algorithm). Without that '
                        'option this '
                        'writes a bitstream the loader streams raw, which '
                        'leaves the board with a dead fabric and needs BOOT0 '
                        'to recover. Build an uncompressed bitstream, or set '
                        'AGM_BITSTREAM_ANY_SIZE=1 to write this one anyway.'
                        % (bs_path, bs_size))

                # ... and the *clock*. The bitstream states the rate the
                # fabric was compiled for (its `.ve`, or the PLL parameters in
                # the generated Verilog); this firmware's rate is a build-time
                # constant. If they disagree, every k_busy_wait(), every tick
                # and every UART divider in the image scales by
                # declared/actual, with no on-chip symptom at all (
                # carries its own clock rate). Refuse a mismatch; a bitstream
                # with nothing to compare against only warns.
                self._check_bitstream_clock(cfg, bs_path)

        # If skipping bitstream, drop the post_verify list (which
        # carries the FPGA bitstream write command) before super()
        # stores it.
        if not should_write_bitstream and post_verify:
            _logger.info(
                'AGM: bitstream write skipped (%d post_verify command(s) '
                'dropped; existing bitstream in FLASH preserved).',
                len(post_verify))
            post_verify = []

        if bitstream_only:
            _logger.info(
                'AGM: --bitstream-only; firmware region untouched, '
                'post_verify (bitstream write) still runs.')

        super().__init__(
            cfg, pre_init=pre_init, pre_init_flash=pre_init_flash,
            reset_halt_cmd=reset_halt_cmd, pre_load=pre_load, erase_cmd=erase_cmd,
            load_cmd=load_cmd, verify_cmd=verify_cmd, post_verify=post_verify,
            do_verify=do_verify, do_verify_only=do_verify_only, do_erase=do_erase,
            tui=tui, config=config, serial=serial, image_type=image_type,
            flash_address=flash_address, no_halt=no_halt, no_init=no_init,
            no_targets=no_targets, tcl_port=tcl_port, telnet_port=telnet_port,
            log_file=log_file, gdb_port=gdb_port, gdb_client_port=gdb_client_port,
            gdb_init=gdb_init, load=load, target_handle=target_handle,
            rtt_port=rtt_port, rtt_server=rtt_server, gdb_pre_debug=gdb_pre_debug)

        # Upstream builds self.openocd_cmd from --openocd / $OPENOCD and
        # falls back to the bare name 'openocd'. Fill that fallback from
        # the AgRV SDK (the patched build carries the `agrv` flash
        # driver), so `west flash --runner agrv_openocd` works without
        # PATH surgery. An explicit --openocd or $OPENOCD still wins.
        if self.openocd_cmd and self.openocd_cmd[0] == 'openocd':
            sdk_openocd = _detect_sdk_openocd()
            if sdk_openocd:
                _logger.info('AGM: using AgRV openocd %s', sdk_openocd)
                self.openocd_cmd[0] = sdk_openocd

    @staticmethod
    def _retarget_bitstream(post_verify, bs_path):
        """Point board.cmake's bitstream write command at ``bs_path``.

        ``--cmd-post-verify`` carries exactly one command, generated by
        board_common.cmake as::

            flash write_image erase <bitstream.bin> <AGM_BITSTREAM_ADDR>

        (the firmware write is ``--cmd-load``, not post_verify). Rewriting
        the path lets ``AGM_BITSTREAM_BIN=<path> west flash ...`` take
        effect on a build dir configured with a different path.
        """
        if not post_verify:
            return post_verify
        out = []
        for cmd in post_verify:
            parts = cmd.split()
            if (len(parts) >= 5 and parts[0] == 'flash'
                    and parts[1] == 'write_image' and parts[2] == 'erase'
                    and parts[3] != bs_path):
                _logger.info('AGM: bitstream write retargeted: %s -> %s',
                             parts[3], bs_path)
                parts[3] = bs_path
                cmd = ' '.join(parts)
            out.append(cmd)
        return out

    # ---- The actual run ----------------------------------------------

    def do_run(self, command, **kwargs):
        """Run the pyusb warmup, then invoke openocd via the parent."""
        try:
            self.require(self.openocd_cmd[0])
        except MissingProgram:
            self.logger.error('AgRV-patched openocd not found at %s',
                              self.openocd_cmd[0])
            self.logger.error('  * install the AgRV PlatformIO SDK '
                              '(tool-agrv_openocd), or')
            self.logger.error('  * export AGRV_SDK_PATH=/path/to/AgRV_pio, or')
            self.logger.error('  * pass --openocd=/path/to/openocd_cmd '
                              '(or set OPENOCD), or')
            self.logger.error('  * use the UART ROM-bootloader path instead: '
                              'west flash --runner agrv32flash')
            raise

        # The board's support/openocd.cfg picks its interface from
        # $AGRV_ADAPTER, and the SDK cfg it sources defaults to J-Link --
        # which fails with "No J-Link device found" on the eval boards. The
        # on-board probe is CMSIS-DAP, so default to that; an explicit
        # AGRV_ADAPTER in the environment still wins (AGRV_ADAPTER=jlink ...).
        os.environ.setdefault('AGRV_ADAPTER', 'cmsis-dap')

        self._run_warmup()
        return super().do_run(command, **kwargs)

    # ---- Internal helpers --------------------------------------------

    def _run_warmup(self):
        script = _find_warmup_script()
        if script is None:
            self.logger.warning(
                "AGM: openocd_warmup.py not found in hal_ag32/tools/. "
                "Set ZEPHYR_HAL_AGM_HOME or restore the file. "
                "Probe may flake without warmup.")
            return

        # Find a python that has pyusb. Prefer the venv the user
        # activated for the build (ZEPHYR_PYTHON or sys.executable).
        py = os.environ.get("ZEPHYR_PYTHON") or os.environ.get("PYTHON") or "python3"
        self.logger.info(f"AGM: running CMSIS-DAP probe warmup via {script}")
        try:
            subprocess.run([py, script], check=True)
        except subprocess.CalledProcessError as e:
            raise RuntimeError(
                f"AGM: probe warmup failed (exit {e.returncode}). "
                "Is the probe plugged in? Do you have pyusb installed?") from e
        except FileNotFoundError as e:
            raise RuntimeError(
                f"AGM: cannot run warmup ({e}). Check pyusb is installed.") from e
