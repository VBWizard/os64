#!/bin/bash
set -eu
cd "$(git rev-parse --show-toplevel)"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

python3 - "$work" <<'PY'
import pathlib, struct, sys, zlib
root = pathlib.Path(sys.argv[1])
def chunk(kind, content=b''):
    body = kind + content
    return struct.pack('>I', len(content)) + body + struct.pack('>I', zlib.crc32(body))
def png(size):
    rgba = bytes([17, 91, 203, 127]) * size
    raw = b''.join(b'\0' + rgba for _ in range(size))
    return (b'\x89PNG\r\n\x1a\n' +
            chunk(b'IHDR', struct.pack('>IIBBBBB', size, size, 8, 6, 0, 0, 0)) +
            chunk(b'IDAT', zlib.compress(raw)) + chunk(b'IEND'))
def png16(size):
    rgba = bytes([0, 17, 0, 91, 0, 203, 0, 127]) * size
    raw = b''.join(b'\0' + rgba for _ in range(size))
    return (b'\x89PNG\r\n\x1a\n' +
            chunk(b'IHDR', struct.pack('>IIBBBBB', size, size, 16, 6, 0, 0, 0)) +
            chunk(b'IDAT', zlib.compress(raw)) + chunk(b'IEND'))
for name, size in [('exact', 24), ('small', 16), ('bad-size', 16),
                   ('retry', 24), ('open-retry', 24)]:
    suffix = 24 if name == 'bad-size' else size
    (root / f'{name}-{suffix}.png').write_bytes(png(size))
(root / 'bad-24.png').write_bytes(png(24)[:-7])
(root / 'large-24.png').write_bytes(b'x' * 65537)
(root / 'wide-64.png').write_bytes(png16(64))
(root / 'staged.png').write_bytes(png(24))
for i in range(130):
    (root / f'pin{i:03}-64.png').write_bytes(png(64))
PY

cc -std=c11 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
  -ffunction-sections -fdata-sections \
  -I userland/libimage/include -I userland/libos64/include \
  -I userland/libpng/include -I userland/libgzip/include -I abi/include \
  userland/libimage/icons.c userland/libos64/ui_icon.c \
  userland/libpng/png.c userland/libgzip/inflate.c userland/libgzip/zlib.c \
  userland/libos64/crc32.c tools/test_icons_host.c \
  -Wl,--gc-sections -o "$work/test_icons"
ASAN_OPTIONS=detect_leaks=1 "$work/test_icons" "$work"
