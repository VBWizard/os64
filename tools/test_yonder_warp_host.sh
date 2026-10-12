#!/usr/bin/env bash
# yonder's warp (userland/apps/yonder/warp.c) on the host, under the
# sanitizers, against references written apart from it
# (tools/test_yonder_warp_host.c; PILE3.md § Transforms, the painter).
set -euo pipefail
cd "$(dirname "$0")/.."
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
cc -std=c11 -g -O1 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all \
   -I abi/include -I userland/libos64/include -I userland/apps/yonder \
   tools/test_yonder_warp_host.c userland/apps/yonder/warp.c -lm -o "$work/warp_driver"
"$work/warp_driver"
