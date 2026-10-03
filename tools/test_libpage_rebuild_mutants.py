#!/usr/bin/env python3
"""Check D3 rules by breaking temporary library copies, never the worktree."""
import argparse
import pathlib
import shutil
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
MUTANTS = [
    ("activation-stale", [("activate.c", "if (p_stale(page))", "if (false)")]),
    ("person-stale", [("core.c", "if (p_stale(page))", "if (false)")]),
    ("snapshot-pin", [
        ("core.c", "page->pin = os64_html_pin(doc);", "page->pin = 0;"),
        ("core.c", "if (page->pin == 0)", "if (false)"),
    ]),
    ("snapshot-unpin", [("core.c", "os64_html_unpin(page->doc, page->pin);", "(void)page;")]),
    ("rebuild-incomplete", [("core.c", "if (page != NULL && page->incomplete)", "if (false)")]),
    ("normalization", [("core.c", "if (!page->incomplete && !p_state_normalize(page))", "if (false)")]),
    ("value-modes", [("state.c", "bool attribute = p_value_is_attribute(element, input);",
                      "bool attribute = p_value_is_attribute(element, input) && false;")]),
    ("script-disabled", [("state.c", "if (input == OS64_PAGE_INPUT_FILE && len != 0)",
        "if (p_has_attr(node, \"disabled\")) return -OS64_PAGE_REASON_DISABLED;\n"
        "    if (input == OS64_PAGE_INPUT_FILE && len != 0)")]),
    ("state-generation", [("state.c", "if (++state->version == 0)", "if (state->version == 0)")]),
    ("publish-all-models", [("state.c", "page = page->state_next)", "page = NULL)")]),
    ("option-node-identity", [("state.c",
        "p_state_find(state, at)->selected = i == index ? 1 : 0;",
        "p_state_find(state, node)->selected = i == index ? 1 : 0;")]),
    ("normalization-commit-order", [("state.c",
        "!p_reserve_node(&reserve, c->node))\n                goto no_memory;",
        "!p_reserve_node(&reserve, c->node))\n                goto no_memory;\n"
        "            p_reserve_commit(&reserve);")]),
    ("current-type-sanitizer", [("state.c",
        "if (edit == NULL || edit->text == NULL || edit->text_version == version)",
        "if (edit == NULL || edit->text == NULL || edit->text_version == version || true)")]),
    ("observed-file-clears-value", [("state.c",
        "p_sanitize_value(&temporary, node, element, input,\n        edit->text, edit->text_len, &len)",
        "p_sanitize_value(&temporary, node, element,\n        input == OS64_PAGE_INPUT_FILE ? OS64_PAGE_INPUT_TEXT : input,\n        edit->text, edit->text_len, &len)")]),
    ("script-user-length-origin", [("state.c", "edit->text_user = user;", "edit->text_user = true;")]),
    ("same-value-origin-transition", [("state.c",
        "bool origin_changed = edit->text_user != user;\n            edit->text_user = user;",
        "bool origin_changed = edit->text_user != user;\n            (void)user;")]),
    ("first-touch-copies-live-table", [("state.c", "if (cap != previous_cap)",
        "if (cap != previous_cap || reserve->table == NULL)")]),
    ("dense-normalized-defaults", [("state.c",
        "c->checked != p_has_attr(c->node, \"checked\")) &&", "true) &&"),
        ("state.c", "c->options[o].selected != p_has_attr(c->options[o].node, \"selected\")) &&", "true) &&")]),
    ("equal-assignment-stale-version", [("state.c",
        "edit->text_clean = false;\n            edit->text_version = os64_html_version(state->doc);",
        "edit->text_clean = false;")]),
    ("options-descend-into-option", [("core.c", "p_is(node, OS64_HTML_TAG_OPTION) ||", "false ||")]),
    ("options-descend-into-select", [("core.c", "p_is(node, OS64_HTML_TAG_SELECT) ||", "false ||")]),
    ("options-descend-into-datalist", [("core.c", "p_is(node, OS64_HTML_TAG_DATALIST);", "false;")]),
    ("options-descend-into-hr", [("core.c", "p_is(node, OS64_HTML_TAG_HR) ||", "false ||")]),
    ("options-descend-into-nested-group", [("core.c", "if (p_is(at, OS64_HTML_TAG_OPTGROUP))", "if (false)")]),
    ("options-ignore-generic-containers", [("core.c", "if (!skip && node->first_child != NULL)",
        "if (!skip && p_is(node, OS64_HTML_TAG_OPTGROUP) && node->first_child != NULL)")]),
    ("sparse-default-old-model-publication", [("core.c",
        "c->checked = p_has_attr(c->node, \"checked\");", "c->checked = initial->checked;")]),
]


def compile_driver(lib, output):
    includes = [ROOT / "userland/libpage/include", ROOT / "userland/libhtml/include",
                ROOT / "userland/libos64/include", ROOT / "abi/include", ROOT / "tools",
                ROOT / "userland/libpage/upstream/ryu"]
    sources = [ROOT / "tools/test_libpage_host.c", ROOT / "userland/apps/wend/render.c"]
    sources += [lib / name for name in
                ["core.c", "state.c", "resolve.c", "value.c", "number.c", "range.c",
                 "upstream/ryu/ryu/d2s.c", "submit.c", "encode.c", "refresh.c", "activate.c"]]
    sources += [ROOT / "userland/libhtml" / name for name in
                ["core.c", "encoding.c", "tokenizer.c", "tree.c", "dom.c", "fragment.c", "serialize.c"]]
    sources += [ROOT / "userland/libos64" / name for name in
                ["str.c", "bidi.c", "url.c", "fmt.c"]]
    command = ["cc", "-std=c11", "-g", "-O1", "-Wall", "-Wextra", "-Werror",
               "-fsanitize=address,undefined", "-fno-sanitize-recover=all"]
    command += ["-I" + str(path) for path in includes]
    command += [str(path) for path in sources] + ["-o", str(output)]
    return subprocess.run(command, capture_output=True, text=True, timeout=90)


def run_driver(driver):
    try:
        result = subprocess.run([str(driver), "--rebuild"], capture_output=True,
                                text=True, timeout=60)
        return result.returncode, result.stdout + result.stderr
    except subprocess.TimeoutExpired:
        return 124, "focused test exceeded its time bound"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--only", action="append", choices=[name for name, _ in MUTANTS])
    parser.add_argument("--list", action="store_true")
    args = parser.parse_args()
    if args.list:
        for name, _ in MUTANTS:
            print(name)
        return 0
    selected = [(name, edits) for name, edits in MUTANTS if not args.only or name in args.only]
    with tempfile.TemporaryDirectory(prefix="libpage-rebuild-mutants-") as work:
        work = pathlib.Path(work)
        baseline = work / "baseline"
        build = compile_driver(ROOT / "userland/libpage", baseline)
        if build.returncode:
            print(build.stderr)
            return 1
        status, output = run_driver(baseline)
        if status:
            print("BASELINE FAILED\n" + output)
            return 1
        caught = built = 0
        for name, edits in selected:
            lib = work / name
            shutil.copytree(ROOT / "userland/libpage", lib)
            for filename, old, new in edits:
                path = lib / filename
                source = path.read_text()
                if old not in source:
                    raise RuntimeError(f"{name}: mutation anchor absent in {filename}")
                path.write_text(source.replace(old, new))
            driver = work / (name + "-driver")
            build = compile_driver(lib, driver)
            if build.returncode:
                print(f"{name}: DID NOT BUILD\n{build.stderr}")
                continue
            built += 1
            status, output = run_driver(driver)
            if status:
                caught += 1
                print(f"{name}: CAUGHT (exit {status})", flush=True)
            else:
                print(f"{name}: MISSED\n{output}", flush=True)
        print(f"D3 mutants: {built} built, {caught} caught, {len(selected)} proposed")
        return 0 if caught == built == len(selected) else 1


if __name__ == "__main__":
    raise SystemExit(main())
