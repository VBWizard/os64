#!/usr/bin/env python3
"""Compare standalone SVG pixels with Chromium image documents and flat references."""
import argparse
import hashlib
import json
from pathlib import Path

from PIL import Image
from capture import Capture
from compare import Fuzzy, compare


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--native', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    source, output = args.native.resolve(), args.output.resolve()
    if 'SVG documents: 20 exact standalone/inline references pass' not in (source/'results.txt').read_text():
        raise ValueError('native standalone SVG suite did not complete')
    if output.exists() and any(output.iterdir()):
        parser.error('use an empty output directory')
    pages = output/'pages'
    pages.mkdir(parents=True)
    for pattern in ('document-*.svg', 'document-*-reference.html'):
        for page in source.glob(pattern):
            (pages/page.name).write_bytes(page.read_bytes())
    provenance = json.loads((source/'provenance.json').read_text())
    records = []
    with Capture(pages, width=128, height=128) as browser:
        for i in range(20):
            base = f'document-{i:02}'
            native = output/(base+'-native.png')
            with Image.open(source/(base+'.ppm')) as im:
                if im.mode != 'RGB' or im.size != (128, 128):
                    raise ValueError('unexpected native image')
                im.save(native)
            # Host CSS has no access to the separate SVG image document.
            html = ("<!doctype html><body style='margin:0'><style>rect{fill:red!important}</style>"
                    f"<img width=128 height=128 src='{base}.svg'>")
            (pages/(base+'-actual.html')).write_text(html)
            (pages/(base+'-shifted.html')).write_text(html.replace('<img ', "<img style='transform:translateX(3px)' "))
            for kind in ('actual', 'reference', 'shifted'):
                page = pages/(base+'-'+kind+'.html')
                shot = output/(base+'-'+kind+'.png')
                browser.screenshot('/'+page.name, shot)
                limit, area = Fuzzy((0, 0), (0, 0)), 0
                # Chromium's isolated root opacity rounds one channel level
                # differently over this 64x64 rectangle.
                if i == 4 and kind != 'reference':
                    limit, area = Fuzzy((0, 1), (0, 4096)), 17
                result = compare(native, shot, fuzzy=limit,
                                 diff_path=output/(base+'-'+kind+'-diff.png'))
                result['pass'] &= result['sumMaxChannelDifference'] <= area*255
                known_difference = i == 9 and kind == 'actual'
                expected = kind != 'shifted' and not known_difference
                result.update(case=i, kind=kind, expected_match=expected,
                              maxPixelEquivalents=area, source_sha256=digest(page),
                              svg_sha256=digest(pages/(base+'.svg')),
                              native_sha256=digest(native), browser_sha256=digest(shot))
                result['as_expected'] = result['pass'] == expected
                if known_difference:
                    result['known_difference'] = 'secure-static native profile ignores SMIL set at 0s; Chromium image mode animates'
                    result['as_expected'] &= (result['maxDifference'], result['totalPixels'],
                                              result['sumMaxChannelDifference']) == (255, 3072, 783360)
                records.append(result)
        report = {'browser': browser.provenance, 'native': provenance, 'results': records}
    (output/'report.json').write_text(json.dumps(report, indent=2)+'\n')
    bad = [r for r in records if not r['as_expected']]
    print(f'{len(records)-len(bad)}/{len(records)} standalone image comparisons behaved as expected')
    for r in bad:
        print(r)
    raise SystemExit(bool(bad))


if __name__ == '__main__':
    main()
