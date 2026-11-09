#!/usr/bin/env python3
"""Compare CSS-wide gradient transforms and negative controls with Chromium."""
import argparse
import json
from pathlib import Path
from PIL import Image
from capture import Capture
from compare import Fuzzy, compare
from gradients import digest


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
        for i in range(14):
            base = f'gradient-transform-{i:02}'
            actual = pages/(base+'.html')
            actual.write_bytes((source/actual.name).read_bytes())
            wrong = pages/(base+'-wrong.html')
            wrong.write_text(actual.read_text().replace('</svg>',
                "<rect x='48' y='32' width='16' height='16' fill='black'/></svg>"))
            native = output/(base+'-native.png')
            with Image.open(source/(base+'.ppm')) as image:
                image.save(native)
            references = [('actual', actual), ('wrong', wrong)]
            if i == 12:
                explicit = pages/(base+'-explicit.html')
                explicit.write_text(actual.read_text().replace(
                    'transform:initial', 'transform:matrix(1,0,0,1,0,0)'))
                references.append(('explicit', explicit))
            for kind, page in references:
                shot = output/(base+'-'+kind+'.png')
                browser.screenshot('/'+page.name, shot)
                result = compare(native, shot, fuzzy=Fuzzy((0, 3), (0, 16384)),
                                 diff_path=output/(base+'-'+kind+'-diff.png'))
                result['pass'] &= result['sumMaxChannelDifference'] <= 32*255
                result.update(case=i, kind=kind,
                    expected_match=kind!='wrong' and not (i==12 and kind=='actual'),
                    maxPixelEquivalents=32, source_sha256=digest(page),
                    native_sha256=digest(native), browser_sha256=digest(shot))
                result['as_expected'] = result['pass'] == result['expected_match']
                if i == 12 and kind == 'actual':
                    result['known_difference'] = (
                        'CSS initial resets transform; Chromium retains the href template transform')
                    result['as_expected'] &= (result['maxDifference'], result['totalPixels'],
                        result['sumMaxChannelDifference']) == (253, 12263, 981477)
                records.append(result)
        report = {'browser': browser.provenance,
                  'native': json.loads((source/'provenance.json').read_text()), 'results': records}
    (output/'report.json').write_text(json.dumps(report, indent=2)+'\n')
    bad = [r for r in records if not r['as_expected']]
    print(f'{len(records)-len(bad)}/{len(records)} gradient-transform outcomes as expected')
    for result in bad:
        print(result)
    raise SystemExit(bool(bad))


if __name__ == '__main__':
    main()
