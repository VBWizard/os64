#!/bin/bash
# One HTTP/2 connection (userland/libfetch/h2_conn.c) against a scripted
# server with real threads (tools/test_h2_conn_host.c says how): once under
# ASan and UBSan, once under TSan, since the two cannot share a build.
set -euo pipefail
cd "$(git rev-parse --show-toplevel)"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
common=(-std=c11 -O1 -g -Wall -Wextra -Werror -fno-sanitize-recover=all -pthread
        -I userland/libfetch -I userland/libtls/include -I userland/libos64/include -I abi/include -I tools
        tools/test_h2_conn_host.c tools/h2_peer.c tools/host_bell.c
        userland/libfetch/h2_conn.c userland/libfetch/h2_frame.c userland/libfetch/h2_hpack.c
        userland/libos64/str.c)
cc "${common[@]}" -fsanitize=address,undefined -o "$work/conn_asan"
cc "${common[@]}" -fsanitize=thread -o "$work/conn_tsan"
ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=1}" "$work/conn_asan"
# TSan's shadow memory wants the low address layout: a kernel that
# randomizes mappings across more bits stops it with "unexpected memory
# mapping" before main, so it runs with address randomization off.
TSAN_OPTIONS="${TSAN_OPTIONS:-halt_on_error=1}" setarch "$(uname -m)" -R "$work/conn_tsan"
