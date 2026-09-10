#!/usr/bin/env python3
"""Controlled os64get HTTPS peer; Python standard library, Linux or Windows.

The fixture key is public test material. --fixtures writes roots.pem and
expected.bin for an explicitly selected test trust store and byte comparison.
"""
import argparse
import ast
import base64
import gzip
from pathlib import Path
import re
import socket
import ssl
import tempfile
import threading
import time

ROOT = Path(__file__).resolve().parents[1]
PAYLOAD = bytes(i * 13 % 256 for i in range(65536))


def fixtures():
    source = (ROOT / 'userland/libtls/test/trust_vectors.c').read_text(encoding='utf-8')
    pem = re.search(r'tls_fixture_a_pem\[\] =\s*(.*?);', source, re.S)[1]
    root = ''.join(ast.literal_eval(s) for s in re.findall(r'"(?:[^"\\]|\\.)*"', pem)).encode('ascii')
    body = re.search(r'tls_fixture_a_leaf\[\] = \{(.*?)\};', source, re.S)[1]
    der = bytes(int(h, 16) for h in re.findall(r'0x([0-9a-f]{2})', body))
    cert = b'-----BEGIN CERTIFICATE-----\n' + base64.encodebytes(der) + b'-----END CERTIFICATE-----\n'
    # Read the existing peer's public fixture constant without running its server.
    tree = ast.parse((ROOT / 'tools/test_tls_transport_peer.py').read_text(encoding='utf-8'))
    key = next(ast.literal_eval(n.value) for n in tree.body if isinstance(n, ast.Assign)
               and any(isinstance(t, ast.Name) and t.id == 'FIXTURE_KEY_PEM' for t in n.targets))
    return root, cert, key


def reply(path, port):
    if path == '/redirect':
        return b'HTTP/1.1 302 Found\r\nLocation: /gzip-chunked\r\nContent-Length: 0\r\n\r\n'
    if path == '/downgrade':
        return (f'HTTP/1.1 302 Found\r\nLocation: http://example.test:{port}/length\r\n'
                'Content-Length: 0\r\n\r\n').encode('ascii')
    if path == '/headers-cut':
        return b'HTTP/1.1 200 OK\r\nContent-Len'
    encoded = path.startswith('/gzip-') or path == '/bad-gzip'
    data = gzip.compress(PAYLOAD, mtime=0) if encoded else PAYLOAD
    if path == '/bad-gzip':
        data = data[:-8] + bytes([data[-8] ^ 1]) + data[-7:]
    if path == '/empty':
        data = b''
    head = b'HTTP/1.1 200 OK\r\nConnection: close\r\n'
    if encoded:
        head += b'Content-Encoding: gzip\r\n'
    if 'chunked' in path:
        head += b'Transfer-Encoding: chunked\r\n\r\n'
        wire = b''.join(f'{len(data[i:i+113]):x}\r\n'.encode() + data[i:i+113] + b'\r\n'
                        for i in range(0, len(data), 113))
        return head + wire + (b'' if path == '/cut-chunked' else b'0\r\nX-Test: complete\r\n\r\n')
    if 'close' in path:
        return head + b'\r\n' + data
    return head + f'Content-Length: {len(data)}\r\n\r\n'.encode() + (data[:31] if path == '/cut-length' else data)


def serve_connection(raw, context, port):
    connection = raw
    try:
        raw.settimeout(40)
        if context:
            connection = context.wrap_socket(raw, server_side=True)
        request = b''
        while b'\r\n\r\n' not in request:
            chunk = connection.recv(2048)
            if not chunk:
                return
            request += chunk
            if len(request) > 8192:
                raise ValueError('request too large')
        line = request.split(b'\r\n', 1)[0].decode('ascii')
        path = line.split(' ')[1]
        print(('HTTPS ' if context else 'HTTP ') + path, flush=True)
        if path in ('/stall', '/cancel'):
            time.sleep(35)
            return
        connection.sendall(reply(path, port))
        if context:
            if path in ('/cut-close', '/bare-length', '/headers-cut'):
                socket.close(connection.detach())  # TCP FIN without close_notify.
            else:
                connection.settimeout(3)
                connection.unwrap().close()
    except (OSError, ValueError) as error:
        print(f'peer: {error}', flush=True)
    finally:
        connection.close()
        raw.close()


def listen(bind, port, context, plain_port, stop_handshake=False):
    with socket.socket() as server:
        server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        server.bind((bind, port))
        server.listen(8)
        print(f'Listening {bind}:{port}', flush=True)
        while True:
            raw, _ = server.accept()
            if stop_handshake:
                def stall(connection):
                    with connection:
                        time.sleep(35)
                target, arguments = stall, (raw,)
            else:
                target, arguments = serve_connection, (raw, context, plain_port)
            threading.Thread(target=target, args=arguments, daemon=True).start()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bind', default='127.0.0.1')
    parser.add_argument('--port', type=int, default=18270)
    parser.add_argument('--stall-port', type=int, default=18271)
    parser.add_argument('--http-port', type=int, default=18272)
    parser.add_argument('--fixtures', type=Path)
    parser.add_argument('--fixtures-only', action='store_true')
    args = parser.parse_args()
    root, cert, key = fixtures()
    if args.fixtures:
        args.fixtures.mkdir(parents=True, exist_ok=True)
        (args.fixtures / 'roots.pem').write_bytes(root)
        (args.fixtures / 'expected.bin').write_bytes(PAYLOAD)
    if args.fixtures_only:
        if not args.fixtures:
            parser.error('--fixtures-only requires --fixtures DIRECTORY')
        return
    with tempfile.TemporaryDirectory(prefix='os64get-https-') as directory:
        work = Path(directory)
        (work / 'cert.pem').write_bytes(cert)
        (work / 'key.pem').write_bytes(key)
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.minimum_version = context.maximum_version = ssl.TLSVersion.TLSv1_2
        context.set_ciphers('ECDHE-ECDSA-AES128-GCM-SHA256')
        context.set_alpn_protocols(['http/1.1'])
        context.load_cert_chain(work / 'cert.pem', work / 'key.pem')
        for port, kwargs in ((args.http_port, {}), (args.stall_port, {'stop_handshake': True})):
            threading.Thread(target=listen, args=(args.bind, port, None, args.http_port),
                             kwargs=kwargs, daemon=True).start()
        listen(args.bind, args.port, context, args.http_port)


if __name__ == '__main__':
    main()
