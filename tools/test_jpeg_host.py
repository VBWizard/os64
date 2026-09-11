#!/usr/bin/env python3
"""Generate JPEG/orientation fixtures and exercise the production decoder under sanitizers."""
import argparse, io, json, os, struct, subprocess, tempfile
from pathlib import Path
from PIL import Image, ImageOps
ROOT=Path(__file__).resolve().parents[1]
def fixtures(work):
    base=Image.new('RGB',(17,9)); base.putdata([((x*17+y*7)%256,(y*31+x*3)%256,(x*11+y*19)%256) for y in range(9) for x in range(17)])
    paths=[]
    def save(name,data,expected=None):
        p=work/(name+'.jpg');p.write_bytes(data)
        if expected is None:
            img=ImageOps.exif_transpose(Image.open(io.BytesIO(data))).convert('RGBA')
            expected=struct.pack('<II',*img.size)+img.tobytes('raw','BGRA')
        (work/(name+'.ref')).write_bytes(expected);paths.append(p)
    for progressive in (False,True):
        for sub in (0,1,2):
            out=io.BytesIO();base.save(out,'JPEG',quality=90,progressive=progressive,subsampling=sub)
            save(f'rgb-{progressive}-{sub}',out.getvalue())
    for mode in ('L','CMYK'):
        out=io.BytesIO();base.convert(mode).save(out,'JPEG',quality=90);save(mode,out.getvalue())
    # Reinterpret four encoded component planes as Y/Cb/Cr/K and cross-check
    # the Adobe transform-2 path with Pillow's independent hosted decoder.
    ycck=bytearray((work/'CMYK.jpg').read_bytes());at=ycck.index(b'Adobe');ycck[at+11]=2
    save('YCCK',ycck)
    out=io.BytesIO();base.save(out,'JPEG',quality=95,subsampling=0);raw=out.getvalue()
    def exif(turn,endian):
        tiff=(b'II' if endian=='<' else b'MM')+struct.pack(endian+'HIH',42,8,1)
        tiff+=struct.pack(endian+'HHI',0x112,3,1)+struct.pack(endian+'H',turn)+b'\0\0'+bytes(4)
        data=b'Exif\0\0'+tiff
        return b'\xff\xe1'+struct.pack('>H',len(data)+2)+data
    for endian in ('<','>'):
        for turn in range(1,9):save(f'orient-{endian==">"}-{turn}',raw[:2]+exif(turn,endian)+raw[2:])
    save('bad-orientation',raw[:2]+exif(9,'<')+raw[2:],bytes([2]))
    save('duplicate-orientation',raw[:2]+exif(1,'<')+exif(1,'>')+raw[2:],bytes([2]))
    bad=bytearray(raw[:2]+exif(1,'<')+raw[2:]);bad[16:20]=b'\xff'*4;save('bad-exif-offset',bad,bytes([2]))
    # A big SOF is refused before output or coefficient allocation.
    bad=bytearray(raw);at=bad.index(b'\xff\xc0');bad[at+5:at+7]=b'\xff\xff';save('huge',bad,bytes([4]))
    bad=bytearray(raw);at=bad.index(b'\xff\xc0');bad[at+4]=12;save('precision12',bad,bytes([3]))
    bad=bytearray(raw);at=bad.index(b'\xff\xc0');bad[at+1]=0xc3;save('lossless',bad,bytes([3]))
    bad=bytearray(raw);at=bad.index(b'\xff\xc0');bad[at+1]=0xc9;save('arithmetic',bad,bytes([3]))
    # A small fixture also becomes the ring-3 oracle, without a runtime host decoder.
    d=raw[:2]+exif(6,'<')+raw[2:];img=ImageOps.exif_transpose(Image.open(io.BytesIO(d))).convert('RGBA')
    return paths,d,struct.pack('<II',*img.size)+img.tobytes('raw','BGRA')
def run(work):
    paths,fixture,expected=fixtures(work)
    core=(ROOT/'userland/libjpeg/sources.mk').read_text().replace('JPEG_CORE_SRCS :=','').replace('\\','').split()
    common=['cc','-pthread','-std=c11','-O2','-g','-fno-builtin','-fno-pie','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-sanitize-recover=all']
    includes=['-I'+str(ROOT/p) for p in ('userland/libjpeg/include','userland/libos64/include','abi/include')]
    private=['-I'+str(ROOT/p) for p in ('userland/libjpeg/port/compat','userland/libjpeg/port','userland/libjpeg/upstream')]
    subprocess.run(['python3',str(ROOT/'tools/jpeg_license.py'),str(work/'jpeg_license.h')],check=True)
    objects=[]
    for source in core+['libjpeg/port/decode.c']:
        obj=work/(Path(source).stem+'.o');objects.append(obj)
        subprocess.run(common+includes+private+['-I'+str(work),'-include',str(ROOT/'userland/libjpeg/port/config.h'),'-Wno-unused-parameter','-Wno-sign-compare','-c',str(ROOT/'userland'/source),'-o',str(obj)],check=True)
    subprocess.run(common+includes+['-no-pie',str(ROOT/'tools/test_jpeg_host.c'),*map(str,objects),'-o',str(work/'test')],check=True)
    env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0'}
    for p in paths:
        subprocess.run([str(work/'test'),str(p),str(p.with_suffix('.ref'))],env=env,check=True)
        print('PASS',p.stem,flush=True)
    print('PASS JPEG reference pixels, every truncated prefix, allocation failures and cleanup',flush=True)
    adjacent = ['userland/libimage/image.c', 'userland/libpng/png.c', 'userland/libgzip/inflate.c',
                'userland/libos64/crc32.c', 'userland/libos64/draw.c', 'tools/test_image_host.c']
    extra = ['-I'+str(ROOT/p) for p in ('userland/libimage/include','userland/libpng/include','userland/libgzip/include')]
    subprocess.run(common+includes+extra+['-no-pie','-masm=intel', *[str(ROOT/p) for p in adjacent], *map(str,objects),'-o',str(work/'image-test')],check=True)
    subprocess.run([str(work/'image-test')],env=env,check=True)

    return fixture,expected
if __name__=='__main__':
    parser=argparse.ArgumentParser();parser.add_argument('--output',type=Path);a=parser.parse_args()
    if a.output:a.output.mkdir();run(a.output.resolve())
    else:
        with tempfile.TemporaryDirectory(prefix='jpeg-host-') as p:run(Path(p))
