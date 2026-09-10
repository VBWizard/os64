#!/usr/bin/env python3
"""OpenSSL-backed peer for tlstransportprobe; uses the public scalar-3 fixture key."""
import argparse
import base64
from pathlib import Path
import re
import socket
import ssl
import tempfile

# Public test material: the P-256 scalar-3 key used by tls_fixture_a_leaf.
# Keeping its PEM here lets the peer run with standard Python on Windows or
# Linux; generating the certificate fixtures is a separate tool's job.
FIXTURE_KEY_PEM = b"""-----BEGIN EC PRIVATE KEY-----
MHcCAQEEIAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAADoAoGCCqGSM49
AwEHoUQDQgAEXsvk0aYzCkTI9++VHUvxZebGtyHvramF+0FmG8bn/WyHNGQMSZj/
fjdLBs4aZKLs2CqwNjhPuD2aebEnon1QMg==
-----END EC PRIVATE KEY-----
"""

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--bind', default='127.0.0.1')
parser.add_argument('--port', type=int, default=17270)
parser.add_argument('--cases', default='good,stall,truncated,badname')
args = parser.parse_args()
source = (Path(__file__).resolve().parents[1] / 'userland/libtls/test/trust_vectors.c').read_text(encoding='utf-8')
body = re.search(r'tls_fixture_a_leaf\[\] = \{(.*?)\};', source, re.S)[1]
der = bytes(int(h, 16) for h in re.findall(r'0x([0-9a-f]{2})', body))
with tempfile.TemporaryDirectory(prefix='tls-transport-peer-') as directory:
    work = Path(directory)
    (work/'cert.pem').write_bytes(b'-----BEGIN CERTIFICATE-----\n'+base64.encodebytes(der)+b'-----END CERTIFICATE-----\n')
    (work/'key.pem').write_bytes(FIXTURE_KEY_PEM)
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.minimum_version = context.maximum_version = ssl.TLSVersion.TLSv1_2
    context.set_ciphers('ECDHE-ECDSA-AES128-GCM-SHA256')
    context.load_cert_chain(work/'cert.pem', work/'key.pem')
    with socket.socket() as server:
        server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        server.bind((args.bind, args.port)); server.listen(2); server.settimeout(120)
        print(f'TLSTRANSPORT peer listening {args.bind}:{args.port} ({ssl.OPENSSL_VERSION})', flush=True)
        for mode in args.cases.split(','):
            raw, _ = server.accept(); raw.settimeout(25)
            with raw:
                if mode == 'stall':
                    # Consume ClientHello without replying; the client's retained deadline must expire.
                    while raw.recv(4096): pass
                elif mode == 'badname':
                    try:
                        with context.wrap_socket(raw, server_side=True) as connection:
                            connection.recv(1)
                    except (ssl.SSLError, ConnectionError): pass
                    else: raise AssertionError('wrong hostname accepted')
                else:
                    with context.wrap_socket(raw, server_side=True) as connection:
                        if mode == 'truncated':
                            # Send FIN without close_notify, then drain raw input so
                            # unread application records cannot turn close into RST.
                            with socket.socket(fileno=connection.detach()) as plain:
                                plain.settimeout(25); plain.shutdown(socket.SHUT_WR)
                                while plain.recv(4096): pass
                        else:
                            assert mode == 'good', mode
                            total = 0
                            while total < 65536:
                                data = connection.recv(min(997, 65536-total))
                                assert data and data == bytes(((total+i)*13)&255 for i in range(len(data)))
                                total += len(data); connection.sendall(data)
                            with connection.unwrap(): pass
                print(f'TLSTRANSPORT peer PASS {mode}', flush=True)
