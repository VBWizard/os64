#!/usr/bin/env python3
"""test_freetype_host.py — build and run the font backend under the sanitizers.

    tools/test_freetype_host.py            # ASan + UBSan at -O2
    tools/test_freetype_host.py --plain    # no sanitizers, for a quick pass
    tools/test_freetype_host.py -O0        # the same tests at another level

WHY A HOST HARNESS AT ALL, for code whose whole point is to run in os64: a
font is an arbitrary file, the parser for it is 400KB of somebody else's C,
and the fastest way to find out whether a malformed one walks off the end of
a table is to let AddressSanitizer watch it do so. QEMU cannot do that, and a
boot costs thirty seconds where this costs a second.

It compiles the PRODUCTION sources — the same pinned upstream translation
units, the same port — with the HOST compiler and the same configuration
headers. `OS64_FREETYPE_HOSTED` is the only difference: it leaves out the
port's own `memcpy`/`memset`/`memmove`/`memcmp` so the sanitizers' own
interceptors can watch those calls instead.

-fcf-protection=none matches os64's build and, more to the point, is required
by port/nonlocal.S: a nonlocal return leaves a function by an indirect jump,
which indirect-branch tracking would treat as an illegal target.
"""

import argparse
import os
import shlex
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
LIB = REPO / "userland" / "libfreetype"
FIXTURES = LIB / "fixtures"

# The same two lists the cross build uses, read from the same file, so a
# source added to the library cannot go untested here by omission.
def sources():
    text = (LIB / "sources.mk").read_text()
    out, current = [], None
    for line in text.splitlines():
        stripped = line.strip()
        if stripped.startswith("FREETYPE_UPSTREAM_SRCS") or \
           stripped.startswith("FREETYPE_PORT_SRCS"):
            current = []
            out.append(current)
            stripped = stripped.split(":=", 1)[1].strip()
        if current is None or not stripped or stripped.startswith("#"):
            continue
        for token in stripped.rstrip("\\").split():
            if token.startswith("libfreetype/"):
                current.append(REPO / "userland" / token)
        if not line.rstrip().endswith("\\"):
            current = None
    upstream, port = out
    return upstream, port


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--plain", action="store_true",
                    help="build without sanitizers")
    ap.add_argument("-O", dest="opt", default="2",
                    help="optimization level (default 2 — the level that ships)")
    ap.add_argument("--keep", action="store_true",
                    help="keep the build directory and print its path")
    args = ap.parse_args()

    cc = os.environ.get("CC", "cc")
    upstream, port = sources()
    harness = REPO / "tools" / "test_freetype_host.c"

    common = [
        f"-O{args.opt}", "-g", "-std=c11",
        "-fcf-protection=none",
        "-fno-builtin", "-fno-tree-loop-distribute-patterns",
        "-DOS64_FREETYPE_HOSTED",
        f"-I{LIB / 'port'}",
        f"-I{LIB / 'upstream' / 'include'}",
        f"-I{REPO / 'userland' / 'libos64' / 'include'}",
    ]
    sanitize = [] if args.plain else [
        "-fsanitize=address,undefined",
        "-fno-sanitize-recover=all",     # a finding must fail the run, not log
        "-fno-omit-frame-pointer",
    ]

    # Upstream's own configuration warts under a trimmed module set, exactly
    # as userland/libfreetype/shared.mk suppresses them for the cross build.
    upstream_only = ["-DFT2_BUILD_LIBRARY",
                     "-Wno-unused-variable", "-Wno-unused-but-set-variable"]
    strict = ["-Wall", "-Wextra", "-Werror"]

    tmp = tempfile.mkdtemp(prefix="ft-host-")
    objects = []
    failed = False

    def compile_one(src, extra):
        obj = Path(tmp) / (src.name + ".o")
        cmd = [cc] + common + sanitize + strict + extra + ["-c", str(src), "-o", str(obj)]
        result = subprocess.run(cmd, capture_output=True, text=True)
        if result.returncode != 0:
            print(" ".join(shlex.quote(c) for c in cmd), file=sys.stderr)
            print(result.stdout + result.stderr, file=sys.stderr)
            return None
        return obj

    for src in upstream:
        obj = compile_one(src, upstream_only)
        if obj is None:
            failed = True
        else:
            objects.append(obj)

    for src in port:
        obj = compile_one(src, [])
        if obj is None:
            failed = True
        else:
            objects.append(obj)

    obj = compile_one(harness, [])
    if obj is None:
        failed = True
    else:
        objects.append(obj)

    if failed:
        print("compilation failed", file=sys.stderr)
        return 1

    binary = Path(tmp) / "test_freetype_host"
    link = [cc, "-Wl,--wrap=FT_Render_Glyph"] + common + sanitize + [str(o) for o in objects] + ["-o", str(binary)]
    result = subprocess.run(link, capture_output=True, text=True)
    if result.returncode != 0:
        print(" ".join(shlex.quote(c) for c in link), file=sys.stderr)
        print(result.stdout + result.stderr, file=sys.stderr)
        return 1

    print(f"host cc: {subprocess.run([cc, '--version'], capture_output=True, text=True).stdout.splitlines()[0]}")
    print(f"flags  : {' '.join(common + sanitize)}")
    print(f"sources: {len(upstream)} upstream + {len(port)} port + harness")

    env = dict(os.environ)
    # The leak checker is on by default under ASan; the harness's own fixture
    # buffers are freed, and anything the engine keeps is a genuine finding.
    env.setdefault("ASAN_OPTIONS", "detect_leaks=1:abort_on_error=0")
    env.setdefault("UBSAN_OPTIONS", "print_stacktrace=1")

    run = subprocess.run([str(binary), str(FIXTURES)], env=env)
    if args.keep:
        print(f"build directory: {tmp}")
    return run.returncode


if __name__ == "__main__":
    sys.exit(main())
