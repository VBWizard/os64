# F.4b — entry and incumbent scope validation

Implementation: `c89bb5b4`, based on `userland` merge `650b51a4` (#571).
Engine source identity:
`c3a9a38ee9ba5c65c5a8f8d1a24e3d382bdc80c0b67a77c87d6ac739fe3dc3ee`.
The interface and remaining browser gates are in
[IFRAME_F4_SETTINGS.md](IFRAME_F4_SETTINGS.md).

## Host and build evidence

| Check | Result |
|---|---|
| Full target-engine runtime suite, final fixtures | 14,390 checks; zero failures and live allocations |
| Full instrumented-engine runtime suite, final fixtures | 61,738 checks; zero failures and live allocations |
| Focused `--settings-scopes`, target engine | 724 checks, zero failures |
| Focused `--settings-scopes`, instrumented engine | 724 checks, zero failures |
| Parser allocation-refusal sweeps, both engines | 40 checks each, zero failures |
| Fatal runtime invariant paths | Expected exit 99 and full JSFA badge |
| Contract headers and hash-checked engine manifest | Pass |
| Preparation regression tests | Six tests pass |
| Teardown allocation/global/visibility audit | Pass |
| Target `libjs.so`, standalone `js`, guest runtime fixture | Build/link pass |
| Tracker validator and stale-reference check | Pass |

The complete runtime harness was run with leak detection enabled. After the
fixture was strengthened to remove masking JavaScript wrappers around native
generator methods, its host object was rebuilt and linked against the same
unchanged engine/support objects. Both final full suites and focused selectors
run those frozen final binaries. The guest fixture was rebuilt as well.

The instrumented engine uses ASan; wrapper/support fixtures retain ASan/UBSan.
Normal exits keep LeakSanitizer enabled. The sandbox diagnostic reported
thread attachment `EPERM` with `TracerPid: 0`; leak-enabled runs therefore ran
outside the sandbox. Leak detection was not disabled. Fatal-path fixtures
exercise the harness's explicit exit hook and are not normal-exit leak checks.

Reproduction from the implementation tree:

```sh
ASAN_OPTIONS=detect_leaks=1 JS_RUNTIME_KEEP=1 bash tools/test_js_runtime_host.sh
# Run each retained probe and sanitized-probe with --settings-scopes.
bash tools/test_js_contract_headers.sh
python3 -I tools/test_js_prepare.py
python3 -I tools/test_js_teardown_audit.py
python3 -I tools/tracker.py --csv docs/design/pending/IFRAME_TRACKER.csv --prefix F --quiet
```

The shared fixture runs both teardown policies. Its independent A/B/C context
identities distinguish host entry, script incumbent and native defining realm.
It covers native-only callback backup, new script activations above a suppressed
caller, repeated suppression of the same activation, ordinary reentrant entry,
return/throw cleanup, output and pending-exception preservation on refusal,
the scope limit, and sticky cancellation. Queries outside explicit entry return
no settings.

Provenance cases include direct/indirect eval, a foreign dynamic function
created with no active script, closure creation under an outer script versus
without one, bytecode round trips for scripts/modules, module/async suspension,
and bound native generator next/return/throw. The last three enter the generator
without a same-realm JavaScript wrapper that could conceal a missing resume
marker. Promise jobs run under an explicit fixture-host scope; this does not
claim automatic Promise callback capture.

## Fault-injection evidence

Fourteen independently compiled ASan variants were caught with leak checking
enabled. The strengthened resume variant fails at the bound-native generator
case rather than relying on a same-realm caller or a later async check.

The variants independently replace:

- Most recent entry with the outer entry; backup with entry; incumbent selection
  with a query that ignores suppression; captured backup with callback entry.
- Backup restoration and entry restoration with clearing.
- Eval provenance with active-chain inheritance, including a direct-eval-only
  variant; ordinary closure provenance with constant present or absent.
- Generator/async resume activation and ordinary-call activation with absent
  provenance; activation restoration with clearing; the scope limit with 63.

Each variant compiles against the same final fixture. A nonzero exit alone is
not counted as a catch: the runner requires an explicit fixture failure,
ASan error, or assertion diagnostic. No fault-injected source is shipped.

## F.16 composition

The frozen F.16 patches used were:

- `0023-execution-scope-snapshot.patch`:
  `5ba6411600ad9ad4237c7c36eb516a56110b019dae69a6d2fd6fbb00f49a5ee4`
- `0024-failed-code-entry.patch`:
  `1dad0a03f2f3a847696748ef435a75a923db0ebc637479a3ccc7496757d63600`

Applying those before or after `0026-settings-scopes.patch`, with zero fuzz,
produces byte-identical sources:

- `quickjs.c`: `56fcbb45cd88a23151f291fbb13a16896bf726c69a9cf64768c43cfbdc82f0bc`
- `quickjs.h`: `d51268dfd1cbd53921b414be670f5f5a2879f7bc8f78bfda492a759baec96a7c`

The combined ASan engine passes the final 724 focused settings checks with
LeakSanitizer enabled, covering both teardown policies.
This is focused combined-engine evidence, not a full F.16 recovery acceptance
run. Scope depth remains active through the native body even without JS frames;
a recovery quiescence check must account for it before committing recovery.

## Limits

No production DOM consumer, WindowProxy permission gate, callback registry,
Promise HostMakeJobCallback integration or active script/module record identity
is supplied. The host must validate lifecycle and retain context backing before
using the raw scope. Browser current/relevant attribution remains separate.
No QEMU, P5, WPT or visible-iframe acceptance is claimed. F.4b and F.4 remain
in progress.
