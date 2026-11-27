#!/bin/bash
# libmedia's picture side on the host: media.c, media_scale.c and
# media_pcm.c as they are built for os64, over the PINNED FFmpeg built for
# the host from its own archive with userland/libav/manifest.json's own
# configure arguments (threads off, as on os64, so FFmpeg's pools have the
# same empty locks), run twice: under ThreadSanitizer, which is what can
# see a release racing the decoder's pool, and under ASan and UBSan.
# tools/test_media_picture_host.c says what it checks.
#
#     tools/test_media_picture_host.sh <ffmpeg-9.0.2.tar.xz>
#     (or OS64_FFMPEG_ARCHIVE=<archive> tools/test_media_picture_host.sh)
#
# The archive is the one manifest.json pins (archive_url there), checked by
# its SHA-256. Each sanitizer's FFmpeg build is cached by the archive, the
# arguments and the toolchain under ~/.cache/os64-libav-host (a first build
# takes a few minutes; XDG_CACHE_HOME moves it), and anything that cannot
# be built FAILS the run rather than skipping it. The test's media (timeline
# offsets, rotation, codecs libav.so does not build) are made by the host's
# ffmpeg, with libx264, at every run: none of them ships.
#
# ThreadSanitizer's programs, the test and the ones FFmpeg's configure
# compiles to probe the compiler, run under `setarch -R`: with the kernel's
# address randomisation they refuse to start ("unexpected memory mapping").

set -eu
cd "$(git rev-parse --show-toplevel)"
ROOT=$PWD

archive=${1:-${OS64_FFMPEG_ARCHIVE:-}}
if [ -z "$archive" ] || [ ! -f "$archive" ]; then
    echo "FAIL test_media_picture_host: give the pinned FFmpeg archive (userland/libav/manifest.json's archive_url)" \
         "as the first argument or OS64_FFMPEG_ARCHIVE" >&2
    exit 1
fi
for tool in ffmpeg ffprobe gcc make setarch python3; do
    command -v "$tool" > /dev/null || { echo "FAIL test_media_picture_host: needs $tool" >&2; exit 1; }
done

# The pin, and configure's arguments for the host: the manifest's, less the
# four that say os64 (a cross build for no OS with os64's compiler).
read -r want_sha prefix args < <(python3 - "$archive" <<'EOF'
import hashlib, json, sys
m = json.load(open('userland/libav/manifest.json'))
got = hashlib.sha256(open(sys.argv[1], 'rb').read()).hexdigest()
if got != m['archive_sha256']:
    sys.exit(f'FAIL test_media_picture_host: {sys.argv[1]} is not the pinned archive')
os64_only = {'--enable-cross-compile', '--arch=x86_64', '--target-os=none', '--cc=gcc'}
print(m['archive_sha256'], m['archive_prefix'].rstrip('/'),
      ' '.join(a for a in m['configure_args'] if a not in os64_only))
EOF
)

cache=${XDG_CACHE_HOME:-$HOME/.cache}/os64-libav-host
# One FFmpeg build per sanitizer, in $cache/<key>/install.
build_ffmpeg() {
    local toolchain=$1
    local key
    key=$(printf '%s %s %s\n' "$want_sha" "$args" "$toolchain" | sha256sum | cut -c1-16)
    local dir=$cache/$key
    if [ ! -f "$dir/done" ]; then
        echo "test_media_picture_host: building FFmpeg for the host ($toolchain) in $dir" >&2
        mkdir -p "$dir/build"
        if [ ! -d "$dir/src" ]; then
            mkdir -p "$dir/src"
            tar -xf "$archive" -C "$dir/src"
        fi
        # configure runs what it compiles, so it too runs without address
        # randomisation (ThreadSanitizer's programs refuse to start with it).
        # shellcheck disable=SC2086  # the arguments are words
        (cd "$dir/build" && setarch -R "$dir/src/$prefix/configure" $args --toolchain="$toolchain" \
             --prefix="$dir/install" > configure.log 2>&1 && make -j"$(nproc)" > make.log 2>&1 &&
             make install > install.log 2>&1) || {
            echo "FAIL test_media_picture_host: FFmpeg did not build ($toolchain); see $dir/build/*.log" >&2
            exit 1
        }
        touch "$dir/done"
    fi
    echo "$dir/install"
}

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

# The os64 headers the glue includes, as the host's.
mkdir -p "$work/shim/os64"
cat > "$work/shim/os64/mem.h" <<'EOF'
#include <stdlib.h>
#include <string.h>
#define os64_malloc malloc
#define os64_calloc calloc
#define os64_realloc realloc
#define os64_free free
#define os64_malloc_aligned(size, alignment) aligned_alloc((alignment), ((size) + (alignment) - 1) / (alignment) * (alignment))
#define os64_memcpy memcpy
#define os64_memmove memmove
#define os64_memset memset
#define os64_memcmp memcmp
EOF
cat > "$work/shim/os64/str.h" <<'EOF'
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <strings.h>
static inline size_t os64_strlen(const char *s) { return strlen(s); }
// libos64's: copies what fits, always terminated, and answers the SOURCE's length.
static inline size_t os64_strcopy(char *d, size_t cap, const char *s)
{
    size_t n = strlen(s);
    if (cap) { size_t k = n < cap - 1 ? n : cap - 1; memcpy(d, s, k); d[k] = 0; }
    return n;
}
static inline bool os64_streq_nocase(const char *a, const char *b) { return strcasecmp(a, b) == 0; }
EOF
cat > "$work/shim/os64/lock.h" <<'EOF'
// libos64's spin lock, with the host's atomics (which ThreadSanitizer reads
// as the acquire and release they are).
#include <sched.h>
#include <stdint.h>
typedef struct { uint32_t value; } os64_lock_t;
#define OS64_LOCK_INIT {0}
static inline void os64_lock_acquire(os64_lock_t *l)
{
    while (__atomic_exchange_n(&l->value, 1, __ATOMIC_ACQUIRE)) sched_yield();
}
static inline void os64_lock_release(os64_lock_t *l) { __atomic_store_n(&l->value, 0, __ATOMIC_RELEASE); }
EOF
cat > "$work/shim/os64/io.h" <<'EOF'
#include <stdint.h>
#include <unistd.h>
#define OS64_SEEK_SET SEEK_SET
#define OS64_SEEK_END SEEK_END
static inline int64_t os64_read(int32_t h, void *out, uint64_t cap) { return read(h, out, cap); }
static inline int64_t os64_seek(int32_t h, int64_t at, int whence) { return lseek(h, at, whence); }
EOF

# The media: a picture and a sound of 4 s, made here. libx264 at a fixed GOP
# with B pictures, so a skip of the unused ones leaves some out.
gen() {
    ffmpeg -hide_banner -loglevel error -y "$@"
}
V=(-f lavfi -i testsrc=size=96x64:rate=30:duration=4)
A=(-f lavfi -i sine=frequency=440:sample_rate=48000:duration=4)
X264=(-c:v libx264 -preset ultrafast -g 30 -bf 2 -pix_fmt yuv420p)
gen "${V[@]}" "${A[@]}" -map 0:v -map 1:a "${X264[@]}" -c:a aac "$work/av.mp4"
for late in 0.1 2; do
    gen "${V[@]}" -itsoffset "$late" "${A[@]}" -map 0:v -map 1:a "${X264[@]}" -c:a aac "$work/sound_late_$late.mp4"
    # Passthrough, or ffmpeg's constant-rate output fills the late start
    # with copies of the first picture from 0.
    gen -itsoffset "$late" "${V[@]}" "${A[@]}" -map 0:v -map 1:a "${X264[@]}" -fps_mode passthrough -c:a aac \
        "$work/picture_late_$late.mp4"
done
# The matrix is written by a stream copy: an encode would turn the pixels
# themselves and write none.
gen "${V[@]}" -map 0:v "${X264[@]}" "$work/upright.mp4"
gen -display_rotation 90 -i "$work/upright.mp4" -c copy "$work/rotated_ccw90.mp4"
gen "${V[@]}" -vf tinterlace=4 -map 0:v -c:v libx264 -preset ultrafast -flags +ildct+ilme -x264-params tff=1 \
    -pix_fmt yuv420p "$work/interlaced.mp4"
# Dense pictures beside a sound, past libmedia's eight MiB of kept packets
# well before the end, for `accept`.
gen -f lavfi -i testsrc2=size=640x360:rate=30:duration=20 "${A[@]/duration=4/duration=20}" -map 0:v -map 1:a \
    -c:v libx264 -preset ultrafast -g 30 -bf 2 -qp 4 -pix_fmt yuv420p -c:a aac -movflags +faststart "$work/dense.mp4"
# Thirty seconds, for what an open reads.
gen -f lavfi -i testsrc=size=640x360:rate=30:duration=30 -f lavfi -i sine=frequency=440:sample_rate=48000:duration=30 \
    -map 0:v -map 1:a "${X264[@]}" -b:v 2M -c:a aac -movflags +faststart "$work/long.mp4"
# Interlaced at a width that is not a multiple of eight, for the edge.
gen -f lavfi -i testsrc=size=854x480:rate=30:duration=1 -vf tinterlace=4 -c:v libx264 -preset ultrafast \
    -flags +ildct+ilme -x264-params tff=1 -pix_fmt yuv420p "$work/interlaced_854.mp4"
gen "${V[@]}" "${A[@]}" -map 0:v -map 1:a -c:v mpeg4 -c:a aac "$work/picture_mpeg4.mp4"
# A sound with cover art (an attached picture), and the same sound without.
gen -f lavfi -i testsrc=size=64x64:rate=1:duration=1 -frames:v 1 "$work/cover.png"
gen "${A[@]}" -c:a aac "$work/sound.m4a"
gen -i "$work/sound.m4a" -i "$work/cover.png" -map 0:a -map 1:v -c copy -disposition:v attached_pic "$work/sound_cover.m4a"
gen "${V[@]}" "${A[@]}" -map 0:v -map 1:a "${X264[@]}" -c:a ac3 "$work/sound_ac3.mp4"
# What each offset file's streams start at, as the container says, for the
# test to compare with.
for f in "$work"/*_late_*.mp4; do
    ffprobe -v error -show_entries stream=codec_type,start_time -of csv=p=0 "$f" | tr '\n' ' ' > "$f.starts"
done

S=userland/libsound
M=userland/libmedia
run_under() {
    local toolchain=$1 flags=$2 runner=$3
    local ff
    ff=$(build_ffmpeg "$toolchain")
    local out=$work/$toolchain
    mkdir -p "$out"
    local cc=(gcc -std=c17 -g -O1 -Wall -Wextra -Werror $flags -D_GNU_SOURCE)
    "${cc[@]}" -I "$work/shim" -I $M -I $M/include -I $S/include -I abi/include -I "$ff/include" \
        -Wno-unused-function -c $M/media.c -o "$out/media.o"
    "${cc[@]}" -I "$work/shim" -I $M -I $M/include -I "$ff/include" -c $M/media_scale.c -o "$out/media_scale.o"
    "${cc[@]}" -I $M -c $M/media_pcm.c -o "$out/media_pcm.o"
    "${cc[@]}" -I "$work/shim" -I $S/include -DOUTSIDE_SPEEX -DRANDOM_PREFIX=os64_speex -DFLOATING_POINT -DEXPORT= \
        -I $S/upstream/include/speex -c $S/port/resample.c -o "$out/port.o"
    "${cc[@]}" -Wno-sign-compare -DOUTSIDE_SPEEX -DRANDOM_PREFIX=os64_speex -DFLOATING_POINT -DEXPORT= \
        -I $S/upstream/include/speex -I $S/upstream/libspeexdsp -c $S/upstream/libspeexdsp/resample.c -o "$out/speex.o"
    "${cc[@]}" -I $M/include -c tools/test_media_picture_host.c -o "$out/test.o"
    # FFmpeg's allocators wrapped, for the test's count (its step 13).
    local wrap=-Wl,--wrap=av_malloc,--wrap=av_mallocz,--wrap=av_calloc,--wrap=av_realloc,--wrap=av_malloc_array
    gcc $flags "$out"/*.o -L "$ff/lib" -lswscale -lavformat -lavcodec -lavutil -lm -lpthread "$wrap" -o "$out/test"
    echo "test_media_picture_host: under $toolchain" >&2
    $runner "$out/test" "$ROOT/tools/pages/bbb-480p-3s-av.mp4" "$work"
}

# A race report fails the run at once, never as a warning on the way past.
run_under gcc-tsan "-fsanitize=thread -fPIE -pie" "env TSAN_OPTIONS=halt_on_error=1 setarch -R"
run_under gcc-asan "-fsanitize=address,undefined -fno-sanitize-recover=undefined" ""
echo "PASS test_media_picture_host: ThreadSanitizer and ASan/UBSan"
