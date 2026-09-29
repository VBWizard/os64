#!/bin/bash
set -eu
cd "$(git rev-parse --show-toplevel)"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
cc -std=c11 -g -O1 -Wall -Wextra -Werror -fsanitize=address,undefined \
   -fno-sanitize-recover=all -I userland/libos64/include -I abi/include \
   tools/test_mkdir_host.c userland/libos64/args.c userland/libos64/fmt.c \
   -o "$work/mkdir-test"
mkdir "$work/fs"
"$work/mkdir-test" "$work/fs"
