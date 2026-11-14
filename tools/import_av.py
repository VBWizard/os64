#!/usr/bin/env python3
"""Reproduce libav's vendored FFmpeg from the pinned release archive.

    tools/import_av.py --check <ffmpeg-X.tar.xz>   prove upstream/, port/config/
                                                    and sources.mk are what the
                                                    archive and manifest produce
    tools/import_av.py --write <ffmpeg-X.tar.xz>   rebuild them and manifest.json's
                                                    file list from the archive

FFmpeg decides what a configuration builds: this script runs FFmpeg's own
`configure` with manifest.json's `configure_args`, so which decoders and
readers exist is one reviewable line of arguments, never a hand-kept list.
os64 cannot run configure, so it runs here, on the host, and what it writes
is committed:

- port/config/: config.h (with manifest.json's `config_overrides` applied:
  configure probes the HOST's C library and build tools, and every answer
  about a C library is replaced by what libos64 and the port's shims
  provide, and every answer about a tool, perl or makeinfo, by "no", which
  is what os64 has and keeps --check from depending on what this machine
  has installed), and the other files configure generates that the build
  compiles or includes. What it records about the C COMPILER (INT128,
  inline-asm abilities) is the host gcc's, which answers as the cross gcc
  does; a host whose cc is not gcc may see --check differ there.
- sources.mk: the objects FFmpeg's own makefiles would build (its
  lib*.a.objs lists), as source paths.
- upstream/: exactly those sources and every FFmpeg header they include,
  found by the cross compiler's own dependency output, byte for byte from
  the archive, plus the licence files. Nothing is patched.

An override that no longer matches a line of the generated config.h fails
the run: an upstream change to what configure writes must be looked at, not
silently skipped. tools/test_av_audit.py checks the tree against the
manifest on every run; this script is what checks the manifest against
FFmpeg.

Needs: a host `cc` (configure's probes), `x86_64-elf-gcc` (the dependency
walk), `make`, and the archive. Takes about a minute.
"""
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
USERLAND = ROOT / 'userland'
LIB = USERLAND / 'libav'
UPSTREAM = LIB / 'upstream'
CONFIG = LIB / 'port' / 'config'
COMPAT = LIB / 'port' / 'compat'
MANIFEST = LIB / 'manifest.json'
SOURCES_MK = LIB / 'sources.mk'

# configure's generated files the build needs, relative to its build tree.
GENERATED = [
    'config.h',
    'config_components.h',
    'libavutil/avconfig.h',
    'libavutil/ffversion.h',
    # The registries: #included by allcodecs.c, parsers.c, allformats.c,
    # protocols.c and bitstream_filters.c, never compiled on their own.
    'libavcodec/codec_list.c',
    'libavcodec/parser_list.c',
    'libavcodec/bsf_list.c',
    'libavformat/demuxer_list.c',
    'libavformat/muxer_list.c',
    'libavformat/protocol_list.c',
]
# Shipped whole whatever the configuration: the licence the library is
# distributed under, and the files that say which release this is.
ALWAYS = ['COPYING.LGPLv2.1', 'LICENSE.md', 'VERSION', 'RELEASE', 'CREDITS']
LIBRARIES = ['libavutil', 'libavcodec', 'libavformat']


def sha256(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def run(cmd, cwd):
    result = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True)
    if result.returncode != 0:
        raise SystemExit(f'{cmd[0]} failed in {cwd}:\n{result.stdout[-2000:]}{result.stderr[-2000:]}')
    return result.stdout


def apply_overrides(text, overrides):
    for name, value in overrides.items():
        if isinstance(value, int):
            pattern = re.compile(rf'^#define {re.escape(name)} [01]$', re.M)
            replacement = f'#define {name} {value}'
        else:
            pattern = re.compile(rf'^#define {re.escape(name)} .*$', re.M)
            replacement = f'#define {name} {json.dumps(value)}'
        text, count = pattern.subn(replacement, text)
        if count != 1:
            raise SystemExit(f'config_overrides: {name} matched {count} lines of config.h, not 1')
    return text


def produce(archive, manifest, out):
    """Everything the import makes, written under `out` in the tree's layout."""
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        with tarfile.open(archive) as tar:
            tar.extractall(tmp / 'src', filter='data')
        src = tmp / 'src' / manifest['archive_prefix'].rstrip('/')
        build = tmp / 'build'
        build.mkdir()
        run([str(src / 'configure')] + manifest['configure_args'], build)
        # ffversion.h is made by FFmpeg's makefile (from VERSION), not by
        # configure, and the lib*.a.objs lists are written while make reads
        # the archive rules, which a dry run of the three archives does.
        run(['make', 'libavutil/ffversion.h'], build)
        run(['make', '-n'] + [f'{lib}/{lib}.a' for lib in LIBRARIES], build)

        config = out / 'port' / 'config'
        for name in GENERATED:
            target = config / name
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(build / name, target)
        config_h = config / 'config.h'
        config_h.write_text(apply_overrides(config_h.read_text(), manifest['config_overrides']))

        objects = []
        for lib in LIBRARIES:
            listed = (build / lib / f'{lib}.a.objs').read_text().split()
            objects += [o for o in listed if o.endswith('.o')]
        sources = []
        for obj in sorted(objects):
            c = obj[:-2] + '.c'
            if not (src / c).exists():
                raise SystemExit(f'{obj}: no source {c} in the archive')
            sources.append(c)

        needed = set(sources) | set(ALWAYS)
        flags = ['-std=c17', '-ffreestanding', '-MM', '-DHAVE_AV_CONFIG_H',
                 f'-I{COMPAT}', f'-I{LIB / "port"}', f'-I{config}', f'-I{src}',
                 f'-I{USERLAND / "libos64/include"}', f'-I{ROOT / "abi/include"}',
                 f'-I{USERLAND / "libmath/include"}']
        for c in sources:
            deps = run(['x86_64-elf-gcc'] + flags + [str(src / c)], build)
            for dep in deps.replace('\\\n', ' ').split(':', 1)[1].split():
                dep = Path(os.path.normpath(dep))
                if dep.is_relative_to(src):
                    needed.add(str(dep.relative_to(src)))

        upstream = out / 'upstream'
        for name in sorted(needed):
            target = upstream / name
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(src / name, target)

        lines = ['# sources.mk — what libav.so compiles. GENERATED by tools/import_av.py',
                 '# from FFmpeg\'s own object lists for manifest.json\'s configure_args;',
                 '# never edited by hand.',
                 'AV_UPSTREAM_SRCS := \\']
        lines += [f'    upstream/{c} \\' for c in sources]
        lines.append('')
        lines.append('AV_PORT_SRCS := port/av_libc.c port/av_format.c port/av_port.c')
        (out / 'sources.mk').write_text('\n'.join(lines) + '\n')
        return {name: sha256(upstream / name) for name in sorted(needed)}


def tree_files(base):
    return {str(p.relative_to(base)) for p in base.rglob('*') if p.is_file()}


def main():
    if len(sys.argv) != 3 or sys.argv[1] not in ('--check', '--write'):
        raise SystemExit(__doc__)
    mode, archive = sys.argv[1], Path(sys.argv[2])
    manifest = json.loads(MANIFEST.read_text())
    if sha256(archive) != manifest['archive_sha256']:
        raise SystemExit(f'{archive}: digest is not the pinned archive_sha256')
    with tempfile.TemporaryDirectory() as tmp:
        out = Path(tmp)
        files = produce(archive, manifest, out)
        if mode == '--check':
            bad = []
            if files != manifest['files']:
                bad.append('manifest.json files')
            for sub in ('upstream', 'port/config'):
                made, have = out / sub, LIB / sub
                if tree_files(made) != tree_files(have):
                    bad.append(f'{sub}: file set differs')
                    continue
                bad += [f'{sub}/{n}' for n in sorted(tree_files(made))
                        if (made / n).read_bytes() != (have / n).read_bytes()]
            if (out / 'sources.mk').read_text() != SOURCES_MK.read_text():
                bad.append('sources.mk')
            if bad:
                raise SystemExit('not what the archive produces: ' + ', '.join(bad))
            print(f'PASS import: {len(files)} upstream files, port/config and sources.mk '
                  'are what the pinned archive and configure produce')
            return
        for sub in ('upstream', 'port/config'):
            if (LIB / sub).exists():
                shutil.rmtree(LIB / sub)
            shutil.copytree(out / sub, LIB / sub)
        shutil.copyfile(out / 'sources.mk', SOURCES_MK)
    manifest['files'] = files
    MANIFEST.write_text(json.dumps(manifest, indent=2) + '\n')
    print(f'WROTE upstream/ ({len(files)} files), port/config/, sources.mk and manifest.json')


main()
