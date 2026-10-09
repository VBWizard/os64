#!/usr/bin/env python3
"""Exercise the public TLS 1.3 byte API against OpenSSL, with real certificate policy."""
import argparse
import os
from pathlib import Path
import subprocess
import sys
import tempfile
sys.dont_write_bytecode = True
from check_bearssl_import import ROOT, BASE
from test_bearssl_host import check
from test_tls_policy_host import certificate
from cryptography import x509
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec, rsa, padding


def fixtures(work):
    root_key = rsa.generate_private_key(public_exponent=65537, key_size=2048)
    root = certificate(root_key, 'TLS 1.3 test root', None, root_key, ca=True, san=None, eku=False)
    (work/'root.der').write_bytes(root)
    other_key = ec.generate_private_key(ec.SECP256R1())
    (work/'other.der').write_bytes(certificate(other_key, 'other root', None, other_key, ca=True, san=None, eku=False))
    issuer = x509.load_der_x509_certificate(root).subject
    for name, key in [('ec', ec.generate_private_key(ec.SECP256R1())),
                      ('ec384', ec.generate_private_key(ec.SECP384R1())),
                      ('ec521', ec.generate_private_key(ec.SECP521R1())),
                      ('rsa', rsa.generate_private_key(public_exponent=65537, key_size=2048))]:
        der = certificate(key, 'example.test', issuer, root_key)
        (work/(name+'.pem')).write_bytes(x509.load_der_x509_certificate(der).public_bytes(serialization.Encoding.PEM))
        (work/(name+'.key')).write_bytes(key.private_bytes(serialization.Encoding.PEM,
            serialization.PrivateFormat.PKCS8, serialization.NoEncryption()))
        if name == 'rsa':
            cert = x509.load_der_x509_certificate(der)
            builder = (x509.CertificateBuilder().subject_name(cert.subject).issuer_name(cert.issuer)
                .public_key(key.public_key()).serial_number(cert.serial_number)
                .not_valid_before(cert.not_valid_before).not_valid_after(cert.not_valid_after))
            for extension in cert.extensions:
                builder = builder.add_extension(extension.value, extension.critical)
            pss = builder.sign(root_key, hashes.SHA256(),
                rsa_padding=padding.PSS(mgf=padding.MGF1(hashes.SHA256()), salt_length=32))
            (work/'pss.der').write_bytes(pss.public_bytes(serialization.Encoding.DER))
            (work/'pss.key').write_bytes((work/'rsa.key').read_bytes())
    return work


def build(archive, work, run_tests=True):
    fixtures(work)
    subprocess.run([sys.executable, str(ROOT/'tools/tls_license.py'), str(work/'tls_license.h')], check=True)
    sources = ['api.c', 'port/client_engine.c', 'port/client_profile.c', 'port/platform_inputs.c',
               'port/certificate_der.c', 'port/certificate_policy.c',
               'port/tls13_schedule.c', 'port/tls13_record.c', 'port/tls13_handshake.c']
    flags = ['-std=c11','-D_DEFAULT_SOURCE','-DOS64_TLS13_TEST','-O2','-g','-Wall','-Wextra','-Werror','-fno-builtin',
             '-fsanitize=address,undefined','-fno-sanitize-recover=all','-fstack-usage',
             '-include',str(BASE/'port/config.h'),'-I'+str(BASE/'upstream/inc'),
             '-I'+str(ROOT/'userland/libos64/include'),'-I'+str(ROOT/'abi/include'),'-I'+str(work)]
    exe = work/'engine'
    subprocess.run([os.environ.get('CC','cc'), *flags, *[str(BASE/p) for p in sources],
                    str(ROOT/'tools/test_tls13_engine_host.c'), str(archive), '-lssl','-lcrypto','-o',str(exe)],check=True)
    if run_tests:
        subprocess.run([str(exe),str(work)],check=True)
    return exe


if __name__ == '__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--foundation',type=Path)
    parser.add_argument('--output',type=Path)
    args=parser.parse_args()
    def run(work):
        if args.foundation: archive=args.foundation.resolve()
        else:
            check(work)
            archive=work/'adapted/core.a'
        build(archive,work)
    if args.output:
        work=args.output.resolve(); work.mkdir(); run(work)
    else:
        with tempfile.TemporaryDirectory(prefix='tls13-engine-') as directory: run(Path(directory))
