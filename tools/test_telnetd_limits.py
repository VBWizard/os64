#!/usr/bin/env python3
"""Exercise Telnet session limits, negotiated resize and erase on an idle guest.

python3 tools/test_telnetd_limits.py --port 2323 --output /tmp/telnetd-limits.log
The target must have telnetd running with no existing sessions. This opens
16 shells, refuses eight excess peers, and checks reuse after a disconnect.
"""
import argparse
import re
import socket
import struct
import time
from pathlib import Path

IAC, WILL, WONT, DO, DONT, SB, SE, EC, EL = 255, 251, 252, 253, 254, 250, 240, 247, 248
ECHO, SGA, NAWS = 1, 3, 31


class Peer:
    """One streaming decoder per connection, including fragmented IAC/SB."""
    def __init__(self, host, port):
        self.sock = socket.create_connection((host, port), timeout=10)
        self.sock.settimeout(0.2)
        self.state = "data"
        self.verb = None
        self.data = bytearray()
        self.eof = False

    def send(self, data):
        self.sock.sendall(data)

    def receive(self):
        try:
            raw = self.sock.recv(16384)
        except socket.timeout:
            return
        except ConnectionResetError:
            self.eof = True
            return
        if not raw:
            self.eof = True
        for c in raw:
            if self.state == "data":
                if c == IAC:
                    self.state = "iac"
                elif c:
                    self.data.append(c)
            elif self.state == "iac":
                if c in (WILL, WONT, DO, DONT):
                    self.verb, self.state = c, "option"
                elif c == SB:
                    self.state = "sb"
                else:
                    if c == IAC:
                        self.data.append(c)
                    self.state = "data"
            elif self.state == "option":
                if self.verb == WILL:
                    self.send(bytes([IAC, DO if c in (ECHO, SGA) else DONT, c]))
                elif self.verb == DO and c != NAWS:
                    self.send(bytes([IAC, WILL if c == SGA else WONT, c]))
                # NAWS is negotiated explicitly by the test, not automatically.
                self.state = "data"
            elif self.state == "sb":
                if c == IAC:
                    self.state = "sb_iac"
            elif self.state == "sb_iac":
                self.state = "data" if c == SE else "sb"

    def until(self, marker, timeout=15):
        end = time.monotonic() + timeout
        while marker not in self.data and not self.eof and time.monotonic() < end:
            self.receive()
        assert marker in self.data, f"missing {marker!r}, eof={self.eof}: {self.data[-1500:]!r}"
        return bytes(self.data)

    def command(self, command, timeout=15):
        self.data.clear()
        self.send(command + b"\r\necho RD11-DONE\r\n")
        return self.until(b"\r\nRD11-DONE\r\n", timeout)

    def close(self):
        self.sock.close()


def size(cols, rows):
    payload = struct.pack("!HH", cols, rows).replace(b"\xff", b"\xff\xff")
    return bytes([IAC, SB, NAWS]) + payload + bytes([IAC, SE])


def geometry(peer, cols, rows):
    data = peer.command(b"cat /proc/self/tty")
    for field, value in ((b"cols", cols), (b"rows", rows)):
        assert re.search(rb"(?:^|\n)" + field + rb"\s+" + str(value).encode() + rb"\s", data), data
    return data


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=2323)
    ap.add_argument("--output", type=Path, required=True)
    args = ap.parse_args()
    peers, transcript = [], bytearray()
    try:
        for _ in range(16):
            peer = Peer(args.host, args.port)
            peers.append(peer)
            peer.command(b"echo RD11-ADMITTED")
        print("16 active sessions admitted", flush=True)
        for _ in range(8):
            extra = Peer(args.host, args.port)
            try:
                end = time.monotonic() + 5
                while not extra.eof and time.monotonic() < end:
                    extra.receive()
                assert extra.eof and not extra.data, "excess session was not closed"
            finally:
                extra.close()
        print("8 excess peers closed; existing sessions remain usable", flush=True)
        peer = peers[0]
        transcript += peer.command(b"ps")
        peer.send(size(111, 37))
        transcript += geometry(peer, 80, 24)
        peer.send(bytes([IAC, WILL, NAWS]) + size(111, 37))
        transcript += geometry(peer, 111, 37)
        peer.send(bytes([IAC, WONT, NAWS]) + size(90, 30))
        transcript += geometry(peer, 111, 37)
        print("NAWS ignored before WILL and after WONT; enabled resize applied", flush=True)
        peer.send(bytes([IAC, WILL, NAWS]))
        peer.send((size(512, 256) + size(80, 24)) * 2500 + size(103, 41))
        transcript += geometry(peer, 103, 41)
        print("5001 resize frames completed; final geometry correct", flush=True)
        for line, marker in ((b"echo RD11-ERASEx" + bytes([IAC, EC]), b"RD11-ERASE"),
                             (b"garbage" + bytes([IAC, EL]) + b"echo RD11-LINE", b"RD11-LINE")):
            data = peer.command(line)
            transcript += data
            assert b"\r\n" + marker + b"\r\n" in data, data
        print("IAC EC and EL edit the actual shell input", flush=True)
        peers.pop().close()
        time.sleep(3)
        replacement = Peer(args.host, args.port)
        peers.append(replacement)
        transcript += replacement.command(b"echo RD11-REUSED")
        print("departed session reaped and slot reused", flush=True)
    finally:
        for peer in peers:
            peer.close()
        args.output.write_bytes(transcript)


if __name__ == "__main__":
    main()
