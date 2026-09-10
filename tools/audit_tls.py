#!/usr/bin/env python3
"""Audit the installed TLS ABI, dependency closure and guest consumer."""
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[1]
USERLAND = ROOT / 'userland'
BASE = USERLAND / 'libtls'
LIBRARY = USERLAND / 'bin/libtls.so'
FIXTURE = USERLAND / 'bin/tests/tlslibtest'
TRANSPORT_PROBE = USERLAND / 'bin/tests/tlstransportprobe'


def command(*args):
    return subprocess.check_output([str(a) for a in args], text=True)


subprocess.run(['make', '-C', str(USERLAND), str(LIBRARY), str(FIXTURE), str(TRANSPORT_PROBE)], check=True)
expected = set(re.findall(r'\b(os64_tls_\w+);', (BASE / 'exports.map').read_text()))
public = set(re.findall(r'\b(os64_tls_\w+)\(', '\n'.join(p.read_text() for p in (BASE / 'include/tls').glob('*.h'))))
assert expected == public, (expected - public, public - expected)
exports = {line.split()[-1] for line in command('x86_64-elf-nm', '-D', '--defined-only', LIBRARY).splitlines()}
assert exports == expected, (exports - expected, expected - exports)
assert all(line.split()[-2] == 'T' for line in command('x86_64-elf-nm', '-D', '--defined-only', LIBRARY).splitlines())
assert re.search(r'Type:\s+DYN\b', command('x86_64-elf-readelf', '-h', LIBRARY))
imports = {line.split()[-1] for line in command('x86_64-elf-nm', '-D', '-u', LIBRARY).splitlines()}
assert imports == {'os64_malloc', 'os64_free', 'os64_memcpy', 'os64_memmove', 'os64_memset',
                   'os64_strlen', 'os64_open', 'os64_read', 'os64_close', 'os64_time',
                   'os64_conf_find', 'os64_slurp', 'os64_ticks', 'os64_read_for', 'os64_write_for'}, imports
for binary, dependencies in ((LIBRARY, ['libos64.so']), (FIXTURE, ['libtls.so', 'libos64.so']),
                             (TRANSPORT_PROBE, ['libtls.so', 'libos64.so'])):
    dynamic = command('x86_64-elf-readelf', '-dW', binary)
    assert re.findall(r'\(NEEDED\).*\[(.*?)\]', dynamic) == dependencies, dynamic
    assert '(HASH)' in dynamic
    assert not re.search(r'\((TEXTREL|INIT|FINI|INIT_ARRAY|FINI_ARRAY|VERDEF|VERNEED|VERSYM)\)', dynamic), dynamic
    relocations = command('x86_64-elf-readelf', '-rW', binary)
    assert set(re.findall(r'R_X86_64_\w+', relocations)) <= {
        'R_X86_64_RELATIVE', 'R_X86_64_JUMP_SLOT', 'R_X86_64_GLOB_DAT'}, relocations
    segments = command('x86_64-elf-readelf', '-lW', binary)
    for line in segments.splitlines():
        if line.lstrip().startswith(('LOAD', 'GNU_STACK')):
            assert not ('W' in line and 'E' in line), line
    undefined = command('x86_64-elf-nm', '-u', binary)
    assert not re.search(r'\b(br_|os64_tls_engine_|os64_tls_policy_|tls_fixture_)', undefined), undefined
assert re.findall(r'\(SONAME\).*\[(.*?)\]', command('x86_64-elf-readelf', '-dW', LIBRARY)) == ['libtls.so']
assert (BASE / 'upstream/LICENSE.txt').read_bytes() in LIBRARY.read_bytes()
symbols = command('x86_64-elf-nm', LIBRARY)
assert not re.search(r'\b(tls_fixture_|bearssl_test_|br_ssl_server_|time_test_certificate)', symbols), 'fixture/server code linked'
assembly = command('x86_64-elf-objdump', '-d', LIBRARY)
assert not re.search(r'\t(?:rdrand|rdseed|syscall|sysenter)\b', assembly), 'unexpected RNG/syscall instruction'
manifest = re.findall(r'libtls/[^\s\\]+\.c', (BASE / 'public_sources.mk').read_text())
assert len(manifest) == len(set(manifest))
assert len(manifest) == len({Path(name).name for name in manifest}), 'ambiguous archive member names'
assert all((USERLAND / name).is_file() for name in manifest)
assert all('/test/' not in name and '/ssl_server' not in name for name in manifest)
archive = USERLAND / 'obj/tls-shared/core.a'
assert set(command('x86_64-elf-ar', 't', archive).splitlines()) == {Path(name).name + '.o' for name in manifest}
link = (USERLAND / 'obj/tls-shared/link.map').read_text()
linked = set(re.findall(r'/tls-shared/core\.a\(([^)]+)\)', link))
assert linked == {Path(name).name + '.o' for name in manifest}, 'unused or unlisted production archive member'
# Compile the consumer header with only its installed include root.
subprocess.run(['x86_64-elf-gcc', '-std=c11', '-ffreestanding', '-Wall', '-Wextra', '-Werror', '-fsyntax-only',
                '-x', 'c', '-I' + str(BASE / 'include'), '-'],
               input='#include <tls/tls.h>\n#include <tls/transport.h>\nos64_tls_client *client;\n', text=True, check=True)
print(f'TLS public ELF audit PASS: {len(expected)} exports, {len(imports)} OS imports, {len(manifest)} selected core sources')
print('PASS public-only header, guest dependency, hidden internals, supported relocations, W^X and license')
