#!/usr/bin/env python3
"""Converts the BMP screenshots written by ui_preview / SWITCHCAST_SNAPSHOT to PNG.

Usage: tests/bmp2png.py file.bmp [...]   (writes file.png next to each input)
"""
import struct
import sys
import zlib


def convert(src, dst):
    data = open(src, "rb").read()
    offset = struct.unpack_from("<I", data, 10)[0]
    width, height = struct.unpack_from("<ii", data, 18)
    bytes_pp = struct.unpack_from("<H", data, 28)[0] // 8
    stride = (width * bytes_pp + 3) & ~3
    rows = []
    for y in range(abs(height)):
        src_y = (abs(height) - 1 - y) if height > 0 else y
        row = data[offset + src_y * stride: offset + src_y * stride + width * bytes_pp]
        out = bytearray(b"\x00")
        for x in range(width):
            b, g, r = row[x * bytes_pp], row[x * bytes_pp + 1], row[x * bytes_pp + 2]
            out += bytes((r, g, b))
        rows.append(bytes(out))

    def chunk(tag, payload):
        return struct.pack(">I", len(payload)) + tag + payload + struct.pack(">I", zlib.crc32(tag + payload) & 0xFFFFFFFF)

    png = (b"\x89PNG\r\n\x1a\n"
           + chunk(b"IHDR", struct.pack(">IIBBBBB", width, abs(height), 8, 2, 0, 0, 0))
           + chunk(b"IDAT", zlib.compress(b"".join(rows), 6))
           + chunk(b"IEND", b""))
    open(dst, "wb").write(png)


if __name__ == "__main__":
    for path in sys.argv[1:]:
        convert(path, path[:-4] + ".png")
