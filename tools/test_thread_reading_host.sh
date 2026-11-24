#!/bin/bash
# Run yonder's thread reading (userland/apps/yonder/thread_reading.c: where
# the window's thread went since the last reading, by the loop's steps and
# the calls inside them) on the host under ASan and UBSan, against the cases
# in tools/test_thread_reading_host.c.

set -eu
cd "$(git rev-parse --show-toplevel)"

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

cc -std=c17 -g -O1 -Wall -Wextra -Werror \
   -fsanitize=address,undefined -fno-sanitize-recover=undefined \
   -I userland/apps/yonder \
   tools/test_thread_reading_host.c userland/apps/yonder/thread_reading.c userland/apps/yonder/load_timeline.c \
   -o "$work/test_thread_reading"

"$work/test_thread_reading"
