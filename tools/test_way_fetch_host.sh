#!/bin/bash
# way_fetch_whole end to end on the host, under the sanitizers: the real
# libfetch and libway against scripted peers and a real temporary cache
# directory. tools/test_way_fetch_host.c says how.
set -eu
cd "$(git rev-parse --show-toplevel)"

work=$(mktemp -d)
cleanup() {
    status=$?
    if [ "$status" -eq 0 ]; then rm -rf "$work"; else echo "way_fetch test artifacts: $work" >&2; fi
}
trap cleanup EXIT

cc -std=c11 -g -O1 -Wall -Wextra -Werror -fsanitize=address,undefined \
   -fno-sanitize-recover=all \
   -I userland/libway/include -I userland/libway -I userland/libfetch/include -I userland/libfetch \
   -I userland/libgzip/include -I userland/libtls/include \
   -I userland/libhtml/include -I userland/libos64/include -I userland \
   -I userland/libpage/include -I userland/libpage/upstream/ryu -I abi/include \
   tools/test_way_fetch_host.c \
   userland/libway/load.c userland/libway/session.c userland/libway/jar.c userland/libway/cache.c \
   userland/libfetch/fetch.c userland/libfetch/http.c \
   userland/libfetch/transport.c userland/libfetch/proxy.c \
   userland/libgzip/inflate.c userland/libgzip/deflate.c userland/libgzip/gzip.c \
   userland/libpage/core.c userland/libpage/resolve.c userland/libpage/value.c \
   userland/libpage/number.c userland/libpage/range.c userland/libpage/upstream/ryu/ryu/d2s.c \
   userland/libpage/submit.c userland/libpage/encode.c userland/libpage/refresh.c \
   userland/libpage/activate.c \
   userland/libhtml/core.c userland/libhtml/encoding.c \
   userland/libhtml/tokenizer.c userland/libhtml/tree.c \
   userland/libos64/str.c userland/libos64/bidi.c userland/libos64/url.c userland/libos64/fmt.c \
   userland/libos64/crc32.c \
   -o "$work/way_fetch"

# A stack-use-after-return is how a fetch's options left pointing into a
# frame that has returned would show.
ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=1:detect_stack_use_after_return=1}" "$work/way_fetch"
