#!/usr/bin/env python3
"""Fetch the explicitly selected, hash-pinned WPT sample into an ignored directory."""
import argparse
import hashlib
import json
from pathlib import Path
import re
from urllib.request import urlopen

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=ROOT/'.vendor/svg-reftests')
    args = parser.parse_args()
    spec = json.loads((HERE/'wpt-sample.json').read_text())
    pin = re.search(r'^WPT_COMMIT=(\w+)$', (ROOT/'tools/wpt_fetch.sh').read_text(), re.M)[1]
    if pin != spec['commit']:
        parser.error('sample pin differs from the shared WPT pin; review before updating')
    output = args.output.resolve()
    for name, sha in spec['files'].items():
        path = (output/name).resolve()
        if not path.is_relative_to(output):
            raise ValueError('escaping sample path')
        data = path.read_bytes() if path.exists() else urlopen(
            f'https://raw.githubusercontent.com/web-platform-tests/wpt/{pin}/{name}', timeout=30).read()
        if hashlib.sha256(data).hexdigest() != sha:
            raise ValueError(f'digest mismatch: {name}')
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
        print(name, sha)
    print('WPT source license: https://github.com/web-platform-tests/wpt/blob/'+pin+'/LICENSE.md')


if __name__ == '__main__':
    main()
