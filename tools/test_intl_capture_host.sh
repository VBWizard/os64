#!/usr/bin/env bash
# test_intl_capture_host.sh — libjs's Intl against a browser's capture, byte
# for byte (INTL.md § 8; tools/intl_capture_check.py says what is compared).
#
#   tools/test_intl_capture_host.sh                    against Chrome's, in the tree
#   tools/test_intl_capture_host.sh <capture.json[.gz]> [--show N]
#
# A capture is made by tools/pages/intl-capture.html in the browser, served
# and saved by tools/intl_capture_server.py. tools/intl_captures/ keeps the
# ones libjs is held to: Chrome's, the ruled oracle (INTL.md § 9.1). The
# runner is tools/test262/build.sh's.
set -euo pipefail
cd "$(git rev-parse --show-toplevel)"
if [ $# -lt 1 ]; then
    set -- tools/intl_captures/chrome-156.json.gz
fi
work=$(mktemp -d -t os64-intl-capture-build.XXXXXX)
trap 'rm -rf "$work"' EXIT
. tools/test262/build.sh
libjs_runner "$work" || exit 2
python3 tools/intl_capture_check.py --runner "$work/runner" "$@"
