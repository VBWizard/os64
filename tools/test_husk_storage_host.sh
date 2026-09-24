#!/bin/bash
set -eu
cd "$(git rev-parse --show-toplevel)"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
cc -std=gnu11 -g -O1 -masm=intel -ffunction-sections -fdata-sections \
   -Wall -Wextra -Werror -Wno-unused-function -fsanitize=address,undefined \
   -I userland/libos64/include -I abi/include -Wl,--gc-sections \
   tools/test_husk_storage_host.c userland/libos64/arena.c \
   userland/libos64/str.c -o "$work/husk-storage"
"$work/husk-storage"
