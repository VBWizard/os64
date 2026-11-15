# libav: FFmpeg, ported

`libav.so` is FFmpeg's libavutil, libavcodec and libavformat: it reads a
media file (the format, its tags, its timeline) and decodes what is inside.
The interface is FFmpeg's own, unchanged (`avformat_open_input`,
`avcodec_send_packet` and the rest), so FFmpeg's documentation is this
library's. Its first user is `/tests/avtest`; libmedia, which yonder's
`<audio>` and `play` are built on, is the next (AUDIO_YONDER.md).

The code is FFmpeg's, **unmodified**. What is os64's is the configuration,
and the C library FFmpeg's sources expect and libos64 does not provide.

## What was taken

FFmpeg 9.0.2, the release archive `manifest.json` names and pins by
SHA-256. Its signature was checked by hand against FFmpeg's release key
(`FCF986EA15E6E293A5644F10B4322F04D67658D8`) on the day of the import, and
`manifest.json` records that (`archive_signature`); from then on the
SHA-256 is the check, and `tools/import_av.py` makes it.

**What is built is FFmpeg's decision, from one line of arguments.**
`manifest.json`'s `configure_args` is FFmpeg's `configure` with everything
disabled and then, by name:

| Kind | Enabled |
|---|---|
| readers (demuxers) | `wav`, `mp3`, `mov` (MP4, M4A), `aac` (ADTS) |
| decoders | `mp3float`, `aac`, `pcm_s16le`, `pcm_s24le`, `pcm_s32le`, `pcm_u8`, `pcm_f32le`, `pcm_alaw`, `pcm_mulaw` |
| parsers | `mpegaudio`, `aac` |
| protocols | none: bytes arrive through the caller's `AVIOContext`, never FFmpeg's own I/O |

`tools/import_av.py --write <archive>` runs that `configure` on the host
(os64 cannot run it), and writes:

- `port/config/`: `config.h` and the other files `configure` generates.
- `sources.mk`: the translation units FFmpeg's own makefiles build for
  this configuration (its `lib*.a.objs` lists).
- `upstream/`: those sources and every FFmpeg header they include, found by
  the cross compiler's dependency output, plus `LICENSE.md`,
  `COPYING.LGPLv2.1`, `VERSION`, `RELEASE` and `CREDITS`.

`tools/import_av.py --check <archive>` proves all three are what the
archive produces; `tools/test_av_audit.py` proves the tree matches the
manifest on every run.

**`config.h`'s answers about a C library are os64's.** `configure` probes
the HOST's C library, so `manifest.json`'s `config_overrides` replace each
of those answers with what the shims below provide: no `unistd.h`, no
`mmap`, no `posix_memalign`, no `gettimeofday`, and the float maths
libmath does not export (`expf`, `sinf`, `exp2`, `lrintf`…), which
FFmpeg's own `libavutil/libm.h` then supplies from the double functions.
The few it calls with no `HAVE_` to answer (`sqrtf`, `fabsf`, `ldexp`,
the AAC decoder's) are `port/compat/math.h`'s, over libmath's own.
`CC_IDENT` is overridden too, so the file does not record the host's
compiler. An override that matches no line fails the import.

### Adding a format or a decoder

Add it to `configure_args`, run `tools/import_av.py --write <archive>`,
build, run `tools/test_av_audit.py`, and prove it in `/tests/avtest`. The
import adds whatever new files the decoder needs; nothing is listed by
hand.

## The licence

This configuration is **LGPL 2.1 or later** (`config.h`'s
`FFMPEG_LICENSE`). The LGPL lets a program use the library provided the
user can replace the library with their own build of it, and a separate
shared library is how os64 meets that: os64's loader serves the file at
`/lib/libav.so`, and replacing the file replaces it for every program
started after.

- **libav.so is a library of its own, and nothing else may link FFmpeg's
  objects in.** The audit fails if any program or other library in
  `userland/bin` carries FFmpeg's code.
- **`--enable-gpl`, `--enable-nonfree` and `--enable-version3` are never
  passed.** The audit fails on the arguments and on `config.h`
  (`CONFIG_GPL`, `CONFIG_NONFREE`, `CONFIG_VERSION3`).
- **The source travels with it**: `upstream/` is FFmpeg's files as
  released, and `license/ffmpeg-LICENSE`, installed on both images as
  `/etc/licenses/ffmpeg.txt`, carries `LICENSE.md`, the LGPL, and where the
  source is.
- `THIRD_PARTY.md` has the row for the monthly scan.

## The C library (`port/`)

FFmpeg's sources include `<stdio.h>`, `<stdlib.h>`, `<string.h>`,
`<errno.h>`, `<time.h>` and the rest. `port/compat/` answers them, private
to this library, in the shape libwebp's and QuickJS's ports use: a name
libos64 has is mapped onto libos64 (`memcpy`, `strlen`, `malloc`…), and
the rest is os64's, in three files:

- **`port/av_libc.c`** is pure (no os64 header): `strncmp`, `strstr`,
  `strspn`, `strcspn`, `strcpy`, the four integer `strto*`, `frexp`,
  `scalbn`, `qsort` (a heapsort: no recursion, no allocation), `bsearch`,
  UTC `gmtime`, `mktime` and `strftime`, and how much of a string
  `strtod` takes (C17's grammar for a floating constant).
- **`port/av_format.c`** is pure too: `snprintf` and `vsnprintf`. It is
  not libos64's because a formatter that does not know a conversion cannot
  step over its argument, and every later argument is then read from the
  wrong place (FFmpeg uses `%zu`; a `%s` after it would print from a size).
  It walks C's whole grammar and takes each argument at its own type.
  Integers, characters, strings and pointers print exactly as glibc
  prints them; a double's digits come from 80-bit arithmetic, exact for
  the 17 a double carries and possibly different from glibc past them.
- `tools/test_av_libc_host.sh` compares all of both with glibc (C17
  rules), under ASan and UBSan.
- **`port/av_port.c`** is the doors. FFmpeg never opens a file here, so
  `open` and `fopen` answer as for a missing file and FFmpeg takes the path
  it already has for that. `stderr` takes the default log callback's words
  and discards them: a caller that wants them installs its own
  (`av_log_set_callback`). `strtod` converts exactly the span `av_libc.c`
  measured, with FFmpeg's own scanf (`av_sscanf`, musl's), which plain
  `sscanf` calls are routed to as well. `clock` is `micros()`.
  `abort` (FFmpeg's `av_assert0`) ends the program with `0x41564641`
  ("AVFA"), QuickJS's port's shape.

What a caller should know the shims do NOT do:

- **No environment reaches the library**: `getenv` answers nothing, and
  local time is UTC.
- **FFmpeg's clock functions have no clock behind them** (the
  configuration says there is no `gettimeofday`, `clock_gettime`,
  `usleep` or `nanosleep`): `av_gettime()` answers -1,
  `av_gettime_relative()` a constant, and `av_usleep()` `ENOSYS`. A caller
  keeps time with `micros()` (`os64/proc.h`).
- **`errno` is one word for every thread**, written by `strto*` on
  overflow, by the shut file doors, and by FFmpeg's own scanf on a failed
  match or a range error. FFmpeg reads it right after such a call.

## Switched off, and how they come on

The daily-driver goal (video) needs each of these, so nothing here assumes
they stay off.

- **Threads** (`--disable-pthreads`). Sound decodes far faster than it
  plays on one thread. **A build without threads takes no locks of its
  own**, and FFmpeg's configure warns that such a build must not be used
  from several threads. What it shares between callers is set up on first
  use (`ff_thread_once`: the registries, a decoder's tables, which run
  when a format is probed or a decoder opened) and its default log
  callback, which keeps state. So a program that uses libav from more than
  one thread does every open and close under one lock of its own and
  installs a log callback that is safe from any thread. libmedia keeps a
  narrower form (its lock spans `avformat_find_stream_info` and the
  decoder's open, where this build's decoders set up, and it makes the
  CRC and AES tables itself at its first open, since a demuxer can ask
  for those while reading); `libmedia/media.c`
  § THREADS says why it is enough, and `tools/test_av_audit.py` pins the
  one-time setups it rests on. Turning threads on means a pthread shim (mutex, condition, once)
  over `os64_thread`, the lock and bells.
- **Assembly** (`--disable-asm`, `--disable-x86asm`): FFmpeg's hand-written
  x86 needs `nasm`, and much of its fast path assumes AVX, which os64
  leaves off. Irrelevant to sound.
- **Inline assembly** (`--disable-inline-asm`): see the next section.

## How it is built (`shared.mk`)

FFmpeg's sources compile with FFmpeg's own flags for this target (its
configure's `CFLAGS`: `-std=c17 -O3 -fno-math-errno -fno-signed-zeros`,
its warning set and the warnings it makes errors), not with userland's
`-Wextra -Werror`, which upstream code was never written against (the
kernel's SBC port follows the same rule). Two of FFmpeg's guards matter
more on os64 than on Linux, because a thread's stack is fixed:
`-Werror=vla` and `-Wstack-usage`. os64's code generation is kept: PIC,
no red zone, SSE2, no AVX, the stack-clash probes. Left out:

- userland's `-masm=intel`: FFmpeg is written for GCC's default syntax,
  and in Intel syntax a symbol spelled like a register (FFmpeg has them)
  does not assemble. This is also why inline assembly is a flag away, not
  a rewrite.
- FFmpeg's `-Wredundant-decls`: the shims declare what FFmpeg's internal
  headers declare again (`av_sscanf`), by design.
- FFmpeg's `-fomit-frame-pointer`: userland's default is kept.

GCC 14 at `-O3` warns about array subscripts in `libavcodec/vlc.c`; that
is upstream's code, unchanged, and they stay warnings. `port/` is os64's
code and keeps userland's full alarm.

The library is linked `--no-undefined --gc-sections` against `libos64.so`
and `libmath.so` only, exports FFmpeg's public prefixes (`exports.map`:
`av_*`, `avcodec_*`, `avformat_*`, `avio_*`, `avutil_*`,
`avsubtitle_free`), and has no
initialisers, no thread-local storage and no system call: the audit checks
each.

A program that calls FFmpeg compiles with `LIBAV_CONSUMER_CFLAGS` and is
named in `LIBAV_PROGRAMS` (`shared.mk`). FFmpeg's public headers include
C-library headers, so the caller sees `port/compat/` too, but only the
part of it that names libos64's exports (`memcpy`, `malloc`…), the error
numbers and the types: the names `av_libc.c`, `av_format.c` and
`av_port.c` supply leave libav.so under no name, and are mapped only
inside libav's own build (`HAVE_AV_CONFIG_H`, which FFmpeg defines for its
sources and `shared.mk` for `port/`). A caller that calls `strncmp` gets a
link error, not a function it cannot reach.

## Proof

- `/tests/avtest`, in `testrun`: in ring 3, an MP3 in memory probes, its
  ID3v2 tag reads (genre "(17)" becomes "Rock" through the routed scanf),
  it decodes to exactly one second at the level the host's FFmpeg decodes
  the same bytes to (`tools/gen_avtest_mp3.py` made them and measured it),
  a seek lands, a WAV decodes sample for sample, noise is refused, and an
  MP4 whose first track is a picture yields its AAC sound, one second of
  the sine holding the energy the host's FFmpeg decodes it to
  (`tools/gen_avtest_m4a.py`).
- `tools/test_av_libc_host.sh`: the shims against glibc.
- `tools/test_av_audit.py`: the pin, the licence, the surface, the imports.
- `tools/import_av.py --check <archive>`: the vendored tree is the
  archive's and configure's.
