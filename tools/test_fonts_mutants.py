#!/usr/bin/env python3
"""One deliberate break at a time to web fonts (docs/design/pending/PILE3.md
§ Web fonts, slice S9) — libgarb's @font-face, libgzip's zlib door and
libpng on it, yonder's WOFF unwrapper — against the suite that holds each
rule: libgarb's corpus and probes, libgzip's, libpng's, and the
unwrapper's. A mutant is CAUGHT when its suite fails, MISSED when it
passes, which means a test is missing and gets written. A build failure is
neither.

The shape is test_motion_mutants.py's: each mutant is applied to the
worktree's copy of its file, its suite is run, and the file is put back
from the bytes read before the change, checked byte for byte, whatever
happens. Run it from a clean tree, and never beside another harness that
edits the same tree; the per-mutant logs are in --logs.

A table packed longer than it unpacks is not broken alone here: the zlib
door's exact output limit refuses such a table too, so the early refusal
in the unwrapper can be told apart from the code only by when it comes.
"""
import argparse
import pathlib
import re
import subprocess
import sys

parser = argparse.ArgumentParser()
parser.add_argument('--logs', type=pathlib.Path, default=pathlib.Path('/tmp/fonts-mutants'))
parser.add_argument('--only', help='run the mutants whose name contains this, or any of these, comma-separated')
args = parser.parse_args()
root = pathlib.Path(__file__).resolve().parent.parent
args.logs.mkdir(parents=True, exist_ok=True)

CASCADE = 'userland/libgarb/cascade.c'
ZLIB = 'userland/libgzip/zlib.c'
PNG = 'userland/libpng/png.c'
UNWRAP = 'userland/apps/yonder/font_unwrap.c'
GARB = 'bash tools/test_garb_host.sh'
GZIP = 'bash tools/test_gzip_host.sh'
PNGS = 'bash tools/test_png_host.sh'
UNWRAPS = 'bash tools/test_font_unwrap_host.sh'

# (name, file, old, new, cases). Each breaks one rule the design states.
mutants = [
    # libgarb: the faces.
    ('a-later-face-is-another', CASCADE,
     '        if (same_face(&c->faces[i], &face)) {',
     '        if (false && same_face(&c->faces[i], &face)) {', GARB),
    ('family-case-matters', CASCADE,
     '        char x = a[i] >= \'A\' && a[i] <= \'Z\' ? (char)(a[i] + 32) : a[i];',
     '        char x = a[i];', GARB),
    ('a-generic-names-a-face', CASCADE,
     '    for (int k = 0; words == 1 && kGenerics[k] != NULL; k++)',
     '    for (int k = 0; false && words == 1 && kGenerics[k] != NULL; k++)', GARB),
    ('tech-is-taken', CASCADE,
     '        if (ieq(v[i].text, v[i].len, "tech"))\n            return false;',
     '        if (ieq(v[i].text, v[i].len, "tech"))\n            continue;', GARB),
    ('two-formats-are-taken', CASCADE,
     '        if (!ieq(v[i].text, v[i].len, "format") || out->format != NULL)',
     '        if (!ieq(v[i].text, v[i].len, "format"))', GARB),
    ('local-takes-what-follows', CASCADE,
     '        if (len < 0 || i + 1 != n)',
     '        if (len < 0)', GARB),
    ('weights-keep-their-order', CASCADE,
     '    *lo = (uint16_t)(w[0] < w[1] ? w[0] : w[1]);',
     '    *lo = (uint16_t)w[0];', GARB),
    ('bold-is-normal', CASCADE,
     '            w[k++] = 700;',
     '            w[k++] = 400;', GARB),
    ('auto-is-normal', CASCADE,
     '            w[0] = 1, w[1] = 1000, k = 2;',
     '            w[0] = 400, w[1] = 400, k = 2;', GARB),
    ('oblique-is-italic', CASCADE,
     '        *out = GARB_FACE_OBLIQUE;',
     '        *out = GARB_FACE_ITALIC;', GARB),
    ('a-wildcard-is-its-top', CASCADE,
     '            lo = lo * 16;\n            hi = hi * 16 + 15;',
     '            lo = lo * 16 + 15;\n            hi = hi * 16 + 15;', GARB),
    ('a-wildcard-takes-digits-after', CASCADE,
     '    if (wild > 0) {\n        if (k != len)\n            return false;',
     '    if (wild > 0) {\n        if (false && k != len)\n            return false;', GARB),
    ('a-range-past-unicode', CASCADE,
     '    if (hi > 0x10FFFF || lo > hi)',
     '    if (lo > hi)', GARB),
    ('a-range-backwards', CASCADE,
     '    if (hi > 0x10FFFF || lo > hi)',
     '    if (hi > 0x10FFFF)', GARB),
    ('ranges-forgive', CASCADE,
     '            int32_t got = ranges != NULL ? parts(c, d, range_part, ranges, most, false) : -1;',
     '            int32_t got = ranges != NULL ? parts(c, d, range_part, ranges, most, true) : -1;', GARB),
    ('sources-do-not-forgive', CASCADE,
     '            int32_t got = src != NULL ? parts(c, d, src_part, src, most, true) : 0;',
     '            int32_t got = src != NULL ? parts(c, d, src_part, src, most, false) : 0;', GARB),
    ('a-face-needs-no-source', CASCADE,
     '    if (face.family == NULL || face.nsrc == 0)\n        return;',
     '    if (face.family == NULL)\n        return;', GARB),
    ('faces-are-uncapped', CASCADE,
     '    if (c->nfaces == GARB_FONT_FACES_MAX ||',
     '    if (false ||', GARB),
    ('a-face-inside-failed-media-is-one', CASCADE,
     '        if (!group || !r->has_block)\n            continue;',
     '        if (((void)group, false) || !r->has_block)\n            continue;', GARB),
    # libgzip: the zlib door.
    ('the-window-is-unchecked', ZLIB,
     ' || (stream->header[0] >> 4) > 7 ||',
     ' ||', GZIP),
    ('the-check-bits-are-unchecked', ZLIB,
     '                    ((uint32_t)stream->header[0] << 8 | stream->header[1]) % 31u != 0)',
     '                    false)', GZIP),
    ('a-dictionary-is-read-past', ZLIB,
     '                if ((stream->header[1] & 0x20u) != 0)',
     '                if (false)', GZIP),
    ('the-adler-is-unchecked', ZLIB,
     '                    return refuse(stream, OS64_ZLIB_CHECKSUM);',
     '                    (void)0;', GZIP),
    ('adler-starts-at-zero', ZLIB,
     '    stream->adler_first = 1;',
     '    stream->adler_first = 0;', GZIP),
    ('a-short-trailer-waits', ZLIB,
     '                stream->trailer[stream->trailer_have++] = *(*input)++;',
     '                if (end_of_input && *input_length < 4u - stream->trailer_have)\n'
     '                    return OS64_ZLIB_NEED_INPUT;\n'
     '                stream->trailer[stream->trailer_have++] = *(*input)++;', GZIP),
    # libpng on the door.
    ('png-bytes-after-the-checksum', PNG,
     '                if (zlib_done)\n                    return OS64_PNG_MALFORMED;',
     '                if (zlib_done)\n                    break;', PNGS),
    ('png-checksum-is-malformed', PNG,
     '        case OS64_ZLIB_CHECKSUM: return OS64_PNG_CHECKSUM;',
     '        case OS64_ZLIB_CHECKSUM: return OS64_PNG_MALFORMED;', PNGS),
    ('png-unfinished-is-whole', PNG,
     '    return zlib_done ? OS64_PNG_OK : OS64_PNG_MALFORMED;',
     '    return OS64_PNG_OK;', PNGS),
    # yonder: the WOFF unwrapper.
    ('tags-in-any-order', UNWRAP,
     '        if ((i > 0 && t[i].tag <= t[i - 1].tag) || t[i].offset % 4 != 0 || t[i].offset < first ||',
     '        if (t[i].offset % 4 != 0 || t[i].offset < first ||', UNWRAPS),
    ('tables-unaligned', UNWRAP,
     ' || t[i].offset % 4 != 0 || t[i].offset < first ||',
     ' || t[i].offset < first ||', UNWRAPS),
    ('tables-overlap', UNWRAP,
     '                st = YONDER_FONT_MALFORMED;\n    }\n    uint8_t *out',
     '                (void)0;\n    }\n    uint8_t *out', UNWRAPS),
    ('the-total-is-unchecked', UNWRAP,
     '    if (st == YONDER_FONT_OK && need != total)',
     '    if (false && need != total)', UNWRAPS),
    ('the-expansion-is-unlimited', UNWRAP,
     ' || (uint64_t)total > (uint64_t)YONDER_FONT_EXPANSION_MAX * len)',
     ')', UNWRAPS),
    ('the-size-is-unlimited', UNWRAP,
     '    if (total > YONDER_FONT_SFNT_MAX ||',
     '    if (false ||', UNWRAPS),
    ('a-table-ends-anywhere', UNWRAP,
     '    return st == OS64_ZLIB_DONE && in_left == 0 && out_left == 0 ? YONDER_FONT_OK',
     '    return st == OS64_ZLIB_DONE && out_left == 0 ? YONDER_FONT_OK', UNWRAPS),
    ('metadata-unchecked', UNWRAP,
     '        (meta_length != 0 && (uint64_t)meta_offset + meta_length > len) ||',
     '        ((void)meta_offset, (void)meta_length, false) ||', UNWRAPS),
    ('private-unchecked', UNWRAP,
     '        (priv_length != 0 && (uint64_t)priv_offset + priv_length > len))',
     '        ((void)priv_offset, (void)priv_length, false))', UNWRAPS),
    ('the-length-is-unchecked', UNWRAP,
     '    if (length != len || ntables == 0 || reserved != 0 ||',
     '    if ((void)length, ntables == 0 || reserved != 0 ||', UNWRAPS),
    ('reserved-is-unchecked', UNWRAP,
     ' || ntables == 0 || reserved != 0 ||',
     ' || ntables == 0 || ((void)reserved, false) ||', UNWRAPS),
    ('the-search-range-is-wrong', UNWRAP,
     '    put16(out + 6, (uint16_t)(power * SFNT_ENTRY));',
     '    put16(out + 6, (uint16_t)(ntables * SFNT_ENTRY));', UNWRAPS),
    ('the-directory-in-data-order', UNWRAP,
     '            uint8_t *e = out + SFNT_HEADER + (size_t)SFNT_ENTRY * order[i];',
     '            uint8_t *e = out + SFNT_HEADER + (size_t)SFNT_ENTRY * i;', UNWRAPS),
]

caught, missed, unbuilt = [], [], []
for name, rel, old, new, cases in mutants:
    if args.only and not any(o in name for o in args.only.split(',')):
        continue
    path = root / rel
    original = path.read_bytes()
    text = original.decode()
    if text.count(old) != 1:
        print(f'{name}: the text to break is not found once in {rel}', file=sys.stderr)
        unbuilt.append(name)
        continue
    log = args.logs / f'{name}.log'
    try:
        path.write_text(text.replace(old, new))
        with log.open('w') as out:
            try:
                result = subprocess.run(['bash', '-c', cases], cwd=root, stdout=out, stderr=out,
                                        timeout=1800)
                code = result.returncode
            except subprocess.TimeoutExpired:
                code = 'timeout'
    finally:
        path.write_bytes(original)
        if path.read_bytes() != original:
            print(f'{name}: {rel} WAS NOT RESTORED', file=sys.stderr)
            sys.exit(2)
    body = log.read_text(errors='replace')
    # A compiler's error, not a sanitizer's "runtime error:", which is a catch.
    built = re.search(r'\.(c|h|inc):\d+:\d+: error:', body) is None and 'undefined reference' not in body
    if not built:
        unbuilt.append(name)
        verdict = 'did not build'
    elif code != 0:
        caught.append(name)
        verdict = 'caught'
    else:
        missed.append(name)
        verdict = 'MISSED'
    print(f'{name}: {verdict}', flush=True)

print(f'\nfonts mutants: {len(caught)} caught, {len(missed)} missed, {len(unbuilt)} did not build')
if missed:
    print('missed: ' + ', '.join(missed))
if unbuilt:
    print('did not build: ' + ', '.join(unbuilt))
sys.exit(1 if missed or unbuilt else 0)
