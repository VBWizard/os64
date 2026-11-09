#!/usr/bin/env python3
"""Compare public-backend masks against separately captured browser SVG images."""
import argparse
import base64
import html
import json
from pathlib import Path
from PIL import Image
from compare import Fuzzy, compare
from run import digest

# Cross-engine fixture allowances, separate from each page's WPT fuzzy data.
# The analytic oracle remains a separate check in the backend harness.
LIMITS = {'cap': (40, 200, 12), 'non-scaling': (40, 100, 4), 'evenodd': (0, 0, 0)}


def mask(path):
    with Image.open(path) as image:
        if image.mode != 'L' or image.size != (192, 128):
            raise ValueError('unexpected native coverage format')
        return image.tobytes()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--native', type=Path, required=True)
    parser.add_argument('--browser', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    if args.output.exists() and any(args.output.iterdir()):
        parser.error('use an empty output directory')
    args.output.mkdir(parents=True, exist_ok=True)
    browser = json.loads((args.browser/'results.json').read_text())
    if (not browser['success'] or browser['environment']['viewport'] != [192,128]
            or browser['environment']['deviceScaleFactor'] != 1):
        raise ValueError('native comparisons require a successful 192x128, DPR 1 browser suite')
    provenance = json.loads((args.native/'provenance.json').read_text())
    results, panels = [], []
    for shape in LIMITS:
        source = args.native/(shape+'.pgm')
        pixels = mask(source)
        variants = [(shape, pixels, 'fixture', source)]
        if shape == 'cap':
            wrong = args.native/'non-scaling.pgm'
            variants += [('missing-stroke',bytes(len(pixels)),'negative-control',None),
                         ('wrong-transform',mask(wrong),'negative-control',wrong)]
        if shape == 'evenodd':
            wrong = args.native/'nonzero.pgm'
            variants.append(('wrong-fill-rule',mask(wrong),'negative-control',wrong))
        for ground, background in [('light', (242,245,250)), ('dark', (20,26,38))]:
            url = f'/{shape}-{ground}.html'
            capture = browser['captures'][url]
            reference = args.browser/capture['file']
            if digest(reference) != capture['sha256']:
                raise ValueError('browser capture digest changed')
            for label, coverage, kind, source in variants:
                rgb = bytes((f*a+b*(255-a)+127)//255 for a in coverage
                            for f, b in zip((32,145,222), background))
                image = args.output/(label+'-'+ground+'.png')
                Image.frombytes('RGB',(192,128),rgb).save(image)
                maximum, count, area = LIMITS[shape]
                result = compare(image, reference, fuzzy=Fuzzy((0,maximum),(0,count)),
                    diff_path=args.output/(label+'-'+ground+'-diff.png'))
                # WPT itself uses two metrics. This extra native-only bound also
                # prevents many individually small errors accumulating unnoticed.
                result['pass'] &= result['sumMaxChannelDifference'] <= area*255
                result.update(shape=shape, label=label, background=ground, kind=kind,
                    maxPixelEquivalents=area, mask_sha256=digest(source) if source else None,
                    browser_sha256=capture['sha256'])
                result['accepted'] = result['pass'] if kind == 'fixture' else not result['pass']
                results.append(result)
                def inline(path):
                    encoded = base64.b64encode(path.read_bytes()).decode()
                    return '<img width="192" height="128" src="data:image/png;base64,'+encoded+'">'
                panels.append('<section><h2>'+html.escape(label+' / '+ground)+' — '+
                    ('match accepted' if result['pass'] else 'difference rejected')+'</h2>'+inline(image)+
                    inline(reference)+inline(args.output/(label+'-'+ground+'-diff.png'))+
                    '<p>Maximum channel difference '+str(result['maxDifference'])+'; changed pixels '+
                    str(result['totalPixels'])+'; '+html.escape(kind)+'</p></section>')
                print(label, ground, result['pass'], result['maxDifference'], result['totalPixels'])
    (args.output/'results.json').write_text(json.dumps({'environment':browser['environment'],
        'browser_report_sha256':digest(args.browser/'results.json'),
        'native_provenance':provenance,'comparisons':results},indent=2)+'\n')
    (args.output/'index.html').write_text('<!doctype html><html lang="en"><meta charset="utf-8">'
        '<title>Native geometry and Chromium</title><style>body{font:16px system-ui;background:#17202d;'
        'color:#eee;margin:32px auto;max-width:1100px}h2{font-size:18px}section{padding:16px;margin:16px 0;'
        'background:#233047}img{margin-right:8px}</style><h1>Native geometry and Chromium</h1>'
        '<p>Each row: native mask composited over the declared background, browser SVG, raw RGB difference. '
        'Negative controls are deliberately wrong. This is geometry evidence, not Yonder SVG support.</p>'+
        '<p>Chromium '+html.escape(browser['environment']['version'])+', viewport 192×128, device scale 1.</p>'+
        ''.join(panels)+'</html>')
    return 0 if all(r['accepted'] for r in results) else 1


if __name__ == '__main__':
    raise SystemExit(main())
