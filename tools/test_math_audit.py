#!/usr/bin/env python3
"""Audit libmath.so: the import is what manifest.json pins, the boundary is what MATH.md says.

Run after a build (`make -C userland`). Each check names the property it
protects, so a failure says what was lost rather than only where.
"""
import hashlib
import json
import re
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
LIB = ROOT / 'userland/libmath'
SO = ROOT / 'userland/bin/libmath.so'


def run(*args):
    return subprocess.check_output([str(a) for a in args], text=True)


def check(cond, what):
    if not cond:
        raise SystemExit(f'FAIL {what}')


# The import: every vendored file is what manifest.json pins, nothing is
# vendored that it does not name, and every patch it names is the file in
# patches/. (tools/import_math.py --check proves the manifest itself is the
# pinned archive plus those patches; it needs the archive, this does not.)
manifest = json.loads((LIB / 'manifest.json').read_text())
upstream = LIB / 'upstream'
present = {str(p.relative_to(upstream)) for p in upstream.rglob('*') if p.is_file()}
check(present == set(manifest['files']), f'upstream/ and manifest.json disagree: {sorted(present ^ set(manifest["files"]))}')
for name, digest in manifest['files'].items():
    check(hashlib.sha256((upstream / name).read_bytes()).hexdigest() == digest, f'{name} differs from manifest.json')
patches = sorted(p.name for p in (LIB / 'patches').glob('*.patch'))
check(patches == [p['file'] for p in manifest['patches']], 'patches/ and manifest.json name different patches')
for p in manifest['patches']:
    check(hashlib.sha256((LIB / 'patches' / p['file']).read_bytes()).hexdigest() == p['sha256'], f'{p["file"]} differs from manifest.json')

# Every vendored routine is built, and only vendored routines are: an
# unbuilt file is dead weight in every review, a built one outside the
# manifest is unpinned code.
sources = (LIB / 'sources.mk').read_text()
names = re.findall(r'MATH_(?:PUBLIC|SUPPORT)_NAMES := (.*?)\n\n', sources, re.S)
built = {f'src/math/{n}.c' for group in names for n in group.replace('\\', ' ').split()}
vendored = {f for f in manifest['files'] if f.startswith('src/math/') and f.endswith('.c')}
check(built == vendored, f'sources.mk and the vendored routines disagree: {sorted(built ^ vendored)}')

# The public surface: exports.map, <math.h> and the library's dynamic
# symbols name the same 32 functions.
exported = set(re.findall(r'\b([a-z0-9]+);', (LIB / 'exports.map').read_text().split('local:')[0]))
declared = set(re.findall(r'^(?:double|long) (\w+)\(', (LIB / 'include/math.h').read_text(), re.M))
dynamic = {l.split()[-1] for l in run('x86_64-elf-nm', '-D', '--defined-only', SO).splitlines()}
check(len(exported) == 32, f'exports.map names {len(exported)} functions, not 32')
check(exported == declared, f'exports.map and <math.h> disagree: {sorted(exported ^ declared)}')
check(dynamic == exported, f'libmath.so exports {sorted(dynamic ^ exported)} beyond or short of exports.map')

# A leaf: no undefined symbol, no DT_NEEDED, nothing to relocate at load,
# no initialisers, and no segment both writable and executable.
check(run('x86_64-elf-nm', '-D', '-u', SO).strip() == '', 'libmath.so has undefined symbols')
dyn = run('x86_64-elf-readelf', '-dW', SO)
check('(NEEDED)' not in dyn, 'libmath.so depends on another library')
check('(HASH)' in dyn, 'libmath.so has no sysv hash table (the kernel sizes .dynsym from it)')
check(not re.search(r'\((TEXTREL|INIT|FINI|INIT_ARRAY|FINI_ARRAY)\)', dyn), 'libmath.so has text relocations or initialisers')
check('R_X86_64' not in run('x86_64-elf-readelf', '-rW', SO), 'libmath.so carries dynamic relocations')
for line in run('x86_64-elf-readelf', '-lW', SO).splitlines():
    if line.lstrip().startswith(('LOAD', 'GNU_STACK')):
        flags = line.split()[-2]
        check(not ('W' in flags and 'E' in flags), f'a segment is writable and executable: {line.strip()}')

# sqrt is the instruction: sqrtsd and ret, no call (MATH.md § Source
# inventory), and musl's generic sqrt and its table are not in the image.
disasm = run('x86_64-elf-objdump', '-d', '-M', 'intel', '--no-show-raw-insn', SO)
body = re.search(r'<sqrt>:\n(.*?)(?:\n\n|\n*\Z)', disasm, re.S).group(1)
ops = [l.split('\t')[-1].split()[0] for l in body.splitlines() if '\t' in l]
check(ops == ['sqrtsd', 'ret'], f'sqrt is {ops}, not sqrtsd; ret')
symbols = run('x86_64-elf-nm', SO)
check('__sqrt_data' not in symbols, "musl's generic sqrt table was linked")

# No way out of the process: arithmetic makes no system call.
check(not re.search(r'\t(syscall|sysenter|int)\b', disasm), 'libmath.so contains a system call')

# The notice travels: musl's COPYRIGHT, whole, in the shipped licence file.
copyright_text = (upstream / 'COPYRIGHT').read_bytes()
check(copyright_text in (ROOT / 'license/libmath-LICENSE').read_bytes(), "license/libmath-LICENSE does not carry musl's COPYRIGHT")

# The fixture loads the library it tests.
needed = re.findall(r'\(NEEDED\).*\[(.*?)\]', run('x86_64-elf-readelf', '-dW', ROOT / 'userland/bin/tests/mathtest'))
check('libmath.so' in needed, f'mathtest does not load libmath.so: {needed}')

print(f'PASS libmath: {len(present)} pinned files ({len(patches)} patched upstream), {len(built)} routines built, '
      f'32 exports, a leaf with no relocations, sqrt = sqrtsd, notice shipped')
