# F.16 dormant execution-scope checkpoint

Source: F16 brief commit `80f6b794` plus the execution-scope patch described
below. This checkpoint is a partial F16b cut. It does not implement recovery,
failed-environment guards, native consistency certification or the F16 exit.
The shipping wrapper retains its ABI 5 runtime-wide failure semantics.

## Implemented primitive

Manifest patch `0023-execution-scope-snapshot.patch` adds owner-thread,
explicitly enabled raw-engine scope tracking. Default constructors leave it
disabled. The [failure-record constructor](IFRAME_F16_FAILURE_RECORDS.md) enables
it before exposure; the engine evidence below qualifies the earlier scope cut.
`JS_EnableExecutionScopeTracking` is a
pre-execution host operation. `JS_VisitExecutionScopes` visits borrowed facts,
innermost first, without allocating, retaining values, executing jobs or
changing failure/budget state. Visitors must not reenter or mutate the engine.

Thin wrappers retain stack-local records around ordinary calls, constructors,
generator/async resumes, eval and compiled eval/module entry. Call records
are linked before the existing interrupt poll, which precedes interpreter
frame installation. Job execution keeps its record through body and value
cleanup, before releasing its held context/owner references.

`caller` is the incoming engine context, not a browser security caller.
`code` and `code_kind` describe the selected code environment where known.
Native C and CFunctionData callbacks retain their definition realm separately
from the incoming context. Bound functions resolve to their target through a bounded scan; deeper chains
remain UNKNOWN until nested dispatch reaches a resolvable target. Proxy and
custom dispatch report UNKNOWN, alongside any nested trap/target call facts.
Eval preparation and job scopes also retain UNKNOWN classification; a neutral
job does not borrow its activity owner's identity as a code owner. These facts
are deliberately insufficient for a recovery completeness certificate.

Scopes borrow contexts held by the active engine operation; they introduce
no engine roots or independent native resources. Enabling tracking cannot
clear interruption, cancel scheduled work or permit failed wrapper entry.
The recovery state machine remains outside this checkpoint. The failure-record
cut adds terminal diagnostics without authorizing recovery.

## Focused evidence

Prepared engine identity:
`365c060beaeb86d59eb74ec4290ce478f36fb321d8ec3d25b631dfe07f472f95`.
The runtime fixture's `--execution-scopes-only` selector passes **119 checks
on each of the shipping and fully instrumented engine builds**, zero failures.
Both binaries have ASan/UBSan on the wrapper; the shipping engine is the real
target core, and the instrumented engine has ASan with its existing engine
UBSan exception. Both use LeakSanitizer. Raw-engine contexts in these fixtures
are trusted test machinery, not authorization for wrapper consumers to create
extra contexts directly.

Observations include three nested bytecode environments, foreign native and
CFunctionData callbacks, raw native reentry, bound and proxy dispatch, getter/
coercion entry, constructor scopes, direct eval, compiled modules, Promise and
neutral jobs, generator/async/async-generator resumes, ordinary exceptions and
uncatchable interrupt unwind. The pre-entry test stops a raw A → B call before
B acquires an interpreter frame and checks that B performed no body side effect.
Scope snapshots make no allocator attempts. After each completed/aborted entry,
the chain is empty, and final engine teardown releases all fixture allocations.
This is engine lifetime evidence, not permission to resume an aborted realm.

Four separately compiled source mutants are caught by named assertions:

| Mutation | Oracle |
|---|---|
| Native code owner replaced with incoming context | Foreign native definition realm differs from caller |
| Interrupt poll moved before pending-call scope | Pre-entry interrupt observes pending child |
| Generator/async resume code attribution removed | Generator resume records suspended code realm |
| Neutral job code owner replaced with activity owner | Neutral native job does not invent code ownership |

Mutants retain ASan and leak detection and reuse coherent unchanged dependent
objects; each returns the fixture's assertion-failure status, with its specific
oracle in stderr. Timeout or build failure does not count as caught evidence.

The sandbox LSan diagnostic reported attachment EPERM with `TracerPid: 0`.
The same binaries passed outside the sandbox with `ASAN_OPTIONS=detect_leaks=1`;
leak detection was not disabled. The compiler cache was disabled because its
normal directory was read-only; target artifacts were built in this worktree.

Reproduction entry points:

```sh
CCACHE_DISABLE=1 JS_RUNTIME_KEEP=1 JS_RUNTIME_FOCUSED_SELECTOR=--execution-scopes-only \
  bash tools/test_js_runtime_host.sh
```

The selector builds both coherent engines. If sandbox thread attachment fails,
retain the reported artifact directory and rerun its `probe` and
`sanitized-probe` with `--execution-scopes-only`, outside that sandbox and with
leak detection enabled. A separately selected realms-only shell run still does
not supply instrumented engine evidence.

## Default-disabled regression checks

On the final engine identity above, the complete runtime fixtures pass:
**13,397 shipping checks** and **60,745 instrumented checks**, zero failures
and zero live fixture allocations. Both compiler-refusal sweeps pass 40 checks;
the shipping raw-construction probe and both engines' fatal/reclaim invariant
paths pass. These runs use the retained coherent binaries with
`ASAN_OPTIONS=detect_leaks=1` outside the sandbox. The full runtime invocation
also includes the final 119 scope checks, not an earlier incremental fixture.

The target library and runtime consumer link successfully. Contract/header
checks, all six source-preparation tests, allocation/global/visibility teardown
audit, tracker, shell syntax, stale-reference and diff checks pass. No stale
retired names or added superlative claims were reported. Only the F16 tracker
row changes; its code status denotes this partial implementation.

## F.4 composition and integration boundary

The current F.4 patch `0022-browser-global-this.patch` and this patch apply
with `patch --batch -F0` in both orders after patch 0021. Both orders produce
byte-identical files:

- `quickjs.c`: `6496020445d07615be1ca06f18292fc83bab63da6016e24c279de71bd21c9cc7`
- `quickjs.h`: `ec108e0d647beb07b88a70a61809b9c48a4d827762e0c39215f879fe1dc49a15`

Both raw primitive fixtures also pass against one combined ASan engine:
119 F16 execution-scope checks and 67 F4 global-this checks, with leak detection
enabled. The combined engine was compiled at O0 and linked separately to each
lane's coherent unchanged dependent object set; this is focused composition
evidence, not a full combined runtime run. F4 input patch SHA-256:
`532ffe0695ac387b0481426542cade6ed6ab47bd380dcb79fb95d84a025d89da`;
F16 input patch SHA-256:
`5ba6411600ad9ad4237c7c36eb516a56110b019dae69a6d2fd6fbb00f49a5ee4`.

This establishes patch composition for those exact inputs; it does not prove
WindowProxy security, recovery, or integration with later F.4 scope policy.
F.4 owns its patch and security settings. Wrapper ABI 5 and public wrapper
layouts are unchanged by this raw-engine API addition; the prepared engine
identity changes, invalidating incompatible bytecode cache entries.

Full DOM, guest/WPT and any P5 recovery acceptance
remain subsequent containment/integration work under the F16 brief. No frame
loading, slot reuse, child-local failure reset or recovery acceptance is claimed.
