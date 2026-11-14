#!/usr/bin/env python3
"""Audit libav.so: the import is what manifest.json pins, the configuration
is LGPL and what configure was asked for, and the library is a library of
its own with the surface and the dependencies its README says.

Run after a build (`make -C userland`). Each check names the property it
protects, so a failure says what was lost rather than only where.
"""
import hashlib
import json
import re
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
USERLAND = ROOT / 'userland'
LIB = USERLAND / 'libav'
SO = USERLAND / 'bin/libav.so'


def run(*args):
    return subprocess.check_output([str(a) for a in args], text=True)


def check(cond, what):
    if not cond:
        raise SystemExit(f'FAIL {what}')


def defines(text):
    return dict(re.findall(r'^#define (\w+) (.*)$', text, re.M))


# The import: every vendored file is what manifest.json pins, and nothing is
# vendored that it does not name. (tools/import_av.py --check proves the
# manifest, port/config and sources.mk are what the pinned archive and
# configure produce; it needs the archive, this does not.) Nothing is
# patched, and the manifest says so.
manifest = json.loads((LIB / 'manifest.json').read_text())
upstream = LIB / 'upstream'
present = {str(p.relative_to(upstream)) for p in upstream.rglob('*') if p.is_file()}
check(present == set(manifest['files']), f'upstream/ and manifest.json disagree: {sorted(present ^ set(manifest["files"]))[:10]}')
for name, digest in manifest['files'].items():
    check(hashlib.sha256((upstream / name).read_bytes()).hexdigest() == digest, f'{name} differs from manifest.json')
check(manifest['patches'] == [] and not (LIB / 'patches').exists(), 'the port is unpatched, and something says otherwise')

# THE LICENCE IS THE CONFIGURATION'S. FFmpeg is LGPL 2.1 unless configure is
# asked for GPL, nonfree or version 3 parts; none may be asked for, and the
# generated configuration must agree (README.md § The licence).
args = manifest['configure_args']
for forbidden in ('--enable-gpl', '--enable-nonfree', '--enable-version3'):
    check(forbidden not in args, f'configure_args asks for {forbidden}')
config = defines((LIB / 'port/config/config.h').read_text())
for name in ('CONFIG_GPL', 'CONFIG_NONFREE', 'CONFIG_VERSION3'):
    check(config.get(name) == '0', f'config.h has {name} {config.get(name)}')
check(config.get('FFMPEG_LICENSE') == '"LGPL version 2.1 or later"', f'config.h says {config.get("FFMPEG_LICENSE")}')
check(manifest['license'] == 'LGPL-2.1-or-later', 'manifest.json names another licence')

# The configuration is the one asked for, with every override in force.
# (configure quotes a list value, --enable-demuxer='wav,mp3', in what it records.)
check(config.get('FFMPEG_CONFIGURATION', '').strip('"').replace("'", '').split() == args,
      'config.h was configured with other arguments than manifest.json\'s')
for name, value in manifest['config_overrides'].items():
    want = str(value) if isinstance(value, int) else json.dumps(value)
    check(config.get(name) == want, f'config.h has {name} {config.get(name)}, manifest.json says {want}')
for name in ('HAVE_PTHREADS', 'HAVE_X86ASM', 'HAVE_INLINE_ASM', 'CONFIG_NETWORK'):
    check(config.get(name) == '0', f'config.h has {name} {config.get(name)}: README.md says it is off')

# Every translation unit sources.mk builds is vendored.
built = set(re.findall(r'upstream/(\S+\.c)', (LIB / 'sources.mk').read_text()))
check(built and built <= set(manifest['files']), f'sources.mk builds unvendored files: {sorted(built - set(manifest["files"]))}')

# The public surface: FFmpeg's own, by the prefixes its version scripts
# export, and nothing of FFmpeg's internals or of the port's shims.
dynamic = {l.split()[-1] for l in run('x86_64-elf-nm', '-D', '--defined-only', SO).splitlines()}
public = re.compile(r'^(av_|avcodec_|avformat_|avio_|avutil_|avsubtitle_free$)')
check(dynamic and all(public.match(s) for s in dynamic), f'libav.so exports {sorted(s for s in dynamic if not public.match(s))[:10]}')
for needed_api in ('avformat_open_input', 'avcodec_send_packet', 'avcodec_receive_frame', 'avio_alloc_context'):
    check(needed_api in dynamic, f'libav.so does not export {needed_api}')

# What it leans on: libos64's allocator and strings, its exit and clock,
# and libmath's double-precision functions. A new import is a new
# dependency somebody should have decided on.
undefined = {l.split()[-1] for l in run('x86_64-elf-nm', '-D', '-u', SO).splitlines()}
allowed_os64 = {'os64_malloc', 'os64_realloc', 'os64_free', 'os64_memchr', 'os64_memcmp', 'os64_memcpy',
                'os64_memmove', 'os64_memset', 'os64_strchr', 'os64_strcmp', 'os64_strlen', 'os64_strrchr',
                'os64_snprintf', 'os64_vsnprintf', 'os64_micros', 'os64_exit', 'memset', 'memcpy'}
math = set(re.findall(r'\b([a-z0-9]+);', (USERLAND / 'libmath/exports.map').read_text().split('local:')[0]))
check(undefined <= allowed_os64 | math, f'libav.so imports {sorted(undefined - allowed_os64 - math)}')
dyn = run('x86_64-elf-readelf', '-dW', SO)
needed = set(re.findall(r'\(NEEDED\).*\[(.*?)\]', dyn))
check(needed == {'libos64.so', 'libmath.so'}, f'libav.so needs {sorted(needed)}')
check('(HASH)' in dyn, 'libav.so has no sysv hash table (the kernel sizes .dynsym from it)')
check(not re.search(r'\((TEXTREL|INIT|FINI|INIT_ARRAY|FINI_ARRAY)\)', dyn),
      'libav.so has text relocations or initialisers (the kernel runs no initialisers)')
check('TLS' not in run('x86_64-elf-readelf', '-lW', SO), 'libav.so has thread-local storage (the kernel has none)')

# No system call of its own: every door FFmpeg reaches for is a shim.
disasm = run('x86_64-elf-objdump', '-d', '--no-show-raw-insn', SO)
check(not re.search(r'\t(syscall|sysenter|int)\b', disasm), 'libav.so contains a system call')

# A LIBRARY OF ITS OWN: the LGPL is met by a library the user can replace,
# so no other program or library may carry FFmpeg's code linked in. The
# test is every FFmpeg name libav.so defines (its full symbol table, local
# ones included: ff_*, avpriv_* and the public prefixes), against what each
# other binary defines. A binary with no symbol table cannot be checked, and
# says so, rather than passing.
ffmpeg_prefix = re.compile(r'^(ff_|avpriv_|av_|avcodec_|avformat_|avio_|avutil_)')
ours = {l.split()[-1] for l in run('x86_64-elf-nm', '--defined-only', SO).splitlines()
        if len(l.split()) == 3 and ffmpeg_prefix.match(l.split()[-1])}
check(len(ours) > 1000, f'libav.so\'s symbol table names only {len(ours)} FFmpeg symbols: is it stripped?')
for path in sorted((USERLAND / 'bin').rglob('*')):
    if not path.is_file() or path == SO or path.read_bytes()[:4] != b'\x7fELF':
        continue
    lines = run('x86_64-elf-nm', '--defined-only', path).splitlines()
    check(lines, f'{path.relative_to(USERLAND)} has no symbol table, so the audit cannot see what it carries')
    theirs = {l.split()[-1] for l in lines if len(l.split()) == 3}
    shared = sorted(theirs & ours)
    check(not shared, f'{path.relative_to(USERLAND)} carries FFmpeg code of its own: {shared[:5]}')

# The notice that travels with both images: FFmpeg's LICENSE.md and the
# LGPL it names, byte for byte, and where the source is.
notice = (ROOT / 'license/ffmpeg-LICENSE').read_text()
check((upstream / 'LICENSE.md').read_text() in notice, 'license/ffmpeg-LICENSE does not carry LICENSE.md')
check((upstream / 'COPYING.LGPLv2.1').read_text() in notice, 'license/ffmpeg-LICENSE does not carry the LGPL')
check('userland/libav' in notice, 'license/ffmpeg-LICENSE does not say where the source is')
root_make = (ROOT / 'GNUmakefile').read_text()
check(root_make.count('/etc/licenses/ffmpeg.txt') == 2, 'the notice is not installed on both images')
check('userland/bin/libav.so' in root_make, 'libav.so is not in the images')

# The programs that use it load it.
for program in ('tests/avtest',):
    needs = set(re.findall(r'\(NEEDED\).*\[(.*?)\]', run('x86_64-elf-readelf', '-dW', USERLAND / 'bin' / program)))
    check('libav.so' in needs, f'{program} does not load libav.so: {needs}')

print(f'PASS av audit: FFmpeg {manifest["release"]}, {len(manifest["files"])} pinned files, LGPL configuration, '
      f'{len(dynamic)} exports, {len(undefined)} imports')
