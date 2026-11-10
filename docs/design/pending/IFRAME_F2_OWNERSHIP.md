# F.2 — binding ownership and cross-document adoption

Design brief, 2026-11-10. Inspection baseline: `03a18579` (`userland`,
including F.1 PR #532). This is tracker slice **F.2**, within IFRAME.md's
F1 foundation stage. It is a proposal for design review, not an as-built
record. No implementation or new test result is claimed here.

## Result and boundary

A native harness can bind two or three independently parsed documents to
F.1 realms of one runtime, exchange their DOM objects, and move nodes
between their documents. Moves preserve native and JavaScript identity,
creation-realm prototypes, listeners, custom-element definitions and
control state. Mutations after the move reach the destination's document
services. The libraries keep backing storage alive through finalizers and
release it in a defined root-teardown sequence.

Yonder continues to create one binding. This cut adds no frame loading,
WindowProxy, cross-origin window access, independent realm retirement or
new scheduling policy. F.3 supplies retirement/job filtering, F.4 supplies
the window security boundary, and F.6 onward integrates Pages. Calling
`os64_dom_drain` on one child is not a substitute for those contracts.

The current tracker stays at fourteen slices. The implementation can use
separate commits for the native lifetime foundation, bindings, and proofs;
if those cannot be reviewed coherently, revise the cut before publishing
implementation rather than calling an incomplete ownership change done.

## Design correction: record creation before wrapping

IFRAME.md's wrapper paragraph was incomplete: looking up the node's current
document when first making its wrapper is wrong after adoption. A node has
a creation realm even when no JavaScript wrapper has been materialized.
The [DOM creation algorithm](https://dom.spec.whatwg.org/#concept-create-node)
selects the node document's realm at creation. Adoption changes its node
document, while preserving its identity and realm.

The required counterexample uses three realms. Create a container in A,
adopt it into B, set its innerHTML in B, then adopt the container into C
before reading the new descendant. The container's prototype remains A's;
the descendant's prototype must be B's, and both ownerDocument values are
C. Neither the caller's realm, the container's creation realm, nor the
first reader's realm is the descendant's creation realm.

Record this distinction in native metadata, not just `DValue`. A wrapper
cache keyed by the node gives one object across the cluster. Construct its
prototype using the recorded creation document's binding. Existing objects
are returned unchanged. A clone/import is a new node and records its own
creation document; it does not copy that field from its source.

## What the source currently ties together

Paths below are relative to `userland/` at the inspection baseline.

| Site | Existing assumption | F.2 change required |
|---|---|---|
| `libhtml/internal.h`, `HDoc`, `HMeta`; `libhtml/core.c`, `os64_html_owns_node` | `document_id` identifies the allocation arena; holds, blocks, reclamation and snapshot pins belong to that arena | Separate storage ownership from node document and creation document; preserve allocation accounting |
| `libhtml/dom.c`, `ours`, `node_free`, `subtree_free`, `d_collect` | A detached subtree can be reclaimed using one document's ledger | Collect a mixed-storage subtree without freeing from the wrong ledger or missing a borrowing snapshot |
| `libhtml/fragment.c`, `isolated`, `d_transfer_blocks` | A temporary parse shares the destination's mark and transfers a whole staging ledger | Preserve creation/current-document metadata when staging commits; failure publishes neither nodes nor accounting |
| `libhtml/range.c`, `os64_html_range_set`, removal hooks | Ranges and boundary holds belong to one native document | Keep live boundaries correct across removal/adoption and later moves between roots |
| `libpage/state.c`, tree plans and `os64_page_node_clone` | The state that performs a verb also owns its nodes and dirty control records | Stage source/destination state changes together; import reads source state and writes destination state |
| `libdom/core.c`, `os64_dom_create`, `d_context`, `d_callback` | Registered anchor class refuses a second binding; a valid context is the primary | Runtime-wide class registration, per-realm anchors, explicit operation ownership |
| `libdom/node.c`, `d_find`, `d_wrap_constructed`, `d_node` | One binding owns the wrapper cache and accepts only its arena's nodes | Cluster identity cache; brand/member checks separate from operation routing |
| `libdom/implementation.c`, `d_node_document`, `d_made_in`, `document_take` | Detached roots remember document in an optional wrapper; windowless documents share one arena | Native document bookkeeping for wrapped and unwrapped nodes; explicit cross-document adoption |
| `libdom/custom.c`, `d_custom_adopted` | The binding performing adoption owns the relevant definitions and reaction queues | Preserve an element's definition owner and order cross-binding reactions |
| `libdom/mutation.c`, `changed` | One binding consumes each document's native notifications | Deliver source removal/destination insertion to their owners, including observers registered elsewhere |
| `libdom/kept.c`, `DKept`; Canvas and Range native records | Finalizers can dereference the creating binding until engine destruction | Keep allocation owners stable; do not migrate these records merely because an element moves |
| `apps/yonder/scripts.c`, `retire`, `yonder_scripts_tree_gone` | Retirement destroys the runtime; layout can delay binding free | Preserve the single-document host path; expose no child-free shortcut |

The implementation audit includes the other primary-runtime call sites in
`animation.c`, `classic.c`, `custom.c`, `event.c`, `fetch.c`, `idle.c`,
`importmap.c`, `intersection.c`, `observer.c`, `performance.c`, `pointer.c`,
`report.c`, `resize.c`, `timer.c`, `validity.c` and `webanimation.c`.
Registering the core classes conditionally is insufficient: each modern
interface's registration and per-context prototype installation must be
examined too.

## Ownership decision

Use a runtime-owned DOM cluster and a native lifetime group for its
participating arenas. Keep arenas separate and keep their allocations
charged to their original owners. Adoption does not copy a subtree, merge
arenas, reparent a JavaScript object, or transfer a permanent chunk containing
unrelated nodes. The group coordinates liveness and snapshot safety across
those separate arenas.

Each native node needs three stable facts:

- **Allocation owner:** the arena whose blocks/free lists and counters must
  release its storage. The existing `document_id` remains an allocation
  identity; audit and rename claims that equate it with ownerDocument.
- **Creation document:** immutable, including nodes created by parsing,
  fragments, cloning, XML, templates and shadow attachment. Its association
  with a realm survives adoption. libhtml stores a native document token,
  not a JSContext or JSValue.
- **Node document:** mutable, including detached nodes with no wrapper.
  Adoption updates this for the affected descendants and attached attributes.
  Windowless documents are explicit document records too.

The first implementation may use pointers in private node metadata; measure
that overhead on the HTML corpus and reclamation tests before selecting a
compact representation. Do not hide it by increasing existing limits.
Ordinary documents use a singleton lifetime group so they exercise the same
invariants. Group membership is native memory membership, not origin access.

### Holds, pins and reclamation

A hold protects node identity and is released against the allocation owner,
regardless of subsequent adoption. Retain source-compatible hold/release
entry points, but resolve the physical ledger explicitly; a document argument
must name a member of the node's lifetime group. Mutation entry points use
the node document instead and still reject unrelated documents/groups.
Audit callers of `os64_html_owns_node`: storage ownership, group membership
and mutation authority become different queries, never one widened predicate.

Put mixed-tree detached-unit bookkeeping at group scope. A unit may contain
nodes from several arenas; its hold sum includes native and parser holds
from each. Reclaiming it dispatches each node, private attribute and text
allocation to its physical ledger. Recycled node metadata is cleared before
recording a new creation document. Preserve fragment replacement's bounded
reclamation and the existing no-full-live-tree-scan fast path.

Keep per-document mutation versions for change readers and layout invalidation.
Use a separate group epoch for storage retirement and snapshot protection.
A snapshot pin records both its document-local version and the group epoch;
replaced storage and detached units are protected until pins that could have
borrowed them leave. Change-log entries retain explicit node/payload holds.
Raw parser references remain counted until their parser releases them.

A group-wide oldest-pin check is the conservative initial reclamation rule:
an old snapshot in A may delay reclamation in B. This is a bounded-retention
tradeoff, not a claim of independent reclamation. Test stalled pins, releasing
pins in either order, cross-arena text replacement, re-adoption, and churn.
Do not use a permanent pin to keep exported nodes alive; that would disable
ordinary reclamation for the source arena. Keep group epoch and document
revision overflow fail-closed, as identity/pin serial exhaustion is today.

The cluster adopts its first arena's existing singleton lifetime group,
preserving that arena's current holds and pins. Join an additional arena
before its DOM binding is installed, with its
parser finished and no external snapshot pins, change readers or node holds.
Validate those preconditions before mutating membership. An existing page
state with no external borrowers can be attached, but its budget must be
admitted too. This restriction is on initial group admission, not on moving
nodes after admission. Incremental frame parsing will use group-aware parser
construction when F.6 integrates the host; do not silently join a live parser.

Once admitted, document records, bindings, states and arena descriptors remain
owned by the cluster until whole-runtime teardown in this cut. Individually
reclaimable nodes and replaced buffers still reclaim normally. F.3 must
specify departure and retained-document limits before exposing child retirement.

### Native state and adoption transaction

Use one internal plan for explicit `adoptNode` and the implicit adoption
performed by insertion/replacement. Prevalidate target hierarchy, root/shadow
restrictions, depth and cluster membership before detaching anything. Stage
fallible control-state migration and required bookkeeping before commit.
An allocation refusal before commit leaves both trees, state and counters
unchanged. Publish native changes without entering JavaScript; deliver
reactions afterward through the existing guarded task.

The commit applies source removal first, including live-range boundary
updates, form-owner repair, mutation records and disconnected reactions.
It then updates node-document metadata, including shadow descendants and the
HTML template-content adopting steps, and finally performs destination
insertion when requested. Explicit adoption alone leaves the node detached.
A Document is not adoptable; a ShadowRoot is not independently adoptable.
Use the pinned standard's distinct exception types. `importNode` copies into
the destination and leaves the source tree and wrapper intact.

Control state belongs to the moving node: dirty input/textarea values,
checkedness, selection offsets, selectedness, validity and other retained
control records need an allocation-safe source-to-destination plan. Preserve
any bytes borrowed by old source snapshots until their pins release; do not
free them when the state table entry changes owners. Repair destination
form associations using its tree. Source changes must not dirty an unrelated
sibling's page state.

Live Range registrations follow their current boundary root when a setter
changes documents, including ranges in a detached subtree that is then
adopted. Keep their JavaScript creation realm separate from that native
registration. Mutation-observer registrations are resolved by target across
the cluster; records return to the observer's binding even after its target
moves. A document-local notification must not lose a foreign observer.

Preserve the element's existing custom definition and callbacks; do not look
up a same-named destination definition for an already-custom element. Make
reaction ordering explicit at cluster task scope so source disconnection,
adoption and destination connection cannot be reordered by separate binding
queues. Newly created elements use their new node document's registry.
Account for callbacks that re-adopt the node or throw during delivery.
`definition_for` currently searches a binding by element name; record a stable
definition owner/reference at upgrade so later adoption cannot change it.

Existing Attr adoption intentionally detaches an attached Attr (`attr.c`).
Preserve and label that existing behavior; do not turn it into an implicit
standards correction in this slice. Attribute wrapper identity and owner
bookkeeping must nevertheless survive cross-binding operations.

## Binding and operation routing

The proposed public surface is:

| Operation | Contract |
|---|---|
| `os64_dom_cluster_create(runtime, limits, outcome)` | Create a native cluster for this runtime with explicit aggregate limits |
| `os64_dom_create_in_realm(cluster, realm, document, state, options, outcome)` | Admit and construct a binding; transfer document/state ownership to the cluster on success |
| `os64_dom_cluster_drain(cluster)` | Stop/drain the cluster's binding references before engine destruction |
| `os64_dom_cluster_free(cluster)` | After engine destruction and release of layout borrowers, free native bindings, states, documents and group |

The types are opaque `os64_dom_cluster_t` and a limits structure with the five
native byte caps named below. Failed admission leaves document/state ownership
with the caller. Once construction begins, the failure path retains its native
records on the poisoned cluster for root teardown; callers receive an explicit
consumed-ownership result so failure cannot make them free backing twice. Use
an output binding/admission result for that distinction, not a NULL pointer
with ambiguous ownership.

The legacy constructor owns an implicit singleton cluster, continues to borrow
its document/state, and preserves its existing drain / engine destroy /
binding free sequence. Explicit-cluster members use cluster drain/free, not
individual public drain/free. Construction uses private cleanup helpers rather
than accidentally draining healthy siblings through a public compatibility
entry point. No realm-run/file/argument API mirrors are needed.

A cluster records the runtime, admitted realms/documents, class registration,
wrapper identity map, lifetime group and resource accounts. One binding per
realm is admitted; the ceiling is F.1's 65 realm slots. Duplicate membership,
a foreign runtime, an arena already belonging to another group, or a second
cluster for the runtime is refused before publishing a binding. The library
continues to leave libjs's runtime/context opaque slots alone.

Class IDs/definitions are registered per runtime; anchors, prototypes,
constructors and globals are installed per realm. A class already registered
by this cluster is reused, not mistaken for an installed prototype in the
new context. Construction runs through a guarded realm task; failure before
publication cannot leave an accessible half-binding. After installation has
begun, failure poisons the cluster and requires whole-runtime teardown in
F.2. Do not promise child-only rollback before F.3 proves it.

Treat the following as separate inputs to a native operation:

| Input | Used for |
|---|---|
| Actual callback context | Engine exception/conversion behavior and membership validation |
| Function's defining binding | Constructor/global services and explicitly captured method state |
| Receiver's creation binding | Stable wrapper/native-record allocation owner and stored definitions |
| Receiver's current node document | DOM mutation, geometry, form state, URL base and document host services |
| Requested new node's document | Creation metadata, prototype realm and initial custom registry |
| Enclosing runtime task | Deadline, stack guard, jobs and sticky failures |

`d_callback` keeps the captured defining binding; it does not make that
binding the owner of every receiver passed to the method. Receiver helpers
brand-check and resolve the corresponding record/document. Borrowing A's
method to operate on B's node is a required case. Raw engine calls use the
appropriate context explicitly; no mutable current-binding/context swap.
The F.1 CFunctionData caller-context correction applies here too.

A context must be a registered, usable realm of the cluster, and a receiver
must have the expected brand and cluster membership. These checks do not
implement same-origin policy. The F.2 harness exchanges raw objects through
trusted native setup; shipping Yonder exposes no multi-document references.
F.4 must authorize window/document acquisition before the host enables frames.
Do not derive authority by comparing URL strings or treating equal runtimes
as equal origins.

Route timers, observers, rAF, report hosts, rejection taking, module-facing
services and event entry tasks through the binding's F.1 realm. A nested
cross-realm operation borrows the enclosing task and never renews its budget.
No F.3 cancellation split or F.4 HTML settings-object attribution is claimed.

## Native lifetime table

| Object/account | Strong owner | Changes when a node is adopted? | Final release in F.2 |
|---|---|---|---|
| Runtime and realm contexts | Host/runtime | No | After draining the cluster's bindings |
| Cluster, document associations and anchors' native backing | Host/cluster | Membership remains | After engine destruction and binding free |
| Arena descriptor and permanent chunks | Cluster lifetime group | No physical transfer | After native holds, snapshots, readers and state owners release |
| Node wrapper and cached Attr/style/collection views | Cluster identity lookup; creation binding's retained records | Node document changes; JS identity and prototype do not | JS references drain before engine; native records after engine |
| Listeners and custom definitions | Stable target/definition records | Preserve callbacks; route tree services by current document | After engine; no source-only drain while a target survives |
| Page control state | Current document's state; old snapshots retain old buffers | Staged migration | After snapshot release and state teardown |
| Live Range / StaticRange | Creating binding record plus native boundary holds | Removal updates live boundaries; static boundaries retain nodes; setter can choose another root | After engine and boundary releases |
| Selection | Its document binding | Remains document-scoped | Binding teardown |
| Canvas/context/path/gradient/native pixel storage | Creation/native allocation owner | Preserve context identity/bitmap; resolve destination geometry/damage services | Finalizers while binding lives; native sweep handles reclaim teardown |
| XHR/Blob/ArrayBuffer kept bodies | DKept's creating binding and refcounts | No migration or counter reset | Last release or post-engine kept sweep |
| Queued clones, requests and task callbacks | Enqueuing binding | No implicit transfer | Cancel/drain before engine destruction |
| Borrowed Web Animation effect data | Effect owner plus layout borrowers | Old layout keeps old data; destination service routing is explicit | After engine and final tree-gone acknowledgement |

Root teardown has explicit phases: stop host deliveries for all members;
drain all binding JS/task references; destroy the shared runtime once; free
binding/native records whose layout borrowers are gone; release page state,
readers/pins and documents; free the group and cluster. Keep borrowed effect
backing through the existing tree-gone handshake. Exercise both libjs teardown
policies; RECLAIM must free native payloads even when finalizers never ran.

F.2 tests dropping either host's borrowed document/state handles first while
the explicit cluster owns backing. Provider opaque objects remain valid until
cluster drain has stopped their calls. This does not free an active host or
implement child retirement. Suppression of jobs,
post-departure callback policy and individual binding drain belong to F.3.

## Bounded foundation

Do not multiply a root budget by the binding count. A cluster is constructed
with aggregate native limits for HTML arena storage, page state, DOM records,
kept bodies and Canvas payloads, enforced before allocation in their owning
allocators. Each binding/arena's existing local cap remains an additional
restriction. Charges include headers/capacity, staging peaks and retained
backing; adoption changes no physical-storage charge.

For the multi-document harness, start with one existing root allowance per
category: 64 MiB HTML, 16 MiB page state, 8 MiB DOM, 256 MiB kept bodies and
128 MiB Canvas (the binding's `CANVAS_PAGE_BYTES` ceiling). Canvas retains
its existing per-context limits too; no second allowance is inferred from frame count.
Test with deliberately tiny explicit caps as well as the root allowances.
The legacy single-document API preserves its current limits. These are
foundation-test/admission choices, not the final Yonder frame-tree product
budget: F.8 adds cascade/pictures and integrated scheduling/admission policy.

A native cap refusal leaves the runtime usable when the operation has not
published state; a shared JS allocator failure follows F.1's sticky contract.
Cluster bookkeeping itself is charged to DOM storage. Retained arenas and
bindings remain charged until released. Expose counters through existing
census patterns rather than reporting adopted allocations twice or hiding
them from the destination's aggregate account.

## Proof plan

Add native ownership cases to the existing HTML/page suites and a focused
multi-realm DOM fixture reused by host and guest. Each case observes both
identities and the owner that receives work; two bindings with identical
providers are not a routing test.

| Boundary | Required observations |
|---|---|
| Realm installation | Two/three bindings; distinct globals/prototypes; second installation does not overwrite first; foreign/duplicate admission refused |
| Explicit/implicit adoption | Both directions; append, insert, replace, replaceChildren and fragments; original object and expando survive; ownerDocument follows move |
| Never-wrapped nodes | Three-realm example above; parser-created descendants first exposed after several moves; text/comments and shadow/template descendants |
| New creation after adoption | Borrowed factory/innerHTML/clone/import; new node's realm follows creation document, independent of receiver/caller realm |
| Event/custom state | Listeners before/after move; distinct same-name definitions; disconnected/adopted/connected order; nested re-adoption and throwing callback |
| Observers and collections | Observers registered in A on a node moved to B; source and destination records; cached childNodes/style/attributes remain live |
| Native state | Dirty controls; form associations; Attr views; live/static ranges; selection document scope; Canvas identity/bitmap and destination damage provider |
| Storage safety | Different physical arenas in one subtree; pending units; source/destination pins in both release orders; detached and parser-held nodes; XML and fragment staging |
| Retained owners | Release either host's external ownership first; held functions/nodes/Blob/ArrayBuffer/Canvas/effects; all native owners remain valid until cluster teardown |
| Failures and budgets | Allocation cuts through admission, installation and adoption plan; no partial tree/state/counter changes; stable charges across repeated moves; cap below one allocation |
| Execution routing | Child timer/event/observer/report host and rejection queue; borrowed methods; no new cross-realm task budget; sibling remains unchanged on nonsticky refusal |
| Teardown | Repeated two/three-document lifecycles; pending jobs and native payloads; both finalizer and reclaim policies; zero surviving native fixture allocations |

After the positive matrix, mutate creation-document selection to current
document, receiver routing to method owner, wrapper lookup to binding-local,
release to destination ledger, omit source pin protection, omit state migration,
choose destination custom definition, and route child task/report entry to
primary. Each mutant needs a passing baseline and an assertion failure;
compilation failure is not evidence. Use the existing isolated mutation runner.

Run HTML parser/fragment/DOM/reclamation, page state, full DOM (target and
instrumented engines), libjs focused realm/runtime and Yonder scripted-page
regressions. Measure the HTML corpus and long fragment-churn memory costs.
Keep ASan/UBSan and LeakSanitizer enabled; reuse binaries outside the sandbox
when thread attachment is denied. A QEMU native consumer must observe both
realms and report its actual exit code. No P5 test is required until a slice
changes visible frame behavior.

### Pinned WPT evidence

Use the repository's pin `a699fb16767be39ba3e61a8e8b6bef60383f08ee`, not an
unrecorded newer checkout. Compare named verdicts and required/lost sets at
`03a18579` and the implementation for `dom/nodes`, `dom/ranges`, `dom/events`
and `custom-elements`; do not silently rewrite expectations.

The native fixture adapts the assertions from these files without pretending
that Yonder can load their iframes. Record those as adapted host cases, separate
from the unmodified WPT runner's exclusions/results:

- `dom/nodes/node-creation-realm.html`
- `dom/nodes/node-realm-preserved-across-adoption.html`
- `dom/nodes/node-realm-mixed-across-adoption.html`
- `dom/nodes/create-element-realm-after-adoption.html`
- `dom/nodes/attach-shadow-realm-after-adoption.html`
- `dom/nodes/Document-adoptNode.html`
- `dom/nodes/Node-mutation-adoptNode.html`
- `dom/ranges/Range-adopt-test.html`
- `custom-elements/adopted-callback.html`

`node-realm-adoption-after-frame-removal.html` and
`event-listener-not-fully-active-adopted-node.html` additionally require F.3's
actual departure behavior. F.2's retained-owner tests are a prerequisite, not
an assertion that those frame-removal tests pass.

## Design review and implementation acceptance

This brief needs a design read before the ownership refactor. In particular:

1. Accept the creation-document correction and the never-wrapped three-realm
   oracle, rather than making wrapper creation determine realm identity.
2. Review the physical-owner / node-document split and group epoch pin rule;
   confirm the bounded retention tradeoff and initial admission preconditions.
3. Review control-state migration, reaction ordering and cross-binding observer
   ownership; identify a missing native payload before changing teardown.
4. Confirm that F.2's conservative cluster lifetime is a foundation for F.3,
   not permission to free a departed binding or claim last-wrapper reclamation.

The implementation PR must carry measured validation and code review before
its tracker completion flip. Preserve the existing single-document browser
and the remaining iframe debts. The design correction is published explicitly
for review; the original refresh did not settle this storage mechanism.
