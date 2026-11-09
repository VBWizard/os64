#!/usr/bin/env python3
"""Characterize decoder paths, memory and scalar/SSE2 pixels; requires Pillow."""
import argparse
import hashlib
import io
import json
import os
from pathlib import Path
import platform
import random
import struct
import subprocess
from PIL import Image, features
from webp_build import BASE, ROOT, build

def chunk(tag, payload):
    return tag + struct.pack('<I', len(payload)) + payload + bytes(len(payload) & 1)

def encode(image, lossless):
    out = io.BytesIO()
    image.save(out, 'WEBP', lossless=lossless, quality=80, method=3, exact=True)
    return out.getvalue()

def fixtures(work):
    ramp = Image.linear_gradient('L').resize((4096, 4096))
    horizontal = ramp.transpose(Image.Transpose.TRANSPOSE)
    rgb = Image.merge('RGB', (horizontal, ramp, horizontal))
    palette = Image.new('RGB', (4096, 4096), (24, 90, 160))
    binary = ramp.point(lambda v: 255 if v > 127 else 0)
    cases = [('lossy-opaque', rgb, False), ('lossless-gradient', rgb, True),
             ('lossless-palette', palette, True),
             ('lossy-alpha-binary', Image.merge('RGBA', (*rgb.split(), binary)), False),
             ('lossy-alpha-gradient', Image.merge('RGBA', (*rgb.split(), ramp)), False)]
    rng = random.Random(20261010)
    noise = Image.frombytes('RGB', (1024, 1024), rng.randbytes(1024 * 1024 * 3))
    cases += [('lossless-noise', noise, True),
              ('lossless-noisy-stripes', Image.frombytes('RGB', (4096, 256),
               rng.randbytes(4096 * 256 * 3)).resize((4096, 4096), Image.Resampling.NEAREST), True),
              ('lossy-wide', Image.new('RGB', (16383, 16), (11, 22, 33)), False)]
    for name, image, lossless in cases:
        path = work / (name + '.webp')
        if not path.exists():
            path.write_bytes(encode(image, lossless))
        print('fixture', name, flush=True)
        yield path
    # The encoder API caps both modes at 16383, but VP8L's reader accepts
    # 16384. Reframe a constant palette raster with the same packed-pixel
    # count; its index stream has no spatial predictor transform.
    wide = bytearray(encode(Image.new('RGB', (8192, 32), (11, 22, 33)), True))
    assert wide[12:16] == b'VP8L' and wide[20] == 0x2f
    bits = struct.unpack_from('<I', wide, 21)[0]
    struct.pack_into('<I', wide, 21, (bits & 0xf0000000) | 16383 | (15 << 14))
    path = work / 'lossless-wide.webp'
    path.write_bytes(wide)
    yield path
    # Force raw alpha independently of the encoder's compression choice.
    lossy = (work / 'lossy-opaque.webp').read_bytes()
    at = 12
    while lossy[at:at+4] != b'VP8 ':
        n = struct.unpack_from('<I', lossy, at + 4)[0]
        at += 8 + n + (n & 1)
    n = struct.unpack_from('<I', lossy, at + 4)[0]
    header = b'\x10\0\0\0' + (4095).to_bytes(3, 'little') * 2
    payload = b'WEBP' + chunk(b'VP8X', header) + chunk(b'ALPH', b'\0' + ramp.tobytes())
    payload += chunk(b'VP8 ', lossy[at+8:at+8+n])
    path = work / 'lossy-alpha-raw.webp'
    path.write_bytes(b'RIFF' + struct.pack('<I', len(payload)) + payload)
    yield path

def run(work, sanitize=False):
    work.mkdir(parents=True, exist_ok=True)
    binaries = {}
    for mode in ('sse2', 'scalar'):
        directory = work / mode
        objs = build(directory, scalar=mode == 'scalar', sanitize=sanitize, probe=True)
        binary = directory / 'probe'
        flags = ['-fsanitize=address,undefined', '-fno-sanitize-recover=all'] if sanitize else []
        subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                        *flags, '-no-pie', '-I'+str(BASE/'upstream'),
                        str(ROOT/'tools/probe_webp_host.c'), *map(str, objs),
                        '-Wl,--gc-sections', '-o', str(binary)], check=True)
        binaries[mode] = binary
    results = []
    env = dict(os.environ, ASAN_OPTIONS='detect_leaks=1')
    for path in fixtures(work):
        record = {'fixture': path.name,
                  'sha256': hashlib.sha256(path.read_bytes()).hexdigest()}
        for mode, binary in binaries.items():
            record[mode] = json.loads(subprocess.check_output([str(binary), str(path)], env=env))
        assert record['sse2']['pixel_hash'] == record['scalar']['pixel_hash'], path
        assert record['sse2']['input'] <= 20 * 1024 * 1024, path
        for mode in binaries:
            sample = record[mode]
            assert sample['width'] * sample['height'] <= 16 * 1024 * 1024, path
            assert sample['total_peak'] <= 256 * 1024 * 1024, path
            expected_alpha = {'lossy-alpha-binary.webp': 1,
                              'lossy-alpha-gradient.webp': 2}.get(path.name, 0)
            assert sample['alpha_paths'] == expected_alpha, path
            if path.name == 'lossless-gradient.webp':
                assert sample['transforms'] & 3 == 3, path
            if path.name == 'lossless-noise.webp':
                assert sample['transforms'] & 4, path
            if path.name == 'lossless-wide.webp':
                assert (sample['width'], sample['height']) == (16384, 16), path
                # FNV-1a of 16384*16 BGRA pixels (33,22,11,255), independently
                # calculated from the solid source rather than decoder output.
                expected = 14695981039346656037
                for byte in bytes((33, 22, 11, 255)) * (16384 * 16):
                    expected = ((expected ^ byte) * 1099511628211) & ((1 << 64) - 1)
                assert sample['pixel_hash'] == f'{expected:016x}', path
        results.append(record)
        print(json.dumps(record), flush=True)
    report = {'release': '1.6.0', 'platform': platform.platform(),
              'compiler': subprocess.check_output(['cc', '--version'], text=True).splitlines()[0],
              'cpu': next((line.split(':', 1)[1].strip() for line in
                           Path('/proc/cpuinfo').read_text().splitlines()
                           if line.startswith('model name')), 'unknown'),
              'fixture_encoder': 'Pillow libwebp '+features.version('webp'),
              'sanitized': sanitize, 'results': results}
    (work/'results.json').write_text(json.dumps(report, indent=2)+'\n')
    return report

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--sanitize', action='store_true')
    args = parser.parse_args()
    run(args.output.resolve(), args.sanitize)
