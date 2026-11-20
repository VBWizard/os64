# F15.22 — selected scalar C3 lifetime adapter

## Baseline task list — 2026-11-19

- [ ] Consume F4's reviewed #682 contract; keep original owner, stable context and publication generation separate.
- [ ] Select a bounded charged cluster-owned registry and a runtime-scoped monotonic context issuer.
- [ ] Separate fallible original-owner/context registration from scalar read-only observation.
- [ ] Enumerate known proxy/cache/Location roots and aliases, retaining explicit unknown exposure/graph/borrow categories.
- [ ] Close live observation before cluster drain and detach native references before storage end.
- [ ] Prove publication, lifecycle, capacity, issuer, mapping, reentry and teardown counterexamples.
- [ ] Freeze matching integration evidence, publish a stacked PR and address Codex findings; Chris merges.

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
