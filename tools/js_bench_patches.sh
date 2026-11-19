#!/usr/bin/env bash
# js_bench_patches.sh — stock QuickJS's own runner (qjs) built on libjs's
# pinned engine with only the first N of libjs's patches applied, for
# finding which patches cost the engine time (tools/js_bench_run.py times
# the result against ~/src/quickjs/qjs).
#
#   tools/js_bench_patches.sh N OUT
#
# The patches are applied as tools/js_prepare.py applies them (the
# manifest's order, `patch -F0 -p1`). Every os64-specific part of them is
# guarded by OS64_JS_TARGET, which this build leaves undefined, so what is
# measured is each patch's effect on the engine itself, built by the host's
# compiler with upstream's own flags, beside upstream's libc layer and runner
# from ~/src/quickjs (the same release; OS64_REFERENCE.txt there).
set -euo pipefail
cd "$(git rev-parse --show-toplevel)"
count=${1:?usage: tools/js_bench_patches.sh N OUT}
out=$(realpath -m "${2:?usage: tools/js_bench_patches.sh N OUT}")
reference=${JSB_QUICKJS:-$HOME/src/quickjs}
[ -f "$reference/qjs.c" ] && [ -f "$reference/repl.c" ] || {
    echo "js_bench_patches: $reference holds no built QuickJS tree (make qjs there first)" >&2; exit 2; }
work=$(mktemp -d -t os64-js-patches.XXXXXX)
trap 'rm -rf "$work"' EXIT
python3 -I - "$count" "$work" <<'PY'
import json, shutil, subprocess, sys
from pathlib import Path
count, work = int(sys.argv[1]), Path(sys.argv[2])
root = Path('userland/libjs')
manifest = json.loads((root / 'manifest.json').read_text())
for entry in manifest['retained_files']:
    shutil.copy(root / entry['path'], work / Path(entry['path']).name)
if count > len(manifest['patches']):
    sys.exit('js_bench_patches: there are %d patches' % len(manifest['patches']))
for entry in manifest['patches'][:count]:
    with (root / entry['path']).open('rb') as patch:
        if subprocess.run(['patch', '-s', '-F0', '-d', str(work), '-p1'], stdin=patch).returncode:
            sys.exit('js_bench_patches: ' + entry['path'] + ' did not apply')
print('applied %d patches, the last %s' % (count, manifest['patches'][count - 1]['path'] if count else 'none'))
PY
for f in qjs.c repl.c quickjs-libc.c quickjs-libc.h list.h; do cp "$reference/$f" "$work/"; done
# JSB_TRACKING=1: the runner turns on execution-scope tracking (patch 0023)
# as soon as it makes its runtime, as libjs's runtimes that keep failure
# records do (yonder's among them), so its per-call cost is measured too.
if [ "${JSB_TRACKING:-0}" = 1 ]; then
    sed -i 's/^\([[:space:]]*\)rt = JS_NewRuntime();/&\n\1JS_EnableExecutionScopeTracking(rt);/' "$work/qjs.c"
    grep -q JS_EnableExecutionScopeTracking "$work/qjs.c" || { echo "js_bench_patches: could not turn tracking on" >&2; exit 2; }
fi
# A header js_prepare.py writes beside the sources, which the patched
# engine includes; a stand-in serves, since only os64's runtime reads it.
[ -f "$work/os64_js_engine_id.h" ] || cp userland/obj/js/upstream/os64_js_engine_id.h "$work/" 2>/dev/null || true
gcc -O2 -fwrapv -D_GNU_SOURCE '-DCONFIG_VERSION="2026-06-04"' -Wno-array-bounds -Wno-format-truncation \
    -o "$out" "$work"/{qjs,repl,quickjs,dtoa,libregexp,libunicode,cutils,quickjs-libc}.c -lm -lpthread -ldl
echo "js_bench_patches: built $out"
