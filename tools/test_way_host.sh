#!/bin/bash
# libway's pure half on the host, under the sanitizers: real pages through
# libhtml and libpage, then every judgement a face acts on
# (docs/completed/06-navigator.md). The I/O half needs a network; the guest
# proves it.
set -eu
cd "$(git rev-parse --show-toplevel)"

work=$(mktemp -d)
cleanup() {
    status=$?
    if [ "$status" -eq 0 ]; then rm -rf "$work"; else echo "libway test artifacts: $work" >&2; fi
}
trap cleanup EXIT

cc -std=c11 -g -O1 -Wall -Wextra -Werror -fsanitize=address,undefined \
   -fno-sanitize-recover=all \
   -I userland/libway/include -I userland/libway -I userland/libfetch/include \
   -I userland/libgzip/include -I userland/libtls/include \
   -I userland/libhtml/include -I userland/libos64/include -I userland \
   -I userland/libpage/include -I userland/libpage/upstream/ryu -I abi/include \
   tools/test_way_host.c userland/libway/session.c userland/libway/jar.c \
   userland/libpage/core.c userland/libpage/resolve.c userland/libpage/value.c \
   userland/libpage/number.c userland/libpage/range.c userland/libpage/upstream/ryu/ryu/d2s.c \
   userland/libpage/submit.c userland/libpage/encode.c userland/libpage/refresh.c \
   userland/libpage/activate.c \
   userland/libhtml/core.c userland/libhtml/encoding.c \
   userland/libhtml/tokenizer.c userland/libhtml/tree.c \
   userland/libos64/str.c userland/libos64/bidi.c userland/libos64/url.c userland/libos64/fmt.c \
   -o "$work/way_driver"

"$work/way_driver"
