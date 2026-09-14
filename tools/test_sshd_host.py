#!/usr/bin/env python3
"""Build the SSH engine with ASan/UBSan and exercise it with real OpenSSH.
All keys are generated into the test directory. The fixture server binds loopback.
"""
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import os
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
CORE = ROOT / 'userland/apps/sshd'
SOURCES = [CORE / ('ssh_' + name + '.c') for name in ('codec', 'keys', 'transport', 'connection')]
FLAGS = ['-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
         '-fno-sanitize-recover=all', '-fno-omit-frame-pointer', '-no-pie']
ENV = {**os.environ, 'ASAN_OPTIONS': 'detect_leaks=0'}


def run(argv, **kw):
    return subprocess.run([str(x) for x in argv], check=True, env=ENV, **kw)


def build(work):
    base = ROOT / 'userland/libtls'
    sources = sorted((base / 'upstream/src').rglob('*.c'))
    digest = hashlib.sha256(repr(FLAGS).encode())
    for path in sorted((base / 'upstream').rglob('*.h')) + sources + [base / 'port/config.h']:
        digest.update(path.read_bytes())
    stamp = work / 'crypto.stamp'
    archive = work / 'crypto.a'
    if not archive.exists() or not stamp.exists() or stamp.read_text() != digest.hexdigest():
        flags = [*FLAGS, '-D_DEFAULT_SOURCE', '-include', base / 'port/config.h',
                 '-I' + str(base / 'upstream/inc'), '-I' + str(base / 'upstream/src')]
        def compile_one(src):
            obj = work / (str(src.relative_to(base / 'upstream/src')).replace('/', '_') + '.o')
            run(['cc', *flags, '-c', src, '-o', obj], capture_output=True)
            return obj
        with ThreadPoolExecutor(max_workers=8) as pool:
            objects = list(pool.map(compile_one, sources))
        run(['ar', 'rcs', archive, *objects])
        stamp.write_text(digest.hexdigest())
    for driver, output in [(ROOT / 'userland/tests/sshtest/sshtest.c', 'unit'),
                           (ROOT / 'tools/sshd_host_server.c', 'server')]:
        run(['cc', *FLAGS, '-DSSH_HOST', driver, *SOURCES, archive, '-lutil', '-o', work / output])
    run([work / 'unit'])
    run([sys.executable, ROOT / 'tools/test_sshd_session_host.py'])


def interop(work):
    for name, kind in [('client', 'ecdsa'), ('wrong', 'ecdsa'), ('ed25519', 'ed25519')]:
        if not (work / name).exists():
            run(['ssh-keygen', '-q', '-t', kind, '-N', '', '-f', work / name])
    case_number = 0
    def command(command, *, data=b'', extra=(), key='client', server_rekey=0):
        nonlocal case_number
        case_number += 1
        with (work / f'server-{case_number}.log').open('w') as log:
            server = subprocess.Popen([str(work / 'server'), '0', str(work / 'client.pub'), str(server_rekey)],
                                      stdout=subprocess.PIPE, stderr=log, text=True, env=ENV)
            try:
                port = server.stdout.readline().strip()
                if not port:
                    raise RuntimeError('fixture server did not bind; see ' + log.name)
                argv = ['ssh', '-F', '/dev/null', '-p', port, '-o', 'BatchMode=yes',
                        '-o', 'LogLevel=ERROR', '-o', 'IdentitiesOnly=yes',
                        '-o', 'StrictHostKeyChecking=accept-new',
                        '-o', 'HostKeyAlias=os64-sshd-test',
                        '-o', 'UserKnownHostsFile=' + str(work / 'known_hosts'),
                        '-i', str(work / key), *extra, 'os64@127.0.0.1', command]
                result = subprocess.run(argv, input=data, capture_output=True, timeout=60, env=ENV)
                server.wait(timeout=5)
                if server.returncode:
                    raise RuntimeError('server failed: ' + Path(log.name).read_text())
                return result
            finally:
                if server.poll() is None:
                    server.kill()
                    server.wait()
                server.stdout.close()
    result = command('printf hello; printf error >&2; exit 7')
    assert (result.returncode, result.stdout, result.stderr) == (7, b'hello', b'error'), result
    print('OpenSSH: separate stdout/stderr and status 7 PASS', flush=True)
    result = command('false')
    assert result.returncode == 1 and not result.stdout, result
    result = command('printf forbidden', key='wrong')
    assert result.returncode == 255 and not result.stdout and b'publickey' in result.stderr, result
    result = command('printf forbidden', key='ed25519')
    assert result.returncode == 255 and not result.stdout and b'publickey' in result.stderr, result
    result = command('x' * 256)
    assert result.returncode == 255 and not result.stdout and b'exec request failed' in result.stderr, result
    print('OpenSSH: false, unenrolled key, Ed25519, oversized exec PASS', flush=True)
    data = bytes(range(256)) * 12288
    result = command('cat', data=data, extra=['-o', 'RekeyLimit=16K'])
    assert result.returncode == 0 and result.stdout == data and not result.stderr, (result.returncode, len(result.stdout), result.stderr)
    print('OpenSSH: 3 MiB binary stdin/stdout, window replenishment, repeated client rekey PASS', flush=True)
    result = command('cat', data=data[:262144], server_rekey=16384)
    assert result.returncode == 0 and result.stdout == data[:262144] and not result.stderr, (result.returncode, len(result.stdout), result.stderr)
    print('OpenSSH: server-initiated rekey with in-flight channel data PASS', flush=True)
    result = command('head -c 3145728 /dev/zero >&2; printf done')
    assert result.returncode == 0 and result.stdout == b'done' and result.stderr == bytes(3145728), (result.returncode, len(result.stderr))
    print('OpenSSH: stderr exceeds channel window PASS', flush=True)
    result = command('printf interactive; exit\n', data=b'', extra=['-tt'])
    # exec with a PTY is deliberately refused: interactive shells use the shell request.
    assert result.returncode == 255 and b'exec request failed' in result.stderr, result
    print('OpenSSH: unsupported PTY exec refused PASS', flush=True)
    from cryptography.hazmat.primitives.asymmetric import ec
    from cryptography.hazmat.primitives import serialization
    import base64
    pub = ec.derive_private_key(1, ec.SECP256R1()).public_key().public_bytes(
        serialization.Encoding.OpenSSH, serialization.PublicFormat.OpenSSH)
    expected = 'SHA256:' + base64.b64encode(hashlib.sha256(base64.b64decode(pub.split()[1])).digest()).decode().rstrip('=')
    found = run(['ssh-keygen', '-lf', work / 'known_hosts'], capture_output=True, text=True).stdout
    assert expected in found, found
    print('OpenSSH: host fingerprint matches independent P-256 serialization PASS', flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', type=Path)
    parser.add_argument('--unit-only', action='store_true')
    args = parser.parse_args()
    def check(work):
        work.mkdir(parents=True, exist_ok=True)
        build(work)
        if not args.unit_only:
            interop(work)
    if args.build_dir:
        check(args.build_dir.resolve())
    else:
        with tempfile.TemporaryDirectory(prefix='sshd-host-') as work:
            check(Path(work))

if __name__ == '__main__':
    main()
