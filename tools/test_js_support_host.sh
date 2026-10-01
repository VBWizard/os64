#!/bin/bash
set -eu
cd "$(git rev-parse --show-toplevel)"
js_support_work=$(mktemp -d)
trap 'rm -rf "$js_support_work"' EXIT
cc -std=c11 -g -O1 -Wall -Wextra -Werror -fno-builtin \
    -fsanitize=address,undefined -I userland/libos64/include -I abi/include \
    tools/test_js_support_host.c userland/libos64/str.c -o "$js_support_work/strings"
"$js_support_work/strings"
cc -std=c11 -g -O1 -Wall -Wextra -Werror -fno-builtin -masm=intel \
    -I userland/libos64/include -I abi/include \
    tools/test_heap_host.c userland/libos64/heap.c userland/libos64/fmt.c \
    userland/libos64/str.c -o "$js_support_work/heap"
"$js_support_work/heap"
