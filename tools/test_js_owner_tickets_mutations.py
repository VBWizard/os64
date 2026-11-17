#!/usr/bin/env python3
"""Replay private metadata negative controls against native and target objects.

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
    source = (root / "userland/libjs/runtime/owner_tickets.c").read_text()
    # Each replacement must match exactly once, so a source change fails closed.
    variants = {
        "live-incarnation": (
            "return incarnation == table->_incarnation ? JS_OWNER_OK : JS_OWNER_FOREIGN;",
            "return JS_OWNER_OK;"),
        "token-incarnation": (
            "if (owner.incarnation != incarnation) return JS_OWNER_FOREIGN;", ""),
        "reuse-token": (
            "table->_tickets_used--;", "table->_tickets_used--; table->_last_token = 0;"),
        "separate-loan-issuer": (
            "uint64_t serial = ++table->_last_token;\n    table->_loans[at]",
            "uint64_t serial = ++table->_loans_used; table->_loans_used--;\n    table->_loans[at]"),
        "duplicate-closed": (
            "return status == JS_OWNER_OK ? issue_ticket(table, owner, out) : status;\n}\nJSOwnerStatus js_owner_ticket_resolve",
            "if (status == JS_OWNER_STALE) status = owner_at(table, incarnation, ticket.owner, &owner);\n"
            "    return status == JS_OWNER_OK ? issue_ticket(table, owner, out) : status;\n}\nJSOwnerStatus js_owner_ticket_resolve"),
        "consume-promotion-loan": (
            "return status == JS_OWNER_OK ? issue_ticket(table, owner, out) : status;\n}\nJSOwnerStatus js_owner_loan_end",
            "if (status == JS_OWNER_OK) js_owner_loan_end(table, incarnation, loan);\n"
            "    return status == JS_OWNER_OK ? issue_ticket(table, owner, out) : status;\n}\nJSOwnerStatus js_owner_loan_end"),
        "partial-owner": (
            "if (owner == table->_limits.owners || at == table->_limits.tickets) return JS_OWNER_CAPACITY;",
            "if (owner == table->_limits.owners) return JS_OWNER_CAPACITY;\n"
            "    if (at == table->_limits.tickets) { table->_owners_used++; return JS_OWNER_CAPACITY; }"),
        "recycle-owner": (
            "table->_tickets_used--;", "table->_tickets_used--; if (!table->_tickets_used) table->_owners_used = 0;"),
        "token-wrap": (
            "if (table->_last_token == UINT64_MAX) return JS_OWNER_EXHAUSTED;\n    put_ticket",
            "put_ticket"),
        "owner-wrap": (
            "table->_last_owner == UINT64_MAX || table->_last_token == UINT64_MAX",
            "table->_last_token == UINT64_MAX"),
        "wrong-loan-balance": (
            "table->_owners[owner].loans--;", "table->_owners[owner].tickets--;"),
        "clear-unknown": (
            "table->_tickets_used--;", "table->_tickets_used--; if (!table->_tickets_used) table->_unknown_escape = false;"),
        "reset-live": (
            "!table || table->_incarnation || !incarnation", "!table || !incarnation"),
        "size-overflow": (
            "if (count > (SIZE_MAX - start) / size) return false;", ""),
    }
    native = ["cc", "-std=c11", "-O2", "-g", "-Wall", "-Wextra", "-Werror",
              "-ffreestanding", "-fno-builtin", "-fno-tree-loop-distribute-patterns",
              "-I" + str(root / "userland/libjs/include"),
              "-I" + str(root / "userland/libjs/runtime")]
    sanitizers = ["-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-Wl,-z,noexecstack"]
    driver = str(root / "tools/test_js_owner_tickets_host.c")
    obj = str(root / "userland/obj/js/runtime/owner_tickets.o")
    recipe = subprocess.check_output(["make", "-C", str(root / "userland"), "-nB", obj], text=True)
    commands = [shlex.split(line) for line in recipe.splitlines()
                if " -c libjs/runtime/owner_tickets.c -o " in line]
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
            subprocess.run(command, cwd=root / "userland", check=True, capture_output=True)
        for mode in ("native", "target"):
            binary = output / (name + "-" + mode)
            run = subprocess.run([str(binary)], capture_output=True, text=True, timeout=30)
            log = run.stdout + run.stderr
            (output / (name + "-" + mode + ".log")).write_text(log)
            # A signal, sanitizer abort, loader error or compile failure cannot
            # stand in for the behavioral refusal oracle.
            if run.returncode != 1 or not log.startswith("FAIL: owners"):
                raise RuntimeError("variant escaped named oracle: " + name + "/" + mode + "\n" + log)
            results.append({"variant": name, "mode": mode, "oracle": log.splitlines()[0],
                            "compile_commands": compile_commands})
            print(name + "/" + mode + ": " + log.splitlines()[0], flush=True)
    (output / "results.json").write_text(json.dumps(results, indent=2) + "\n")
    print(str(len(results)) + " negative controls rejected by named behavioral oracles")


if __name__ == "__main__":
    main()
