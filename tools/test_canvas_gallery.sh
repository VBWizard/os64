#!/usr/bin/env bash
# Export retained bitmaps before/after the real gallery's button handlers.
set -euo pipefail
cd "$(dirname "$0")/.."
work=$(realpath -m "${1:?usage: test_canvas_gallery.sh OUTPUT}")
mkdir -p "$work"
. tools/yonder_host_build.sh
yonder_host_build "$work" tools/test_canvas_gallery.c "$work/canvas-gallery" -Wno-unused-function
ASAN_OPTIONS=${ASAN_OPTIONS:-detect_leaks=1} "$work/canvas-gallery" "${2:-tools/pages/canvas-paths.html}" "$work"
