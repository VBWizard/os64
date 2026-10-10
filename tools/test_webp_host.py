#!/usr/bin/env python3
"""Exercise the production WebP wrapper, framing, budgets, cleanup and contention."""
import argparse
import hashlib
import io
import json
import os
from pathlib import Path
import struct
import subprocess
from PIL import Image
from webp_build import ROOT, BASE, build

def chunk(tag, p):
    return tag + struct.pack('<I', len(p)) + p + bytes(len(p) & 1)

def riff(*chunks):
    p = b'WEBP' + b''.join(chunks)
    return b'RIFF' + struct.pack('<I', len(p)) + p

def chunks(data):
    at = 12
    while at < len(data):
        n = struct.unpack_from('<I', data, at+4)[0]
        yield data[at:at+4], data[at+8:at+8+n]
        at += 8+n+(n&1)

def fixtures(work):
    paths = []
    def save(name, data, expected=None):
        path = work/(name+'.webp'); path.write_bytes(data)
        if expected is None:
            image = Image.open(io.BytesIO(data)).convert('RGBA')
            expected = struct.pack('<II', *image.size) + image.tobytes('raw', 'BGRA')
        path.with_suffix('.ref').write_bytes(expected); paths.append(path)
        return expected
    source = Image.new('RGBA', (31, 19))
    source.putdata([((x*17+y*7)%256, (y*31+x*3)%256, (x*11+y*19)%256,
                     [0, 64, 128, 255][(x+y)%4]) for y in range(19) for x in range(31)])
    raw = {}
    for name, image, lossless in [('lossy', source.convert('RGB'), False),
                                  ('alpha', source, False), ('lossless', source, True),
                                  ('palette', Image.new('RGBA', (31,19), (20,40,90,80)), True)]:
        out = io.BytesIO(); image.save(out, 'WEBP', lossless=lossless, quality=90, exact=True)
        raw[name] = out.getvalue(); save(name, raw[name])
    vp8 = next(p for t,p in chunks(raw['lossy']) if t == b'VP8 ')
    vp8l = next(p for t,p in chunks(raw['lossless']) if t == b'VP8L')
    def extended(flags=0, w=31, h=19, extra=b''):
        return chunk(b'VP8X', bytes([flags,0,0,0])+(w-1).to_bytes(3,'little')+(h-1).to_bytes(3,'little')+extra)
    pixels = (work/'lossy.ref').read_bytes()
    save('trailing', raw['lossy']+b'external trailing bytes', pixels)
    save('unknown-tail', riff(chunk(b'VP8 ',vp8),chunk(b'what',b'odd')), pixels)
    save('metadata', riff(extended(0x2c),chunk(b'EXIF',b'not interpreted'),chunk(b'ICCP',b'profile'),
                         chunk(b'VP8 ',vp8),chunk(b'XMP ',b'meta'),chunk(b'EXIF',b'duplicate')), pixels)
    save('inactive-anim', riff(extended(),chunk(b'ANIM',b'ignored'),chunk(b'VP8 ',vp8)), pixels)
    save('vp8x-reserved', riff(extended(0xc1,extra=b'future'),chunk(b'VP8 ',vp8)), pixels)
    raw_alpha = riff(extended(16),chunk(b'ALPH',b'\0'+source.getchannel('A').tobytes()),chunk(b'VP8 ',vp8))
    alpha_pixels = save('alpha-raw', raw_alpha)
    for header in (0xc0,0x20,0x30):
        save(f'alpha-ignored-{header}', riff(extended(16),chunk(b'ALPH',bytes([header])+source.getchannel('A').tobytes()),chunk(b'VP8 ',vp8)), alpha_pixels)
    compressed = [(t,p) for t,p in chunks(raw['alpha'])]
    save('alpha-interleaved', riff(*[chunk(t,p)+(chunk(b'what',b'odd') if t==b'ALPH' else b'') for t,p in compressed]), (work/'alpha.ref').read_bytes())
    animation = riff(extended(2),chunk(b'ANIM',bytes(6)),chunk(b'ANMF',bytes(6)+(30).to_bytes(3,'little')+(18).to_bytes(3,'little')+bytes(4)+chunk(b'VP8 ',vp8)))
    save('animated', animation, bytes([3]))
    bad = {
        'duplicate-raster': riff(chunk(b'VP8 ',vp8),chunk(b'VP8L',vp8l)),
        'wrong-canvas': riff(extended(w=30),chunk(b'VP8 ',vp8)),
        'huge-canvas': riff(extended(w=1<<24,h=1<<24),chunk(b'VP8 ',vp8)),
        'short-vp8x': riff(chunk(b'VP8X',bytes(9)),chunk(b'VP8 ',vp8)),
        'duplicate-vp8x': riff(extended(),extended(),chunk(b'VP8 ',vp8)),
        'short-chunk-header': riff(chunk(b'VP8 ',vp8),b'bad'),
        'huge-tail-chunk': riff(chunk(b'VP8 ',vp8),b'what'+b'\xff'*4),
        'missing-pad': riff(chunk(b'VP8 ',vp8),b'what'+struct.pack('<I',1)+b'x'),
        'late-icc': riff(extended(32),chunk(b'VP8 ',vp8),chunk(b'ICCP',b'profile')),
        'late-alpha': riff(extended(),chunk(b'VP8 ',vp8),chunk(b'ALPH',b'\0xx')),
        'double-alpha': riff(extended(16),chunk(b'ALPH',b'\0xx'),chunk(b'ALPH',b'\0xx'),chunk(b'VP8 ',vp8)),
        'alpha-vp8l': riff(extended(16),chunk(b'ALPH',b'\0xx'),chunk(b'VP8L',vp8l)),
        'missing-alpha': riff(extended(16),chunk(b'VP8 ',vp8)),
        'missing-anim': riff(extended(2),chunk(b'VP8 ',vp8)),
        'missing-frames': riff(extended(2),chunk(b'ANIM',bytes(6))),
        'frame-without-flag': riff(extended(),chunk(b'ANMF',bytes(16)),chunk(b'VP8 ',vp8)),
        'broken-animation-tail': animation[:-1],
        'empty-vp8': riff(chunk(b'VP8 ',b'')),
        'invalid-vp8l': riff(chunk(b'VP8L',bytes(20))),
        'truncated-vp8-body': riff(chunk(b'VP8 ',vp8[:len(vp8)//2])),
        'truncated-vp8l-body': riff(chunk(b'VP8L',vp8l[:len(vp8l)//2])),
    }
    for name,data in bad.items(): save(name,data,bytes([2]))
    for name,data in [('raw-codec',vp8),('avi',b'RIFF'+bytes(4)+b'AVI '+bytes(20))]: save(name,data,bytes([1]))
    return paths

def build_wrapper(work, sanitize=True):
    objs = build(work, sanitize=sanitize)
    flags = ['-std=c11','-O2','-g','-Wall','-Wextra','-Werror','-fno-builtin','-fPIC']
    if sanitize: flags += ['-fsanitize=address,undefined','-fno-sanitize-recover=all']
    includes = ['-I'+str(p) for p in (BASE/'include',BASE/'upstream',ROOT/'userland/libos64/include',ROOT/'abi/include',work)]
    obj = work/'wrapper.o'
    subprocess.run(['cc',*flags,*includes,'-c',str(BASE/'port/decode.c'),'-o',str(obj)],check=True)
    return [*objs,obj], flags, includes

def guest_fixtures(work):
    destination = ROOT/'userland/tests/webptest/vectors.h'
    destination.parent.mkdir(parents=True,exist_ok=True)
    lines = ['/* Generated by tools/test_webp_host.py --write-guest from synthetic images. */']
    for name in ('lossy','alpha','lossless','animated'):
        for suffix, label in (('.webp',name+'_webp'),('.ref',name+'_pixels')):
            if name == 'animated' and suffix == '.ref': continue
            data = (work/(name+suffix)).read_bytes()
            lines.append('static const uint8_t '+label+'[] = {')
            lines += ['    '+','.join(f'0x{x:02x}' for x in data[i:i+16])+',' for i in range(0,len(data),16)]
            lines.append('};')
    destination.write_text('\n'.join(lines)+'\n')
    pages = ROOT/'tools/pages'
    image = Image.new('RGBA',(256,128))
    image.putdata([(x, y*2, 255-x, min(255,x*2)) for y in range(128) for x in range(256)])
    image.save(pages/'webp-alpha.png')
    for name, im, lossless in [('webp-lossy',image.convert('RGB'),False),
                              ('webp-lossless',image.convert('RGB'),True),('webp-alpha',image,False)]:
        im.save(pages/(name+'.webp'),'WEBP',lossless=lossless,quality=85,exact=True)
    (pages/'webp-mislabeled.jpg').write_bytes((pages/'webp-lossless.webp').read_bytes())
    (pages/'webp-animated.webp').write_bytes((work/'animated.webp').read_bytes())

def run(work, characterization=None):
    work.mkdir(parents=True,exist_ok=True)
    paths = fixtures(work)
    objs, flags, includes = build_wrapper(work/'objects')
    subprocess.run(['cc',*flags,*includes,'-pthread','-no-pie',str(ROOT/'tools/test_webp_host.c'),
                    *map(str,objs),'-Wl,--gc-sections','-o',str(work/'test')],check=True)
    env = os.environ.copy(); env.setdefault('ASAN_OPTIONS','detect_leaks=1')
    for path in paths:
        args = [str(work/'test'),str(path),str(path.with_suffix('.ref'))]
        if path.stem == 'alpha': args.append('concurrent-first-call')
        subprocess.run(args,env=env,check=True)
        print('PASS',path.stem,flush=True)
    print('PASS WebP reference pixels, framing, prefixes, fault injection, exact budget boundary, six callers and cleanup',flush=True)
    if characterization:
        report = json.loads((characterization/'results.json').read_text())
        for row in report['results']:
            path = characterization/row['fixture']
            assert hashlib.sha256(path.read_bytes()).hexdigest() == row['sha256'], path
            sample = row['sse2']
            expected = work/(path.stem+'.hash')
            expected.write_bytes(struct.pack('<IIQ',sample['width'],sample['height'],int(sample['pixel_hash'],16)))
            subprocess.run([str(work/'test'),str(path),str(expected),'large'],env=env,check=True)

if __name__ == '__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--write-guest',action='store_true',help='regenerate committed guest vectors and visual fixtures')
    parser.add_argument('--characterization',type=Path,help='also verify wrapper pixels/budgets against a bench_webp_host output directory')
    args = parser.parse_args()
    if args.write_guest:
        args.output.mkdir(parents=True,exist_ok=True)
        fixtures(args.output.resolve()); guest_fixtures(args.output.resolve())
    else:
        run(args.output.resolve(),args.characterization)
