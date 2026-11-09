#!/bin/bash
# libway's origins and CORS (way/origin.h) on the host, under the
# sanitizers: a case per sentence of the Fetch Standard origin.c applies,
# the Fetch Metadata lines, then every allocation failed in turn. tools/test_way_origin_host.c says
# what is proved.
set -eu
cd "$(git rev-parse --show-toplevel)"

work=$(mktemp -d)
cleanup() {
    status=$?
    if [ "$status" -eq 0 ]; then rm -rf "$work"; else echo "origin test artifacts: $work" >&2; fi
}
trap cleanup EXIT

. tools/host_build.sh
host_flags=(-std=c11 -g -O1 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all -DWAY_ORIGIN_STEPS
            -I userland/libway/include -I userland/libos64/include -I abi/include)
host_sources=(tools/test_way_origin_host.c userland/libway/origin.c
              userland/libos64/url.c userland/libos64/str.c userland/libos64/fmt.c)
host_objects "$work"
cc -fsanitize=address,undefined "${host_objs[@]}" -o "$work/origin_driver"

"$work/origin_driver"
