#!/bin/bash
# os64_utf8_decode/encode: the whole code space round-tripped, every
# truncation, and the malformed classics, under the sanitizers.
set -eu
cd "$(git rev-parse --show-toplevel)"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
cc -std=c11 -g -O1 -Wall -Wextra -Werror -fsanitize=address,undefined \
   -I userland/libos64/include -I abi/include \
   tools/test_utf8_host.c userland/libos64/str.c -o "$work/test_utf8"
"$work/test_utf8"
