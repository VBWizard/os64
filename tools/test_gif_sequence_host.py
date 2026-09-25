#!/usr/bin/env python3
"""Exact composed GIF frames and bounded, allocation-free playback under sanitizers."""
import argparse
import importlib.util
import io
import os
from pathlib import Path
import struct
import subprocess
import tempfile
from PIL import Image

ROOT=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('gif_fixture',ROOT/'tools/test_gif_host.py')
base=importlib.util.module_from_spec(spec);spec.loader.exec_module(base)


def frame(w,h,values,left=0,top=0,disposal=1,delay=50,transparent=0,local=False,interlace=False):
    encoded=base.gif(w,h,values=values,local=local,interlace=interlace,
                     offset=(left,top),screen=(left+w,top+h))
    raster=encoded[(13 if local else 25):-1]
    gce=b'!\xf9\x04'+bytes([(disposal<<2)|(transparent is not None)])
    gce+=struct.pack('<HBB',delay//10,transparent or 0,0)
    return gce+raster


def animation(w,h,frames,loop=None):
    data=base.gif(w,h)[:25]
    if loop is not None:
        data+=b'!\xff\x0bNETSCAPE2.0\x03\x01'+struct.pack('<H',loop)+b'\0'
    return data+b''.join(frames)+b';'


def reference(data,path):
    im=Image.open(io.BytesIO(data))
    count=getattr(im,'n_frames',1)
    plays=im.info['loop']+1 if im.info.get('loop') else (0 if 'loop' in im.info else 1)
    with path.open('wb') as f:
        f.write(struct.pack('<IIIII',0,*im.size,count,plays))
        for i in range(count):
            im.seek(i)
            rgba=im.convert('RGBA')
            # Transparent pixels in a composed canvas are canonical zero.
            rgba.paste((0,0,0,0),mask=rgba.getchannel('A').point(lambda a:255 if a==0 else 0))
            f.write(struct.pack('<I',im.info.get('duration',0)))
            f.write(rgba.tobytes('raw','BGRA'))


def guest_vectors(work, destination):
    lines=['// Generated from tools/test_gif_sequence_host.py; composed frames cross-checked with Pillow.']
    for i,name in enumerate(['disposal-2-loop-1','restore-chain']):
        for ext,label in [('gif','data'),('seq','expected')]:
            data=(work/(name+'.'+ext)).read_bytes()
            lines.append(f'static const uint8_t sequence_{label}_{i}[] = {{')
            lines.extend('    '+','.join(f'0x{x:02x}' for x in data[n:n+16])+','
                         for n in range(0,len(data),16))
            lines.append('};')
    destination.write_text('\n'.join(lines)+'\n')


def run(work, real):
    sources=['userland/libimage/image.c','userland/libimage/gif.c','tools/test_gif_sequence_host.c']
    includes=['userland/libimage/include','userland/libos64/include','userland/libpng/include',
              'userland/libjpeg/include','abi/include']
    subprocess.run(['cc','-std=c11','-O1','-g','-Wall','-Wextra','-Werror',
                    '-fsanitize=address,undefined','-fno-sanitize-recover=all','-fno-pie','-no-pie',
                    *['-I'+str(ROOT/p) for p in includes],*[str(ROOT/p) for p in sources],
                    '-o',str(work/'test')],check=True)
    env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0'}
    def check(name,data,status=0,expected=None,bad=False):
        p=work/(name+'.gif');p.write_bytes(data)
        ref=p.with_suffix('.seq')
        if status:
            ref.write_bytes(struct.pack('<I',status))
        else:
            reference(expected or data,ref)
        subprocess.run([str(work/'test'),str(p),str(ref)]+(['bad'] if bad else []),env=env,check=True)
        if status: print('PASS refusal',name,flush=True)
    for dispose in (0,1,2,3):
        frames=[frame(4,3,[1]*12,disposal=dispose),
                frame(2,2,[2,0,0,2],1,1,disposal=dispose,delay=1000,local=True,interlace=True),
                frame(1,1,[3],2,0,disposal=dispose,delay=10)]
        for loop in (None,0,1,2):
            check(f'disposal-{dispose}-loop-{loop}',animation(4,3,frames,loop))
    # Successive restore-previous frames must not restore each other's output.
    restore=[frame(5,4,[1]*20),frame(3,2,[2]*6,1,1,disposal=3),
             frame(1,3,[3]*3,2,0,disposal=3),frame(1,1,[2],0,3)]
    check('restore-chain',animation(5,4,restore,0))
    check('transparent-loop-reset',animation(5,4,[frame(1,1,[1]),frame(1,1,[2],4,3)],0))
    check('singleton',animation(1,1,[frame(1,1,[2],disposal=3,delay=0)],0))
    local=[frame(3,3,[1]*9),frame(2,2,[2]*4,1,1,local=True)]
    changed=bytearray(local[1]); changed[18:30]=bytes([20,40,60,80,100,120,140,160,180,200,220,240])
    check('local-palette-change',animation(3,3,[local[0],bytes(changed)],1))
    frames=[frame(2,2,[1]*4),frame(2,2,[2]*4,disposal=3)]
    valid=animation(2,2,frames,0)
    bad=bytearray(frames[1]);bad[-2]=0 # Remove EOI from this later frame.
    check('later-corruption',animation(2,2,[frames[0],bad],0),expected=valid,bad=True)
    check('bad-gce',base.gif(1,1,extensions=b'!\xf9\x04\x02\0\0\0\0'),7)
    check('plain-text',base.gif(1,1,extensions=b'!\x01\x0c'+bytes(12)+b'\0'),7)
    check('loop-duplicate',valid[:25]+valid[25:44]+valid[25:],6)
    check('loop-bad-size',valid[:39]+b'\x02'+valid[40:],6)
    check('too-many-frames',animation(1,1,[frame(1,1,[1])]*4097),8)
    huge=bytearray(animation(4096,4096,[frame(4096,4096,[],disposal=3)]))
    check('working-memory-cap',huge,8)
    check('encoded-cap',valid+bytes(20*1024*1024),2)
    check('ppm',b'P6\n2 1\n255\n'+bytes([255,0,0,0,255,0]))
    for path in real:
        check(path.stem,path.read_bytes())
    print('PASS sequence frames, delays, loops, disposal, rollback, allocation failures, ownership and budgets',flush=True)


if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--output',type=Path);p.add_argument('--real',type=Path,action='append',default=[]);p.add_argument('--guest-vectors',type=Path)
    args=p.parse_args()
    if args.output:
        args.output.mkdir(parents=True,exist_ok=True);run(args.output.resolve(),args.real)
        if args.guest_vectors: guest_vectors(args.output.resolve(),args.guest_vectors)
    else:
        with tempfile.TemporaryDirectory(prefix='gif-sequence-') as d:
            run(Path(d),args.real)
            if args.guest_vectors: guest_vectors(Path(d),args.guest_vectors)
