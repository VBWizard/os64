#!/usr/bin/env python3
"""Summarize webpwatch samples and Yonder's page records; reject incomplete runs."""
import argparse
import gzip
import json
from pathlib import Path
import re


def analyze(watch, diagnostics, rounds, prefix=''):
    text = gzip.decompress(watch.read_bytes()).decode() if watch.suffix == '.gz' else watch.read_text()
    samples = []
    torn = 0
    for elapsed, cpu, threads, heap in re.findall(
            r'sample elapsed_us=(\d+) cpu_us=(\d+) threads=(\d+)\n(.*?)end-sample', text, re.S):
        fields = dict(re.findall(r'^(\w+)\t([^\n]+)$', heap, re.M))
        if fields.get('torn') == 'yes':
            torn += 1
            continue
        if fields.get('torn') != 'no':
            continue  # startup before the heap is published
        assert fields.get('audit') == 'ok', fields
        samples.append(dict(us=int(elapsed), cpu_us=int(cpu), threads=int(threads),
                            live=int(fields['live']), mapped=int(fields['mapped'])))
    assert samples, 'no stable heap samples'
    exited = re.search(r'exit elapsed_us=(\d+) code=(\d+) timeout=(\d+)', text)
    assert exited and exited.groups()[1:] == ('0', '0'), 'child did not exit cleanly'
    # The fixture's decoded output alone is 64 MiB. Separate active decode
    # bursts from quiescent UI storage, rather than counting startup as one.
    bursts = []
    active = False
    for sample in samples:
        high = sample['live'] >= 64 * 1024 * 1024
        if high and not active:
            bursts.append(dict(start_us=sample['us'], peak_live=sample['live']))
        if high:
            bursts[-1]['peak_live'] = max(bursts[-1]['peak_live'], sample['live'])
        if active and not high:
            bursts[-1]['drained_us'] = sample['us']
            bursts[-1]['idle_live'] = sample['live']
        active = high
    records = []
    for path in sorted(diagnostics.glob('*.txt')):
        record = path.read_text()
        address = re.search(r'^address: (.+)$', record, re.M)
        if address and address[1].startswith(prefix) and '/load-' in address[1]:
            coming = re.search(r'pictures: 6, (\d+) shown, (\d+) could not be read, '
                               r'(\d+) past the memory kept, (\d+) still coming', record)
            assert coming, record
            assert coming[2] == coming[3] == '0', record
            records.append(dict(address=address[1], shown=int(coming[1]), coming=int(coming[4])))
    if rounds:
        records = list({r['address']: r for r in records}.values())
        assert len(records) == rounds, ('navigation rounds', len(records), rounds)
        assert all(r['coming'] > 0 for r in records), 'navigation missed active picture jobs'
        assert len(bursts) >= rounds, ('decoded-image bursts', len(bursts), rounds)
        assert all('drained_us' in b for b in bursts), 'last decode burst did not drain'
        assert samples[-1]['live'] < 32 * 1024 * 1024, 'large image storage retained at idle'
    return dict(stable_samples=len(samples), torn_samples_skipped=torn,
                peak_live=max(s['live'] for s in samples),
                peak_mapped=max(s['mapped'] for s in samples),
                final_live=samples[-1]['live'], final_mapped=samples[-1]['mapped'],
                exit_us=int(exited[1]), bursts=bursts, navigation=records)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('watch', type=Path)
    parser.add_argument('diagnostics', type=Path)
    parser.add_argument('--rounds', type=int, default=12)
    parser.add_argument('--address-prefix', default='', help='select one run when the diagnostic directory is shared')
    args = parser.parse_args()
    print(json.dumps(analyze(args.watch, args.diagnostics, args.rounds, args.address_prefix), indent=2))
