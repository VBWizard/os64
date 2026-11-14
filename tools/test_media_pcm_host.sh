#!/bin/bash
# Run libmedia's pure core (userland/libmedia/media_pcm.c: any decoded
# sound to 16-bit stereo) on the host under ASan and UBSan, against the
# model in tools/test_media_pcm_host.c.

set -eu
cd "$(git rev-parse --show-toplevel)"

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

cc -std=c17 -g -O1 -Wall -Wextra -Werror \
   -fsanitize=address,undefined -fno-sanitize-recover=undefined \
   -I userland/libmedia \
   tools/test_media_pcm_host.c userland/libmedia/media_pcm.c -lm -o "$work/test_media_pcm"

"$work/test_media_pcm"
