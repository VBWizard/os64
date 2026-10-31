#!/usr/bin/env python3
"""Check exported fixture runs against their visible reports and reject lost data."""
import json
from pathlib import Path
import sys
import tempfile
from layout_results import PREFIX, records

for name in sys.argv[1:]:
    report = Path(name)
    visible, _ = json.JSONDecoder().raw_decode(report.read_text())
    exported = records(Path(name + '.console.log'))
    assert exported[0]['report'] == visible, f'{name}: exported samples differ from visible report'
    states = [entry['report']['state'] for entry in exported]
    expected = ['complete'] if visible['format'] == 'yonder-stream-layout-v1' else ['complete', 'cancelled', 'invalid']
    assert states == expected, (name, states)
    if 'checkpoints' in visible:
        assert len(visible['samples_ms']) == len(visible['heights']) == visible['checkpoints']
    else:
        assert all(len(result['samples_ms']) == visible['samples'] for result in visible['results'])
    print(f'{name}: exact report and {len(exported)} distinct run states preserved')

with tempfile.TemporaryDirectory() as directory:
    path = Path(directory) / 'console.log'
    for bad in ['unrelated output\n', PREFIX + '{"format":',
                PREFIX + '{"format":"yonder-layout-graft-v1","state":"complete","bad":NaN}']:
        path.write_text(bad)
        try:
            records(path)
        except ValueError:
            pass
        else:
            raise AssertionError('missing/truncated/invalid output was accepted')
print('Missing, truncated and non-JSON output rejected')
