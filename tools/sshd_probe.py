#!/usr/bin/env python3
"""Check os64 sshd using OpenSSH: exec streams/status, rekey, PTY resize and DROP.

Enroll an ECDSA P-256 public key on the target first. Host identity checking is
strict by default; --accept-new is useful for a new private QEMU guest.
This probe runs commands and its guest fixture but does not install software.
"""
import argparse
import fcntl
import os
from pathlib import Path
import pty
import re
import select
import signal
import struct
import subprocess
import termios
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('host')
    parser.add_argument('--port', type=int, default=22)
    parser.add_argument('--key', required=True, type=Path)
    parser.add_argument('--known-hosts', required=True, type=Path)
    parser.add_argument('--accept-new', action='store_true')
    parser.add_argument('--bytes', type=int, default=3 * 1024 * 1024)
    parser.add_argument('--skip-fixture', action='store_true')
    parser.add_argument('--fixture', default='/tests/sshtest')
    parser.add_argument('--output', type=Path, default=Path('/tmp/sshd-probe'))
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    base = ['ssh', '-F', '/dev/null', '-p', str(args.port), '-o', 'BatchMode=yes',
            '-o', 'IdentitiesOnly=yes', '-o', 'ConnectTimeout=10', '-o', 'LogLevel=ERROR',
            '-o', 'StrictHostKeyChecking=' + ('accept-new' if args.accept_new else 'yes'),
            '-o', 'UserKnownHostsFile=' + str(args.known_hosts.resolve()), '-i', str(args.key.resolve())]
    def command(text, data=b'', options=(), timeout=180):
        return subprocess.run([*base, *options, 'os64@' + args.host, text],
                              input=data, capture_output=True, timeout=timeout)
    def require(result, status=0, stdout=None, stderr=None):
        assert result.returncode == status, (result.returncode, result.stdout[:500], result.stderr[:500])
        if stdout is not None:
            assert result.stdout == stdout, (len(result.stdout), result.stdout[:500])
        if stderr is not None:
            assert result.stderr == stderr, (len(result.stderr), result.stderr[:500])
    require(command('echo SSHD_EXEC_OK'), stdout=b'SSHD_EXEC_OK\n', stderr=b'')
    require(command('false'), status=1, stdout=b'', stderr=b'')
    missing = command('ls /sshd-probe-does-not-exist')
    require(missing, status=1, stdout=b'')
    assert b'sshd-probe-does-not-exist' in missing.stderr, missing
    print('guest: exec stdout, stderr and exit status PASS', flush=True)
    if not args.skip_fixture:
        fixture = command(args.fixture, timeout=60)
        require(fixture)
        assert b'0 failures' in fixture.stdout, fixture
        (args.output / 'fixture.log').write_bytes(fixture.stdout)
        print(fixture.stdout.decode().strip(), flush=True)
    data = (bytes(range(256)) * ((args.bytes + 255) // 256))[:args.bytes]
    require(command('cat', data, ['-o', 'RekeyLimit=16K']), stdout=data, stderr=b'')
    print(f'guest: {len(data)} binary bytes echoed across forced rekeys PASS', flush=True)
    if not args.skip_fixture:
        require(command(args.fixture + ' -streams'), status=7, stdout=b'stdout-marker\n', stderr=b'stderr-marker\n')
        require(command(args.fixture + ' -stderr', data), stdout=b'', stderr=data)
        print(f'guest: {len(data)} binary stderr bytes PASS', flush=True)
    long = command('x' * 256)
    require(long, status=255, stdout=b'')
    assert b'exec request failed' in long.stderr, long
    print('guest: overlong command refused PASS', flush=True)

    master, slave = pty.openpty()
    fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack('HHHH', 30, 88, 0, 0))
    proc = subprocess.Popen([*base, '-tt', 'os64@' + args.host], stdin=slave,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            env={**os.environ, 'TERM': 'xterm'})
    os.close(slave)
    transcript = bytearray()
    def until(needle, timeout=15, start=0):
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            if needle in transcript[start:]:
                return
            ready, _, _ = select.select([proc.stdout], [], [], .2)
            if ready:
                chunk = os.read(proc.stdout.fileno(), 8192)
                if not chunk:
                    break
                transcript.extend(chunk)
        raise AssertionError(('PTY did not produce', needle, bytes(transcript[-2000:])))
    try:
        until(b'husk>')
        # The requested terminal type must reach the seated shell as TERM.
        os.write(master, b'echo TERM=$TERM\r')
        until(b'\r\nTERM=xterm\r\n')
        os.write(master, b'cat /proc/self/tty\r')
        until(b'cols')
        time.sleep(.3)
        fcntl.ioctl(master, termios.TIOCSWINSZ, struct.pack('HHHH', 41, 103, 0, 0))
        os.kill(proc.pid, signal.SIGWINCH)
        time.sleep(.3)
        os.write(master, b'cat /proc/self/tty\r')
        until(b'103')
        until(b'41')
        assert re.search(rb'cols\s+103', transcript), transcript
        assert re.search(rb'rows\s+41', transcript), transcript
        assert b'\r\n' in transcript, transcript
        assert b'\n' not in bytes(transcript).replace(b'\r\n', b''), transcript
        # Refused dimensions must leave the real guest PTY unchanged;
        # accepted limits and unspecified zero components must agree with it.
        for case, (cols, rows, expected_cols, expected_rows) in enumerate([
                (600, 300, 103, 41), (1, 1, 103, 41),
                (512, 256, 512, 256), (0, 0, 512, 256), (103, 41, 103, 41)]):
            fcntl.ioctl(master, termios.TIOCSWINSZ, struct.pack('HHHH', rows, cols, 0, 0))
            os.kill(proc.pid, signal.SIGWINCH)
            time.sleep(.3)
            start = len(transcript)
            marker = f'SSHD_GEOMETRY_{case}'.encode()
            os.write(master, b'cat /proc/self/tty\recho ' + marker + b'\r')
            until(b'\r\n' + marker + b'\r\n', start=start)
            part = transcript[start:]
            assert re.search(rb'cols\s+' + str(expected_cols).encode() + rb'\s', part), part
            assert re.search(rb'rows\s+' + str(expected_rows).encode() + rb'\s', part), part
        print('guest: refused resize preserves geometry; maximum and zero dimensions agree PASS', flush=True)
        os.write(master, b'exit\r')
        proc.wait(timeout=15)
        assert proc.returncode == 0, proc.returncode
        print('guest: interactive shell and live PTY resize PASS', flush=True)
    finally:
        if proc.poll() is None:
            proc.kill()
            proc.wait()
        os.close(master)
        proc.stdout.close()
        (args.output / 'pty.log').write_bytes(transcript)
    # Abruptly remove a live client while the guest output reader is idle.
    drop = subprocess.Popen([*base, '-tt', 'os64@' + args.host], stdin=subprocess.PIPE,
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    time.sleep(1)
    drop.kill()
    drop.communicate(timeout=5)
    time.sleep(2)
    require(command('echo SSHD_AFTER_DROP'), stdout=b'SSHD_AFTER_DROP\n', stderr=b'')
    status = command('ps -e')
    require(status)
    (args.output / 'processes.log').write_bytes(status.stdout)
    print('guest: fresh command after dropped interactive connection PASS', flush=True)

if __name__ == '__main__':
    main()
