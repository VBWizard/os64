# D8: reporting runtime destruction

Status: implemented and validated; approved by Fable, 2026-10-06.
Branch `codex/dom-d8`, worktree `.worktrees/dom-d8`, based on merged `userland`
`29641e20` (including D6). D7 is concurrent work and is not required for these
results. This slice implements DOM.md ruling 8 and its teardown acceptance.
It does not change the default-off scripting switch.

## Delivered behavior

Yonder selects an audited reclamation policy when creating a page runtime.
Retirement drains its DOM registry, closes engine handles and reports/destroys
the runtime. If the engine finds retained objects/weak references at its
teardown assertions, it returns a verdict before further cleanup. The wrapper
frees its tracked allocation ledger without engine entry. Remaining raw buffers
and ordinary strings after clean engine cleanup are also reported/reclaimed.
Native DOM records and node holds are freed independently afterward.

A reclaimed leak is sent to the kernel log with the page URL, preserved by
logd, and counted through
`yonder_scripts_teardown_leaks()`. Unexpected counts fail browser fixtures.
The deliberate lost-wrapper fixture proves that native state is reclaimed and
that a subsequent page can still execute. Existing runner creation retains
fatal teardown, and other engine aborts remain fatal in the reclaiming profile.

The additive public interfaces are `os64_js_create_with_teardown` and
`os64_js_destroy_report`; existing public layouts/ABI identity remain unchanged.
The precise host obligations, detection boundary, native ownership rules,
allocator/global-reference audit and D7 handoff are in
[libjs/TEARDOWN.md](../../../userland/libjs/TEARDOWN.md) and
[CONTRACT.md](../../../userland/libjs/CONTRACT.md).

## Evidence

| Check | Result |
| --- | --- |
| `make -j8` | Full strict target/userland build and boot image pass. Fresh worktree dependency downloads required network escalation. |
| `bash tools/test_js_runtime_host.sh` | Target engine: 939 checks; sanitized engine: 4,163 checks; zero failures/live allocations. Each runs 10,000 leaked runtimes. Five fatal controls per profile pass with full JSFA badge verification. |
| `bash tools/test_yonder_scripts_host.sh` | 2,083 checks, zero failures; ordinary fixtures have zero teardown leaks/logs. The deliberate lost-wrapper case sends one kernel-log message with the page URL and reclaimed block/byte totals, counts one leak and leaves page/native/engine storage reclaimed; the next page runs and retires without another leak log. |
| `bash tools/test_js_acceptance_host.sh` | Each engine profile passes 383 independent checks; 55 upstream functions pass, four unsupported-feature skips. Diagnostic refusal leaves zero live allocations; separate fatal control passes. |
| `bash tools/test_js_cli_host.sh` | 196 runner checks and 38 real-library checks pass. |
| `bash tools/test_js_contract_headers.sh` | Manifest hashes, cross-header and binding syntax pass. |
| `python3 tools/test_js_port_target.py` | 13 runtime exports, unchanged 186 engine exports; real dependency, visibility, binding and relink audits pass. |
| `python3 tools/test_js_teardown_audit.py` | Target direct-allocation/global-state/private-symbol audit passes. |
| QEMU, final VM 56309 | `/tests/jsembedtest`: 437 checks, zero failures, 41 native calls; status `0x4A535254` (JSRT). Ten thousand leaked runtimes preserve `/proc/self/heap` live bytes and blocks; heap verification passes. |
| Guest fatal policy | Intentional default leak reports its engine invariant and exits `0x4A534641` (JSFA). |
| Shipped guest runner | `js -e "print(6*7)"` outputs `42`, status zero. |
| Post-stop filesystems | Read-only ext2 checks on final copied root and home images pass. |

Host sanitizer suites ran outside sandbox ptrace restrictions with normal leak
checking enabled. The first browser heap comparison used different rendered
text and measured shared glyph-cache growth; using the same rendered text
isolates page-owned storage. Both the corrected comparison and final overall
heap cleanup pass. This required no production-code change.

The first VM, 56308, passed 434 runtime checks before the guest heap-counter
assertions were added and the fatal control; it is stopped. The final guest is stopped. It used
copied disks, a validation-only fixture installed in the isolated worktree's
root image and the production built library. Final delivered bytes match:

| Artifact | Bytes | SHA-256 |
| --- | --- | --- |
| `libjs.so` | 4,527,960 | `8d550baae7b213c0cb7f83834aea72282b993ab5417918696e43813994cee866` |
| `jsembedtest` | 97,680 | `f3497584a436d05114fa45a2ba23768df7620af9d51b7bfc5998b298b0056106` |

Raw outputs are `/tmp/dom-d8-runtime.log`, `/tmp/dom-d8-yonder.log`,
`/tmp/dom-d8-acceptance.log`, `/tmp/dom-d8-cli.log`, `/tmp/dom-d8-target.log`,
`/tmp/dom-d8-final-runtime.txt` and `/tmp/dom-d8-final-status.txt`.
Browser lost-wrapper recovery is host evidence against the merged D5/D6 browser;
this does not claim an interactive guest recovery demonstration, D7 integration,
new P5 evidence. Fable's independent review approved the implementation;
the review follow-up routes the reclaimed-leak verdict through the kernel
log and corrects the guest fixture name to the installed `jsembedtest` name.
The follow-up target Yonder rebuild and sanitized browser suite pass; the
runtime implementation and original guest validation artifacts are unchanged.

## Integration handoff

D7's function-call/checkpoint interfaces do not depend on the D8 additions.
The overlap is runtime creation/destruction and browser retirement. Preserve
D7's dispatch work while adopting D8's creation-selected policy, report logging
and count. Verify that timers, listeners and queued event values drain before
destroy; native records must release without using dead engine handles. The
D7 event finalizer can own engine-allocator records, which are covered by the
ledger, but external resources need independent cleanup. Re-run the browser
leak and D7 suites on that joined implementation before its acceptance.
