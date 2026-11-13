# F.16 dormant engine containment checkpoint

This implements the engine primitives in the approved
[F16 brief](IFRAME_F16_RECOVERY.md), following the
[scope checkpoint](IFRAME_F16_SCOPES_VALIDATION.md). The initial integration baseline
is `650b51a4` (accepted F4 native-call facts), including F4 global-this and
source-tracked patches 0022/0025. The merge-forward qualification below adds
accepted F4 settings scopes at `e7708cec`; the iterator review correction adds
patch 0027. **Child-local runtime recovery remains
unimplemented and disabled.** This is an engine foundation cut, not F16
completion or certification of DOM/native consistency after interruption.

## Implemented entry boundary

Patch 0023 supplies opt-in allocation-free execution-scope facts. Patch
`0024-failed-code-entry.patch` adds `JS_MarkContextExecutableFailed` and
`JS_IsContextExecutableFailed`. Marking requires tracking enabled, runs on the
owner thread outside engine/native entry after unwind, and refuses tracked
active entry. It allocates nothing, releases nothing and leaves queued jobs,
exceptions, task accounting and cancellation unchanged. A successful mark is
irreversible. No wrapper entry point marks a context or enables recovery.

Once a context is marked, the raw engine refuses its callable code before
body entry, including ordinary/closure/bound functions, constructors, native
C and data functions (including neutral continuations), accessors/coercions,
eval/compiled bytecode, module execution and suspended generator/async resumes.
Proxy target discovery precedes trap lookup; failed traps/getters are separately
caught at their actual function entry. Discovery reads engine links without
calling author code. Over-deep wrapper chains and unknown custom call classes
fail closed once executable failure has been seen. This conservative policy
persists for that runtime; it is not a count of currently retained contexts.

Patch 0027 applies the same entry guard to the optimized native iterator path,
which bypasses `JS_CallInternal` to return an unboxed iteration result. It
records the native execution scope and incoming/defining context pair before
the callback, then restores both on normal or exceptional return. The callback
still receives the incoming context; this native scope supplies no script
provenance or browser settings authority.

A marked execution, callback or activity context blocks a queued native job
before its body can load modules, resume an algorithm or call author handlers.
The job still releases its existing holds through the engine's normal release
path. Mixed/neutral settlement consistency and wrapper executed/discarded job
accounting are separate recovery-protocol obligations; this primitive does not
silently assign obsolete jobs to a fresh budget.

A blocked alias normally throws a catchable TypeError in the healthy caller.
If an uncatchable exception is still pending, the guard returns that exception
without replacing it or allocating another error. A consumed exception's
residual marker cannot suppress the next local refusal. Existing wrapper
resource/host/cancellation failure remains sticky; the guard does not reset it.
Error creation may still fail under the existing allocation-failure contract.

Ordinary retirement is unchanged and allows retained direct calls. Executable
containment preserves object identity, ordinary data fields and already
initialized module namespace data; it blocks code that could interpret those
objects. Native records are not freed to manufacture containment. This does
not prove that an interrupted native algorithm or self-hosted Streams state is
consistent enough to permit recovery.

## Module ownership and teardown

Modules retain and GC-mark their creation context through an explicit private
engine edge. This ownership applies to JavaScript and native modules, including
handles unlinked from a context's module list. Release drops the matching hold.
It makes failed-owner checks valid after the external context reference is
released and GC runs. It is a lifetime/creation owner, not a browser security
caller or a substitute for the context passed to a native module initializer.

Module graph resolution and direct synchronous/asynchronous execution check
that owner separately, including dependencies and resumed graph work. Compiled
input refusal preserves the consuming API's release obligation. This matters
because module execution can enter the async engine directly, and native
module initialization does not pass through a public JS_Call wrapper.

The ownership edge exists even while executable containment is unused. Default
wrapper behavior still keeps its realms until root teardown; raw module handles
now independently retain the context needed by their ownership checks. Engine
objects and the wrapper containing its prepared identity are rebuilt together.
Public wrapper layouts and embedding ABI 5 are unchanged; the prepared engine
identity changes, so incompatible bytecode cache entries are refused.

## Behavioral evidence

The focused `--failed-code-only` matrix covers:

- Plain, bound, deep-bound, call/apply/spread and constructor aliases; healthy
  proxies targeting failed code, failed proxy handlers and failed trap getters;
  object proxy get/set/has/own-key/descriptor/prototype/extensibility operations.
- Getters, setters and coercion; native/data/neutral functions; eval, indirect
  eval, Function construction, compile-only eval and retained compiled inputs
  selected through a healthy context.
- Promise callbacks, thenables, exported resolvers, native jobs with separate
  execution/callback/activity ownership, async/generator/async-generator paths,
  dynamic import and a pending top-level-await module.
- Failed native module dependencies before initializer side effects, retained
  module-owner GC, and initialized module namespace data after marking.
- Catchable healthy-caller refusals, original uncatchable exception identity,
  skipped author catch/finally and a consumed-abort-marker counterexample.
- Both FATAL and RECLAIM wrapper teardown profiles, retained ordinary departure
  behavior, healthy wrapper reuse, and exact forbidden-side-effect counters.

A separate three-context fixture interrupts B only after it has exported a
closure over a stack local and a plain data object. After complete engine
unwind and an explicit raw mark, A cannot execute the closure, A/C can still
mutate the data, B's finally did not run, and teardown releases the retained
references. This is an engine-only containment fixture. It does not reset a
wrapper latch, certify native consistency or establish a browser recovery path.

Nine source mutations independently remove function, constructor, compile,
module-owner, native-job or proxy-target gates, replace an original abort,
retain a consumed abort marker, or omit the module context hold. Each must
compile and trigger its named assertion or the engine's specific GC reference-
count invariant. Build errors and timeouts do not count. The constructor mutant
is caught by the healthy construct trap around a failed target; the module
hold mutant trips the GC refcount invariant before any anticipated UAF.

## Validation and acceptance boundary

Focused and full results are qualified by final prepared identity, retained
coherent binaries, build profile and exact selectors in the source closeout.
Use these entry points:

```sh
CCACHE_DISABLE=1 JS_RUNTIME_KEEP=1 JS_RUNTIME_FOCUSED_SELECTOR=--failed-code-only \
  bash tools/test_js_runtime_host.sh
CCACHE_DISABLE=1 DOM_HOST_KEEP=1 bash tools/test_dom_host.sh --realms
```

The runtime focused selector builds both engine profiles. Leak-enabled runs
use retained binaries outside the sandbox when thread attachment is denied;
ASan/UBSan remain on the wrapper, and the fully instrumented engine keeps its
existing engine-UBSan exception. A generic ptrace hint is not the diagnosis:
attachment EPERM with TracerPid 0 was established, and leak detection remains
enabled on the supported outside-sandbox runs.

DOM regression checks cover complete shipping behavior and instrumented realm,
retirement, departure, Streams and installation paths. Unchanged DOM/native
objects may be reused only with matching public layouts and source; rebuild
the engine and the wrapper object embedding its identity for each cohort.
F4's accepted native-call query has its own focused selector and contract;
execution-scope and executable-failure facts grant no browser caller authority.

The next cut supplies the distinct original-task/global-health records,
affected-set completeness, generation-bound recovery protocol and unchanged
accounting. DOM/provider certification, host adoption, guest/WPT and any
required P5 recovery evidence follow under the brief. None is established by
clearing a failure and executing one sibling, and F16 remains in progress.

## Frozen source closeout

Final prepared identity:
`6d87fe1aaafc03a38b5dddae1875fef27ea6cbb2bfc1ca4fbaea2b1f430c073e`.
Patch 0024 SHA-256:
`1dad0a03f2f3a847696748ef435a75a923db0ebc637479a3ccc7496757d63600`.
The source combines accepted patches 0022/0025 with F16 patches 0023/0024;
wrapper and instrumented engine objects are rebuilt against that cohort.

| Final check | Shipping | Fully instrumented |
|---|---:|---:|
| Failed-code matrix | 177 checks | 177 checks |
| Execution scopes | 119 checks | 119 checks |
| Accepted F4 native-call facts | 324 checks | 324 checks |
| Accepted F4 global-this | 67 checks | 67 checks |
| Complete runtime | 13,963 checks | 61,311 checks |
| Compiler refusal | 40 checks | 40 checks |

All pass with zero fixture failures; complete runtime teardown reports zero
live allocations. Shipping raw construction and both engines' fatal/reclaim
invariant paths also pass. All nine guard mutations are caught against the
final prepared source, using ASan and leak detection with coherent dependent
objects. The module-owner mutation is caught at the GC refcount invariant,
not counted as an unobserved use-after-free.

Complete shipping DOM passes **322,022 checks**, including **12,941 native
allocation cuts**. Instrumented routing (**678**), retirement/provider queues
(**9,770**), departure (**353**), cross-realm Streams (**104**) and child
installation/engine budgets (**1,306**) pass. These are regression checks with
wrapper containment unused; they are not native recovery certification.
Unchanged DOM/native source and public layouts were verified before reusing
their objects; the engine and identity-bearing wrapper were rebuilt/relinked.

Contract/header checks, six preparation tests, allocation/global/visibility
audit, tracker, shell syntax and diff/stale-reference checks pass. Target
libjs/runtime consumer link, including the new raw exports, passes. No guest,
WPT or P5 recovery acceptance is claimed by this dormant foundation cut.

## Merge-forward qualification: F4 settings scopes

PR #573's test-harness conflict was resolved against `userland` `e7708cec`,
which includes accepted F4 settings patch 0026. Both F16 selectors and the F4
settings selector remain present; the complete realm matrix also retains both
lanes. The shell focused-selector allowlist accepts `--settings-scopes`.
No F16 engine patch or failure policy changed in this resolution.

The coherently rebuilt merged engine has prepared identity
`48edfb6260cd8fbf7f36cc42dd3532a6041715ac3c64634db09adac2b4f01bb1`.
Both engine profiles pass containment (**177**), execution scopes (**119**),
settings scopes (**724**), native-call facts (**324**) and global-this (**67**).
Complete runtime passes **14,686 shipping checks** and **62,034 instrumented
checks**, zero failures and zero live allocations, with leak detection enabled
outside the sandbox. Header/manifest preparation (six tests), teardown audit,
tracker, shell syntax, stale-reference and diff checks pass.

The earlier DOM/mutation totals qualify the preceding frozen cohort; those
campaigns were not repeated for this harness merge. The new F4 native settings
body remains a separate quiescence obligation for the future recovery protocol;
entry/incumbent getters and a successful raw context mark certify no native
unwind on their own.

## Review correction: optimized native iterator entry

The regression reproduced the reviewed bypass against the merged engine:
ordinary `JS_Call` refused the failed callback, but `for...of`, spread,
`Array.from` and `Iterator.from` still invoked it. The initial probe reported
nine failures, including missing native scope and context-pair facts.

Patch `0027-optimized-iterator-entry.patch` guards that direct callback and
brackets it with execution-scope and native-context records. It preserves the
incoming callback context, argument convention and unboxed result; normal,
exceptional and nested return restore the enclosing records. The sibling native
iterator dispatch already passes through guarded `JS_CallInternal`. Existing
patches 0023–0026 retain their hashes. Native-context contracts now describe the
optimized dispatch explicitly.

Prepared identity:
`da43badc52c71c121b1be56f8f08077531c31fcacdb9a82b6751f6baad739b94`.
Patch 0027 SHA-256:
`314ee7bd15a9e88e766ffcee19d79995f4030b31dbfdd6af1b8ca2158a4aa467`.

The focused command builds and runs both engine profiles:

```sh
ASAN_OPTIONS=detect_leaks=1 CCACHE_DISABLE=1 \
JS_RUNTIME_FOCUSED_SELECTOR=--failed-code-only \
bash tools/test_js_runtime_host.sh
```

Both profiles pass **244 containment checks**, execution scopes **119**,
settings scopes **724**, native-call facts **324** and global-this **67**.
Complete runtime passes **14,753 shipping checks** and **62,101 instrumented
checks**, zero failures and zero live allocations. Both compiler-refusal
matrices (40 checks each), shipping raw construction and both profiles' fatal
and reclaim invariant checks pass. ASan/UBSan and leak detection remain enabled;
leak-enabled runs use the supported environment outside the attachment-restricted
sandbox. Header/manifest checks, six preparation tests and teardown audit pass.

Complete shipping DOM passes **322,022 checks** and **12,941 native allocation
cuts**. Instrumented routing (**678**), retirement (**9,770**), departure
(**353**), Streams (**104**) and child installation/engine budgets (**1,306**)
pass. Unchanged DOM/native objects are reused with rebuilt engine and
identity-bearing wrapper objects; these checks qualify this prepared identity.
Tracker, shell syntax, stale-reference and diff checks pass. These remain
entry-boundary regressions with wrapper recovery disabled, not native recovery
certification or guest/WPT/P5 acceptance.
