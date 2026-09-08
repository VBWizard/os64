#!/usr/bin/env python3
"""Exercise the private TLS byte engine against pinned BearSSL fixture servers."""
import argparse
import os
from pathlib import Path
import subprocess
import sys
import tempfile

sys.dont_write_bytecode = True
from check_bearssl_import import ROOT, BASE
from test_bearssl_host import check


def engine(archive, work):
    executable = work / "engine"
    subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-O2", "-g",
        "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
        "-fno-sanitize-recover=all", "-ffreestanding", "-fno-builtin",
        "-include", str(BASE / "port/config.h"),
        "-I" + str(BASE / "upstream/inc"),
        "-I" + str(ROOT / "userland/libos64/include"),
        "-I" + str(ROOT / "abi/include"),
        str(BASE / "port/client_engine.c"), str(BASE / "port/client_profile.c"),
        str(ROOT / "tools/test_tls_engine_host.c"),
        str(ROOT / "tools/tls_engine_fixture_ec.c"),
        str(ROOT / "tools/tls_engine_fixture_rsa.c"),
        str(archive), "-o", str(executable)], check=True)
    subprocess.run([executable], check=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--foundation", type=Path,
        help="reuse an adapted/core.a from a successful host foundation run of this pin/configuration")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="tls-engine-") as directory:
        work = Path(directory)
        if args.foundation:
            archive = args.foundation.resolve()
        else:
            check(work)
            archive = work / "adapted/core.a"
        engine(archive, work)
