#!/bin/bash
# vncd's ZRLE tile encoder against an independent decoder written from RFC
# 6143 section 7.7.6, across pixel formats, tile shapes and image kinds.
set -eu
cd "$(git rev-parse --show-toplevel)"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
cc -std=c11 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
   tools/test_zrle_host.c userland/apps/vncd/zrle.c -o "$work/zrle"
"$work/zrle" "$work/cases"
python3 - "$work/cases" <<'PY'
import struct, sys
data = open(sys.argv[1], 'rb').read()
pos = 0
def take(n):
    global pos
    b = data[pos:pos + n]; pos += n; return b
cases = shapes = 0
while pos < len(data):
    w, h = struct.unpack('<II', take(8))
    bpp, depth, be, rmax, gmax, bmax, rs, gs, bs, _ = take(10)
    px = struct.unpack(f'<{w*h}I', take(4 * w * h))
    n = struct.unpack('<I', take(4))[0]
    enc = take(n)
    def value(xrgb):
        r, g, b = (xrgb >> 16) & 255, (xrgb >> 8) & 255, xrgb & 255
        return ((r * rmax + 127) // 255) << rs | ((g * gmax + 127) // 255) << gs | ((b * bmax + 127) // 255) << bs
    bits = rmax << rs | gmax << gs | bmax << bs
    if bpp == 32 and depth <= 24 and bits < (1 << 24): cb, high = 3, False
    elif bpp == 32 and depth <= 24 and bits & 0xFF == 0: cb, high = 3, True
    else: cb, high = bpp // 8, False
    p = 0
    def cpixel():
        global p
        raw = enc[p:p + cb]; p += cb
        v = int.from_bytes(raw, 'big' if be else 'little')
        return v << 8 if high else v
    def runlen():
        global p
        total = 1
        while True:
            b = enc[p]; p += 1; total += b
            if b != 255: return total
    out = [0] * (w * h)
    for ty in range(0, h, 64):
        for tx in range(0, w, 64):
            tw, th = min(64, w - tx), min(64, h - ty)
            sub = enc[p]; p += 1
            vals = []
            if sub == 0:
                vals = [cpixel() for _ in range(tw * th)]
            elif sub == 1:
                vals = [cpixel()] * (tw * th)
            elif 2 <= sub <= 16:
                pal = [cpixel() for _ in range(sub)]
                bitw = 1 if sub == 2 else 2 if sub <= 4 else 4
                for _ in range(th):
                    rowbytes = (tw * bitw + 7) // 8
                    row = enc[p:p + rowbytes]; p += rowbytes
                    for x in range(tw):
                        bit = x * bitw
                        idx = (row[bit // 8] >> (8 - bitw - bit % 8)) & ((1 << bitw) - 1)
                        vals.append(pal[idx])
            elif sub == 128:
                while len(vals) < tw * th:
                    c = cpixel(); vals += [c] * runlen()
            elif sub >= 130:
                pal = [cpixel() for _ in range(sub - 128)]
                while len(vals) < tw * th:
                    b = enc[p]; p += 1
                    vals += [pal[b & 127]] * (runlen() if b & 128 else 1)
            else:
                raise SystemExit(f'subencoding {sub} is not ZRLE')
            shapes |= 1 << (sub if sub < 17 else 17 if sub == 128 else 18)
            assert len(vals) == tw * th, (sub, len(vals), tw * th)
            for y in range(th):
                for x in range(tw):
                    out[(ty + y) * w + tx + x] = vals[y * tw + x]
    assert p == n, (p, n)
    want = [value(v) for v in px]
    assert out == want, (w, h, bpp, be, [i for i in range(w * h) if out[i] != want[i]][:5])
    cases += 1
assert shapes & (1 << 1) and shapes & (1 << 2) and shapes & (1 << 17) and shapes & (1 << 18) and shapes & 1, bin(shapes)
print(f'zrle: {cases} images across 7 pixel formats decode exactly; raw, solid, packed palette, RLE and palette RLE all chosen PASS')
PY
