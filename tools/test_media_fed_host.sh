#!/bin/bash
# libmedia's fed media on the host: media.c § THE FED MEDIA, media_fed.c,
# the placement and the track buffer and parser beneath them, as they are
# built for os64, over the PINNED FFmpeg built for the host (the archive
# userland/libav/manifest.json names, its configure_args less the cross
# compile), under ThreadSanitizer and then ASan/UBSan. The fixture is
# tools/make_dash_fixture.sh's (tools/pages/sync-fed-*), and the reference
# is the host ffmpeg's framecrc of each picture rendition.
# tools/test_media_fed_host.c says what it checks.
#
#     tools/test_media_fed_host.sh <pinned FFmpeg archive>
#
# The FFmpeg builds are cached as tools/test_media_picture_host.sh caches
# them (~/.cache/os64-libav-host), and shared with it.
set -eu
cd "$(git rev-parse --show-toplevel)"
ROOT=$PWD

archive=${1:-${OS64_FFMPEG_ARCHIVE:-}}
if [ -z "$archive" ] || [ ! -f "$archive" ]; then
    echo "FAIL test_media_fed_host: give the pinned FFmpeg archive (userland/libav/manifest.json's archive_url)" \
         "as the first argument or OS64_FFMPEG_ARCHIVE" >&2
    exit 1
fi
for tool in ffmpeg ffprobe gcc make setarch python3; do
    command -v "$tool" > /dev/null || { echo "FAIL test_media_fed_host: needs $tool" >&2; exit 1; }
done

# The pin, and configure's arguments for the host: the manifest's, less the
# four that say os64 (a cross build for no OS with os64's compiler).
read -r want_sha prefix args < <(python3 - "$archive" <<'EOF'
import hashlib, json, sys
m = json.load(open('userland/libav/manifest.json'))
got = hashlib.sha256(open(sys.argv[1], 'rb').read()).hexdigest()
if got != m['archive_sha256']:
    sys.exit(f'FAIL test_media_fed_host: {sys.argv[1]} is not the pinned archive')
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
        echo "test_media_fed_host: building FFmpeg for the host ($toolchain) in $dir" >&2
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
            echo "FAIL test_media_fed_host: FFmpeg did not build ($toolchain); see $dir/build/*.log" >&2
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

cat > "$work/shim/os64/bell.h" <<'EOF'
// A bell, for its type: the test rings none (its fed media's bell is NULL).
#include <stdint.h>
typedef struct { int32_t handle; } os64_bell_t;
static inline int64_t os64_bell_ring(os64_bell_t *b) { (void)b; return 0; }
static inline int64_t os64_bell_wait(os64_bell_t *b, uint64_t ms) { (void)b; (void)ms; return 0; }
EOF

for f in sync-fed-720p sync-fed-480p; do
    ffmpeg -hide_banner -loglevel error -i "tools/pages/$f.mp4" -f framecrc "$work/$f.framecrc"
done

S=userland/libsound
M=userland/libmedia
run_under() {
    local toolchain=$1 flags=$2 runner=$3
    local ff
    ff=$(build_ffmpeg "$toolchain")
    local out=$work/$toolchain
    mkdir -p "$out"
    # shellcheck disable=SC2206  # the flags are words
    local cc=(gcc -std=c17 -g -O1 -Wall -Wextra -Werror $flags -D_GNU_SOURCE)
    for c in media media_scale media_fed media_fed_sound media_track media_fragment; do
        "${cc[@]}" -I "$work/shim" -I $M -I $M/include -I $S/include -I abi/include -I "$ff/include" \
            -Wno-unused-function -c $M/$c.c -o "$out/$c.o"
    done
    "${cc[@]}" -I $M -c $M/media_pcm.c -o "$out/media_pcm.o"
    "${cc[@]}" -I "$work/shim" -I $S/include -DOUTSIDE_SPEEX -DRANDOM_PREFIX=os64_speex -DFLOATING_POINT -DEXPORT= \
        -I $S/upstream/include/speex -c $S/port/resample.c -o "$out/port.o"
    "${cc[@]}" -Wno-sign-compare -DOUTSIDE_SPEEX -DRANDOM_PREFIX=os64_speex -DFLOATING_POINT -DEXPORT= \
        -I $S/upstream/include/speex -I $S/upstream/libspeexdsp -c $S/upstream/libspeexdsp/resample.c -o "$out/speex.o"
    "${cc[@]}" -I "$work/shim" -I $M -I $M/include -I abi/include -I "$ff/include" \
        -c tools/test_media_fed_host.c -o "$out/test.o"
    # shellcheck disable=SC2086
    gcc $flags "$out"/*.o -L "$ff/lib" -lswscale -lavformat -lavcodec -lavutil -lm -lpthread -o "$out/test"
    echo "test_media_fed_host: under $toolchain" >&2
    $runner "$out/test" "$ROOT/tools/pages" "$work"
}

# A race report fails the run at once, never as a warning on the way past.
run_under gcc-tsan "-fsanitize=thread -fPIE -pie" "env TSAN_OPTIONS=halt_on_error=1 setarch -R"
run_under gcc-asan "-fsanitize=address,undefined -fno-sanitize-recover=undefined" ""
echo "PASS test_media_fed_host: ThreadSanitizer and ASan/UBSan"
