#!/usr/bin/env bash
# test_yonder_wpt_host.sh — one web-platform-tests directory through yonder's
# own window, headless, against its expectation file (MODERN_WEB.md § The
# oracle; tools/test_yonder_wpt_host.c says what is run, how, and what the
# three sections of an expectation file mean).
#
#   tools/test_yonder_wpt_host.sh dom/nodes             against the expectations
#   tools/test_yonder_wpt_host.sh dom/nodes --update    rewrite [passing] from this run
#   tools/test_yonder_wpt_host.sh dom/nodes --only Node-cloneNode
#   tools/test_yonder_wpt_host.sh --selftest            the runner's own pages
#
# The suite comes from tools/wpt_fetch.sh, at its pin, in .vendor/wpt
# (WPT_DIR elsewhere); a directory's expectations are
# tools/wpt/expected/<dir>.txt. The pages are those WPT's MANIFEST.json
# lists, through tools/wpt/pages.py, when the checkout has one, and those
# found by file name when it does not. Exit 1 is a required test failed or
# a pass lost (with --modern-off, a required test that passed); 2, a run
# that could not be judged.
#
# --selftest runs tools/wpt/selftest/, pages whose every outcome is known,
# beside the checkout's own resources, found by file name and again through
# a manifest (tools/wpt/selftest.manifest.json), and holds the runner to
# tools/wpt/selftest.out line for line both ways; then holds --expect, --modern-off,
# --update and the expectation file's three sections to what they promise.
set -euo pipefail
cd "$(git rev-parse --show-toplevel)"
if [ $# -lt 1 ] || { [[ "$1" == -* ]] && [ "$1" != --selftest ]; }; then
    echo "usage: tools/test_yonder_wpt_host.sh <wpt directory> [--update] [--only <substring>]" >&2
    echo "       tools/test_yonder_wpt_host.sh --selftest" >&2
    exit 2
fi
checkout=${WPT_DIR:-.vendor/wpt}
if [ ! -f "$checkout/resources/testharness.js" ]; then
    echo "wpt: no checkout in $checkout; run tools/wpt_fetch.sh" >&2
    exit 2
fi
work=$(mktemp -d -t os64-yonder-wpt.XXXXXX)
cleanup() {
    result=$?
    if [ "$result" -le 1 ] && [ "${YONDER_WPT_KEEP:-0}" = 0 ]; then
        rm -rf "$work"
    else
        echo "WPT runner artifacts: $work" >&2
    fi
}
trap cleanup EXIT
. tools/yonder_host_build.sh
# The runner includes the scripted-page host whole for its fakes; that
# host's cases are compiled in and never called.
yonder_host_build "$work" tools/test_yonder_wpt_host.c "$work/yonder-wpt" -Wno-unused-function
runner=("$work/yonder-wpt" --report tools/wpt/testharnessreport.js)

if [ "$1" = --selftest ]; then
    fail() { echo "wpt selftest: $*" >&2; exit 2; }
    # The runner, its output with the work directory's name taken out,
    # and its exit status in $rc; what it says on stderr in err.txt.
    run() {
        rc=0
        "${runner[@]}" --wpt "$work/wpt" --dir selftest "$@" > "$work/raw.txt" 2> "$work/err.txt" || rc=$?
        sed "s|$work/||g" "$work/raw.txt" > "$work/out.txt"
    }
    section() { sed -n "/^\\[$1\\]\$/,/^\\[/p" "$2" | sed '1d;/^\[/d'; }
    mkdir "$work/wpt"
    ln -s "$(realpath "$checkout/resources")" "$work/wpt/resources"
    cp -r tools/wpt/selftest "$work/wpt/selftest"
    cp tools/wpt/selftest.expect "$work/expect.txt"

    run --expect "$work/expect.txt"
    [ "$rc" -eq 0 ] && diff -u tools/wpt/selftest.out "$work/out.txt" ||
        fail "found by file name, the output is not tools/wpt/selftest.out (exit $rc)"

    python3 -I tools/wpt/pages.py tools/wpt/selftest.manifest.json selftest > "$work/pages.txt"
    run --expect "$work/expect.txt" --pages "$work/pages.txt"
    grep -v '^selftest: not run: [0-9]* pages, ' "$work/out.txt" > "$work/listed.txt" || true
    [ "$rc" -eq 0 ] && diff -u tools/wpt/selftest.out "$work/listed.txt" ||
        fail "listed by the manifest, the output is not tools/wpt/selftest.out (exit $rc)"
    grep -qx 'selftest: not run: 2 pages, a worker or shadow-realm variant' "$work/out.txt" &&
        grep -qx 'selftest: not run: 1 pages, an XML document' "$work/out.txt" ||
        fail "the manifest's worker variants and XML document were not counted as not run"

    run --expect "$work/expect.txt" --update
    [ "$rc" -eq 0 ] || fail "--update failed (exit $rc)"
    for name in required excluded; do
        diff <(section "$name" tools/wpt/selftest.expect) <(section "$name" "$work/expect.txt") > /dev/null ||
            fail "--update changed [$name], which is written by hand"
    done
    section passing "$work/expect.txt" > "$work/passing.txt"
    [ "$(wc -l < "$work/passing.txt")" -eq "$(grep -c '^NEW' tools/wpt/selftest.out)" ] &&
        LC_ALL=C sort -c "$work/passing.txt" ||
        fail "--update did not write each pass once, in order"
    ! grep -q '^timeout.html' "$work/passing.txt" ||
        fail "a page whose harness timed out passed a test"

    run --expect "$work/expect.txt"
    [ "$rc" -eq 0 ] && ! grep -q '^NEW\|^LOST\|^REQUIRED' "$work/out.txt" ||
        fail "a run against its own passes found a difference (exit $rc)"

    cp "$work/expect.txt" "$work/lost.txt"
    printf '\npass-fail.html\ta pass that is gone\n' >> "$work/lost.txt"
    run --expect "$work/lost.txt"
    [ "$rc" -eq 1 ] && grep -qx 'LOST	pass-fail.html	a pass that is gone' "$work/out.txt" ||
        fail "a pass the file has after a blank line, and the run lacks, was not LOST, exit 1 (exit $rc)"

    sed 's/^\[required\]$/[required]\npass-fail.html\ta test that fails/' "$work/expect.txt" > "$work/required.txt"
    run --expect "$work/required.txt"
    [ "$rc" -eq 1 ] && grep -qx 'REQUIRED	pass-fail.html	a test that fails' "$work/out.txt" ||
        fail "a required test that failed did not fail the run (exit $rc)"

    run --expect "$work/lost.txt" --only clock --update
    grep -qx 'pass-fail.html	a pass that is gone' "$work/lost.txt" ||
        fail "--update with --only dropped a line it did not select"

    printf '[excluded]\nclock.html\n' > "$work/bad.txt"
    run --expect "$work/bad.txt"
    [ "$rc" -eq 2 ] || fail "an exclusion with no reason was taken (exit $rc)"
    printf 'clock.html\ttimers fire in their order as the clock moves\n' > "$work/bad.txt"
    run --expect "$work/bad.txt"
    [ "$rc" -eq 2 ] || fail "a line in no section was taken (exit $rc)"
    printf '[required]\nexcluded.html\tx\n[excluded]\nexcluded.html\twhy\n' > "$work/bad.txt"
    run --expect "$work/bad.txt"
    [ "$rc" -eq 2 ] || fail "a test required of an excluded page was taken (exit $rc)"

    # Expectations that exist and cannot be read judge nothing, and --update
    # leaves them where they are.
    mkdir "$work/dir.txt"
    run --expect "$work/dir.txt" --update
    [ "$rc" -eq 2 ] && [ -d "$work/dir.txt" ] || fail "a directory was taken for expectations (exit $rc)"
    truncate -s $((16 * 1024 * 1024 + 1)) "$work/big.txt"
    run --expect "$work/big.txt" --update
    [ "$rc" -eq 2 ] && [ "$(stat -c %s "$work/big.txt")" -eq $((16 * 1024 * 1024 + 1)) ] ||
        fail "an expectation file past the size it may have was taken, or rewritten (exit $rc)"
    # A run with a fault rewrites nothing, whatever its pages said.
    cp "$work/lost.txt" "$work/fault.txt"
    run --expect "$work/fault.txt" --update --fault
    [ "$rc" -eq 2 ] && cmp -s "$work/lost.txt" "$work/fault.txt" ||
        fail "--update with a fault rewrote the expectations (exit $rc)"

    # With the switch off, a required test that passes leaked through it.
    printf '[required]\npass-fail.html\ta test that passes\n[passing]\n' > "$work/leak.txt"
    run --expect "$work/leak.txt" --modern-off
    [ "$rc" -eq 1 ] && grep -qx 'LEAKED	pass-fail.html	a test that passes' "$work/out.txt" ||
        fail "a required test passing with the switch off was not LEAKED, exit 1 (exit $rc)"
    printf '[required]\npass-fail.html\ta test that fails\n[passing]\nclock.html\tgone\n' > "$work/gated.txt"
    run --expect "$work/gated.txt" --modern-off
    [ "$rc" -eq 0 ] && ! grep -q '^LEAKED\|^LOST' "$work/out.txt" ||
        fail "with the switch off, every required test absent was not a green run (exit $rc)"
    run --expect "$work/gated.txt" --modern-off --update
    [ "$rc" -eq 2 ] || fail "--update was taken with the switch off (exit $rc)"

    # Only a page run to a verdict may change the expectations: a manifest
    # naming a source that is not there, or one past the size a file may
    # have, is a run that cannot be judged, and --update writes nothing.
    truncate -s $((16 * 1024 * 1024 + 1)) "$work/wpt/selftest/huge.html"
    for page in ghost.html huge.html; do
        printf '{"items":{"testharness":{"selftest":{"%s":["%s",[null,{}]]}}},"url_base":"/","version":9}\n' \
            "$page" "$(printf '0%.0s' {1..40})" > "$work/manifest.json"
        python3 -I tools/wpt/pages.py "$work/manifest.json" selftest > "$work/one.txt"
        cp "$work/lost.txt" "$work/unjudged.txt"
        run --expect "$work/unjudged.txt" --pages "$work/one.txt" --update
        [ "$rc" -eq 2 ] && cmp -s "$work/lost.txt" "$work/unjudged.txt" &&
            grep -q "^UNJUDGED	$page	" "$work/out.txt" ||
            fail "a listed page whose source is $page was not UNJUDGED, exit 2, the file untouched (exit $rc)"
    done

    echo "wpt selftest: the runner's pages ran as tools/wpt/selftest.out says, found either way;" \
        "the expectations' three sections hold, unreadable ones and faulted runs are refused," \
        "and the switch's proof fails on a leak"
    exit 0
fi

dir=${1%/}
shift
if [ ! -d "$checkout/$dir" ]; then
    echo "wpt: no $checkout/$dir; add $dir to tools/wpt_fetch.sh's list and run it" >&2
    exit 2
fi
expect=tools/wpt/expected/$dir.txt
mkdir -p "$(dirname "$expect")"
pages=()
if [ -f "$checkout/MANIFEST.json" ]; then
    python3 -I tools/wpt/pages.py "$checkout/MANIFEST.json" "$dir" > "$work/pages.txt"
    pages=(--pages "$work/pages.txt")
else
    echo "wpt: no $checkout/MANIFEST.json; finding the tests by file name" >&2
fi
"${runner[@]}" --wpt "$checkout" --dir "$dir" --expect "$expect" "${pages[@]}" "$@"
