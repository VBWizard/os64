#!/bin/bash
# libmedia's track buffer (userland/libmedia/media_track.c) beside a model of
# the spec's coded frame processing, under ASan and UBSan:
# tools/test_media_track_host.c says what it compares. The random scripts run
# for the seconds given (default 2) and say how many operations they did.
#
#     tools/test_media_track_host.sh [seconds]
#
# tools/test_media_track_mutants.py breaks each rule against it (through
# tools/mutants_parallel.py); tools/fuzz_media_track_host.sh drives the buffer
# with operation streams made of fuzz bytes.
set -eu
top=$(cd "$(dirname "$0")/.." && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
cc -std=c17 -D_POSIX_C_SOURCE=200809L -g -O1 -Wall -Wextra -Werror -fsanitize=address,undefined \
    -fno-sanitize-recover=undefined -I "$top/userland/libmedia/include" \
    "$top/tools/test_media_track_host.c" "$top/userland/libmedia/media_track.c" \
    "$top/userland/libmedia/media_fragment.c" -o "$work/t"
"$work/t" "${1:-2}"
