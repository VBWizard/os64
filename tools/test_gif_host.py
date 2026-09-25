#!/usr/bin/env python3
"""GIF reference pixels, handcrafted boundaries, allocation failures and truncation."""
import argparse
import io
import os
from pathlib import Path
import random
import struct
import subprocess
import tempfile
from PIL import Image

ROOT = Path(__file__).resolve().parents[1]


def blocks(data, chunk=255):
    return b''.join(bytes([len(data[i:i+chunk])]) + data[i:i+chunk]
                    for i in range(0, len(data), chunk)) + b'\0'


def packed(codes, minimum):
    """Pack explicit codes; Pillow independently verifies their interpretation."""
    clear = 1 << minimum
    next_code, width, previous = clear + 2, minimum + 1, False
    bits, buffered, out = 0, 0, bytearray()
    for code in codes:
        assert code < 1 << width
        bits |= code << buffered
        buffered += width
        while buffered >= 8:
            out.append(bits & 255)
            bits >>= 8
            buffered -= 8
        if code == clear:
            next_code, width, previous = clear + 2, minimum + 1, False
        elif code != clear + 1:
            if previous and next_code < 4096:
                next_code += 1
                if next_code == 1 << width and width < 12:
                    width += 1
            previous = True
    if buffered:
        out.append(bits)
    return bytes(out)


def gif(w, h, depth=2, values=None, codes=None, local=False, interlace=False,
        transparent=None, offset=(0, 0), screen=None, extensions=b'', chunk=255):
    colors = 1 << depth
    pal = bytes(v for i in range(colors) for v in ((i*53)%256, (i*97)%256, (i*193)%256))
    sw, sh = screen or (w, h)
    header = b'GIF89a' + struct.pack('<HHBBB', sw, sh, 0 if local else 0x80 | (depth-1), 0, 0)
    if not local:
        header += pal
    if transparent is not None:
        header += b'!\xf9\x04\x01\0\0' + bytes([transparent, 0])
    header += extensions
    flags = (0x80 | (depth-1) if local else 0) | (0x40 if interlace else 0)
    desc = b',' + struct.pack('<HHHHB', *offset, w, h, flags)
    if local:
        desc += pal
    minimum = max(depth, 2)
    if codes is None:
        if values is None:
            values = [(x*3 + y*5) % colors for y in range(h) for x in range(w)]
        if interlace:
            rows = [y for start, step in ((0,8), (4,8), (2,4), (1,2)) for y in range(start,h,step)]
            values = [values[y*w+x] for y in rows for x in range(w)]
        codes = [1 << minimum, *values, (1 << minimum)+1]
    return header + desc + bytes([minimum]) + blocks(packed(codes, minimum), chunk) + b';'


def reference(data, rectangle=None):
    img = Image.open(io.BytesIO(data)).convert('RGBA')
    # Our browser-facing contract clears unpainted canvas space. Pillow uses
    # the GIF background index there; compare frame pixels without changing
    # their RGB or alpha and check transparent margins explicitly.
    if rectangle is not None:
        left, top, width, height = rectangle
        canvas = Image.new('RGBA', img.size, (0, 0, 0, 0))
        canvas.paste(img.crop((left, top, left+width, top+height)), (left, top))
        img = canvas
    return struct.pack('<II', *img.size) + img.tobytes('raw', 'BGRA')


def fixtures(work):
    paths = []
    def save(name, data, expected=None, rectangle=None):
        p = work / (name + '.gif')
        p.write_bytes(data)
        p.with_suffix('.ref').write_bytes(reference(data, rectangle) if expected is None else expected)
        paths.append(p)
        return data

    for depth in range(1, 9):
        for interlace in (False, True):
            for local in (False, True):
                for transparent in (None, 0):
                    save(f'd{depth}-i{interlace}-l{local}-t{transparent}',
                         gif(19, 11, depth, interlace=interlace, local=local,
                             transparent=transparent, chunk=1 if local else 255))
    for height in (1, 2, 3, 4, 5, 8, 9, 16, 17):
        save(f'interlace-short-{height}', gif(3, height, interlace=True))
    save('one', gif(1, 1))
    save('wide', gif(4097, 1, depth=8))
    for trans in (None, 1):
        save(f'offset-{trans}', gif(3, 5, offset=(2, 3), screen=(8, 10), transparent=trans),
             rectangle=(2, 3, 3, 5))
    # KwKwK builds strings 0, 00, 000. Literal streams fill the dictionary
    # through every width transition and continue after its 4096-entry limit.
    save('kwkwk', gif(6, 1, codes=[4, 0, 6, 7, 5], chunk=1))
    values = [i % 4 for i in range(12000)]
    save('deferred-clear', gif(120, 100, codes=[4, *values, 5]))
    save('clear-full-table', gif(120, 100, codes=[4, *values[:6000], 4, *values[6000:], 5]))
    save('clear-run', gif(1, 1, codes=[4, 4, 4, 0, 4, 5]))
    save('no-initial-clear', gif(2, 1, codes=[0, 1, 5]))
    # Real dictionary references and resets from an independent encoder.
    rng = random.Random(6403)
    for mode, size in (('P', (83, 47)), ('P', (300, 80)), ('L', (113, 57))):
        image = Image.new(mode, size)
        if mode == 'P':
            image.putpalette(bytes(v for i in range(256) for v in (i, 255-i, i^0x55)))
        image.putdata([rng.randrange(256) for _ in range(size[0]*size[1])])
        f = io.BytesIO()
        image.save(f, 'GIF', interlace=True)
        save(f'pillow-{mode}-{size[0]}', f.getvalue())
    base = gif(2, 2)
    first_ref = reference(base)
    save('87a', b'GIF87a' + base[6:])
    save('comments', gif(2, 2, extensions=b'!\xfe' + blocks(b'hello'*80)))
    save('unknown-extension', gif(2, 2, extensions=b'!\xce' + blocks(b'unknown')))
    save('netscape', gif(2, 2, extensions=b'!\xff\x0bNETSCAPE2.0\x03\x01\0\0\0'))
    text = b'!\x01\x0c' + struct.pack('<HHHHBBBB',0,0,2,2,1,1,0,1) + blocks(b'A')
    save('plain-text-consumes-gce', gif(2, 2, transparent=0, extensions=text), first_ref)
    # Later frames are structurally walked but their LZW is deliberately not
    # decoded. Corruption in their block framing must still reject the file.
    raster = base[13+12:-1]
    save('two-frames', base[:-1] + raster + b';', first_ref)
    corrupt = bytearray(raster); corrupt[-2] ^= 0xff
    save('later-lzw-not-decoded', base[:-1] + corrupt + b';', first_ref)

    malformed = bytes([6])
    early_malformed = bytes([6, 1])
    limit = bytes([8, 1])
    save('missing-eoi', gif(1, 1, codes=[4, 0]), malformed)
    save('short-raster', gif(2, 1, codes=[4, 0, 5]), malformed)
    save('long-raster', gif(1, 1, codes=[4, 0, 1, 5]), malformed)
    save('invalid-first-code', gif(1, 1, codes=[4, 6, 5]), malformed)
    save('invalid-forward-code', gif(2, 1, codes=[4, 0, 7, 5]), malformed)
    save('bad-palette-index', gif(1, 1, depth=1, codes=[4, 2, 5]), malformed)
    save('bad-transparent-index-used', gif(1, 1, depth=1, transparent=2, codes=[4,2,5]), malformed)
    save('trailer-only', base[:25]+b';', early_malformed)
    save('no-palette', base[:10]+b'\0'+base[11:13]+base[25:], bytes([7, 1]))
    save('bad-text-header', gif(2, 2, extensions=b'!\x01\x01\0\0'), early_malformed)
    save('bad-app-header', gif(2, 2, extensions=b'!\xff\x01\0\0'), early_malformed)
    save('bad-gce-size', gif(2, 2, extensions=b'!\xf9\x03\0\0\0\0\0'), early_malformed)
    save('bad-gce-reserved', gif(2, 2, extensions=b'!\xf9\x04\x80\0\0\0\0'), early_malformed)
    save('bad-gce-disposal', gif(2, 2, extensions=b'!\xf9\x04\x1c\0\0\0\0'), early_malformed)
    save('bad-gce-terminator', gif(2, 2, extensions=b'!\xf9\x04\0\0\0\0\x01'), early_malformed)
    gce = b'!\xf9\x04\x01\0\0\0\0'
    save('duplicate-gce', gif(2, 2, transparent=0, extensions=gce), early_malformed)
    save('dangling-gce', base[:-1]+gce+b';', early_malformed)
    save('comment-keeps-gce', gif(2, 2, transparent=0, extensions=b'!\xfe'+blocks(b'hi')))
    save('trailing-data', base+b'junk', early_malformed)
    save('missing-trailer', base[:-1], early_malformed)
    for w, h, status in ((0,1,early_malformed), (1,0,early_malformed),
                         (16385,1,limit), (1,16385,limit), (4097,4097,limit)):
        save(f'screen-{w}-{h}', base[:6]+struct.pack('<HH',w,h)+base[10:], status)
    image_at = 25
    for name, at, value in (('zero-frame',image_at+5,0), ('outside',image_at+1,2),
                            ('reserved',image_at+9,8), ('minimum1',image_at+10,1),
                            ('minimum9',image_at+10,9)):
        b = bytearray(base); b[at] = value
        save(name, b, early_malformed)
    return paths


def guest_vectors(work, destination):
    names = ['d2-iFalse-lFalse-tNone', 'd2-iTrue-lTrue-t0', 'offset-1', 'kwkwk']
    lines = ['// Generated from tools/test_gif_host.py fixtures; expected pixels checked with Pillow.']
    for i, name in enumerate(names):
        for extension, kind in [('gif', 'data'), ('ref', 'pixels')]:
            data = (work / (name + '.' + extension)).read_bytes()
            lines.append(f'static const uint8_t gif_{kind}_{i}[] = {{')
            lines.extend('    ' + ','.join(f'0x{x:02x}' for x in data[n:n+16]) + ','
                         for n in range(0, len(data), 16))
            lines.append('};')
    destination.write_text('\n'.join(lines) + '\n')


def run(work, vectors=None):
    sources = ['userland/libimage/image.c', 'userland/libimage/gif.c', 'tools/test_gif_host.c']
    includes = ['userland/libimage/include', 'userland/libos64/include', 'userland/libpng/include',
                'userland/libjpeg/include', 'abi/include']
    subprocess.run(['cc', '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-fno-pie', '-no-pie',
                    *['-I'+str(ROOT/p) for p in includes], *[str(ROOT/p) for p in sources],
                    '-o', str(work/'test')], check=True)
    paths = fixtures(work)
    env = {**os.environ, 'ASAN_OPTIONS': 'detect_leaks=0'}
    for p in paths:
        subprocess.run([str(work/'test'), str(p), str(p.with_suffix('.ref'))], env=env, check=True)
        print('PASS', p.stem, flush=True)
    if vectors:
        guest_vectors(work, vectors)
    print(f'PASS {len(paths)} GIF fixtures; reference pixels, every valid-file truncated prefix, '
          'mutations, allocation failures and cleanup', flush=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--output', type=Path)
    parser.add_argument('--guest-vectors', type=Path)
    args = parser.parse_args()
    if args.output:
        args.output.mkdir(parents=True, exist_ok=True)
        run(args.output.resolve(), args.guest_vectors)
    else:
        with tempfile.TemporaryDirectory(prefix='gif-host-') as directory:
            run(Path(directory), args.guest_vectors)
