#!/bin/bash
# The occlusion geometry (kernel/src/gui/visible.c, no kernel headers) on
# the host, under the sanitizers, against a pixel oracle.
set -eu
cd "$(git rev-parse --show-toplevel)"
work=$(mktemp -d)
cleanup() {
    status=$?
    if [ "$status" -eq 0 ]; then rm -rf "$work"; else echo "visible test artifacts: $work" >&2; fi
}
trap cleanup EXIT
. tools/host_build.sh
host_flags=(-std=c11 -g -O1 -Wall -Wextra -Werror -fsanitize=address,undefined
   -fno-sanitize-recover=all -I kernel/include)
host_sources=(tools/test_visible_host.c kernel/src/gui/visible.c)
host_objects "$work"
cc -fsanitize=address,undefined "${host_objs[@]}" -o "$work/visible_driver"
"$work/visible_driver" "$@"
