#!/usr/bin/env python3
"""Break one document.write rule (DOM_D9.md) per isolated copy of libhtml and
require the parser's hand cases (test_html_driver.c --checks) to catch it."""
import argparse
import pathlib
import shutil
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
MUTANTS = [
    # The pump reads a running script's text only as far as where it writes next...
    ("pump-insertion-bound", "core.c",
     "size_t written_end = p->running ? p->insertion : p->written_len;",
     "size_t written_end = p->written_len;"),
    # ...and nothing behind that point: not the window's remainder, not the hold.
    ("pump-stops-at-the-writer", "core.c",
     "        } else if (p->running)\n            break;\n        else if (p->parsed",
     "        } else if (p->parsed"),
    ("insertion-advances", "core.c",
     "    p->written_len += n;\n    p->insertion += n;",
     "    p->written_len += n;"),
    # A run's first write goes in at the cursor, not at the end of what waits.
    ("insertion-at-the-cursor", "core.c",
     "        p->insertion = p->written_at;",
     "        p->insertion = p->written_len;"),
    ("resume-ends-the-run", "core.c",
     "        bool found = p->script != p->running;\n        h_ref_set(p, &p->running, NULL);",
     "        bool found = p->script != p->running;"),
    ("resume-written-script-first", "core.c",
     "        if (found)\n            return settle(p);",
     "        (void)found;"),
    ("write-needs-a-stop", "core.c",
     "    if (!p->script || (!utf8 && len))\n        return OS64_HTML_BAD_ARGUMENT;",
     "    if (!utf8 && len)\n        return OS64_HTML_BAD_ARGUMENT;"),
    ("write-is-utf8", "core.c",
     "    if (!d_text_ok(utf8, len))\n        return OS64_HTML_BAD_TEXT;",
     ""),
    ("write-is-counted", "core.c",
     "        d->pub.input_bytes += take;",
     ""),
    ("write-is-cut", "core.c",
     "        take = left;\n        while",
     "        take = len;\n        while"),
    ("write-cut-on-a-character", "core.c",
     "        while (take && ((unsigned char)utf8[take] & 0xc0) == 0x80)\n            take--;",
     ""),
    ("write-moves-the-version", "core.c",
     "        return settle(p);\n    p->moved = false;",
     "        return settle(p);"),
    ("written-freed-when-read", "core.c",
     "    if (p->written_at == p->written_len)\n        written_free(p);",
     ""),
    ("written-freed-at-release", "core.c",
     "    hold_free(p);\n    written_free(p);",
     "    hold_free(p);"),
    ("finish-reads-it-all", "core.c",
     "    h_ref_set(p, &p->script, NULL);\n    h_ref_set(p, &p->running, NULL);\n    h_pump(p);",
     "    h_ref_set(p, &p->script, NULL);\n    h_pump(p);"),
    ("utf8-four-bytes", "core.c",
     "u[0] < 0xf0 ? 3 : 4;",
     "u[0] < 0xf0 ? 3 : 3;"),
]


def build(library, output):
    command = ["cc", "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
               "-fsanitize=address,undefined", "-fno-sanitize-recover=all"]
    command += ["-I" + str(ROOT / path) for path in
                ["userland/libhtml/include", "userland/libos64/include", "abi/include"]]
    command += [str(library / name) for name in
                ["core.c", "encoding.c", "tokenizer.c", "tree.c", "dom.c", "fragment.c", "serialize.c"]]
    command += [str(ROOT / "tools/test_html_driver.c"), "-o", str(output)]
    return subprocess.run(command, capture_output=True, text=True, timeout=180)


def run(driver):
    # No random walks: the hand cases are what each rule answers to. A parse
    # that never ends (a run never closed reads nothing behind it) is caught
    # by the clock.
    try:
        return subprocess.run([str(driver), "--checks", "0"], capture_output=True, text=True, timeout=120)
    except subprocess.TimeoutExpired:
        return subprocess.CompletedProcess(driver, 1, "", "hung past 120 seconds")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--only", help="Run names containing this text")
    args = parser.parse_args()
    selected = [m for m in MUTANTS if not args.only or args.only in m[0]]
    if not selected:
        parser.error("no matching mutants")
    with tempfile.TemporaryDirectory(prefix="html-write-mutants-") as temporary:
        temporary = pathlib.Path(temporary)
        baseline = temporary / "baseline"
        result = build(ROOT / "userland/libhtml", baseline)
        if result.returncode:
            print("BASELINE BUILD FAILED\n" + result.stderr)
            return 1
        result = run(baseline)
        if result.returncode:
            print("BASELINE FAILED\n" + result.stdout + result.stderr)
            return 1
        caught = compiled = 0
        for name, source, before, after in selected:
            library = temporary / name
            shutil.copytree(ROOT / "userland/libhtml", library)
            file = library / source
            contents = file.read_text()
            if contents.count(before) != 1:
                print(f"{name}: expected exactly one mutation site, found {contents.count(before)}")
                return 1
            file.write_text(contents.replace(before, after))
            driver = temporary / (name + ".test")
            result = build(library, driver)
            if result.returncode:
                print(f"{name}: BUILD FAILED\n" + result.stderr)
                return 1
            compiled += 1
            result = run(driver)
            if result.returncode:
                caught += 1
                lines = result.stderr.splitlines()
                evidence = next((line for line in lines if line.startswith("FAIL ")
                                 or "ERROR: AddressSanitizer" in line or "runtime error" in line
                                 or line.startswith("html safety")),
                                lines[0] if lines else f"exit {result.returncode}")
                print(f"{name}: CAUGHT ({evidence})", flush=True)
            else:
                print(f"{name}: SURVIVED", flush=True)
        print(f"HTML write mutants: {compiled} compiled, {caught} caught, {len(selected)} proposed")
        return 0 if caught == compiled == len(selected) else 1


if __name__ == "__main__":
    raise SystemExit(main())
