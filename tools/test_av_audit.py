#!/usr/bin/env python3
"""Audit libav.so: the import is what manifest.json pins, the configuration
is LGPL and what configure was asked for, and the library is a library of
its own with the surface and the dependencies its README says.

Run after a build (`make -C userland`). Each check names the property it
protects, so a failure says what was lost rather than only where.
"""
import hashlib
import json
import re
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
USERLAND = ROOT / 'userland'
LIB = USERLAND / 'libav'
SO = USERLAND / 'bin/libav.so'


def run(*args):
    return subprocess.check_output([str(a) for a in args], text=True)


def check(cond, what):
    if not cond:
        raise SystemExit(f'FAIL {what}')


def defines(text):
    return dict(re.findall(r'^#define (\w+) (.*)$', text, re.M))


# The import: every vendored file is what manifest.json pins, and nothing is
# vendored that it does not name. (tools/import_av.py --check proves the
# manifest, port/config and sources.mk are what the pinned archive and
# configure produce; it needs the archive, this does not.) Nothing is
# patched, and the manifest says so.
manifest = json.loads((LIB / 'manifest.json').read_text())
upstream = LIB / 'upstream'
present = {str(p.relative_to(upstream)) for p in upstream.rglob('*') if p.is_file()}
check(present == set(manifest['files']), f'upstream/ and manifest.json disagree: {sorted(present ^ set(manifest["files"]))[:10]}')
for name, digest in manifest['files'].items():
    check(hashlib.sha256((upstream / name).read_bytes()).hexdigest() == digest, f'{name} differs from manifest.json')
check(manifest['patches'] == [] and not (LIB / 'patches').exists(), 'the port is unpatched, and something says otherwise')

# THE LICENCE IS THE CONFIGURATION'S. FFmpeg is LGPL 2.1 unless configure is
# asked for GPL, nonfree or version 3 parts; none may be asked for, and the
# generated configuration must agree (README.md § The licence).
args = manifest['configure_args']
for forbidden in ('--enable-gpl', '--enable-nonfree', '--enable-version3'):
    check(forbidden not in args, f'configure_args asks for {forbidden}')
config = defines((LIB / 'port/config/config.h').read_text())
for name in ('CONFIG_GPL', 'CONFIG_NONFREE', 'CONFIG_VERSION3'):
    check(config.get(name) == '0', f'config.h has {name} {config.get(name)}')
check(config.get('FFMPEG_LICENSE') == '"LGPL version 2.1 or later"', f'config.h says {config.get("FFMPEG_LICENSE")}')
check(manifest['license'] == 'LGPL-2.1-or-later', 'manifest.json names another licence')

# The configuration is the one asked for, with every override in force.
# (configure quotes a list value, --enable-demuxer='wav,mp3', in what it records.)
check(config.get('FFMPEG_CONFIGURATION', '').strip('"').replace("'", '').split() == args,
      'config.h was configured with other arguments than manifest.json\'s')
for name, value in manifest['config_overrides'].items():
    want = str(value) if isinstance(value, int) else json.dumps(value)
    check(config.get(name) == want, f'config.h has {name} {config.get(name)}, manifest.json says {want}')
for name in ('HAVE_PTHREADS', 'CONFIG_NETWORK'):
    check(config.get(name) == '0', f'config.h has {name} {config.get(name)}: README.md says it is off')
# FFmpeg's x86 assembly is built, and FFmpeg chooses its routines from what
# the CPU and the OS offer at run time (README.md § How it is built).
for name in ('HAVE_X86ASM', 'HAVE_INLINE_ASM', 'CONFIG_RUNTIME_CPUDETECT', 'CONFIG_PIC'):
    check(config.get(name) == '1', f'config.h has {name} {config.get(name)}: README.md says it is on')
# AVX-512 is configured OUT: os64 never enables its state (AVX.md), so those
# routines could never be chosen, and leaving them out keeps FFmpeg's ALIGN at
# 32, which os64_malloc_aligned's dedicated blocks exceed.
for flag in ('--disable-avx512', '--disable-avx512icl'):
    check(flag in args, f'configure_args lacks {flag}')
for name in ('HAVE_AVX512', 'HAVE_AVX512ICL', 'HAVE_AVX512_EXTERNAL', 'HAVE_AVX512ICL_EXTERNAL'):
    check(config.get(name) == '0', f'config.h has {name} {config.get(name)}: AVX-512 must be absent')

# Every translation unit sources.mk builds is vendored.
built = set(re.findall(r'upstream/(\S+\.c)', (LIB / 'sources.mk').read_text()))
check(built and built <= set(manifest['files']), f'sources.mk builds unvendored files: {sorted(built - set(manifest["files"]))}')

# The public surface: FFmpeg's own, by the prefixes its version scripts
# export, and nothing of FFmpeg's internals or of the port's shims.
dynamic = {l.split()[-1] for l in run('x86_64-elf-nm', '-D', '--defined-only', SO).splitlines()}
public = re.compile(r'^(av_|avcodec_|avformat_|avio_|avutil_|avsubtitle_free$|sws_|swscale_)')
check(dynamic and all(public.match(s) for s in dynamic), f'libav.so exports {sorted(s for s in dynamic if not public.match(s))[:10]}')
for needed_api in ('avformat_open_input', 'avcodec_send_packet', 'avcodec_receive_frame', 'avio_alloc_context',
                   'sws_alloc_context', 'sws_scale_frame', 'sws_free_context', 'sws_test_format'):
    check(needed_api in dynamic, f'libav.so does not export {needed_api}')

# What it leans on: libos64's allocator and strings, its exit and clock,
# and libmath's double-precision functions. A new import is a new
# dependency somebody should have decided on.
undefined = {l.split()[-1] for l in run('x86_64-elf-nm', '-D', '-u', SO).splitlines()}
allowed_os64 = {'os64_malloc', 'os64_malloc_aligned', 'os64_realloc', 'os64_free', 'os64_memchr', 'os64_memcmp', 'os64_memcpy',
                'os64_memmove', 'os64_memset', 'os64_strchr', 'os64_strcmp', 'os64_strlen', 'os64_strrchr',
                'os64_snprintf', 'os64_vsnprintf', 'os64_micros', 'os64_exit',
                # the C names the compiler emits for a copy or a fill it recognises
                'memset', 'memcpy', 'memmove'}
math = set(re.findall(r'\b([a-z0-9]+);', (USERLAND / 'libmath/exports.map').read_text().split('local:')[0]))
check(undefined <= allowed_os64 | math, f'libav.so imports {sorted(undefined - allowed_os64 - math)}')
dyn = run('x86_64-elf-readelf', '-dW', SO)
needed = set(re.findall(r'\(NEEDED\).*\[(.*?)\]', dyn))
check(needed == {'libos64.so', 'libmath.so'}, f'libav.so needs {sorted(needed)}')
check('(HASH)' in dyn, 'libav.so has no sysv hash table (the kernel sizes .dynsym from it)')
check(not re.search(r'\((TEXTREL|INIT|FINI|INIT_ARRAY|FINI_ARRAY)\)', dyn),
      'libav.so has text relocations or initialisers (the kernel runs no initialisers)')
check('TLS' not in run('x86_64-elf-readelf', '-lW', SO), 'libav.so has thread-local storage (the kernel has none)')

# No system call of its own: every door FFmpeg reaches for is a shim.
disasm = run('x86_64-elf-objdump', '-d', '--no-show-raw-insn', SO)
check(not re.search(r'\t(syscall|sysenter|int)\b', disasm), 'libav.so contains a system call')
# No AVX-512 code that can run. One routine is assembled anyway: the scaler's
# experimental backend (libswscale/x86/ops_common.asm) instantiates
# packed_shuffle64_64 for AVX-512 with no guard, where the scaler's other
# AVX-512 routines (x86/rgb_2_rgb.asm) sit behind HAVE_AVX512ICL_EXTERNAL.
# Nothing can select it: x86/ops.c reaches it only with a
# 64-byte vector, which compile() picks only when EXTERNAL_AVX512 holds, and
# with HAVE_AVX512_EXTERNAL 0 (pinned above) that is false at compile time.
# Any other routine holding a zmm register fails here.
zmm_routines, routine = set(), None
for line in disasm.splitlines():
    label = re.match(r'^[0-9a-f]+ <([^>]+)>:$', line)
    if label:
        routine = label.group(1).split('.')[0]
    elif '%zmm' in line:
        zmm_routines.add(routine)
check(zmm_routines <= {'ff_packed_shuffle64_64_avx512'},
      f'libav.so contains AVX-512 code (a zmm register) in {sorted(zmm_routines)}')

# THE ONLY AVX IN THE LIBRARY IS FFMPEG'S ASSEMBLY, chosen at run time: every
# C object (upstream's and the port's) is compiled -mno-avx, so none may hold
# a VEX-encoded instruction (their mnemonics are the v-prefixed ones), and
# the assembly objects are where the AVX routines must be.
objdir = USERLAND / 'obj/av'
c_objects = sorted(objdir.rglob('*.c.o'))
asm_objects = sorted(objdir.rglob('*.asm.o'))
check(c_objects and asm_objects, 'no libav objects to read: build userland first')
vex = re.compile(r'^\s+[0-9a-f]+:\s+v[a-z0-9]+\s', re.M)
for obj in c_objects:
    found = vex.search(run('x86_64-elf-objdump', '-d', '--no-show-raw-insn', obj))
    check(found is None, f'{obj.relative_to(USERLAND)} (C) holds a VEX instruction: {found and found.group(0).strip()}')
check(any('%ymm' in run('x86_64-elf-objdump', '-d', '--no-show-raw-insn', o) for o in asm_objects),
      'no assembly object holds an AVX (ymm) routine: is the assembly built?')

# A LIBRARY OF ITS OWN: the LGPL is met by a library the user can replace,
# so no other program or library may carry FFmpeg's code linked in. The
# test is every FFmpeg name libav.so defines (its full symbol table, local
# ones included: ff_*, avpriv_* and the public prefixes), against what each
# other binary defines. A binary with no symbol table cannot be checked, and
# says so, rather than passing.
ffmpeg_prefix = re.compile(r'^(ff_|avpriv_|av_|avcodec_|avformat_|avio_|avutil_)')
ours = {l.split()[-1] for l in run('x86_64-elf-nm', '--defined-only', SO).splitlines()
        if len(l.split()) == 3 and ffmpeg_prefix.match(l.split()[-1])}
check(len(ours) > 1000, f'libav.so\'s symbol table names only {len(ours)} FFmpeg symbols: is it stripped?')
for path in sorted((USERLAND / 'bin').rglob('*')):
    if not path.is_file() or path == SO or path.read_bytes()[:4] != b'\x7fELF':
        continue
    lines = run('x86_64-elf-nm', '--defined-only', path).splitlines()
    check(lines, f'{path.relative_to(USERLAND)} has no symbol table, so the audit cannot see what it carries')
    theirs = {l.split()[-1] for l in lines if len(l.split()) == 3}
    shared = sorted(theirs & ours)
    check(not shared, f'{path.relative_to(USERLAND)} carries FFmpeg code of its own: {shared[:5]}')

# The notice that travels with both images: FFmpeg's LICENSE.md and the
# LGPL it names, byte for byte, and where the source is.
notice = (ROOT / 'license/ffmpeg-LICENSE').read_text()
check((upstream / 'LICENSE.md').read_text() in notice, 'license/ffmpeg-LICENSE does not carry LICENSE.md')
check((upstream / 'COPYING.LGPLv2.1').read_text() in notice, 'license/ffmpeg-LICENSE does not carry the LGPL')
check('userland/libav' in notice and 'https://github.com/VBWizard/os64' in notice,
      'license/ffmpeg-LICENSE does not say where the source is')
root_make = (ROOT / 'GNUmakefile').read_text()
check(root_make.count('/etc/licenses/ffmpeg.txt') == 2, 'the notice is not installed on both images')
check('userland/bin/libav.so' in root_make, 'libav.so is not in the images')

# ONE DOOR TO FFMPEG: libmedia.so is the library programs play sound
# through (os64/media.h), and the only one that may load libav.so, beside
# libav's own fixtures (avtest, and avbench, which measures the video
# decoder). A player that called FFmpeg directly would tie itself to the
# decoder libmedia exists to hide.
def needed(path):
    return set(re.findall(r'\(NEEDED\).*\[(.*?)\]', run('x86_64-elf-readelf', '-dW', path)))

callers = set()
for path in sorted((USERLAND / 'bin').rglob('*')):
    if path.is_file() and path != SO and path.read_bytes()[:4] == b'\x7fELF' and 'libav.so' in needed(path):
        callers.add(str(path.relative_to(USERLAND / 'bin')))
check(callers == {'libmedia.so', 'tests/avtest', 'tests/avbench'},
      f'libav.so is loaded by {sorted(callers)}, not only libmedia.so and the fixtures')
MEDIA = USERLAND / 'bin/libmedia.so'
check(needed(MEDIA) == {'libav.so', 'libsound.so', 'libos64.so'}, f'libmedia.so needs {sorted(needed(MEDIA))}')
media_exports = {l.split()[-1] for l in run('x86_64-elf-nm', '-D', '--defined-only', MEDIA).splitlines()}
check(media_exports and all(e.startswith('os64_media_') for e in media_exports),
      f'libmedia.so exports {sorted(e for e in media_exports if not e.startswith("os64_media_"))}')
for program in ('play', 'tests/mediatest'):
    check('libmedia.so' in needed(USERLAND / 'bin' / program), f'{program} does not load libmedia.so')

# A PICTURE IS RELEASED ON ANY THREAD AND RETURNED ON THE DECODING THREAD
# (libmedia/media.c § THREADS): FFmpeg's pools and reference counts have no
# lock of their own in a build without threads, so nothing that may run off
# the decoding thread may reach libav.so except the scaler's own calls on
# memory no decoder shares. The test follows every call and jump within
# libmedia.so from each such entry point, and names what it reaches of
# libav.so:
#   os64_media_picture_release   nothing (it only pushes onto a locked list)
#   os64_media_scaler_convert    sws_scale_frame, which on a dynamic context
#                                with the caller's buffers takes no
#                                reference to the source (pinned below)
#   os64_media_scaler_create     the scaler's context and its own canvas
#   os64_media_scaler_destroy    frame, which holds no pool buffer
# The convert entry must reach exactly what it names (so a change in how
# the disassembly spells a call cannot pass by finding nothing), and no
# entry may reach an indirect call or jump, whose target the test cannot
# follow.
media_disasm = run('x86_64-elf-objdump', '-d', '--no-show-raw-insn', MEDIA)
media_calls, media_indirect, routine = {}, set(), None
for line in media_disasm.splitlines():
    label = re.match(r'^[0-9a-f]+ <([^>@+]+)>:$', line)
    if label:
        routine = label.group(1)
        media_calls[routine] = set()
        continue
    if routine and re.search(r'\t(?:call|jmp)\s+\*', line):
        media_indirect.add(routine)
    target = re.search(r'\t(?:call|jmp)\s+[0-9a-f]+ <([^>+]+)', line)
    if routine and target and target.group(1) != routine:
        media_calls[routine].add(target.group(1))
libav_exports = dynamic


def libav_reached(entry):
    seen, todo, reached = set(), [entry], set()
    while todo:
        f = todo.pop()
        if f in seen:
            continue
        seen.add(f)
        check(f not in media_indirect, f'{entry} reaches an indirect call or jump in {f}, which the thread rule\'s '
              'test cannot follow (libmedia/media.c § THREADS)')
        for t in media_calls.get(f, ()):
            name = t.removesuffix('@plt')
            if t.endswith('@plt'):
                if name in libav_exports:
                    reached.add(name)
            else:
                todo.append(name)
    return reached


check('os64_media_picture_release' in media_calls and 'os64_media_scaler_convert' in media_calls,
      'libmedia.so lacks the picture entry points the thread rule names')
for entry, allowed in (('os64_media_picture_release', set()),
                       ('os64_media_scaler_create', {'sws_alloc_context', 'av_frame_alloc', 'sws_free_context',
                                                     'av_frame_free'}),
                       ('os64_media_scaler_destroy', {'sws_free_context', 'av_frame_free'})):
    reached = libav_reached(entry)
    check(reached <= allowed, f'{entry} reaches {sorted(reached - allowed)} of libav.so, off the decoding thread '
          '(libmedia/media.c § THREADS)')
convert_reaches = libav_reached('os64_media_scaler_convert')
check(convert_reaches == {'sws_scale_frame'},
      f'os64_media_scaler_convert reaches {sorted(convert_reaches)} of libav.so, not exactly the scaler\'s own calls '
      '(libmedia/media.c § THREADS)')

# The FFmpeg half: sws_scale_frame, given a dynamic context and a
# destination with buffers of its own, references nothing of the source;
# its only frame_ref is in the branch where the destination has no
# buffers, and the legacy path (sws_frame_start, which does) is taken only
# for a context sws_init_context made, which libmedia never makes for
# converting. Pinned by the function's text, so a release that changes it
# fails here until it has been read again for that.
swscale_c = (upstream / 'libswscale/swscale.c').read_text()
body = re.search(r'^int sws_scale_frame\(SwsContext \*sws, AVFrame \*dst, const AVFrame \*src\)\n\{\n.*?^\}\n',
                 swscale_c, re.M | re.S)
check(body is not None, 'libswscale/swscale.c has no sws_scale_frame to read')
SWS_SCALE_FRAME_SHA = '82cc34d051ee3d068ea34e3f9627c9802c8b2128de6141fa7d6f37d446e4b9e1'
check(hashlib.sha256(body.group(0).encode()).hexdigest() == SWS_SCALE_FRAME_SHA,
      'sws_scale_frame changed: read it again for a reference to the source frame (libmedia/media_scale.c), '
      'then update SWS_SCALE_FRAME_SHA')
# THE UNSCALED EDGE (libmedia/media_scale.c): the unscaled YUV-to-RGB
# converter's x86 form writes whole groups of eight pixels across when the
# pitch has room for them, which is the bound the bounce buffer's pitch is
# sized for. Pinned by its two lines.
yuv2rgb_x86 = (upstream / 'libswscale/x86/yuv2rgb.c').read_text()
check('h_size = (c->opts.dst_w + 7) & ~7;' in yuv2rgb_x86 and
      'if (h_size * depth > FFABS(dstStride[0]))' in yuv2rgb_x86,
      'libswscale/x86/yuv2rgb.c no longer rounds a row to whole groups of eight: read THE UNSCALED EDGE in '
      'libmedia/media_scale.c again')

# CODE THE SCALER WRITES AT RUN TIME (README.md § The x86 assembly):
# libswscale's fast bilinear writes an x86 horizontal filter of its own
# and runs it, in memory from mmap and mprotect where it has them and from
# av_malloc where, as here, it has not (HAVE_MMAP 0); os64's data memory
# does not execute, so running it is a no-execute fault. It does so only
# for SWS_FAST_BILINEAR and only when enlarging (canMMXEXTBeUsed needs
# dstW >= srcW), which is why os64's callers ask for fast bilinear only
# when shrinking. A release that changes either fails here until it has
# been read again.
utils_c = (upstream / 'libswscale/utils.c').read_text()
check(config.get('HAVE_MMAP') == '0' and config.get('HAVE_MPROTECT') == '0',
      'config.h has mmap or mprotect: the scaler would write its code where it can execute, read this again')
check('c->canMMXEXTBeUsed = dstW >= srcW &&' in utils_c and
      'if (c->canMMXEXTBeUsed && (flags & SWS_FAST_BILINEAR)) {' in utils_c,
      'libswscale/utils.c changed when the scaler writes code at run time: read README.md § The x86 assembly again')

# FFmpeg's ONE-TIME SETUPS, which a build without threads leaves unlocked
# (README.md § Switched off). Every ff_thread_once in every vendored file,
# templates and headers included (a decoder's setup can live in a template
# its .c file includes), with its count, so a new one anywhere fails here.
# Where each is reached in this build, which is why libmedia/media.c
# § THREADS is enough:
#   mpegaudiodec_*, mpegaudiodsp*, pcm.c,  a decoder's open: libmedia's,
#   aac/aacdec_float.c, aac/aacdec_tab.c,  and avformat_find_stream_info's,
#   aactab.c, sinewin_tablegen.h           both under libmedia's lock (the
#                                          AAC four from its init,
#                                          ff_aac_decode_init_float and the
#                                          ff_aac_decode_init it calls)
#   crc.c, aes.c                           libmedia makes every CRC table and
#                                          the AES tables at its first open
#                                          (the MP4 reader runs AES over an
#                                          `adrm` file's packets, unlocked)
#   tx_template.c                          the AAC decoder's open (its
#                                          MDCTs, up to 1024, under the
#                                          lock); SBR asks again while
#                                          decoding, unlocked, for 64-point
#                                          ones, which that open already
#                                          made: a power of two's tables
#                                          include every smaller one's
#   h264dec.c                              the H.264 decoder's open (its VLC
#                                          tables): find_stream_info's when
#                                          libmedia probes an MP4 with a
#                                          video track, under the lock, and
#                                          a video program's own open
#                                          (avbench decodes on one thread)
#   libswscale/utils.c                     the scaler's: rgb2rgb's tables in
#                                          sws_init_context, which a dynamic
#                                          context's graph calls for its
#                                          legacy passes at the first
#                                          conversion (avbench converts on
#                                          one thread); the XYZ gamma tables,
#                                          for the XYZ pixel kinds only,
#                                          which no decoder here produces
known_once = {'libavcodec/aac/aacdec_float.c': 1, 'libavcodec/aac/aacdec_tab.c': 1,
              'libavcodec/h264dec.c': 1, 'libswscale/utils.c': 2,
              'libavcodec/aactab.c': 1, 'libavcodec/sinewin_tablegen.h': 1,
              'libavcodec/mpegaudiodec_common.c': 1, 'libavcodec/mpegaudiodec_template.c': 1,
              'libavcodec/mpegaudiodsp.c': 1, 'libavcodec/mpegaudiodsp_template.c': 1,
              'libavcodec/pcm.c': 3, 'libavutil/aes.c': 1, 'libavutil/crc.c': 1,
              'libavutil/tx_template.c': 2}
once_sites = {}
for name in manifest['files']:
    if name.endswith(('.c', '.h')) and name != 'libavutil/thread.h':   # thread.h defines it
        n = len(re.findall(r'\bff_thread_once\s*\(', (upstream / name).read_text(errors='replace')))
        if n:
            once_sites[name] = n
check(once_sites == known_once, f'one-time setups changed: {sorted(set(once_sites.items()) ^ set(known_once.items()))}; '
      'decide whether libmedia/media.c § THREADS still covers them, then update this table')

print(f'PASS av audit: FFmpeg {manifest["release"]}, {len(manifest["files"])} pinned files, LGPL configuration, '
      f'{len(dynamic)} exports, {len(undefined)} imports')
