# F15.22 — selected scalar C3 lifetime adapter

Implementation PR **#684**.

## Baseline task list — 2026-11-19

- [x] Consume F4's reviewed #682 contract; keep original owner, stable context and publication generation separate.
- [x] Select a bounded charged cluster-owned registry and a runtime-scoped monotonic context issuer.
- [x] Separate fallible original-owner/context registration from scalar read-only observation.
- [x] Enumerate known proxy/cache/Location roots and aliases, retaining explicit unknown exposure/graph/borrow categories.
- [x] Close live observation before cluster drain and detach native references before storage end.
- [x] Prove publication, lifecycle, capacity, issuer, mapping, reentry and teardown counterexamples.
- [x] Freeze matching integration evidence and publish #684 for Codex review; address findings without merging.

Depends on F4 contract [#682](https://github.com/VBWizard/os64/pull/682), final
`b375b5fb`, and the callback milestone #683. This implementation consumes the
agreed C3 inputs without supplying a release certificate or enabling public
checked profiles, physical reclamation, provider release or slot reuse.

## Selected representation

One explicitly selected cluster-owned arena reserves finite binding/context
maps, charged to the cluster's aggregate DOM ledger. Context IDs use runtime
incarnation plus a nonzero monotonic runtime issuer in the charged owner arena;
no address cast, publication-generation ID, eviction or identity reuse.
Default DOM runtimes have no selected owner arena and refuse unchanged output.

Registration validates live grouped membership at quiescence, maps the original
binding realm through the legacy compatibility ticket conversion, copies its
owner identity and closes the temporary ticket. Its sticky raw-retention veto
remains. Mapping is preparation; scalar observation creates no holder or root.
A target not yet mapped refuses rather than inventing an owner or dereferencing
an unvalidated binding. Publication generation and target eligibility are
independent observations.

The adapter closes and drops native record references before cluster drain.
Its charged scalar/storage arena remains through engine finalizer diagnostics
until native cluster free, which occurs after engine destruction. A closed
adapter refuses before consulting runtime or saved native identities. Copied
facts grant no lifetime, origin, navigation or reclamation authority.

## ADDED findings

- **ADDED 2026-11-19 — shared-library boundary:** hidden libjs owner helpers
  cannot be imported by libdom. Explicit selection therefore copies a private
  trusted host bridge. Its wrappers map compatibility owners and copy scalar
  facts without publishing private symbols. The production shared-library
  `--no-undefined` link is part of acceptance.
- **ADDED 2026-11-19 — bridge phase drain:** host bridge calls borrow their
  backing until return. Preparing/provider barriers exclude reentrant native
  operations; drain/poison is checked after each phase before publication.
  Close detaches bridge and native references before releasing values, while
  scalar summary storage remains through engine finalizers.
- **ADDED 2026-11-19 — parallel F4 integration:** #682 includes #680's mutable
  name layout. A separate combined source cohort enables the rename/root-name
  reset counterexamples with fresh DOM objects. Standalone evidence never
  substitutes an old object layout for that combined proof.

- **ADDED 2026-11-19 — existing cluster admission:** DOM registers one cluster
  per runtime and rejects another through its class-registration guard. Keep
  that bound. Two contexts prove fresh serials within an incarnation; separate
  and recreated runtimes prove distinct full context IDs and rejection of old
  copied owners. The C3 contract's second cluster/context case does not enable
  multi-cluster admission in one runtime.

The native bridge preserves the cross-library boundary: libdom imports no
hidden `js_owner_` function. Original-owner mapping remains an explicit legacy
compatibility conversion in the selected host; observation uses scalar
ID-to-original-realm facts without creating holders. Inline copied immutable
origins are diagnostic inputs, not a new origin/member/navigation policy.
The known counters enumerate only the original binding's proxy/descriptor and
Location backing/function roots plus its Window-identity alias. Other bindings,
engine context edges, escaped references and all-native-borrow counts are not
inferred from them. Entry/incumbent/current/native-caller roles remain the
checked scoped runtime observations from #665/#667, separate from these idle
publication snapshots; quiescent observation invents no settings role.

## Implemented proof boundary

The adapter is selected only by a private native host. ABI 5 default constructors
and the public browser remain unchanged. The selected owner metadata gains a
separate context issuer; the target wrapper remains **13,128 bytes**. The real
libjs/libdom shared-library link passes `--no-undefined`; **335 / 155** public
symbols contain no private owner/adapter export, and libdom imports no hidden
owner helper. No engine patch is added. Prepared engine ID remains
`0e34fab233ca0a3e3dfc4f8fabd76774231359899c8134892a4318e8232ac1a0`.

- `DOM_HOST_KEEP=1 ASAN_OPTIONS=detect_leaks=1 bash tools/test_dom_host.sh --c3-lifetimes`:
  **298 checks per profile**, both teardown policies, shipping and instrumented
  engines. The final `--c3-lifetimes-controls` selector passes the same cases.
  Adapter allocation/aggregate capacity, missing mapping, full map, actual
  runtime issuer exhaustion, stale publication, lifecycle, native/task barriers,
  native finalizers and drain during each bridge phase are covered. A closed
  adapter safely refuses select/register/observe after engine destruction while
  its summary shell remains available.
- Seven targeted mutations in the retained compile/link recipes produce
  **14 named behavioral failures** across the two profiles. Controls reject
  generation-as-context-ID, stale publication acceptance, pointer-as-eligibility,
  missing quiescence, missing pre-drain gate, alias-as-root counting and serial
  reuse. Compiler/sanitizer/invariant failures do not count as detections.
- Fresh combined source, local integration `36f3385c0c84c66fc33375e1df993d289e9df076`,
  merges F4 #682 `b375b5fb` with this implementation `c0b28e9a` (and #683).
  `DOM_C3_F4_NAMES` adds the actual mutable-name/root-origin reset cases:
  **302 checks per profile**, both policies, leak-enabled ASan/UBSan outside the
  sandbox. These receipts use fresh coherent DOM objects and do not mix F4's
  different native name layout into the standalone cohort.
- The runtime metadata extension passes **1,053 / 1,125** focused callback
  checks and **42,794 / 143,262** full runtime checks with zero live allocations,
  including raw construction, compiler cuts and five full invariant badges.
  The deliberate unrelated raw-root leak remains detected as a separate
  negative witness. Seven prepared engine inputs and eight reused instrumented
  engine/port objects match #683's retained hashes; target core is freshly built.
- Header, teardown/visibility, tracker, diff and stale-reference audits pass.
  Selected adapter/source and actual shared-library objects are retained with
  their matching host recipes. Leak detection stays enabled outside the sandbox.

Known exposure counts belong to the **original registered binding**; Window
identity is an alias, not an extra strong root. Closed summary counts describe
retained registration cells, not live native borrows. Scalar copied origins and
owner snapshots grant no origin/member/navigation permission. Unknown escaped
roots, engine graph reachability and complete native borrowing remain unknown.
No selected-adapter guest or P5 acceptance is claimed; this milestone is a
native-host implementation and source/shared-library integration proof.

Combined adjacent regressions also pass in both profiles: Window.name
**582 / 686**, browsing contexts **1,078 / 1,078**, WindowProxy **1,426 / 3,726**,
Location context **424 / 424**, Location exotics **1,932 / 3,052** and browser
jobs **801 / 801**. Standalone browser jobs also pass **801 / 801**. The two
native cohorts and exact combined source/recipe are retained separately.
