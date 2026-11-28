#!/bin/bash
# A refused thread start in yonder's media host, on the host, under ASan and
# UBSan (apps/yonder/media.c with its threads, player and fetch stubbed:
# tools/test_yonder_media_threads_host.c says what it proves).
#
#   tools/test_yonder_media_threads_host.sh

set -eu
cd "$(git rev-parse --show-toplevel)"

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

cc -std=gnu11 -g -O1 -Wall -Wextra -Werror -fsanitize=address,undefined \
   -fno-sanitize-recover=undefined -fno-builtin \
   -I abi/include -I userland/libos64/include -I userland/libmedia/include \
   -I userland/libdom/include -I userland/libhtml/include -I userland/libpage/include \
   -I userland/libflow/include -I userland/libgarb/include -I userland/libway/include \
   -I userland/libfetch/include -I userland/libimage/include -I userland/libtls/include \
   -I userland/libtls/upstream/inc -I userland/libjs/include -I userland/libjs/port \
   -I userland/libcanvas/include -I userland/libdraw2d/include -I userland/libpng/include \
   -I userland/libxml/include -I userland/libsvg/include -I userland/libmath/include \
   -I userland/libwebp/include -I userland/libgzip/include -I userland/apps/yonder \
   -isystem userland/obj/js/upstream \
   tools/test_yonder_media_threads_host.c userland/apps/yonder/media_bytes.c \
   -lm -o "$work/test_yonder_media_threads"

"$work/test_yonder_media_threads"
