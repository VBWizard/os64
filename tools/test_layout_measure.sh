#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
work=$(realpath -m "${1:?usage: test_layout_measure.sh OUTPUT}")
mkdir -p "$work"
. tools/yonder_host_build.sh
yonder_host_build "$work" tools/test_layout_measure.c "$work/layout-measure" -Wno-unused-function
ASAN_OPTIONS=${ASAN_OPTIONS:-detect_leaks=1} "$work/layout-measure" "$work/report.json"
ASAN_OPTIONS=${ASAN_OPTIONS:-detect_leaks=1} "$work/layout-measure" "$work/report-full.json" --full
python3 tools/layout_measure_local.py "$work/layout-measure-local.html"
ASAN_OPTIONS=${ASAN_OPTIONS:-detect_leaks=1} "$work/layout-measure" "$work/report-local.json" --quick "$work/layout-measure-local.html"
ASAN_OPTIONS=${ASAN_OPTIONS:-detect_leaks=1} "$work/layout-measure" "$work/report-local-full.json" --full "$work/layout-measure-local.html"
python3 tools/test_layout_export.py "$work/report.json" "$work/report-full.json" "$work/report-local.json" "$work/report-local-full.json"
