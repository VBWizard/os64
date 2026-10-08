#!/usr/bin/env python3
"""Verify the pinned engine and apply its patch series into a build directory."""
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys

root = Path(__file__).resolve().parents[1] / 'userland/libjs'
manifest = json.loads((root / 'manifest.json').read_text())
for entry in manifest['retained_files'] + manifest['patches']:
    data = (root / entry['path']).read_bytes()
    if hashlib.sha256(data).hexdigest() != entry['sha256']:
        sys.exit('QuickJS source hash mismatch: ' + entry['path'])
destination = Path(sys.argv[1])
destination.mkdir(parents=True, exist_ok=True)
for entry in manifest['retained_files']:
    shutil.copyfile(root / entry['path'], destination / Path(entry['path']).name)
for entry in manifest['patches']:
    with (root / entry['path']).open('rb') as patch:
        # Context anchors engine edits across line shifts; discarding it can
        # silently put a helper in the wrong function after an earlier patch.
        result = subprocess.run(['patch', '-s', '-F0', '-d', str(destination), '-p1'], stdin=patch)
        if result.returncode:
            sys.exit('QuickJS patch failed: ' + entry['path'])
source = (destination / 'quickjs.c').read_text()
if '#define CONFIG_ATOMICS' in source or '#define CONFIG_STACK_CHECK' not in source:
    sys.exit('QuickJS profile mismatch: Atomics omission / stack checks')
