#!/bin/bash
# libfetch's kept connections under four threads (tools/test_fetch_pool_host.c
# says how): once under ASan and UBSan, once under TSan, since the two
# cannot share a build.
set -euo pipefail
cd "$(git rev-parse --show-toplevel)"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
common=(-std=c11 -O1 -g -Wall -Wextra -Werror -fno-sanitize-recover=all -pthread
        -I userland/libfetch/include -I userland/libfetch -I userland/libtls/include
        -I userland/libos64/include -I abi/include
        tools/test_fetch_pool_host.c userland/libfetch/pool.c userland/libos64/str.c)
cc "${common[@]}" -fsanitize=address,undefined -o "$work/pool_asan"
cc "${common[@]}" -fsanitize=thread -o "$work/pool_tsan"
ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=1}" "$work/pool_asan"
# TSan's shadow memory wants the low address layout: a kernel that
# randomizes mappings across more bits stops it with "unexpected memory
# mapping" before main, so it runs with address randomization off.
TSAN_OPTIONS="${TSAN_OPTIONS:-halt_on_error=1}" setarch "$(uname -m)" -R "$work/pool_tsan"
