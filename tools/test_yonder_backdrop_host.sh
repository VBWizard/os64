#!/usr/bin/env bash
# yonder's moving-background layer (userland/apps/yonder/backdrop.c) on the
# host, under the sanitizers, against references written apart from it
# (tools/test_yonder_backdrop_host.c; BACKDROP_LAYER.md § Proof).
set -euo pipefail
cd "$(dirname "$0")/.."
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
cc -std=c11 -g -O1 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all \
   -I abi/include -I userland/libos64/include -I userland/apps/yonder \
   tools/test_yonder_backdrop_host.c userland/apps/yonder/backdrop.c shared/damage.c \
   -o "$work/backdrop_driver"
"$work/backdrop_driver"
