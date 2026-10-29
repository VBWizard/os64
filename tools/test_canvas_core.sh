#!/bin/bash
# Native Canvas and shared paint under ASan/UBSan, with leak checks enabled.
set -euo pipefail
cd "$(dirname "$0")/.."
work=${1:-$(mktemp -d /tmp/os64-canvas-XXXXXX)}
mkdir -p "$work"
mapfile -t sources < <(python3 - <<'PY'
from pathlib import Path
for name in ('draw2d', 'canvas'):
    text = Path(f'userland/lib{name}/library.mk').read_text().replace('\\\n', ' ')
    for source in text.split(f'LIB{name.upper()}_SRCS := ', 1)[1].splitlines()[0].split():
        print('userland/' + source)
PY
)
"${CC:-cc}" -O2 -g -std=c11 -Wall -Wextra -Werror -fno-builtin \
    -fno-tree-loop-distribute-patterns -fno-fast-math -ffp-contract=off \
    -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer \
    -Iuserland/libdraw2d/include -Iuserland/libcanvas/include -Iuserland/libos64/include \
    "${sources[@]}" tools/test_canvas_core.c -o "$work/canvas-core"
ASAN_OPTIONS=${ASAN_OPTIONS:-detect_leaks=1} UBSAN_OPTIONS=halt_on_error=1 \
    "$work/canvas-core" 2>&1 | tee "$work/results.txt"
