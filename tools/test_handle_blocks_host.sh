#!/bin/bash
# Drive the handle table's arithmetic and counts on the host, under ASan and
# UBSan (kernel/src/handle_blocks.c: where a number lives, the room after k
# blocks, the counts; tools/test_handle_blocks_host.c says what it checks).

set -eu
cd "$(git rev-parse --show-toplevel)"

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

cc -std=c11 -g -O1 -Wall -Wextra -Werror -fsanitize=address,undefined \
   -fno-sanitize-recover=undefined -pthread \
   -I kernel/include -I abi/include \
   kernel/src/handle_blocks.c tools/test_handle_blocks_host.c -o "$work/test_handle_blocks"

"$work/test_handle_blocks"
