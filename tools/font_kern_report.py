#!/usr/bin/env python3
"""font_kern_report.py — read a font's kerning pairs WITHOUT FreeType.

    tools/font_kern_report.py userland/libfreetype/fixtures/*.ttf \\
                              userland/libfreetype/fixtures/*.otf
    tools/font_kern_report.py --pairs AV,AT,To,LT --ppem 16,32 <font>…
    tools/font_kern_report.py --fixtures        # exactly the table in FIXTURES.md

WHY THIS EXISTS. The backend's kerning tests assert against numbers, and a
number the backend produced cannot check the backend. This is the second
opinion: an independent reader of the same bytes, sharing no code with
FreeType or with `port/backend.c`, so when the two agree the agreement means
something. The values in `userland/libfreetype/fixtures/FIXTURES.md`, and the
expectations pinned in `tools/test_freetype_host.c` and
`userland/tests/fonttest/fonttest.c`, all come from `--fixtures` below.

WHAT IT DELIBERATELY COPIES, being the one thing it cannot be independent
about: FreeType 2.14.3's rule for which GPOS pair tables it will read at all
(`src/sfnt/ttgpos.c`) — `valueFormat1 == 0x0004` and `valueFormat2 == 0`, an
X advance on the first glyph and nothing on the second. A reader that
accepted more would report kerning the engine does not apply and call the
engine wrong. The rule is marked GPOS_ACCEPTED_* below so it can be found
when the pin moves; UPSTREAM_REVIEW.md carries the argument.

Precedence is upstream's too: a `kern` table WINS over GPOS, and the two are
never summed.

Everything else here — the table directory, `cmap` format 4, `kern` format 0,
coverage tables, class definitions, PairPos formats 1 and 2, extension
lookups — is read from the OpenType specification directly.
"""

import argparse
import struct
import sys
from pathlib import Path

# FreeType 2.14.3, src/sfnt/ttgpos.c, tt_face_validate_pair_pos{1,2}: the only
# value formats its "basic GPOS kerning" support consents to read.
GPOS_ACCEPTED_VALUE_FORMAT_1 = 0x0004      # XAdvance on the first glyph
GPOS_ACCEPTED_VALUE_FORMAT_2 = 0x0000      # nothing on the second


def u16(b, o):
    return struct.unpack_from(">H", b, o)[0]


def i16(b, o):
    return struct.unpack_from(">h", b, o)[0]


def u32(b, o):
    return struct.unpack_from(">I", b, o)[0]


def table_directory(data):
    """tag -> (offset, length) for an SFNT file."""
    if len(data) < 12:
        raise ValueError("too short to be a font")
    count = u16(data, 4)
    tables = {}
    for i in range(count):
        entry = 12 + i * 16
        tag = data[entry:entry + 4].decode("latin-1")
        tables[tag] = (u32(data, entry + 8), u32(data, entry + 12))
    return tables


def units_per_em(data, tables):
    head, _ = tables["head"]
    return u16(data, head + 18)


def unicode_cmap(data, tables):
    """scalar -> glyph id, from the best format-4 Unicode subtable."""
    base, length = tables["cmap"]
    cmap = data[base:base + length]
    chosen = None
    for i in range(u16(cmap, 2)):
        plat, enc, offset = struct.unpack_from(">HHI", cmap, 4 + i * 8)
        if (plat == 3 and enc in (1, 10)) or plat == 0:
            if u16(cmap, offset) == 4:
                chosen = offset
    if chosen is None:
        raise ValueError("no format-4 Unicode character map")

    seg_x2 = u16(cmap, chosen + 6)
    segs = seg_x2 // 2
    ends = chosen + 14
    starts = ends + seg_x2 + 2
    deltas = starts + seg_x2
    ranges = deltas + seg_x2

    out = {}
    for s in range(segs):
        end = u16(cmap, ends + s * 2)
        start = u16(cmap, starts + s * 2)
        delta = i16(cmap, deltas + s * 2)
        range_offset = u16(cmap, ranges + s * 2)
        for ch in range(start, min(end, 0xFFFF) + 1):
            if range_offset == 0:
                gid = (ch + delta) & 0xFFFF
            else:
                at = ranges + s * 2 + range_offset + (ch - start) * 2
                if at + 2 > len(cmap):
                    continue
                gid = u16(cmap, at)
                if gid:
                    gid = (gid + delta) & 0xFFFF
            if gid:
                out[ch] = gid
    return out


def coverage_table(g, at):
    """glyph id -> coverage index."""
    fmt = u16(g, at)
    out = {}
    if fmt == 1:
        for i in range(u16(g, at + 2)):
            out[u16(g, at + 4 + i * 2)] = i
    elif fmt == 2:
        for i in range(u16(g, at + 2)):
            first, last, start_index = struct.unpack_from(">HHH", g, at + 4 + i * 6)
            for k, gid in enumerate(range(first, last + 1)):
                out[gid] = start_index + k
    return out


def class_def_table(g, at):
    """glyph id -> class (absent means class 0)."""
    fmt = u16(g, at)
    out = {}
    if fmt == 1:
        start, count = struct.unpack_from(">HH", g, at + 2)
        for i in range(count):
            out[start + i] = u16(g, at + 6 + i * 2)
    elif fmt == 2:
        for i in range(u16(g, at + 2)):
            first, last, klass = struct.unpack_from(">HHH", g, at + 4 + i * 6)
            for gid in range(first, last + 1):
                out[gid] = klass
    return out


def kern_table_pairs(data, tables):
    """(left, right) -> font units, from the legacy `kern` table."""
    if "kern" not in tables:
        return {}
    base, length = tables["kern"]
    k = data[base:base + length]
    pairs = {}
    at = 4
    for _ in range(u16(k, 2)):
        sub_length = u16(k, at + 2)
        coverage = u16(k, at + 4)
        # Horizontal, not minimum, format 0 in the high byte. FreeType reads
        # only format 0, and only horizontal data.
        horizontal = (coverage & 0x0001) != 0
        if (coverage >> 8) == 0 and horizontal:
            for j in range(u16(k, at + 6)):
                e = at + 14 + j * 6
                left, right, value = struct.unpack_from(">HHh", k, e)
                if value:
                    pairs[(left, right)] = value
        at += sub_length
    return pairs


def gpos_kern_pairs(data, tables):
    """(left, right) -> font units, from GPOS, FreeType's accepted subset."""
    if "GPOS" not in tables:
        return {}
    base, length = tables["GPOS"]
    g = data[base:base + length]
    if len(g) < 10:
        return {}

    feature_list = u16(g, 6)
    lookup_list = u16(g, 8)

    wanted = set()
    for i in range(u16(g, feature_list)):
        record = feature_list + 2 + i * 6
        if g[record:record + 4] != b"kern":
            continue
        feature = feature_list + u16(g, record + 4)
        for j in range(u16(g, feature + 2)):
            wanted.add(u16(g, feature + 4 + j * 2))

    pairs = {}
    for index in sorted(wanted):
        lookup = lookup_list + u16(g, lookup_list + 2 + index * 2)
        lookup_type = u16(g, lookup)
        for k in range(u16(g, lookup + 4)):
            sub = lookup + u16(g, lookup + 6 + k * 2)
            real_type = lookup_type
            if lookup_type == 9:            # Positioning Extension
                if u16(g, sub) != 1:
                    continue
                real_type = u16(g, sub + 2)
                sub = sub + u32(g, sub + 4)
            if real_type != 2:              # PairPos
                continue

            pair_format = u16(g, sub)
            value_format_1 = u16(g, sub + 4)
            value_format_2 = u16(g, sub + 6)
            if (value_format_1 != GPOS_ACCEPTED_VALUE_FORMAT_1 or
                    value_format_2 != GPOS_ACCEPTED_VALUE_FORMAT_2):
                continue                    # FreeType would skip this subtable

            coverage = coverage_table(g, sub + u16(g, sub + 2))

            if pair_format == 1:
                pair_set_count = u16(g, sub + 8)
                for gid, ci in coverage.items():
                    if ci >= pair_set_count:
                        continue
                    pair_set = sub + u16(g, sub + 10 + ci * 2)
                    for m in range(u16(g, pair_set)):
                        e = pair_set + 2 + m * 4
                        second, value = struct.unpack_from(">Hh", g, e)
                        if value:
                            pairs[(gid, second)] = value

            elif pair_format == 2:
                c1_off, c2_off, c1_count, c2_count = struct.unpack_from(
                    ">HHHH", g, sub + 8)
                class1 = class_def_table(g, sub + c1_off)
                class2 = class_def_table(g, sub + c2_off)
                records = sub + 16
                by_class = {}
                for a in range(c1_count):
                    for b in range(c2_count):
                        value = i16(g, records + (a * c2_count + b) * 2)
                        if value:
                            by_class[(a, b)] = value
                if by_class:
                    for gid in coverage:
                        a = class1.get(gid, 0)
                        for gid2, b in class2.items():
                            value = by_class.get((a, b))
                            if value:
                                pairs[(gid, gid2)] = value
    return pairs


def read_font(path):
    data = Path(path).read_bytes()
    tables = table_directory(data)
    kern = kern_table_pairs(data, tables)
    gpos = gpos_kern_pairs(data, tables)
    # Upstream's precedence: `kern` wins outright when it is present, and the
    # two are never added together.
    source = "kern" if kern else ("GPOS" if gpos else "none")
    return {
        "path": path,
        "upem": units_per_em(data, tables),
        "cmap": unicode_cmap(data, tables),
        "pairs": kern if kern else gpos,
        "source": source,
        "has_kern": bool(kern),
        "has_gpos": bool(gpos),
    }


def to_26_6(units, upem, ppem):
    """Font units to 26.6 pixels: what FT_KERNING_UNFITTED reports."""
    return units * ppem * 64.0 / upem


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("fonts", nargs="*", help="font files to read")
    ap.add_argument("--pairs", default="AV,AT,AW,To,Ta,LT,Yo,II",
                    help="comma-separated two-character pairs")
    ap.add_argument("--ppem", default="16,32",
                    help="comma-separated pixel sizes")
    ap.add_argument("--fixtures", action="store_true",
                    help="reproduce the table recorded in fixtures/FIXTURES.md")
    args = ap.parse_args()

    fonts = args.fonts
    pairs = args.pairs.split(",")
    ppems = [int(p) for p in args.ppem.split(",")]

    if args.fixtures:
        here = Path(__file__).resolve().parent.parent / "userland/libfreetype/fixtures"
        fonts = [str(here / n) for n in ("DejaVuSans.ttf", "DejaVuSansMono.ttf",
                                         "SourceSans3-Regular.otf",
                                         "SourceCodePro-Regular.otf")]
        pairs = ["AV", "AT", "To", "LT"]
        ppems = [16, 32]

    if not fonts:
        ap.error("name at least one font, or pass --fixtures")

    for path in fonts:
        try:
            font = read_font(path)
        except Exception as exc:                      # noqa: BLE001
            print(f"{path}: {exc}", file=sys.stderr)
            continue

        print(f"\n{Path(path).name}  unitsPerEm={font['upem']}  "
              f"kern={'yes' if font['has_kern'] else 'no'}  "
              f"GPOS={'yes' if font['has_gpos'] else 'no'}  "
              f"-> reads from: {font['source']}  "
              f"({len(font['pairs'])} accepted pairs)")

        header = f"  {'pair':<6} {'gids':<12} {'units':>7}"
        for ppem in ppems:
            header += f" {str(ppem) + 'px (26.6)':>14}"
        print(header)

        for pair in pairs:
            if len(pair) != 2:
                continue
            left = font["cmap"].get(ord(pair[0]))
            right = font["cmap"].get(ord(pair[1]))
            if left is None or right is None:
                print(f"  {pair!r:<6} {'-':<12} {'no glyph':>7}")
                continue
            units = font["pairs"].get((left, right))
            gids = f"{left}->{right}"
            if units is None:
                row = f"  {pair!r:<6} {gids:<12} {'0':>7}"
                for _ in ppems:
                    row += f" {'0.00':>14}"
            else:
                row = f"  {pair!r:<6} {gids:<12} {units:>7}"
                for ppem in ppems:
                    row += f" {to_26_6(units, font['upem'], ppem):>14.2f}"
            print(row)
    print()
    return 0


if __name__ == "__main__":
    sys.exit(main())
