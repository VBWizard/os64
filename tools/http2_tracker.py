#!/usr/bin/env python3
"""HTTP/2 defaults for the shared tracker; HTTP2.md § 0 defines upkeep.

    tools/http2_tracker.py
    tools/http2_tracker.py --html out.html

The command and CSV format are preserved for existing launchers.
"""
import runpy
import sys
from pathlib import Path

if __name__ == '__main__':
    tool = Path(__file__).resolve()
    # run_path also works with python -I, which excludes this directory from
    # module search. The launcher intentionally uses isolated Python.
    tracker = runpy.run_path(str(tool.with_name('tracker.py')))
    sys.exit(tracker['main'](
        default_csv=str(tool.parent.parent / 'docs/design/pending/HTTP2_TRACKER.csv'),
        default_title='HTTP/2 Tracker',
        default_prefix='H2',
        default_contract='HTTP2.md § 0',
    ))
