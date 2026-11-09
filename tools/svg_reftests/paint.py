#!/usr/bin/env python3
"""Compare prepared-scene painting with Chromium on light and dark backgrounds."""
import argparse
import base64
import hashlib
import html
import json
from pathlib import Path
import re

from PIL import Image
from capture import Capture
from compare import Fuzzy, compare

NAMES = ['group-opacity', 'element-opacity', 'nested-opacity', 'evenodd', 'nonzero',
         'scaled-round-cap', 'miter-square-cap', 'visible-child', 'low-alpha',
         'sheared-clipped-stroke', 'paint-and-element-opacity', 'arc-circle']
# Cross-engine edge envelopes, measured against the independent analytic
# fixtures as well as Chromium; see docs/design/pending/SVG_PAINT.md.
LIMITS = {5: (40, 200, 12), 6: (24, 250, 4), 9: (24, 400, 6), 11: (64, 200, 12)}


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--native', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    source, output = args.native.resolve(), args.output.resolve()
    if 'assertions passed' not in (source/'results.txt').read_text():
        raise ValueError('native painting suite did not complete successfully')
    if not (source/'provenance.json').is_file():
        raise ValueError('native build provenance is required')
    if output.exists() and any(output.iterdir()):
        parser.error('use an empty output directory')
    output.mkdir(parents=True, exist_ok=True)
    pages = output/'pages'
    pages.mkdir()
    records, panels = [], []
    for page in source.glob('paint-*.html'):
        (pages/page.name).write_bytes(page.read_bytes())

    def negative(text, index):
        if index == 0:
            text = text.replace('opacity="0.5"', 'opacity="1"', 1)
            return re.sub(r'(<path[^>]*?) opacity="1"', r'\1 opacity="0.5"', text)
        if index == 1:
            text = text.replace(' opacity="0.5"', ' opacity="1"')
            return text.replace('fill-opacity="1" stroke-opacity="1"',
                                'fill-opacity="0.5" stroke-opacity="0.5"')
        if index == 3:
            return text.replace('fill-rule="evenodd"', 'fill-rule="nonzero"')
        if index == 5:
            return text.replace('matrix(4 0 0 1 0 8)', 'matrix(1 0 0 1 0 8)')
        if index == 9:
            return re.sub(r'matrix\(1 0\.2[^)]*\)', 'matrix(1 0 0 1 0 0)', text)
        if index == 11:
            return text.replace('matrix(1 0 0 1 0 0)', 'matrix(0.75 0 0 0.75 8 8)', 1)
        return re.sub(r'stroke="rgba\([^"]*\)"', 'stroke="none"', text)

    with Capture(pages, width=64, height=64) as browser:
        for index, label in enumerate(NAMES):
            for theme in range(2):
                basename = f'paint-{index}-{theme}'
                native = output/(basename+'-native.png')
                with Image.open(source/(basename+'.ppm')) as image:
                    if image.size != (64, 64) or image.mode != 'RGB':
                        raise ValueError('unexpected native fixture')
                    image.save(native)
                variants = [('fixture', pages/(basename+'.html'))]
                if index in (0, 1, 3, 5, 6, 9, 11):
                    wrong = pages/(basename+'-wrong.html')
                    original = variants[0][1].read_text()
                    changed = negative(original, index)
                    if changed == original:
                        raise ValueError('negative control did not change its input')
                    wrong.write_text(changed)
                    variants.append(('negative-control', wrong))
                for kind, page in variants:
                    name = basename+('-wrong' if kind != 'fixture' else '')
                    capture, difference = output/(name+'-browser.png'), output/(name+'-diff.png')
                    browser.screenshot('/'+page.name, capture)
                    maximum, count, area = LIMITS.get(index, (1, 4096, 16))
                    if index in (3, 4, 7):
                        maximum, count, area = 0, 0, 0
                    result = compare(native, capture, fuzzy=Fuzzy((0, maximum), (0, count)),
                                     diff_path=difference)
                    result['pass'] &= result['sumMaxChannelDifference'] <= area*255
                    result.update(label=label, background=['light', 'dark'][theme], kind=kind,
                                  maxPixelEquivalents=area, source_sha256=digest(page),
                                  native_sha256=digest(native), browser_sha256=digest(capture))
                    if kind == 'fixture' and index in (6, 9, 11):
                        rgba = (source/f'oracle-{index}.rgba').read_bytes()
                        if len(rgba) != 64*64*4:
                            raise ValueError('bad analytic RGBA fixture')
                        background = [(242,245,250), (20,26,38)][theme]
                        rgb = bytes((rgba[i+j]*rgba[i+3]+b*(255-rgba[i+3])+127)//255
                                    for i in range(0,len(rgba),4) for j,b in enumerate(background))
                        oracle = output/(basename+'-oracle.png')
                        Image.frombytes('RGB',(64,64),rgb).save(oracle)
                        result['analytic_rgba_sha256'] = digest(source/f'oracle-{index}.rgba')
                        result['analytic_native'] = compare(native,oracle,fuzzy=Fuzzy((0,255),(0,4096)))
                        result['analytic_browser'] = compare(capture,oracle,fuzzy=Fuzzy((0,255),(0,4096)))
                    result['accepted'] = result['pass'] if kind == 'fixture' else not result['pass']
                    records.append(result)

                    def inline(path):
                        return '<img width="192" height="192" src="data:image/png;base64,'+base64.b64encode(path.read_bytes()).decode()+'">'

                    panels.append('<section><h2>'+html.escape(label+' / '+result['background'])+
                                  ' — '+('accepted' if result['accepted'] else 'FAILED')+'</h2>'+
                                  inline(native)+inline(capture)+inline(difference)+
                                  '<p>'+html.escape(kind)+f'; maximum channel difference {result["maxDifference"]}; '
                                  f'changed pixels {result["totalPixels"]}</p></section>')
                    print(label, theme, kind, result['accepted'], result['maxDifference'],
                          result['totalPixels'], result['sumMaxChannelDifference'])
        environment = browser.provenance
    metadata = source/'provenance.json'
    report = {'comparison_script_sha256': digest(Path(__file__)), 'environment': environment, 'success': all(r['accepted'] for r in records),
              'native_provenance': json.loads(metadata.read_text()),
              'native_log_sha256': digest(source/'results.txt'), 'comparisons': records}
    (output/'results.json').write_text(json.dumps(report, indent=2)+'\n')
    (output/'index.html').write_text('<!doctype html><html lang="en"><meta charset="utf-8">'
        '<title>SVG solid painting comparison</title><style>body{font:16px system-ui;background:#17202d;'
        'color:#eee;margin:32px auto;max-width:1000px}section{padding:16px;margin:16px 0;background:#233047}'
        'img{margin-right:12px;image-rendering:pixelated}h2{font-size:18px}</style>'
        '<h1>SVG solid painting and Chromium</h1><p>Each row: native libsvg, Chromium SVG, raw RGB difference. '
        '64×64 fixtures enlarged 3×. Negative controls deliberately change the browser input. '
        'These exercise prepared-scene painting, not Yonder integration.</p>'+
        '<p>Chromium '+html.escape(environment['version'])+', device scale 1.</p>'+''.join(panels)+'</html>')
    return 0 if report['success'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
