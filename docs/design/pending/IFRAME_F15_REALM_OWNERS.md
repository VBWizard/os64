# F15 — actual realm owners and checked source access

Proof baseline: merged `userland` `60301c0d`, embedding ABI 5 and bytecode ABI 6.
The shipping engine digest is
`36d2ff8a0092e5ce32441ad2f6defef276a9aa2ec735600f247078cbbaf657d2`.
This implements the next private boundary from the
[export-lifetime proposal](IFRAME_F15_EXPORT_LIFETIMES.md), after
[metadata admission](IFRAME_F15_OWNER_ADMISSION.md). F15 remains in progress.

## Implemented boundary

The explicitly selected private constructor reserves an immutable owner ID for
the actual primary realm. Successful checked, raw and staged child admissions
assign IDs to their actual stable native slots. IDs do not encode slot indices.
A bounded ID-to-slot map lives in the same charged native arena as the ticket
pools. Checked sizing covers map/pool arithmetic and alignment; its actual usable
allocation charge is subtracted before engine construction. Public constructors
allocate no arena and preserve their engine allowance. The runtime wrapper remains
**13,080 bytes**, with no public header, export or engine patch change.

Row-only reservation and opening a ticket for a known owner avoid permanent
internal anchor holders. Checked child creation atomically reserves a row and
first holder after existing setup admission. Metadata refusal publishes neither.
If engine construction subsequently fails, its bounded unpublished row remains
consumed and its temporary holder closes. Existing native/engine failure policy
still applies. No owner row or ID is reset or recycled. Assigned counts include
retired and staged slots; unpublished counts identify consumed rows without an
assigned usable slot, not failed installation of an already admitted realm.

The hidden runtime helpers issue primary/known-realm tickets, explicitly
duplicate/close independent holders, copy scalar owner state, create checked children,
retire the original realm and run source in that realm. Copying ticket bits
aliases one holder; explicit duplication creates another. A closed ticket cannot
resolve or duplicate even after its holder cell is reused. A new actual runtime
rejects tickets from the old incarnation despite repeated local serial numbers.
The caller supplies a live runtime on its owner thread. Outputs are distinct
caller storage as specified in `owner_runtime.h`; copied diagnostics grant no
execution or storage-lifetime authority.

Checked source shares existing entry, lifecycle, source, budget, settings and
complete-run logic. After admission, an independent loan spans source allocation,
engine execution, exception/value cleanup and job draining. Reentrant holder
close invalidates later ticket use while the admitted invocation finishes.
Nested BUSY preserves the outer outcome and loan; nested child admission borrows
its active execution budget. Retired/staged realms cannot become executable
through a valid ticket. Cancellation and global failure still block execution.
Scalar inspection/close remain available during synchronous root finalizers;
new issuance refuses destruction. Neither run nor scalar inspection returns a
raw view or persistent engine value.

## ABI 5 compatibility and remaining gates

Legacy raw exports retain their ABI 5 lifetimes. Selected metadata reserves
bookkeeping up front; setting runtime-wide sticky `unknown_escape` neither
allocates nor changes a normal raw result. The audited sites include context
and realm/settings getters, raw child publication, native settings/callback and
setup bodies, import-base callbacks, rejection routing/reporting/value transfer,
module-handle publication, failure observation and recovery preparation.
Known raw-realm conversion also records the veto. It accepts a known retained
member, not an arbitrary freed pointer. Internal use of a public raw path has
no ambient exemption. Provider callbacks returning strings do not themselves
publish engine pointers; raw engine graph/value/native families still require
separate coverage. An unset veto or zero counters is not a release certificate.

There is no public checked-profile negotiation, DOM ownership migration, root
release, descriptor/context/slot reuse, graph-pin proof or reclamation here.
The private constructor does not select F16 failure-record/recovery profiles;
its scalar failed bit is not a health certificate. C3 entry/incumbent/origin,
target/publication and exposure/cache contracts remain independent. Quinn agreed
the private interface and cumulative admission rules before implementation;
her F4 engine/DOM Promise-lifetime milestone is not imported by this PR.

## Validation

- Full runtime suite: **32,189** shipping and **113,157** instrumented checks,
  zero failures/live allocations, plus the compile-failure regressions. Both
  root teardown policies are exercised. ASan/UBSan and LeakSanitizer remain
  enabled outside the sandbox.
- Real child-construction sweeps: **27** shipping and **570** instrumented
  allocation cuts per root policy. Output tickets remain unchanged on refusal;
  actual assigned/unpublished counts and native/engine heap balance are checked.
  A successful allocation cut retains its original mapping. Actual loan-pool
  capacity and token exhaustion refuse before source execution without wrap.
- Shared host/guest cases cover primary/multiple children, distinct globals,
  alias/duplicate/stale/foreign tickets, owner and holder capacity, retirement,
  staged/failed installation, native microtask reentry, diagnostic finalizers,
  root-finalizer close, nested outcome aliasing/budgets and cancellation.
- Metadata source and actual target object: **23,369 checks each**. Existing
  **28** metadata, **14** admission and **8** runtime-identity negative controls
  replayed. **18** realm-owner controls test wrong mapping, closed-ticket acceptance,
  omitted lifecycle guards, premature loan end, failed ticket publication,
  erased consumed rows and missing raw-export vetoes. Only named behavioral
  assertions count; compile failures or invariant aborts are excluded.
- Actual cross-built consumer in Os64: **546 checks**, target heap balance and
  parent-observed exact exit badge **`0x4f520400`**, complete PASS. The retained
  five-file closure, matching production objects, source/build-input hashes,
  command and serial output are frozen separately from the unchanged source
  disk/boot images. No P5 acceptance campaign is added.
- Default CLI: **199 + 40 checks**; nested shared test262 builder: **4/4**
  selected tests. Public header/symbol-only/production-link audit: **228 engine
  and 84 runtime exports** unchanged. Teardown allocation/global inventory,
  strict production build, wrapper layout, tracker and stale-reference checks
  pass.

Replay the host and failure proofs with leak detection enabled:

```sh
ASAN_OPTIONS=detect_leaks=1 JS_RUNTIME_KEEP=1 bash tools/test_js_runtime_host.sh
ASAN_OPTIONS=detect_leaks=1 python3 tools/test_js_owner_realms_mutations.py \
    --artifacts <retained-runtime-artifacts> --output <fresh-output-directory>
```

`--owner-realms` selects the focused host cases. Existing metadata/admission/
identity mutation tools accept their corresponding retained objects. Negative
controls demonstrate oracle sensitivity, not positive sanitizer/leak evidence.
F15's exit still requires reviewed reachability/native teardown and safe reuse,
separate from F11 browser/P5 acceptance.
