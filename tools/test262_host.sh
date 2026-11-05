#!/usr/bin/env bash
# test262_host.sh — test262 through the real libjs, held to its expectation
# files (INTL.md § 8; tools/test262/run.py says what is run and what the
# gate is).
#
#   tools/test262_host.sh                        every directory with expectations
#   tools/test262_host.sh intl402/DateTimeFormat one directory
#   tools/test262_host.sh --update [dir ...]     rewrite [excluded] from this run
#   tools/test262_host.sh --only <substring> dir only the tests whose path holds it
#
# The suite comes from tools/test262_fetch.sh, at its pin, in .vendor/test262
# (TEST262_DIR elsewhere). The runner is tools/test262/build.sh's.
set -euo pipefail
cd "$(git rev-parse --show-toplevel)"
work=$(mktemp -d -t os64-test262-build.XXXXXX)
trap 'rm -rf "$work"' EXIT
. tools/test262/build.sh
libjs_runner "$work" || exit 2
python3 tools/test262/run.py --runner "$work/runner" "$@"
