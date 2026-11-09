#!/usr/bin/env python3
"""EOF-based peer for /tests/netclose HOST PORT; verifies the entire stream."""
import argparse
import socket

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--port', type=int, default=17250)
args = parser.parse_args()
expected = bytes((i * 37 + 11) & 255 for i in range(32768))
with socket.socket() as server:
    server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    server.bind(('127.0.0.1', args.port))
    server.listen(1)
    server.settimeout(120)
    print(f'netclose sink listening on {args.port}', flush=True)
    conn, peer = server.accept()
    with conn:
        conn.settimeout(60)
        received = bytearray()
        while True:
            part = conn.recv(65536)
            if not part:
                break
            received.extend(part)
            if len(received) > len(expected):
                raise AssertionError('bytes beyond the expected stream')
        assert received == expected, f'bad/truncated stream: {len(received)} bytes'
    print('netclose PASS: 32768 correct bytes followed by EOF', flush=True)
