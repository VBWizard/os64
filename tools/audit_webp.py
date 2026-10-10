#!/usr/bin/env python3
"""Audit the pristine WebP import and freestanding decoder dependency closure."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import tempfile
from webp_build import BASE, ROOT, build

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

def shared():
    binary = ROOT/'userland/bin/libwebp.so'
    exports = {line.split()[-1] for line in command('x86_64-elf-nm','-D','--defined-only',binary).splitlines()}
    assert exports == {'os64_webp_decode','os64_webp_free','os64_webp_status_name','os64_webp_license'}, exports
    imports = {line.split()[-1] for line in command('x86_64-elf-nm','-D','-u',binary).splitlines()}
    assert imports == {'os64_malloc','os64_free','os64_memcpy','os64_memmove','os64_memset','os64_memcmp','os64_sleep'}, imports
    for name, needed in [('libwebp.so',['libos64.so']),
                         ('libimage.so',['libpng.so','libjpeg.so','libwebp.so','libos64.so'])]:
        path = binary.parent/name
        dynamic = command('x86_64-elf-readelf','-dW',path)
        assert re.findall(r'\(NEEDED\).*\[(.*?)\]',dynamic) == needed, dynamic
        assert '(HASH)' in dynamic and not re.search(r'\((TEXTREL|INIT|FINI|INIT_ARRAY|FINI_ARRAY|VERDEF|VERNEED|VERSYM)\)',dynamic)
        assert set(re.findall(r'R_X86_64_\w+',command('x86_64-elf-readelf','-rW',path))) <= {'R_X86_64_RELATIVE','R_X86_64_JUMP_SLOT','R_X86_64_GLOB_DAT'}
        for line in command('x86_64-elf-readelf','-lW',path).splitlines():
            if line.lstrip().startswith(('LOAD','GNU_STACK')):
                assert not ('W' in line and 'E' in line), line
        assert not re.search(r'\bTLS\b',command('x86_64-elf-readelf','-Ws',path))
    disassembly = command('x86_64-elf-objdump','-d',binary)
    assert not re.search(r'\t(?:cpuid|xgetbv|syscall|sysenter|v[a-z][a-z0-9]*)\b',disassembly)
    symbols = command('x86_64-elf-nm',binary)
    assert not re.search(r'\b(?:WebPEncode\w*|pthread_\w*|GetColorPalette|PrepareMapToPalette)\b',symbols)
    notice = (ROOT/'license/libwebp-LICENSE').read_bytes()
    assert notice in binary.read_bytes()
    for name in ('COPYING','PATENTS','AUTHORS'):
        assert (BASE/'upstream'/name).read_bytes() in notice
    subprocess.run(['x86_64-elf-gcc','-std=c11','-ffreestanding','-Wall','-Wextra','-Werror',
                    '-fsyntax-only','-x','c','-I'+str(BASE/'include'),'-'],
                   input='#include <webp/webp.h>\n',text=True,check=True)
    for name in ('gview','desktop','yonder','tests/webptest'):
        assert 'libimage.so' in command('x86_64-elf-readelf','-dW',binary.parent/name)
    print('PASS shared WebP: four exports, seven OS imports, decoder closure, notice bytes, public header, W^X and supported relocations')
    print('PASS libimage dependency and gview/desktop/Yonder/webptest loading')

def packaged():
    payloads = [('lib/libwebp.so','userland/bin/libwebp.so'),
                ('lib/libimage.so','userland/bin/libimage.so'),
                ('tests/webptest','userland/bin/tests/webptest'),
                ('tests/webpbench','userland/bin/tests/webpbench'),
                ('tests/webpbenchscalar','userland/bin/tests/webpbenchscalar'),
                ('etc/licenses/libwebp.txt','license/libwebp-LICENSE')]
    payloads += [('tests/pages/'+p.name,str(p.relative_to(ROOT)))
                 for p in sorted((ROOT/'tools/pages').glob('webp-*'))]
    with tempfile.TemporaryDirectory(prefix='webp-images-') as directory:
        for target, source in payloads:
            for filesystem in ('ext2','fat'):
                extracted = Path(directory)/(filesystem+'-'+Path(target).name)
                if filesystem == 'ext2':
                    subprocess.run(['debugfs','-R',f'dump /{target} {extracted}',str(ROOT/'disk/ext2_test.img')],
                                   stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL,check=True)
                else:
                    subprocess.run(['mcopy','-o','-i',str(ROOT/'disk/os64.img')+'@@1048576',
                                    '::/'+target,str(extracted)],check=True)
                assert extracted.read_bytes() == (ROOT/source).read_bytes(), (filesystem,target)
    print(f'PASS ext2/FAT: {len(payloads)} library, test, notice and visual-fixture payloads match source bytes')

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--shared', action='store_true', help='also audit the built guest libraries and consumers')
    parser.add_argument('--images', action='store_true', help='also verify ext2/FAT payload bytes after make')
    args = parser.parse_args()
    audit(args.output.resolve())
    if args.shared:
        shared()
    if args.images:
        packaged()
