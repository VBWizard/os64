#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
input=$(realpath "${1:?usage: test_layout_stream.sh ARTICLE OUTPUT}")
work=$(realpath -m "${2:?usage: test_layout_stream.sh ARTICLE OUTPUT}")
mkdir -p "$work"
. tools/yonder_host_build.sh
yonder_host_build "$work" tools/test_layout_stream.c "$work/layout-stream" -Wno-unused-function
ASAN_OPTIONS=${ASAN_OPTIONS:-detect_leaks=1} "$work/layout-stream" "$input" "$work/report.txt"
python3 tools/test_layout_export.py "$work/report.txt"
