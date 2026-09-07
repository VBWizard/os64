#!/bin/bash
set -eu
cd "$(git rev-parse --show-toplevel)"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
cc -std=c11 -O2 -g -Wall -Wextra -Werror -masm=intel \
   -fsanitize=address,undefined \
   -I userland/libos64/include -I abi/include \
   tools/test_rngprobe_host.c -o "$work/test_rngprobe"
"$work/test_rngprobe"
if [ "${1:-}" = --live ]; then
    "$work/test_rngprobe" --live
fi
