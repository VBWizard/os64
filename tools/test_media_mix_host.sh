#!/bin/bash
# Run libmedia's mixer (userland/libmedia/media_mix.c: the voices' clocks
# and the mix) on the host under ASan and UBSan, against the model in
# tools/test_media_mix_host.c.

set -eu
cd "$(git rev-parse --show-toplevel)"

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

cc -std=c17 -g -O1 -Wall -Wextra -Werror \
   -fsanitize=address,undefined -fno-sanitize-recover=undefined \
   -I userland/libmedia/include \
   tools/test_media_mix_host.c userland/libmedia/media_mix.c -o "$work/test_media_mix"

"$work/test_media_mix"
