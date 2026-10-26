#!/usr/bin/env bash
# test_importmap_host.sh — yonder's import map (userland/apps/yonder/
# importmap.c) on the host, under ASan and UBSan: the merge's rules, every
# allocation failing in turn, and WPT's data-driven import map cases
# (tools/wpt/importmap_cases.py), which need the WPT checkout
# (tools/wpt_fetch.sh; WPT_DIR elsewhere).
#
# The data-driven verdicts are held to tools/wpt/import-maps-data.txt, the
# cases known to fail: a case not on it that fails, and a case on it that
# passes, each fail the suite. --update rewrites the list from this run. (It
# is not in tools/wpt/expected/, whose files are the WPT runner's, one for
# each directory it runs.)
set -euo pipefail
cd "$(git rev-parse --show-toplevel)"
checkout=${WPT_DIR:-.vendor/wpt}
expected=tools/wpt/import-maps-data.txt
if [ ! -d "$checkout/import-maps/data-driven" ]; then
    echo "importmap: no WPT checkout with import-maps in $checkout; run tools/wpt_fetch.sh" >&2
    exit 2
fi
work=$(mktemp -d -t os64-importmap.XXXXXX)
cleanup() {
    status=$?
    if [ "$status" -eq 0 ]; then rm -rf "$work"; else echo "importmap test artifacts: $work" >&2; fi
}
trap cleanup EXIT

cc -std=gnu11 -g -O1 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all \
   -I userland/apps/yonder -I userland/libpage/include -I userland/libhtml/include -I userland/libos64/include \
   -I abi/include -I userland/libpage/upstream/ryu \
   tools/test_importmap_host.c userland/apps/yonder/importmap.c \
   userland/libpage/core.c userland/libpage/state.c userland/libpage/resolve.c userland/libpage/value.c \
   userland/libpage/number.c userland/libpage/range.c userland/libpage/upstream/ryu/ryu/d2s.c \
   userland/libpage/submit.c userland/libpage/encode.c userland/libpage/refresh.c userland/libpage/activate.c \
   userland/libhtml/core.c userland/libhtml/encoding.c userland/libhtml/tokenizer.c \
   userland/libhtml/tree.c userland/libhtml/dom.c userland/libhtml/fragment.c userland/libhtml/serialize.c \
   userland/libos64/str.c userland/libos64/bidi.c userland/libos64/url.c userland/libos64/fmt.c \
   -o "$work/importmap"

"$work/importmap" --merge
"$work/importmap" --sweep
python3 tools/wpt/importmap_cases.py "$checkout" > "$work/cases.txt"
"$work/importmap" "$work/cases.txt" > "$work/verdicts.txt"
pass=$(grep -c '^PASS' "$work/verdicts.txt" || true)
skip=$(grep -c '^SKIP' "$work/verdicts.txt" || true)
grep '^FAIL' "$work/verdicts.txt" | cut -f2 | LC_ALL=C sort > "$work/failing.txt" || true
if [ "${1:-}" = "--update" ]; then
    {
        echo "# The data-driven import map cases yonder's map fails, written by"
        echo "# tools/test_importmap_host.sh --update. Each line is a case and the"
        echo "# check that fails in it; MODERN_WEB.md § Import maps, as built, says why."
        cat "$work/failing.txt"
    } > "$expected"
    echo "importmap: $pass pass, $(wc -l < "$work/failing.txt") fail, $skip libdom's; $expected rewritten"
    exit 0
fi
{ grep -v '^#' "$expected" || true; } | LC_ALL=C sort > "$work/expected.txt"
status=0
while IFS= read -r line; do
    got=$(grep -F -m1 "FAIL	$line	" "$work/verdicts.txt" | cut -f3)
    echo "importmap: FAILS, and is not expected to: $line (got: $got)" >&2
    status=1
done < <(LC_ALL=C comm -23 "$work/failing.txt" "$work/expected.txt")
while IFS= read -r line; do
    echo "importmap: PASSES, and the expectations say it fails (--update): $line" >&2
    status=1
done < <(LC_ALL=C comm -13 "$work/failing.txt" "$work/expected.txt")
echo "importmap: data-driven $pass pass, $(wc -l < "$work/failing.txt") expected to fail, $skip libdom's"
exit $status
