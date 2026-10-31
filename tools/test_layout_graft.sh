#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
work=$(realpath -m "${1:?usage: test_layout_graft.sh OUTPUT}")
mkdir -p "$work"
. tools/yonder_host_build.sh
yonder_host_build "$work" tools/test_layout_graft.c "$work/layout-graft" -Wno-unused-function
ASAN_OPTIONS=${ASAN_OPTIONS:-detect_leaks=1} "$work/layout-graft" "$work/report.txt"
python3 tools/test_layout_export.py "$work/report.txt"
