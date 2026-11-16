#!/bin/bash
# Render an arc's CSV and open the generated page in Firefox.
# Usage: tools/tracker.sh iframe [worktree]
# TRACKER_NO_OPEN=1 renders without launching a browser.
set -eu
if [ "$#" -lt 1 ] || [ "$#" -gt 2 ]; then
    echo "usage: $0 <arc> [worktree]" >&2
    exit 2
fi
arc="$1"
case "$arc" in
    ''|*[!a-zA-Z0-9_-]*) echo "invalid arc name: $arc" >&2; exit 2 ;;
esac
arc="${arc,,}"
tool_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
tree="${2:-$tool_root}"
tool="$tool_root/tools/tracker.py"
csv="$tree/docs/design/pending/${arc^^}_TRACKER.csv"
out="${TRACKER_OUTPUT_DIR:-$HOME/${arc}_tracker}/index.html"
if [ ! -f "$csv" ]; then
    echo "no tracker in $tree; pass a worktree containing ${arc^^}_TRACKER.csv" >&2
    exit 1
fi
mkdir -p -- "$(dirname -- "$out")"
title="${arc^} Tracker"
if [ "$arc" = http2 ]; then title='HTTP/2 Tracker'; fi
python3 -I "$tool" --csv "$csv" --title "$title" --html "$out"
branch=$(git -C "$tree" rev-parse --abbrev-ref HEAD 2>/dev/null || echo '?')
echo "rendered from $tree ($branch) -> $out"
if [ "${TRACKER_NO_OPEN:-0}" != 1 ]; then
    uri=$(python3 -I -c 'import pathlib, sys; print(pathlib.Path(sys.argv[1]).resolve().as_uri())' "$out")
    # Detach Firefox so closing the terminal does not close the board.
    setsid firefox "$uri" >/dev/null 2>&1 < /dev/null &
fi
