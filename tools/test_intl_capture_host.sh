#!/usr/bin/env bash
# test_intl_capture_host.sh — libjs's Intl against a browser's capture, byte
# for byte (INTL.md § 8; tools/intl_capture_check.py says what is compared).
#
#   tools/test_intl_capture_host.sh <capture.json> [--show N]
#
# A capture is made by tools/pages/intl-capture.html in the browser, served
# and saved by tools/intl_capture_server.py. The runner is
# tools/test262/build.sh's.
set -euo pipefail
cd "$(git rev-parse --show-toplevel)"
if [ $# -lt 1 ]; then
    echo "usage: tools/test_intl_capture_host.sh <capture.json> [--show N]" >&2
    exit 2
fi
work=$(mktemp -d -t os64-intl-capture-build.XXXXXX)
trap 'rm -rf "$work"' EXIT
. tools/test262/build.sh
libjs_runner "$work" || exit 2
python3 tools/intl_capture_check.py --runner "$work/runner" "$@"
