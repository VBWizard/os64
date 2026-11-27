#!/bin/bash
# Run libmedia's show-thread core (userland/libmedia/media_show.c) on the
# host under ASan and UBSan, against tools/test_media_show_host.c. Its
# mutants: tools/test_media_show_mutants.py.

set -eu
cd "$(git rev-parse --show-toplevel)"

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

cc -std=c17 -g -O1 -Wall -Wextra -Werror \
   -fsanitize=address,undefined -fno-sanitize-recover=undefined \
   -I userland/libmedia/include \
   tools/test_media_show_host.c userland/libmedia/media_show.c -o "$work/test_media_show"

"$work/test_media_show" "${1:-20000}"
