#!/usr/bin/env python3
"""Run F0 fixture tests and compile the candidate headers for the target ABI."""
import argparse
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def run(output):
    output.mkdir(parents=True, exist_ok=True)
    includes = ["-I" + str(ROOT / p) for p in
                ("userland/libos64/include", "abi/include", "tools/fonts")]
    common = ["-std=c11", "-O2", "-g", "-Wall", "-Wextra", "-Werror", "-fno-builtin"]
    sources = [str(ROOT / "tools/fonts" / p) for p in ("fake_backend.c", "contract_test.c")]
    command = shlex.split(os.environ.get("CC", "cc")) + common + includes + sources + [
        "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-fno-omit-frame-pointer",
        "-fno-pie", "-no-pie", "-o", str(output / "contracts")]
    print(shlex.join(command), flush=True)
    subprocess.run(command, check=True)
    subprocess.run([str(output / "contracts")], check=True)
    with (output / "metrics.json").open("w") as f:
        subprocess.run([str(output / "contracts"), "--dump"], stdout=f, check=True)
    subprocess.run(["python3", str(ROOT / "tools/fonts/check_fixtures.py"),
                    str(output / "metrics.json")], check=True)
    cross = shlex.split(os.environ.get("CROSS_CC", "x86_64-elf-gcc"))
    flags = common + includes + ["-ffreestanding", "-fPIC", "-mno-red-zone", "-msse2",
                                "-masm=intel", "-fno-stack-protector", "-fcf-protection=none"]
    subprocess.run(cross + flags + ["-c", sources[0], "-o", str(output / "fake_backend.o")], check=True)
    for header in ("font_backend", "text", "text_draw"):
        smoke = output / (header + "_header.c")
        smoke.write_text(f'#include "os64/{header}.h"\n'
                         '_Static_assert(sizeof(os64_font_pos_t) == 4, "26.6 ABI");\n')
        subprocess.run(cross + flags + ["-c", str(smoke), "-o",
                                        str(output / (header + "_header.o"))], check=True)
    print("PASS: strict freestanding target compilation (headers and fake backend; no guest claim)")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    if args.output:
        run(args.output.resolve())
    else:
        with tempfile.TemporaryDirectory(prefix="os64-font-contracts-") as directory:
            run(Path(directory))
