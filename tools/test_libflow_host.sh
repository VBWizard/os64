#!/bin/bash
# libflow on the host, under the sanitizers: markup in, what libflow makes
# of it out.
#
# The expected dumps in tools/test_libflow_*.inc are computed BY HAND from
# the standard, never captured from the engine (LAYOUT.md § Proof), so a
# case that passes says the engine agrees with the rule, not with itself.
set -eu
cd "$(git rev-parse --show-toplevel)"

work=$(mktemp -d)
cleanup() {
    status=$?
    if [ "$status" -eq 0 ]; then rm -rf "$work"; else echo "libflow test artifacts: $work" >&2; fi
}
trap cleanup EXIT

# F_BLOCK_BYTES=1 makes every arena record its own allocation, so the
# sweeps fail each record in turn rather than each 64KB block.
# -fno-builtin and -fno-tree-loop-distribute-patterns for test_garb_host.sh's
# reason: libos64's str.c defines memset, and an optimizer may turn
# os64_memset's loop into a call to it that recurses forever.
cc -std=c11 -g -O1 -Wall -Wextra -Werror -fsanitize=address,undefined \
   -fno-builtin -fno-tree-loop-distribute-patterns \
   -DF_BLOCK_BYTES=1 \
   -fno-sanitize-recover=all \
   -I userland/libflow/include -I userland/libflow -I userland/libpage/include \
   -I userland/libgarb/include -I userland \
   -I userland/libhtml/include -I userland/libos64/include -I abi/include -I tools \
   -I userland/libpage/upstream/ryu \
   tools/test_libflow_host.c \
   userland/libflow/store.c userland/libflow/attrs.c userland/libflow/style.c \
   userland/libflow/dump.c userland/libflow/boxes.c userland/libflow/layout.c \
   userland/libflow/flow.c tools/test_libflow_fonts.c \
   userland/libgarb/tokenize.c userland/libgarb/parse.c userland/libgarb/decode.c \
   userland/libgarb/dump.c userland/libgarb/select.c userland/libgarb/values.c \
   userland/libgarb/props.c userland/libgarb/media.c userland/libgarb/cascade.c \
   userland/libos64/arena.c \
   userland/libos64/text.c userland/libos64/text_cache.c userland/libos64/text_decode.c \
   userland/libos64/text_bitmap.c \
   userland/libpage/core.c userland/libpage/resolve.c userland/libpage/value.c \
   userland/libpage/number.c userland/libpage/range.c userland/libpage/upstream/ryu/ryu/d2s.c \
   userland/libpage/submit.c userland/libpage/encode.c userland/libpage/refresh.c \
   userland/libpage/activate.c \
   userland/libhtml/core.c userland/libhtml/encoding.c userland/libhtml/tokenizer.c \
   userland/libhtml/tree.c \
   userland/libos64/str.c userland/libos64/bidi.c userland/libos64/url.c userland/libos64/fmt.c \
   -o "$work/libflow_driver"

# LIBFLOW_DRIVER=path keeps a copy of the driver there, for the tools that
# run it many times (tools/grid_probe).
if [ -n "${LIBFLOW_DRIVER:-}" ]; then
    cp "$work/libflow_driver" "$LIBFLOW_DRIVER"
fi
"$work/libflow_driver" "$@"
