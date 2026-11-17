# F15 — scoped native access and explicit compatibility export

Proof baseline: merged `userland` `55f9ee30` (#642/#643), embedding ABI 5,
bytecode ABI 6, engine digest
`e0ab3d653a7a184e84f61cce94aa238768d14eef27b2db949dca6ab3f02e9c6f`.
This follows [actual realm owners](IFRAME_F15_REALM_OWNERS.md) and the
[export-lifetime proposal](IFRAME_F15_EXPORT_LIFETIMES.md).

## Implemented private boundary

`js_owner_runtime_scoped_run` invokes a new native callback contract against
the original ticket's actual realm. Before publishing its borrowed context,
it admits an independent bounded loan. The callback returns one owned engine
value or `JS_EXCEPTION`; the wrapper performs returned-value/exception diagnostic
and native-finalizer cleanup while the loan remains alive. A shared execution
body preserves the source path's deadline, settings, error and cleanup behavior.
The loan also spans `complete_run` and the job draining it performs.

The invocation-only opaque scope and borrowed context/values must not escape
into persistent native storage or later continuations. Like existing realm-run,
an exceptional completion may leave queued engine work; that work does not extend
the scope's lifetime. Its engine/native ownership remains a separate obligation.
This is a private C callback contract, not validation of arbitrary stale scope
pointers, a persistent value-root facility or a shipping checked profile.

`js_owner_scope_owner` copies the original immutable owner ID from the live
loan. `js_owner_scope_promote` explicitly reserves an independent owner ticket,
including after reentrant close of the admitting ticket. A full holder pool or
exhausted issuer refuses without changing the loan or output. Copying a scope
pointer does not promote it. Promotion preserves identity and holder lifetime;
it does not replace engine tracing or grant execution/security authority.

`js_owner_scope_export` is an explicit compatibility path. It uses existing
ABI 5 context-getter admission, records runtime-wide sticky `unknown_escape`
before publishing the context and leaves output unchanged on refusal. That
context retains its ABI 5 lifetime after the scope and ticket close. Failed or
cancelled getter admission publishes no context and sets no new veto. Ordinary
public raw operations inside a scope retain their compatibility promises; there
is no ambient exemption.

## Admission, settings and boundedness

The new operation shares existing entry/outcome, ABI, lifecycle, idle-turn,
clock, cancellation and budget rules. Retired/staged realms refuse before the
callback. Native nested invocation returns BUSY without clearing an outer outcome
alias, adding a loan or renewing the deadline. Real loan-capacity and issuer
refusals expose no context. Allocation failure retains its existing sticky/global
classification and releases the admitted loan after cleanup.

With existing host settings selected, the new borrowed callback uses the same
entry-owner and absent-incumbent setup as source execution. Its private trampoline
carries the explicit scoped contract. Existing legacy settings/callback bodies
keep their raw-export vetoes. No new origin, context/target identity, exposure,
cache or security contract is chosen. Only the new callback body has shorter
borrowed-view obligations; a constructor or surrounding scope does not shorten
an old signature's promises.

The scope is bounded stack storage. It uses the arena and ticket/loan pools
already charged at selected runtime admission and adds no hidden registry or
allocation. The wrapper remains **13,080 bytes**. Public constructors, headers,
exports, defaults and engine allowance are unchanged. No DOM migration, root
release, realm/descriptor/context reuse or reclamation is implemented. C3 and
independent graph/native/creation/provider obligations remain gates. An unset
compatibility veto or zero holders is not a release certificate.

Quinn agreed the private boundary and file ownership. F15 changes owner helpers,
the scoped wrapper and shared execution/settings dispatch; her independent F4b
FinalizationRegistry scheduling owns job dispatch/checkpoints and its engine/DOM
host bridge. No unmerged F4 changes are imported.

## Validation

- Focused host: **195 shipping / 571 instrumented checks**, zero failures.
  Real callback allocation sweeps cover **7 / 54 cuts per root policy**;
  holder close during allocation failure leaves no ticket or loan, and both
  teardown policies balance engine roots and native metadata. ASan/UBSan and
  LeakSanitizer remain enabled outside the sandbox.
- Shared host/guest cases cover distinct mapped children, scoped settings without
  implicit export, holder pressure and live-loan promotion after close, nested
  BUSY/outcome aliases, returned-value and exception finalizers, queued native
  callbacks, explicit raw-export lifetime, incidental public getters, ABI/body/
  name refusal, retirement/staging and cancellation. Deterministic host deadline
  tests show direct native evaluation cannot bypass the guarded turn.
- **16 negative controls** reject wrong callback context, rejected promotion after
  close, false full-pool success, legacy trampoline substitution, omitted explicit
  export retention, lifecycle bypass and loan end before draining. Only named
  behavioral assertions count, not compilation failures or invariant aborts.
- Actual cross-built Os64 consumer: **660 checks**, target heap integrity/balance,
  exact parent-observed exit **`0x4f520500`** and complete PASS. The frozen five-file
  closure includes matching libraries; source/build-input/object hashes, command
  and serial log are retained. Source disk/boot images remain unchanged. No P5
  acceptance campaign is implied.
- Default CLI **199 + 40 checks**, shared test262 builder **4/4**, source-preparation
  tests **6**, strict target build, public header/symbol/production-link audit
  (**229 engine / 84 runtime exports**), teardown inventory and wrapper layout
  pass. The engine export increase belongs to merged #642; this PR adds none.

Full runtime: **35,421 shipping / 123,719 instrumented checks**, zero failures
and live allocations, plus the compile-failure and fatal-invariant regressions.
DOM scheduled settings: **5,570 shipping / 6,066 instrumented checks**.
Both profiles also pass departure **353**, retained Streams **104** and
finalization settings **234** checks each, with leak detection enabled.
F15 remains `code`; this milestone does not close its exit.

```sh
ASAN_OPTIONS=detect_leaks=1 JS_RUNTIME_KEEP=1 \
    JS_RUNTIME_FOCUSED_SELECTOR=--owner-scopes bash tools/test_js_runtime_host.sh
ASAN_OPTIONS=detect_leaks=1 python3 tools/test_js_owner_scopes_mutations.py \
    --artifacts <retained-runtime-artifacts> --output <fresh-output-directory>
```

Negative controls demonstrate oracle sensitivity, not positive sanitizer/leak
evidence. Initial test-class registration and missing-declaration build failures
were corrected and excluded from passing evidence.
