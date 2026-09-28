# SPDX-License-Identifier: Apache-2.0
"""Tests for tools/agm_bind.py — the per-chip binding KDF and tag.

The device and the host have to agree on these two functions byte for byte;
everything else about binding (where the salt lives, when the loader checks
the tag) can change without breaking a fielded device, but a change here
invalidates every bound image. The vectors below are HKDF-SHA256 from RFC 5869
test case 1 (so the construction itself is pinned against the standard, not
just against itself) plus one end-to-end vector for our own info string.
"""

import hashlib
import importlib.util
import pathlib
import struct
import sys
import zlib

TOOLS = pathlib.Path(__file__).resolve().parents[1]


def load(name):
    spec = importlib.util.spec_from_file_location(name, TOOLS / f"{name}.py")
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


bind = load("agm_bind")


def test_hkdf_matches_rfc5869_case_1():
    """RFC 5869 A.1: IKM 0x0b*22, salt 0x000102..0c, info 0xf0f1..f9, L=42."""
    ikm = b"\x0b" * 22
    salt = bytes(range(0x00, 0x0D))
    info = bytes(range(0xF0, 0xFA))
    okm = bind.hkdf_sha256(ikm, salt, info, 42)

    assert okm.hex() == ("3cb25f25faacd57a90434f64d0362f2a"
                         "2d2d0a90cf1a5a4c5db02d56ecc4c5bf"
                         "34007208d5b887185865")
    assert okm[:32].hex() == "3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf"
    assert okm[32:].hex() == "34007208d5b887185865"


def test_key_is_stable_and_uses_both_halves():
    uid = bytes.fromhex("4433221188776655ccbbaa9900ffeedd")
    salt_a = bytes(range(16))
    salt_b = bytes(range(1, 17))
    k1 = bind.bind_key(uid, salt_a)

    assert k1 == bind.bind_key(uid, salt_a), "same inputs, same key"
    assert k1 != bind.bind_key(uid, salt_b), "the salt has to matter"
    # ... and so does the UID: without this the key would be per-batch.
    uid2 = bytearray(uid)
    uid2[5] ^= 0x01
    assert k1 != bind.bind_key(bytes(uid2), salt_a), "the UID has to matter"


def container(image_len=64, version=(0, 0, 0, 0)):
    """A minimal MCUboot container: header(32) + image + a TLV area stub."""
    img = bytes(range(image_len))
    head = (struct.pack("<IIHHI", 0x96F3B83D, 0x8007E020, 0x20, 0, image_len)
            + b"\x00" * 4                       # ih_flags
            + struct.pack("<BBHI", *version)    # ih_ver: major, minor, rev, build
            + b"\x00" * 4)                      # pin
    assert len(head) == bind.MCUBOOT_HDR_SIZE
    # ... and a TLV area: the info header (magic, size), 4 bytes, then the
    # padding MCUboot aligns the following image with.
    return head + img + struct.pack("<HH", bind.MCUBOOT_TLV_INFO_MAGIC, 4) + b"\x00" * 12


def test_key_and_tag_lengths_and_sizes():
    uid = bytes(16)
    salt = bytes(16)
    key = bind.bind_key(uid, salt)

    assert len(key) == bind.KEY_LEN
    assert len(bind.bind_tag(key, container())) == 32
    for bad in (bytes(15), bytes(17), b""):
        try:
            bind.bind_key(bad, salt)
        except ValueError:
            pass
        else:
            raise AssertionError("a UID of the wrong length has to be refused")
        try:
            bind.bind_key(uid, bad)
        except ValueError:
            pass
        else:
            raise AssertionError("a salt of the wrong length has to be refused")


def test_tag_covers_image_and_version():
    uid = bytes.fromhex("4433221188776655ccbbaa9900ffeedd")
    salt = bytes(range(16))
    key = bind.bind_key(uid, salt)

    c = container()
    assert bind.bind_tag(key, c) == bind.bind_tag(key, c)
    assert bind.bind_tag(key, c) != bind.bind_tag(key, container(65))
    # The version comes from the container's own header, so re-signing for a
    # new release moves the tag without anyone having to remember a flag.
    v2 = container(version=(2, 0, 0, 0))
    assert bind.version_bytes(c) == b"\x00" * 8
    assert bind.version_bytes(v2) == struct.pack("<BBHI", 2, 0, 0, 0)
    assert bind.bind_tag(key, c) != bind.bind_tag(key, v2)
    # Even the build number counts: the tag is over the header as it is.
    assert (bind.bind_tag(key, container(version=(2, 0, 0, 0)))
            != bind.bind_tag(key, container(version=(2, 0, 0, 1))))
    # The tag must not move when the *TLV area* changes: it is defined over
    # header+image so the tag itself can live in that area.
    c2 = bytearray(c)
    c2[-1] ^= 0xFF
    assert bind.bind_tag(key, c) == bind.bind_tag(key, bytes(c2))
    assert bind.bind_tag(key, c) == bind.bind_tag(key, bind.attach_bind(c, bytes(32)))


def test_known_answer_vectors():
    """The vectors the device-side C implementation is pinned to.

    UID and salt are the dev board values (the 00..0f test
    salt) and the container is a *committed fixture* -- the same one the native
    suite (tests/drivers/misc/boot_agm_bind) verifies, so the two
    implementations are pinned to the same bytes rather than to two parallel
    copies of the same recipe.
    """
    uid = bytes.fromhex("415034363334311200d6b83656060178")
    salt = bytes(range(16))
    key = bind.bind_key(uid, salt)
    signed_ok = (TOOLS.parent / "tests/drivers/misc/boot_agm_ecdsa/fixtures"
                 / "signed_ok.bin").read_bytes()

    assert key.hex() == ("d386ff9a773d41f230d3693f64e014c1"
                         "992d4b517cb384c5098050f9771df2e0")
    assert bind.version_bytes(signed_ok) == struct.pack("<BBHI", 1, 0, 0, 0)
    assert bind.bind_tag(key, signed_ok).hex() == (
        "cc1ffd58c83471f0ffa5aeb314999676"
        "4e225d0a6fc4c0ce67e5628461911c20")
    assert hashlib.sha256(key).digest()[:4].hex() == "f7b6f8a8"


def test_salt_sector_layout():
    salt = bytes(range(16))
    sector = bind.salt_sector(salt)

    assert len(sector) == bind.SALT_SECTOR_LEN
    magic, version = struct.unpack_from("<II", sector, 0)
    assert (magic, version) == (bind.SALT_SECTOR_MAGIC, bind.SALT_SECTOR_VERSION)
    assert sector[8:24] == salt
    assert struct.unpack_from("<I", sector, 24)[0] == zlib.crc32(sector[:24]) & 0xFFFFFFFF
    assert sector[28:] == b"\xff" * (bind.SALT_SECTOR_LEN - 28)


def test_uid_from_words_is_little_endian():
    """The device prints the UID as four hex words; this is how they fold back.

    The vector is the dev board's own UID: words 0x36345041 0x12313433
    0x36b8d600 0x78010656.
    """
    words = (0x36345041, 0x12313433, 0x36B8D600, 0x78010656)
    uid = bind.uid_from_words(words)

    assert uid.hex() == "41503436" + "33343112" + "00d6b836" + "56060178"
    assert uid[:4] == b"AP46"  # the ASCII run the vendor's own ID starts with


def test_uid_from_log_reads_the_loader_console():
    log = ("loader> info\n"
           "record   : empty (nothing installed yet)\n"
           "uid      : 41503436 33343112 00d6b836 56060178\n"
           "loader> ")
    assert bind.uid_from_log(log).hex() == "415034363334311200d6b83656060178"

    # A board that could not read it prints a placeholder: that has to be an
    # error, not a UID of zeros.
    for bad in ("uid      : <read failed>\n", "no uid here\n",
                "uid      : 41503436 33343112\n"):
        try:
            bind.uid_from_log(bad)
        except ValueError:
            pass
        else:
            raise AssertionError(f"{bad!r} has to be refused")


def test_attach_bind_appends_one_tlv_and_rewrites_it_in_place():
    tag = bytes(range(32))
    c = container()
    bound = bind.attach_bind(c, tag)
    body, total = bind.tlv_area(c)

    # The entry goes at the end of the TLV area and it_tlv_tot grows by it.
    assert len(bound) == len(c) + 4 + bind.BIND_LEN
    assert struct.unpack_from("<H", bound, body + 2)[0] == total + 4 + bind.BIND_LEN
    assert bind.container_bind(bound) == tag
    assert bind.container_bind(c) is None
    # The signed half is untouched -- that is why the signature still holds.
    assert bound[:body] == c[:body]
    assert bound[body + 4 + 4 + bind.BIND_LEN:] == c[body + 4:]

    # A container that ends with its TLV area (what imgtool produces) grows by
    # exactly the entry, i.e. the common case is a plain append.
    tight = container()[:body + 4]
    b2 = bind.attach_bind(tight, tag)
    assert len(b2) == len(tight) + 4 + bind.BIND_LEN
    # Only the TLV area's size field changes (and the entry that follows it).
    assert b2[:body + 2] == tight[:body + 2]
    assert b2[body + 4:] == struct.pack("<HH", bind.TLV_BIND, bind.BIND_LEN) + tag
    assert bind.container_bind(b2) == tag

    # Rewriting is idempotent in place: no second TLV, no growth.
    again = bind.attach_bind(bound, bytes(reversed(tag)))
    assert len(again) == len(bound)
    assert bind.container_bind(again) == bytes(reversed(tag))
    assert [t for _, t, _ in bind.iter_tlvs(again)].count(bind.TLV_BIND) == 1


def test_attach_bind_refuses_a_container_without_a_tlv_area():
    head_and_image = container()[:96]
    for bad in (head_and_image, b"\x00" * 8):
        try:
            bind.attach_bind(bad, bytes(32))
        except ValueError:
            pass
        else:
            raise AssertionError("a container with no TLV area has to be refused")


def test_inspect_salt_sector_states():
    salt = bytes(range(16))

    assert bind.inspect_salt_sector(b"\xff" * bind.SALT_SECTOR_LEN) == {
        "state": "blank", "salt": None, "detail": "erased"}
    assert bind.inspect_salt_sector(b"\x00" * bind.SALT_SECTOR_LEN)["state"] == "blank"

    good = bind.inspect_salt_sector(bind.salt_sector(salt))
    assert (good["state"], good["salt"]) == ("valid", salt)

    # Every way a sector can be wrong is "invalid", never "blank": a tool that
    # treated a corrupt sector as empty would overwrite it without asking.
    wrong_magic = bytearray(bind.salt_sector(salt))
    wrong_magic[0] ^= 0xFF
    wrong_version = bytearray(bind.salt_sector(salt))
    wrong_version[4] = 9
    bad_crc = bytearray(bind.salt_sector(salt))
    bad_crc[8] ^= 0x01
    for data, why in ((wrong_magic, "magic"), (wrong_version, "version"),
                      (bad_crc, "CRC")):
        got = bind.inspect_salt_sector(bytes(data))
        assert got["state"] == "invalid", why
        assert got["salt"] is None
        assert why in got["detail"]


def test_the_committed_fixtures_are_what_the_tool_produces():
    """signed_bound.bin etc. are generated by this tool -- keep them honest.

    They are checked in so the native suite needs no tooling at build time;
    this is what stops them from drifting away from the recipe in
    the ecdsa fixture tree.
    """
    fix = TOOLS.parent / "tests/drivers/misc/boot_agm_ecdsa/fixtures"
    uid = bytes.fromhex("415034363334311200d6b83656060178")
    salt = bytes(range(16))
    key = bind.bind_key(uid, salt)
    other = bind.bind_key(uid, bytes(s ^ 0xFF for s in salt))
    signed_ok = (fix / "signed_ok.bin").read_bytes()
    signed_v2 = (fix / "signed_v2.bin").read_bytes()

    assert (fix / "signed_bound.bin").read_bytes() == \
        bind.attach_bind(signed_ok, bind.bind_tag(key, signed_ok))
    assert (fix / "signed_bound_other_chip.bin").read_bytes() == \
        bind.attach_bind(signed_ok, bind.bind_tag(other, signed_ok))
    # 2.0.0 carrying 1.0.0's tag: the "re-signed but not re-bound" mistake.
    assert (fix / "signed_v2_stale_bind.bin").read_bytes() == \
        bind.attach_bind(signed_v2, bind.bind_tag(key, signed_ok))
    assert (fix / "bind_salt_sector.bin").read_bytes() == bind.salt_sector(salt)

    # And the tags inside them are the ones this tool computes.
    assert bind.container_bind((fix / "signed_bound.bin").read_bytes()) == \
        bind.bind_tag(key, signed_ok)
