#!/usr/bin/env python3
"""tcpsink.py — the far end of /tests/netsend: accept, drain, verify.

    python3 tools/tcpsink.py [--port 7200]

One connection at a time. Every byte is checked against the LCG stream
netsend generates (seed 0x5EED, Numerical Recipes' constants, the byte from
bits 16..23 of each state), so the line printed per connection says not
just how many bytes arrived but whether they were the right ones, and at
which offset they stopped being right. Runs until killed; the guest's clock
is the one that matters for a measurement, but the wall time here is
printed too, for a second opinion.
"""
import socket
import sys
import time

SEED = 0x5EED


def pattern(n, x):
    out = bytearray(n)
    for i in range(n):
        x = (x * 1103515245 + 12345) & 0xFFFFFFFF
        out[i] = (x >> 16) & 0xFF
    return bytes(out), x


def main():
    port = 7200
    args = sys.argv[1:]
    if args[:1] == ["--port"] and len(args) >= 2:
        port = int(args[1])
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("0.0.0.0", port))
    srv.listen(4)
    print(f"tcpsink: listening on {port}", flush=True)
    while True:
        conn, peer = srv.accept()
        started = time.monotonic()
        x = SEED
        got = 0
        wrong_at = None
        while True:
            data = conn.recv(65536)
            if not data:
                break
            want, x = pattern(len(data), x)
            if wrong_at is None and data != want:
                for i, (a, b) in enumerate(zip(data, want)):
                    if a != b:
                        wrong_at = got + i
                        break
            got += len(data)
        conn.close()
        took = time.monotonic() - started
        verdict = "ok" if wrong_at is None else f"WRONG from byte {wrong_at}"
        print(f"tcpsink: {peer[0]}:{peer[1]} sent {got} bytes in {took:.2f}s — {verdict}", flush=True)


if __name__ == "__main__":
    main()
