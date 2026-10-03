#!/usr/bin/env python3
"""Break scoped tree-planning rules in temporary source copies."""
import pathlib
import shutil
import subprocess
import tempfile

from test_libpage_rebuild_mutants import ROOT, compile_driver

MUTANTS = [
    ("whole-document-plan", "PTreeNames names = {0}, ids = {0};",
     "roots[2].node = state->doc->document;\n    PTreeNames names = {0}, ids = {0};"),
    ("radio-peer-scan", "if (!tree_plan_peers(&reserve,", "if (false && !tree_plan_peers(&reserve,"),
    ("explicit-owner-id-scan", "if ((pass == 0 ? ids->count : names->count) == 0) continue;",
     "if (pass == 0 || names->count == 0) continue;"),
    ("old-option-list", "if (!tree_plan_select(reserve, stages, before) ||",
     "if ((false && !tree_plan_select(reserve, stages, before)) ||"),
    ("new-option-list", "!tree_plan_select(reserve, stages, after)) return false;",
     "(false && !tree_plan_select(reserve, stages, after))) return false;"),
    ("stage-link-abort", "stages->original->stage = NULL;", "(void)state;"),
]

def run(driver):
    return subprocess.run([str(driver), "--tree-only"], capture_output=True,
                          text=True, timeout=60)

def main():
    with tempfile.TemporaryDirectory(prefix="libpage-tree-mutants-") as directory:
        directory = pathlib.Path(directory)
        baseline = directory / "baseline"
        built = compile_driver(ROOT / "userland/libpage", baseline)
        if built.returncode:
            print(built.stderr)
            return 1
        checked = run(baseline)
        if checked.returncode:
            print("BASELINE FAILED\n" + checked.stdout + checked.stderr)
            return 1
        caught = compiled = 0
        for name, old, new in MUTANTS:
            library = directory / name
            shutil.copytree(ROOT / "userland/libpage", library)
            source = library / "state.c"
            text = source.read_text()
            if text.count(old) != 1:
                raise RuntimeError(f"{name}: anchor occurs {text.count(old)} times")
            source.write_text(text.replace(old, new))
            driver = directory / (name + "-driver")
            built = compile_driver(library, driver)
            if built.returncode:
                print(f"{name}: DID NOT BUILD\n{built.stderr}")
                continue
            compiled += 1
            try:
                checked = run(driver)
            except subprocess.TimeoutExpired:
                print(f"{name}: TIMEOUT (not counted)", flush=True)
                continue
            if checked.returncode:
                caught += 1
                print(f"{name}: CAUGHT", flush=True)
            else:
                print(f"{name}: SURVIVED", flush=True)
        print(f"Tree mutants: {compiled} compiled, {caught} caught, {len(MUTANTS)} proposed")
        return 0 if caught == compiled == len(MUTANTS) else 1

if __name__ == "__main__":
    raise SystemExit(main())
