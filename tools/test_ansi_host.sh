#!/bin/bash
# Drive the terminal's escape reader on the host, under ASan and UBSan.
#
# Every case is a script of bytes and the actions it must produce. There is
# no external reference implementation to diff against the way http.client
# serves the HTTP parser, so the expectations here are written by hand — and
# each one names the behaviour it pins rather than merely asserting it, so a
# future change that breaks a case has to argue with a sentence.

set -eu
cd "$(git rev-parse --show-toplevel)"

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

cc -std=c11 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
   -I kernel/include -I abi/include \
   kernel/src/ansi.c tools/test_ansi_host.c -o "$work/test_ansi"

python3 - "$work" <<'PY'
import pathlib
import subprocess
import sys

work = pathlib.Path(sys.argv[1])
exe = str(work / "test_ansi")
failures = 0


def check(script, want, why):
    """`script` in the harness's escaping; `want` the action lines it must
    produce, in order."""
    global failures
    done = subprocess.run([exe, script], capture_output=True, text=True)
    if done.returncode != 0:
        raise SystemExit(f"harness failed ({done.returncode}): {done.stderr!r}")
    got = [l for l in done.stdout.split("\n") if l]
    if got != want:
        failures += 1
        print(f"FAIL {script!r} ({why})\n  got  {got}\n  want {want}")


def text(s):
    return [f"print {ord(c):02x}" for c in s]


# ── Ordinary text is untouched ──────────────────────────────────────────
check("hi", text("hi"), "plain bytes pass through")
check("a\\nb", text("a\nb"), "control bytes are ordinary bytes to this parser")

# ── SGR ─────────────────────────────────────────────────────────────────
check("\\e[31m", ["sgr 31"], "one parameter")
check("\\e[0m", ["sgr 0"], "reset")
check("\\e[m", ["sgr"], "no parameter at all is legal and means reset")
check("\\e[1;31;44mX", ["sgr 1 31 44"] + text("X"), "a run of parameters, then text")
check("\\e[;31m", ["sgr 0 31"], "an omitted parameter is zero, not skipped")
check("A\\e[32mB", text("A") + ["sgr 32"] + text("B"), "text either side")

# ── Cursor position, erase ──────────────────────────────────────────────
check("\\e[10;20H", ["cup 10 20"], "CUP")
check("\\e[10;20f", ["cup 10 20"], "'f' is CUP's other spelling, from 1979")
check("\\e[H", ["cup"], "no parameters: home, decided by the caller")
check("\\e[2J", ["ed 2"], "erase display")
check("\\e[K", ["el"], "erase line, default mode")

# ── OSC 11: the terminal's own background ───────────────────────────────
check("\\e]11;#001122\\a", ["bg 001122"], "OSC ended by BEL")
check("\\e]11;#001122\\e\\\\", ["bg 001122"], "OSC ended by ST (ESC backslash)")
check("\\e]11;#f0f\\a", ["bg ff00ff"], "#rgb doubles each digit")
check("\\e]11;rgb:00/11/22\\a", [], "a spelling this does not read is ignored, not guessed")
check("\\e]12;#001122\\a", [], "OSC 12 is somebody else's business")

# ── What must NOT reach the screen ──────────────────────────────────────
# A terminal that prints the bytes of a sequence it does not understand
# spills parameters across the page; every one of these is consumed whole.
check("\\e[38;5;208m", ["sgr 38 5 208"], "256-colour SGR parses; the CALLER ignores it")
check("\\e[?25l", [], "a DEC private sequence is consumed, not printed")
# A COLON IS A PARAMETER BYTE. ECMA-48 gives 0x30-0x3F to parameters, and
# true-colour SGR spells itself with colons — a parser that ends the sequence
# at the first colon prints the rest of it on the screen, which is exactly
# what happened before this range was written the spec's way.
check("\\e[38:2::255:0:0mX", text("X"), "colon sub-parameters are swallowed whole")
check("\\e[1:32mX", text("X"), "a colon where a semicolon was meant eats the sequence, not the screen")
check("\\e[<0;1;2mX", text("X"), "a private-parameter sequence is swallowed whole")
check("\\e[6n", [], "a query with no answer here is still consumed")
check("\\e7", [], "a two-byte escape is consumed")
check("\\e(B", [], "a charset selection is consumed whole, including its final byte")
check("\\e[999999999m", [], "a parameter past any sane value discards the sequence")
check("\\e[1;2;3;4;5;6;7;8;9;10;11;12;13;14;15;16;17m", [],
      "more parameters than the parser holds discards the sequence")

# ── Recovery: a half-arrived sequence must not eat the screen ───────────
check("\\e[31", ["pending"], "an unfinished sequence waits, printing nothing")
check("\\e[31\\nX", text("\nX"),
      "a control byte mid-sequence is EXECUTED and the sequence abandoned")
check("\\e[31\\e[32mX", ["sgr 32"] + text("X"),
      "an ESC mid-sequence starts a new one — how a truncated sequence recovers")
check("\\e]11;#00112", ["pending"], "an unterminated OSC waits rather than acting")
check("\\e]11;" + "#" * 200 + "\\a", [], "an OSC string longer than the buffer is discarded")
# AN OSC THAT NEVER ENDS MUST GIVE THE TERMINAL BACK. A missing BEL — a
# program that forgot one, or a `\a` written in a vocabulary that has no `\a`
# — would otherwise swallow every byte printed afterwards, forever, which is
# a terminal you have to reboot to recover.
#
# The PROPERTY is what is pinned, not the byte count: some bounded number of
# bytes is swallowed and then printing resumes. Asserting the exact count
# would be asserting the buffer size, which is free to change.
done_run = subprocess.run([exe, "\\e]11;" + "x" * 400 + "BACK"],
                          capture_output=True, text=True)
if done_run.returncode != 0:
    raise SystemExit(f"harness failed ({done_run.returncode}): {done_run.stderr!r}")
lines = [l for l in done_run.stdout.split("\n") if l]
swallowed = 400 - sum(1 for l in lines if l == "print 78")
if lines[-4:] != ["print 42", "print 41", "print 43", "print 4b"]:
    failures += 1
    print(f"FAIL runaway OSC: printing did not resume: {lines[-6:]}")
elif not (64 <= swallowed <= 300):
    failures += 1
    print(f"FAIL runaway OSC: swallowed {swallowed} bytes — bounded, but not sanely")

if failures:
    raise SystemExit(1)
PY

# Concatenate a corpus into one run of the byte-fed parser so state left by
# one sequence is tested by the next without restarting the harness.
python3 - "$work" <<'PY'
import pathlib, subprocess, sys
work = pathlib.Path(sys.argv[1]); exe = str(work / "test_ansi")
corpus = ("A\\e[31mB\\e[0m\\e[10;20H\\e[2J\\e]11;#010203\\a"
          "C\\e[?25l\\e[38;5;9mD\\e[31\\e[32mE")
want = ["print 41", "sgr 31", "print 42", "sgr 0", "cup 10 20", "ed 2",
        "bg 010203", "print 43", "sgr 38 5 9", "print 44", "sgr 32", "print 45"]
done = subprocess.run([exe, corpus], capture_output=True, text=True)
if done.returncode != 0:
    raise SystemExit(f"harness failed ({done.returncode}): {done.stderr!r}")
got = [l for l in done.stdout.split("\n") if l]
if got != want:
    print(f"FAIL corpus\n  got  {got}\n  want {want}")
    raise SystemExit(1)
PY

# Run the tty/selection and framebuffer consumers as well as the parser.
# Link-time section collection drops unrelated kernel entry points. The tty
# fixture records renderer calls; the margin fixture paints into RAM. Neither
# invokes privileged entry points or models SMP interleavings.
for fixture in tty_ansi ansi_margins; do
    cc -std=gnu11 -D_POSIX_C_SOURCE=200809L -g -O1 -fno-builtin -masm=intel \
       -ffunction-sections -fdata-sections -Wall -Wextra -Werror \
       -Wno-unused-function -fsanitize=address,undefined \
       -I kernel/include -I kernel/include/memory -I kernel/include/driver/system \
       -I kernel/include/strings -I abi/include -Wl,--gc-sections \
       "tools/test_${fixture}_host.c" kernel/src/ansi.c -o "$work/test_$fixture"
    "$work/test_$fixture"
done

# Capture the real husk prompt builder's writes, then feed them through the
# parser to check that length limiting cannot strand a partial escape.
cc -std=gnu11 -D_POSIX_C_SOURCE=200809L -g -O1 -fno-builtin -masm=intel \
   -ffunction-sections -fdata-sections -Wall -Wextra -Werror \
   -Wno-unused-function -fsanitize=address,undefined \
   -I userland/libos64/include -I abi/include -I kernel/include \
   -Wl,--gc-sections tools/test_husk_prompt_host.c kernel/src/ansi.c \
   -o "$work/test_husk_prompt"
"$work/test_husk_prompt"

echo "test_ansi_host: all checks passed"
