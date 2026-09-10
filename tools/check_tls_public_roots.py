#!/usr/bin/env python3
"""Reproduce the pinned public-root subset using os64's actual anchor loader."""
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
DATA = ROOT / 'trust/mozilla/2026-08-13'
POLICY = ['OK', 'DER', 'LIMIT', 'DUPLICATE', 'EXTENSION', 'CRITICAL', 'SAN',
          'EKU', 'CA', 'KEY_USAGE', 'KEY', 'SIGNATURE', 'ANCHOR', 'SEQUENCE']


def digest(data):
    return hashlib.sha256(data).hexdigest()


def main():
    manifest = json.loads((DATA / 'manifest.json').read_text())
    source = (DATA / 'cacert.pem').read_bytes()
    assert digest(source) == manifest['source_sha256']
    assert (DATA / 'cacert.pem.sha256').read_text().split()[0] == manifest['source_sha256']
    roots = (DATA / 'install/roots.pem').read_bytes()
    assert digest(roots) == manifest['bundle_sha256']
    pattern = rb'-----BEGIN CERTIFICATE-----.*?-----END CERTIFICATE-----\n'
    blocks = re.findall(pattern, source, re.S)
    assert len(blocks) == len(manifest['accepted']) + len(manifest['excluded']) == 121
    userland = ROOT / 'userland'
    # Link the production BearSSL object archive into a host probe. The policy
    # and PEM sources are also compiled with sanitizers; only OS allocation
    # and memory operations are substituted, not the certificate decisions.
    subprocess.run(['make', '-C', str(userland), '-j4', str(userland / 'bin/libtls.so')],
                   check=True, stdout=subprocess.DEVNULL)
    with tempfile.TemporaryDirectory(prefix='os64-public-roots-') as directory:
        work = Path(directory)
        paths = []
        for i, block in enumerate(blocks):
            path = work / f'root-{i:03d}.pem'
            path.write_bytes(block)
            paths.append(path)
        probe = work / 'probe'
        port = userland / 'libtls/port'
        command = [os.environ.get('HOST_CC', 'cc'), '-std=c11', '-O2', '-g', '-Wall', '-Wextra', '-Werror',
                   '-ffunction-sections', '-fdata-sections', '-fsanitize=address,undefined',
                   '-fno-sanitize-recover=all', '-fno-pie', '-no-pie', '-include', str(port / 'config.h')]
        command += ['-I' + str(ROOT / p) for p in ('userland/libtls/include', 'userland/libtls/upstream/inc',
                                                 'userland/libos64/include', 'abi/include')]
        command += [str(ROOT / 'tools/tls_roots_probe.c')]
        command += [str(port / f) for f in ('certificate_der.c', 'certificate_policy.c', 'trust_pem.c')]
        command += [str(userland / 'obj/tls-shared/core.a'), '-Wl,--gc-sections,-z,noexecstack', '-o', str(probe)]
        subprocess.run(command, check=True)
        env = {**os.environ, 'ASAN_OPTIONS': 'detect_leaks=0'}
        output = subprocess.check_output([str(probe), *map(str, paths), str(DATA / 'install/roots.pem')], env=env, text=True)
        lines = [line.rsplit(' ', 4) for line in output.splitlines()]
        expected = {entry['der_sha256']: entry for group in ('accepted', 'excluded') for entry in manifest[group]}
        accepted = []
        import base64
        for block, (_, status, reason, _, count) in zip(blocks, lines[:-1], strict=True):
            der = base64.b64decode(b''.join(block.splitlines()[1:-1]), validate=True)
            entry = expected[digest(der)]
            assert POLICY[int(reason)] == entry['policy'], entry
            assert int(status) == (0 if entry['policy'] == 'OK' else 6), entry
            if int(status) == 0:
                assert count == '1'
                accepted.append(block)
        assert re.findall(pattern, roots, re.S) == accepted
        assert lines[-1][1:3] == ['0', '0'] and int(lines[-1][-1]) == len(accepted)
        print(f'PASS pinned source and output hashes; {len(accepted)} accepted, {len(blocks)-len(accepted)} documented exclusions')
        print('PASS unchanged certificate bytes, per-root policy decisions and complete production bundle load')


if __name__ == '__main__':
    main()
