#!/bin/bash
# yonder's painter on the host, under the sanitizers: a page in, drawing out.
#
# The expected dumps in tools/test_yonder_cases.inc are computed BY HAND from
# the layout and the drawing rules, never captured from the painter (YONDER.md), so a
# case that passes says the painter agrees with the rule, not with itself.
set -eu
cd "$(git rev-parse --show-toplevel)"

work=$(mktemp -d)
cleanup() {
    status=$?
    if [ "$status" -eq 0 ]; then rm -rf "$work"; else echo "yonder test artifacts: $work" >&2; fi
}
trap cleanup EXIT

cc -std=c11 -g -O1 -Wall -Wextra -Werror -fsanitize=address,undefined \
   -fno-sanitize-recover=all \
   -I userland/libflow/include -I userland/libflow -I userland/apps/yonder -I userland/libpage/include \
   -I userland/libhtml/include -I userland/libos64/include -I abi/include -I tools \
   -I userland/libpage/upstream/ryu \
   tools/test_yonder_host.c userland/apps/yonder/paint.c userland/apps/yonder/bar.c userland/apps/yonder/scale.c \
   userland/apps/yonder/mail.c \
   userland/libflow/store.c userland/libflow/attrs.c userland/libflow/style.c \
   userland/libflow/dump.c userland/libflow/boxes.c userland/libflow/layout.c \
   userland/libflow/flow.c tools/test_libflow_fonts.c \
   userland/libos64/text.c userland/libos64/text_cache.c userland/libos64/text_decode.c \
   userland/libos64/text_bitmap.c \
   userland/libpage/core.c userland/libpage/resolve.c userland/libpage/value.c \
   userland/libpage/number.c userland/libpage/range.c userland/libpage/upstream/ryu/ryu/d2s.c \
   userland/libpage/submit.c userland/libpage/encode.c userland/libpage/refresh.c \
   userland/libpage/activate.c \
   userland/libhtml/core.c userland/libhtml/encoding.c userland/libhtml/tokenizer.c \
   userland/libhtml/tree.c \
   userland/libos64/str.c userland/libos64/bidi.c userland/libos64/url.c userland/libos64/fmt.c \
   -o "$work/yonder_driver"

"$work/yonder_driver" "$@"
