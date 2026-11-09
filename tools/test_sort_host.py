#!/usr/bin/env python3
"""Exercise the real sort command through host I/O and allocation adapters."""
import itertools
import os
from pathlib import Path
import random
import subprocess
import sys
import tempfile

exe = str(Path(sys.argv[1]).resolve())
env = dict(os.environ, LC_ALL="C", ASAN_OPTIONS="detect_leaks=1",
           UBSAN_OPTIONS="halt_on_error=1")
checks = 0


def run(args=(), data=b"", extra=None):
    return subprocess.run([exe, *args], input=data, capture_output=True,
                          env=dict(env, **(extra or {})), timeout=30)


def check(args=(), data=b"", expected=None, status=0, extra=None):
    global checks
    got = run(args, data, extra)
    assert got.returncode == status, (args, got.returncode, got.stderr)
    if expected is not None:
        assert got.stdout == expected, (args, got.stdout[:200], expected[:200])
    assert b"Sanitizer" not in got.stderr and b"runtime error:" not in got.stderr, got.stderr
    if status:
        assert b"sort:" in got.stderr, got.stderr
    checks += 1


check(data=b"pear\napple\nbanana\n", expected=b"apple\nbanana\npear\n")
check(data=b"", expected=b"")
check(data=b"\n\nz", expected=b"\n\nz\n")
check(["-nu"], b"02\n2.00\n-0\n0\n", b"-0\n02\n")
check(["-fu"], b"apple\nAPPLE\nBanana\nbanana\n", b"apple\nBanana\n")
check(["-bu"], b" a\na\n\ta\n", b" a\n")
check(["-rnu"], b"2\n02\n1\n", b"2\n1\n")
check(["--reverse", "--unique"], b"b\na\nb\n", b"b\na\n")
check(["--help"])
check(["--bogus"], status=2)

rng = random.Random(64)
records = [b"", b"-0", b".00001", b"-.00002", b"0002.0", b"+3", b"1e9",
           b"0." + b"0" * 100 + b"1", b"9" * 100, b"-" + b"9" * 100,
           b"ABC", b"abc", b"\x00tail", b"\xff", b"\r", b" 2", b"\t-3.20"]
records += [bytes(rng.choice(b"0123456789.-+ abcABC\t\x00\xff")
                  for _ in range(rng.randrange(60))) for _ in range(400)]
data = b"\n".join(records + records[:50])
for bits in itertools.product((False, True), repeat=5):
    flags = "".join(flag for flag, enabled in zip("nrufb", bits) if enabled)
    args = ["-" + flags] if flags else []
    reference = subprocess.run(["sort", *args], input=data, capture_output=True,
                               env=env, check=True).stdout
    check(args, data, reference)

for size in (32767, 32768, 32769, 131072):
    line = b"x" * size
    check(data=line + b"\na", expected=b"a\n" + line + b"\n")
check(data=b"z\nb\na", expected=b"a\nb\nz\n", extra={"SORT_SHORT_IO": "1"})
large = b"\n".join(str(i).encode() for i in range(20000, -1, -1))
check(["-n"], large, b"".join(str(i).encode() + b"\n" for i in range(20001)))

with tempfile.TemporaryDirectory() as directory:
    first, second = Path(directory) / "first", Path(directory) / "-second"
    first.write_bytes(b"z\nb")
    second.write_bytes(b"a\n")
    check([str(first), "-", str(second)], b"c", b"a\nb\nc\nz\n")
    check(["--", str(second)], expected=b"a\n")
    check([str(first), "--bogus"], status=2)
    check([directory], expected=b"", status=1)
    check([str(first) + ".missing"], expected=b"", status=1)
    check([str(first)], expected=b"", status=1, extra={"SORT_FAIL_CLOSE": "1"})

for fault in ("SORT_FAIL_READ", "SORT_FAIL_WRITE", "SORT_ZERO_WRITE"):
    check(data=b"b\na\n", expected=b"", status=1, extra={fault: "1"})
# This input grows the byte buffer, then allocates both line arrays.
for allocation in range(1, 5):
    check(data=b"x" * 32768, expected=b"", status=1,
          extra={"SORT_FAIL_ALLOC": str(allocation)})
print(f"sort: {checks} checks passed (including all 32 flag combinations)")
