#!/bin/bash
# Build and run bench.c from the repository root: bash docs/fonts/f4-evidence/c3/log-bench/run.sh
# Unsanitized -O2, so the numbers are the code's and not ASan's. The whole
# program is built with -fno-tree-loop-distribute-patterns, as FreeType's
# objects always are: at -O2 GCC otherwise turns the byte loops in libos64's
# own str.c into calls to the functions they implement, and recurses.
set -e
ROOT=$(cd "$(dirname "$0")/../../../../.." && pwd)
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=$(mktemp -d /tmp/os64-log-bench.XXXXXX)
cd "$ROOT"
sed 's|^int main(int argc, char \*\*argv)$|int harness_main(int argc, char **argv)|' \
    tools/test_scribe_host.c > "$OUT/scribe_harness_copy.c"
FLAGS="-std=c11 -O2 -g -fno-omit-frame-pointer -fno-tree-loop-distribute-patterns \
 -ffunction-sections -fdata-sections -Wl,--gc-sections -I$OUT -Itools \
 -Iuserland/libos64/include -Iuserland/libos64 -Iabi/include -Itools/fonts \
 -Iuserland/apps/scribe -DUI_TEXT_REAL"
python3 - "$OUT" "$FLAGS" <<'EOF'
import runpy, subprocess, sys
from pathlib import Path
out, flags = Path(sys.argv[1]), sys.argv[2].split()
up, port = runpy.run_path('tools/test_freetype_host.py')['sources']()
for n, s in enumerate(up + port):
    extra = ['-DFT2_BUILD_LIBRARY', '-Wno-unused-variable', '-Wno-unused-but-set-variable'] if s in up else []
    subprocess.run(['cc', *flags, '-fcf-protection=none', '-fno-builtin',
                    '-DOS64_FREETYPE_HOSTED', '-Iuserland/libfreetype/port',
                    '-Iuserland/libfreetype/upstream/include', *extra,
                    '-c', str(s), '-o', str(out / f'ft{n}.o')], check=True)
EOF
cc $FLAGS -w "$HERE/bench.c" userland/apps/scribe/scribe_buf.c \
   userland/libos64/{ui_font,ui,ui_controls,ui_list,ui_text,font_provider,font_adopt,text,text_cache,text_decode,text_bitmap,text_draw,draw,str}.c \
   "$OUT"/ft*.o -o "$OUT/bench"
"$OUT/bench" userland/libfreetype/fixtures
