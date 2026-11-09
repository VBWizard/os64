#!/usr/bin/env python3
"""Inspect a real ClientHello and test rejection of disallowed ServerHellos."""
import argparse
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile

sys.dont_write_bytecode = True
from check_bearssl_import import ROOT, BASE
from test_bearssl_host import check


class Reader:
    def __init__(self, data):
        self.data = data
        self.at = 0

    def take(self, count):
        if self.at + count > len(self.data):
            raise AssertionError("truncated protocol field")
        result = self.data[self.at:self.at + count]
        self.at += count
        return result

    def number(self, count):
        return int.from_bytes(self.take(count), "big")

    def vector(self, size):
        return self.take(self.number(size))

    def done(self):
        return self.at == len(self.data)


def inspect(hello):
    record = Reader(hello)
    assert record.number(1) == 22 and record.number(2) == 0x0303
    handshake = Reader(record.vector(2))
    assert record.done() and handshake.number(1) == 1
    client = Reader(handshake.vector(3))
    assert handshake.done() and client.number(2) == 0x0303
    client.take(32)
    assert client.vector(1) == b""  # Fresh connection, no session resumption.
    suites = client.vector(2)
    offered = list(struct.unpack(">" + "H" * (len(suites) // 2), suites))
    assert len(offered) == len(set(offered))
    assert [s for s in offered if s != 0x00ff] == [0xcca9, 0xcca8, 0xc02b, 0xc02f, 0xc02c, 0xc030]
    assert client.vector(1) == b"\0"
    extensions = Reader(client.vector(2))
    assert client.done()
    fields = {}
    while not extensions.done():
        kind = extensions.number(2)
        assert kind not in fields
        fields[kind] = extensions.vector(2)
    names = Reader(fields[0])
    name = Reader(names.vector(2))
    assert names.done() and name.number(1) == 0 and name.vector(2) == b"example.test" and name.done()
    algorithms = Reader(fields[13])
    pairs = algorithms.vector(2)
    assert algorithms.done() and len(pairs) == 12
    assert set(zip(pairs[0::2], pairs[1::2])) == {(h, s) for h in (4, 5, 6) for s in (1, 3)}
    groups = Reader(fields[10])
    curves = groups.vector(2)
    assert groups.done() and set(struct.unpack(">4H", curves)) == {29, 23, 24, 25}
    assert 43 not in fields  # No TLS 1.3 supported_versions offer.
    assert fields[16] == b"\0\x09\x08http/1.1"
    print("TLS profile: ClientHello version, six suites, SHA-2 signatures, curves, SNI, and empty session PASS")


def server_hello(version, suite, alpn=None):
    extensions = b""
    if alpn is not None:
        selected = struct.pack(">H", len(alpn) + 1) + bytes([len(alpn)]) + alpn
        extensions = struct.pack(">HH", 16, len(selected)) + selected
    body = (struct.pack(">H", version) + bytes(32) + b"\0" + struct.pack(">H", suite)
            + b"\0" + struct.pack(">H", len(extensions)) + extensions)
    handshake = b"\x02" + len(body).to_bytes(3, "big") + body
    return b"\x16\x03\x03" + len(handshake).to_bytes(2, "big") + handshake


def profile(archive, work):
    executable = work / "profile"
    subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-O2", "-g",
        "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
        "-fno-sanitize-recover=all", "-include", str(BASE / "port/config.h"),
        "-I" + str(BASE / "upstream/inc"), str(BASE / "port/client_profile.c"),
        str(ROOT / "tools/test_tls_profile_host.c"), str(archive), "-o", str(executable)], check=True)
    inspect(subprocess.check_output([executable]))
    # Error values are pinned upstream details, not os64's eventual public ABI.
    for version, suite, expected in [(0x0301, 0xc02f, 3), (0x0302, 0xc02f, 3),
                                      (0x0303, 0x002f, 16), (0x0303, 0xc013, 16)]:
        output = subprocess.check_output([executable, "reject"], input=server_hello(version, suite))
        if output != f"{expected}\n".encode():
            raise AssertionError(f"unexpected rejection for version {version:x}, suite {suite:x}: {output!r}")
    print("TLS profile: fragmented TLS 1.0/1.1 and unoffered RSA/CBC ServerHellos rejected PASS")
    for alpn, expected in [(None, 0), (b"http/1.1", 0), (b"h2", 10)]:
        output = subprocess.check_output([executable, "reject"],
            input=server_hello(0x0303, 0xc02f, alpn))
        assert output == f"{expected}\n".encode(), (alpn, output)
    print("TLS profile: absent/matching ALPN accepted, unoffered ALPN rejected PASS")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--foundation", type=Path,
        help="reuse an adapted/core.a from a successful host foundation run of this pin/configuration")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="tls-profile-") as directory:
        work = Path(directory)
        if args.foundation:
            archive = args.foundation.resolve()
        else:
            check(work)
            archive = work / "adapted/core.a"
        profile(archive, work)
