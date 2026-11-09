#!/usr/bin/env python3
"""Peer for /tests/tcpwriteprobe HOST PORT: stall, resume, verify through EOF."""
import argparse
import socket

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--bind', default='127.0.0.1')
parser.add_argument('--port', type=int, default=17260)
args = parser.parse_args()
with socket.socket() as server:
    server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    server.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)
    server.bind((args.bind, args.port)); server.listen(2); server.settimeout(120)
    print(f'TCPWRITE peer listening on {args.bind}:{args.port}', flush=True)
    control, _ = server.accept()
    with control:
        data, _ = server.accept()
        with data:
            control.settimeout(30); data.settimeout(45)
            control.sendall(b'R')
            line = bytearray()
            while not line.endswith(b'\n'):
                byte = control.recv(1)
                assert byte and len(line) < 64, 'missing/oversized resume command'
                line.extend(byte)
            verb, count = line.decode('ascii').split()
            assert verb == 'resume'
            expected = int(count); assert 32768 <= expected <= 16 * 1024 * 1024 + 32768
            total = 0
            while True:
                part = data.recv(65536)
                if not part: break
                assert total + len(part) <= expected, 'unexpected stream suffix'
                wanted = bytes((((total+i) * 2654435761) & 0xffffffff) >> 24 for i in range(len(part)))
                assert part == wanted, f'wrong stream bytes at offset {total}'
                total += len(part)
            assert total == expected, (total, expected)
            control.sendall(b'K')
            print(f'TCPWRITE peer PASS: {total} exact bytes through EOF after pause/resume', flush=True)
