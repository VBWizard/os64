#!/bin/bash
# libmedia's fed sound placement (userland/libmedia/media_fed_sound.c) on
# the host under ASan and UBSan: tools/test_media_fed_sound_host.c says what
# it proves. The random streams run for the seconds given (default 2).
# Its mutants: tools/test_media_fed_sound_mutants.py.
set -eu
top=$(cd "$(dirname "$0")/.." && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
cc -std=c17 -D_POSIX_C_SOURCE=200809L -g -O1 -Wall -Wextra -Werror -fsanitize=address,undefined \
    -fno-sanitize-recover=undefined -I "$top/userland/libmedia/include" \
    "$top/tools/test_media_fed_sound_host.c" "$top/userland/libmedia/media_fed_sound.c" \
    "$top/userland/libmedia/media_track.c" "$top/userland/libmedia/media_fragment.c" -o "$work/t"
"$work/t" "${1:-2}"
