#!/usr/bin/env python3
"""Reproduce libsound's upstream/ from the pinned speexdsp archive.

    tools/import_sound.py --check <speexdsp-1.2.1.tar.gz>   prove upstream/ is
                                                              the archive's files
    tools/import_sound.py --write <speexdsp-1.2.1.tar.gz>   rebuild upstream/ and
                                                              manifest.json from it

The FILE LIST is manifest.json's: what the port takes from speexdsp is a
reviewed decision (userland/libsound/README.md), so the import reproduces
that list and never widens it. Nothing is patched: every vendored byte is
the archive's. tools/test_sound_audit.py checks the tree against the
manifest on every run; this script is what checks the manifest against
speexdsp.
"""
import hashlib
import json
import shutil
import sys
import tarfile
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
LIB = ROOT / 'userland/libsound'
UPSTREAM = LIB / 'upstream'
MANIFEST = LIB / 'manifest.json'


def sha256(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def main():
    if len(sys.argv) != 3 or sys.argv[1] not in ('--check', '--write'):
        raise SystemExit(__doc__)
    mode, archive = sys.argv[1], Path(sys.argv[2])
    manifest = json.loads(MANIFEST.read_text())
    if sha256(archive) != manifest['archive_sha256']:
        raise SystemExit(f'{archive}: digest is not the pinned archive_sha256')
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        with tarfile.open(archive) as tar:
            for name in manifest['files']:
                member = tar.getmember(manifest['archive_prefix'] + name)
                target = tmp / name
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(tar.extractfile(member).read())
        files = {name: sha256(tmp / name) for name in sorted(manifest['files'])}
        if mode == '--check':
            bad = [n for n, d in files.items() if d != manifest['files'][n] or d != sha256(UPSTREAM / n)]
            if bad:
                raise SystemExit(f'not the archive: {bad}')
            print(f'PASS import: {len(files)} files are the pinned archive')
            return
        shutil.rmtree(UPSTREAM)
        shutil.copytree(tmp, UPSTREAM)
    manifest['files'] = files
    MANIFEST.write_text(json.dumps(manifest, indent=2) + '\n')
    print(f'WROTE upstream/ and manifest.json: {len(files)} files')


main()
