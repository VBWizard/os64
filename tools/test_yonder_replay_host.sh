#!/usr/bin/env bash
# The replay corpus on the host (MODERN_WEB.md rule 3; the runner's own
# comment, tools/test_yonder_replay_host.c, says what a replay is).
#
#   tools/test_yonder_replay_host.sh KEPT-DIR OUT-DIR [--seconds N] [--only TEXT]
#       replays every page kept in KEPT-DIR (yonder.conf's keep_pages) and
#       writes its record again into OUT-DIR; compare two such directories
#       with tools/yonder_corpus_diff.py
#   tools/test_yonder_replay_host.sh
#       the proof: the census tools' tests, then the runner's self-test,
#       which keeps the fixture pages as yonder would, replays them, and
#       requires every replayed record to be the page's own, and the corpus
#       diff to find nothing between them; then the same corpus given no
#       time, whose unfinished pages the diff must call broken
set -euo pipefail
cd "$(git rev-parse --show-toplevel)"
work=$(mktemp -d -t os64-yonder-replay.XXXXXX)
cleanup() {
    result=$?
    if [ "$result" -eq 0 ] && [ "${YONDER_REPLAY_KEEP:-0}" = 0 ]; then
        rm -rf "$work"
    else
        echo "Yonder replay artifacts: $work" >&2
    fi
}
trap cleanup EXIT
. tools/host_build.sh
. tools/yonder_host_build.sh
yonder_host_build tools/test_yonder_replay_host.c "$work" "$work/yonder-replay"
if [ "$#" -gt 0 ]; then
    "$work/yonder-replay" "$@"
    exit
fi
TMPDIR="$work" python3 -I tools/test_yonder_tally.py
TMPDIR="$work" python3 -I tools/test_yonder_corpus_diff.py
mkdir "$work/self"
"$work/yonder-replay" --self-test "$work/self"
python3 -I tools/yonder_corpus_diff.py "$work/self/kept" "$work/self/out" | tail -n 1
# The same corpus given no time: the pages that cannot arrive in none say
# so on their records, and the diff calls them broken and fails, never
# reading what they lack as progress. (A page that arrived without the
# time for its later timers is judged, and may read as progress: the two
# replays were not given the same budget.)
"$work/yonder-replay" "$work/self/kept" "$work/no-time" --seconds 0 > "$work/zero.log"
if python3 -I tools/yonder_corpus_diff.py "$work/self/kept" "$work/no-time" > "$work/zero.diff"; then
    echo "replay: a corpus given no time passed the diff" >&2
    cat "$work/zero.diff" >&2
    exit 1
fi
broken=$(sed -n 's/^broken: \([0-9]*\) pages$/\1/p' "$work/zero.diff")
unfinished=$(sed -n 's/.*, unfinished \([0-9]*\).*/\1/p' "$work/zero.log" | tail -n 1)
[ -n "$broken" ] && [ "$broken" = "$unfinished" ] || {
    echo "replay: the pages given no time were not each called broken" >&2
    cat "$work/zero.diff" >&2
    exit 1
}
tail -n 1 "$work/zero.log"
tail -n 1 "$work/zero.diff"
