#!/usr/bin/env python3
"""Extract complete JSON records emitted by Yonder's layout measurement pages.

Reads redirected console logs. Invalid/cancelled/failed runs are preserved with
that state; a truncated record or a log with no records is an error.
"""
import argparse
import json
from pathlib import Path

PREFIX = 'YONDER_LAYOUT_RESULT '
FORMATS = {'yonder-layout-5a-v1', 'yonder-layout-local-v1',
           'yonder-layout-graft-v1', 'yonder-stream-layout-v1'}
STATES = {'complete', 'cancelled', 'invalid', 'failed'}


def records(path):
    found = []
    for number, line in enumerate(path.read_text(encoding='utf-8').splitlines(), 1):
        if not line.startswith(PREFIX):
            continue
        try:
            value = json.loads(line[len(PREFIX):], parse_constant=reject_constant)
            if not isinstance(value, dict) or value.get('format') not in FORMATS or value.get('state') not in STATES:
                raise ValueError('unknown report format or state')
        except (ValueError, TypeError) as error:
            raise ValueError(f'{path}:{number}: invalid measurement record: {error}') from error
        found.append({'source': str(path), 'line': number, 'report': value})
    if not found:
        raise ValueError(f'{path}: no measurement records (check output redirection and run completion)')
    return found


def reject_constant(value):
    raise ValueError(f'non-JSON number {value}')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('logs', nargs='+', type=Path)
    args = parser.parse_args()
    try:
        reports = [record for path in args.logs for record in records(path)]
    except (OSError, ValueError) as error:
        parser.exit(1, f'{error}\n')
    print(json.dumps(reports, ensure_ascii=False, indent=2, allow_nan=False))


if __name__ == '__main__':
    main()
