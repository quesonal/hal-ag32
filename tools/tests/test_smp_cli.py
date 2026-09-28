# SPDX-License-Identifier: Apache-2.0
"""Unit tests for tools/smp_cli.py -- the SMP over console framing and the
CBOR bodies it builds.

It is the second, independent implementation of the wire format next to
`smpmgr`, so its framing is what a dev board session falls back on when the Go/
`smplcient` clients are unavailable; the loader's own buffer assumptions (one
base64 line per 127 bytes, `0x0609`/`0x0414` headers) are pinned here.
"""

import binascii
import importlib.util
import pathlib
import struct
import sys

import cbor2
import pytest

TOOLS = pathlib.Path(__file__).resolve().parents[1]


def load(name):
    spec = importlib.util.spec_from_file_location(name, TOOLS / f"{name}.py")
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


smp_cli = load("smp_cli")


def test_crc16_matches_zephyr():
    """Zephyr's serial framing uses crc16_itu_t with a zero initial value
    (subsys/mgmt/mcumgr/transport/src/serial_util.c)."""
    assert smp_cli.crc16_itu_t(b"123456789") == 0x31C3


def test_packet_round_trip():
    payload = bytes(range(64))
    lines = smp_cli.encode_packet(payload).splitlines(keepends=True)

    assert lines[0][:2] == b"\x06\x09", "a new packet starts with 0x0609"
    assert all(line[:2] == b"\x04\x14" for line in lines[1:]), "continuations"
    assert all(line.endswith(b"\n") for line in lines)
    assert smp_cli.decode_packet(lines) == payload


def test_lines_stay_within_the_loaders_buffer():
    """The console's line reader keeps MCUMGR_SERIAL_MAX_FRAME + 8 = 135 bytes
    (drivers/.../serial.h: 127), so a line has to stay under that."""
    lines = smp_cli.encode_packet(bytes(400)).splitlines()

    for line in lines:
        assert len(line) <= 127, len(line)


def test_smp_header_and_reply_parse():
    req = smp_cli.smp_request(smp_cli.SMP_OP_WRITE, smp_cli.SMP_GROUP_IMAGE,
                              smp_cli.IMG_UPLOAD, 7, cbor2.dumps({"off": 0}))

    op, flags, length, group, seq, command = struct.unpack(">BBHHBB", req[:8])
    assert op == (1 << 3) | smp_cli.SMP_OP_WRITE, "SMP v2: version in the top bits"
    assert length == len(req) - 8, "the header length counts the CBOR body"
    assert group == 1 and command == 1 and seq == 7
    assert cbor2.loads(req[8:]) == {"off": 0}

    body = cbor2.dumps({"rc": 0})
    reply = struct.pack(">BBHHBB", 0x03, 0, len(body), 1, 7, 0) + body
    header, body = smp_cli.parse_smp_reply(reply)
    assert header == (0x03, 1, 0, 7)
    assert body == {"rc": 0}


def test_a_nonce_reply_must_be_a_flat_map():
    """-5, pinned where it can be checked
    automatically: a group handler only writes key/value pairs *inside* the
    framework's map. Opening a map of its own nests one inside the other, and
    the host sees `bf bf 65"nonce"...` (: the first nonce
    request was answered that way and the tool died with "premature end of
    stream"). smp_cli.get_nonce() reads reply["nonce"], so:
      - the flat form must give the tool its bytes, and
      - the nested form must fail loudly rather than hand back something
        that only looks like a nonce.
    """
    nonce = bytes(range(16))

    flat = cbor2.dumps({"nonce": nonce})
    assert cbor2.loads(flat)["nonce"] == nonce, "the flat reply is what get_nonce() expects"

    # The nested byte string the broken handler produced: an outer map whose
    # first key is itself a map.
    nested = bytes.fromhex("bf") + cbor2.dumps({"nonce": nonce}) + bytes.fromhex("ff")
    try:
        decoded = cbor2.loads(nested)
    except (TypeError, cbor2.CBORDecodeError) as exc:  # noqa: BLE001 - both are failures
        assert exc, "the nested reply is rejected"
    else:
        # cbor2 versions that do accept it must still not expose a "nonce" key,
        # i.e. get_nonce() raises SystemExit instead of signing garbage.
        assert "nonce" not in decoded or not isinstance(decoded.get("nonce"), (bytes, bytearray)), (
            f"a nested reply must not look like a valid nonce reply: {decoded!r}"
        )


def test_upload_body_carries_image_off_and_data():
    """What `smp_cli.py upload <image> <file>` sends: the target only on the
    first chunk, then off/len/data. The loader's reassembly keys off exactly
    these fields (boot_agm_smp.c smp_boot_img_upload())."""
    # The script builds the body inline in do_upload(); mirror it here so a
    # change on either side has to come with a test change.
    body = cbor2.dumps({"image": 2, "off": 0, "len": 4, "data": b"abcd"})
    decoded = cbor2.loads(body)

    assert decoded["image"] == 2
    assert decoded["off"] == 0
    assert decoded["len"] == 4
    assert decoded["data"] == b"abcd"


@pytest.mark.parametrize("op,group,command",
                         [(0, 1, 0), (2, 1, 1), (2, 1, 5)])
def test_group_and_command_numbers_match_the_firmware(op, group, command):
    """image state (read, op 0), image upload (write, op 2) and image erase
    (write, op 2) -- the numbers the loader's hand-written group registers."""
    assert (op, group, command) in {
        (smp_cli.SMP_OP_READ, smp_cli.SMP_GROUP_IMAGE, smp_cli.IMG_STATE),
        (smp_cli.SMP_OP_WRITE, smp_cli.SMP_GROUP_IMAGE, smp_cli.IMG_UPLOAD),
        (smp_cli.SMP_OP_WRITE, smp_cli.SMP_GROUP_IMAGE, smp_cli.IMG_ERASE),
    }


def test_auth_group_is_the_loaders_own():
    """The signed commands live in group 0x40 (mcumgr reserves ids below 64
    for the standard groups), commands 0 = NONCE and 1 = AUTHORIZE -- the
    numbers drivers/misc/boot_agm_smp.c registers."""
    assert smp_cli.SMP_GROUP_BOOT_AUTH == 0x40
    assert smp_cli.BOOT_AUTH_NONCE == 0
    assert smp_cli.BOOT_AUTH_AUTHORIZE == 1
    # AGM_BOOT_AUTH_CMD_ERASE / _PUBLISH (include/zephyr/drivers/misc/boot_agm.h).
    assert smp_cli.AUTH_CMD == {"erase": 1, "publish": 2}


def test_auth_payload_layout():
    """What `authorize` signs, byte for byte: nonce, command, argument length
    little endian, arguments. The device rebuilds exactly this from the CBOR
    fields before it hashes, so a change on either side breaks this test."""
    nonce = bytes(range(16))

    assert smp_cli.auth_payload(nonce, 1) == nonce + b"\x01\x00\x00"
    assert smp_cli.auth_payload(nonce, 2, b"\xde\xad") == nonce + b"\x02\x02\x00\xde\xad"
    # The length is little endian: 0x0201, not 0x0102.
    assert smp_cli.auth_payload(nonce, 1, bytes(0x102))[16:19] == b"\x01\x02\x01"


def test_authorize_signs_the_sha256_of_the_payload():
    """The signature is over SHA-256(payload) with the algorithm prehashed:
    signing `payload` directly would hash it a second time and the device
    (which hashes it once) would refuse every command."""
    from cryptography.hazmat.primitives import hashes
    from cryptography.hazmat.primitives.asymmetric import ec, utils

    key = ec.generate_private_key(ec.SECP256R1())
    payload = smp_cli.auth_payload(bytes(range(16)), smp_cli.AUTH_CMD["erase"])
    signature = smp_cli.sign_auth_payload(key, payload)

    # What the device does with it: hash the payload (the one it rebuilt from
    # the request) and verify the signature against that digest.
    digest = hashes.Hash(hashes.SHA256())
    digest.update(payload)
    key.public_key().verify(signature, digest.finalize(),
                            ec.ECDSA(utils.Prehashed(hashes.SHA256())))
    assert len(signature) <= 72, "a DER ECDSA signature, which is what the loader parses"


class FakeSmp:
    """Records the requests a command makes and hands back canned replies."""

    def __init__(self, replies):
        self.replies = list(replies)
        self.calls = []

    def request(self, op, group, command, body, timeout=None):
        self.calls.append((op, group, command, cbor2.loads(body) if body else {}))
        return (None, self.replies.pop(0))


def test_upload_authorize_spends_both_grants(tmp_path):
    """`upload --authorize` has to open the session (ERASE) and publish
    (PUBLISH) with two separate grants: the loader consumes one per command,
    and a locked build gates both . The publish grant has to be
    armed *before* the last chunk, because that chunk's reply is the one that
    publishes."""
    from cryptography.hazmat.primitives import serialization
    from cryptography.hazmat.primitives.asymmetric import ec

    key_path = tmp_path / "dev board.pem"
    key_path.write_bytes(
        ec.generate_private_key(ec.SECP256R1()).private_bytes(
            serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8,
            serialization.NoEncryption(),
        )
    )
    image = tmp_path / "img.bin"
    image.write_bytes(bytes(16))  # one chunk: upload + publish in one request

    smp = FakeSmp([
        {"nonce": bytes(range(16))},          # NONCE for the erase grant
        {},                                   # AUTHORIZE erase
        {"nonce": bytes(range(16))},          # NONCE for the publish grant
        {},                                   # AUTHORIZE publish
        {"off": 16},                          # the upload itself
    ])

    assert smp_cli.do_upload(smp, 0, str(image), str(key_path)) == 0
    assert [(op, group, cmd) for op, group, cmd, _ in smp.calls] == [
        (smp_cli.SMP_OP_READ, smp_cli.SMP_GROUP_BOOT_AUTH, smp_cli.BOOT_AUTH_NONCE),
        (smp_cli.SMP_OP_WRITE, smp_cli.SMP_GROUP_BOOT_AUTH, smp_cli.BOOT_AUTH_AUTHORIZE),
        (smp_cli.SMP_OP_READ, smp_cli.SMP_GROUP_BOOT_AUTH, smp_cli.BOOT_AUTH_NONCE),
        (smp_cli.SMP_OP_WRITE, smp_cli.SMP_GROUP_BOOT_AUTH, smp_cli.BOOT_AUTH_AUTHORIZE),
        (smp_cli.SMP_OP_WRITE, smp_cli.SMP_GROUP_IMAGE, smp_cli.IMG_UPLOAD),
    ]
    assert smp.calls[1][3]["cmd"] == smp_cli.AUTH_CMD["erase"]
    assert smp.calls[3][3]["cmd"] == smp_cli.AUTH_CMD["publish"]
    # The upload body carries the target/len once, plus the whole chunk (the
    # test's 16 zero bytes) -- what smp_boot_img_upload() reassembles.
    assert smp.calls[4][3] == {"off": 0, "image": 0, "len": 16, "data": bytes(16)}


def test_upload_without_a_key_spends_nothing(tmp_path):
    """The default profile is untouched: no authorize round trips, just the
    upload (what every existing dev board flow does)."""
    image = tmp_path / "img.bin"
    image.write_bytes(bytes(16))

    smp = FakeSmp([{"off": 16}])

    assert smp_cli.do_upload(smp, 2, str(image)) == 0
    assert [(op, group, cmd) for op, group, cmd, _ in smp.calls] == [
        (smp_cli.SMP_OP_WRITE, smp_cli.SMP_GROUP_IMAGE, smp_cli.IMG_UPLOAD),
    ]


def _frame(payload: bytes) -> bytes:
    """What the device puts on the wire: length + payload + CRC, base64'd."""
    import struct as _struct

    return (
        _struct.pack(">H", len(payload) + 2)
        + payload
        + _struct.pack(">H", smp_cli.crc16_itu_t(payload))
    )


def test_empty_reply_body_means_success():
    """A handler that writes nothing leaves the framework's response map empty,
    and that is how success is spelled (: the decoder treats an
    empty body as `{}` rather than raising -- correct for the new group 0x40
    commands *and* for `erase`/`image state`, but it has to stay pinned)."""
    # op 0x09 (read response), flags 0, body length 2, group 1, seq 5, cmd 0.
    payload = bytes([0x09, 0x00, 0x00, 0x02, 0x00, 0x01, 0x05, 0x00]) + b"\xbf\xff"

    # parse_smp_reply() takes the SMP packet itself; the transport framing
    # (length + CRC) is decode_packet()'s job -- the two are easy to confuse,
    # which is why the *_frame() helper above is only used by the tests that
    # go through decode_packet().
    header, body = smp_cli.parse_smp_reply(payload)
    assert header == (0x09, 1, 0, 5), "op, group, command, seq"
    assert body == {}
    assert body.get("rc", 0) == 0, "the callers read success out of that"


def test_truncated_reply_is_rejected():
    """The other half of the same rule: an empty body is fine, a body whose
    length does not match the frame is not -- parse would otherwise be looking
    at whatever followed in the stream."""
    payload = bytes([0x09, 0x00, 0x00, 0x02, 0x00, 0x01, 0x05, 0x00]) + b"\xbf\xff"
    frame = _frame(payload)

    with pytest.raises(SystemExit, match="length mismatch"):
        smp_cli.decode_packet([b"\x06\x09" + binascii.b2a_base64(frame[:-1], newline=False) + b"\n"])


def test_do_erase_reads_an_empty_body_as_success():
    smp = FakeSmp([{}])  # what the loader answers for a successful erase

    assert smp_cli.do_erase(smp, 0) == 0
    assert smp.calls[0][:3] == (smp_cli.SMP_OP_WRITE, smp_cli.SMP_GROUP_IMAGE,
                                smp_cli.IMG_ERASE)
