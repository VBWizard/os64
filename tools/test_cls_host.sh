#!/bin/bash
set -eu
cd "$(git rev-parse --show-toplevel)"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
shopt -s globstar
includes=(-Iabi/include)
for dir in kernel/include/**/; do includes+=(-I"$dir"); done
cc -std=gnu11 -D_POSIX_C_SOURCE=200809L -O0 -g -Wall -Wextra -Werror -masm=intel \
    "${includes[@]}" tools/test_cls_host.c -o "$work/test_cls"
"$work/test_cls"
