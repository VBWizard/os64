#!/usr/bin/env python3
"""One deliberate break at a time to the register image's rules
(kernel/src/fpu_image.c: the reset image, the sanitizer every restore goes
through, the MXCSR mask, the mode decision), against
tools/test_fpu_image_host.sh: a mutant is CAUGHT when the suite fails,
MISSED when it passes, which means a test is missing and gets written. A
build failure is neither.

The shape is test_media_keys_mutants.py's: each mutant is applied to the
tree's copy of the file, the suite is run, and the file is put back from the
bytes read before the change, checked byte for byte, whatever happens. Run it
from a clean tree, and never beside another harness that edits the same tree;
the per-mutant logs are in --logs. Under tools/mutants_parallel.py it needs
`--with kernel`: the runner's copies do not carry the kernel's sources
unless asked.
"""
import argparse
import pathlib
import re
import subprocess
import sys

parser = argparse.ArgumentParser()
parser.add_argument('--logs', type=pathlib.Path, default=pathlib.Path('/tmp/fpu-image-mutants'))
parser.add_argument('--only', help='run the mutants whose name contains this')
args = parser.parse_args()
root = pathlib.Path(__file__).resolve().parent.parent
args.logs.mkdir(parents=True, exist_ok=True)

SOURCE = 'kernel/src/fpu_image.c'
CASES = 'bash tools/test_fpu_image_host.sh'

mutants = [
    # The reset image (R5).
    ('reset-keeps-old-bytes', '\tfor (uint32_t i = 0; i < FPU_IMAGE_SIZE; i++)\n\t\timage[i] = 0;\n\t// All',
     '\tfor (uint32_t i = 0; i < FPU_IMAGE_LEGACY_SIZE; i++)\n\t\timage[i] = 0;\n\t// All'),
    ('reset-fcw-low-byte', '\timage[FPU_IMAGE_FCW_OFFSET]     = (uint8_t)FPU_IMAGE_RESET_FCW;',
     '\timage[FPU_IMAGE_FCW_OFFSET]     = 0;'),
    ('reset-fcw-high-byte', '\timage[FPU_IMAGE_FCW_OFFSET + 1] = (uint8_t)(FPU_IMAGE_RESET_FCW >> 8);',
     '\timage[FPU_IMAGE_FCW_OFFSET + 1] = 0;'),
    ('reset-no-mxcsr', '\timage_set_u32(image, FPU_IMAGE_MXCSR_OFFSET, FPU_IMAGE_RESET_MXCSR);\n\timage[FPU_IMAGE_HEADER',
     '\timage[FPU_IMAGE_HEADER'),
    ('reset-marks-avx-present', '\timage[FPU_IMAGE_HEADER_OFFSET] = (uint8_t)(FPU_XCR0_X87 | FPU_XCR0_SSE);',
     '\timage[FPU_IMAGE_HEADER_OFFSET] = (uint8_t)FPU_XCR0;'),
    ('reset-marks-nothing-present', '\timage[FPU_IMAGE_HEADER_OFFSET] = (uint8_t)(FPU_XCR0_X87 | FPU_XCR0_SSE);',
     '\timage[FPU_IMAGE_HEADER_OFFSET] = 0;'),
    # The sanitizer (R4).
    ('xstate-bv-unmasked', '\timage[FPU_IMAGE_HEADER_OFFSET] &= (uint8_t)FPU_XCR0;\n', ''),
    ('xstate-bv-loses-avx', '\timage[FPU_IMAGE_HEADER_OFFSET] &= (uint8_t)FPU_XCR0;',
     '\timage[FPU_IMAGE_HEADER_OFFSET] &= (uint8_t)(FPU_XCR0_X87 | FPU_XCR0_SSE);'),
    ('xstate-bv-high-bytes-kept', '\tfor (uint32_t i = FPU_IMAGE_HEADER_OFFSET + 1; i < FPU_IMAGE_AVX_OFFSET; i++)',
     '\tfor (uint32_t i = FPU_IMAGE_HEADER_OFFSET + 8; i < FPU_IMAGE_AVX_OFFSET; i++)'),
    ('xcomp-bv-kept', '\tfor (uint32_t i = FPU_IMAGE_HEADER_OFFSET + 1; i < FPU_IMAGE_AVX_OFFSET; i++)',
     '\tfor (uint32_t i = FPU_IMAGE_HEADER_OFFSET + 16; i < FPU_IMAGE_AVX_OFFSET; i++)'),
    ('last-reserved-byte-kept', '\tfor (uint32_t i = FPU_IMAGE_HEADER_OFFSET + 1; i < FPU_IMAGE_AVX_OFFSET; i++)',
     '\tfor (uint32_t i = FPU_IMAGE_HEADER_OFFSET + 1; i < FPU_IMAGE_AVX_OFFSET - 1; i++)'),
    ('zeroing-eats-avx', '\tfor (uint32_t i = FPU_IMAGE_HEADER_OFFSET + 1; i < FPU_IMAGE_AVX_OFFSET; i++)',
     '\tfor (uint32_t i = FPU_IMAGE_HEADER_OFFSET + 1; i < FPU_IMAGE_AVX_OFFSET + 16; i++)'),
    ('zeroing-eats-legacy', '\tfor (uint32_t i = FPU_IMAGE_HEADER_OFFSET + 1; i < FPU_IMAGE_AVX_OFFSET; i++)',
     '\tfor (uint32_t i = FPU_IMAGE_HEADER_OFFSET - 16; i < FPU_IMAGE_AVX_OFFSET; i++)'),
    ('no-fallback-mask', '\tif (mxcsr_mask == 0)\n\t\tmxcsr_mask = FPU_MXCSR_FALLBACK_MASK;\n\timage_set', '\timage_set'),
    ('mxcsr-unmasked', '\t              image_u32(image, FPU_IMAGE_MXCSR_OFFSET) & mxcsr_mask);',
     '\t              image_u32(image, FPU_IMAGE_MXCSR_OFFSET));'),
    ('mxcsr-masked-to-reset', '\t              image_u32(image, FPU_IMAGE_MXCSR_OFFSET) & mxcsr_mask);',
     '\t              FPU_IMAGE_RESET_MXCSR & mxcsr_mask);'),
    ('mxcsr-at-mask-offset', '\timage_set_u32(image, FPU_IMAGE_MXCSR_OFFSET,\n\t              image_u32(image, FPU_IMAGE_MXCSR_OFFSET) & mxcsr_mask);',
     '\timage_set_u32(image, FPU_IMAGE_MXCSR_MASK_OFFSET,\n\t              image_u32(image, FPU_IMAGE_MXCSR_MASK_OFFSET) & mxcsr_mask);'),
    ('u32-drops-top-byte', '((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);', '((uint32_t)p[2] << 16);'),
    ('set-u32-drops-top-byte', '\tp[3] = (uint8_t)(value >> 24);\n', ''),
    # The MXCSR mask.
    ('mask-no-fallback', '\treturn mask ? mask : FPU_MXCSR_FALLBACK_MASK;', '\treturn mask;'),
    ('mask-always-fallback', '\treturn mask ? mask : FPU_MXCSR_FALLBACK_MASK;',
     '\treturn mask ? FPU_MXCSR_FALLBACK_MASK : FPU_MXCSR_FALLBACK_MASK;'),
    ('mask-reads-mxcsr', '\tuint32_t mask = image_u32(image, FPU_IMAGE_MXCSR_MASK_OFFSET);',
     '\tuint32_t mask = image_u32(image, FPU_IMAGE_MXCSR_OFFSET);'),
    # The mode (R2/R3).
    ('no-xsave-accepted', '\tif (!a->xsave)\n\t\treturn FPU_WHY_NO_XSAVE;\n', ''),
    ('no-avx-accepted', '\tif (!a->avx)\n\t\treturn FPU_WHY_NO_AVX;\n', ''),
    ('noavx-ignored', '\tif (a->noavx)\n\t\treturn FPU_WHY_NOAVX;\n', ''),
    ('noavx-blamed-first', '\tif (!a->xsave)\n\t\treturn FPU_WHY_NO_XSAVE;\n\tif (!a->avx)\n\t\treturn FPU_WHY_NO_AVX;\n\tif (a->noavx)\n\t\treturn FPU_WHY_NOAVX;',
     '\tif (a->noavx)\n\t\treturn FPU_WHY_NOAVX;\n\tif (!a->xsave)\n\t\treturn FPU_WHY_NO_XSAVE;\n\tif (!a->avx)\n\t\treturn FPU_WHY_NO_AVX;'),
    ('components-any-bit', '\tif ((a->components & FPU_XCR0) != FPU_XCR0)', '\tif ((a->components & FPU_XCR0) == 0)'),
    ('components-exact', '\tif ((a->components & FPU_XCR0) != FPU_XCR0)', '\tif (a->components != FPU_XCR0)'),
    ('size-not-checked', '\tif (a->size != FPU_IMAGE_SIZE)\n\t\treturn FPU_WHY_SIZE;\n', ''),
    ('size-at-least', '\tif (a->size != FPU_IMAGE_SIZE)', '\tif (a->size < FPU_IMAGE_SIZE)'),
    ('avx-offset-not-checked', 'a->avx_offset != FPU_IMAGE_AVX_OFFSET || ', ''),
    ('avx-size-not-checked', ' || a->avx_size != FPU_IMAGE_AVX_SIZE', ''),
]

caught, missed, unbuilt = [], [], []
for name, old, new in mutants:
    if args.only and args.only not in name:
        continue
    path = root / SOURCE
    original = path.read_bytes()
    text = original.decode()
    if text.count(old) != 1:
        print(f'{name}: the text to break is not found once in {SOURCE}', file=sys.stderr)
        unbuilt.append(name)
        continue
    log = args.logs / f'{name}.log'
    try:
        path.write_text(text.replace(old, new))
        with log.open('w') as out:
            try:
                result = subprocess.run(['bash', '-c', CASES], cwd=root, stdout=out, stderr=out, timeout=120)
                code = result.returncode
            except subprocess.TimeoutExpired:
                code = 'timeout'
    finally:
        path.write_bytes(original)
        if path.read_bytes() != original:
            print(f'{name}: {SOURCE} WAS NOT RESTORED', file=sys.stderr)
            sys.exit(2)
    body = log.read_text(errors='replace')
    # A compiler's error, not a sanitizer's "runtime error:", which is a catch.
    built = re.search(r'\.(c|h):\d+:\d+: error:', body) is None and 'undefined reference' not in body
    if not built:
        unbuilt.append(name)
        verdict = 'did not build'
    elif code != 0:
        caught.append(name)
        verdict = 'caught'
    else:
        missed.append(name)
        verdict = 'MISSED'
    print(f'{name}: {verdict}', flush=True)

print(f'\nfpu_image mutants: {len(caught)} caught, {len(missed)} missed, {len(unbuilt)} did not build')
if missed:
    print('missed: ' + ', '.join(missed))
if unbuilt:
    print('did not build: ' + ', '.join(unbuilt))
sys.exit(1 if missed or unbuilt else 0)
