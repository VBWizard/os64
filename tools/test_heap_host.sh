#!/bin/bash
# The heap's host suite on its own: tools/test_heap_host.c over heap.c, once
# as written, once with HEAPCHECK=1 (verify on every call). Its own door so
# tools/test_heap_mutants.py can name it; test_js_support_host.sh runs the
# same suite among its others.
set -eu
cd "$(git rev-parse --show-toplevel)"
heap_work=$(mktemp -d)
trap 'rm -rf "$heap_work"' EXIT
cc -std=c11 -g -O1 -Wall -Wextra -Werror -fno-builtin -masm=intel \
    -I userland/libos64/include -I abi/include \
    tools/test_heap_host.c userland/libos64/heap.c userland/libos64/fmt.c \
    userland/libos64/str.c -o "$heap_work/heap"
"$heap_work/heap"
HEAPCHECK=1 "$heap_work/heap" > "$heap_work/heapcheck.log" 2>&1 || {
    tail -20 "$heap_work/heapcheck.log"
    exit 1
}
tail -1 "$heap_work/heapcheck.log"
