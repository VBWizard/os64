#!/usr/bin/env python3
"""Compare Yonder's real shape adapter fixtures with Chromium and shifted controls."""
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
    if 'SVG shapes: 19 exact shape/path pairs' not in (source/'results.txt').read_text():
        raise ValueError('native shape suite did not complete')
    provenance = json.loads((source/'provenance.json').read_text())
    if output.exists() and any(output.iterdir()):
        parser.error('use an empty output directory')
    pages = output/'pages'
    pages.mkdir(parents=True)
    for page in source.glob('shape-*.html'):
        (pages/page.name).write_bytes(page.read_bytes())
    records = []
    with Capture(pages, width=200, height=100) as browser:
        for i in range(19):
            base = f'shape-{i:02}'
            native = output/(base+'-native.png')
            with Image.open(source/(base+'.ppm')) as im:
                if im.mode != 'RGB' or im.size != (200, 100):
                    raise ValueError('unexpected native image')
                im.save(native)
            # Straight, integral boundaries are exact. Curved/diagonal outlines
            # allow bounded edge variation, capped again by integrated error.
            exact = i in (0, 3, 12, 13, 14, 15, 16, 17)
            maximum, pixels, area = (0, 0, 0) if exact else (64, 500, 16)
            # Radius-20 circle edges differ by up to 65/255 with this pinned
            # backend/browser pair (under 8.2 integrated pixel equivalents).
            if i in (4, 5, 6):
                maximum, pixels, area = 72, 160, 9
            # Half-opacity compositing differs by one level over the interior,
            # plus curved-edge coverage; bound the aggregate error as well.
            if i == 18:
                maximum, pixels, area = 32, 2000, 11
            limit = Fuzzy((0, maximum), (0, pixels))
            shape = pages/(base+'-shape.html')
            wrong = pages/(base+'-shifted.html')
            wrong.write_text(shape.read_text().replace("<svg id=art", "<svg style='transform:translateX(3px)' id=art"))
            for kind in ('shape', 'path', 'shifted'):
                page = pages/(base+'-'+kind+'.html')
                shot = output/(base+'-'+kind+'.png')
                browser.screenshot('/'+page.name, shot)
                result = compare(native, shot, fuzzy=limit,
                                 diff_path=output/(base+'-'+kind+'-diff.png'))
                result['pass'] &= result['sumMaxChannelDifference'] <= area*255
                expected = kind != 'shifted' or i in (15, 16)
                result.update(case=i, kind=kind, expected_match=expected,
                              maxPixelEquivalents=area, source_sha256=digest(page),
                              native_sha256=digest(native), browser_sha256=digest(shot))
                result['as_expected'] = result['pass'] == expected
                records.append(result)
        report = {'browser':browser.provenance, 'native':provenance, 'results':records}
    (output/'report.json').write_text(json.dumps(report,indent=2)+'\n')
    bad = [r for r in records if not r['as_expected']]
    print(f'{len(records)-len(bad)}/{len(records)} shape comparisons behaved as expected')
    for r in bad:
        print(r)
    raise SystemExit(bool(bad))


if __name__ == '__main__':
    main()
