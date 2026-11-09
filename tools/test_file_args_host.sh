#!/bin/bash
set -eu
cd "$(git rev-parse --show-toplevel)"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
for app in grep tail; do
cc -std=gnu11 -g -O1 -Wall -Wextra -Werror -fno-builtin -fno-pie -no-pie \
   -fsanitize=address,undefined -fno-sanitize-recover=all \
   -I userland/libos64/include -I abi/include \
   "tools/test_${app}_args_host.c" userland/libos64/args.c \
   userland/libos64/fmt.c userland/libos64/str.c -o "$work/$app-args"
"$work/$app-args"
done
