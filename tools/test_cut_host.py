#!/usr/bin/env python3
"""Check cut against coreutils and explicit UTF-8 and failure fixtures."""
import os
from pathlib import Path
import random
import subprocess
import sys
import tempfile

candidate = sys.argv[1]
checks = 0


def run(args, data=b"", settings=None, reference=False):
    env = dict(os.environ, LC_ALL="C")
    if settings:
        env.update({key: str(value) for key, value in settings.items()})
    return subprocess.run(["/usr/bin/cut" if reference else candidate, *args],
                          input=data, capture_output=True, env=env, timeout=15)


def check(args, data=b"", expected=None, status=0, settings=None):
    global checks
    result = run(args, data, settings)
    if expected is None:
        ref = run(args, data, reference=True)
        assert ref.returncode == 0, (args, ref.stderr)
        expected = ref.stdout
    assert result.returncode == status, (args, result.returncode, result.stderr)
    assert result.stdout == expected, (args, result.stdout[:200], expected[:200])
    assert b"Sanitizer" not in result.stderr and b"runtime error:" not in result.stderr, result.stderr
    if status:
        assert result.stderr, args
    checks += 1


data = b"alpha beta gamma\n  leading  trailing \nplain\n\nlast two"
for selection in ["1", "2", "1,3", "3,1,1", "1-3,2-4", "-2", "2-", "9-", "1 3"]:
    for mode in ["-b", "-c", "-f"]:
        args = [mode, selection]
        if mode == "-f":
            args += ["-d", " "]
        for extra in [[], ["--complement"], ["--output-delimiter=::"], ["--output-delimiter="], ["--complement", "--output-delimiter=:"]]:
            check(args + extra, data)
            check(args + extra, data, settings={"CUT_READ_CHUNK": 1, "CUT_WRITE_CHUNK": 1})

for delimiter in [" ", "\t", ":", "", "\n"]:
    for extra in [[], ["-s"], ["--complement"], ["-s", "--complement"]]:
        for source in [b"", b"\n\n", b"abc", b"abc\n", b"a:b::c\n\0:tail\r\n", data]:
            check(["-f", "2,4-", "-d", delimiter] + extra, source)

check(["--fields=2", "--delimiter= "], data)
check(["-sf2", "-d "], data)
check(["-b2-4"], b"a\0b\rc\nlast")
check(["-b", "18446744073709551615-"], b"abc\n", b"\n")
check(["-b", "1-18446744073709551615"], b"abc\n", b"abc\n")
check(["-b1,2,4-5", "--output-delimiter=:"], b"abcdef\n")
check(["-b1-2,2-3", "--output-delimiter=:"], b"abcdef\n")
check(["-b1-"], bytes(range(256)) * 4)
check(["-f", "2", "-d", " "], b"a " + b"x" * 200000 + b" z\n")
check(["-b", "32768-65537"], b"q" * 150000 + b"\n", settings={"CUT_READ_CHUNK": 32767, "CUT_WRITE_CHUNK": 13})
check(["-c", "2-3"], "Aé💙Z\n".encode(), "é💙\n".encode())
check(["-c", "2", "--complement"], "Aé💙Z".encode(), "A💙Z\n".encode(), settings={"CUT_READ_CHUNK": 1})
check(["-c", "2-4"], b"a\xff\xc0\x80z\n", b"\xff\xc0\x80\n")
check(["-b", "3", "-n"], "AéZ\n".encode(), "é\n".encode())
check(["-b", "2", "-n"], "AéZ\n".encode(), b"\n")
check(["-b", "7", "-n"], "Aé💙Z\n".encode(), "💙\n".encode(), settings={"CUT_READ_CHUNK": 1})

bad_lists = ["", "0", "1,0", "3-1", "-", "1,,2", ",1", "1,", "1--2", "-0", "2-0", "x", "1x", "1-2-3", "18446744073709551616", "1-18446744073709551616"]
for value in bad_lists:
    check(["-f", value], data, b"", 2)
for args in [[], ["-b1", "-f1"], ["-c1", "-b1"], ["-f1", "-f2"], ["-b1", "-d", " "],
             ["-c1", "-s"], ["-f1", "-n"], ["-f1", "-d", "ab"], ["-b"], ["--unknown"]]:
    check(args, data, b"", 2)
help_result = run(["--help"])
assert help_result.returncode == 0 and b"cut -d ' ' -f 2" in help_result.stdout
checks += 1

with tempfile.TemporaryDirectory() as folder:
    first = Path(folder) / "first"
    second = Path(folder) / "second"
    first.write_bytes(b"a b\n")
    second.write_bytes(b"c d")
    check(["-d", " ", "-f2", str(first), "-", str(second)], b"e f\n")
    check(["-b1", str(first), "--unknown"], expected=b"", status=2)
    check(["-b1", str(first), str(Path(folder) / "missing"), str(second)], expected=b"a\nc\n", status=1)
    check(["-b1", folder], expected=b"", status=1)
    check(["-b1", str(first)], expected=b"", status=1, settings={"CUT_OPEN_FAIL": 1})
    check(["-b1", str(first)], expected=b"a\n", status=1, settings={"CUT_CLOSE_FAIL": 1})
    dash = Path(folder) / "-file"
    dash.write_bytes(b"z\n")
    check(["-b1", "--", str(dash)])

check(["-b1"], data, b"", 1, {"CUT_READ_FAIL": 1})
check(["-b1"], b"first\nsecond", b"f\n", 1, {"CUT_READ_CHUNK": 6, "CUT_READ_FAIL": 2})
check(["-b1-"], b"abcdef\n", b"abc", 1, {"CUT_WRITE_FAIL": 3})
check(["-b1-"], b"x" * 40000 + b"\n", b"xxx", 1, {"CUT_WRITE_FAIL": 3})
check(["-b1"], data, b"", 1, {"CUT_WRITE_ZERO": 1})
for fail_at in [1, 2, 3, 4, 5, 6]:
    check(["-b1"], b"x" * 150000, b"", 1, {"CUT_ALLOC_FAIL": fail_at})
check(["-b1"], b"first\n" + b"x" * 150000, b"f\n", 1, {"CUT_ALLOC_FAIL": 4})

rng = random.Random(0x64C07)
for _ in range(180):
    source = bytes(rng.choice(b"abc :\t\n\r\0\xff") for _ in range(rng.randrange(500)))
    parts = []
    for _ in range(rng.randrange(1, 8)):
        low = rng.randrange(1, 20)
        parts.append(rng.choice([str(low), f"{low}-", f"-{low}", f"{low}-{low + rng.randrange(10)}"]))
    args = [rng.choice(["-b", "-f"]), ",".join(parts)]
    if args[0] == "-f":
        args += ["-d", rng.choice([" ", ":", "\t", ""])]
        if rng.randrange(2): args += ["-s"]
    if rng.randrange(2): args += ["--complement"]
    if rng.randrange(2): args += ["--output-delimiter", rng.choice(["|", "::", ""]) ]
    check(args, source, settings={"CUT_READ_CHUNK": rng.randrange(1, 40), "CUT_WRITE_CHUNK": rng.randrange(1, 15)})

print(f"cut: {checks} checks passed (ASan/UBSan, coreutils comparisons, UTF-8, short I/O and failures)")
