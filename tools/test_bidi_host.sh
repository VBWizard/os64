#!/bin/bash
# The Bidi_Class table three ways: the generated .inc is not stale, the
# lookups say what the standard says, and the WHOLE code space agrees with an
# independent read of the pinned Unicode data file.
#
# The independent read is the point of the digest. The C side knows only the
# generated table; the check below parses tools/unicode/DerivedBidiClass.txt
# again, here, with its own rules — so a generator that mis-read the file, a
# binary search that walked off a range edge, and a stale .inc are three
# different failures instead of one silent agreement.
set -eu
cd "$(git rev-parse --show-toplevel)"
export PYTHONDONTWRITEBYTECODE=1

work=$(mktemp -d)
cleanup() {
    status=$?
    if [ "$status" -eq 0 ]; then rm -rf "$work"; else echo "bidi test artifacts: $work" >&2; fi
}
trap cleanup EXIT

python3 tools/gen_bidi_table.py --check
cc -std=c11 -g -O1 -Wall -Wextra -Werror -fsanitize=address,undefined \
   -fno-sanitize-recover=all \
   -I userland/libos64/include -I abi/include \
   tools/test_bidi_host.c userland/libos64/bidi.c userland/libos64/str.c -o "$work/test_bidi"

"$work/test_bidi"
"$work/test_bidi" --digest > "$work/digest"
cat "$work/digest"

python3 - "$work/digest" <<'PY'
import json, pathlib, re, sys

data = pathlib.Path('tools/unicode/DerivedBidiClass.txt')
pinned = json.loads(pathlib.Path('tools/unicode/PINNED.json').read_text())
LAST = 0x10FFFF
# The three strong classes and the two spellings the file uses for each: the
# short name on an assignment row, the long one on an @missing line.
STRONG = {'L': 0, 'Left_To_Right': 0, 'R': 1, 'Right_To_Left': 1,
          'AL': 2, 'Arabic_Letter': 2}
NOT_STRONG = 3

klass = [None] * (LAST + 1)
defaults, rows = [], []
for line in data.read_text(encoding='utf-8').splitlines():
    at = re.match(r'^#\s*@missing:\s*([0-9A-F]+)\.\.([0-9A-F]+);\s*(\S+)', line)
    if at:
        defaults.append((int(at.group(1), 16), int(at.group(2), 16), at.group(3)))
        continue
    body = line.split('#')[0].strip()
    if not body:
        continue
    span, _, name = body.partition(';')
    first, _, last = span.strip().partition('..')
    rows.append((int(first, 16), int(last or first, 16), name.strip()))
# @missing first because it is the answer for what is NOT listed; the
# assignment rows then overwrite the code points that are.
for first, last, name in defaults + rows:
    for cp in range(first, last + 1):
        klass[cp] = STRONG.get(name, NOT_STRONG)

hash_ = 0xcbf29ce484222325
counts = [0, 0, 0, 0]
for cp in range(LAST + 1):
    k = klass[cp]
    if k is None:
        raise SystemExit(f'U+{cp:04X}: the data file gives no class and no default')
    counts[k] += 1
    hash_ = ((hash_ ^ k) * 0x100000001b3) & 0xFFFFFFFFFFFFFFFF

want = (f'bidi digest {hash_:016x} L={counts[0]} R={counts[1]} '
        f'AL={counts[2]} not-strong={counts[3]}')
got = pathlib.Path(sys.argv[1]).read_text().strip()
if got != want:
    print(f'  table: {got}\n  pinned Unicode {pinned["version"]}: {want}', file=sys.stderr)
    raise SystemExit('the table and the data file disagree')
print(f'digest agrees with the pinned Unicode {pinned["version"]} data '
      f'across {LAST + 1} code points')
PY
