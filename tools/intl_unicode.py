#!/usr/bin/env python3
"""Generate libjs Intl's segmentation table from pinned Unicode data.

Intl.Segmenter (SEGMENTER.md) finds boundaries by UAX #29, whose rules are
written over a handful of character properties. This writes them, merged
into one range table, for userland/libjs/intl/segment.c:

    Grapheme_Cluster_Break, Word_Break, Sentence_Break   the three rule sets'
    Indic_Conjunct_Break (InCB)                          GB9c's
    Extended_Pictographic                                GB11's and WB3c's
    letter or number (General_Category L* or N*)         isWordLike's

Each range is a start code point and its properties packed into one word;
a code point's properties are those of the last range starting at or below
it. The Unicode version is the engine's own (libunicode.h), so a page's
`\\p{...}` and its Segmenter agree about every character.

The property files are not kept in the tree (DerivedCoreProperties.txt
alone is a megabyte, and the table is all the build needs): --fetch puts
them in .vendor/ucd-<version>/, each checked against its SHA-256 in
tools/unicode/segmentation/PINNED.json, and a file whose hash moved is
refused. Unicode's three *BreakTest.txt files are kept in the tree, pinned
the same way, because every run of the host test reads them.

    tools/intl_unicode.py --fetch    download the pinned files and verify them
    tools/intl_unicode.py            regenerate segment_data.inc
    tools/intl_unicode.py --check    fail if the checked-in table is stale
"""
import argparse
import hashlib
import json
import sys
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PINS = ROOT / 'tools/unicode/segmentation/PINNED.json'
OUT = ROOT / 'userland/libjs/intl/segment_data.inc'

# The values each property may take, in the order segment.c's enums give
# them: the index is what is packed. The first is the default for a code
# point no file lists.
GCB = ['Other', 'CR', 'LF', 'Control', 'Extend', 'ZWJ', 'Regional_Indicator', 'Prepend', 'SpacingMark',
       'L', 'V', 'T', 'LV', 'LVT']
WB = ['Other', 'CR', 'LF', 'Newline', 'Extend', 'ZWJ', 'Regional_Indicator', 'Format', 'Katakana',
      'Hebrew_Letter', 'ALetter', 'Single_Quote', 'Double_Quote', 'MidNumLet', 'MidLetter', 'MidNum',
      'Numeric', 'ExtendNumLet', 'WSegSpace']
SB = ['Other', 'CR', 'LF', 'Extend', 'Sep', 'Format', 'Sp', 'Lower', 'Upper', 'OLetter', 'Numeric',
      'ATerm', 'SContinue', 'STerm', 'Close']
INCB = ['None', 'Linker', 'Consonant', 'Extend']

# Where each value is packed: bits 0-3 GCB, 4-8 WB, 9-12 SB, 13-14 InCB,
# 15 Extended_Pictographic, 16 letter or number. segment.c reads the same.
SHIFT_GCB, SHIFT_WB, SHIFT_SB, SHIFT_INCB, BIT_PICT, BIT_WORDY = 0, 4, 9, 13, 15, 16
assert len(GCB) <= 16 and len(WB) <= 32 and len(SB) <= 16 and len(INCB) <= 4


def pins():
    return json.loads(PINS.read_text())


def vendor(pinned):
    return ROOT / '.vendor' / ('ucd-' + pinned['version'])


def verified(path, digest):
    data = path.read_bytes()
    if hashlib.sha256(data).hexdigest() != digest:
        sys.exit(f'{path}: its SHA-256 is not the pinned one; a new version moves the pin by hand')
    return data.decode('utf-8')


def fetch(pinned):
    base = f"https://www.unicode.org/Public/{pinned['version']}/ucd/"
    folder = vendor(pinned)
    folder.mkdir(parents=True, exist_ok=True)
    for name, entry in pinned['fetched'].items():
        target = folder / Path(name).name
        with urllib.request.urlopen(base + name) as answer:
            target.write_bytes(answer.read())
        verified(target, entry)
        print(f'{name}: fetched and verified')


def ranges_of(text, keep=None, value_field=1):
    """Each (first, last, value) a property file lists."""
    for line in text.splitlines():
        line = line.split('#', 1)[0].strip()
        if not line:
            continue
        fields = [f.strip() for f in line.split(';')]
        if keep is not None and not keep(fields):
            continue
        first, _, last = fields[0].partition('..')
        yield int(first, 16), int(last or first, 16), fields[value_field] if value_field < len(fields) else ''


def table(pinned):
    folder = vendor(pinned)
    read = {Path(name).name: verified(folder / Path(name).name, digest) for name, digest in pinned['fetched'].items()}
    size = 0x110000
    gcb, wb, sb = bytearray(size), bytearray(size), bytearray(size)
    incb, pict, wordy = bytearray(size), bytearray(size), bytearray(size)
    for column, values, name in ((gcb, GCB, 'GraphemeBreakProperty.txt'), (wb, WB, 'WordBreakProperty.txt'),
                                 (sb, SB, 'SentenceBreakProperty.txt')):
        for first, last, value in ranges_of(read[name]):
            column[first:last + 1] = bytes([values.index(value)]) * (last - first + 1)
    for first, last, value in ranges_of(read['DerivedCoreProperties.txt'],
                                        keep=lambda f: len(f) > 2 and f[1] == 'InCB', value_field=2):
        incb[first:last + 1] = bytes([INCB.index(value)]) * (last - first + 1)
    for first, last, _ in ranges_of(read['emoji-data.txt'], keep=lambda f: f[1] == 'Extended_Pictographic'):
        pict[first:last + 1] = b'\1' * (last - first + 1)
    for first, last, value in ranges_of(read['DerivedGeneralCategory.txt']):
        if value[0] in 'LN':
            wordy[first:last + 1] = b'\1' * (last - first + 1)
    rows, previous = [], None
    for cp in range(size):
        packed = (gcb[cp] << SHIFT_GCB | wb[cp] << SHIFT_WB | sb[cp] << SHIFT_SB | incb[cp] << SHIFT_INCB |
                  pict[cp] << BIT_PICT | wordy[cp] << BIT_WORDY)
        if packed != previous:
            rows.append((cp, packed))
            previous = packed
    return rows


def render(pinned, rows):
    lines = [f"/* GENERATED by tools/intl_unicode.py from Unicode {pinned['version']}'s property files",
             " * (tools/unicode/segmentation/PINNED.json). Do not edit; regenerate. */",
             f'#define INTL_SEGMENT_UNICODE "{pinned["version"]}"',
             f'#define INTL_SEGMENT_RANGES {len(rows)}',
             'static const uint32_t kSegmentStarts[INTL_SEGMENT_RANGES] = {']
    for i in range(0, len(rows), 8):
        lines.append('    ' + ', '.join(f'0x{cp:x}' for cp, _ in rows[i:i + 8]) + ',')
    lines.append('};')
    lines.append('static const uint32_t kSegmentProps[INTL_SEGMENT_RANGES] = {')
    for i in range(0, len(rows), 8):
        lines.append('    ' + ', '.join(f'0x{p:x}' for _, p in rows[i:i + 8]) + ',')
    lines.append('};')
    return '\n'.join(lines) + '\n'


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--fetch', action='store_true')
    parser.add_argument('--check', action='store_true')
    args = parser.parse_args()
    pinned = pins()
    if args.fetch:
        fetch(pinned)
        return
    for name, digest in pinned['kept'].items():
        verified(ROOT / 'tools/unicode/segmentation' / name, digest)
    rows = table(pinned)
    text = render(pinned, rows)
    if args.check:
        if OUT.read_text() != text:
            sys.exit(f'{OUT} is stale: run tools/intl_unicode.py')
        print(f'{OUT.name}: current ({len(rows)} ranges)')
        return
    OUT.write_text(text)
    print(f'{OUT.name}: {len(rows)} ranges, {len(rows) * 8} bytes')


if __name__ == '__main__':
    main()
