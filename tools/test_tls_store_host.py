#!/usr/bin/env python3
"""Test bounded trust-bundle loading and the os64 adapter under ASan/UBSan."""
import argparse
import os
from pathlib import Path
import subprocess
import sys
import tempfile

sys.dont_write_bytecode = True
from check_bearssl_import import ROOT, BASE
from test_bearssl_host import check


def run(archive, work):
    executable = work / "trust-store"
    sources = [BASE / "port" / name for name in (
        "certificate_der.c", "certificate_policy.c", "trust_pem.c", "trust_config.c", "trust_file.c")]
    sources += [BASE / "test" / name for name in ("trust_test.c", "trust_vectors.c")]
    subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-O2", "-g", "-Wall", "-Wextra", "-Werror",
        "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-ffreestanding", "-fno-builtin",
        "-include", str(BASE / "port/config.h"), "-I" + str(BASE / "upstream/inc"),
        "-I" + str(ROOT / "userland/libos64/include"), "-I" + str(ROOT / "abi/include"),
        *map(str, sources), str(ROOT / "userland/libos64/slurp.c"), str(ROOT / "tools/test_tls_store_host.c"),
        str(archive), "-o", str(executable)], check=True)
    subprocess.run([executable], check=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--foundation", type=Path, help="reuse a matching adapted/core.a")
    parser.add_argument("--output", type=Path, help="retain artifacts in a new directory")
    args = parser.parse_args()
    def test(work):
        if not args.foundation:
            check(work)
        run(args.foundation.resolve() if args.foundation else work / "adapted/core.a", work)
    if args.output:
        args.output.mkdir()
        test(args.output.resolve())
    else:
        with tempfile.TemporaryDirectory(prefix="tls-store-") as directory:
            test(Path(directory))
