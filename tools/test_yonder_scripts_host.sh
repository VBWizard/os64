#!/usr/bin/env bash
set -euo pipefail
cd "$(git rev-parse --show-toplevel)"
work=$(mktemp -d -t os64-yonder-scripts.XXXXXX)
cleanup() {
    result=$?
    if [ "$result" -eq 0 ] && [ "${YONDER_SCRIPTS_KEEP:-0}" = 0 ]; then
        rm -rf "$work"
    else
        echo "Yonder script artifacts: $work" >&2
    fi
}
trap cleanup EXIT
. tools/yonder_host_build.sh
yonder_host_build "$work" tools/test_yonder_scripts_host.c "$work/yonder-scripts"
"$work/yonder-scripts" "$@"
