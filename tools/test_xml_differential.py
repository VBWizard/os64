#!/usr/bin/env python3
"""Deterministic structural mutations, checked by host Expat independently."""
import argparse
from collections import Counter
from pathlib import Path
import random
import subprocess
import tempfile
from xml.parsers import expat


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--driver', type=Path, required=True)
    args = ap.parse_args()
    rng = random.Random(0x6A2026)
    seeds = [b'<r/>', b'<r a="&#9;&amp;&lt;">hello\r\nworld</r>',
             b'<?pi meta?><p:r xmlns:p="urn:p"><r xmlns="urn:d"/></p:r>',
             b'<r><!--comment--><![CDATA[<raw>&]]><?pi text?></r>',
             b'<r xmlns:a="urn:u" xmlns:b="urn:u" a:a="v"><b:a b:b="w"/></r>']
    # Expat accepts malformed XML version labels, so declaration grammar
    # uses the explicit specification fixtures in test_xml_driver.c instead.
    cases = list(seeds)
    for _ in range(2500):
        data = bytearray(rng.choice(seeds))
        for _ in range(rng.randrange(1, 5)):
            pos = rng.randrange(len(data) + 1)
            operation = rng.randrange(3)
            if operation == 0 and pos < len(data):
                del data[pos]
            elif operation == 1 and pos < len(data):
                data[pos] = rng.choice(b'<>/!?[]-:=&;\'" abcxyz012\t\n\r')
            else:
                data[pos:pos] = bytes([rng.choice(b'<>/!?[]-:=&;\'" abcxyz012\t\n\r')])
        cases.append(bytes(data))
    expected = []
    for data in cases:
        parser = expat.ParserCreate(namespace_separator='|')
        try:
            parser.Parse(data, True)
            expected.append('DONE')
        except expat.ExpatError:
            expected.append('MALFORMED')
    with tempfile.TemporaryDirectory() as directory:
        paths = [Path(directory) / f'{i}.xml' for i in range(len(cases))]
        for path, data in zip(paths, cases):
            path.write_bytes(data)
        result = subprocess.run([str(args.driver.resolve()), '--batch'], input=''.join(str(p)+'\n' for p in paths), text=True, capture_output=True, check=True, timeout=60)
    if result.stderr:
        print(result.stderr, end='')
    actual = result.stdout.splitlines()
    if len(actual) != len(expected):
        raise SystemExit('differential driver protocol failure')
    mismatches = [(i, a, e, cases[i]) for i, (a, e) in enumerate(zip(actual, expected))
                  if a != e]
    if mismatches:
        for mismatch in mismatches[:20]:
            print(mismatch)
        raise SystemExit('XML/Expat acceptance mismatch')
    print('XML structural differential:', dict(Counter(actual)))


if __name__ == '__main__':
    main()
