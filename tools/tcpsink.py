#!/usr/bin/env python3
"""tcpsink.py — the far end of /tests/netsend: accept, drain, verify, answer.

    python3 tools/tcpsink.py [--port 7200]

One connection at a time. The stream opens with one line, `netsend <BYTES>`,
then exactly that many bytes, each checked against the LCG stream netsend
generates (seed 0x5EED, Numerical Recipes' constants, the byte from bits
16..23 of each state). When the last byte is in, ONE BYTE goes back — `K`
if every byte was right, `W` if not — and that byte is what netsend's
clock stops on: a sender's write returns when its bytes are queued, so
only the receiver can say when they arrived. The line printed here names
the count, the verdict, the offset where the bytes went wrong if they did,
and the wall time from accept to the last byte, as a second opinion on the
guest's number. Runs until killed.
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


ANNOUNCE_MAX = 64          # bytes through the newline; longer is not an announce
BYTES_MAX = 1 << 30        # a stream past this is a typo, not a test


def read_announce(conn):
    """The opening line — `netsend <BYTES>\\n` — with whatever recv delivered
    past the newline handed back as the stream's first bytes. The line is
    capped BEFORE it is parsed: this sink listens on every interface, and a
    stranger's kilobyte of digits must be a refused announce, not a
    ValueError that ends the run."""
    line = bytearray()
    while True:
        chunk = conn.recv(65536)
        if not chunk:
            return None, b""
        nl = chunk.find(b"\n", 0, ANNOUNCE_MAX - len(line))
        if nl < 0:
            if len(line) + len(chunk) >= ANNOUNCE_MAX:
                return None, b""
            line += chunk
            continue
        line += chunk[:nl]
        rest = chunk[nl + 1:]
        words = line.split()
        if (len(words) != 2 or words[0] != b"netsend" or not words[1].isdigit()
                or len(words[1]) > 12):
            return None, rest
        n = int(words[1])
        if n == 0 or n > BYTES_MAX:
            return None, rest
        return n, rest


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
        expected, data = read_announce(conn)
        if expected is None:
            print(f"tcpsink: {peer[0]}:{peer[1]} did not announce a stream — closed", flush=True)
            conn.close()
            continue
        x = SEED
        got = 0
        wrong_at = None
        while True:
            if data:
                take = data[:expected - got]
                want, x = pattern(len(take), x)
                if wrong_at is None and take != want:
                    for i, (a, b) in enumerate(zip(take, want)):
                        if a != b:
                            wrong_at = got + i
                            break
                got += len(take)
            if got >= expected:
                break
            data = conn.recv(65536)
            if not data:
                break
        took = time.monotonic() - started
        if got < expected:
            verdict = f"SHORT: {got} of {expected} bytes, then the sender hung up"
        elif wrong_at is None:
            verdict = "ok"
        else:
            verdict = f"WRONG from byte {wrong_at}"
        try:
            if got >= expected:
                conn.sendall(b"K" if wrong_at is None else b"W")
        except OSError as e:
            verdict += f" (verdict undeliverable: {e})"
        conn.close()
        print(f"tcpsink: {peer[0]}:{peer[1]} sent {got} bytes in {took:.2f}s — {verdict}", flush=True)


if __name__ == "__main__":
    main()
