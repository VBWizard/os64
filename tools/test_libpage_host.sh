#!/bin/bash
# libpage on the host, under the sanitizers: markup in, the REQUEST out.
#
# The corpus is the durable artefact (LIBPAGE.md § Proof before integration).
# It is pure computation over libhtml's tree — no syscalls, no terminal, no
# wire — so it runs here at full speed with ASan and UBSan watching, and a
# case survives every front end written over the library.
set -eu
cd "$(git rev-parse --show-toplevel)"

work=$(mktemp -d)
cleanup() {
    status=$?
    if [ "$status" -eq 0 ]; then rm -rf "$work"; else echo "libpage test artifacts: $work" >&2; fi
}
trap cleanup EXIT

cc -std=c11 -g -O1 -Wall -Wextra -Werror -fsanitize=address,undefined \
   -fno-sanitize-recover=all \
   -I userland/libpage/include -I userland/libhtml/include -I userland/libos64/include \
   -I abi/include -I tools \
   tools/test_libpage_host.c \
   userland/libpage/core.c userland/libpage/resolve.c userland/libpage/value.c \
   userland/libpage/submit.c userland/libpage/encode.c userland/libpage/refresh.c userland/libpage/activate.c \
   userland/libhtml/core.c userland/libhtml/encoding.c userland/libhtml/tokenizer.c \
   userland/libhtml/tree.c \
   userland/libos64/str.c userland/libos64/bidi.c userland/libos64/url.c userland/libos64/fmt.c \
   -o "$work/libpage_driver"

"$work/libpage_driver" --sweep
