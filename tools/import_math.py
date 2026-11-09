#!/usr/bin/env python3
"""Reproduce libmath's upstream/ from the pinned musl archive.

    tools/import_math.py --check <musl-1.2.5.tar.gz>   prove upstream/ is
                                                         archive + patches/
    tools/import_math.py --write <musl-1.2.5.tar.gz>   rebuild upstream/ and
                                                         manifest.json from it

The FILE LIST is manifest.json's: what the port takes from musl is a
reviewed decision (MATH.md § Source inventory), so the import reproduces
that list and never widens it. Patches in patches/ are applied in sorted
order with `patch -p1` from inside upstream/, the prefix musl's own commits
carry; manifest.json pins each patch file and every resulting byte.
tools/test_math_audit.py checks the tree against the manifest on every run;
this script is what checks the manifest against musl.
"""
import hashlib
import json
import shutil
import subprocess
import sys
import tarfile
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
LIB = ROOT / 'userland/libmath'
UPSTREAM = LIB / 'upstream'
PATCHES = LIB / 'patches'
MANIFEST = LIB / 'manifest.json'


def sha256(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def build(archive, manifest, into):
    """Extract the manifest's files from the archive into `into`, then patch."""
    if sha256(archive) != manifest['archive_sha256']:
        raise SystemExit(f'{archive}: digest is not the pinned archive_sha256')
    prefix = manifest['archive_prefix']
    with tarfile.open(archive) as tar:
        for name in manifest['files']:
            member = tar.getmember(prefix + name)
            target = into / name
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(tar.extractfile(member).read())
    applied = []
    for patch in sorted(PATCHES.glob('*.patch')):
        subprocess.run(['patch', '-s', '-p1', '--no-backup-if-mismatch', '-i', str(patch)],
                       cwd=into, check=True)
        commit = patch.read_text().split()[1]
        applied.append({'file': patch.name, 'upstream_commit': commit, 'sha256': sha256(patch)})
    return applied


def main():
    if len(sys.argv) != 3 or sys.argv[1] not in ('--check', '--write'):
        raise SystemExit(__doc__)
    mode, archive = sys.argv[1], Path(sys.argv[2])
    manifest = json.loads(MANIFEST.read_text())
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        applied = build(archive, manifest, tmp)
        files = {name: sha256(tmp / name) for name in sorted(manifest['files'])}
        if mode == '--check':
            if applied != manifest['patches']:
                raise SystemExit('patches/ and manifest.json disagree')
            bad = [n for n, d in files.items() if d != manifest['files'][n] or d != sha256(UPSTREAM / n)]
            if bad:
                raise SystemExit(f'not archive + patches: {bad}')
            print(f'PASS import: {len(files)} files are the pinned archive plus {len(applied)} patch(es)')
            return
        shutil.rmtree(UPSTREAM)
        shutil.copytree(tmp, UPSTREAM)
    manifest['patches'] = applied
    manifest['files'] = files
    MANIFEST.write_text(json.dumps(manifest, indent=2) + '\n')
    print(f'WROTE upstream/ and manifest.json: {len(files)} files, {len(applied)} patch(es)')


main()
