#!/bin/bash
# Run the arithmetic of a sound's bytes off the network
# (userland/apps/yonder/media_bytes.c: the seek plan, the window's front
# drop, the head's length, ranges and type, the words a failure is
# recorded in) on the host under ASan and UBSan,
# against the model in tools/test_media_bytes_host.c.

set -eu
cd "$(git rev-parse --show-toplevel)"

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

cc -std=c17 -g -O1 -Wall -Wextra -Werror \
   -fsanitize=address,undefined -fno-sanitize-recover=undefined \
   -I userland/apps/yonder \
   tools/test_media_bytes_host.c userland/apps/yonder/media_bytes.c -o "$work/test_media_bytes"

"$work/test_media_bytes"
