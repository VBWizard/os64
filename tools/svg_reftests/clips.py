#!/usr/bin/env python3
"""Compare Yonder's clip adapter fixtures with Chromium and shifted controls."""
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
    if 'SVG clips: 22 exact references' not in (source/'results.txt').read_text():
        raise ValueError('native clip suite did not complete')
    provenance = json.loads((source/'provenance.json').read_text())
    if output.exists() and any(output.iterdir()):
        parser.error('use an empty output directory')
    pages = output/'pages'
    pages.mkdir(parents=True)
    for page in source.glob('clip-*.html'):
        (pages/page.name).write_bytes(page.read_bytes())
    records = []
    with Capture(pages, width=128, height=128) as browser:
        for i in range(22):
            base = f'clip-{i:02}'
            native = output/(base+'-native.png')
            with Image.open(source/(base+'.ppm')) as im:
                if im.mode != 'RGB' or im.size != (128, 128):
                    raise ValueError('unexpected native image')
                im.save(native)
            area = 0
            limit = Fuzzy((0, 0), (0, 0))
            # The radius-32 circle uses the same native geometry for mask and
            # flat fill. Bound its measured browser edge difference separately.
            if i == 1:
                limit, area = Fuzzy((0, 64), (0, 256)), 18
            shape = pages/(base+'-actual.html')
            wrong = pages/(base+'-shifted.html')
            wrong.write_text(shape.read_text().replace("<svg id=art", "<svg style='transform:translateX(3px)' id=art"))
            for kind in ('actual', 'reference', 'shifted'):
                page = pages/(base+'-'+kind+'.html')
                shot = output/(base+'-'+kind+'.png')
                browser.screenshot('/'+page.name, shot)
                current_limit, current_area = limit, area
                # Chromium rounds this isolated half-opacity group one level
                # lower than the flattened reference throughout its rectangle.
                if i == 9 and kind in ('actual', 'shifted'):
                    current_limit, current_area = Fuzzy((0, 1), (0, 1600)), 7
                result = compare(native, shot, fuzzy=current_limit,
                                 diff_path=output/(base+'-'+kind+'-diff.png'))
                result['pass'] &= result['sumMaxChannelDifference'] <= current_area*255
                known_difference = i == 13 and kind == 'actual'
                expected = kind != 'shifted' and not known_difference
                result.update(case=i, kind=kind, expected_match=expected,
                              maxPixelEquivalents=current_area, source_sha256=digest(page),
                              native_sha256=digest(native), browser_sha256=digest(shot))
                result['as_expected'] = result['pass'] == expected
                if known_difference:
                    # Masking 1 makes display:none clip resources available.
                    # Chromium 140 ignores this reference and paints the whole rect.
                    result['known_difference'] = 'display:none clip resource ignored by Chromium 140'
                    result['as_expected'] &= (result['maxDifference'], result['totalPixels'],
                                              result['sumMaxChannelDifference']) == (255, 16000, 4080000)
                records.append(result)
        report = {'browser':browser.provenance, 'native':provenance, 'results':records}
    (output/'report.json').write_text(json.dumps(report,indent=2)+'\n')
    bad = [r for r in records if not r['as_expected']]
    print(f'{len(records)-len(bad)}/{len(records)} clip comparisons behaved as expected')
    for r in bad:
        print(r)
    raise SystemExit(bool(bad))


if __name__ == '__main__':
    main()
