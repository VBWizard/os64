#!/bin/bash
# Run libmedia's three cores for a video player (userland/libmedia/
# media_pictures.c, media_clock.c, media_pace.c) on the host under ASan and
# UBSan, against tools/test_media_cores_host.c. Their mutants:
# tools/test_media_cores_mutants.py.

set -eu
cd "$(git rev-parse --show-toplevel)"

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

cc -std=c17 -g -O1 -Wall -Wextra -Werror \
   -fsanitize=address,undefined -fno-sanitize-recover=undefined \
   -I userland/libmedia/include \
   tools/test_media_cores_host.c userland/libmedia/media_pictures.c userland/libmedia/media_clock.c \
   userland/libmedia/media_pace.c -o "$work/test_media_cores"

"$work/test_media_cores"
