#!/usr/bin/env python3
"""One deliberate break at a time to the copy and fill libos64 and the kernel
share (shared/copy_fill.c: the short moves from both ends, the passes and
the ends read before them, the direction memmove picks, `rep movsb` and
`rep stosb`), against tools/test_str_host.sh, which runs both builds: a
mutant is CAUGHT when it fails, MISSED when it passes, which means a test is
missing and gets written. A build failure is neither.

The shape is test_paint_cache_mutants.py's: each mutant is applied to the
worktree's copy of copy_fill.c, the test is run, and the file is put back
from the bytes read before the change, checked byte for byte, whatever
happens. Run it from a clean tree, and never beside another harness that
edits the same tree; the per-mutant logs are in --logs.

Left out because only speed can tell them apart: the cutoffs where either
build changes method, whether the kernel's string forms align the
destination before their eight-byte steps, which short move a length on a boundary takes (the
moves from each end overlap, so either neighbour copies it whole), memmove
moving a region onto itself, and the order of a pass's reads among its
writes (each pass's writes miss the region it reads in either direction).
"""
import argparse
import pathlib
import re
import subprocess
import sys

parser = argparse.ArgumentParser()
parser.add_argument('--logs', type=pathlib.Path, default=pathlib.Path('/tmp/str-mutants'))
parser.add_argument('--only', help='run the mutants whose name contains this')
args = parser.parse_args()
root = pathlib.Path(__file__).resolve().parent.parent
args.logs.mkdir(parents=True, exist_ok=True)

STR = 'shared/copy_fill.c'
CASES = 'bash tools/test_str_host.sh'

COPY_UP_ENDS = ('    v16 t0 = *(const v16 *)(s + n - 64), t1 = *(const v16 *)(s + n - 48);\n'
                '    v16 t2 = *(const v16 *)(s + n - 32), t3 = *(const v16 *)(s + n - 16);\n')
COPY_UP_LOOP = ('    size_t skip = -(uintptr_t)d & 15;           // to the first aligned store\n'
                '    uint8_t *to = d + skip;\n'
                '    const uint8_t *from = s + skip;\n'
                '    for (size_t left = n - skip; left > 64; left -= 64, to += 64, from += 64) {\n'
                '        v16 a = *(const v16 *)from, b = *(const v16 *)(from + 16);\n'
                '        v16 c = *(const v16 *)(from + 32), e = *(const v16 *)(from + 48);\n'
                '        *(v16_aligned *)to = a;\n'
                '        *(v16_aligned *)(to + 16) = b;\n'
                '        *(v16_aligned *)(to + 32) = c;\n'
                '        *(v16_aligned *)(to + 48) = e;\n'
                '    }\n')
COPY_DOWN_ENDS = ('    v16 h0 = *(const v16 *)s, h1 = *(const v16 *)(s + 16);\n'
                  '    v16 h2 = *(const v16 *)(s + 32), h3 = *(const v16 *)(s + 48);\n')
COPY_DOWN_LOOP = ('    size_t skip = (uintptr_t)(d + n) & 15;      // to the end of the last aligned store\n'
                  '    uint8_t *to = d + n - skip;\n'
                  '    const uint8_t *from = s + n - skip;\n'
                  '    for (size_t left = n - skip; left > 64; left -= 64, to -= 64, from -= 64) {\n'
                  '        v16 a = *(const v16 *)(from - 16), b = *(const v16 *)(from - 32);\n'
                  '        v16 c = *(const v16 *)(from - 48), e = *(const v16 *)(from - 64);\n'
                  '        *(v16_aligned *)(to - 16) = a;\n'
                  '        *(v16_aligned *)(to - 32) = b;\n'
                  '        *(v16_aligned *)(to - 48) = c;\n'
                  '        *(v16_aligned *)(to - 64) = e;\n'
                  '    }\n')

# (name, old, new). Each breaks one rule the comments state.
mutants = [
    # Short copies: a move from each end.
    ('a-short-copy-forgets-its-end', '        *(u64_any *)(d + n - 8) = b;\n', '        (void)b;\n'),
    ('a-short-copy-reads-its-end-early',
     '        uint64_t a = *(const u64_any *)s, b = *(const u64_any *)(s + n - 8);',
     '        uint64_t a = *(const u64_any *)s, b = *(const u64_any *)(s + n - 9);'),
    ('past-48-the-middle-is-missed', '        *(v16 *)(d + 16) = b;\n        *(v16 *)(d + n - 32) = c;',
     '        (void)b;\n        *(v16 *)(d + n - 32) = c;'),
    ('one-byte-is-not-copied', '        *d = *s;', '        (void)s;'),
    ('a-copy-under-64-takes-the-passes',
     '    if (n <= 64)\n        copy_short(dst, src, n);\n    else\n        copy_up(dst, src, n);',
     '    if (n <= 16)\n        copy_short(dst, src, n);\n    else\n        copy_up(dst, src, n);'),
    # Longer copies, forward.
    ('the-rep-copy-falls-short',
     '    __asm__ volatile("rep movsb" : "+D"(d), "+S"(s), "+c"(n) : : "memory");',
     '    n--;\n    __asm__ volatile("rep movsb" : "+D"(d), "+S"(s), "+c"(n) : : "memory");'),
    # The kernel's string copy: bytes to the boundary, words, bytes.
    ('the-kernel-copy-skips-its-head',
     '    __asm__ volatile("rep movsb" : "+D"(d), "+S"(s), "+c"(head) : : "memory");',
     '    d += head;\n    s += head;'),
    ('the-kernel-copy-drops-a-word', '    __asm__ volatile("rep movsq" : "+D"(d), "+S"(s), "+c"(quads)',
     '    quads -= quads > 0;\n    __asm__ volatile("rep movsq" : "+D"(d), "+S"(s), "+c"(quads)'),
    ('the-kernel-copy-forgets-its-tail',
     '    __asm__ volatile("rep movsb" : "+D"(d), "+S"(s), "+c"(tail) : : "memory");', '    (void)tail;'),
    ('the-end-is-read-after-the-passes', COPY_UP_ENDS + COPY_UP_LOOP, COPY_UP_LOOP + COPY_UP_ENDS),
    ('the-passes-stop-a-pass-early', 'left > 64; left -= 64, to += 64, from += 64)',
     'left > 128; left -= 64, to += 64, from += 64)'),
    ('the-stores-align-to-the-source', '    size_t skip = -(uintptr_t)d & 15;', '    size_t skip = -(uintptr_t)s & 15;'),
    ('the-first-sixteen-are-not-written', '    *(v16 *)d = head;\n', '    (void)head;\n'),
    # memmove, backward.
    ('memmove-always-goes-forward', '    else if ((uintptr_t)dst - (uintptr_t)src >= n)',
     '    else if (((void)src, 1))'),
    ('the-start-is-read-after-the-passes', COPY_DOWN_ENDS + COPY_DOWN_LOOP, COPY_DOWN_LOOP + COPY_DOWN_ENDS),
    ('the-backward-passes-stop-early', 'left > 64; left -= 64, to -= 64, from -= 64)',
     'left > 128; left -= 64, to -= 64, from -= 64)'),
    ('the-backward-stores-align-to-the-source', '    size_t skip = (uintptr_t)(d + n) & 15;',
     '    size_t skip = (uintptr_t)(s + n) & 15;'),
    ('the-last-sixteen-are-not-moved-back', '    *(v16 *)(d + n - 16) = tail;\n', '    (void)tail;\n'),
    # memset.
    ('the-fill-word-is-not-the-byte', '0x0101010101010101ull', '0x0101010101010100ull'),
    ('the-fill-vector-is-not-the-byte', '    v += b;', '    v += (uint8_t)(b + 1);'),
    ('rep-stosb-fills-with-nothing', '"+c"(n) : "a"(b) : "memory");', '"+c"(n) : "a"(b ^ b) : "memory");'),
    ('the-kernel-fill-skips-its-head', '    __asm__ volatile("rep stosb" : "+D"(d), "+c"(head) : "a"(b) : "memory");',
     '    d += head;'),
    ('the-kernel-fill-word-is-not-the-byte', ': "a"(w) : "memory");', ': "a"(w + 1) : "memory");'),
    ('the-kernel-fill-drops-a-word', '    __asm__ volatile("rep stosq" : "+D"(d), "+c"(quads)',
     '    quads -= quads > 0;\n    __asm__ volatile("rep stosq" : "+D"(d), "+c"(quads)'),
    ('the-kernel-fill-forgets-its-tail', '    __asm__ volatile("rep stosb" : "+D"(d), "+c"(tail) : "a"(b) : "memory");',
     '    (void)tail;'),
    ('the-fill-passes-stop-early', 'left > 64; left -= 64, to += 64) {', 'left > 128; left -= 64, to += 64) {'),
    ('the-fill-forgets-its-end', '        *(v16 *)(d + n - 16) = v;\n    } else if (n > 32) {',
     '    } else if (n > 32) {'),
    ('one-byte-is-not-filled', '        *d = b;', '        (void)b;'),
]

caught, missed, unbuilt = [], [], []
for name, old, new in mutants:
    if args.only and args.only not in name:
        continue
    path = root / STR
    original = path.read_bytes()
    text = original.decode()
    if text.count(old) != 1:
        print(f'{name}: the text to break is not found once in {STR}', file=sys.stderr)
        unbuilt.append(name)
        continue
    log = args.logs / f'{name}.log'
    try:
        path.write_text(text.replace(old, new))
        with log.open('w') as out:
            try:
                result = subprocess.run(['bash', '-c', CASES], cwd=root, stdout=out, stderr=out, timeout=900)
                code = result.returncode
            except subprocess.TimeoutExpired:
                code = 'timeout'
    finally:
        path.write_bytes(original)
        if path.read_bytes() != original:
            print(f'{name}: {STR} WAS NOT RESTORED', file=sys.stderr)
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

print(f'\nstr mutants: {len(caught)} caught, {len(missed)} missed, {len(unbuilt)} did not build')
if missed:
    print('missed: ' + ', '.join(missed))
if unbuilt:
    print('did not build: ' + ', '.join(unbuilt))
sys.exit(1 if missed or unbuilt else 0)
