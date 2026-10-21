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
#       diff to find nothing between them; then a page kept with a picture
#       whose fetch failed, which the diff must not read as progress; the
#       same corpus written under a file-size limit, whose records must each
#       be whole or say they could not be written; and the corpus given no
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
. tools/yonder_host_build.sh
YONDER_HOST_UNITS=tools/test_yonder_replay_fs.c \
    yonder_host_build "$work" tools/test_yonder_replay_host.c "$work/yonder-replay"
if [ "$#" -gt 0 ]; then
    "$work/yonder-replay" "$@"
    exit
fi
TMPDIR="$work" python3 -I tools/test_yonder_records.py
TMPDIR="$work" python3 -I tools/test_yonder_tally.py
TMPDIR="$work" python3 -I tools/test_yonder_corpus_diff.py
mkdir "$work/self"
"$work/yonder-replay" --self-test "$work/self"
python3 -I tools/yonder_corpus_diff.py "$work/self/kept" "$work/self/out" | tail -n 1
# A page kept with a picture that failed, replayed without it: the diff
# compares no picture line on either side, so it is neither progress nor a
# regression.
python3 -I tools/yonder_corpus_diff.py "$work/self/picture" "$work/self/picture-out" > "$work/picture.diff" &&
    grep -q '^progress: 0 pages$' "$work/picture.diff" || {
    echo "replay: the diff read a picture the replay does not reproduce" >&2
    cat "$work/picture.diff" >&2
    exit 1
}
# The same corpus written under a file-size limit, as a quota or a full
# disk would cut it (the signal ignored, so a write past it fails rather
# than ending the run): every record that could not be written whole is
# not there at all, or says it is unjudged, so no page reads as a record
# cut short; the run answers 2; and no temporary file is left.
if ( trap '' XFSZ; exec prlimit --fsize=2048 "$work/yonder-replay" "$work/self/kept" "$work/capped" ) \
        > "$work/capped.log" 2>&1; then
    echo "replay: a corpus written under a size limit passed" >&2
    exit 1
fi
python3 -I - "$work/capped" <<'PY' || { cat "$work/capped.log" >&2; exit 1; }
import importlib.util, pathlib, sys
spec = importlib.util.spec_from_file_location('yonder_records', 'tools/yonder_records.py')
records = importlib.util.module_from_spec(spec)
spec.loader.exec_module(records)
out = pathlib.Path(sys.argv[1])
files = sorted(out.iterdir())
broken = [str(records.read(f)) for f in files if f.suffix == '.txt' and isinstance(records.read(f), records.Unjudged)]
temps = [f.name for f in files if f.suffix != '.txt']
unjudged = sum(1 for f in files if f.suffix == '.txt' and not isinstance(records.read(f), records.Unjudged)
               and records.read(f).state == records.State.UNJUDGED)
if broken or temps or unjudged == 0:
    print(f'replay: under a size limit: {broken} not whole, {temps} left over, {unjudged} said unjudged', file=sys.stderr)
    sys.exit(1)
print(f'size limit: {len(files)} records, every one whole, {unjudged} saying they could not be written')
PY
# The same corpus given no time: the pages that cannot arrive in none say
# so on their records, and the diff calls each broken (whatever else its
# record says) and fails, never reading what they lack as progress. (A page that arrived without the time for its
# later timers is judged, and may read as progress: the two replays were
# not given the same budget.)
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
