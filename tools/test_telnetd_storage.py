#!/usr/bin/env python3
"""Hold closing Telnet connections after their session children exit.

Run against an idle private guest with telnetd enabled. DONT ECHO ends each
session, but the peer withholds its own FIN. The guest's FIN_WAIT_2 rings
must remain charged until TCP strips them. The host TCP stack acknowledges
the server's FIN; withholding that ACK is covered by test_tcp_host.sh.
"""
import argparse
import re
import socket
import time
from pathlib import Path
from test_telnetd_limits import Peer, IAC, DONT, ECHO


def snapshot(observer):
    data = observer.command(b"cat /sys/net/tcp")
    values = {k.decode(): int(v) for k, v in re.findall(rb"(?:^|\n)([a-z_]+): (\d+)", data)}
    return data, values


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=2323)
    ap.add_argument("--output", type=Path, required=True)
    args = ap.parse_args()
    held, transcript = [], bytearray()
    observer = Peer(args.host, args.port)
    try:
        data, stats = snapshot(observer)
        transcript += data
        assert stats["passive_buffered"] == 1 and stats["passive_buffer_limit"] == 32, stats
        start = time.monotonic()
        for _ in range(31):
            sock = socket.create_connection((args.host, args.port), timeout=5)
            held.append(sock)
            sock.settimeout(5)
            sock.sendall(bytes([IAC, DONT, ECHO]))
            received = bytearray()
            while True:
                chunk = sock.recv(8192)
                if not chunk:
                    break
                received += chunk
            assert b"this server echoes" in received, received
        elapsed = time.monotonic() - start
        assert elapsed < 25, f"fixture took {elapsed:.1f}s; early closes may expire before saturation"
        print(f"31 session processes exited; peers retain their closing sockets ({elapsed:.1f}s)", flush=True)
        data, stats = snapshot(observer)
        transcript += data
        assert stats["passive_buffered"] == 32, stats
        assert len(re.findall(rb" FIN_WAIT_2 ", data)) == 31, data
        before = stats["syns_dropped_storage"]
        # Slirp may complete the host-facing connect before the guest's
        # handshake. An excess peer must receive no Telnet session bytes.
        extra = None
        try:
            extra = socket.create_connection((args.host, args.port), timeout=1)
            extra.settimeout(1)
            try:
                chunk = extra.recv(1024)
            except socket.timeout:
                chunk = b""
            assert not chunk, chunk
        except (socket.timeout, ConnectionResetError, ConnectionRefusedError):
            pass
        finally:
            if extra:
                extra.close()
        data, stats = snapshot(observer)
        transcript += data
        assert stats["passive_buffered"] == 32 and stats["syns_dropped_storage"] > before, stats
        print("storage cap holds at 32 despite exited children; excess SYN counted", flush=True)
        held.pop().close()
        end = time.monotonic() + 5
        while True:
            data, stats = snapshot(observer)
            if stats["passive_buffered"] < 32:
                break
            assert time.monotonic() < end, stats
            time.sleep(0.1)
        transcript += data
        replacement = Peer(args.host, args.port)
        try:
            data = replacement.command(b"echo STORAGE-REUSED")
            transcript += data
            assert b"\r\nSTORAGE-REUSED\r\n" in data, data
        finally:
            replacement.close()
        print("peer FIN releases storage and admits a replacement session", flush=True)
    finally:
        for sock in held:
            sock.close()
        observer.close()
        args.output.write_bytes(transcript)


if __name__ == "__main__":
    main()
