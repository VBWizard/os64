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
#       requires every replayed record to be the page's own; then the same
#       corpus replayed again, which the corpus diff must find no different;
#       an OUT-DIR that is the kept directory by a bind mount, refused; the
#       corpus written under a file-size limit, whose records must each be
#       whole or say they could not be written; and the corpus given no
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
# The runner prints what a page wrote re-spelled: a record whose quoted fact
# holds a terminal's title sequence reaches the screen as \xHH, never raw.
"$work/yonder-replay" "$work/self/escapes" "$work/escapes-shown" > "$work/escapes.log" 2>&1 || true
if LC_ALL=C grep -q $'\x1b\|\x07' "$work/escapes.log" || ! grep -q 'not whole: \\x1B\]0;owned\\x07' "$work/escapes.log"; then
    echo "replay: the runner printed a page's terminal sequence raw" >&2
    cat -v "$work/escapes.log" >&2
    exit 1
fi
# The diff compares a replay with a replay of the same kept corpus, never
# kept records with replayed ones: the same corpus replayed twice must
# differ in nothing.
"$work/yonder-replay" "$work/self/kept" "$work/again" > "$work/again.log"
python3 -I tools/yonder_corpus_diff.py "$work/self/out" "$work/again" > "$work/again.diff" || {
    echo "replay: the same corpus replayed twice did not diff clean" >&2
    cat "$work/again.diff" >&2
    exit 1
}
tail -n 1 "$work/again.diff"
# An OUT-DIR that is the kept directory by another path (a bind mount) is
# the kept directory: refused before anything is written.
mkdir "$work/bound"
if unshare -rm true 2> /dev/null; then
    if unshare -rm sh -c 'mount --bind "$1" "$2" && exec "$3" "$1" "$2" --only fixture' sh \
            "$work/self/kept" "$work/bound" "$work/yonder-replay" > "$work/bound.log" 2>&1 ||
            ! grep -q 'is the kept directory itself' "$work/bound.log"; then
        echo "replay: a bind mount of the kept directory was not refused as OUT-DIR" >&2
        cat "$work/bound.log" >&2
        exit 1
    fi
else
    echo "replay: no user namespace here, so the bind-mount case is skipped"
fi
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
# record says) and fails, never reading what they lack as progress. (A
# page that arrived without the time for its later timers is judged, and
# may read as progress: the two replays were not given the same budget.)
"$work/yonder-replay" "$work/self/kept" "$work/no-time" --seconds 0 > "$work/zero.log"
if python3 -I tools/yonder_corpus_diff.py "$work/self/out" "$work/no-time" > "$work/zero.diff"; then
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
