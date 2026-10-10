#!/usr/bin/env python3
"""Audit the pristine WebP import and freestanding decoder dependency closure."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
from webp_build import BASE, build

def command(*args):
    return subprocess.check_output(list(map(str, args)), text=True)

def audit(work):
    manifest = json.loads((BASE/'manifest.json').read_text())
    actual = {str(p.relative_to(BASE/'upstream')): p for p in (BASE/'upstream').rglob('*') if p.is_file()}
    assert set(actual) == set(manifest['files'])
    for name, path in actual.items():
        assert hashlib.sha256(path.read_bytes()).hexdigest() == manifest['files'][name], name
    notices = 'libwebp 1.6.0 — upstream notices\n\n'
    for name in ('COPYING', 'PATENTS', 'AUTHORS'):
        notices += f'--- {name} ---\n' + (BASE/'upstream'/name).read_text()
        if name != 'AUTHORS':
            notices += '\n'
    assert (BASE.parents[1]/'license/libwebp-LICENSE').read_text() == notices
    for mode in ('sse2', 'scalar'):
        directory = work / mode
        objects = build(directory, scalar=mode == 'scalar', guest=True)
        artifact = directory/'decoder.o'
        roots = ['WebPDecode', 'WebPGetFeaturesInternal', 'WebPInitDecoderConfigInternal',
                 'WebPFreeDecBuffer', 'WebPGetInfo']
        subprocess.run(['x86_64-elf-ld', '-r', '--gc-sections',
                        *['--undefined='+root for root in roots],
                        *map(str, objects), '-o', str(artifact)], check=True)
        # Relocatable GC leaves undefined symbol-table entries from discarded
        # functions. Strip unreferenced entries before auditing live imports;
        # symbols needed by surviving relocations remain in the object.
        subprocess.run(['x86_64-elf-objcopy', '--strip-unneeded', str(artifact)], check=True)
        undefined = {line.split()[-1] for line in command('x86_64-elf-nm', '-u', artifact).splitlines()}
        allowed = {'webp_port_malloc', 'webp_port_calloc', 'webp_port_free',
                   'os64_memcpy', 'os64_memmove', 'os64_memset', 'os64_memcmp'}
        assert undefined <= allowed, undefined - allowed
        disassembly = command('x86_64-elf-objdump', '-d', artifact)
        assert not re.search(r'\t(?:cpuid|xgetbv|syscall|sysenter|v[a-z][a-z0-9]*)\b', disassembly)
        symbols = command('x86_64-elf-nm', artifact)
        assert not re.search(r'\bTLS\b', command('x86_64-elf-readelf', '-Ws', artifact))
        assert not re.search(r'\b(?:WebPEncode\w*|pthread_\w*|GetColorPalette|PrepareMapToPalette)\b', symbols)
        if mode == 'sse2':
            assert 'VP8DspInitSSE2' in symbols and re.search(r'\t(?:paddw|pmaddwd|pshufd)\b', disassembly)
        else:
            assert 'VP8DspInitSSE2' not in symbols
        print(f'PASS {mode}: {len(objects)} sources; external imports: '+', '.join(sorted(undefined)))
    print(f"PASS {len(actual)} pristine files, exact manifest inventory")
    print('Scope: relocatable decoder core; production shared-library ABI and packaging need their own audit.')

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    audit(args.output.resolve())
