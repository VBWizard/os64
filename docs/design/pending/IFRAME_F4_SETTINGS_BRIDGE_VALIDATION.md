# F.4b — checked settings bridge validation

Implementation: `c9281a85`, based on `userland` `73b0742b`, including #572.
The engine source identity is unchanged:
`c3a9a38ee9ba5c65c5a8f8d1a24e3d382bdc80c0b67a77c87d6ac739fe3dc3ee`.
This cut changes the wrapper and adds opt-in host APIs; no engine patch or
production DOM consumer is included. Contract and remaining integration gates:
[IFRAME_F4_SETTINGS.md](IFRAME_F4_SETTINGS.md) and
[libjs CONTRACT.md](../../../userland/libjs/CONTRACT.md).

## Validation

| Check | Result |
|---|---|
| Full shipping-engine runtime suite | 15,158 checks; zero failures and live allocations |
| Full instrumented-engine runtime suite | 62,506 checks; zero failures and live allocations |
| Focused `--settings-bridge`, shipping engine | 769 checks; zero failures |
| Focused `--settings-bridge`, instrumented engine | 769 checks; zero failures |
| Parser allocation-refusal suites | 40 checks per engine; zero failures |
| Fatal/reclaim invariant fixtures | Expected exit 99 and full JSFA badge |
| Independent wrapper fault variants | 13 caught |
| Hash manifest and header/binding syntax checks | Pass |
| Preparation regression tests | Six pass |
| Teardown allocation/global/visibility audit | Pass |
| Target `libjs.so`, standalone `js`, guest runtime consumer | Build/link pass |
| Tracker validator, stale-reference check and diff check | Pass |

The instrumented engine uses ASan, with ASan/UBSan on wrapper and support
fixtures. Successful normal-exit runs retain LeakSanitizer. Sandbox diagnosis
reported thread-attachment `EPERM` and `TracerPid: 0`; successful checks ran
outside that sandbox with `ASAN_OPTIONS=detect_leaks=1`. Leak detection was
not disabled. Explicit fatal-exit fixtures are not normal-exit leak checks.

Reproduce with:

```sh
ASAN_OPTIONS=detect_leaks=1 JS_RUNTIME_KEEP=1 bash tools/test_js_runtime_host.sh
# Run each retained probe and sanitized-probe with --settings-bridge.
bash tools/test_js_contract_headers.sh
python3 -I tools/test_js_prepare.py
python3 -I tools/test_js_teardown_audit.py
python3 -I tools/tracker.py --csv docs/design/pending/IFRAME_TRACKER.csv --prefix F --quiet
```

## Independent oracles

`userland/libjs/tests/settings_bridge.h` runs under both teardown policies,
with independently identified A/B/C realms and a private staged realm. It
checks the following behaviors:

- Entry A, script incumbent B, and captured native-callback incumbent C remain
  distinct through nested normal and abrupt calls. The outer task remains A.
- Missing budget, ABI/argument errors, foreign realms, staged entry/incumbent,
  private setup and retired entry/incumbent refuse before the body runs and
  preserve the value output. A query after retirement rechecks eligibility.
- Native-body scope lifetime blocks task closure, top-level reentry and
  quiescent cleanup even without JS frames. Scope exit releases that guard.
- All 64 scope levels preserve the checked entry and intermediate outcome
  storage. Overflow is local, preserves the result and unwinds without
  poisoning the task. Nested alias attempts do not clear an outer outcome.
- Native-only scope bookkeeping leaves heap charge unchanged and preserves
  an already pending exception. Abrupt body return transfers JS_EXCEPTION
  without consuming the pending value or invoking diagnostic conversion.
- Queued jobs remain pending until the caller's task checkpoint. Cancellation
  closes queries, survives scope exit, and retains the body's transferred
  result for the caller to release.

`tools/test_js_settings_bridge.inc` supplies host-clock control. Queries leave
its counter unchanged even when each clock read would advance time. The body
exhausts the remainder of a previously armed task deadline; the scope reports
LIMIT/EXECUTION without renewal. Raw-only scopes and unregistered same-engine
contexts cannot publish a checked realm through the query.

## Fault variants

Each variant is a separately compiled ASan/UBSan wrapper linked with the same
instrumented engine and final fixture. All 13 exited with explicit fixture
failures. Compilation failure or a nonzero exit alone was not counted.

The variants omit retirement, staging or private-setup checks; substitute entry
for captured incumbent; drop the native guard, scope-chain restoration or
intermediate outcome-alias protection; permit raw-only query; invent a realm
for an unregistered context; sample the clock in a query; renew the deadline;
skip post-body failure observation; or consume the body's pending exception.
No mutated source is included in the delivered library.

## Scope limits

The wrapper validates its own runtime and realm lifecycle. It does not enable
F.16 child-local recovery: that protocol must synchronize engine failed-context
state and wrapper eligibility while preserving the native-body guard.
This cohort does not include the unmerged F.16 engine changes or qualify their
iterator-path repair. Browser current/relevant attribution, callback conversion
capture, Promise reaction/thenable capture, DOM entry-site integration and
operation-specific origin checks remain F.4b gates. A checked settings handle
is not a permission check. No QEMU, P5, WPT or visible-iframe acceptance is
claimed, and F.4 remains in progress.
