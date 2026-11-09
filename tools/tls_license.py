#!/usr/bin/env python3
"""Emit a C notice from the pinned BearSSL license, without fixture data."""
import json
from pathlib import Path
import sys

source = Path(__file__).resolve().parents[1] / 'userland/libtls/upstream/LICENSE.txt'
Path(sys.argv[1]).write_text('static const char tls_license[] =\n' +
                           json.dumps(source.read_text()) + ';\n')
