"""Check hand-authored F0 acceptance vectors against exported fake metrics.

This is a small contract arithmetic oracle, not the production layout engine.
Cluster decisions are supplied by the vectors; it does not prove a UTF-8 parser.
"""
import json
from pathlib import Path
import sys


def check(metrics_path):
    fonts = json.loads(Path(metrics_path).read_text())
    fixture = json.loads(Path(__file__).with_name("fixtures.json").read_text())
    assert fixture["revision"] == 2 and fixture["units_per_pixel"] == 64
    fonts = {name: {"glyphs": {g["id"]: g for g in f["glyphs"]},
                    "pairs": {(left, right): delta for left, right, delta in f["pairs"]}}
             for name, f in fonts.items()}
    names = set()
    for case in fixture["cases"]:
        name = case["name"]
        assert name not in names
        names.add(name)
        data = bytes.fromhex(case["hex"])
        assert b"\n" not in data
        pen = 0
        carets = [[0, 0]]
        origins, inks, spans = [], [], []
        previous = None
        end = 0
        for start, stop, glyphs in case["clusters"]:
            assert start == end and start < stop <= len(data), name
            end = stop
            for ordinal, (face, glyph) in enumerate(glyphs):
                if face == "tab":
                    assert data[start:stop] == b"\t"
                    origin, interval = case["tab_origin"], case["tab_interval"]
                    assert interval > 0
                    pen = origin + ((pen - origin) // interval + 1) * interval
                    previous = None
                else:
                    if face == "marker":
                        advance, ink = 512, [0, -768, 512, 256]
                        previous = None
                    else:
                        m = fonts[face]["glyphs"][glyph]
                        assert m["present"], name
                        # These explicit accent cases test source spans without
                        # borrowing normalization from the host's Unicode version.
                        raw = data[start:stop]
                        scalar = 0xe9 if raw == bytes.fromhex("65cc81") else ord(raw.decode("utf-8")[0])
                        assert scalar == m["scalar"], name
                        for earlier in case.get("font_order", [face]):
                            if earlier == face:
                                break
                            assert not any(g["scalar"] == scalar and g["present"]
                                           for g in fonts[earlier]["glyphs"].values()), name
                        if previous and previous[0] == face:
                            assert ordinal == 0, name
                            adjustment = fonts[face]["pairs"].get((previous[1], glyph), 0)
                            assert carets[-1][1] + adjustment > carets[-2][1], name
                            pen += adjustment
                            carets[-1][1] += adjustment
                        advance, ink = m["advance"], m["ink"]
                        previous = (face, glyph)
                    origins.append(pen)
                    spans.append([start, stop])
                    if ink[0] != ink[2] and ink[1] != ink[3]:
                        inks.append([pen + ink[0], ink[1], pen + ink[2], ink[3]])
                    pen += advance
            carets.append([stop, pen])
        assert end == len(data), name
        union = [min(i[0] for i in inks), min(i[1] for i in inks),
                 max(i[2] for i in inks), max(i[3] for i in inks)] if inks else [0, 0, 0, 0]
        assert pen == case["advance"], name
        assert spans == case["placement_spans"], name
        assert origins == case["origins"] and union == case["ink"], name
        assert carets == case["carets"], name
        assert all(a[0] < b[0] and a[1] < b[1] for a, b in zip(carets, carets[1:])), name
        if "paint_x" in case:
            assert [(x + 32) // 64 for x in origins] == case["paint_x"], name
        for x, expected in case["hits"]:
            assert min(carets, key=lambda c: (abs(c[1] - x), -c[0]))[0] == expected, name
        for width, expected in case["fits"]:
            assert max(c[0] for c in carets if c[1] <= width) == expected, name
        by_byte = dict(carets)
        start, stop, rect = case["selection"]
        assert ([by_byte[start], -768, by_byte[stop], 256] if start != stop else [0, 0, 0, 0]) == rect, name
        if "row_box" in case:
            y0, y1 = case["row_box"]
            assert [by_byte[start], y0, by_byte[stop], y1] == case["row_selection"], name
            assert [union[0], max(union[1], y0), union[2], min(union[3], y1)] == case["clipped_ink"], name
        for offset, bias, expected in case.get("snaps", []):
            actual = (max(c[0] for c in carets if c[0] <= offset) if bias == "before"
                      else min(c[0] for c in carets if c[0] >= offset))
            assert actual == expected, name
    print(f"PASS: {len(names)} hand-authored layout acceptance vectors (arithmetic consistency)")


if __name__ == "__main__":
    check(sys.argv[1])
