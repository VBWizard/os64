#!/usr/bin/env bash
# test_yonder_script_job_host.sh — a `src` script's worker (yonder's
# script_job.c) on the host under ASan and UBSan, against a fake fetch and a
# fake cache (tools/test_yonder_script_job_host.c says what it holds).
set -euo pipefail
cd "$(git rev-parse --show-toplevel)"
work=$(mktemp -d -t os64-yonder-script-job.XXXXXX)
cleanup() {
    status=$?
    if [ "$status" -eq 0 ]; then rm -rf "$work"; else echo "script job test artifacts: $work" >&2; fi
}
trap cleanup EXIT
. tools/host_build.sh
host_flags=(-std=gnu11 -g -O1 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all
    -Iuserland/apps/yonder -Iuserland/libway/include -Iuserland/libfetch/include -Iuserland/libhtml/include
    -Iuserland/libhtml -Iuserland/libpage/include -Iuserland/libgzip/include -Iuserland/libtls/include
    -Iuserland/libos64/include -Iabi/include)
host_sources=(tools/test_yonder_script_job_host.c userland/apps/yonder/script_job.c userland/apps/yonder/bytecode.c
    userland/apps/yonder/script_decode.c userland/libway/origin.c userland/libos64/str.c userland/libos64/fmt.c
    userland/libos64/url.c)
host_objects "$work"
cc -fsanitize=address,undefined "${host_objs[@]}" -o "$work/script-job"
"$work/script-job"
