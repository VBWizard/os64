#!/bin/bash
# HTTP/2's frame layer and HPACK (userland/libfetch/h2_frame.c, h2_hpack.c)
# against RFC 7541's worked examples and RFC 9113's frame rules, under ASan
# and UBSan. tools/test_h2_host.c says what the cases are.
set -euo pipefail
cd "$(git rev-parse --show-toplevel)"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
# h2_hpack.c is included by the test, which reaches its static functions.
cc -std=c11 -O1 -g -Wall -Wextra -Werror -fno-sanitize-recover=all -fsanitize=address,undefined \
   -I userland/libfetch -I userland/libos64/include -I abi/include \
   tools/test_h2_host.c userland/libfetch/h2_frame.c userland/libos64/str.c -o "$work/h2"
ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=1}" "$work/h2"
