#!/bin/bash
# libmedia's fragment parser on real streams, decoded by the pinned FFmpeg and
# compared with the host's ffmpeg: every picture and sound frame bit for bit,
# and every presentation time (tools/test_media_fragment_decode_host.c says
# what each scenario proves). Runs under ASan.
#
#     tools/test_media_fragment_decode_host.sh <ffmpeg-9.0.2.tar.xz>
#     (or OS64_FFMPEG_ARCHIVE=<archive> tools/test_media_fragment_decode_host.sh)
#
# The archive is the one userland/libav/manifest.json pins, checked by its
# SHA-256. Its host build is shared with tools/test_media_picture_host.sh: the
# same key (the archive, the manifest's arguments, the toolchain) in the same
# cache, so whichever runs first builds it. The streams are made here by the
# host's ffmpeg with libx264, from synthetic sources: nothing ships, and
# nothing is from any site.

set -eu
cd "$(git rev-parse --show-toplevel)"
ROOT=$PWD

archive=${1:-${OS64_FFMPEG_ARCHIVE:-}}
if [ -z "$archive" ] || [ ! -f "$archive" ]; then
    echo "FAIL test_media_fragment_decode_host: give the pinned FFmpeg archive (userland/libav/manifest.json's archive_url)" \
         "as the first argument or OS64_FFMPEG_ARCHIVE" >&2
    exit 1
fi
for tool in ffmpeg gcc make python3; do
    command -v "$tool" > /dev/null || { echo "FAIL test_media_fragment_decode_host: needs $tool" >&2; exit 1; }
done

read -r want_sha prefix args < <(python3 - "$archive" <<'EOF'
import hashlib, json, sys
m = json.load(open('userland/libav/manifest.json'))
got = hashlib.sha256(open(sys.argv[1], 'rb').read()).hexdigest()
if got != m['archive_sha256']:
    sys.exit(f'FAIL test_media_fragment_decode_host: {sys.argv[1]} is not the pinned archive')
os64_only = {'--enable-cross-compile', '--arch=x86_64', '--target-os=none', '--cc=gcc'}
print(m['archive_sha256'], m['archive_prefix'].rstrip('/'),
      ' '.join(a for a in m['configure_args'] if a not in os64_only))
EOF
)

# test_media_picture_host.sh's cache rule, so the two share one build.
toolchain=gcc-asan
cache=${XDG_CACHE_HOME:-$HOME/.cache}/os64-libav-host
key=$(printf '%s %s %s\n' "$want_sha" "$args" "$toolchain" | sha256sum | cut -c1-16)
dir=$cache/$key
if [ ! -f "$dir/done" ]; then
    echo "test_media_fragment_decode_host: building FFmpeg for the host ($toolchain) in $dir" >&2
    mkdir -p "$dir/build"
    if [ ! -d "$dir/src" ]; then
        mkdir -p "$dir/src"
        tar -xf "$archive" -C "$dir/src"
    fi
    # shellcheck disable=SC2086  # the arguments are words
    (cd "$dir/build" && "$dir/src/$prefix/configure" $args --toolchain="$toolchain" --prefix="$dir/install" \
         > configure.log 2>&1 && make -j"$(nproc)" > make.log 2>&1 && make install > install.log 2>&1) || {
        echo "FAIL test_media_fragment_decode_host: FFmpeg did not build; see $dir/build/*.log" >&2
        exit 1
    }
    touch "$dir/done"
fi
ff=$dir/install

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

# The streams: a picture in two qualities and a sound, each a DASH
# representation (an init segment, then two-second moof+mdat segments in files
# of their own), closed GOPs with B-frames so presentation and decode times
# differ and the init carries an edit list.
media=$work/media
mkdir -p "$media"
(cd "$media" && ffmpeg -hide_banner -loglevel error -y \
    -f lavfi -i testsrc2=size=1280x720:rate=30 -f lavfi -i sine=frequency=440:sample_rate=48000 -t 12 \
    -filter_complex "[0:v]split=2[a][b];[b]scale=640:360[c]" -map "[a]" -map "[c]" -map 1:a \
    -c:v libx264 -profile:v main -pix_fmt yuv420p -g 60 -keyint_min 60 -sc_threshold 0 -bf 2 \
    -x264-params open-gop=0 -b:v:0 2500k -b:v:1 700k -c:a aac -b:a 128k -ac 2 \
    -f dash -seg_duration 2 -use_template 1 -use_timeline 0 \
    -init_seg_name 'init-$RepresentationID$.m4s' -media_seg_name 'seg-$RepresentationID$-$Number$.m4s' stream.mpd)
for r in 0 1 2; do
    # shellcheck disable=SC2046  # the segment names are words
    cat "$media/init-$r.m4s" $(ls "$media"/seg-$r-*.m4s | sort -t- -k3 -n) > "$media/whole-$r.mp4"
done
ffmpeg -hide_banner -loglevel error -y -i "$media/whole-0.mp4" -map 0:v -f framemd5 -c:v rawvideo -pix_fmt yuv420p "$media/ref-0.md5"
ffmpeg -hide_banner -loglevel error -y -i "$media/whole-1.mp4" -map 0:v -f framemd5 -c:v rawvideo -pix_fmt yuv420p "$media/ref-1.md5"
ffmpeg -hide_banner -loglevel error -y -i "$media/whole-2.mp4" -map 0:a -f framemd5 -c:a pcm_f32le "$media/ref-2.md5"

gcc -std=c17 -g -O1 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=undefined \
    -I "$ROOT/userland/libmedia/include" -I "$ff/include" \
    "$ROOT/tools/test_media_fragment_decode_host.c" "$ROOT/userland/libmedia/media_fragment.c" \
    "$ff/lib/libavcodec.a" "$ff/lib/libavutil.a" -lm -lpthread -o "$work/decode"

"$work/decode" "$media"
