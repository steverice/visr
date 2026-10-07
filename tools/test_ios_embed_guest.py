"""Tests for tools/ios_embed_guest.py."""

import hashlib
import struct

from tools import ios_embed_guest


def guest_elf(code=b"\x1f\x20\x03\xd5" * 4, data=b"\x01\x02"):
    """a minimal AArch64 ELF64 with the guest's layout: one RX segment at 0x88000000, one RW segment after it"""
    header = bytearray(64)
    header[:6] = b"\x7fELF\x02\x01"
    struct.pack_into("<H", header, 18, 183)            # e_machine: AArch64
    struct.pack_into("<Q", header, 32, 64)             # e_phoff
    struct.pack_into("<HH", header, 54, 56, 2)         # e_phentsize, e_phnum
    segments = struct.pack("<IIQQQQQQ", 1, 5, 0x1000, 0x88000000, 0, len(code), len(code), 0x4000)
    segments += struct.pack("<IIQQQQQQ", 1, 6, 0x2000, 0x88004000, 0, len(data), 0x100, 0x4000)
    image = bytes(header) + segments
    image += b"\0" * (0x1000 - len(image)) + code
    image += b"\0" * (0x2000 - len(image)) + data
    return image


def test_the_header_names_the_guest_image_s_sha256(tmp_path):
    image = guest_elf()
    (tmp_path / "guest.elf").write_bytes(image)
    ios_embed_guest.embed(tmp_path / "guest.elf", tmp_path / "out")
    header = (tmp_path / "out/guest_image.h").read_text()
    assert f'#define HALO_GUEST_SHA256 "{hashlib.sha256(image).hexdigest()}"' in header


def test_two_guests_get_two_hashes(tmp_path):
    for name, code in (("a", b"\x1f\x20\x03\xd5" * 4), ("b", b"\xc0\x03\x5f\xd6" * 4)):
        (tmp_path / f"{name}.elf").write_bytes(guest_elf(code))
        ios_embed_guest.embed(tmp_path / f"{name}.elf", tmp_path / name)
    assert (tmp_path / "a/guest_image.h").read_text() != (tmp_path / "b/guest_image.h").read_text()
