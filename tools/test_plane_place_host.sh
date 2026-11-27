#!/bin/bash
# Run yonder's plane decisions and the map of what lies over a picture
# (userland/apps/yonder/plane_place.c) on the host under ASan and UBSan,
# against tools/test_plane_place_host.c.

set -eu
cd "$(git rev-parse --show-toplevel)"

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

cc -std=c17 -g -O1 -Wall -Wextra -Werror \
   -fsanitize=address,undefined -fno-sanitize-recover=undefined \
   -I userland/apps/yonder \
   tools/test_plane_place_host.c userland/apps/yonder/plane_place.c -o "$work/test_plane_place"

"$work/test_plane_place"
