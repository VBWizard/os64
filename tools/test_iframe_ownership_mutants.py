#!/usr/bin/env python3
"""F.2a negative proofs using isolated source files and bounded child logs."""
import os
from pathlib import Path
import resource
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent
LIMIT = 8 * 1024 * 1024
FLAGS = ["-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-pthread",
         "-DHTML_RECLAIM_TEST", "-fsanitize=address,undefined", "-fno-sanitize-recover=all"]
INCLUDES = ["userland/libhtml/include", "userland/libpage/include", "userland/libos64/include",
            "abi/include", "tools", "userland/libpage/upstream/ryu"]
HTML = ["userland/libhtml/" + name + ".c" for name in
        ["core", "encoding", "tokenizer", "tree", "dom", "fragment", "serialize", "shadow", "range"]]
PAGE = ["userland/libpage/" + name + ".c" for name in
        ["core", "state", "resolve", "value", "number", "range", "submit", "encode", "refresh", "activate"]]
PAGE += ["userland/libpage/upstream/ryu/ryu/d2s.c", "userland/apps/wend/render.c"]
PAGE += ["userland/libos64/" + name + ".c" for name in ["str", "bidi", "url", "fmt"]]
MUTANTS = [
    ("creation-realm-overwritten", "html", "userland/libhtml/dom.c",
     "h_meta(n)->document = document_at(node, n, document);",
     "h_meta(n)->document = document_at(node, n, document); h_meta(n)->creation_realm = to->creation_realm;"),
    ("logical-document-not-moved", "html", "userland/libhtml/dom.c",
     "h_meta(n)->document = document_at(node, n, document);", "(void)document;"),
    ("physical-node-owner-lost", "html", "userland/libhtml/dom.c",
     "d = h_meta(n)->allocation_owner;", "d = d_node_document_owner(n);"),
    ("range-registry-not-moved", "html", "userland/libhtml/dom.c",
     "if (from != to) d_ranges_adopted(from, to);", "if (false) d_ranges_adopted(from, to);"),
    ("template-document-not-moved", "html", "userland/libhtml/dom.c",
     "return parent->template_contents == n ? template_document(owner) : owner;",
     "return parent->template_contents == n ? owner : owner;"),
    ("sibling-pin-ignored", "html", "userland/libhtml/core.c",
     "bool d_pinned_below(const HDoc *d, uint64_t epoch)\n{\n"
     "    for (const HDoc *m = d->group ? d->group->first : d; m;\n"
     "         m = d->group ? m->group_next : NULL)",
     "bool d_pinned_below(const HDoc *d, uint64_t epoch)\n{\n"
     "    for (const HDoc *m = d; m; m = NULL)"),
    ("state-stays-in-source", "page", "userland/libpage/state.c",
     "os64_page_state_t *owner = os64_html_owns_node(to->doc, r->node) ? to : from;",
     "os64_page_state_t *owner = from;"),
    ("source-snapshot-freed-early", "page", "userland/libpage/state.c",
     "if (block->data.shared && g && group_borrowed(g))",
     "if (false && block->data.shared && g && group_borrowed(g))"),
    ("aggregate-state-cap-ignored", "page", "userland/libpage/state.c",
     "(state->group && total > state->group->limit - state->group->bytes)",
     "(false && state->group && total > state->group->limit - state->group->bytes)"),
]


def child_limits():
    resource.setrlimit(resource.RLIMIT_FSIZE, (LIMIT, LIMIT))
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))


def run(command, log, timeout=120):
    env = dict(os.environ)
    env["ASAN_OPTIONS"] = "detect_leaks=1"
    with log.open("wb") as stream:
        result = subprocess.run(command, cwd=ROOT, stdout=stream, stderr=subprocess.STDOUT,
                                env=env, timeout=timeout, preexec_fn=child_limits)
    # Deliberately bounded even for a mutant producing repeated diagnostics.
    with log.open("rb") as stream:
        excerpt = stream.read(65536).decode("utf-8", errors="replace")
    return result.returncode, excerpt


def compile_source(source, output, original, log):
    command = ["cc"] + FLAGS + ["-I" + str(ROOT / path) for path in INCLUDES]
    command += ["-I" + str((ROOT / original).parent), "-c", str(source), "-o", str(output)]
    status, excerpt = run(command, log)
    if status:
        raise RuntimeError("compile failed: " + original + "\n" + excerpt)


def main():
    with tempfile.TemporaryDirectory(prefix="iframe-ownership-mutants-") as work:
        work = Path(work)
        suites = {"html": HTML + ["tools/test_html_dom_host.c"],
                  "page": HTML + PAGE + ["tools/test_libpage_host.c"]}
        objects = {}
        for index, source in enumerate(dict.fromkeys(suites["html"] + suites["page"])):
            obj = work / (str(index) + ".o")
            compile_source(ROOT / source, obj, source, work / "compile.log")
            objects[source] = obj
        def link(suite, executable, changed=None):
            paths = [changed[1] if changed and path == changed[0] else objects[path] for path in suites[suite]]
            status, excerpt = run(["cc"] + FLAGS + list(map(str, paths)) + ["-o", str(executable)], work / "link.log")
            if status:
                raise RuntimeError("link failed\n" + excerpt)
        for suite in suites:
            driver = work / (suite + "-baseline")
            link(suite, driver)
            status, excerpt = run([str(driver), "--ownership"], work / (suite + ".log"))
            if status:
                raise RuntimeError("baseline failed\n" + excerpt)
            print(suite + " baseline: PASS", flush=True)
        caught = 0
        for name, suite, filename, old, new in MUTANTS:
            source = (ROOT / filename).read_text()
            if source.count(old) != 1:
                raise RuntimeError(name + ": mutation anchor is not unique")
            modified = work / (name + ".c")
            modified.write_text(source.replace(old, new))
            obj, driver = work / (name + ".o"), work / name
            compile_source(modified, obj, filename, work / "compile.log")
            link(suite, driver, (filename, obj))
            status, excerpt = run([str(driver), "--ownership"], work / (name + ".log"))
            # Timeout, file-size signal and arbitrary crashes are not evidence
            # that an assertion or sanitizer recognized the injected defect.
            recognized = status != 0 and any(mark in excerpt for mark in
                ["FAIL", "ERROR: AddressSanitizer", "runtime error:", "libhtml: "])
            if not recognized:
                print(name + ": NOT CAUGHT\n" + excerpt[:4096], flush=True)
                return 1
            caught += 1
            print(name + ": CAUGHT", flush=True)
        print(f"Ownership mutants: {caught}/{len(MUTANTS)} compiled and caught", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
