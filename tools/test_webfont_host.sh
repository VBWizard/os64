#!/bin/bash
# yonder's font job (userland/apps/yonder/webfont.c) under the sanitizers,
# against a licensed fixture and a WOFF made from it with Python's struct
# alone (each table stored): read, fetched, unwrapped, refused, and every
# allocation refused in turn.

set -eu
cd "$(git rev-parse --show-toplevel)"

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

ttf=userland/libfreetype/fixtures/DejaVuSans.ttf
python3 - "$ttf" "$work/a.woff" <<'PY'
import struct, sys
sfnt = open(sys.argv[1], 'rb').read()
n = struct.unpack_from('>H', sfnt, 4)[0]
ts = [struct.unpack_from('>4sIII', sfnt, 12 + 16 * i) for i in range(n)]
first = 44 + 20 * n
entries, data = [], b''
for tag, check, off, length in ts:
    body = sfnt[off:off + length]
    entries.append(struct.pack('>4sIIII', tag, first + len(data), length, length, check))
    data += body + b'\0' * (-len(body) % 4)
total = 12 + 16 * n + sum((t[3] + 3) & ~3 for t in ts)
length = first + len(data)
head = struct.pack('>4sIIHHIHHIIIII', b'wOFF', struct.unpack_from('>I', sfnt, 0)[0], length, n, 0, total,
                   1, 0, 0, 0, 0, 0, 0)
open(sys.argv[2], 'wb').write(head + b''.join(entries) + data)
PY

cc -std=c11 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all \
   -fno-builtin -fno-tree-loop-distribute-patterns \
   -I userland/apps/yonder -I userland/libgzip/include -I userland/libway/include \
   -I userland/libfetch/include -I userland/libtls/include -I userland/libhtml/include \
   -I userland/libpage/include -I userland/libos64/include -I abi/include \
   userland/apps/yonder/webfont.c userland/apps/yonder/font_unwrap.c userland/libgzip/zlib.c \
   userland/libgzip/inflate.c userland/libos64/str.c userland/libos64/fmt.c tools/test_webfont_host.c \
   -o "$work/webfont"
"$work/webfont" "$ttf" "$work/a.woff"
