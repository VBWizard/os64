#!/usr/bin/env python3
"""Audit libsound.so: the import is what manifest.json pins, and the library is what its README says.

Run after a build (`make -C userland`). Each check names the property it
protects, so a failure says what was lost rather than only where.
"""
import hashlib
import json
import re
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
LIB = ROOT / 'userland/libsound'
SO = ROOT / 'userland/bin/libsound.so'


def run(*args):
    return subprocess.check_output([str(a) for a in args], text=True)


def check(cond, what):
    if not cond:
        raise SystemExit(f'FAIL {what}')


# The import: every vendored file is what manifest.json pins, and nothing is
# vendored that it does not name. (tools/import_sound.py --check proves the
# manifest itself is the pinned archive; it needs the archive, this does
# not.) Nothing is patched, and the manifest says so.
manifest = json.loads((LIB / 'manifest.json').read_text())
upstream = LIB / 'upstream'
present = {str(p.relative_to(upstream)) for p in upstream.rglob('*') if p.is_file()}
check(present == set(manifest['files']), f'upstream/ and manifest.json disagree: {sorted(present ^ set(manifest["files"]))}')
for name, digest in manifest['files'].items():
    check(hashlib.sha256((upstream / name).read_bytes()).hexdigest() == digest, f'{name} differs from manifest.json')
check(manifest['patches'] == [] and not (LIB / 'patches').exists(), 'the port is unpatched, and something says otherwise')

# Every vendored translation unit is built, and only vendored ones are.
sources = (LIB / 'sources.mk').read_text()
built = set(re.findall(r'libsound/upstream/(\S+\.c)', sources))
vendored = {f for f in manifest['files'] if f.endswith('.c')}
check(built == vendored, f'sources.mk and the vendored sources disagree: {sorted(built ^ vendored)}')

# The public surface: exports.map, os64/resample.h and the library's dynamic
# symbols name the same functions, and none of speexdsp's own.
exported = set(re.findall(r'\b(os64_\w+);', (LIB / 'exports.map').read_text().split('local:')[0]))
declared = set(re.findall(r'\b(os64_resampler_\w+)\(', (LIB / 'include/os64/resample.h').read_text().split('#pragma GCC visibility push')[1]))
dynamic = {l.split()[-1] for l in run('x86_64-elf-nm', '-D', '--defined-only', SO).splitlines()}
check(exported == declared, f'exports.map and os64/resample.h disagree: {sorted(exported ^ declared)}')
check(dynamic == exported, f'libsound.so exports {sorted(dynamic ^ exported)} beyond or short of exports.map')

# What it leans on: an allocator and three functions of libmath, and
# nothing else. A new import is a new dependency somebody should have
# decided on.
undefined = {l.split()[-1] for l in run('x86_64-elf-nm', '-D', '-u', SO).splitlines()}
allowed = {'os64_malloc', 'os64_calloc', 'os64_realloc', 'os64_free', 'sin', 'floor', 'fabs'}
check(undefined <= allowed, f'libsound.so imports {sorted(undefined - allowed)}')
dyn = run('x86_64-elf-readelf', '-dW', SO)
needed = set(re.findall(r'\(NEEDED\).*\[(.*?)\]', dyn))
check(needed == {'libos64.so', 'libmath.so'}, f'libsound.so needs {sorted(needed)}')
check('(HASH)' in dyn, 'libsound.so has no sysv hash table (the kernel sizes .dynsym from it)')
check(not re.search(r'\((TEXTREL|INIT|FINI|INIT_ARRAY|FINI_ARRAY)\)', dyn), 'libsound.so has text relocations or initialisers')

# No system call of its own: it computes, and asks libos64 for memory.
disasm = run('x86_64-elf-objdump', '-d', '--no-show-raw-insn', SO)
check(not re.search(r'\t(syscall|sysenter|int)\b', disasm), 'libsound.so contains a system call')

# The notice that travels with both images. The licence asks a binary
# distribution to reproduce "the above copyright notice, this list of
# conditions and the following disclaimer", and resample.c and its header
# carry a notice of their own that COPYING does not repeat (another holder,
# other wording). So the installed file must hold COPYING and the leading
# comment of every vendored source file, each byte for byte.
notice = (ROOT / 'license/speexdsp-LICENSE').read_text()
check((upstream / 'COPYING').read_text() in notice, 'license/speexdsp-LICENSE does not carry the vendored COPYING')
for name in manifest['files']:
    if name.endswith(('.c', '.h')):
        text = (upstream / name).read_text()
        # Through the end of the comment that holds the disclaimer: arch.h's
        # copyright line is a comment of its own, ahead of its conditions.
        head = text[:text.index('*/', text.index('SUCH DAMAGE')) + 2]
        check(head in notice, f'license/speexdsp-LICENSE does not carry the notice at the head of {name}')
root_make = (ROOT / 'GNUmakefile').read_text()
check(root_make.count('/etc/licenses/speexdsp.txt') == 2, 'the notice is not installed on both images')
check('userland/bin/libsound.so' in root_make, 'libsound.so is not in the images')

# The programs that use it load it.
for program in ('play', 'tests/resampletest'):
    needs = set(re.findall(r'\(NEEDED\).*\[(.*?)\]', run('x86_64-elf-readelf', '-dW', ROOT / 'userland/bin' / program)))
    check('libsound.so' in needs, f'{program} does not load libsound.so: {needs}')

print(f'PASS sound audit: {len(manifest["files"])} pinned files, {len(exported)} exports, imports {sorted(undefined)}')
