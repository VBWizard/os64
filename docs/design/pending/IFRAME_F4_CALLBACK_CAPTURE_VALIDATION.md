# F.4b — native callback capture validation

Based on `userland` `f73ddd56`, including the #575 checked settings bridge and
#573 execution/failure/iterator engine work. This cut adds native record
ownership, conversion-time incumbent capture and guarded invocation adapters.
It adds no engine patch or production browser consumer, and changes no existing
public layout or ABI ID. Prepared engine identity remains
`da43badc52c71c121b1be56f8f08077531c31fcacdb9a82b6751f6baad739b94`.

Contracts: [IFRAME_F4_SETTINGS.md](IFRAME_F4_SETTINGS.md) and
[libjs CONTRACT.md](../../../userland/libjs/CONTRACT.md).

## Validation

| Check | Result |
|---|---|
| Full shipping-engine runtime suite | 15,717 checks; zero failures/live allocations |
| Full instrumented-engine runtime suite | 63,065 checks; zero failures/live allocations |
| Focused `--callback-settings` | 197 checks per engine; zero failures/live allocations |
| Parser allocation-refusal suites | 40 checks per engine; zero failures |
| Fatal/reclaim invariant fixtures | Expected exit 99 and full JSFA badge |
| Independent wrapper fault variants | 12 caught |
| Source manifest, public-header/binding syntax | Pass |
| Preparation regressions | Six pass |
| Teardown allocation/global/visibility audit | Pass |
| Target library, standalone `js`, guest runtime consumer | Build/link pass |
| Tracker validator, stale references, diff check | Pass |

ASan covers the instrumented engine; ASan/UBSan cover wrapper/support fixtures.
Successful normal exits retain LeakSanitizer. The sandbox denied thread
attachment with `EPERM`, with `TracerPid: 0`; successful tests ran outside it
with `ASAN_OPTIONS=detect_leaks=1`. Leak detection was not disabled. Explicit
fatal-exit fixtures do not supply normal-exit leak checks.

Reproduce with:

```sh
ASAN_OPTIONS=detect_leaks=1 JS_RUNTIME_KEEP=1 bash tools/test_js_runtime_host.sh
# Focused alternative, with both engine builds:
ASAN_OPTIONS=detect_leaks=1 JS_RUNTIME_FOCUSED_SELECTOR=--callback-settings bash tools/test_js_runtime_host.sh
bash tools/test_js_contract_headers.sh
python3 -I tools/test_js_prepare.py
python3 -I tools/test_js_teardown_audit.py
python3 -I tools/tracker.py --csv docs/design/pending/IFRAME_TRACKER.csv --prefix F --quiet
```

## Independent oracles

The shared host/guest fixture `userland/libjs/tests/callback_settings.h` uses
three independently identified realms under both teardown policies:

- Host entry A invokes registration script C through a native binding from A;
  capture retains C for a bound native callback from B. The same callback can
  independently capture A. New author code in B overrides the captured backup.
- Replacing the record during invocation changes later deliveries while the
  active scope keeps its original incumbent. Object callback lookup runs under
  entry B with a getter authored in C; the native method then uses captured A.
- Empty-scope capture, populated/non-object capture, invalid ABI/runtime clear,
  and active outcome aliases refuse transactionally. Refusals preserve the
  pending exception and held reference. A thrown callback keeps its exception
  and restores settings for the following invocation.
- A C-root callback survives GC. Self-removal keeps its current invocation
  alive, then releases the temporary reference inside the guard. Direct last-
  reference cleanup resets the record before native finalizer reentry.
- Tracing permits a callback/holder cycle to be collected. Another cycle is
  finalized during runtime destruction after cancellation and realm retirement.
  Clear is idempotent after failure; retired capture refuses later delivery
  before the body runs or the value output changes.

Twelve separately compiled ASan/UBSan wrapper variants were linked with the
same instrumented engine and fixture. They substitute entry for incumbent;
lose or leak capture/invocation references; clear after finalization; omit the
GC edge; substitute entry or no incumbent at delivery; bypass clear's ABI or
outcome-alias refusal; or admit non-object capture. Each produced an explicit
fixture failure or ASan use-after-free report. Compilation failure/nonzero exit
alone was not counted. Mutated sources are not shipped.

## Scope limits

This is a native capture and lifetime building block. The invocation adapter
must separately prove the associated entry realm and apply the binding's
callback-type, activity and security checks. No inference from callable target,
`handleEvent`, registration binding or task owner is provided. DOM conversion
and delivery sites, Promise reaction/thenable capture, current/relevant settings
attribution and origin checks remain F.4b work. The merged F.16 engine changes
are included in this runtime cohort; wrapper recovery is not enabled.
No QEMU, P5, WPT or visible-frame acceptance is claimed. F.4 remains `code`.
