#!/usr/bin/env python3
"""Prove TLS 1.3 S1 against RFC traces and independent host crypto under ASan/UBSan."""
import argparse
import hashlib
import hmac
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

sys.dont_write_bytecode = True
from check_bearssl_import import BASE, ROOT
from test_bearssl_host import check
from cryptography.hazmat.primitives.asymmetric import ec, x25519
from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.ciphers.aead import AESGCM, ChaCha20Poly1305


def independent_header():
    """Fixture expectations come from Python HMAC/hashlib and cryptography, not Bear."""
    lines = ['// Generated host expectations; public fixture keys, not installed.']
    schedules, records, exchanges = [], [], []
    message = bytes(range(256))
    for suite, hash_name, key_len in [(0x1301, 'sha256', 16), (0x1302, 'sha384', 32), (0x1303, 'sha256', 32)]:
        def digest(data):
            return hashlib.new(hash_name, data).digest()

        def extract(salt, ikm):
            return hmac.digest(salt, ikm, hash_name)

        def expand(secret, label, context, length):
            name = b'tls13 ' + label.encode('ascii')
            info = length.to_bytes(2, 'big') + bytes([len(name)]) + name + bytes([len(context)]) + context
            return hmac.digest(secret, info + b'\x01', hash_name)[:length]

        hl = len(digest(b''))
        def derive(secret, label, transcript):
            return expand(secret, label, digest(transcript), hl)

        zero = bytes(hl)
        early = extract(zero, zero)
        derived = derive(early, 'derived', b'')
        shared = bytes(range(hl))
        hs = extract(derived, shared)
        chs = derive(hs, 'c hs traffic', message)
        shs = derive(hs, 's hs traffic', message)
        master = extract(derive(hs, 'derived', b''), zero)
        cap = derive(master, 'c ap traffic', message + message[:17])
        sap = derive(master, 's ap traffic', message + message[:17])
        key = expand(sap, 'key', b'', key_len)
        iv = expand(sap, 'iv', b'', 12)
        finished = extract(expand(chs, 'finished', b'', hl), digest(message))
        updated = expand(sap, 'traffic upd', b'', hl)
        retry_hash = digest(b'\xfe\x00\x00' + bytes([hl]) + digest(message + message[:17]))
        updated_key = expand(updated, 'key', b'', key_len)
        updated_iv = expand(updated, 'iv', b'', 12)
        updated_aead = ChaCha20Poly1305(updated_key) if suite == 0x1303 else AESGCM(updated_key)
        header = b'\x17\x03\x03\x00\x15'
        updated_wire = header + updated_aead.encrypt(updated_iv, b'next\x17', header)
        schedules.append([suite, early, derived, shared, hs, chs, shs, master, cap, sap,
                          key, iv, finished, updated, retry_hash, updated_key, updated_iv, updated_wire])
        key, iv = bytes(range(key_len)), bytes(range(12))
        aead = ChaCha20Poly1305(key) if suite == 0x1303 else AESGCM(key)
        cases = []
        for seq in (0, 1, 2**32, 2**64-1):
            cases.append((seq, bytes(range(97)) + b'\x17', 0, True))
        for size in (0, 1, 16384):
            cases.append((0, bytes(i % 251 for i in range(size)) + b'\x17', 0, True))
        cases += [
            (0, b'abc\x17' + bytes(128), 0, True),
            (0, b'x\x17' + bytes(16383), 0, True),
            (0, b'x\x17' + bytes(16384), 6, True),
            (0, bytes(32), 10, True),
            (0, b'', 7, True),
            (0, b'\x01\x14', 12, True),
            (0, b'x\x19', 10, True),
            (0, b'\x16', 10, True),
            (0, b'\x15', 13, True),
            (0, b'\x01\x00\x00\x15', 13, True),
            (0, b'x\x17', 10, False),
            (0, b'handshake bytes\x16', 0, False),
            (0, b'\x01\x00\x15', 0, False),
        ]
        for seq, inner, error, application in cases:
            nonce = (int.from_bytes(iv, 'big') ^ seq).to_bytes(12, 'big')
            header = b'\x17\x03\x03' + (len(inner)+16).to_bytes(2, 'big')
            wire = header + aead.encrypt(nonce, inner, header)
            records.append([suite, seq, key, iv, inner, wire, error, application])
    for group, curve in [(23, ec.SECP256R1()), (24, ec.SECP384R1())]:
        a, b = ec.derive_private_key(7, curve), ec.derive_private_key(11, curve)
        size = (curve.key_size + 7) // 8
        exchanges.append([group, (7).to_bytes(size, 'big'),
            b.public_key().public_bytes(serialization.Encoding.X962, serialization.PublicFormat.UncompressedPoint),
            a.exchange(ec.ECDH(), b.public_key())])
        # Find a fixed-width shared X coordinate whose first byte is zero.
        for peer_scalar in range(1, 4096):
            b = ec.derive_private_key(peer_scalar, curve)
            shared = a.exchange(ec.ECDH(), b.public_key())
            if shared[0] == 0:
                exchanges.append([group, (7).to_bytes(size, 'big'),
                    b.public_key().public_bytes(serialization.Encoding.X962, serialization.PublicFormat.UncompressedPoint), shared])
                break
        else:
            raise AssertionError('no leading-zero ECDH fixture')
    a = x25519.X25519PrivateKey.from_private_bytes(bytes(range(32)))
    b = x25519.X25519PrivateKey.from_private_bytes(bytes(range(32,64)))
    exchanges.append([29, bytes(range(32))[::-1],
        b.public_key().public_bytes(serialization.Encoding.Raw, serialization.PublicFormat.Raw), a.exchange(b.public_key())])
    def emit(kind, name, rows):
        lines.append(f'static const {kind} {name}[] = {{')
        for row in rows:
            values = []
            for value in row:
                if isinstance(value, bytes):
                    hexvalue = value.hex()
                    values.append('\n'.join(json.dumps(hexvalue[i:i+96]) for i in range(0,max(len(hexvalue),1),96)))
                elif isinstance(value, bool): values.append('true' if value else 'false')
                elif value > 2**32-1: values.append(f'UINT64_C({value})')
                else: values.append(str(value))
            lines.append('    {' + ',\n     '.join(values) + '},')
        lines.append('};')
    emit('diff_schedule', 'diff_schedules', schedules)
    emit('diff_ecdh', 'diff_exchanges', exchanges)
    emit('diff_record', 'diff_records', records)
    return '\n'.join(lines) + '\n'


def vectors(archive, work):
    subprocess.run([sys.executable, str(ROOT/'tools/tls13_vectors.py'), '--check'], check=True)
    (work/'tls13_independent.h').write_text(independent_header())
    cc = os.environ.get('CC', 'cc')
    flags = ['-std=c11','-O2','-g','-Wall','-Wextra','-Werror','-fno-builtin',
             '-fsanitize=address,undefined','-fno-sanitize-recover=all','-fstack-usage',
             '-include',str(BASE/'port/config.h'),
             '-I'+str(BASE/'upstream/inc'),'-I'+str(ROOT/'userland/libos64/include'),
             '-I'+str(ROOT/'abi/include'),'-I'+str(work)]
    objects = []
    for source in [BASE/'port/tls13_schedule.c',BASE/'port/tls13_record.c',ROOT/'tools/test_tls13_vectors_host.c']:
        obj = work/(source.stem+'.o')
        subprocess.run([cc,*flags,'-c',str(source),'-o',str(obj)],check=True)
        objects.append(obj)
    exe = work/'tls13_vectors'
    subprocess.run([cc,*flags,*map(str,objects),str(archive),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--foundation',type=Path,help='reuse an adapted/core.a from a successful foundation run of this pin/configuration')
    parser.add_argument('--output',type=Path,help='retain artifacts in a new directory')
    args = parser.parse_args()
    def run(work):
        if args.foundation:
            archive = args.foundation.resolve()
        else:
            check(work)
            archive = work/'adapted/core.a'
        vectors(archive,work)
    if args.output:
        work = args.output.resolve(); work.mkdir()
        run(work)
    else:
        with tempfile.TemporaryDirectory(prefix='tls13-vectors-') as directory:
            run(Path(directory))
