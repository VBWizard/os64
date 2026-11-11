# F.2b — binding implementation record

Work in progress against `0a1c6afb`, which merged F.2a (#537) after a clean
Codex review of `8e29c9dd`. The accepted scope remains
[IFRAME_F2_OWNERSHIP.md](IFRAME_F2_OWNERSHIP.md). This record does not mark
F.2 complete or claim that cross-binding adoption is implemented.

## Explicit realm routing checkpoint

Each binding retains its borrowed libjs realm handle alongside its runtime.
Context access, task entry, callback invocation, string-timer evaluation,
script-host installation/removal and rejection taking select that realm.
Task completion, budget checks and the pending-job queue remain runtime-wide.
The public constructor selects the primary realm as before. A private
installation helper accepts a realm and checks runtime membership before
native allocation. Runtime class registration still refuses a second binding.
No new public child-binding API or visible iframe path is enabled.

The new `--realms` DOM fixture installs a binding in a secondary realm while
the primary keeps independent globals, an exception provider and a pending
rejection. It verifies callback and string timers, animation, idle callbacks,
host event dispatch, mutation-observer delivery, child error/rejection delivery,
foreign-runtime admission refusal and preservation of the primary provider
through child drain. It runs under both libjs teardown policies and checks
that native allocation ledgers empty after teardown.

| Check | Result |
|---|---|
| DOM `--realms`, shipping engine | 66 checks; zero failures |
| DOM `--realms`, instrumented engine | 66 checks; zero failures |
| Full DOM, shipping engine | 43,532 checks; zero failures; 3,877 allocation sweep points |
| DOM `--mutants`, instrumented engine | 30,799 checks; zero failures; 2,435 sweep points |
| Yonder scripted-page integration | 24,827 checks; zero failures; 114 completed refusal cases |

Host runs retain ASan/UBSan and LeakSanitizer (`detect_leaks=1`), outside the
sandbox for leak-check thread attachment. Logs are streamed with bounded
storage and readers. `--mutants` selects the existing functional matrix and
omits the final comprehensive allocation sweeps; it is not a new negative
mutation campaign. The focused `--realms` run uses both engine profiles from
`tools/test_dom_host.sh --realms`.

## Remaining F.2b work

The next implementation is cluster ownership/admission and guarded installation,
followed by the cluster identity cache, creation-realm prototypes and operation
routing by current node document. Shared custom-element reactions and mutation
observers, aggregate DOM/kept/Canvas caps, explicit retained Document ownership
and cluster teardown still require implementation and proofs.

The final F.2b acceptance also includes cross-binding fixtures, native failures,
negative mutation proofs, pinned named-WPT baseline comparisons and an actual
multi-realm DOM guest fixture. The tracker stays in progress until those
requirements and review are complete. No P5 test is needed for this checkpoint.
