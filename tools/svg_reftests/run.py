#!/usr/bin/env python3
"""Capture selected local reftests, compare them, and retain evidence."""
import argparse
import hashlib
import html
import json
from pathlib import Path

from capture import Capture
from compare import Unsupported, compare, file_for, metadata, verdict

HERE = Path(__file__).resolve().parent


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def name(url):
    return hashlib.sha256(url.encode()).hexdigest()[:20]


def run(root, suite, output):
    if output.is_relative_to(root):
        raise ValueError('output must be outside the fixture root')
    if output.exists() and any(output.iterdir()):
        raise ValueError('use an empty output directory to avoid stale evidence')
    output.mkdir(parents=True, exist_ok=True)
    spec = json.loads(suite.read_text())
    width, height = spec.get('viewport', [800, 600])
    if not all(type(n) is int and 0 < n <= 4096 for n in (width, height)):
        raise ValueError('invalid viewport')
    for source, expected in spec.get('files', {}).items():
        if digest(file_for(root, '/' + source)) != expected:
            raise ValueError(f'pinned source digest mismatch: {source}')
    cases = spec['cases']
    urls = [case['url'] for case in cases]
    if len(set(urls)) != len(urls) or not urls:
        raise ValueError('duplicate or empty suite')
    report = {'suite_sha256': digest(suite), 'captures': {}, 'cases': [], 'sources': {}}
    # Hash dependencies too; the browser can load local resources beside tests.
    for path in sorted(root.rglob('*')):
        if path.is_symlink():
            raise ValueError('fixture symlink is outside the captured source inventory')
        if path.is_file():
            report['sources'][str(path.relative_to(root))] = digest(path)
    with Capture(root, width, height) as browser:
        report['environment'] = browser.provenance
        def capture(url):
            target = output/(name(url)+'.png')
            if url not in report['captures']:
                info = browser.screenshot(url, target)
                info.update(file=target.name, sha256=digest(target))
                report['captures'][url] = info
            return target
        for case in cases:
            url, kind = case['url'], case['kind']
            if kind not in ('fixture', 'negative-control'):
                raise ValueError('unknown case kind')
            row = {'url': url, 'kind': kind, 'comparisons': []}
            try:
                meta = metadata(root, url)
                if not meta.references:
                    raise ValueError('no references')
                for ref, relation in meta.references:
                    if ref == url or metadata(root, ref).references:
                        raise Unsupported('cyclic or chained references require the full WPT runner')
                    diff = output/(name(url+' '+ref+' '+relation)+'-diff.png')
                    result = compare(capture(url), capture(ref), relation, meta.tolerance(ref), diff)
                    result.update(reference=ref, diff=diff.name)
                    row['comparisons'].append(result)
                row['status'] = 'PASS' if verdict(row['comparisons']) else 'FAIL'
            except Unsupported as error:
                row.update(status='UNSUPPORTED', error=str(error))
            except Exception as error:
                row.update(status='ERROR', error=str(error))
            row['expected'] = 'FAIL' if kind == 'negative-control' else 'PASS'
            row['accepted'] = row['status'] == row['expected']
            report['cases'].append(row)
            print(row['status'], url, '(negative control)' if kind == 'negative-control' else '')
    report['success'] = all(row['accepted'] for row in report['cases'])
    (output/'results.json').write_text(json.dumps(report, indent=2)+'\n')
    panels = []
    for row in report['cases']:
        entries = [row['url']] + [r['reference'] for r in row['comparisons']]
        images = ''.join(f'<img src="{report["captures"][url]["file"]}" title="{html.escape(url)}">'
                         for url in entries if url in report['captures'])
        panels.append(f'<section><h2>{html.escape(row["url"])}: {row["status"]}</h2>{images}'
                      f'<pre>{html.escape(json.dumps(row, indent=2))}</pre></section>')
    (output/'index.html').write_text('<!doctype html><meta charset="utf-8"><title>SVG reftests</title>'
        '<style>body{background:#17202d;color:#eee;font:16px system-ui;margin:32px}'
        'img{margin:8px;image-rendering:pixelated}pre{white-space:pre-wrap}h2{font-size:18px}</style>'
        '<h1>Browser reftest harness</h1><p>Browser fixtures and deliberate negative controls. '
        'These results do not measure Yonder SVG support.</p>'+''.join(panels))
    return report['success']


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, default=HERE/'fixtures')
    parser.add_argument('--suite', type=Path, default=HERE/'suite.json')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    try:
        return 0 if run(args.root.resolve(), args.suite, args.output.resolve()) else 1
    except Exception as error:
        parser.exit(2, f'reftest setup error: {error}\n')


if __name__ == '__main__':
    raise SystemExit(main())
