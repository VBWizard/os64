#!/usr/bin/env python3
"""Compare radial SVG gradient fixtures with pinned Chromium and painted controls."""
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
    if output.exists() and any(output.iterdir()):
        parser.error('use an empty output directory')
    pages = output/'pages'
    pages.mkdir(parents=True)
    records = []
    with Capture(pages, width=128, height=128) as browser:
        for i in range(36):
            base = f'radial-{i:02}'
            actual = pages/(base+'.html')
            actual.write_bytes((source/actual.name).read_bytes())
            wrong = pages/(base+'-wrong.html')
            wrong.write_text(actual.read_text().replace('</svg>',
                "<rect x='48' y='32' width='16' height='16' fill='black'/></svg>"))
            native = output/(base+'-native.png')
            with Image.open(source/(base+'.ppm')) as image:
                if image.size != (128, 128):
                    raise ValueError('unexpected native size')
                image.save(native)
            # Axis-aligned interiors permit small interpolation/rounding error.
            # Case 32 also has fractional transformed edges.
            maximum, pixels, area = (4, 8000, 40) if i != 32 else (96, 8000, 40)
            references = [('actual', actual), ('wrong', wrong)]
            if i in (14, 15):
                flat = pages/(base+'-explicit.html')
                flat.write_text(actual.read_text().replace("fill='url(#g)'", "fill='none'"))
                references.append(('explicit', flat))
            for kind, page in references:
                shot = output/(base+'-'+kind+'.png')
                browser.screenshot('/'+page.name, shot)
                result = compare(native, shot, fuzzy=Fuzzy((0, maximum), (0, pixels)),
                                 diff_path=output/(base+'-'+kind+'-diff.png'))
                result['pass'] &= result['sumMaxChannelDifference'] <= area*255
                result.update(case=i, kind=kind, expected_match=kind!='wrong' and not (kind=='actual' and i in (14, 15)),
                              maxPixelEquivalents=area, source_sha256=digest(page),
                              native_sha256=digest(native), browser_sha256=digest(shot))
                result['as_expected'] = result['pass'] == result['expected_match']
                if kind == 'actual' and i in (14, 15):
                    result['known_difference'] = 'SVG2 identical start/end circles paint nothing; pinned Chromium uses the last stop'
                    result['as_expected'] &= (result['maxDifference'], result['totalPixels'],
                                              result['sumMaxChannelDifference']) == (223, 7680, 1712640)
                records.append(result)
        report = {'browser': browser.provenance,
                  'native': json.loads((source/'provenance.json').read_text()), 'results': records}
    (output/'report.json').write_text(json.dumps(report, indent=2)+'\n')
    bad = [r for r in records if not r['as_expected']]
    print(f'{len(records)-len(bad)}/{len(records)} radial comparisons behaved as expected')
    for result in bad:
        print(result)
    raise SystemExit(bool(bad))


if __name__ == '__main__':
    main()
