#!/usr/bin/env bash
# yonder's kept drawings' store (userland/apps/yonder/keep.c) on the host,
# under the sanitizers (tools/test_yonder_keep_host.c; PILE3.md §
# Transforms, the painter: Kept drawings).
set -euo pipefail
cd "$(dirname "$0")/.."
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
cc -std=c11 -g -O1 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all \
   -I abi/include -I userland/libos64/include -I userland/apps/yonder -I userland/libflow/include \
   -I userland/libflow -I userland/libdraw2d/include -I userland/libhtml/include \
   -I userland/libgarb/include -I userland/libpage/include -I userland \
   tools/test_yonder_keep_host.c userland/apps/yonder/keep.c -o "$work/keep_driver"
"$work/keep_driver"
