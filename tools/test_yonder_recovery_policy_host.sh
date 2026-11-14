#!/usr/bin/env bash
set -euo pipefail
cd "$(git rev-parse --show-toplevel)"
work=$(mktemp -d -t os64-yonder-recovery.XXXXXX)
cleanup() {
    result=$?
    if [ "$result" -eq 0 ] && [ "${YONDER_RECOVERY_KEEP:-0}" = 0 ]; then rm -rf "$work";
    else echo "Yonder recovery policy artifacts: $work" >&2; fi
}
trap cleanup EXIT
. tools/yonder_host_build.sh
yonder_host_build "$work" tools/test_yonder_recovery_policy_host.c "$work/yonder-recovery" -Wno-unused-function
"$work/yonder-recovery"
