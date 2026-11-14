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
SHA-256 (its signature checked against FFmpeg's release key,
`FCF986EA15E6E293A5644F10B4322F04D67658D8`, on import).

**What is built is FFmpeg's decision, from one line of arguments.**
`manifest.json`'s `configure_args` is FFmpeg's `configure` with everything
disabled and then, by name:

| Kind | Enabled |
|---|---|
| readers (demuxers) | `wav`, `mp3` |
| decoders | `mp3float`, `pcm_s16le`, `pcm_s24le`, `pcm_s32le`, `pcm_u8`, `pcm_f32le`, `pcm_alaw`, `pcm_mulaw` |
| parsers | `mpegaudio` |
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
libos64 has is mapped onto libos64 (`memcpy`, `strlen`, `malloc`,
`snprintf`…), and the rest is os64's, in two files:

- **`port/av_libc.c`** is pure (no os64 header): `strncmp`, `strstr`,
  `strspn`, `strcspn`, `strcpy`, the four integer `strto*`, `frexp`,
  `scalbn`, `qsort` (a heapsort: no recursion, no allocation), `bsearch`,
  and UTC `gmtime`, `mktime` and `strftime`. `tools/test_av_libc_host.sh`
  compares every one with glibc (C17 rules), under ASan and UBSan.
- **`port/av_port.c`** is the doors. FFmpeg never opens a file here, so
  `open` and `fopen` answer as for a missing file and FFmpeg takes the path
  it already has for that. `stderr` takes the default log callback's words
  and discards them: a caller that wants them installs its own
  (`av_log_set_callback`). `strtod` goes through FFmpeg's own scanf
  (`av_sscanf`, musl's), as plain `sscanf` does. `clock` is `micros()`.
  `abort` (FFmpeg's `av_assert0`) ends the program with `0x41564641`
  ("AVFA"), QuickJS's port's shape.

What a caller should know the shims do NOT do:

- **No floating point in `snprintf`.** libos64's formatter has none: a
  `%f` prints as itself, and the arguments after it still line up (a
  double travels in a register of its own). FFmpeg formats floats into its
  logs, its dumps (`av_dump_format`), and a few text renderings of numbers
  (`av_opt_get` of a float option, EXIF and SDP text). None is on the path
  from bytes to sound.
- **No environment and no clock reach the library**: `getenv` answers
  nothing, `time()` answers 0 (FFmpeg's "unknown"), and local time is UTC.
- **`errno` is one word**, written only by `strto*` on overflow.

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
  installs a log callback that is safe from any thread. That is libmedia's
  rule. Turning threads on means a pthread shim (mutex, condition, once)
  over `os64_thread`, the lock and bells.
- **Assembly** (`--disable-asm`, `--disable-x86asm`): FFmpeg's hand-written
  x86 needs `nasm`, and much of its fast path assumes AVX, which os64
  leaves off. Irrelevant to sound.
- **Inline assembly** (`--disable-inline-asm`): see the next section.

## How it is built (`shared.mk`)

FFmpeg's sources compile with FFmpeg's own flags for this target (its
configure's `CFLAGS`: `-std=c17 -O3 -fno-math-errno -fno-signed-zeros`
and its warning set), not with userland's `-Wextra -Werror`, which
upstream code was never written against (the kernel's SBC port follows
the same rule). os64's code generation is kept: PIC, no red zone, SSE2,
no AVX, the stack-clash probes. Two of userland's flags are dropped for
them:

- `-masm=intel`: FFmpeg is written for GCC's default syntax, and in Intel
  syntax a symbol spelled like a register (FFmpeg has them) does not
  assemble. This is also why inline assembly is a flag away, not a rewrite.
- `-Wredundant-decls`: the shims declare what FFmpeg's internal headers
  declare again (`av_sscanf`), by design.

GCC 14 at `-O3` warns about array subscripts in `libavcodec/vlc.c`; that
is upstream's code, unchanged, and they stay warnings. `port/` is os64's
code and keeps userland's full alarm.

The library is linked `--no-undefined --gc-sections` against `libos64.so`
and `libmath.so` only, exports FFmpeg's public prefixes (`exports.map`:
`av_*`, `avcodec_*`, `avformat_*`, `avio_*`, `avutil_*`), and has no
initialisers, no thread-local storage and no system call: the audit checks
each.

A program that calls FFmpeg compiles with `LIBAV_CONSUMER_CFLAGS` and is
named in `LIBAV_PROGRAMS` (`shared.mk`): FFmpeg's public headers include
C-library headers, so the caller sees the shims too.

## Proof

- `/tests/avtest`, in `testrun`: in ring 3, an MP3 in memory probes, its
  ID3v2 tag reads (genre "(17)" becomes "Rock" through the routed scanf),
  it decodes to exactly one second at the level the host's FFmpeg decodes
  the same bytes to (`tools/gen_avtest_mp3.py` made them and measured it),
  a seek lands, a WAV decodes sample for sample, and noise is refused.
- `tools/test_av_libc_host.sh`: the shims against glibc.
- `tools/test_av_audit.py`: the pin, the licence, the surface, the imports.
- `tools/import_av.py --check <archive>`: the vendored tree is the
  archive's and configure's.
