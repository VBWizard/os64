#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
work=${1:?usage: test_png_encode_host.sh OUTPUT};mkdir -p "$work"
cc -O2 -g -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all \
 -Iuserland/libpng/include -Iuserland/libgzip/include -Iuserland/libos64/include -Iabi/include \
 userland/libpng/{png,encode}.c userland/libgzip/{inflate,zlib}.c userland/libos64/crc32.c \
 tools/test_png_encode_host.c -o "$work/png-encode"
ASAN_OPTIONS=${ASAN_OPTIONS:-detect_leaks=1} "$work/png-encode" "$work/encoded.png"
python3 - "$work/encoded.png" <<'PY'
import sys
from PIL import Image
im=Image.open(sys.argv[1]);assert im.size==(257,257)
assert abs(im.info['dpi'][0]-96)<.02
assert im.info['srgb']==0
pixels=im.convert('RGBA').load()
for y in range(257):
 for x in range(257):assert pixels[x,y]==(x%256,y%256,(x^y)%256,(x+y)%256)
print('Independent Pillow decode: 66049 exact RGBA pixels, sRGB and 96 dpi passed')
PY
