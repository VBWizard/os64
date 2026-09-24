#!/bin/bash
set -eu
cd "$(git rev-parse --show-toplevel)"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
cc -std=c11 -g -O1 -Wall -Wextra -Werror -fsanitize=address,undefined \
   -I userland/libos64/include -I abi/include \
   tools/test_killall_host.c userland/libos64/args.c \
   userland/libos64/str.c userland/libos64/fmt.c -o "$work/test_killall"
"$work/test_killall"
