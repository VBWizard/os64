#!/bin/bash
# Generate PNGs independently on the host and drive libpng across its format
# and refusal boundaries under the sanitizers.

set -eu
cd "$(git rev-parse --show-toplevel)"

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

cc -std=c11 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
   -I userland/libpng/include -I userland/libgzip/include \
   -I userland/libos64/include -I abi/include \
   userland/libpng/png.c userland/libgzip/inflate.c \
   userland/libos64/crc32.c tools/test_png_host.c \
   -o "$work/test_png"

python3 - "$work" <<'PY'
import pathlib
import struct
import sys
import zlib

from PIL import Image

root = pathlib.Path(sys.argv[1])
signature = b"\x89PNG\r\n\x1a\n"

depths = {
    0: (1, 2, 4, 8, 16),
    2: (8, 16),
    3: (1, 2, 4, 8),
    4: (8, 16),
    6: (8, 16),
}
channels = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}

def chunk(kind, payload=b""):
    body = kind + payload
    return struct.pack(">I", len(payload)) + body + struct.pack(">I", zlib.crc32(body))

def ihdr(w, h, depth, color, interlace=0):
    return chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, depth, color, 0, 0, interlace))

def pack_samples(values, depth):
    if depth == 8:
        return bytes(values)
    if depth == 16:
        return b"".join(struct.pack(">H", value) for value in values)
    out = bytearray()
    byte = 0
    used = 0
    for value in values:
        byte |= value << (8 - used - depth)
        used += depth
        if used == 8:
            out.append(byte)
            byte = 0
            used = 0
    if used:
        out.append(byte)
    return bytes(out)

def paeth(a, b, c):
    p = a + b - c
    pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
    return a if pa <= pb and pa <= pc else b if pb <= pc else c

def filter_row(raw, previous, bpp, kind):
    result = bytearray(len(raw))
    for i, value in enumerate(raw):
        left = raw[i - bpp] if i >= bpp else 0
        up = previous[i] if previous else 0
        upper_left = previous[i - bpp] if previous and i >= bpp else 0
        prediction = (0, left, up, (left + up) // 2,
                      paeth(left, up, upper_left))[kind]
        result[i] = (value - prediction) & 0xff
    return bytes([kind]) + result

def scale(value, depth):
    if depth == 8:
        return value
    if depth == 16:
        return value >> 8
    maximum = (1 << depth) - 1
    return (value * 255 + maximum // 2) // maximum

def rgba_for(rows, depth, color, palette=None, trns=None):
    out = bytearray()
    for row in rows:
        step = channels[color]
        for at in range(0, len(row), step):
            values = row[at:at + step]
            if color == 0:
                gray = scale(values[0], depth)
                alpha = 0 if trns is not None and values[0] == trns else 255
                out += bytes((gray, gray, gray, alpha))
            elif color == 2:
                alpha = 0 if trns is not None and tuple(values) == tuple(trns) else 255
                out += bytes((scale(values[0], depth), scale(values[1], depth),
                              scale(values[2], depth), alpha))
            elif color == 3:
                r, g, b = palette[values[0]]
                alpha = trns[values[0]] if trns is not None and values[0] < len(trns) else 255
                out += bytes((r, g, b, alpha))
            elif color == 4:
                gray = scale(values[0], depth)
                out += bytes((gray, gray, gray, scale(values[1], depth)))
            else:
                out += bytes(scale(value, depth) for value in values)
    return bytes(out)

def zstream(raw, level=6, strategy=zlib.Z_DEFAULT_STRATEGY):
    encoder = zlib.compressobj(level, zlib.DEFLATED, 15, 8, strategy)
    return encoder.compress(raw) + encoder.flush()

def assemble(w, h, depth, color, rows, filters=None, palette=None, trns=None,
             level=6, strategy=zlib.Z_DEFAULT_STRATEGY, split=None,
             before=(), interlace=0, raw_override=None, z_override=None):
    assert depth in depths[color]
    filters = filters or [0] * h
    bpp = (channels[color] * depth + 7) // 8
    previous = None
    filtered = bytearray()
    for values, kind in zip(rows, filters):
        raw = pack_samples(values, depth)
        filtered += filter_row(raw, previous, bpp, kind)
        previous = raw
    raw_stream = bytes(filtered) if raw_override is None else raw_override
    compressed = zstream(raw_stream, level, strategy) if z_override is None else z_override

    pieces = [compressed]
    if split:
        pieces = []
        at = 0
        for amount in split:
            pieces.append(compressed[at:at + amount])
            at += amount
        if at < len(compressed):
            pieces.append(compressed[at:])

    body = bytearray(signature + ihdr(w, h, depth, color, interlace))
    body += b"".join(before)
    if palette is not None:
        body += chunk(b"PLTE", bytes(component for rgb in palette for component in rgb))
    if trns is not None:
        if color == 0:
            payload = struct.pack(">H", trns)
        elif color == 2:
            payload = struct.pack(">HHH", *trns)
        else:
            payload = bytes(trns)
        body += chunk(b"tRNS", payload)
    body += b"".join(chunk(b"IDAT", piece) for piece in pieces)
    body += chunk(b"IEND")
    return bytes(body), rgba_for(rows, depth, color, palette, trns), compressed

def valid(name, w, h, depth, color, rows, **kwargs):
    png, expected, compressed = assemble(w, h, depth, color, rows, **kwargs)
    (root / f"{name}.png").write_bytes(png)
    (root / f"{name}.rgba").write_bytes(expected)
    # Pillow is the independent decoder for the <=8-bit paths. Its conversion
    # of 16-bit multichannel PNG to RGBA is build-dependent, so those fixtures
    # retain explicit sample values and test os64's documented high-byte
    # reduction directly instead of laundering it through that conversion.
    transparent = kwargs.get("trns")
    if depth <= 8 and not (color == 0 and depth < 8 and transparent is not None):
        with Image.open(root / f"{name}.png") as image:
            actual = image.convert("RGBA").tobytes()
        if actual != expected:
            raise SystemExit(f"host decoder disagrees on {name}")
    return png, expected, compressed

def invalid(name, contents):
    (root / f"{name}.png").write_bytes(contents)

# Every legal non-interlaced color/depth pair. Values avoid symmetry and hit
# both ends of each sample range.
for color, legal_depths in depths.items():
    for depth in legal_depths:
        maximum = (1 << depth) - 1
        if color == 0:
            row = [0, maximum, maximum // 2, 1]
            palette = trns = None
        elif color == 2:
            row = [maximum, 0, maximum // 2, 0, maximum, 1]
            palette = trns = None
        elif color == 3:
            palette = [(255, 0, 0), (0, 255, 0)]
            row = [0, 1]
            trns = [17, 231]
        elif color == 4:
            row = [0, maximum, maximum, maximum // 2]
            palette = trns = None
        else:
            row = [maximum, 0, maximum // 2, maximum,
                   0, maximum, 1, maximum // 2]
            palette = trns = None
        valid(f"c{color}d{depth}", len(row) // channels[color], 1,
              depth, color, [row], palette=palette, trns=trns)

# Five rows, five filters, one-byte IDAT chunks, and three DEFLATE block kinds.
filter_rows = []
for y in range(5):
    row = []
    for x in range(5):
        row += [(x * 41 + y * 7) & 255, (y * 53 + x * 3) & 255,
                (x * 17 + y * 29) & 255, 255 - x * 11 - y * 5]
    filter_rows.append(row)
base, base_rgba, base_z = valid("filters", 5, 5, 8, 6, filter_rows,
                                filters=[0, 1, 2, 3, 4],
                                split=[1] * 4096,
                                before=[chunk(b"abCd", b"safe-to-skip")])
valid("stored", 5, 5, 8, 6, filter_rows, level=0)
valid("fixed", 5, 5, 8, 6, filter_rows, strategy=zlib.Z_FIXED)

valid("gray_trns", 3, 1, 4, 0, [[0, 5, 15]], trns=5)
valid("rgb_trns", 2, 1, 8, 2, [[1, 2, 3, 4, 5, 6]], trns=(4, 5, 6))

# Refusals are rebuilt with correct outer CRCs when testing an inner zlib or
# scanline error, so the expected layer—not an earlier checksum—answers.
invalid("bad_signature", b"not png")
bad_crc = bytearray(base)
bad_crc[-5] ^= 1
invalid("bad_crc", bad_crc)
invalid("bad_adler", assemble(5, 5, 8, 6, filter_rows,
        z_override=base_z[:-1] + bytes([base_z[-1] ^ 1]))[0])
invalid("bad_zlib", assemble(5, 5, 8, 6, filter_rows,
        z_override=bytes([base_z[0] ^ 1]) + base_z[1:])[0])
dictionary_flag = next(flag for flag in range(256)
                       if flag & 0x20 and ((0x78 << 8) | flag) % 31 == 0)
invalid("preset_dictionary", assemble(5, 5, 8, 6, filter_rows,
        z_override=bytes((0x78, dictionary_flag)) + b"\0\0\0\0" + base_z[2:])[0])
bad_filter_raw = bytes([5]) + pack_samples(filter_rows[0], 8)
invalid("bad_filter", assemble(5, 1, 8, 6, [filter_rows[0]],
        raw_override=bad_filter_raw)[0])
invalid("truncated", assemble(5, 5, 8, 6, filter_rows,
        z_override=base_z[:-3])[0])
invalid("zlib_trailing", assemble(5, 5, 8, 6, filter_rows,
        z_override=base_z + b"x")[0])
invalid("unknown_critical", assemble(5, 5, 8, 6, filter_rows,
        before=[chunk(b"ABCD", b"future")])[0])
invalid("reserved_name", assemble(5, 5, 8, 6, filter_rows,
        before=[chunk(b"abce", b"reserved")])[0])
invalid("interlaced", assemble(5, 5, 8, 6, filter_rows, interlace=1)[0])

nonconsecutive = (signature + ihdr(5, 5, 8, 6) +
                  chunk(b"IDAT", base_z[:3]) + chunk(b"abCd", b"gap") +
                  chunk(b"IDAT", base_z[3:]) + chunk(b"IEND"))
invalid("nonconsecutive", nonconsecutive)
invalid("after_iend", base + b"x")
invalid("zero_width", signature + ihdr(0, 1, 8, 6) +
        chunk(b"IDAT", zstream(b"")) + chunk(b"IEND"))

palette_bad = (signature + ihdr(1, 1, 2, 3) +
               chunk(b"PLTE", bytes((0, 0, 0, 255, 255, 255))) +
               chunk(b"IDAT", zstream(bytes([0]) + pack_samples([3], 2))) +
               chunk(b"IEND"))
invalid("palette_index", palette_bad)
PY

run() {
    ASAN_OPTIONS=detect_leaks=0 "$work/test_png" "$@"
}

for color in 0 2 3 4 6; do
    case "$color" in
        0) ds="1 2 4 8 16" ;;
        2) ds="8 16" ;;
        3) ds="1 2 4 8" ;;
        4|6) ds="8 16" ;;
    esac
    for depth in $ds; do
        case "$color" in
            0) width=4 ;;
            *) width=2 ;;
        esac
        run "$work/c${color}d${depth}.png" "$work/c${color}d${depth}.rgba" \
            "$width" 1 0 ok any
    done
done
echo "PNG legal color types and bit depths                         PASS"

for name in filters stored fixed gray_trns rgb_trns; do
    case "$name" in
        filters|stored|fixed) width=5; height=5 ;;
        gray_trns) width=3; height=1 ;;
        rgb_trns) width=2; height=1 ;;
    esac
    run "$work/$name.png" "$work/$name.rgba" "$width" "$height" 0 ok any
done
echo "PNG filters, IDAT splits, block kinds, and transparency       PASS"

run "$work/bad_signature.png" - 0 0 0 "not a PNG" 0
run "$work/bad_crc.png" - 0 0 0 "PNG checksum mismatch" 0
run "$work/bad_adler.png" - 0 0 0 "PNG checksum mismatch" any
run "$work/bad_zlib.png" - 0 0 0 "malformed PNG" any
run "$work/preset_dictionary.png" - 0 0 0 "malformed PNG" any
run "$work/bad_filter.png" - 0 0 0 "malformed PNG" any
run "$work/truncated.png" - 0 0 0 "malformed PNG" any
run "$work/zlib_trailing.png" - 0 0 0 "malformed PNG" any
run "$work/unknown_critical.png" - 0 0 0 "unsupported PNG variant" 0
run "$work/reserved_name.png" - 0 0 0 "malformed PNG" 0
run "$work/interlaced.png" - 0 0 0 "unsupported PNG variant" 0
run "$work/nonconsecutive.png" - 0 0 0 "malformed PNG" 0
run "$work/after_iend.png" - 0 0 0 "malformed PNG" 0
run "$work/zero_width.png" - 0 0 0 "malformed PNG" 0
run "$work/palette_index.png" - 0 0 0 "malformed PNG" any
run "$work/filters.png" - 0 0 24 "decoded image exceeds limit" 0
echo "PNG structural, checksum, zlib, scanline, and limit refusals  PASS"

run mutate "$work/filters.png"
echo "PNG every-byte truncation and mutation sanitizer sweep         PASS"
