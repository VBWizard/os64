#!/bin/bash
# Fuzz libmedia's fragment parser on the host, under ASan and UBSan, guided by
# gcc's -fsanitize-coverage=trace-pc (tools/fuzz_media_fragment_host.c says
# how). The seeds are fragmented MP4 made here by the host's ffmpeg in every
# layout it writes: DASH init and media segments, one fragmented file with
# picture and sound in one moof, separate moofs, CMAF, a fragment a frame,
# an empty moov then fragments, and AAC alone. Nothing from any site.
#
#     tools/fuzz_media_fragment_host.sh [seconds]     (default 600)
#
# A report fails the run and leaves the input that caused it in
# media-fragment-crash.bin in the current directory.

set -eu
seconds=${1:-600}
top=$(cd "$(dirname "$0")/.." && pwd)
crash=$PWD/media-fragment-crash.bin
command -v ffmpeg > /dev/null || { echo "FAIL fuzz_media_fragment_host: needs the host's ffmpeg" >&2; exit 1; }

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

gcc -std=c17 -g -O1 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=undefined \
    -fsanitize-coverage=trace-pc -I "$top/userland/libmedia/include" \
    -c "$top/userland/libmedia/media_fragment.c" -o "$work/media_fragment.o"
gcc -std=c17 -g -O1 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=undefined \
    -I "$top/userland/libmedia/include" \
    "$top/tools/fuzz_media_fragment_host.c" "$work/media_fragment.o" -o "$work/fuzz"

# The seeds: small (176x144, two seconds) so a run is quick.
gen() { ffmpeg -hide_banner -loglevel error -y "$@"; }
src="-f lavfi -i testsrc2=size=176x144:rate=15 -f lavfi -i sine=frequency=440:sample_rate=48000 -t 2"
x264="-c:v libx264 -profile:v main -pix_fmt yuv420p -g 15 -bf 2 -c:a aac -b:a 64k"
mkdir -p "$work/seeds" "$work/dash"
# shellcheck disable=SC2086
gen $src $x264 -f mp4 -movflags frag_keyframe+empty_moov+default_base_moof "$work/seeds/av-dbim.mp4"
# shellcheck disable=SC2086
gen $src $x264 -f mp4 -movflags frag_keyframe+empty_moov "$work/seeds/av-chained.mp4"
# shellcheck disable=SC2086
gen $src $x264 -f mp4 -movflags frag_keyframe+empty_moov+separate_moof "$work/seeds/av-separate.mp4"
# shellcheck disable=SC2086
gen $src $x264 -f mp4 -movflags cmaf "$work/seeds/av-cmaf.mp4"
# shellcheck disable=SC2086
gen $src $x264 -f mp4 -movflags frag_every_frame+empty_moov+default_base_moof "$work/seeds/av-every-frame.mp4"
# shellcheck disable=SC2086
gen $src $x264 -an -f mp4 -movflags frag_keyframe+empty_moov+default_base_moof "$work/seeds/v-only.mp4"
# shellcheck disable=SC2086
gen $src -vn -c:a aac -b:a 64k -f mp4 -movflags frag_keyframe+empty_moov+default_base_moof "$work/seeds/a-only.mp4"
# shellcheck disable=SC2086
(cd "$work/dash" && gen $src $x264 -map 0:v -map 1:a -f dash -seg_duration 1 \
    -init_seg_name 'init-$RepresentationID$.m4s' -media_seg_name 'seg-$RepresentationID$-$Number$.m4s' s.mpd)
for r in 0 1; do
    cat "$work/dash/init-$r.m4s" "$work/dash"/seg-$r-*.m4s > "$work/seeds/dash-$r.mp4"
done

rm -f "$crash"
echo "fuzz_media_fragment_host: $(ls "$work/seeds" | wc -l) seeds, $seconds s" >&2
"$work/fuzz" "$seconds" "$crash" "$work"/seeds/*
