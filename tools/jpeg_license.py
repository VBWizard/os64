#!/usr/bin/env python3
"""Embed the pinned codec's source notices and binary acknowledgment."""
import json
from pathlib import Path
import sys
root = Path(__file__).resolve().parents[1] / 'userland/libjpeg/upstream'
text = 'This software is based in part on the work of the Independent JPEG Group.\n\n'
text += '\n\n'.join((root / name).read_text() for name in ('LICENSE.md', 'README.ijg'))
Path(sys.argv[1]).write_text('static const char jpeg_license_text[] =\n' +
                           '\n'.join(json.dumps(line + '\n') for line in text.splitlines()) + ';\n')
