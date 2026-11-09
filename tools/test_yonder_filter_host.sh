#!/usr/bin/env bash
# yonder's filtered groups (userland/apps/yonder/filter.c; FILTER.md) on the
# host, under the sanitizers, against WPT's expectations and references
# written apart from it (tools/test_yonder_filter_host.c).
set -euo pipefail
cd "$(dirname "$0")/.."
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
cc -std=c11 -g -O1 -Wall -Wextra -Werror -fsanitize=address,undefined,float-cast-overflow -fno-sanitize-recover=all \
   -I abi/include -I userland/libos64/include -I userland/apps/yonder -I userland/libflow/include \
   -I userland/libgarb/include -I userland/libhtml/include -I userland/libpage/include -I userland \
   tools/test_yonder_filter_host.c userland/apps/yonder/filter.c -lm -o "$work/filter_driver"
"$work/filter_driver"
