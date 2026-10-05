# libdom

libdom makes the script-visible document over libhtml's mutable tree and
libpage's persistent control state. It uses libjs's pinned QuickJS binding
surface; it neither parses JavaScript nor owns a browser loop.

This is D5a. D5b seats this library in Yonder and proves J3's visible changes,
field preservation and navigation teardown. D4 and D7 remain Fable's slices.

## Ownership and calls

`include/dom/dom.h` is the public native contract. Creation borrows a fresh,
idle runtime, a finished document and that document's page state. One binding
belongs to one runtime. The host grants console output separately through
`os64_js_install_output`; the binding grants no filesystem or network access.

Callbacks capture a private anchor object through function data. Runtime and
context opaque slots belong to libjs and are untouched. Binding translation
units check their compiled-in ABI identity before entering the engine. Class
IDs come from `os64_js_class_id` and are registered in each runtime.

The wrapper table maps native node identities to strongly held engine values.
Looking up a node returns its existing wrapper, retaining expandos across GC,
detachment, reinsertion and model rebuilds. The host drains every value held
by C through the binding registry. Engine-owned properties and function data
follow the engine's reference graph. Finalizers clear engine opaque pointers;
the native ledger owns native storage.

Retire the page and its queued work before calling `os64_dom_drain` outside
JavaScript. Drain is idempotent and also works after a sticky libjs failure.
Then destroy the runtime, free the binding's native ledger, release models
and control state, and finally free the document. Native data survives engine
finalizers. A failed constructor can leave partial engine registration; it
clears native opaque pointers and retained values, and the host destroys that
runtime without evaluating more source. The idle custom installer cannot
distinguish engine-cap exhaustion from host allocation failure; it consumes
the pending exception and reports construction failure. Evaluation's limit
classification belongs to the guarded libjs operation.

## Objects and collections

The D5 surface follows `docs/design/pending/DOM_BRIEFS.md`: document landmarks,
queries, creation, tree mutation/navigation, attributes, text and markup,
and control properties. `window` names the global object. Document landmarks
are read from the attached tree rather than trusted parser-time pointers.

Kind prototypes inherit Node, and the HTML control prototypes inherit Element.
The supported properties and methods are placed on those chains:

| Kind | Surface beyond Node |
| --- | --- |
| Document | documentElement/head/body, child-element access, ID/tag queries, creation |
| Element | tagName, id/className, innerHTML/outerHTML, attributes, tag queries, child-element access and element siblings |
| Text and Comment (CharacterData) | data and element siblings |
| DocumentFragment | child-element access and ID queries |
| HTML input/textarea/select/button | value |
| HTML input | checked (native control-state operations support checkbox/radio) |
| HTML select | selectedIndex |

Node supplies tree navigation, ownerDocument, childNodes, nodeType/nodeName,
nodeValue/textContent and tree methods. A property absent from a node's chain
reads undefined and is absent from `in`; assignment may create an ordinary
expando. Borrowed methods and accessors still validate native receivers.
Interface constructors and a complete DOM surface are outside this slice.

Child collections and tag queries are native live objects with `length`,
`item(index)` and indexed access. A query refreshes against the HTML version;
its node-vector replacement is staged before publication. A held collection
therefore changes after a mutation. Numeric properties are read-only, including
definitions of future indices, and the object refuses preventExtensions so
live growth can continue. Nonnumeric strings and Symbols remain expando keys.
Template contents are a separate fragment:
ordinary children/textContent follow actual children, while innerHTML uses
that contents fragment. outerHTML is read-only in this slice.

Strings cross the native boundary as UTF-8. NUL and lone UTF-16 surrogates
become U+FFFD, while valid surrogate pairs retain their character. HTML names
are ASCII-folded where the operation requires it. Native placement validity
belongs to libhtml; bindings turn its status into an exception.
Element and attribute names use the DOM Standard's separate name-validation
rules. Nullable nodeValue/textContent map null and undefined to empty text;
innerHTML and CharacterData data map null to empty but convert undefined to
the literal string.

## Mutation and control state

Property edits delegate to D3. Attribute writes, including id/className,
delegate to `os64_page_node_set_attr`: it stages prospective attributes and
control effects, then commits the HTML attribute batch before publishing
state. This observes type/constraint/group transitions even when no property
getter runs between them. `os64_html_set_attrs` stages a complete final list,
preserves attribute namespace/order and pinned old bytes, and publishes one
HTML revision. Failure restores its accounting, including peak.

State-aware tree entrances reserve the affected control records before the
native insertion/removal, then publish radio/select effects without another
allocation. Moving controls changes their group/selection state at the move;
a later getter or model rebuild cannot recover unobserved intermediate moves.
Dirty flags retain their meaning as property edits rather than tree effects.
Planning walks moving subtrees, prepares the old and new affected option lists,
and resets clean textarea state only for direct child changes. Radio peer scans
run for moving radio names or IDs that can retarget explicit form owners;
ID planning is skipped when libhtml counts no owned inputs with a `form`
attribute. The count includes detached inputs and template contents.
Unrelated control records stay sparse. Stage lookup uses a temporary link from
reserved records, cleared on commit/refusal. An unrelated text/tree edit needs
no control-state allocation or model publication.

Content setters stage detached text or a contextual fragment. The shared
replace-children entrance reserves once, inserts before the original first
child, and removes the saved original range. Refusal preserves visible
children. Successful detached preparation remains document-owned on a later
refusal; D6 adds reclamation, so this is not an allocation-rollback claim.
Tree methods prepare their return wrappers before a visible mutation.

Cloning uses the shared state-aware clone entrance. INPUT copies its current
value, checkedness and dirty flags; TEXTAREA copies its current value and dirty
flag even for a shallow clone without text children. Other control state starts
from cloned markup. The clone owns its copied state bytes independently of the
source. A clean textarea's current value follows subsequent changes to its
direct children and their CharacterData; unrelated mutations leave it alone.
CharacterData
setters use the state-aware text entrance so changes restored between reads are
observed. Clone preparation reserves its return wrapper before publishing native
state, and cloning does not advance either document or state revision.

HTML and page-state revisions are independent. A host compares both to decide
whether to rebuild a model or update control presentation.

## Budgets and errors

The default binding-owned cap is 8 MiB. It includes registry records, query
vectors, converted strings and staged native storage. An explicit zero is a
literal cap; NULL options select defaults. Engine, document and page-state
storage have their own caps in their owning libraries. This composition does
not claim a single combined page allocator.

| Refusal | Script-visible name |
| --- | --- |
| Native allocation, arena, depth or work quota | `QuotaExceededError` |
| Invalid placement or required document root | `HierarchyRequestError` |
| Reference/old child not under the specified parent | `NotFoundError` |
| Invalid element/attribute name | `InvalidCharacterError` |
| Wrong receiver or unsupported native argument | `TypeError` |
| Retired binding or nonempty file-input value assignment | `InvalidStateError` |

Named native refusals are Error objects with own name/message properties;
this slice does not install a DOMException constructor.

During evaluation, engine memory exhaustion follows libjs's sticky `LIMIT/MEMORY` result. It
cannot be caught and resumed as a native DOM quota exception. Ordinary
JavaScript exceptions follow the existing runtime outcome contract.

Alert borrows normalized UTF-8 during a synchronous host callback. The host
copies it if needed and does not enter a nested loop or JavaScript. Confirm
returns false; prompt returns null. Events, timers, document.write, geometry,
cookies and browser script scheduling belong to later work.

## Proof boundary

`tools/test_dom_host.sh` tests the actual target engine core and a separately
ASan-instrumented target-profile engine, with sanitized bindings/tree/state.
`domtest` exercises the shared-library boundary and heap in ring 3. These are
library proofs; Yonder repaint, script scheduling and navigation acceptance
remain D5b. Result counts are recorded in DOM.md after the final source freeze.

D6 holds wrapper identity keys, query roots and cached query answers. A
successful refresh holds its complete successor before releasing the old
answer; refusal preserves the old holds. Native holds survive drain and
engine finalizers and are released by `os64_dom_free` after engine destruction.
Copied JavaScript strings retain no native snapshot bytes. Hosts select fatal
or D8's opt-in reporting/reclaiming destruction at runtime creation. Both use
the same drain/destroy/free ordering; the reporting host consumes the teardown
report and logs/counts leaks. Independent D8 acceptance is required before
scripting is enabled for ordinary browsing.
