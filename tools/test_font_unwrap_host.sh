#!/bin/bash
# yonder's WOFF unwrapper (userland/apps/yonder/font_unwrap.c) under the
# sanitizers. WOFF files are made here from the licensed fixtures with
# Python's zlib alone — the host has no fontTools — and each must unwrap to
# its font's own tables, byte for byte; then every header and directory
# field is broken in turn, each to its own refusal; then the sweep.

set -eu
cd "$(git rev-parse --show-toplevel)"

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

cc -std=c11 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
   -fno-sanitize-recover=all \
   -I userland/apps/yonder -I userland/libgzip/include \
   -I userland/libos64/include -I abi/include \
   userland/apps/yonder/font_unwrap.c userland/libgzip/zlib.c \
   userland/libgzip/inflate.c tools/test_font_unwrap_host.c \
   -o "$work/unwrap"

python3 - "$work" "$work/unwrap" <<'PY'
import pathlib, struct, subprocess, sys, zlib

work, unwrap = pathlib.Path(sys.argv[1]), sys.argv[2]
fixtures = pathlib.Path('userland/libfreetype/fixtures')
faces = ['DejaVuSans.ttf', 'DejaVuSerif-BoldItalic.ttf', 'DejaVuSansMono.ttf',
         'SourceSans3-Regular.otf']

def tables(sfnt):
    n = struct.unpack_from('>H', sfnt, 4)[0]
    out = []
    for i in range(n):
        tag, check, off, length = struct.unpack_from('>4sIII', sfnt, 12 + 16 * i)
        out.append((tag, check, sfnt[off:off + length]))
    return struct.unpack_from('>I', sfnt, 0)[0], out

def woff(sfnt, store=(), meta=b'', reverse=False, shift=None, tail=None):
    # `reverse` lays the data in the opposite order to the tags; `shift`
    # puts that table's data 2 bytes off a 4-byte boundary; `tail` follows
    # that table's zlib stream with 4 stray bytes inside its length.
    flavor, ts = tables(sfnt)
    n = len(ts)
    entries, data = [None] * n, b''
    first = 44 + 20 * n
    for i in (reversed(range(n)) if reverse else range(n)):
        tag, check, body = ts[i]
        packed = zlib.compress(body, 9)
        if tag in store or len(packed) >= len(body):
            packed = body
        if tag == tail:
            packed = zlib.compress(body, 9) + b'\0\0\0\0'
        if tag == shift:
            data += b'\0\0'
        entries[i] = (tag, first + len(data), len(packed), len(body), check)
        data += packed + b'\0' * (-len(data + packed) % 4)
    total = 12 + 16 * n + sum((len(b) + 3) & ~3 for _, _, b in ts)
    meta_off = first + len(data) if meta else 0
    length = first + len(data) + len(meta)
    head = struct.pack('>4sIIHHIHHIIIII', b'wOFF', flavor, length, n, 0, total, 1, 0,
                       meta_off, len(meta), len(meta), 0, 0)
    return bytearray(head + b''.join(struct.pack('>4sIIII', *e) for e in entries) + data + meta)

def run(*args):
    return subprocess.run([unwrap, *map(str, args)], capture_output=True, text=True)

checked = 0
for face in faces:
    sfnt = (fixtures / face).read_bytes()
    w = woff(sfnt, store={b'name'}, reverse=face.startswith('DejaVuSerif'))
    (work / (face + '.woff')).write_bytes(w)
    r = run('unwrap', work / (face + '.woff'), work / (face + '.out'))
    assert r.returncode == 0, (face, r.stdout, r.stderr)
    out = (work / (face + '.out')).read_bytes()
    want_flavor, want = tables(sfnt)
    got_flavor, got = tables(out)
    assert got_flavor == want_flavor and got == want, face
    n = len(want)
    power = 1 << (n.bit_length() - 1)
    assert struct.unpack_from('>HHHH', out, 4) == (n, power * 16, power.bit_length() - 1, n * 16 - power * 16), face
    checked += 1
print(f'WOFF unwrap: {checked} faces, every table back byte for byte, the directory rebuilt '
      'in the order of its tags whatever order the data lay in')

# Containers valid but for the one rule each breaks.
def synth(tables_):
    # A bare SFNT of the given (tag, body) tables, for the unwrapper alone.
    n = len(tables_)
    head, body, at = b'', b'', 12 + 16 * n
    for tag, b in tables_:
        head += struct.pack('>4sIII', tag, 0, at + len(body), len(b))
        body += b + b'\0' * (-len(b) % 4)
    return struct.pack('>IHHHH', 0x00010000, n, 16, 0, 0) + head + body
def status(name, w, want):
    (work / 'iso.woff').write_bytes(w)
    got = run('status', work / 'iso.woff').stdout.strip()
    assert got == want, (name, got, want)
mono = (fixtures / 'DejaVuSansMono.ttf').read_bytes()
_, mono_tables = tables(mono)
status('a table off its 4-byte boundary', woff(mono, store={b'name'}, shift=mono_tables[-1][0]), 'malformed container')
status('stray bytes after a table\'s stream', woff(mono, tail=b'glyf'), 'malformed container')
same = synth([(b'aaaa', b'12345678'), (b'bbbb', b'12345678')])
w = woff(same)
struct.pack_into('>I', w, 44 + 20 + 4, struct.unpack_from('>I', w, 44 + 4)[0])
status('two tables over one stretch', w, 'malformed container')
big = synth([(b'aaaa', bytes(range(256)) * 8192)])
w = woff(big, store={b'aaaa'})     # 2 MiB as stored: 64 to 1 allows 128 MiB
struct.pack_into('>I', w, 16, (64 << 20) + 4)
status('an SFNT past 64 MiB within 64 to 1', w, 'past the size limits')
many = synth([(struct.pack('>I', i), b'\x01\x02\x03\x04') for i in range(1, 4097)])
status('4096 tables, past the 16-bit search fields', woff(many), 'malformed container')
status('4095 tables, the most', woff(synth([(struct.pack('>I', i), b'\x01\x02\x03\x04') for i in range(1, 4096)])), 'ok')
for flavor in (b'ttcf', b'wOFF', b'wOF2'):
    w = woff(mono)
    w[4:8] = flavor
    status(f'a {flavor.decode()} flavor', w, 'malformed container')
w = woff(mono, store={b'name'})
off1 = struct.unpack_from('>I', w, 44 + 20 * 1 + 4)[0]
gap = w[:off1] + b'\0\0\0\0' + w[off1:]
for i in range(len(mono_tables)):
    o = struct.unpack_from('>I', gap, 44 + 20 * i + 4)[0]
    if o >= off1:
        struct.pack_into('>I', gap, 44 + 20 * i + 4, o + 4)
struct.pack_into('>I', gap, 8, len(gap))
status('a gap between two tables', gap, 'malformed container')
w = woff(mono) + b'\0\0\0\0'
struct.pack_into('>I', w, 8, len(w))
status('bytes after the last part', w, 'malformed container')
w = woff(mono)
struct.pack_into('>II', w, 28, 4, 4)
status('a metadata length with no offset', w, 'malformed container')
w = woff(mono)
struct.pack_into('>I', w, 40, 4)
status('a private length with no offset', w, 'malformed container')
w = woff(synth([(b'aaaa', b'')]))
struct.pack_into('>I', w, 44 + 12, 8000)      # nothing stored, 8000 claimed:
struct.pack_into('>I', w, 16, 12 + 16 + 8000)  # past 64 to 1 if read on
status('a container that is its directory alone', w, 'malformed container')
status('an SFNT that is its directory alone', woff(synth([(b'aaaa', b''), (b'bbbb', b'')]), meta=b'<m/>'),
       'malformed container')
w = woff(mono, meta=b'<metadata/>')
struct.pack_into('>I', w, 32, 0)
status('metadata unpacking to nothing', w, 'malformed container')
status('a first tag of zero', woff(synth([(b'\0\0\0\0', b'1234'), (b'bbbb', b'5678')])), 'malformed container')
print('WOFF unwrap: misalignment, a stream with bytes after it, overlap, 64 MiB, 4096 tables, '
      'a collection or container flavor, a gap, trailing bytes, lengths with no offset, either file '
      'its directory alone, metadata from nothing and a zero first tag, each alone')

# Each field broken in turn, each to the status it must give.
base = woff((fixtures / 'DejaVuSansMono.ttf').read_bytes(), meta=b'<metadata/>')
n = struct.unpack_from('>H', base, 12)[0]
def entry(b, i, field, value):
    struct.pack_into('>I', b, 44 + 20 * i + 4 * field, value)
def case(name, mutate, want):
    b = bytearray(base)
    mutate(b)
    (work / 'case.woff').write_bytes(b)
    got = run('status', work / 'case.woff').stdout.strip()
    assert got == want, (name, got, want)
    return name
cases = [
    case('whole', lambda b: None, 'ok'),
    case('signature', lambda b: b.__setitem__(slice(0, 4), b'wOFX'), 'malformed container'),
    case('length short', lambda b: struct.pack_into('>I', b, 8, len(b) - 1), 'malformed container'),
    case('length long', lambda b: struct.pack_into('>I', b, 8, len(b) + 4), 'malformed container'),
    case('no tables', lambda b: struct.pack_into('>H', b, 12, 0), 'malformed container'),
    case('too many tables', lambda b: struct.pack_into('>H', b, 12, 0xffff), 'malformed container'),
    case('reserved', lambda b: struct.pack_into('>H', b, 14, 1), 'malformed container'),
    case('total size', lambda b: struct.pack_into('>I', b, 16, struct.unpack_from('>I', b, 16)[0] + 4), 'malformed container'),
    case('total past 64 MiB', lambda b: struct.pack_into('>I', b, 16, (64 << 20) + 4), 'past the size limits'),
    case('total past 64 to 1', lambda b: struct.pack_into('>I', b, 16, len(b) * 64 + 4), 'past the size limits'),
    case('metadata past the end', lambda b: struct.pack_into('>I', b, 24, len(b)), 'malformed container'),
    case('private past the end', lambda b: (struct.pack_into('>I', b, 36, len(b) - 2), struct.pack_into('>I', b, 40, 8)), 'malformed container'),
    case('tags out of order', lambda b: entry(b, 1, 0, 0), 'malformed container'),
    case('offset unaligned', lambda b: entry(b, 0, 1, struct.unpack_from('>I', b, 44 + 4)[0] + 1), 'malformed container'),
    case('offset in the directory', lambda b: entry(b, 0, 1, 44), 'malformed container'),
    case('table past the end', lambda b: entry(b, n - 1, 2, len(b)), 'malformed container'),
    case('packed longer than unpacked', lambda b: entry(b, 0, 3, struct.unpack_from('>I', b, 44 + 8)[0] - 1), 'malformed container'),
    case('overlap', lambda b: entry(b, 1, 1, struct.unpack_from('>I', b, 44 + 4)[0]), 'malformed container'),
    case('unpacks short', lambda b: (entry(b, 0, 3, struct.unpack_from('>I', b, 44 + 12)[0] + 4),
                                     struct.pack_into('>I', b, 16, struct.unpack_from('>I', b, 16)[0] + 4)), 'malformed container'),
    case('zlib broken', lambda b: b.__setitem__(struct.unpack_from('>I', b, 44 + 4)[0] + 5,
                                                b[struct.unpack_from('>I', b, 44 + 4)[0] + 5] ^ 0xff), 'malformed container'),
]
print(f'WOFF unwrap: {len(cases)} header and directory fields broken in turn, each refused')
PY

for face in DejaVuSans.ttf SourceSans3-Regular.otf; do
    printf 'WOFF unwrap sweep, %s: ' "$face"
    ASAN_OPTIONS=detect_leaks=1 "$work/unwrap" sweep "$work/$face.woff"
done
