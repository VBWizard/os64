#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
work=$(realpath -m "${1:?usage: test_layout_measure.sh OUTPUT}")
mkdir -p "$work"
. tools/yonder_host_build.sh
yonder_host_build "$work" tools/test_layout_measure.c "$work/layout-measure" -Wno-unused-function
ASAN_OPTIONS=${ASAN_OPTIONS:-detect_leaks=1} "$work/layout-measure" "$work/report.json"
ASAN_OPTIONS=${ASAN_OPTIONS:-detect_leaks=1} "$work/layout-measure" "$work/report-full.json" --full
