#!/usr/bin/env python3
"""Compare native SVG default-object dimensions with Chromium image metadata."""
import argparse
import hashlib
import json
from pathlib import Path
from capture import Capture


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--native', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    source, output = args.native.resolve(), args.output.resolve()
    cases = json.loads((source/'image-sizing.json').read_text())
    if output.exists() and any(output.iterdir()):
        parser.error('use an empty output directory')
    pages = output/'pages'
    pages.mkdir(parents=True)
    for case in cases:
        (pages/case['file']).write_bytes((source/case['file']).read_bytes())
    results = []
    with Capture(pages) as capture:
        page = capture.browser.new_page()
        for case in cases:
            page.set_content(f"<img id='image' src='{capture.origin}/{case['file']}'>")
            page.wait_for_function('document.getElementById("image").complete')
            measured = page.evaluate('''() => {
                const image = document.getElementById('image');
                return [image.naturalWidth, image.naturalHeight];
            }''')
            match = measured == [case['width'], case['height']]
            result = {**case, 'browser_size': measured, 'pass': match,
                      'as_expected': match,
                      'sha256': hashlib.sha256((pages/case['file']).read_bytes()).hexdigest()}
            if case['file'] in ('image-size-08.svg', 'image-size-09.svg'):
                result['known_difference'] = ('SVG2 root CSS sizing gives 80x40; pinned Chromium '
                                              'natural dimensions use the 10x20 attributes')
                result['as_expected'] = (not match and measured == [10, 20]
                                         and [case['width'], case['height']] == [80, 40])
            results.append(result)
        page.close()
        report = {'browser': capture.provenance,
                  'native': json.loads((source/'provenance.json').read_text()), 'results': results}
    (output/'report.json').write_text(json.dumps(report, indent=2)+'\n')
    for result in results:
        if not result['as_expected']:
            print(result)
    good = sum(r['as_expected'] for r in results)
    print(f'{good}/{len(results)} SVG image size comparisons behaved as expected')
    raise SystemExit(good != len(results))


if __name__ == '__main__':
    main()
