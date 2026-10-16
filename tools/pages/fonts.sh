#!/bin/bash
# The fonts tools/pages/pile3-fonts.html names, into tools/pages/fonts/
# (not kept in git): copies of the licensed fixtures, and a WOFF made from
# one with Python's zlib, each table compressed (font_unwrap.c's input).
set -eu
cd "$(git rev-parse --show-toplevel)"
out=tools/pages/fonts
mkdir -p "$out"
src=userland/libfreetype/fixtures
cp "$src/SourceCodePro-Regular.otf" "$src/DejaVuSerif.ttf" "$src/DejaVuSerif-Italic.ttf" "$out/"
python3 - "$src/DejaVuSans-Bold.ttf" "$out/DejaVuSans-Bold.woff" <<'PY'
import struct, sys, zlib
sfnt = open(sys.argv[1], 'rb').read()
n = struct.unpack_from('>H', sfnt, 4)[0]
ts = [struct.unpack_from('>4sIII', sfnt, 12 + 16 * i) for i in range(n)]
first = 44 + 20 * n
entries, data = [], b''
for tag, check, off, length in ts:
    body = sfnt[off:off + length]
    packed = zlib.compress(body, 9)
    if len(packed) >= len(body):
        packed = body
    entries.append(struct.pack('>4sIIII', tag, first + len(data), len(packed), length, check))
    data += packed + b'\0' * (-len(packed) % 4)
total = 12 + 16 * n + sum((t[3] + 3) & ~3 for t in ts)
head = struct.pack('>4sIIHHIHHIIIII', b'wOFF', struct.unpack_from('>I', sfnt, 0)[0], first + len(data), n, 0,
                   total, 1, 0, 0, 0, 0, 0, 0)
open(sys.argv[2], 'wb').write(head + b''.join(entries) + data)
PY
ls -la "$out"
