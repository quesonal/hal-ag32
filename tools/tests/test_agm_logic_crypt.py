# SPDX-License-Identifier: Apache-2.0
"""Tests for tools/agm_logic_crypt.py -- the vendor's per-chip bitstream cipher.

The invariants pinned here are the ones a device-side re-seal (or an offline
unlock of an encrypted factory region) depends on: the transform matches the
vendor binary byte for byte, it is its own inverse, and the key material comes
out of the SPI NOR unique ID exactly the way `agrv32_get_flash_key()` builds it.

The three known-answer vectors were captured from the vendor's own
`encrypt_encode()`; if one of these ever fails, the port and
the vendor toolchain have drifted apart and anything sealed by this tool is
worthless on a real chip.
"""

import importlib.util
import pathlib
import subprocess
import sys

TOOLS = pathlib.Path(__file__).resolve().parents[1]


def load(name):
    spec = importlib.util.spec_from_file_location(name, TOOLS / f"{name}.py")
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


crypt_tool = load("agm_logic_crypt")

# 0x4B returns these bytes; the four little-endian words are
# 11223344 55667788 99aabbcc ddeeff00.
UID = bytes.fromhex("4433221188776655ccbbaa9900ffeedd")
FLASH_KEY = [0x444444CC, 0xCCCCCC44, 0x444444CC, 0x99AABBCC]

# (flash key, length, ciphertext) straight from the vendor binary.
KAT = [
    (FLASH_KEY, 32,
     "664799aadc51b875ede1711d752624819125014b26d04d70c65043493bffa249"),
    ([0xA1B2C3D4, 0x5E6F7081, 0x92A3B4C5, 0xD6E7F809], 65,
     "c98878bb423d4084202ea4a7b7809b4bb886e57751f9de968b4ad3453a404a25"
     "b62e1fede20e1629e9e9de31a44dd890929a4cef6639dea0c2f9737ebd316511e9"),
    ([0x11111111, 0x22222222, 0x33333333, 0x44444444], 128,
     "3b9e5f51e2dc79e4e9bbf70f4909fadf3e0f3fbb47631ff34e67913e6011feba"
     "70a242a05b0ab2966c4fd9aa99f956af633fca2cf68d734305db328028057ae1"
     "43c70a0b1e8497ea693a6c934c8dd3b8efc84babec43ac478ac2b900b698ba07"
     "1b31910944d5d5726b4e92309938f5c6411a1c76886eefc0bca068b4599be289"),
]


def pattern(n):
    """The plaintext the vendor vectors were produced from."""
    return bytes(((i * 7 + 3) & 0xFF) for i in range(n))


def test_flash_key_comes_from_the_uid():
    assert crypt_tool.uid_to_flash_key(UID) == FLASH_KEY


def test_the_uid_is_exactly_sixteen_bytes():
    try:
        crypt_tool.uid_to_flash_key(UID[:8])
    except ValueError as exc:
        assert "16 bytes" in str(exc)
    else:
        raise AssertionError("a short UID must be refused")


def test_known_answer_vectors_from_the_vendor_binary():
    for key, length, want in KAT:
        got = crypt_tool.crypt(pattern(length), key)
        assert got.hex() == want, "len=%d" % length


def test_the_transform_is_its_own_inverse():
    for key, length, want in KAT:
        sealed = bytes.fromhex(want)
        assert crypt_tool.crypt(sealed, key, decrypt=True) == pattern(length)


def test_flash_key_word_two_is_a_dead_store():
    """The vendor code loads `keys[2]` into the state and then overwrites it --
    in the host tool and in the device-side blob alike. Two key sets that
    differ only there must produce identical ciphertext."""
    other = list(FLASH_KEY)
    other[2] ^= 0xFFFFFFFF
    assert crypt_tool.crypt(pattern(64), FLASH_KEY) == crypt_tool.crypt(pattern(64), other)


def test_every_uid_word_reaches_the_key_stream():
    """The *combination* u2^u3 is dropped, but all four ID words still matter:
    u2 and u3 also land in flash_key[1] and flash_key[3]."""
    base = crypt_tool.crypt(pattern(64), crypt_tool.uid_to_flash_key(UID))
    for word in range(4):
        changed = bytearray(UID)
        changed[4 * word:4 * word + 4] = b"\x01\x02\x03\x04"
        assert crypt_tool.crypt(pattern(64), crypt_tool.uid_to_flash_key(bytes(changed))) != base, \
            "UID word %d" % word


def test_bitstream_helper_leaves_the_idcode_words_alone():
    """The vendor encrypts a config from +8 (IDCODE/USERID stay raw)."""
    image = bytes(((i * 5 + 1) & 0xFF) for i in range(300))
    sealed = crypt_tool.crypt_bitstream(image, UID)
    assert sealed[:8] == image[:8]
    assert sealed[8:] != image[8:]
    assert crypt_tool.crypt_bitstream(sealed, UID, offset=8, decrypt=True) == image


def test_bitstream_helper_only_touches_the_requested_window():
    image = bytes(range(256))
    sealed = crypt_tool.crypt_bitstream(image, UID, offset=16, length=32)
    assert sealed[:16] == image[:16]
    assert sealed[48:] == image[48:]
    assert sealed[16:48] == crypt_tool.crypt(image[16:48], FLASH_KEY)


def test_selftest_and_cli(tmp_path):
    script = TOOLS / "agm_logic_crypt.py"
    assert subprocess.run([sys.executable, str(script), "selftest"],
                          capture_output=True, text=True).returncode == 0

    got = subprocess.run([sys.executable, str(script), "keys", "--uid", UID.hex()],
                         capture_output=True, text=True, check=True).stdout
    assert got.strip() == "flash_key: " + " ".join("%08x" % w for w in FLASH_KEY)

    # A round trip through the CLI, and the documented reference vector.
    image = tmp_path / "config.bin"
    image.write_bytes(pattern(32))
    sealed = tmp_path / "sealed.bin"
    subprocess.run([sys.executable, str(script), "encrypt", str(image),
                    "-o", str(sealed), "--uid", UID.hex(), "--offset", "0"],
                   check=True, capture_output=True)
    assert sealed.read_bytes().hex() == KAT[0][2]

    back = tmp_path / "back.bin"
    subprocess.run([sys.executable, str(script), "decrypt", str(sealed),
                    "-o", str(back), "--uid", UID.hex(), "--offset", "0"],
                   check=True, capture_output=True)
    assert back.read_bytes() == image.read_bytes()


def test_cli_refuses_a_uid_of_the_wrong_length(tmp_path):
    script = TOOLS / "agm_logic_crypt.py"
    out = subprocess.run([sys.executable, str(script), "keys", "--uid", "11223344"],
                         capture_output=True, text=True)
    assert out.returncode != 0
    assert "16 bytes" in out.stderr + out.stdout
