#!/usr/bin/env python3
"""Check encoding contracts and saved-page snapshots; refresh only by explicit flag."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess

ROOT = Path(__file__).resolve().parent / 'html_corpus'


def parse(driver, data, charset=None, chunk=0):
    label = charset.encode() if charset else b''
    wire = struct.pack('<4I', 2, chunk, len(data), len(label)) + data + label
    result = subprocess.run([driver], input=wire, stdout=subprocess.PIPE, check=True, timeout=20)
    return json.loads(result.stdout)


def encoding_checks(driver):
    replacement = '\ufffd'
    fixtures = [
        (b'<p>\x80', None, 'windows-1252', '\u20ac', None),
        (b'<p>\x80', 'ISO-8859-1', 'windows-1252', '\u20ac', None),
        (b'<p>\x80', 'shift_jis', 'windows-1252', '\u20ac', 'shift_jis'),
        (b'\xef\xbb\xbf<p>\xc3\xa9', 'windows-1252', 'utf-8', '\xe9', None),
        (b'<meta charset=utf-8><p>\xc3\xa9', None, 'utf-8', '\xe9', None),
        (b'<meta content="text/html; charset=utf-8" http-equiv=content-type><p>\xc3\xa9', None, 'utf-8', '\xe9', None),
        (b'<!-- <meta charset=utf-8> --><p>\x80', None, 'windows-1252', '\u20ac', None),
        (b'<meta charset=utf-8><p>\x80', 'windows-1252', 'windows-1252', '\u20ac', None),
        (b'<p>\xf0\x90\x80\x80', 'utf-8', 'utf-8', '\U00010000', None),
        (b'<p>\xed\xa0\x80', 'utf-8', 'utf-8', replacement * 3, None),
        (b'<p>\xe0\x80\x80', 'utf-8', 'utf-8', replacement * 3, None),
        (b'<p>\xf4\x90\x80\x80', 'utf-8', 'utf-8', replacement * 4, None),
        (b'<p>\xe2\x82', 'utf-8', 'utf-8', replacement, None),
        (b'<p>\xe2\x82X', 'utf-8', 'utf-8', replacement + 'X', None),
        (b'<!--><meta charset=utf-8><p>\xc3\xa9', None, 'utf-8', '\xe9', None),
        (b'</x a="<meta charset=utf-8>"><p>\x80', None, 'windows-1252', '\u20ac', None),
        (b'<p>\x80<meta charset=utf-8', None, 'windows-1252', '\u20ac', None),
        (b'<p>A\r\nB\rC', 'utf-8', 'utf-8', 'A\nB\nC', None),
    ]
    for codec, bom in [('utf-16le', b'\xff\xfe'), ('utf-16be', b'\xfe\xff')]:
        fixtures.append((bom + '<p>\U0001f499'.encode(codec), 'windows-1252', codec, '\U0001f499', None))
        fixtures.append((bom + '<p>\ud800X\udc00'.encode(codec, 'surrogatepass'), None, codec, replacement + 'X' + replacement, None))
        fixtures.append((bom + '<p>\ud800'.encode(codec, 'surrogatepass') + b'X', None, codec, replacement, None))
        fixtures.append((bom + '<p>'.encode(codec) + b'X', None, codec, replacement, None))
    for data, label, encoding, text, unsupported in fixtures:
        baseline = None
        for chunk in [0, 1, 0xffffffff]:
            result = parse(driver, data, label, chunk)
            assert result['refusal'] == 0, result
            assert result['charset'] == encoding, result
            assert result['unsupported'] == unsupported, result
            assert '"' + text + '"' in result['tree'], (data, result)
            stable = {k: v for k, v in result.items() if k != 'seconds'}
            assert baseline is None or baseline == stable, (data, result)
            baseline = stable
    data = b'<p>\x80' + b' ' * 1002 + b'<meta charset=utf-8>'
    result = parse(driver, data)
    assert result['charset'] == 'windows-1252' and result['late_meta'] == 'utf-8', result
    data = b'<p>' + b'x' * 1024 + b'<meta charset=utf-8>\x80'
    result = parse(driver, data)
    assert result['charset'] == 'windows-1252' and result['late_meta'] == 'utf-8', result
    assert '\u20ac' in result['tree'], result
    print(f'Encoding: {len(fixtures)} cases in three chunkings; late-meta deviation recorded')


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--driver', required=True)
    ap.add_argument('--refresh-snapshots', action='store_true')
    args = ap.parse_args()
    encoding_checks(args.driver)
    sources = json.loads((ROOT / 'SOURCES.json').read_text())
    for name, source in sources.items():
        data = (ROOT / (name + '.html')).read_bytes()
        assert hashlib.sha256(data).hexdigest() == source['sha256'], name
        baseline = None
        for chunk in [0, 1, 0xffffffff]:
            result = parse(args.driver, data, source['charset'], chunk)
            assert result['refusal'] == 0, (name, result['refusal'])
            stable = {k: v for k, v in result.items() if k != 'seconds'}
            assert baseline is None or baseline == stable, name
            baseline = stable
            if chunk == 0:
                print(f"Corpus {name}: bytes={len(data)} nodes={result['nodes']} "
                      f"work={result['work']} peak_arena={result['arena']} seconds={result['seconds']:.4f}")
        snapshot = ROOT / (name + '.tree')
        expected = (baseline['tree'] + '\n').encode()
        if args.refresh_snapshots:
            snapshot.write_bytes(expected)
        else:
            assert snapshot.read_bytes() == expected, f'{name}: snapshot differs'


if __name__ == '__main__':
    main()
