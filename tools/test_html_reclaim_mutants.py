#!/usr/bin/env python3
"""Break one D6 lifetime rule per isolated copy and require host tests to catch it."""
import argparse
import pathlib
import shutil
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
MUTANTS = [
    ("external-hold", "dom.c", "if (h_meta(n)->holds || (h_meta(n)->flags & H_NODE_PARSER))", "if (h_meta(n)->flags & H_NODE_PARSER)"),
    ("parser-hold", "dom.c", "if (h_meta(n)->holds || (h_meta(n)->flags & H_NODE_PARSER))", "if (h_meta(n)->holds)"),
    ("parser-head-ref", "dom.c", "    parser_mark(d, p->head);", "    /* mutated parser head reference */"),
    ("parser-form-ref", "dom.c", "    parser_mark(d, p->form);", "    /* mutated parser form reference */"),
    ("counted-holders", "core.c", "m->holds++;", "m->holds = 1;"),
    ("delayed-stamp", "dom.c", "m->stamp = d->version + 1;", "m->stamp = d->version;"),
    ("pin-equality", "dom.c", "d->pins[i] < stamp", "d->pins[i] <= stamp"),
    ("pin-protection", "dom.c", "if (!older_pin(d, m->stamp))", "if (true || !older_pin(d, m->stamp))"),
    ("free-list", "core.c", "HNode *n = d->free_nodes;", "HNode *n = NULL;"),
    ("form-record-count", "dom.c", "        d->records--;", "        /* mutated record subtraction */"),
    ("explicit-input-count", "dom.c", "        d->form_inputs--;", "        /* mutated input subtraction */"),
    ("packed-payload", "dom.c", "    if (m->flags & H_NODE_PACKED)\n        h_free(d, n);", "    if (m->flags & H_NODE_PACKED)\n        (void)n;"),
    ("private-attributes", "dom.c", "if (n->kind == ELEMENT && (*h_word(n) & H_ATTRS_PRIVATE))", "if (false && n->kind == ELEMENT && (*h_word(n) & H_ATTRS_PRIVATE))"),
    ("node-count", "dom.c", "    d->pub.node_count--;", "    /* mutated node subtraction */"),
    ("packed-text", "dom.c", "if ((n->kind == TEXT || n->kind == COMMENT) && *h_word(n))", "if (false && (n->kind == TEXT || n->kind == COMMENT) && *h_word(n))"),
    ("empty-container", "dom.c", "        d_detached(d, node);\n        h_meta(node)->flags &= ~H_NODE_FRESH;", "        /* mutated empty-container retirement */\n        h_meta(node)->flags &= ~H_NODE_FRESH;"),
]


def build(library, output):
    command = ["cc", "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-pthread",
               "-fsanitize=address,undefined", "-fno-sanitize-recover=all"]
    command += ["-I" + str(ROOT / path) for path in
                ["userland/libhtml/include", "userland/libos64/include", "abi/include"]]
    command += [str(library / name) for name in
                ["core.c", "encoding.c", "tokenizer.c", "tree.c", "dom.c", "fragment.c", "serialize.c"]]
    command += [str(ROOT / "tools/test_html_dom_host.c"), "-o", str(output)]
    return subprocess.run(command, capture_output=True, text=True, timeout=90)


def run(driver):
    return subprocess.run([str(driver), "--reclaim-rules"], capture_output=True, text=True, timeout=60)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--only", help="Run names containing this text")
    args = parser.parse_args()
    selected = [m for m in MUTANTS if not args.only or args.only in m[0]]
    if not selected:
        parser.error("no matching mutants")
    with tempfile.TemporaryDirectory(prefix="html-reclaim-mutants-") as temporary:
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
                evidence = next((line for line in result.stderr.splitlines() if line.startswith("FAIL ")
                                 or "ERROR: AddressSanitizer" in line), result.stderr.splitlines()[0]
                                if result.stderr else f"exit {result.returncode}")
                print(f"{name}: CAUGHT ({evidence})", flush=True)
            else:
                print(f"{name}: SURVIVED", flush=True)
        print(f"HTML reclamation mutants: {compiled} compiled, {caught} caught, {len(selected)} proposed")
        return 0 if caught == compiled == len(selected) else 1


if __name__ == "__main__":
    raise SystemExit(main())
