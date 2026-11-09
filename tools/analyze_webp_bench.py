#!/usr/bin/env python3
"""Validate a complete guest WebP benchmark report and summarize phase timings."""
import argparse
import json
from pathlib import Path
import statistics


def analyze(path):
    records = []
    for line in path.read_text().splitlines():
        words = line.split()
        if words:
            records.append((words[0], dict(w.split('=', 1) for w in words[1:] if '=' in w)))

    def rows(kind, mode=None):
        return [r for k, r in records if k == kind and (mode is None or r.get('mode') == mode)]

    headers, endings = rows('webpbench'), rows('complete')
    assert len(headers) == len(endings) == 1, 'missing/duplicate header or completion'
    scope = headers[0]['scope']
    assert headers[0]['version'] == '1' and scope in ('quick', 'full'), 'unknown report version/scope'
    assert endings[0] == dict(scope=scope, result='0'), 'benchmark did not pass'
    child = rows('scalar_child')
    assert len(child) == 1 and int(child[0]['pid']) > 0 and int(child[0]['wait']) >= 0 and child[0]['code'] == '0', 'scalar child failed'
    expected = {f'{kind}_{size}' for size in ((512,) if scope == 'quick' else (512, 4096))
                for kind in ('lossy', 'lossless', 'alpha')}
    phases = ('call_us', 'gate_us', 'allocate_us', 'scratch_free_us', 'other_decode_us', 'verify_us', 'output_free_us')
    result = dict(scope=scope, modes={})
    reference = {}
    for mode in ('sse2', 'scalar'):
        builds = rows('build', mode)
        assert len(builds) == 1 and builds[0]['libwebp'] == '1.6.0', 'missing build identity'
        fixtures = rows('fixture', mode)
        assert len(fixtures) == len(expected) and {f['name'] for f in fixtures} == expected, 'missing/duplicate fixtures'
        by_name = {f['name']: f for f in fixtures}
        for f in fixtures:
            assert f['input_crc'] == f['expected_input_crc'], 'input CRC mismatch'
            identity = tuple(f[k] for k in ('width', 'height', 'input_bytes', 'input_crc', 'expected_pixel_crc'))
            assert reference.setdefault(f['name'], identity) == identity, 'mode fixture mismatch'
        samples = rows('sample', mode)
        keys = [(s['phase'], s['fixture'], int(s['index'])) for s in samples]
        wanted = {('serial', name, i) for name in expected for i in range(3 if name.endswith('_512') else 1)}
        wanted.add(('warmup', 'lossy_512', 0))
        contended_name = 'alpha_512' if scope == 'quick' else 'alpha_4096'
        wanted.update(('contended', contended_name, i) for i in range(6))
        assert len(keys) == len(wanted) and set(keys) == wanted, 'missing/duplicate samples'
        for s in samples:
            assert s['valid'] == '1' and s['status'] == 'ok', 'decode/pixel failure'
            assert s['crc'] == by_name[s['fixture']]['expected_pixel_crc'], 'pixel CRC mismatch'
            assert all(int(s[k]) >= 0 for k in phases), 'negative phase duration'
            assert int(s['call_us']) == sum(int(s[k]) for k in ('gate_us', 'allocate_us', 'scratch_free_us', 'other_decode_us')), 'phase accounting mismatch'
        finished = rows('mode_result', mode)
        assert len(finished) == 1 and finished[0]['result'] == finished[0]['heap_problems'] == '0', 'heap/mode failure'
        contention = rows('contention', mode)
        assert len(contention) == 1 and contention[0]['workers'] == '6' and contention[0]['cpu_valid'] == '1' and contention[0]['result'] == '0', 'contention measurement failed'
        result['modes'][mode] = dict(serial_median_us={
            name: {k: statistics.median(int(s[k]) for s in samples if s['phase'] == 'serial' and s['fixture'] == name) for k in phases}
            for name in sorted(expected)}, contention=contention[0],
            maximum_gate_us=max(int(s['gate_us']) for s in samples if s['phase'] == 'contended'))
    return result


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('report', type=Path)
    args = parser.parse_args()
    try:
        print(json.dumps(analyze(args.report), indent=2))
    except (AssertionError, KeyError, ValueError) as error:
        parser.exit(1, f'FAIL: {error}\n')
