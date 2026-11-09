#!/usr/bin/env python3
"""Generate the two deterministic PNGs used by image-responsive.html."""
from pathlib import Path
import struct
import zlib

PAGES = Path(__file__).resolve().parent / "pages"


def chunk(kind, body):
    return (struct.pack(">I", len(body)) + kind + body
            + struct.pack(">I", zlib.crc32(kind + body)))


for scale, left, right in ((1, (32, 96, 192, 255), (240, 120, 32, 255)),
                           (2, (32, 160, 80, 255), (224, 192, 32, 255))):
    width, height = 160 * scale, 90 * scale
    rows = (b"\0" + bytes(left) * (width // 2) + bytes(right) * (width // 2)) * height
    png = (b"\x89PNG\r\n\x1a\n"
           + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0))
           + chunk(b"IDAT", zlib.compress(rows, 9)) + chunk(b"IEND", b""))
    (PAGES / f"image-responsive-{scale}x.png").write_bytes(png)
