#!/bin/bash
# yonder's fetch job (userland/apps/yonder/fetch_job.c) on the host, under
# the sanitizers, against libfetch's contract faked (tools/test_fetch_fake.inc)
# with the real origin.c and cookie jar. tools/test_yonder_fetch_job_host.c
# says what is proved.
set -eu
cd "$(git rev-parse --show-toplevel)"

work=$(mktemp -d)
cleanup() {
    status=$?
    if [ "$status" -eq 0 ]; then rm -rf "$work"; else echo "fetch job test artifacts: $work" >&2; fi
}
trap cleanup EXIT

. tools/host_build.sh
host_flags=(-std=gnu11 -g -O1 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all -Wno-missing-field-initializers
            -I userland/apps/yonder -I userland/libway/include -I userland/libway -I userland/libfetch/include
            -I userland/libtls/include -I userland/libgzip/include -I userland/libos64/include
            -I userland/libhtml/include -I userland/libpage/include -I abi/include -I tools)
host_sources=(tools/test_yonder_fetch_job_host.c userland/apps/yonder/fetch_job.c
              userland/libway/origin.c userland/libway/jar.c userland/libway/cache.c
              userland/libos64/url.c userland/libos64/str.c userland/libos64/fmt.c)
host_objects "$work"
cc -fsanitize=address,undefined "${host_objs[@]}" -o "$work/fetch_job_driver"

"$work/fetch_job_driver"
