#!/usr/bin/env python3
"""Replay private incarnation negative controls against native and target objects.

Deliberately broken variants must reach the named behavioral oracle. Their
failures are sensitivity evidence, not successful sanitizer/leak checks.
Run with ASAN_OPTIONS=detect_leaks=1 in an environment supporting LSan.
"""
import argparse
import json
from pathlib import Path
import shlex
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    source = (root / "userland/libjs/runtime/owner_incarnation.c").read_text()
    # Each replacement must match exactly once, so a source change fails closed.
    variants = {
        "wrap": ("if (previous == UINT64_MAX) return JS_OWNER_EXHAUSTED;", ""),
        "old-number": ("*out = previous + 1;", "*out = previous;"),
        "no-advance": ("&previous, previous + 1,", "&previous, previous,"),
        "reset-global": ("return js_owner_incarnation_reserve(&process_issuer, out);",
                         "__atomic_store_n(&process_issuer._last, 0, __ATOMIC_RELAXED); return js_owner_incarnation_reserve(&process_issuer, out);"),
        "burn-invalid": ("if (!issuer || !out) return JS_OWNER_INVALID;",
                         "if (!issuer) return JS_OWNER_INVALID;\n    if (!out) { __atomic_fetch_add(&issuer->_last, 1, __ATOMIC_RELAXED); return JS_OWNER_INVALID; }"),
    }
    native = ["cc", "-std=c11", "-O2", "-g", "-Wall", "-Wextra", "-Werror",
              "-ffreestanding", "-fno-builtin", "-pthread", "-fno-tree-loop-distribute-patterns",
              "-I" + str(root / "userland/libjs/include"),
              "-I" + str(root / "userland/libjs/runtime")]
    sanitizers = ["-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-Wl,-z,noexecstack"]
    driver = str(root / "tools/test_js_owner_incarnation_host.c")
    obj = str(root / "userland/obj/js/runtime/owner_incarnation.o")
    recipe = subprocess.check_output(["make", "-C", str(root / "userland"), "-nB", obj], text=True)
    commands = [shlex.split(line) for line in recipe.splitlines()
                if " -c libjs/runtime/owner_incarnation.c -o " in line]
    if len(commands) != 1:
        raise RuntimeError("expected one production object compile recipe")
    target = commands[0]
    target = target[:target.index("-c")] + ["-I" + str(root / "userland/libjs/runtime")]
    results = []
    for name, (old, new) in variants.items():
        if source.count(old) != 1:
            raise RuntimeError("mutation no longer matches once: " + name)
        mutated = output / (name + ".c")
        mutated.write_text(source.replace(old, new))
        target_object = output / (name + ".o")
        compile_commands = [
            native + sanitizers + [str(mutated), driver, "-o", str(output / (name + "-native"))],
            target + ["-c", str(mutated), "-o", str(target_object)],
            native + sanitizers + [driver, str(target_object), "-o", str(output / (name + "-target"))],
        ]
        for command in compile_commands:
            run = subprocess.run(command, cwd=root / "userland", capture_output=True, text=True)
            if run.returncode:
                raise RuntimeError("compile failed: " + name + "\n" + run.stderr)
        for mode in ("native", "target"):
            binary = output / (name + "-" + mode)
            run = subprocess.run([str(binary)], capture_output=True, text=True, timeout=30)
            log = run.stdout + run.stderr
            (output / (name + "-" + mode + ".log")).write_text(log)
            # A signal, sanitizer abort, loader error or compile failure cannot
            # stand in for the behavioral refusal oracle.
            if run.returncode != 1 or not log.startswith("FAIL: incarnation"):
                raise RuntimeError("variant escaped named oracle: " + name + "/" + mode + "\n" + log)
            results.append({"variant": name, "mode": mode, "oracle": log.splitlines()[0],
                            "compile_commands": compile_commands})
            print(name + "/" + mode + ": " + log.splitlines()[0], flush=True)
    (output / "results.json").write_text(json.dumps(results, indent=2) + "\n")
    print(str(len(results)) + " negative controls rejected by named behavioral oracles")


if __name__ == "__main__":
    main()
