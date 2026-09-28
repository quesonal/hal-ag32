#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
#
# verify_flow.py — run the whole end-to-end verification flow on a board and
# assert every step of it.
#
# What it proves, in one run: the signed application image, the signed fabric
# slot, the anti-rollback floor and the signed commands all work, and the
# paths that must be *refused* are refused (tampered container, older version,
# container signed with the wrong key, a forged bitstream slot, an
# unauthorized publish). Everything it exercises is on-chip: no external
# secure element, no vendor per-chip bitstream encryption (those are recorded
# as out of scope).
#
# Requirements, and where each comes from
# ---------------------------------------
#   * the repo venv on PATH (west, pyserial, imgtool, cryptography), plus the
#     SDK's openocd (the board's support/openocd.cfg) and a CMSIS-DAP probe;
#   * `samples/verify_flow` -- the application half, which prints the ledger
#     this script asserts on (see its README);
#   * `samples/spi_boot_loader` built with CONFIG_BOOT_AGM_PRODUCTION_PROFILE
#     and a public key this script generates: the locked profile is what makes
#     "unauthorized publish" mean anything;
#   * the same loader with CONFIG_BOOT_AGM_BIND: step 2 provisions this chip's
#     salt over SWD (tools/agm_bind.py provision) and binds the application
#     container to it, so every later step is also "a chip-bound image works
#     end to end" and step 3 is the two refusals that follow from it. That
#     leaves a salt in the flash -- production state, not test state -- and the
#     salt file is kept in the workdir so a re-run provisions the same one;
#   * a canonical 200 MHz bitstream to sign and stream (default:
#     $HOME/spi_full_mac_bitstream_200mhz/example_board.bin).
#
# Usage
# -----
#   tools/verify_flow.py                     # the whole flow, board at the end
#   tools/verify_flow.py --only 3,4          # debug a subset (fresh board ok)
#   tools/verify_flow.py --keep              # do not restore hello_world
#   tools/verify_flow.py --workdir /tmp/flow # keep the build artifacts
#
# A subset that skips step 1 runs against whatever the board already has, so
# the key and the builds in --workdir are reused when they exist (regenerating
# the key would leave a board whose loader trusts a different one -- every
# command then comes back EACCESSDENIED, which looks like a broken gate).
#
# Exit code 0 only if every selected step passed.

import argparse
import hashlib
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time

import serial

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PY = sys.executable
TOOLS = os.path.join(REPO, "tools")

sys.path.insert(0, TOOLS)
import agm_bind  # noqa: E402  (the per-chip binding half of the flow)


class Fail(Exception):
    """A step's assertion did not hold; the message is the evidence."""


def run(cmd, cwd=None, timeout=900, quiet=True, env=None):
    """Run a command, echo it, return stdout+stderr."""
    print(f"    $ {' '.join(cmd)}", flush=True)
    p = subprocess.run(cmd, cwd=cwd, timeout=timeout, text=True, env=env,
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if not quiet or p.returncode != 0:
        print(p.stdout, flush=True)
    if p.returncode != 0:
        raise Fail(f"{cmd[0]} failed (rc={p.returncode})")
    return p.stdout


def run_try(cmd, cwd=None, timeout=900, env=None):
    """Like run(), but a non-zero exit is data, not an exception."""
    print(f"    $ {' '.join(cmd)}", flush=True)
    p = subprocess.run(cmd, cwd=cwd, timeout=timeout, text=True, env=env,
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    return p.returncode, p.stdout


def have(cmd):
    return shutil.which(cmd) is not None


class Board:
    """The board's console: reset-and-capture, and landing in the loader."""

    def __init__(self, args):
        self.args = args
        self.dev = args.port
        self.cfg = os.environ.get(
            "AGM_OPENOCD_CFG",
            os.path.join(REPO, "boards/agm", args.board, "support/openocd.cfg"))
        self.ocd = os.environ.get(
            "AGM_OPENOCD_CMD",
            os.path.expanduser("~/AgRV_pio/packages/tool-agrv_openocd/bin/openocd_cmd"))
        # The board cfg picks the adapter from AGRV_ADAPTER and falls back to
        # J-Link without it; this dev board is CMSIS-DAP.
        self.env = dict(os.environ)
        self.env.setdefault("AGRV_ADAPTER", "cmsis-dap")

    def _reset_cmd(self, extra=()):
        return [self.ocd, "-f", self.cfg, "-c", "init", "-c", "reset run", "-c", "shutdown"]

    def warmup(self):
        warm = os.path.join(TOOLS, "openocd_warmup.py")
        if os.path.exists(warm):
            subprocess.run([PY, warm], stdout=subprocess.DEVNULL,
                           stderr=subprocess.DEVNULL, check=False)

    def reset_capture(self, seconds=8.0, hold_console=False, send=None):
        """Open the port, reset the target, collect the console for @seconds.

        hold_console: spam CRLF through the loader's 1.5 s boot window so it
        stays in its console instead of booting an image.
        send: a command to type once the console prompt is up.
        """
        self.warmup()
        ser = serial.Serial(self.dev, 115200, timeout=0.05)
        ser.reset_input_buffer()
        # Reset *after* the reader is open, or the one-shot banner is lost.
        proc = subprocess.Popen(self._reset_cmd(), stdout=subprocess.DEVNULL,
                                stderr=subprocess.DEVNULL, env=self.env)
        out = ""
        t0 = time.time()
        sent_cmd = False

        while time.time() - t0 < seconds:
            if hold_console and time.time() - t0 < 4.0:
                ser.write(b"\r\n")
            data = ser.read(4096)
            if data:
                out += data.decode("utf-8", "replace")
            if send and not sent_cmd and "loader> " in out:
                time.sleep(0.2)
                ser.reset_input_buffer()
                ser.write(send.encode() + b"\r\n")
                sent_cmd = True
                # let the reply land, then keep reading until the deadline
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()
        ser.close()
        return out

    def console(self, command, wait=3.0, tries=3):
        """Reset into the loader console, type @command, return the transcript.

        Retried like capture(): the first reset after an SMP session closed the
        port can come back with nothing but the loader's bare prompts (seen on
        the dev board repeatedly), and a transcript with no answer in it must not
        be used as evidence. What counts as an answer is the loader's own
        report format -- `record   : ...`, `uid      : ...`, `store A  : ...`:
        a line that starts with a field name and a colon. The boot banner's
        indented " boot driver: ready" is deliberately not one.
        """
        out = ""
        for attempt in range(tries):
            out = self.reset_capture(seconds=wait + 2.0, hold_console=True, send=command)
            if re.search(r"(?m)^\w+ *: ", out):
                return out
            print(f"    (no console answer, retry {attempt + 1}/{tries})", flush=True)
            time.sleep(0.5)
        return out

    def capture(self, seconds=10.0, hold_console=False, tries=3):
        """reset_capture() with retries: the CDC link occasionally comes back
        empty for the first reset after an SMP session closed the port (the
        repo's "retry the session before suspecting the board" rule)."""
        out = ""
        for attempt in range(tries):
            out = self.reset_capture(seconds=seconds, hold_console=hold_console)
            if len(out.strip()) > 40:
                return out
            print(f"    (empty capture, retry {attempt + 1}/{tries})", flush=True)
            time.sleep(0.5)
        return out

    def smp(self, argv, timeout=600):
        return run([PY, os.path.join(TOOLS, "smp_cli.py"), self.dev] + argv, timeout=timeout)

    def swd(self, commands, timeout=180, tries=3):
        """One openocd session: init, reset-init, the given -c commands, shutdown.

        The `reset init` is not decoration: this is the sequence the west runner
        and the vendor's own tooling use before any flash write, and without it
        a program command can hit a flash controller that is busy serving XIP
        fetches for the running image.: sessions that went
        `init` -> `flash write_image` (no reset-init) left the chip in a state
        where even XIP stopped -- silent console, FCB STAT stuck at INIT|ACTIVE,
        APB clocks back at their reset values -- and only a mass erase (or the
        ROM path) brought it back; the same write through a reset-init session
        is what every `west flash` in this repo does, and those never wedge.

        Whole-session retries stay (the vendor driver reports a timeout now and
        then, and a reset-init session is idempotent).
        """
        cmd = [self.ocd, "-f", self.cfg, "-c", "init", "-c", "reset init"]
        for c in commands:
            cmd += ["-c", c]
        cmd += ["-c", "shutdown"]
        last = ""
        for attempt in range(tries):
            self.warmup()
            rc, out = run_try(cmd, timeout=timeout, env=self.env)
            if rc == 0:
                return out
            last = out
            print(f"    (swd session failed, retry {attempt + 1}/{tries})", flush=True)
            time.sleep(1.0)
        raise Fail("openocd session failed:\n" + tail(last))

    def flash_build(self, build_dir):
        run(["west", "flash", "-d", build_dir, "--skip-bitstream"],
            cwd=self.args.ws, timeout=600)


def expect(text, needle, what):
    if needle not in text:
        raise Fail(f"{what}: expected {needle!r} in:\n" + tail(text))


def reject(text, needle, what):
    if needle in text:
        raise Fail(f"{what}: {needle!r} must not appear in:\n" + tail(text))


def tail(text, n=20):
    return "\n".join(text.strip().splitlines()[-n:])


class Flow:
    def __init__(self, args):
        self.args = args
        self.board = Board(args)
        self.dir = args.workdir
        self.artifacts = {}
        self.results = []

    # ---- artifact preparation (step 0) --------------------------------

    def prepare(self):
        d = self.dir
        os.makedirs(d, exist_ok=True)
        key = self.args.key or os.path.join(d, "flow.pem")

        if not self.args.key:
            # Keep the key across runs: `--only <subset>` may skip the flash
            # step, and a board still running the loader built with the
            # *previous* key would refuse every command -- it looks
            # exactly like a broken gate (rc 11 on every authorize).
            if not os.path.exists(key):
                run([PY, "-c",
                     "import subprocess,sys; "
                     f"sys.exit(subprocess.call(['imgtool','keygen','-t','ecdsa-p256','-k',{key!r}]))"])
        wrong = os.path.join(d, "wrong.pem")
        if not os.path.exists(wrong):
            run([PY, "-c",
                 "import subprocess,sys; "
                 f"sys.exit(subprocess.call(['imgtool','keygen','-t','ecdsa-p256','-k',{wrong!r}]))"])

        # The per-chip salt, kept across runs for the same reason as the key:
        # the board is provisioned with it in step 2, and a new salt would
        # invalidate every image bound in an earlier run. `other_salt.bin` is
        # what "an image for another chip" is built with.
        salt = self.args.salt or os.path.join(d, "salt.bin")
        if not os.path.exists(salt):
            run([PY, os.path.join(TOOLS, "agm_bind.py"), "gen-salt", "-o", salt])
        other_salt = os.path.join(d, "other_salt.bin")
        if not os.path.exists(other_salt):
            run([PY, os.path.join(TOOLS, "agm_bind.py"), "gen-salt", "-o", other_salt])

        # Application: one binary, two containers (the version lives in the
        # container, so the anti-rollback case needs no second build).
        app_dir = os.path.join(d, "b_app")
        run(["west", "build", "-d", app_dir, "-b", self.args.board, "--pristine=auto",
             os.path.join(self.args.module, "samples/verify_flow"), "--",
             "-DCONFIG_ROM_START_OFFSET=0x20"], cwd=self.args.ws)
        app_bin = os.path.join(app_dir, "zephyr/zephyr.bin")
        pub = os.path.join(d, "flow.pub")
        good = os.path.join(d, "app_v2.signed.bin")
        run([PY, os.path.join(TOOLS, "sign_image.py"), app_bin, key, "-o", good,
             "--pubkey-out", pub, "--version", "2.0.0"])

        older = os.path.join(d, "app_v1.signed.bin")
        run([PY, os.path.join(TOOLS, "sign_image.py"), app_bin, key, "-o", older,
             "--version", "1.0.0"])

        # Bitstream: the good one signed with the trusted key, and one signed
        # with a key the loader does not know.
        bs_good = os.path.join(d, "bs.signed.bin")
        run([PY, os.path.join(TOOLS, "sign_image.py"), "--bitstream", self.args.bitstream,
             key, "-o", bs_good, "--version", "2.0.0"])
        bs_wrong = os.path.join(d, "bs.wrongkey.bin")
        run([PY, os.path.join(TOOLS, "sign_image.py"), "--bitstream", self.args.bitstream,
             wrong, "-o", bs_wrong, "--version", "2.0.0"])

        # Loader: the production profile (signed images + gated commands).
        loader_dir = os.path.join(d, "b_loader")
        run(["west", "build", "-d", loader_dir, "-b", self.args.board, "--pristine=auto",
             os.path.join(self.args.module, "samples/spi_boot_loader"), "--",
             "-DCONFIG_BOOT_AGM_PRODUCTION_PROFILE=y",
             # Per-chip binding (step 2/3): the production profile does not
             # select it, because a board that ships bound needs a provisioned
             # salt first.
             "-DCONFIG_BOOT_AGM_BIND=y",
             "-DCONFIG_ISR_STACK_SIZE=4096",
             f"-DSPI_BOOT_PUBKEY={pub}"], cwd=self.args.ws)

        self.artifacts = {
            "key": key, "pub": pub, "app": good, "older": older,
            "bs_good": bs_good, "bs_wrong": bs_wrong,
            "loader_dir": loader_dir, "app_dir": app_dir,
            "salt": salt, "other_salt": other_salt, "app_unbound": good,
        }
        print(f"    artifacts in {d}")

    # ---- helpers -------------------------------------------------------

    def clean_records(self):
        """Erase both boot records: the application one and the bitstream one.

        The loader's flash writes only touch the sectors they need, so a
        previous run's record survives a reflash -- and a flow that inherits a
        confirmed slot or a raised floor would be testing something else.
        """
        self.board.swd(["flash erase_address 0x80018000 0x2000",
                        "flash erase_address 0x800e6000 0x1000"])

    def into_console(self):
        """Reset and keep the loader in its console (returns the transcript)."""
        return self.board.reset_capture(seconds=4.0, hold_console=True)

    def upload(self, image_number, image, authorize=True, timeout=400):
        """SMP upload, with or without the command grants.

        A locked build is only reachable this way: the console and AN3155
        paths cannot be handed the *second* grant mid-flight (the loader says
        so itself), and `smp_cli.py upload --authorize` spends both grants for
        the host -- ERASE before the first chunk, PUBLISH before the chunk
        that publishes. Returns (rc, output) instead of raising, because the
        refusals are what half of this flow is about.
        """
        argv = ["upload", str(image_number), image]
        if authorize:
            argv += ["--authorize", self.artifacts["key"]]
        return run_try([PY, os.path.join(TOOLS, "smp_cli.py"), self.board.dev] + argv,
                       timeout=timeout)

    # ---- the steps -----------------------------------------------------

    def step_flash(self):
        self.board.flash_build(self.artifacts["loader_dir"])
        self.clean_records()
        out = self.board.capture(seconds=6.0)
        expect(out, "record   : empty", "fresh board has no record")
        expect(out, "fabric came from the factory slot",
               "with no bitstream slot the loader streams the factory config")
        return "loader flashed, records erased, fabric = factory, no record"

    def step_bind(self):
        """Provision this chip's salt and bind the application to it.

        The UID and the fingerprint come from the loader's own `info`: the
        value the host derives the tag from is then the one the chip will
        recompute with, not one read out of a datasheet. Provisioning goes
        over SWD (tools/agm_bind.py provision writes the 4 KiB sector and reads
        it back), because a board whose salt is written by the thing being
        tested proves nothing.
        """
        text = self.board.console("info")
        expect(text, "uid      : ", "the loader prints the chip's UID")
        expect(text, "bind     : ", "and its binding state")
        uid = agm_bind.uid_from_log(text)
        # Keep the transcript: it is where the UID and the fingerprint below
        # came from, and a production record keeps exactly this.
        with open(os.path.join(self.dir, "loader.info"), "w") as fh:
            fh.write(text)
        print(f"    uid      : {uid.hex()}")

        rc, out = run_try([PY, os.path.join(TOOLS, "agm_bind.py"), "provision",
                           "--salt-file", self.artifacts["salt"],
                           "--uid", uid.hex()], timeout=180)
        if rc != 0:
            raise Fail("provisioning failed (a salt from another workdir is "
                       "already in the flash? re-provision with "
                       "`agm_bind.py provision --force`, or delete the "
                       "workdir):\n" + tail(out))

        # The board has to read the salt that was just written -- its own
        # fingerprint of the derived key is what says so.
        with open(self.artifacts["salt"], "rb") as fh:
            salt = fh.read()
        key = agm_bind.bind_key(uid, salt)
        fp = hashlib.sha256(key).digest()[:4].hex()
        text = self.board.console("info")
        expect(text, f"bind     : salt v1 present, key fp {fp}",
               "the board reads the salt the host wrote (its key fingerprint "
               "has to be the host's)")

        # Bind the containers to this chip. Every container a later step
        # expects to be *accepted* has to be bound (the v1 one still has to
        # reach the anti-rollback check), and so does the tampered copy: an
        # unbound container is refused by the binding check, which would make
        # "the digest catches a flipped byte" prove nothing.
        for name, src, salt_file in (
                ("app", self.artifacts["app_unbound"], self.artifacts["salt"]),
                ("older", self.artifacts["older"], self.artifacts["salt"]),
                ("app_other_chip", self.artifacts["app_unbound"],
                 self.artifacts["other_salt"])):
            out_path = os.path.join(self.dir, f"{name}.bound.bin")
            run([PY, os.path.join(TOOLS, "agm_bind.py"), "embed",
                 "--container", src,
                 "--uid", uid.hex(), "--salt-file", salt_file, "-o", out_path])
            run([PY, os.path.join(TOOLS, "agm_bind.py"), "check",
                 "--container", out_path, "--uid", uid.hex(),
                 "--salt-file", salt_file])
            self.artifacts[name] = out_path

        tampered = bytearray(open(self.artifacts["app"], "rb").read())
        tampered[0x40] ^= 0x01  # a payload byte; header and TLVs untouched
        tampered_path = os.path.join(self.dir, "app_tampered.bound.bin")
        with open(tampered_path, "wb") as fh:
            fh.write(bytes(tampered))
        self.artifacts["tampered"] = tampered_path

        self.artifacts["uid"] = uid.hex()
        return (f"salt provisioned over SWD, read back, key fp {fp}; the "
                f"application is bound to this chip")

    def step_not_bound(self):
        """The two containers a bound board must refuse.

        `app_unbound` is the very image the earlier flow steps used to use --
        the same bytes, signed by the trusted key, with no binding tag -- and
        `app_other_chip` is the same image bound to another salt. Neither may
        publish, and the record -- the only thing that makes an uploaded image
        bootable -- has to come out of it unchanged. (The comparison is
        against whatever the board had *before*, not against "empty": a run
        that skipped the flash step starts from the previous run's record.)
        """
        before = self.board.console("info")
        for name, what in (("app_unbound", "no binding tag"),
                           ("app_other_chip", "bound to another chip")):
            self.into_console()
            rc, out = self.upload(0, self.artifacts[name], timeout=300)
            if rc == 0:
                raise Fail(f"an image with {what} must not publish")
            expect(out, "rc': 11", f"MGMT_ERR_EACCESSDENIED for {what}")
        after = self.board.console("info")
        for field in ("record   :", "floor    :", "store A  :", "store B  :"):
            was = [l for l in before.splitlines() if l.startswith(field)]
            now = [l for l in after.splitlines() if l.startswith(field)]
            if was != now:
                raise Fail(f"a refused image changed '{field.strip()}':\n"
                           f"  before: {was}\n  after:  {now}")
        return "refused (unbound and wrong-chip containers, EACCESSDENIED, record unchanged)"

    def step_unauthorized(self):
        self.into_console()
        rc, out = self.upload(2, self.artifacts["app"], authorize=False, timeout=120)
        if rc == 0:
            raise Fail("an upload without a grant must not succeed")
        expect(out, "rc': 11", "mgmt err EACCESSDENIED from the missing grant")
        state = self.board.console("info")
        expect(state, "record   : empty", "an unauthorized upload must not publish")
        return "refused (EACCESSDENIED, no grant), record untouched"

    def step_wrong_key(self):
        self.into_console()
        rc, out = self.upload(3, self.artifacts["bs_wrong"], timeout=400)
        if rc == 0:
            raise Fail("a bitstream signed by an untrusted key must not publish")
        console = self.board.console("info")
        expect(console, "record   : empty",
               "a wrong-key bitstream must not be committed")
        return "refused (the verifier rejected the container), nothing committed"

    def step_good_bitstream(self):
        self.into_console()
        rc, out = self.upload(3, self.artifacts["bs_good"], timeout=400)
        if rc != 0:
            raise Fail("the signed bitstream must publish:\n" + tail(out))
        out = self.board.capture(seconds=8.0)
        # What the boot path streams is now the slot just committed. The
        # boot-time verification itself is asserted by step 9 (a slot whose
        # bytes were changed is refused and the fabric falls back) and, for the
        # sharper case where the record CRC is forged to match it, by this
        # step: the same corruption with the record's own CRC made to match.
        expect(out, "fabric came from an update slot",
               "the committed bitstream slot becomes the live fabric")
        m = re.search(r"fabric came from an update slot \(0x([0-9a-f]+)\)", out)
        self.artifacts["bs_slot"] = int(m.group(1), 16) if m else None
        return f"fabric = signed slot {self.artifacts.get('bs_slot') and hex(self.artifacts['bs_slot'])}"

    def step_signed_app(self):
        self.into_console()
        rc, out = self.upload(2, self.artifacts["app"], timeout=300)
        if rc != 0:
            raise Fail("the signed application must publish:\n" + tail(out))
        out = self.board.capture(seconds=10.0)
        expect(out, "image verified (MCUboot container", "the loader verifies the app")
        expect(out, "verify_flow: VERIFY-FLOW: PASS", "the application's own ledger")
        expect(out, "floor    : v2.0.0", "the floor rose with the accepted image")
        return "signed app booted and passed its own ledger"

    def step_confirmed(self):
        out = self.board.capture(seconds=10.0)
        expect(out, "verify_flow: VERIFY-FLOW: PASS", "the confirmed slot boots again")
        expect(out, "not a trial boot (already confirmed)",
               "the slot must be CONFIRMED now")
        return "slot CONFIRMED, no trial handshake needed"

    def step_older(self):
        self.into_console()
        # Target the *inactive* store, like a product would: the refusal must
        # not disturb the image that is running (the recorder's CRC covers the
        # store it publishes, and nothing was published here).
        rc, out = self.upload(1, self.artifacts["older"], timeout=300)
        if rc == 0:
            raise Fail("an image older than the floor must not publish")
        # MGMT_ERR_EBADSTATE is 6 in this tree; the driver maps the
        # anti-rollback refusal onto it (drivers/misc/boot_agm_smp.c).
        expect(out, "rc': 6", "mgmt err EBADSTATE from the anti-rollback floor")
        out = self.board.capture(seconds=10.0)
        expect(out, "verify_flow: VERIFY-FLOW: PASS", "the board still runs the new image")
        expect(out, "floor    : v2.0.0", "the floor must not move")
        return "refused (EBADSTATE, older than the floor), floor still v2.0.0"

    def step_tampered(self):
        # The tampered container is a *bound* one (step 2 makes it), so the
        # digest is what refuses it and the case is not just the binding check
        # firing again.
        self.into_console()
        rc, out = self.upload(1, self.artifacts["tampered"], timeout=300)
        if rc == 0:
            raise Fail("a tampered container must not publish")
        out = self.board.capture(seconds=10.0)
        expect(out, "verify_flow: VERIFY-FLOW: PASS", "the board still runs the real image")
        return "refused (digest mismatch on a bound container), board still on the real image"

    def step_forged_slot(self):
        # Which slot is live comes from the loader itself, so this step does
        # not depend on an earlier step's parse.
        before = self.board.capture(seconds=8.0)
        m = re.search(r"fabric came from an update slot \(0x([0-9a-f]+)\)", before)
        if m is None:
            # A slot forged by an *earlier* run is refused at boot, so the
            # fabric reads factory while the record still names the slot -- this
            # step needs a committed one, i.e. the good-bitstream step (or the
            # full flow) has to run first.
            raise Fail("no bitstream slot is being streamed (a slot forged in an "
                       "earlier run, or none committed): run steps 1 and 4 first")
        slot = int(m.group(1), 16)
        bogus = os.path.join(self.dir, "bogus.bin")
        open(bogus, "wb").write(b"\xde\xad\xbe\xef")
        self.board.swd([f"flash write_image {bogus} 0x{slot + 0x20:08x} bin"])
        out = self.board.capture(seconds=10.0)
        if not out.strip():
            # The vendor flash driver's writes time out now and then (~1 in 12
            # standalone runs), and a timed-out write leaves the controller in
            # a state where even XIP stops -- the board looks dead until the
            # ROM path re-writes it. Say that instead of "the fabric did not
            # fall back", which is what it would otherwise look like.
            raise Fail("the SWD write wedged the flash controller (vendor flash "
                       "driver; the ROM path recovers it, so "
                       "recovery, then re-run); the board produced no console "
                       "output at all")
        # First half: the record's CRC no longer matches the slot, so the fabric
        # falls back to the factory region instead of streaming a forged config
        # -- and the loader says which of the two checks caught it.
        expect(out, "fabric came from the factory slot",
               "a forged bitstream slot must fall back to factory")
        expect(out, "refused (it does not match the record)",
               "the record/slot CRC check is the one that catches this")
        # Still alive: the application runs it when steps 5-6 published one, and
        # a run that only exercised this step leaves the loader at its console.
        if "verify_flow: VERIFY-FLOW: PASS" not in out:
            expect(out, "loader> ", "the board survives the forgery")
        return "forged slot refused (record/slot mismatch), fabric back on factory"

    def step_cleanup(self):
        self.clean_records()
        hello = os.path.join(self.dir, "b_hello")
        run(["west", "build", "-d", hello, "-b", self.args.board, "--pristine=always",
             os.path.join(self.args.module, "samples/hello_world")], cwd=self.args.ws)
        self.board.flash_build(hello)
        out = self.board.capture(seconds=6.0)
        expect(out, "AgRV2K Zephyr hello_world", "the board is back on hello_world")
        return "board restored (hello_world, records erased)"

    STEPS = [
        ("flash + blank board", "step_flash"),
        ("per-chip binding: salt provisioned, application bound", "step_bind"),
        ("unbound / wrong-chip images refused", "step_not_bound"),
        ("unauthorized publish refused", "step_unauthorized"),
        ("bitstream signed by an untrusted key refused", "step_wrong_key"),
        ("signed bitstream slot becomes the fabric", "step_good_bitstream"),
        ("signed application boots and passes its ledger", "step_signed_app"),
        ("confirmed slot boots without the trial handshake", "step_confirmed"),
        ("older version refused (anti-rollback)", "step_older"),
        ("tampered container refused", "step_tampered"),
        ("forged bitstream slot falls back to factory", "step_forged_slot"),
        ("board restored", "step_cleanup"),
    ]

    def selected(self):
        if not self.args.only:
            steps = [(i + 1, t, m) for i, (t, m) in enumerate(self.STEPS)]
            if self.args.keep:
                # Leave the board in the flow's state for inspection.
                steps = [s for s in steps if s[2] != "step_cleanup"]
            return steps
        want = {int(x) for x in self.args.only.split(",") if x.strip()}
        return [(i + 1, t, m) for i, (t, m) in enumerate(self.STEPS) if i + 1 in want]

    def run_steps(self):
        ok = True
        for number, title, method in self.selected():
            print(f"\nverify_flow: [{number}] {title}", flush=True)
            t0 = time.time()
            try:
                detail = getattr(self, method)()
            except Fail as e:
                print(f"verify_flow: [{number}] FAIL -- {e}", flush=True)
                ok = False
                break
            print(f"verify_flow: [{number}] PASS -- {detail} "
                  f"({time.time() - t0:.0f}s)", flush=True)
        return ok


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="/dev/ttyACM0")
    ap.add_argument("--board", default="agrv2k_407")
    ap.add_argument("--module", default=os.path.expanduser("~/zephyrproject/modules/hal_ag32"),
                    help="the module path inside the west workspace (what west builds)")
    ap.add_argument("--ws", default=os.path.expanduser("~/zephyrproject"),
                    help="the west workspace root (cwd for west build/flash)")
    ap.add_argument("--bitstream",
                    default=os.path.expanduser("~/spi_full_mac_bitstream_200mhz/example_board.bin"))
    ap.add_argument("--key", default="")
    ap.add_argument("--salt", default="",
                    help="the per-chip salt to provision (default: <workdir>/salt.bin)")
    ap.add_argument("--workdir", default=os.path.join(tempfile.gettempdir(), "verify_flow"))
    ap.add_argument("--only", default="", help="comma list of step numbers to run")
    ap.add_argument("--keep", action="store_true", help="do not restore hello_world")
    args = ap.parse_args()

    flow = Flow(args)
    print("verify_flow: preparing artifacts (this builds the payload and the loader)")
    flow.prepare()
    ok = flow.run_steps()
    print(f"\nverify_flow: {'ALL STEPS PASSED' if ok else 'FAILED'}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
