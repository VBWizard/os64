#!/bin/bash
# Native Canvas paths (tools/test_canvas_paths.c: fills, strokes, clips,
# images, shadows, and what each does to the bitmap's revision) under the
# sanitizers, built by test_svg_core.py's --canvas into a directory of its
# own that goes when it ends.
set -euo pipefail
cd "$(dirname "$0")/.."
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
python3 tools/test_svg_core.py --canvas --output "$work"
