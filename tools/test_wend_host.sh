#!/bin/bash
# wend's renderer on the host: the fold's table, the
# wrapper's edges, an allocation failure at every step, and every saved corpus
# page rendered and diffed against its checked-in dump.
#
# Refresh the dumps deliberately, never by accident:
#   tools/test_wend_host.sh --refresh
set -eu
cd "$(git rev-parse --show-toplevel)"
export PYTHONDONTWRITEBYTECODE=1

refresh=0
[ "${1:-}" = "--refresh" ] && refresh=1

work=$(mktemp -d)
cleanup() {
    status=$?
    if [ "$status" -eq 0 ]; then rm -rf "$work"; else echo "wend test artifacts: $work" >&2; fi
}
trap cleanup EXIT

cc -std=c11 -g -O1 -Wall -Wextra -Werror -fsanitize=address,undefined \
   -fno-sanitize-recover=all \
   -I userland/libhtml/include -I userland/libos64/include -I userland \
   -I abi/include \
   tools/test_wend_host.c userland/apps/wend/render.c \
   userland/libhtml/core.c userland/libhtml/encoding.c \
   userland/libhtml/tokenizer.c userland/libhtml/tree.c \
   userland/libos64/str.c userland/libos64/url.c userland/libos64/fmt.c \
   -o "$work/wend_driver"

"$work/wend_driver" --checks

# THE CORPUS IS RENDERED AT THE WIDTH A PERSON READS IT AT, and `example` at a
# second, narrow one: wrapping is where a renderer goes wrong, and a page that
# fits in 80 columns proves nothing about it.
python3 - "$work" "$refresh" <<'PY'
import json, pathlib, subprocess, sys

work, refresh = sys.argv[1], sys.argv[2] == '1'
root = pathlib.Path('tools/html_corpus')
sources = json.loads((root / 'SOURCES.json').read_text())
widths = {name: [80] for name in sources}
widths['example'].append(40)

failed = False
for name, source in sorted(sources.items()):
    for width in widths[name]:
        args = [f'{work}/wend_driver', '--render', str(root / f'{name}.html'),
                '--width', str(width), '--base', source['final_url'], '--name', name]
        if source['charset']:
            args += ['--charset', source['charset']]
        got = subprocess.run(args, stdout=subprocess.PIPE, check=True).stdout
        snapshot = root / (f'{name}.lines' if width == 80 else f'{name}.{width}.lines')
        if refresh:
            snapshot.write_bytes(got)
            print(f'{name} @{width}: {len(got.splitlines())} dump lines written')
            continue
        want = snapshot.read_bytes() if snapshot.exists() else b''
        if want != got:
            failed = True
            print(f'{name} @{width}: DIFFERS from {snapshot}', file=sys.stderr)
            pathlib.Path(f'{work}/{snapshot.name}').write_bytes(got)
            subprocess.run(['diff', '-u', str(snapshot), f'{work}/{snapshot.name}'])
        else:
            lines = got.split(b'\n--\n')[0].decode('latin-1')
            counts = dict(l.split(': ', 1) for l in lines.splitlines() if ': ' in l)
            print(f"{name} @{width}: {counts.get('lines')} lines, "
                  f"{counts.get('spots')} spots, {counts.get('forms')} forms, "
                  f"incomplete={counts.get('incomplete')}")
sys.exit(1 if failed else 0)
PY
