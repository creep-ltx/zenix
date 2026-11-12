#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""The demo floppy's ADF (DESIGN D25 revision 3, section 4; the layout: boot/demounpack.h).

usage: mkdemo.py BOOTBLOCK.bin STAGE2.zx KERNEL.bin ROOT.img OUT.adf [dd|hd]

The kernel (padded with zeros to a multiple of 512) and the RAM disk's image are packed as one zlib stream behind the
bootblock, the stage 2 and the table sector. The packer is libdeflate at its level 12 (its slow, thorough search:
about 6% smaller than zlib's level 9 on the DD disk, 2026-11-11, 882,779 -> 829,9xx bytes; the same zlib format, so
the stage 2's unpacker is unchanged). Without libdeflate (a build requirement, docs/README.md) Python's zlib at
level 9 is used and the build says so: the DD disk may then not fit. Every build prints the parts and
the space left; an image that does not fit is refused, saying by how much (nothing is written then).
"""
import ctypes
import ctypes.util
import struct
import sys
import zlib

SECTOR = 512
GEOMETRY = {"dd": (901120, 5632), "hd": (1802240, 11264)}   # bytes, a track's bytes (11 or 22 sectors)
DT_MAGIC = 0x5A444D31          # "ZDM1"


def carry_sum(data: bytes) -> int:
    s = 0
    for (v,) in struct.iter_unpack(">I", data):
        s += v
        if s > 0xFFFFFFFF:
            s = (s & 0xFFFFFFFF) + 1
    return s


def pad(b: bytes, n: int = SECTOR) -> bytes:
    return b + bytes(-len(b) % n)


def pack(data: bytes) -> bytes:
    """data as a zlib stream: libdeflate's level 12 when the library is there, else zlib's level 9 (said)"""
    name = ctypes.util.find_library("deflate")
    if name:
        lib = ctypes.CDLL(name)
        lib.libdeflate_alloc_compressor.restype = ctypes.c_void_p
        lib.libdeflate_alloc_compressor.argtypes = [ctypes.c_int]
        lib.libdeflate_zlib_compress.restype = ctypes.c_size_t
        lib.libdeflate_zlib_compress.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_size_t,
                                                 ctypes.c_char_p, ctypes.c_size_t]
        lib.libdeflate_free_compressor.argtypes = [ctypes.c_void_p]
        c = lib.libdeflate_alloc_compressor(12)
        if c:
            out = ctypes.create_string_buffer(len(data) + len(data) // 8 + 4096)
            n = lib.libdeflate_zlib_compress(c, data, len(data), out, len(out))
            lib.libdeflate_free_compressor(c)
            if n and zlib.decompress(out.raw[:n]) == data:
                return out.raw[:n]
    print("mkdemo.py: libdeflate not found (or failed): packed with zlib's level 9, about 6% larger", file=sys.stderr)
    return zlib.compress(data, 9)


def main() -> None:
    if len(sys.argv) not in (6, 7):
        sys.exit(__doc__)
    bb_path, s2_path, k_path, img_path, out = sys.argv[1:6]
    kind = sys.argv[6] if len(sys.argv) == 7 else "dd"
    if kind not in GEOMETRY:
        sys.exit(f"mkdemo.py: the disk is dd or hd, not {kind}")
    disk_len, track = GEOMETRY[kind]
    bootcode = open(bb_path, "rb").read()
    stage2 = open(s2_path, "rb").read()
    kernel = open(k_path, "rb").read()
    image = open(img_path, "rb").read()
    if stage2[4:8] != b"ZEXE" or kernel[4:8] != b"ZEXE":
        sys.exit("mkdemo.py: the stage 2 and the kernel must be ZEXE images")
    if len(image) % 1024 or image[1080:1082] != b"\x53\xef":
        sys.exit("mkdemo.py: the RAM disk must be an ext2 image of whole 1 KB blocks")
    if len(bootcode) > 1016 - 12:
        sys.exit(f"mkdemo.py: bootblock code too big: {len(bootcode)} > {1016 - 12}")
    s2_mem = max(struct.unpack(">I", stage2[12:16])[0], len(stage2))
    s2 = pad(stage2)

    bb = bytearray(1024)
    bb[0:4] = b"DOS\0"
    bb[8:12] = struct.pack(">I", len(s2))
    bb[1016:1020] = struct.pack(">I", track)       # the disk's track bytes: demoboot.S compares the drive's
    bb[1020:1024] = struct.pack(">I", (s2_mem + 3) & ~3)
    bb[12:12 + len(bootcode)] = bootcode
    bb[4:8] = struct.pack(">I", 0xFFFFFFFF - carry_sum(bytes(bb)))
    assert carry_sum(bytes(bb)) == 0xFFFFFFFF

    kpad = pad(kernel)
    kmem = struct.unpack(">I", kernel[12:16])[0]
    plain = kpad + image
    stream = pack(plain)
    table_off = 1024 + len(s2)
    stream_off = table_off + SECTOR
    used = stream_off + len(pad(stream))
    if used > disk_len:
        sys.exit(f"mkdemo.py: {out}: {used - disk_len} bytes too big for a {kind.upper()} disk "
                 f"({used} of {disk_len}: the packed stream is {len(stream)} bytes); nothing written")

    t = bytearray(SECTOR)
    struct.pack_into(">9I", t, 0, DT_MAGIC, 1, stream_off, len(stream), len(kpad), kmem, len(image), disk_len, track)
    s = sum(struct.unpack(">127I", t[:508])) & 0xFFFFFFFF
    struct.pack_into(">I", t, 508, (-s) & 0xFFFFFFFF)

    adf = bytearray(disk_len)
    adf[0:1024] = bb
    adf[1024:1024 + len(s2)] = s2
    adf[table_off:table_off + SECTOR] = t
    adf[stream_off:stream_off + len(stream)] = stream
    open(out, "wb").write(adf)
    free = disk_len - used
    print(f"{out}: {kind.upper()} {disk_len} bytes: bootblock 1024, stage 2 {len(s2)}, table {SECTOR}, "
          f"stream {len(stream)} (kernel {len(kernel)} + RAM disk {len(image)} = {len(plain)} unpacked, "
          f"{100 * len(stream) // len(plain)}%); {free} bytes free ({free // 1024} KB, {100 * free // disk_len}%)")


if __name__ == "__main__":
    main()
