#!/usr/bin/env python3
"""Refresh the libhtml reference inputs at explicitly selected upstream commits."""
import argparse
import hashlib
import json
import shutil
import tempfile
from pathlib import Path
import urllib.request

ROOT = Path(__file__).resolve().parent / 'html5lib-tests'
PINS = {
    'tokenizer': ('html5lib/html5lib-tests', 'a9f44960a9fedf265093d22b2aa3c7ca123727b9'),
    'tree': ('html5lib/html5lib-tests', 'a9f44960a9fedf265093d22b2aa3c7ca123727b9'),
    'standard': ('whatwg/html', 'e981fc31912af57267fad15222f0add44625e87a'),
}

def get(url):
    with urllib.request.urlopen(url, timeout=90) as response:
        return response.read()

def main():
    ap = argparse.ArgumentParser(description=__doc__)
    for key, (_, commit) in PINS.items():
        ap.add_argument('--' + key, default=commit)
    ap.add_argument('--entities-sha256', default='d741d877ac77c4194c4ad526b5b4a19aef8dfe411ab840a466891cdbb9f362e6',
                    help='expected entities.json digest; an upstream change requires an explicit new digest')
    args = ap.parse_args()
    ROOT.parent.mkdir(parents=True, exist_ok=True)
    staging = tempfile.TemporaryDirectory(prefix='html-import-', dir=ROOT.parent)
    destination = Path(staging.name)
    manifest = {'sources': {}, 'files': {}}
    def save(path, data):
        dest = destination / path
        dest.parent.mkdir(parents=True, exist_ok=True)
        dest.write_bytes(data)
        manifest['files'][path] = hashlib.sha256(data).hexdigest()
    for key, (repo, _) in PINS.items():
        commit = getattr(args, key)
        manifest['sources'][key] = {'repository': repo, 'commit': commit}
        raw = f'https://raw.githubusercontent.com/{repo}/{commit}/'
        if key == 'standard':
            save('standard/source', get(raw + 'source'))
            save('standard/LICENSE', get(raw + 'LICENSE'))
            continue
        folder = 'tokenizer' if key == 'tokenizer' else 'tree-construction'
        entries = json.loads(get(f'https://api.github.com/repos/{repo}/contents/{folder}?ref={commit}'))
        for entry in entries:
            if entry['type'] == 'file' and (entry['name'].endswith(('.test', '.dat')) or entry['name'].startswith('README')):
                save(key + '/' + entry['name'], get(raw + entry['path']))
        save(key + '/LICENSE', get(raw + 'LICENSE'))
    entities = get('https://html.spec.whatwg.org/entities.json')
    if hashlib.sha256(entities).hexdigest() != args.entities_sha256:
        raise SystemExit('entities.json changed; inspect it and explicitly select its new digest')
    save('standard/entities.json', entities)
    (destination / 'PINNED.json').write_text(json.dumps(manifest, indent=2, sort_keys=True) + '\n')
    import html_reference
    html_reference.ROOT = destination
    counts, skips = html_reference.inventory()
    (destination / 'INVENTORY.json').write_text(json.dumps(counts, indent=2, sort_keys=True) + '\n')
    (destination / 'SKIPS.tsv').write_text('\n'.join(skips) + '\n')
    # Publish only after downloads and inventory succeed. Replace the source
    # directories so a revision that removes a fixture cannot leave it stale.
    ROOT.mkdir(exist_ok=True)
    for name in ('tokenizer', 'tree', 'standard'):
        if (ROOT / name).exists():
            shutil.rmtree(ROOT / name)
        shutil.move(str(destination / name), ROOT / name)
    for name in ('PINNED.json', 'INVENTORY.json', 'SKIPS.tsv'):
        (destination / name).replace(ROOT / name)
    staging.cleanup()
    print(f'Saved {len(manifest["files"])} files; revisions and SHA-256 hashes in {ROOT}/PINNED.json')

if __name__ == '__main__':
    main()
