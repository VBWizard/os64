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

# -fno-builtin and -fno-tree-loop-distribute-patterns for test_garb_host.sh's
# reason: libos64's str.c defines memset, and an optimizer may turn
# os64_memset's loop into a call to it that recurses forever.
cc -std=c11 -g -O1 -Wall -Wextra -Werror -fsanitize=address,undefined \
   -fno-sanitize-recover=all -fno-builtin -fno-tree-loop-distribute-patterns \
   -I userland/libflow/include -I userland/libflow -I userland/apps/yonder -I userland/libpage/include \
   -I userland/libgarb/include -I userland/libfetch/include -I userland/libgzip/include -I userland/libtls/include -I userland \
   -I userland/libhtml/include -I userland/libos64/include -I abi/include -I tools \
   -I userland/libpage/upstream/ryu \
   tools/test_yonder_host.c userland/apps/yonder/paint.c userland/apps/yonder/bar.c userland/apps/yonder/scale.c \
   userland/apps/yonder/mail.c userland/apps/yonder/agent.c \
   userland/libflow/store.c userland/libflow/attrs.c userland/libflow/style.c \
   userland/libflow/dump.c userland/libflow/boxes.c userland/libflow/layout.c \
   userland/libflow/flow.c tools/test_libflow_fonts.c \
   userland/libgarb/tokenize.c userland/libgarb/parse.c userland/libgarb/decode.c \
   userland/libgarb/dump.c userland/libgarb/select.c userland/libgarb/values.c \
   userland/libgarb/props.c userland/libgarb/media.c userland/libgarb/cascade.c \
   userland/libos64/arena.c \
   userland/libos64/text.c userland/libos64/text_cache.c userland/libos64/text_decode.c \
   userland/libos64/text_bitmap.c \
   userland/libpage/core.c userland/libpage/state.c userland/libpage/resolve.c userland/libpage/value.c \
   userland/libpage/number.c userland/libpage/range.c userland/libpage/upstream/ryu/ryu/d2s.c \
   userland/libpage/submit.c userland/libpage/encode.c userland/libpage/refresh.c \
   userland/libpage/activate.c \
   userland/libhtml/core.c userland/libhtml/encoding.c userland/libhtml/tokenizer.c \
   userland/libhtml/tree.c userland/libhtml/dom.c userland/libhtml/fragment.c userland/libhtml/serialize.c \
   userland/libos64/str.c userland/libos64/bidi.c userland/libos64/url.c userland/libos64/fmt.c \
   -lm -o "$work/yonder_driver"

"$work/yonder_driver" "$@"
