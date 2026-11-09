#!/usr/bin/env python3
"""Check explicit-input accounting by breaking temporary libhtml copies."""
import pathlib
import shutil
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
UPDATE = "d->form_inputs = d->form_inputs - form_input + d_form_input(e);"
MUTANTS = [
    ("parser-count", "tree.c", "p->d->form_inputs += d_form_input(n);", ""),
    ("fragment-copy-count", "fragment.c", "p->d->form_inputs += d_form_input(n);", ""),
    ("fragment-transfer-count", "core.c", "to->form_inputs += from->form_inputs;", ""),
    ("clone-count", "dom.c", "d->form_inputs += d_form_input(n);", ""),
    ("batch-count", "dom.c", "e->attrs = prospective.attrs;\n    " + UPDATE,
     "e->attrs = prospective.attrs;\n    (void)form_input;"),
    ("single-set-count", "dom.c", "*slot = fresh;\n    " + UPDATE,
     "*slot = fresh;\n    (void)form_input;"),
    ("single-remove-count", "dom.c", "*slot = old->next;\n    " + UPDATE,
     "*slot = old->next;\n    (void)form_input;"),
]

def compile_driver(library, output):
    command = ["cc", "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
               "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-pthread"]
    command += ["-I" + str(ROOT / path) for path in
                ["userland/libhtml/include", "userland/libos64/include", "abi/include"]]
    command += [str(library / name) for name in
                ["core.c", "encoding.c", "tokenizer.c", "tree.c", "dom.c", "fragment.c", "serialize.c"]]
    command += [str(ROOT / "tools/test_html_dom_host.c"), "-o", str(output)]
    return subprocess.run(command, capture_output=True, text=True, timeout=90)

def run_driver(driver):
    return subprocess.run([str(driver), "--form-input-count"], capture_output=True,
                          text=True, timeout=60)

def main():
    with tempfile.TemporaryDirectory(prefix="html-form-count-mutants-") as directory:
        directory = pathlib.Path(directory)
        baseline = directory / "baseline"
        built = compile_driver(ROOT / "userland/libhtml", baseline)
        if built.returncode:
            print(built.stderr)
            return 1
        checked = run_driver(baseline)
        if checked.returncode:
            print("BASELINE FAILED\n" + checked.stdout + checked.stderr)
            return 1
        caught = compiled = 0
        for name, file, old, new in MUTANTS:
            library = directory / name
            shutil.copytree(ROOT / "userland/libhtml", library)
            source = library / file
            text = source.read_text()
            if text.count(old) != 1:
                raise RuntimeError(f"{name}: anchor occurs {text.count(old)} times")
            source.write_text(text.replace(old, new))
            driver = directory / (name + "-driver")
            built = compile_driver(library, driver)
            if built.returncode:
                print(f"{name}: DID NOT BUILD\n{built.stderr}", flush=True)
                continue
            compiled += 1
            try:
                checked = run_driver(driver)
            except subprocess.TimeoutExpired:
                print(f"{name}: TIMEOUT (not counted)", flush=True)
                continue
            if checked.returncode:
                caught += 1
                print(f"{name}: CAUGHT", flush=True)
            else:
                print(f"{name}: SURVIVED", flush=True)
        print(f"HTML form-count mutants: {compiled} compiled, {caught} caught, {len(MUTANTS)} proposed")
        return 0 if caught == compiled == len(MUTANTS) else 1

if __name__ == "__main__":
    raise SystemExit(main())
