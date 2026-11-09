#!/usr/bin/env python3
"""Compare Yonder's stroke adapter fixtures with Chromium and shifted controls."""
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
    if 'SVG strokes: 35 cases' not in (source/'results.txt').read_text():
        raise ValueError('native stroke suite did not complete')
    provenance = json.loads((source/'provenance.json').read_text())
    if output.exists() and any(output.iterdir()):
        parser.error('use an empty output directory')
    pages = output/'pages'
    pages.mkdir(parents=True)
    for page in source.glob('stroke-*.html'):
        (pages/page.name).write_bytes(page.read_bytes())
    records = []
    with Capture(pages, width=128, height=128) as browser:
        for i in range(35):
            base = f'stroke-{i:02}'
            native = output/(base+'-native.png')
            with Image.open(source/(base+'.ppm')) as im:
                if im.mode != 'RGB' or im.size != (128, 128):
                    raise ValueError('unexpected native image')
                im.save(native)
            # Integral straight boundaries have no raster tolerance. Curves
            # have separate channel, affected-area and integrated-error bounds.
            exact = i in (0, 1, 2, 3, 4, 5, 6, 7, 22, 23, 24, 25, 26, 27, 28, 29)
            maximum, pixels, area = (0, 0, 0) if exact else (80, 500, 20)
            # Measured curve envelopes for the pinned browser/backend pair.
            # Circle placement also has an independent supersampled analytic
            # gate in test_svg_paint.c; this browser is not its geometry oracle.
            if i == 18:
                maximum, pixels, area = 88, 240, 12
            elif i == 19:
                maximum, pixels, area = 160, 400, 34
            elif i == 20:
                maximum, pixels, area = 120, 220, 17
            limit = Fuzzy((0, maximum), (0, pixels))
            shape = pages/(base+'-actual.html')
            wrong = pages/(base+'-shifted.html')
            wrong.write_text(shape.read_text().replace("<svg id=art", "<svg style='transform:translateX(3px)' id=art"))
            for kind in ('actual', 'reference', 'shifted'):
                page = pages/(base+'-'+kind+'.html')
                shot = output/(base+'-'+kind+'.png')
                browser.screenshot('/'+page.name, shot)
                result = compare(native, shot, fuzzy=limit,
                                 diff_path=output/(base+'-'+kind+'-diff.png'))
                result['pass'] &= result['sumMaxChannelDifference'] <= area*255
                expected = kind != 'shifted' or i in (25, 26)
                result.update(case=i, kind=kind, expected_match=expected,
                              maxPixelEquivalents=area, source_sha256=digest(page),
                              native_sha256=digest(native), browser_sha256=digest(shot))
                result['as_expected'] = result['pass'] == expected
                records.append(result)
        report = {'browser':browser.provenance, 'native':provenance, 'results':records}
    (output/'report.json').write_text(json.dumps(report,indent=2)+'\n')
    bad = [r for r in records if not r['as_expected']]
    print(f'{len(records)-len(bad)}/{len(records)} stroke comparisons behaved as expected')
    for r in bad:
        print(r)
    raise SystemExit(bool(bad))


if __name__ == '__main__':
    main()
