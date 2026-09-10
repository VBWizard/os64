#!/usr/bin/env python3
"""Verify the JPEG import, public interfaces and image-library dependency direction."""
import hashlib,json,re,subprocess
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
base=ROOT/'userland/libjpeg'
m=json.loads((base/'manifest.json').read_text())
files={p.name:p for p in (base/'upstream').iterdir() if p.is_file()}
assert set(files)==set(m['files'])
for n,p in files.items():assert hashlib.sha256(p.read_bytes()).hexdigest()==m['files'][n],n
bin=ROOT/'userland/bin'
def command(*args):return subprocess.check_output(list(map(str,args)),text=True)
for name,needed,prefix in [('libjpeg.so',['libos64.so'],'os64_jpeg_'),
                           ('libimage.so',['libpng.so','libjpeg.so','libos64.so'],'os64_image_')]:
    p=bin/name
    exports={l.split()[-1] for l in command('x86_64-elf-nm','-D','--defined-only',p).splitlines()}
    expect=set(re.findall(r'\b('+prefix+r'\w+);',(ROOT/'userland'/name[:-3]/'exports.map').read_text()))
    assert exports==expect,(name,exports,expect)
    dynamic=command('x86_64-elf-readelf','-dW',p)
    assert re.findall(r'\(NEEDED\).*\[(.*?)\]',dynamic)==needed,dynamic
    assert '(HASH)' in dynamic and not re.search(r'\((TEXTREL|INIT|FINI|INIT_ARRAY|FINI_ARRAY|VERDEF|VERNEED|VERSYM)\)',dynamic)
    assert set(re.findall(r'R_X86_64_\w+',command('x86_64-elf-readelf','-rW',p))) <= {'R_X86_64_RELATIVE','R_X86_64_JUMP_SLOT','R_X86_64_GLOB_DAT'}
    for l in command('x86_64-elf-readelf','-lW',p).splitlines():
        if l.lstrip().startswith(('LOAD','GNU_STACK')):assert not ('W' in l and 'E' in l),l
    header=ROOT/'userland'/name[:-3]/'include'
    subprocess.run(['x86_64-elf-gcc','-std=c11','-ffreestanding','-Wall','-Wextra','-Werror','-fsyntax-only','-x','c','-I'+str(header),'-'],input=f'#include <{name[3:-3]}/{name[3:-3]}.h>\n',text=True,check=True)
assert {l.split()[-1] for l in command('x86_64-elf-nm','-D','-u',bin/'libjpeg.so').splitlines()}=={'os64_malloc','os64_free','os64_memcpy','os64_memset'}
assert not re.search(r'\bos64_image_',command('x86_64-elf-nm','-D',bin/'libos64.so'))
assert not re.search(r'\b(jpeg_CreateCompress|jpeg_stdio_src|jpeg_std_error|jinit_arith_decoder)\b',command('x86_64-elf-nm',bin/'libjpeg.so'))
assert not re.search(r'\t(?:syscall|sysenter|cpuid)\b',command('x86_64-elf-objdump','-d',bin/'libjpeg.so'))
for name in ('gview','desktop','tests/jpegtest'):
    needed=re.findall(r'\(NEEDED\).*\[(.*?)\]',command('x86_64-elf-readelf','-dW',bin/name))
    assert 'libimage.so' in needed,(name,needed)
for n in ('LICENSE.md','README.ijg'):
    text=(base/'upstream'/n).read_bytes()
    assert text in (bin/'libjpeg.so').read_bytes()
    assert text in (ROOT/'license/libjpeg-turbo-LICENSE').read_bytes()
print(f'PASS JPEG: {len(files)} pristine imported files, four exports, four OS imports, hidden decoder, notices and ELF protection')
print('PASS image dependency direction, consumer loading and public-only headers')
