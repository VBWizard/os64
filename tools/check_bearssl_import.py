#!/usr/bin/env python3
"""Verify the patched import against its pristine per-file SHA-256 manifest."""
import argparse
import hashlib
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
BASE = ROOT / "userland/libtls"


def pristine(destination):
    shutil.copytree(BASE / "upstream", destination)
    for patch in reversed(sorted((BASE / "patches").glob("*.patch"))):
        subprocess.run(["patch", "--batch", "--fuzz=0", "-R", "-p1", "-i", str(patch)],
                       cwd=destination, check=True, stdout=subprocess.PIPE)
    expected = {}
    for line in (BASE / "upstream.sha256").read_text().splitlines():
        digest, name = line.split("  ", 1)
        expected[name] = digest
    actual = {p.relative_to(destination).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
              for p in destination.rglob("*") if p.is_file()}
    if actual != expected:
        different = sorted(k for k in actual.keys() | expected.keys()
                           if actual.get(k) != expected.get(k))
        raise RuntimeError("upstream import mismatch: " + ", ".join(different))
    selected = [word.removeprefix("libtls/upstream/")
                for word in (BASE / "sources.mk").read_text().split()
                if word.startswith("libtls/upstream/")]
    core = sorted(name for name in expected if name.startswith("src/") and name.endswith(".c"))
    if sorted(selected) != core:
        raise RuntimeError("foundation source manifest does not match upstream core")
    subprocess.run(["python3", str(ROOT / "tools/bearssl_vectors.py"), "--check"], check=True)
    print(f"BearSSL import: {len(actual)} pristine files, {len(core)} core sources verified", flush=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pristine", type=Path, help="also retain a pristine copy at a new path")
    args = parser.parse_args()
    if args.pristine:
        pristine(args.pristine)
    else:
        with tempfile.TemporaryDirectory(prefix="bearssl-import-") as work:
            pristine(Path(work) / "upstream")
