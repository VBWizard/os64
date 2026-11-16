# F.4b — current execution realm validation

Baseline: merged `userland` `e42fd39e`, including #576 callback ownership and
#577 failure diagnostics. Patch `0028-current-realm.patch` adds current realm
tracking and the pre-native-entry snapshot; the wrapper exposes checked
CURRENT and NATIVE_CALLER roles. This adds no production browser consumer or
origin permission. Existing public layouts and embedding ABI 5 are unchanged.

Prepared engine identity:
`3dff9a1757ed4988883d1e589351f1f32b143063cd0230a3906881c5ab3654fa`.
Engine objects and headers must rebuild together; cached bytecode from the
previous identity is refused. Contracts and remaining gates:
[IFRAME_F4_SETTINGS.md](IFRAME_F4_SETTINGS.md) and
[libjs CONTRACT.md](../../../userland/libjs/CONTRACT.md).

## Entry-path audit

| Path | Current realm and restoration |
|---|---|
| Explicit host settings body | Prepared entry realm; prior current restored with entry/incumbent on exit |
| Bytecode and generator/async resume | Code realm, independently of ScriptOrModule presence or incumbent suppression; automatic activation saves prior current |
| Generic C function, accessor and constructor | Defining realm; incoming/defining native facts and callback argument convention preserved |
| C-function-data | Retained defining realm; native body still receives incoming `ctx` |
| Optimized iterator | Defining realm despite bypassing normal call dispatch; body still receives incoming `ctx` |
| Bound/proxy forwarding | No new execution context; actual target/trap establishes its context |
| Known generator/async/resolving-function classes | Function realm around class dispatch, including native resolver property lookup |
| Unknown custom call class | Current becomes unavailable; nested native calls receive no inherited caller proof |
| Ordinary bytecode construction | Callee realm before prototype lookup, restored on object result, primitive result or exception |
| Direct async module execution | Explicit realm bracket around module's direct async entry, in addition to bytecode/resume tracking |

The engine adds two runtime pointers (16 bytes on the target) and automatic
saved pointers. It introduces no retained roots, new allocation sites or
suspended native-stack pointers. Queries require explicit settings preparation;
checked roles additionally validate scope, wrapper membership and lifecycle.
NATIVE_CALLER is the innermost active C/data/optimized-iterator invocation's
snapshot across reentry, not a generic caller or origin authorization.

## Validation

| Check | Result |
|---|---|
| Full shipping-engine runtime | 16,914 checks; zero failures/live allocations |
| Full instrumented-engine runtime | 64,262 checks; zero failures/live allocations |
| Focused `--current-settings` | 379 checks per engine; zero failures/live allocations |
| Parser allocation-refusal suites | 40 checks per engine; zero failures |
| Fatal/reclaim invariants | Expected exit 99 and full JSFA badge |
| Independent engine fault variants | 12 caught |
| Full shipping DOM | 322,022 checks; 12,941 allocation cuts; zero failures |
| Instrumented DOM and focused shipping checks | Window, core, realms, Streams, callbacks, departure, installation and report retirement pass |
| Manifest/header/preparation/teardown audits | Pass; six preparation regressions |
| Target library, standalone `js`, guest runtime consumer | Build/link pass |
| Tracker, stale-reference and diff checks | Pass |

ASan instruments the engine; ASan/UBSan instrument wrapper/support fixtures.
Successful normal exits retain LeakSanitizer, run outside the sandbox after
its diagnosed thread-attachment `EPERM` with `TracerPid: 0`. Leak detection was
not disabled. Explicit fatal exits do not supply normal-exit leak evidence.

Reproduce with:

```sh
ASAN_OPTIONS=detect_leaks=1 JS_RUNTIME_KEEP=1 bash tools/test_js_runtime_host.sh
ASAN_OPTIONS=detect_leaks=1 JS_RUNTIME_FOCUSED_SELECTOR=--current-settings bash tools/test_js_runtime_host.sh
ASAN_OPTIONS=detect_leaks=1 DOM_HOST_KEEP=1 bash tools/test_dom_host.sh --window
# Retained target-core-probe: full suite; both probes: focused selectors below.
bash tools/test_js_contract_headers.sh
python3 -I tools/test_js_prepare.py
python3 -I tools/test_js_teardown_audit.py
python3 -I tools/tracker.py --csv docs/design/pending/IFRAME_TRACKER.csv --prefix F --quiet
```

The focused DOM selectors are `--window`, `--core`, `--realms`,
`--stream-realms`, `--callback-reports`, `--departure`, `--child-install-engine`
and `--report-retirement`. Both engines pass; child-installation checks are
1,402 shipping / 1,306 instrumented. The instrumented DOM full allocation sweep
was not repeated; its listed focused suites supply that engine's evidence.

## Independent observations

`userland/libjs/tests/current_settings.h` runs under both teardown policies.
An exotic property observer samples the current realm without introducing a
native function context. Separate A/B/C pointers supply the oracle:

- Host entry A, bytecode B and native C distinguish current, entry, incumbent
  and pre-native-entry caller. A C body deliberately passes incoming A to a
  data function: the recorded caller remains C. Nested host preparation uses
  current/entry B and captured incumbent C while retaining the outer native
  invocation's caller A.
- Native/data calls, an accessor, bound/empty/trapping/revoked proxies, foreign
  `Function.prototype.call`, `Reflect.apply`, direct/indirect eval, dynamic
  functions and native/bytecode constructors restore attribution. Constructor
  prototype lookup observes B before the B body starts. A Promise resolver
  created in B performs its `then` lookup in B when called from A.
- Foreign author reentry changes CURRENT and incumbent while the enclosing
  native caller snapshot stays fixed. Normal/abrupt returns restore both;
  unknown custom call classes refuse inherited current/caller authority.
- Generator, async and async-generator resumes, module top-level await and
  cached bytecode retain code realm B under an explicit host entry A. These
  tests provide host scopes explicitly; they do not implement Promise capture.
- Native-only queries leave heap charge and pending exception unchanged.
  Idle/raw-only wrapper queries refuse. Retirement rechecks current and native
  caller handles; cancellation unwinds scopes without clearing task failure.

Twelve separately compiled instrumented engine variants were linked against
the same final wrapper and fixture. Each produced an explicit fixture failure.
They substitute entry/incumbent for current, incoming for native caller,
incoming for data-function current, omit iterator/bytecode/host current entry,
lose host/native restoration, permit custom-class inheritance, omit special
built-in current, or omit constructor current before prototype lookup.
Compilation failure/nonzero exit alone was not counted; no mutant is shipped.

## Limits

Relevant settings still come from the target binding. DOM entry-site and
pre-member security integration, associated callback-object realm proof,
WebIDL conversion sites, Promise capture and exact active-script identity
remain F.4b gates. Native job callbacks require host preparation separately;
queue owner/activity is not substituted for current or incumbent. The engine
attribution fields are not F.16 recovery proof and enable no recovery reset.
No QEMU, P5, WPT or visible-frame acceptance is claimed. F.4 remains `code`.
