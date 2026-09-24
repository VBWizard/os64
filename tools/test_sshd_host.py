#!/usr/bin/env python3
"""Build the SSH engine with ASan/UBSan and exercise it with real OpenSSH.
All keys are generated into the test directory. The fixture server binds loopback.
"""
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import os
import socket
import threading
import time
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
    for size in (1000, 4095):
        text = 'q' * (size - 7)
        result = command('printf ' + text)
        assert result.returncode == 0 and result.stdout == text.encode(), result
    result = command('x' * 4096)
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
    forwards(work)
    from cryptography.hazmat.primitives.asymmetric import ec
    from cryptography.hazmat.primitives import serialization
    import base64
    pub = ec.derive_private_key(1, ec.SECP256R1()).public_key().public_bytes(
        serialization.Encoding.OpenSSH, serialization.PublicFormat.OpenSSH)
    expected = 'SHA256:' + base64.b64encode(hashlib.sha256(base64.b64decode(pub.split()[1])).digest()).decode().rstrip('=')
    found = run(['ssh-keygen', '-lf', work / 'known_hosts'], capture_output=True, text=True).stdout
    assert expected in found, found
    print('OpenSSH: host fingerprint matches independent P-256 serialization PASS', flush=True)


def echo_server():
    """A host echo service on loopback, one thread per connection."""
    listener = socket.socket()
    listener.bind(('127.0.0.1', 0))
    listener.listen(16)
    def serve(conn):
        with conn:
            while True:
                data = conn.recv(65536)
                if not data:
                    return
                conn.sendall(data)
    def accept():
        while True:
            try:
                conn, _ = listener.accept()
            except OSError:
                return
            threading.Thread(target=serve, args=(conn,), daemon=True).start()
    threading.Thread(target=accept, daemon=True).start()
    return listener


def free_port():
    probe = socket.socket()
    probe.bind(('127.0.0.1', 0))
    port = probe.getsockname()[1]
    probe.close()
    return port


def listening(port):
    with open('/proc/net/tcp') as table:
        for line in table.readlines()[1:]:
            fields = line.split()
            if int(fields[1].split(':')[1], 16) == port and fields[3] == '0A':
                return True
    return False


def exchange(port, data):
    """Send data through a forwarded port while reading the echo back."""
    conn = socket.create_connection(('127.0.0.1', port), timeout=30)
    received = bytearray()
    def reader():
        while len(received) < len(data):
            chunk = conn.recv(65536)
            if not chunk:
                return
            received.extend(chunk)
    thread = threading.Thread(target=reader)
    thread.start()
    conn.sendall(data)
    thread.join(timeout=60)
    conn.close()
    return bytes(received)


def forwards(work):
    """direct-tcpip through the production engine (REMOTE.md section 2)."""
    echo = echo_server()
    echo_port = echo.getsockname()[1]
    closed_port = free_port()
    def session(destination, command=None, check=None, extra=(), server_rekey=0):
        local = free_port()
        log = (work / f'server-forward-{local}.log').open('w')
        server = subprocess.Popen([str(work / 'server'), '0', str(work / 'client.pub'), str(server_rekey)],
                                  stdout=subprocess.PIPE, stderr=log, text=True, env=ENV)
        port = server.stdout.readline().strip()
        # INFO, not ERROR: OpenSSH reports a refused channel open at INFO, and
        # the refusal's reason is what the checks below read.
        argv = ['ssh', '-F', '/dev/null', '-p', port, '-o', 'BatchMode=yes', '-o', 'LogLevel=INFO',
                '-o', 'IdentitiesOnly=yes', '-o', 'StrictHostKeyChecking=accept-new',
                '-o', 'HostKeyAlias=os64-sshd-test', '-o', 'UserKnownHostsFile=' + str(work / 'known_hosts'),
                '-i', str(work / 'client'), *extra, '-L', f'{local}:{destination}',
                *(['os64@127.0.0.1', command] if command else ['-N', 'os64@127.0.0.1'])]
        client = subprocess.Popen(argv, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                  stderr=subprocess.PIPE, env=ENV)
        try:
            # Wait for ssh to listen WITHOUT connecting: a probe connection
            # would open a channel of its own and hold one of the slots the
            # checks below count.
            for _ in range(200):
                if listening(local):
                    break
                time.sleep(0.05)
            outcome = check(local)
            if command:
                out, err = client.communicate(timeout=60)
            else:
                client.terminate()
                out, err = client.communicate(timeout=30)
            server.wait(timeout=10)
            assert server.returncode == 0, Path(log.name).read_text()
            return outcome, out, err
        finally:
            for proc in (client, server):
                if proc.poll() is None:
                    proc.kill()
                    proc.wait()
            server.stdout.close()
            log.close()

    data = os.urandom(3 * 1024 * 1024)
    outcome, _, err = session(f'localhost:{echo_port}', check=lambda local: exchange(local, data))
    assert outcome == data, (len(outcome), err)
    print('OpenSSH: -N -L to localhost, 3 MiB each way through one forward PASS', flush=True)
    outcome, _, err = session(f'localhost:{echo_port}', check=lambda local: exchange(local, data),
                              extra=['-o', 'RekeyLimit=64K'], server_rekey=65536)
    assert outcome == data, (len(outcome), err)
    print('OpenSSH: 3 MiB each way through a forward across client and server rekeys PASS', flush=True)

    def concurrent(local):
        chunks = [os.urandom(262144) for _ in range(7)]
        with ThreadPoolExecutor(max_workers=7) as pool:
            return chunks, list(pool.map(lambda c: exchange(local, c), chunks))
    (chunks, results), _, err = session(f'127.0.0.1:{echo_port}', check=concurrent)
    assert chunks == results, err
    print('OpenSSH: seven concurrent forwarded connections PASS', flush=True)

    def eighth(local):
        held = [socket.create_connection(('127.0.0.1', local), timeout=10) for _ in range(7)]
        for conn in held:
            conn.sendall(b'x')
            assert conn.recv(1) == b'x'
        extra = socket.create_connection(('127.0.0.1', local), timeout=10)
        extra.settimeout(10)
        closed = extra.recv(1) == b''
        extra.close()
        for conn in held:
            conn.close()
        return closed
    outcome, _, err = session(f'127.0.0.1:{echo_port}', check=eighth)
    assert outcome and b'resource shortage' in err.lower(), err
    print('OpenSSH: an eighth forward is refused as a resource shortage PASS', flush=True)

    def refused(local):
        conn = socket.create_connection(('127.0.0.1', local), timeout=10)
        conn.settimeout(10)
        answer = conn.recv(1)
        conn.close()
        return answer
    outcome, _, err = session(f'10.255.255.1:{echo_port}', check=refused)
    assert outcome == b'' and b'administratively prohibited' in err and b'loopback only' in err, err
    print('OpenSSH: a non-loopback destination is refused PASS', flush=True)
    outcome, _, err = session(f'127.0.0.1:{closed_port}', check=refused)
    assert outcome == b'' and b'connect failed' in err, err
    print('OpenSSH: a closed loopback port is CONNECT_FAILED PASS', flush=True)

    small = os.urandom(65536)
    outcome, out, err = session(f'localhost:{echo_port}', command='sleep 2; printf done',
                                check=lambda local: exchange(local, small))
    assert outcome == small and out == b'done', (out, err)
    print('OpenSSH: a session and a forward share one connection PASS', flush=True)
    echo.close()


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
