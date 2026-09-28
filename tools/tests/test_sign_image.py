# SPDX-License-Identifier: Apache-2.0
"""Tests for tools/sign_image.py -- the host side of the signed-image mode.

The invariants here are the ones the bootloader depends on: an MCUboot header
at the front, the payload at `slot + header size`, and a KEYHASH TLV that is the
SHA-256 of the public key encoding *that key type* uses -- the
SubjectPublicKeyInfo DER for ECDSA, the PKCS#1 DER for RSA (the loader rebuilds
that hash from the key blob it is built with; if this test ever fails, every
signed board starts refusing every image).
"""

import hashlib
import importlib.util
import importlib
import pathlib
import struct
import subprocess
import sys

import pytest

TOOLS = pathlib.Path(__file__).resolve().parents[1]


def load(name):
    spec = importlib.util.spec_from_file_location(name, TOOLS / f"{name}.py")
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


sign_image = load("sign_image")


@pytest.fixture(scope="module")
def keys(tmp_path_factory):
    """Two throwaway P-256 keys; the tests never touch a real one."""
    d = tmp_path_factory.mktemp("keys")
    out = {}
    for name in ("key1", "key2"):
        path = d / f"{name}.pem"
        sign_image.run_imgtool(["keygen", "-k", str(path), "-t", "ecdsa-p256"])
        out[name] = path
    return out


@pytest.fixture(scope="module")
def rsa_keys(tmp_path_factory):
    """Two throwaway RSA-2048 keys, for the RSA-2048-PSS profile."""
    d = tmp_path_factory.mktemp("rsa_keys")
    out = {}
    for name in ("key1", "key2"):
        path = d / f"{name}.pem"
        sign_image.run_imgtool(["keygen", "-k", str(path), "-t", "rsa-2048"])
        out[name] = path
    return out


def make_image(path, size=1024, first_byte=0xA5):
    data = bytearray((i * 7 + 3) & 0xFF for i in range(size))
    data[0] = first_byte
    path.write_bytes(bytes(data))
    return path


def sign(tmp_path, keys, image, **kwargs):
    out = tmp_path / "signed.bin"
    pub = tmp_path / "pub.bin"
    argv = [str(image), str(keys["key1"]), "-o", str(out),
            "--pubkey-out", str(pub)]
    for key, value in kwargs.items():
        argv += [f"--{key.replace('_', '-')}", str(value)]
    assert sign_image.main(argv) == 0
    return out.read_bytes(), pub.read_bytes()


def tlv(container, hdr):
    """Yield (type, data) for the TLV area of a container."""
    off = hdr["hdr_size"] + hdr["img_size"]
    off = (off + 7) & ~7  # 8-byte alignment, like MCUboot
    magic, total = struct.unpack_from("<HH", container, off)
    assert magic == 0x6907
    off += 4
    end = off - 4 + total
    while off < end:
        itype, ilen = struct.unpack_from("<HH", container, off)
        off += 4
        yield itype, container[off:off + ilen]
        off += ilen


def test_container_shape_and_payload_offset(tmp_path, keys):
    container, pub = sign(tmp_path, keys, make_image(tmp_path / "app.bin"))
    hdr = sign_image.read_header(container)

    assert hdr["magic"] == 0x96F3B83D
    assert hdr["hdr_size"] == 0x20
    assert hdr["img_size"] == 1024
    assert len(pub) == 64, "the loader takes the raw X||Y key"

    types = dict(tlv(container, hdr))
    assert 0x10 in types and len(types[0x10]) == 32, "SHA256 TLV"
    assert 0x22 in types, "signature TLV"


def test_keyhash_matches_the_emitted_public_key(tmp_path, keys):
    container, _ = sign(tmp_path, keys, make_image(tmp_path / "app.bin"))
    hdr = sign_image.read_header(container)
    keyhash = dict(tlv(container, hdr))[0x01]

    # What the loader recomputes: the P-256 SPKI prefix + 0x04 || X || Y.
    prefix = bytes.fromhex("3059301306072a8648ce3d020106082a8648ce3d030107034200")
    priv = __import__("cryptography.hazmat.primitives.serialization",
                      fromlist=["serialization"])
    key = priv.load_pem_private_key(keys["key1"].read_bytes(), password=None)
    nums = key.private_numbers().public_numbers
    raw = nums.x.to_bytes(32, "big") + nums.y.to_bytes(32, "big")

    assert hashlib.sha256(prefix + b"\x04" + raw).digest() == keyhash


def test_payload_gap_is_never_doubled(tmp_path, keys):
    """A payload built with CONFIG_ROM_START_OFFSET already starts with the gap;
    one that does not gets padded. Either way the payload lands at +0x20."""
    plain = make_image(tmp_path / "plain.bin")
    container, _ = sign(tmp_path, keys, plain)
    hdr = sign_image.read_header(container)
    assert container[hdr["hdr_size"]:hdr["hdr_size"] + 4] == plain.read_bytes()[:4]

    # An image built with CONFIG_ROM_START_OFFSET already carries the gap;
    # imgtool writes the header into it, so the code is at +0x20 here too and
    # img_size counts the code, not the gap.
    reserved = tmp_path / "reserved.bin"
    reserved.write_bytes(b"\x00" * 0x20 + plain.read_bytes())
    container2, _ = sign(tmp_path, keys, reserved)
    hdr2 = sign_image.read_header(container2)
    assert hdr2["img_size"] == len(plain.read_bytes())
    assert container2[hdr2["hdr_size"]:hdr2["hdr_size"] + 4] == plain.read_bytes()[:4]


def test_a_tampered_container_fails_the_self_check(tmp_path, keys):
    container, _ = sign(tmp_path, keys, make_image(tmp_path / "app.bin"))
    tampered = bytearray(container)
    tampered[0x40] ^= 0xFF
    path = tmp_path / "tampered.bin"
    path.write_bytes(bytes(tampered))

    # This is the same check the loader's verifier performs (wrong digest).
    hdr = sign_image.read_header(bytes(tampered))
    digest = hashlib.sha256(bytes(tampered[:hdr["hdr_size"] + hdr["img_size"]])).digest()
    assert digest != dict(tlv(bytes(tampered), hdr))[0x10]


def test_another_key_produces_a_different_keyhash(tmp_path, keys):
    image = make_image(tmp_path / "app.bin")
    out_a = tmp_path / "a.bin"
    out_b = tmp_path / "b.bin"
    for key, out in (("key1", out_a), ("key2", out_b)):
        assert sign_image.main([str(image), str(keys[key]), "-o", str(out)]) == 0

    ha = sign_image.read_header(out_a.read_bytes())
    hb = sign_image.read_header(out_b.read_bytes())
    assert (dict(tlv(out_a.read_bytes(), ha))[0x01] !=
            dict(tlv(out_b.read_bytes(), hb))[0x01])


def test_version_lands_in_the_header_for_anti_rollback(tmp_path, keys):
    """The loader's anti-rollback orders images by the container's `ih_ver`
    (major, minor, revision -- the build number is ignored, so a rebuild of the
    same release does not look newer), so `--version` has to land there."""
    image = make_image(tmp_path / "app.bin")
    out = tmp_path / "signed.bin"

    assert sign_image.main([str(image), str(keys["key1"]), "-o", str(out),
                            "--version", "3.4.5"]) == 0

    container = out.read_bytes()
    # image_version { major, minor, revision(u16 LE), build(u32 LE) } sits at
    # offset 20, right after ih_flags -- the loader parses the same layout.
    major, minor, rev_lo, rev_hi = container[20:24]
    assert (major, minor, (rev_hi << 8) | rev_lo) == (3, 4, 5)


def test_header_records_where_the_image_runs(tmp_path, keys):
    """The loader runs a container at `slot + header size` and refuses one whose
    header says otherwise, so this address has to be the one it was built for."""
    container, _ = sign(tmp_path, keys, make_image(tmp_path / "app.bin"))
    hdr = sign_image.read_header(container)

    assert hdr["load_addr"] == 0x8007c000 + hdr["hdr_size"]

    container, _ = sign(tmp_path, keys, make_image(tmp_path / "app.bin"),
                        slot_base="0x80020000")
    hdr = sign_image.read_header(container)
    assert hdr["load_addr"] == 0x80020000 + hdr["hdr_size"], \
        "another slot base has to move the recorded run address with it"


def test_a_contradicting_load_addr_is_refused(tmp_path, keys):
    """--load-addr only exists to make the check explicit: a value the header
    cannot agree with means the loader would refuse the container, so the tool
    has to refuse it first."""
    image = make_image(tmp_path / "app.bin")
    out = tmp_path / "signed.bin"

    with pytest.raises(SystemExit):
        sign_image.main([str(image), str(keys["key1"]), "-o", str(out),
                         "--load-addr", "0x80000000"])

    # ... and the matching value is accepted.
    assert sign_image.main([str(image), str(keys["key1"]), "-o", str(out),
                            "--load-addr", "0x8007c020"]) == 0


def test_slot_base_boundaries_are_refused(tmp_path, keys):
    """The address goes into a 32-bit header field and into the loader's entry
    check, so the tool has to refuse what cannot mean anything there -- before
    imgtool quietly wraps it."""
    image = make_image(tmp_path / "app.bin")
    out = tmp_path / "signed.bin"

    # Overflows only *after* the header is added: it looks like a plain
    # address until then.
    with pytest.raises(SystemExit):
        sign_image.main([str(image), str(keys["key1"]), "-o", str(out),
                         "--slot-base", "0xfffffff0"])

    for bad in ("-0x20", "0x100000000", "0x8007e002"):
        with pytest.raises(SystemExit):
            sign_image.main([str(image), str(keys["key1"]), "-o", str(out),
                             "--slot-base", bad])

    # The aligned, in-range neighbour is still fine.
    assert sign_image.main([str(image), str(keys["key1"]), "-o", str(out),
                            "--slot-base", "0x8007c000"]) == 0


# ---- the fabric (bitstream) mode ---------------------------------------
#
# A fabric container is the same container with a different stamp: the loader
# refuses anything whose header does not say AGM_BITSTREAM_CONTAINER_ADDR
# (slot 1 + header size, the one stamp both fabric slots share) or whose payload
# is not the whole 99944-byte fabric, so the tool has to produce exactly that --
# and refuse an input it cannot stamp.


def make_fabric(path, size=None):
    return make_image(path, size=size or sign_image.BITSTREAM_IMAGE_LEN)


def test_bitstream_mode_stamps_the_fabric_layout(tmp_path, keys):
    image = make_fabric(tmp_path / "fabric.bin")
    out = tmp_path / "bs.signed.bin"
    pub = tmp_path / "bs.pub"

    assert sign_image.main([str(image), str(keys["key1"]), "-o", str(out),
                            "--bitstream", "--pubkey-out", str(pub)]) == 0

    container = out.read_bytes()
    hdr = sign_image.read_header(container)

    assert hdr["magic"] == 0x96F3B83D
    assert hdr["hdr_size"] == 0x20
    assert hdr["img_size"] == sign_image.BITSTREAM_IMAGE_LEN
    assert hdr["load_addr"] == sign_image.BITSTREAM_SLOT_BASE + 0x20
    assert len(pub.read_bytes()) == 64, "the fabric keeps a raw X||Y key"
    # The payload is the fabric itself, byte for byte, behind the header ...
    assert container[0x20:0x20 + 64] == image.read_bytes()[:64]
    # ... the three TLVs the loader walks are there ...
    assert sorted(dict(tlv(container, hdr))) == [0x01, 0x10, 0x22]
    # ... and the container fits the 100 KiB slot it goes into.
    assert len(container) <= sign_image.BITSTREAM_SLOT_SIZE


def test_bitstream_mode_refuses_another_size(tmp_path, keys):
    out = tmp_path / "bs.signed.bin"

    for size in (sign_image.BITSTREAM_IMAGE_LEN - 4,
                 sign_image.BITSTREAM_IMAGE_LEN + 4):
        image = make_fabric(tmp_path / "fabric.bin", size=size)
        with pytest.raises(SystemExit):
            sign_image.main([str(image), str(keys["key1"]), "-o", str(out),
                             "--bitstream"])


def test_bitstream_mode_refuses_a_conflicting_stamp_or_slot(tmp_path, keys):
    image = make_fabric(tmp_path / "fabric.bin")
    out = tmp_path / "bs.signed.bin"

    for extra in (["--slot-base", "0x800cd000"], ["--slot-size", "0x80000"]):
        with pytest.raises(SystemExit):
            sign_image.main([str(image), str(keys["key1"]), "-o", str(out),
                             "--bitstream"] + extra)

    # The fabric's own values, spelled out, are accepted.
    assert sign_image.main([str(image), str(keys["key1"]), "-o", str(out),
                            "--bitstream", "--slot-base", "0x800b4000",
                            "--slot-size", "0x19000"]) == 0
    assert sign_image.read_header(out.read_bytes())["load_addr"] == 0x800B4020


# ---- the RSA-2048-PSS profile ------------------------------------------
#
# The same container, three different conventions: the signature TLV type, the
# encoding it carries, and the bytes the KEYHASH covers. The signed-images
# plan pins them; these are the tests that keep the tool, the fixtures
# and the loader's copy of the layout from drifting apart.


def test_rsa_container_uses_the_pss_tlv_and_a_pkcs1_keyhash(tmp_path, rsa_keys):
    container, pub = sign(tmp_path, rsa_keys, make_image(tmp_path / "app.bin"))
    hdr = sign_image.read_header(container)
    types = dict(tlv(container, hdr))

    assert hdr["magic"] == 0x96F3B83D
    assert len(pub) == 270, "the loader takes the PKCS#1 RSAPublicKey DER"
    assert len(types[0x10]) == 32, "SHA256 TLV"
    assert 0x20 in types, "IMAGE_TLV_RSA2048_PSS"
    assert 0x22 not in types, "the RSA profile has no ECDSA TLV"
    assert len(types[0x20]) == 256, "the raw signature, not DER"

    # The KEYHASH is the hash of the emitted blob *because* the blob is the
    # PKCS#1 DER -- and deliberately not the hash of the SPKI the ECDSA
    # profile would use for the same key.
    serialization = importlib.import_module(
        "cryptography.hazmat.primitives.serialization")
    key = serialization.load_pem_private_key(rsa_keys["key1"].read_bytes(),
                                             password=None)
    spki = key.public_key().public_bytes(
        encoding=serialization.Encoding.DER,
        format=serialization.PublicFormat.SubjectPublicKeyInfo)
    assert types[0x01] == hashlib.sha256(pub).digest()
    assert types[0x01] != hashlib.sha256(spki).digest()


def test_rsa_signature_verifies_against_the_public_key(tmp_path, rsa_keys):
    """Independent check that what we wrote really is an RSA-2048-PSS
    signature over the image hash: verify it here with `cryptography`, so a
    wrong PSS/MGF1/flags choice fails on the host, not on the dev board."""
    container, pub = sign(tmp_path, rsa_keys, make_image(tmp_path / "app.bin"),
                          slot_base="0x80030000")
    hdr = sign_image.read_header(container)
    signature = dict(tlv(container, hdr))[0x20]
    # cryptography's verify() hashes the data it is given, which is what
    # imgtool does when it signs; the loader hashes the same bytes itself and
    # hands psa_verify_hash() the digest. Both are the same signature over
    # SHA-256(header + image).
    signed_bytes = container[:hdr["hdr_size"] + hdr["img_size"]]

    padding = importlib.import_module(
        "cryptography.hazmat.primitives.asymmetric.padding")
    hashes = importlib.import_module(
        "cryptography.hazmat.primitives.hashes")
    serialization = importlib.import_module(
        "cryptography.hazmat.primitives.serialization")
    public = serialization.load_der_public_key(pub)
    public.verify(signature, signed_bytes,
                  padding.PSS(mgf=padding.MGF1(hashes.SHA256()),
                              salt_length=hashes.SHA256().digest_size),
                  hashes.SHA256())

    assert public.public_numbers().e == 65537
    assert hashlib.sha256(pub).digest() == dict(tlv(container, hdr))[0x01]


def test_rsa_another_key_produces_a_different_keyhash(tmp_path, rsa_keys):
    image = make_image(tmp_path / "app.bin")
    out_a = tmp_path / "a.bin"
    out_b = tmp_path / "b.bin"
    for key, out in (("key1", out_a), ("key2", out_b)):
        assert sign_image.main([str(image), str(rsa_keys[key]), "-o", str(out)]) == 0

    ha = sign_image.read_header(out_a.read_bytes())
    hb = sign_image.read_header(out_b.read_bytes())
    assert (dict(tlv(out_a.read_bytes(), ha))[0x01] !=
            dict(tlv(out_b.read_bytes(), hb))[0x01])
