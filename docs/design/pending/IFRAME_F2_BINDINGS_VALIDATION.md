# F.2b — binding ownership and cross-document adoption

Implementation [#539](https://github.com/VBWizard/os64/pull/539), on the merged
native foundation [#537](https://github.com/VBWizard/os64/pull/537). The accepted
scope is [IFRAME_F2_OWNERSHIP.md](IFRAME_F2_OWNERSHIP.md); native ownership costs
and proofs are in [IFRAME_F2_NATIVE_VALIDATION.md](IFRAME_F2_NATIVE_VALIDATION.md).
Implementation acceptance still requires code review before F.2's completion
flip. This record supersedes F.2b's initial realm-routing checkpoint.

## Implemented contract

An explicit DOM cluster admits independently parsed document/state pairs into
realms of one runtime. Its public admission result distinguishes a refusal
that preserves caller ownership from a consumed pair. Installation after
consumption runs inside a guarded realm task; failure drains and poisons the
whole cluster. Duplicate realms, foreign runtimes, external native borrowers
and aggregate-cap failures are rejected before ownership transfer. Runtime
classes are registered once, with prototypes and anchors installed per realm.
The legacy constructor keeps its borrowed, single-document contract.

A cluster identity cache returns the same node wrapper across members. Native
creation-realm metadata selects the prototype, including when the first wrap
occurs after several adoptions. Current-document metadata routes mutations,
control state, geometry, style, image and navigation services. Neither a
borrowed method's defining binding nor a wrapper's allocation owner selects
those services. Reentrant conversions re-resolve the operation's owner.

Explicit adoption and implicit insertion use the native state transaction.
Dirty controls, live ranges, cached collections, Attr/style views, Canvas
contexts and pixels survive moves. Imports create destination-realm objects.
Additional and inert Document roots remain arena-owned; unwrapped detached
nodes retain their true ownerDocument without a permanent snapshot pin.
Removing ordinary children still permits reclamation.

Custom registries remain per realm. An upgraded element retains its selected
definition; the cluster shares one reaction stack and backup queue, preserving
source disconnection, adoption and destination connection order. Nested
re-adoption and throwing callbacks are exercised. Mutation observers share
creation order and notification state, while each callback and MutationRecord
uses its observer's binding. Listeners retain their callback binding; event
paths use the target's current document. Timers, reporting, rejection taking,
rAF and idle entry use explicit libjs realm APIs and share the enclosing
runtime budget.

Native allocations keep their physical owner. DOM block headers now retain
that owner, so a sibling's release debits the correct account. Removing the
obsolete wrapper-local Document field offsets the larger block header for
ordinary wrappers; the existing 128-byte charged-wrapper bound still passes.
Aggregate HTML, page-state, DOM, kept-body and Canvas ceilings supplement local
limits. Harness root allowances are 64/16/8/256/128 MiB respectively; deliberate
small-cap tests prove refusal, charge recovery and continued sibling use.
No existing product limit was increased.

Web Animation effect backing stays with its allocation owner. Cluster-wide
generations and the oldest borrowing host protect old effect data after a
target moves; destination layout can find the effect. Hosts release layout
borrows before cluster free. This is a lifetime proof, not integrated frame
animation scheduling.

Teardown closes native delivery doors, drains member JS references, destroys
the shared runtime once, releases layout borrowers, then sweeps bindings and
native groups. Binding descriptors remain until sibling releases finish.
Individual drain/free is not a child-retirement API. Provider opaque storage
must survive cluster drain. Both libjs FATAL/finalizer and RECLAIM policies
are covered with zero surviving native fixture allocations.

## Corrections found during integration

Pinned WPT exposed two errors in the native fragment assumptions: template
contents can be adopted independently, whereas a ShadowRoot cannot; insertion
moves a fragment's children and preserves the fragment's own Document, even
when empty. Native insertion, source notifications and assertions now follow
that distinction. Four named WPT cases improve below.

Installing the native template-document association must not advance a tree's
rendering revision: it changes ownership metadata, not rendered tree, style
or control state. The unconditional revision bump invalidated Yonder models
built before DOM installation. Removing it restores the scripted-page suite.

A focused regression also covers native changes to a compiled event-handler
attribute after adoption: refreshing the slot must resolve its current
Document before deciding whether to compile. The test failed under both
teardown policies before that correction; ordinary compiled callbacks still
retain the realm where they were compiled. A second reproducer creates an
animation before admitting a sibling, then moves its target into that sibling
without another animation update. New bindings start with an invalidated effect
index so their first layout discovers effects published before admission.

## Validation

Host checks use ASan/UBSan and LeakSanitizer with `detect_leaks=1`, outside the
sandbox for thread attachment. Retained host objects are rebuilt for changed
units; both shipping and instrumented engine profiles link the same native
fixture. Logs are streamed to bounded files and read in bounded excerpts.
No log is loaded wholesale for validation.

The full instrumented sweep precedes the final handler-refresh and late-admission
effect-index corrections. After those corrections the instrumented realm/cap
probe passes 258 checks, the event probe passes 1,020, and Web Animations pass
278; admission allocation cuts are also rerun. The full shipping-engine run
includes both corrections. The retained full-sweep evidence is not relabeled
as an exact final-source instrumented run.

| Check | Result |
|---|---|
| Three-realm DOM and small-cap probes, both engine profiles | 258 checks per engine profile; zero failures |
| Complete DOM, shipping engine | 43,749 checks; zero failures; 3,879 allocation sweep points |
| Complete DOM, instrumented engine | 308,981 checks; zero failures; 36,607 sweep points; final-delta qualification above |
| Second-binding installation allocation failures | 1,048 cuts per teardown policy; 16,778 checks; zero failures |
| Native HTML ownership/DOM | 800,487 checks; zero failures |
| HTML parser/reference suite | 1,277 native checks; 10,531/10,531 reference cases; 376 roundtrips |
| HTML fragment fixture | 333 checks; zero failures |
| HTML reclamation | 310,833 checks; zero failures; 216,000 churn steps flat at 39,279 bytes / 104 nodes |
| HTML parser fuzz | 113,920 mutations; four chunkings each; 30 seconds |
| Page state | 254,708 checks; zero failures; 415 allocation cuts and lifetime-fatal proof |
| libjs focused realms/runtime | 1,152 checks; zero failures |
| Yonder scripted pages | 24,831 checks; zero failures; 114 completed refusal cases |
| Yonder painting regression | 2,255 checks; zero failures; six corpus paints match |
| Binding ownership mutants | Passing baseline; 8/8 compiled and caught |
| Existing mutation probes affected by the refactor | 11/11 compiled and caught against focused passing baselines |
| Native ownership mutants | Passing HTML/page baselines; 9/9 compiled and caught |
| QEMU DOM consumer | Three actual realms, both teardown policies; parent observes exit `0x446f0100` |

Focused entry points are `tools/test_dom_host.sh --realms` and
`tools/test_dom_host.sh --cluster-failures`; omitting the selector runs the
complete matrix. Retain objects with `DOM_HOST_KEEP=1`, then run
`python3 tools/test_dom_mutants.py <objects> --realms` for the binding
mutants. The native campaign remains `tools/test_iframe_ownership_mutants.py`.
`make -C userland dom-realms-test` builds the guest consumer. The four pinned
WPT groups use `tools/test_yonder_wpt_host.sh <group>` against the pinned
checkout; neither `--update` nor expectation edits are part of this evidence.

The shared host/guest oracle creates a container in A, adopts it into B,
creates descendants there, then adopts into C before first wrapping them.
It proves A's container prototype, B's descendant prototypes, C's
ownerDocument and preserved native/JS identity. The host extends this with
distinct providers (geometry widths 100/101/102), shadow/template/XML creation,
borrowed methods, controls, listeners, observer order, custom definitions,
reentrant adoption, Canvas damage and retained effect data.

The mutant campaign breaks creation-realm selection, receiver routing, shared
wrapper lookup, physical release accounting, preserved custom definitions,
child task/report routing and aggregate DOM limits. Native mutants separately
break document migration, physical node ownership, ranges/templates, sibling
pin protection, state migration and retained snapshot storage. A compilation
failure, timeout or arbitrary exit is not a successful catch.

QEMU uses a frozen SHA-256-manifested cohort of 19 consumer/library files
(5,709,296 bytes), an existing kernel and a disposable disk. The consumer
executes the shared adoption oracle under both teardown policies and verifies
its heap; the parent checks `os64_wait`'s actual status. The network-disabled
boot also reports the unrelated `net_arp_pending_order` test failure. The
acceptance claim is for the DOM consumer, not a wholly passing kernel suite.

The native corpus capacities match F.2a's measured table. The long churn
plateau remains unchanged. F.2a's stalled-pin and old-model recovery costs
remain applicable: HTML B 5,616 → 513,972 → 5,616 bytes, aggregate peak 529,764;
page state 1,909 → 55,586 → 6,000 bytes, peak 63,922. F.8 must budget these
retention costs alongside its integrated rendering accounts.

## Pinned named-WPT comparison

Checkout `a699fb16767be39ba3e61a8e8b6bef60383f08ee`; baseline `03a18579` in an
isolated worktree. Named `(page, test)` verdicts and REQUIRED/LOST sets were
compared, with no expectation or exclusion edits. Both sides have empty
REQUIRED/LOST sets in all four groups.

| Group | Baseline pass/fail/timeout/notrun/precondition | F.2b | Pages |
|---|---|---|---:|
| `dom/nodes` | 7902 / 1279 / 82 / 676 / 6 | 7905 / 1279 / 79 / 676 / 6 | 361 |
| `dom/ranges` | 37377 / 274 / 1 / 4176 / 0 | unchanged | 69 |
| `dom/events` | 389 / 154 / 10 / 11 / 0 | 389 / 156 / 8 / 11 / 0 | 90 |
| `custom-elements` | 196 / 1028 / 34 / 250 / 2 | 197 / 1027 / 34 / 250 / 2 | 189 |

Named FAIL → PASS changes:

- `dom/nodes/adoption.window.html`: `appendChild() and DocumentFragment with host`.
- The same page: `adoptNode() and DocumentFragment with host`.
- `dom/nodes/Document-adoptNode-DocumentFragment-with-host.window.html`:
  `adoptNode() <template> element: content follows via template adopting steps`.
- `custom-elements/registries/upgrade.html`:
  `Elements inside a template contents DocumentFragment node`.

Five other named verdicts change TIMEOUT → FAIL, with no lost passing test:
`moveBefore/continue-css-animation-left.html`,
`moveBefore/continue-css-animation-transform.html`,
`moveBefore/css-animation-commit-styles.html`, and the `Listener of animationend`
and `Listener of animationstart` cases in `EventListener-invoke-legacy.html`.
The three moveBefore cases now reach the missing-method TypeError. The two
animation listeners pass their subtests but the runner downgrades them because
the enclosing page still times out. These remain failures, not new conformance
claims. Range verdicts are identical.

Exclusions are unchanged: nodes omit 47 XML pages; events omit 115 expectation
exclusions and 10 worker/shadow-realm variants; custom elements omit seven XML
pages. The brief's iframe-dependent realm/adoption assertions are adapted in
the native host/guest setup, not reported as unmodified iframe WPT passes.
Frame-removal tests still require F.3 departure behavior.

## Review boundary

Yonder retains its single-binding path. F.2b exposes no frame loading,
WindowProxy, same-origin gate, independent realm retirement, child-only
installation rollback or integrated frame scheduler. Shared-runtime membership
is a native lifetime/brand check, not origin authorization. Raw object exchange
is confined to trusted fixture setup. F.3 and F.4 remain prerequisites for
host frame integration. No P5 acceptance is required for this foundation.
