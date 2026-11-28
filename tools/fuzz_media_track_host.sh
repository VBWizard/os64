#!/bin/bash
# Fuzz libmedia's track buffer on the host, under ASan and UBSan, guided by
# gcc's -fsanitize-coverage=trace-pc: an input is an operation stream
# (tools/fuzz_media_track_host.c says what each byte means and what is
# checked after every operation). It seeds itself.
#
#     tools/fuzz_media_track_host.sh [seconds]     (default 600)
#
# A report fails the run and leaves the input that caused it in
# media-track-crash.bin in the current directory.
set -eu
seconds=${1:-600}
top=$(cd "$(dirname "$0")/.." && pwd)
crash=$PWD/media-track-crash.bin
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
flags="-std=c17 -g -O1 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=undefined"
# shellcheck disable=SC2086
gcc $flags -fsanitize-coverage=trace-pc -I "$top/userland/libmedia/include" \
    -c "$top/userland/libmedia/media_track.c" -o "$work/media_track.o"
# shellcheck disable=SC2086
gcc $flags -I "$top/userland/libmedia/include" \
    "$top/tools/fuzz_media_track_host.c" "$work/media_track.o" "$top/userland/libmedia/media_fragment.c" -o "$work/fuzz"
rm -f "$crash"
"$work/fuzz" "$seconds" "$crash"
