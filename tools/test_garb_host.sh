#!/bin/bash
# libgarb's parser on the host, under the sanitizers (GARB.md § Slices, G1):
# css-parsing-tests through the driver, then every allocation failed in turn
# over a real 2026 sheet.
set -eu
cd "$(git rev-parse --show-toplevel)"

work=$(mktemp -d)
cleanup() {
    status=$?
    if [ "$status" -eq 0 ]; then rm -rf "$work"; else echo "libgarb test artifacts: $work" >&2; fi
}
trap cleanup EXIT

cc -std=c11 -g -O1 -Wall -Wextra -Werror -fsanitize=address,undefined \
   -fno-sanitize-recover=all \
   -I userland/libgarb/include -I userland/libgarb -I userland/libhtml/include \
   -I userland/libos64/include -I userland -I abi/include \
   tools/test_garb_host.c \
   userland/libgarb/tokenize.c userland/libgarb/parse.c userland/libgarb/decode.c \
   userland/libgarb/dump.c \
   userland/libhtml/core.c userland/libhtml/encoding.c \
   userland/libhtml/tokenizer.c userland/libhtml/tree.c \
   userland/libos64/str.c userland/libos64/fmt.c userland/libos64/arena.c \
   -o "$work/garb_driver"

python3 tools/test_garb_suite.py "$work/garb_driver"
"$work/garb_driver" --sweep tools/garb_corpus/sweep.css
