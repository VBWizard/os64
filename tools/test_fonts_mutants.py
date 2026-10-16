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
    ('tech-is-taken', CASCADE,
     '        if (ieq(v[i].text, v[i].len, "tech"))\n            return false;',
     '        if (ieq(v[i].text, v[i].len, "tech"))\n            continue;', GARB),
    ('two-formats-are-taken', CASCADE,
     '        if (!ieq(v[i].text, v[i].len, "format") || out->format != NULL)',
     '        if (!ieq(v[i].text, v[i].len, "format"))', GARB),
    ('local-takes-what-follows', CASCADE,
     '        if (i + 1 != n)\n            return false;\n        out->kind = GARB_FACE_LOCAL;',
     '        if (false)\n            return false;\n        out->kind = GARB_FACE_LOCAL;', GARB),
    ('bold-is-normal', CASCADE,
     '            w[k++] = 700;',
     '            w[k++] = 400;', GARB),
    ('auto-is-every-weight', CASCADE,
     '            w[0] = 400, w[1] = 400, k = 2;',
     '            w[0] = 1, w[1] = 1000, k = 2;', GARB),
    ('auto-is-refused', CASCADE,
     '        else if (t->kind == GARB_IDENT && ieq(t->text, t->len, "auto") && d->nvalue == 1)',
     '        else if (false)', GARB),
    ('a-system-font-names-a-face', CASCADE,
     '    if (words == 1 && garb_system_font(v[first].text, v[first].len))',
     '    if (false && words == 1 && garb_system_font(v[first].text, v[first].len))', GARB),
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
     '    if (c->nfaces == GARB_FONT_FACES_MAX) {',
     '    if (false) {', GARB),
    ('a-rule-past-the-cap-is-read', CASCADE,
     '    if (c->nfaces == GARB_FONT_FACES_MAX) {\n        c->faces_lost++;\n        return;\n    }\n'
     '    if (!garb_items_of(owner, r->block, r->nblock, &items, &n)) {\n'
     '        c->incomplete = true;\n        c->faces_lost++;\n        return;\n    }\n',
     '    if (!garb_items_of(owner, r->block, r->nblock, &items, &n)) {\n'
     '        c->incomplete = true;\n        c->faces_lost++;\n        return;\n    }\n'
     '    if (c->nfaces == GARB_FONT_FACES_MAX) {\n        c->faces_lost++;\n        return;\n    }\n', GARB),
    ('a-long-family-is-dropped', CASCADE,
     '    char *out = alloc(c, need + 1);',
     '    char *out = need < 256 ? alloc(c, need + 1) : NULL;', GARB),
    ('a-long-quoted-family-is-dropped', CASCADE,
     '        return i + 1 == n ? copy_text(c, v[i].text, v[i].len) : NULL;',
     '        return i + 1 == n && v[i].len < 256 ? copy_text(c, v[i].text, v[i].len) : NULL;', GARB),
    ('format-takes-any-keyword', CASCADE,
     '            if (ieq(v[i].text, v[i].len, kFormats[k]))',
     '            if (true)', GARB),
    ('format-ends-on-a-comma', CASCADE,
     '    return want_string ? NULL : first;',
     '    return first;', GARB),
    ('format-strings-need-no-commas', CASCADE,
     '        if (want_string ? v[i].kind != GARB_STRING : v[i].kind != GARB_COMMA)',
     '        if (v[i].kind != GARB_STRING && v[i].kind != GARB_COMMA)', GARB),
    ('a-face-skips-each-name', CASCADE,
     '            if (name != NULL)\n                face.family = name, face.family_len = len;',
     '            if (name != NULL)\n                face.family = name, face.family_len = len, '
     'skipped(c, GARB_SKIP_FONT, name, len);', GARB),
    ('a-face-inside-failed-media-is-one', CASCADE,
     '        if (!group || !r->has_block)\n            continue;',
     '        if (((void)group, false) || !r->has_block)\n            continue;', GARB),
    ('a-repeat-replaces-the-face', CASCADE,
     '    if (!grow(c, (void **)&c->faces, c->nfaces, &c->facecap, sizeof(garb_font_face_t))) {',
     '    for (int32_t i = 0; i < c->nfaces; i++)\n'
     '        if (c->faces[i].family_len == face.family_len && c->faces[i].weight_min == face.weight_min &&\n'
     '            c->faces[i].style == face.style && os64_memcmp(c->faces[i].family, face.family, face.family_len) == 0) {\n'
     '            c->faces[i] = face;\n'
     '            return;\n'
     '        }\n'
     '    if (!grow(c, (void **)&c->faces, c->nfaces, &c->facecap, sizeof(garb_font_face_t))) {', GARB),
    ('a-generic-names-a-face', CASCADE,
     '    if (words == 1 && garb_generic_family(v[first].text, v[first].len))',
     '    if (false && words == 1 && garb_generic_family(v[first].text, v[first].len))', GARB),
    ('a-css-wide-keyword-names-a-face', CASCADE,
     '        if (v[k].kind != GARB_IDENT || garb_family_word_reserved(v[k].text, v[k].len))',
     '        if (v[k].kind != GARB_IDENT || (k == first && garb_family_word_reserved(v[k].text, v[k].len)))', GARB),
    ('default-names-a-family', 'userland/libgarb/props.c',
     '    return ieq(text, len, "default");',
     '    return false;', GARB),
    ('a-prelude-is-ignored', CASCADE,
     '        if (r->prelude[i].kind != GARB_WHITESPACE)\n            return;',
     '        if (false && r->prelude[i].kind != GARB_WHITESPACE)\n            return;', GARB),
    ('an-escaped-range-counts', CASCADE,
     '        if (t->kind == GARB_WHITESPACE || t->escaped || len + need >= sizeof(text) ||',
     '        if (t->kind == GARB_WHITESPACE || len + need >= sizeof(text) ||', GARB),
    ('escapes-go-unmarked', 'userland/libgarb/tokenize.c',
     '    t.v.escaped = tz->escaped;',
     '    t.v.escaped = false;', GARB),
    ('style-auto-is-refused', CASCADE,
     '    if ((ieq(t->text, t->len, "normal") || ieq(t->text, t->len, "auto")) && i + 1 == n) {',
     '    if (ieq(t->text, t->len, "normal") && i + 1 == n) {', GARB),
    ('weights-keep-their-order', CASCADE,
     '    *lo = (float)(w[0] < w[1] ? w[0] : w[1]);',
     '    *lo = (float)w[0];', GARB),
    ('weights-are-whole', CASCADE,
     '    *hi = (float)(w[0] < w[1] ? w[1] : w[0]);',
     '    *hi = (float)(int)(w[0] < w[1] ? w[1] : w[0]);', GARB),
    ('oblique-is-italic', CASCADE,
     '    *out = GARB_FACE_OBLIQUE;\n    return true;',
     '    *out = GARB_FACE_ITALIC;\n    return true;', GARB),
    ('oblique-takes-anything', CASCADE,
     '        if (++angles > 2 || !angle_deg(&d->value[i], &deg) || deg < -90 || deg > 90)',
     '        if (false && (++angles > 2 || !angle_deg(&d->value[i], &deg) || deg < -90 || deg > 90))', GARB),
    ('an-oblique-angle-past-90', CASCADE,
     '        if (++angles > 2 || !angle_deg(&d->value[i], &deg) || deg < -90 || deg > 90)',
     '        if (++angles > 2 || !angle_deg(&d->value[i], &deg))', GARB),
    ('three-oblique-angles', CASCADE,
     '        if (++angles > 2 || !angle_deg(&d->value[i], &deg) || deg < -90 || deg > 90)',
     '        if (++angles > 3 || !angle_deg(&d->value[i], &deg) || deg < -90 || deg > 90)', GARB),
    ('turns-are-degrees', CASCADE,
     '{"turn", 360}};',
     '{"turn", 1}};', GARB),
    ('a-half-read-face-is-kept', CASCADE,
     '    if (c->allocs_failed != failed) {\n        c->faces_lost++;\n        return;\n    }',
     '    (void)failed;', GARB),
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
     '        if (t[i].tag <= (i > 0 ? t[i - 1].tag : 0) || t[i].comp > t[i].orig)',
     '        if (t[i].comp > t[i].orig)', UNWRAPS),
    ('a-zero-first-tag', UNWRAP,
     '        if (t[i].tag <= (i > 0 ? t[i - 1].tag : 0) || t[i].comp > t[i].orig)',
     '        if ((i > 0 && t[i].tag <= t[i - 1].tag) || t[i].comp > t[i].orig)', UNWRAPS),
    ('a-container-of-its-directory', UNWRAP,
     '        WOFF_HEADER + (uint64_t)WOFF_ENTRY * ntables >= len ||',
     '        WOFF_HEADER + (uint64_t)WOFF_ENTRY * ntables > len ||', UNWRAPS),
    ('an-sfnt-of-its-directory', UNWRAP,
     '        SFNT_HEADER + (uint64_t)SFNT_ENTRY * ntables >= total ||',
     '        false ||', UNWRAPS),
    ('metadata-from-nothing', UNWRAP,
     '        (meta_length != 0 && meta_orig == 0) ||',
     '        false ||', UNWRAPS),
    ('too-many-tables', UNWRAP,
     ' || ntables > 0xFFF || reserved != 0 ||',
     ' || reserved != 0 ||', UNWRAPS),
    ('any-flavor', UNWRAP,
     '        yonder_font_sniff(flavor_bytes, 4) != YONDER_FONT_SFNT ||',
     '        (yonder_font_sniff(flavor_bytes, 4), false) ||', UNWRAPS),
    ('a-gap-between-tables', UNWRAP,
     '            if (x->offset != at)',
     '            if (x->offset < at)', UNWRAPS),
    ('bytes-after-the-last-part', UNWRAP,
     '        if (st == YONDER_FONT_OK && at != len)',
     '        if (st == YONDER_FONT_OK && at > len)', UNWRAPS),
    ('metadata-anywhere', UNWRAP,
     '            if (meta_offset != at)',
     '            if (false)', UNWRAPS),
    ('private-unaligned', UNWRAP,
     '            at = (at + 3) & ~(uint64_t)3;\n            if (priv_offset != at)',
     '            if (priv_offset != at)', UNWRAPS),
    ('lengths-without-offsets', UNWRAP,
     '        (meta_offset == 0 && (meta_length != 0 || meta_orig != 0)) ||',
     '        false ||', UNWRAPS),
    ('a-private-length-without-an-offset', UNWRAP,
     '        (priv_offset == 0 && priv_length != 0))',
     '        false)', UNWRAPS),
    ('the-length-is-unchecked', UNWRAP,
     '    if (length != len || ntables == 0 ||',
     '    if ((void)length, ntables == 0 ||', UNWRAPS),
    ('reserved-is-unchecked', UNWRAP,
     ' || ntables > 0xFFF || reserved != 0 ||',
     ' || ntables > 0xFFF || ((void)reserved, false) ||', UNWRAPS),
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
