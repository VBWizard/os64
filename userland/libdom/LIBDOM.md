# libdom

libdom makes the script-visible document over libhtml's mutable tree and
libpage's persistent control state. It uses libjs's pinned QuickJS binding
surface; it neither parses JavaScript nor owns a browser loop.

D5a built the document surface; D5b seated it in Yonder. D7a adds the
EventTarget half of the DOM, HTML's event handlers and timers, and the asks a
script makes of the page around it (§ Events, § Timers, § Asks). The browser
loop that drives them is yonder's (DOM_D7.md); this library owns no loop. D9
adds `document.write`, which is the host's parser's (§ `document.write`).

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
and control properties. `window` names the global object, and so do `self`, `frames`, `parent` and
`top`, as they do for a window with no frames: the first three are
[Replaceable] (a page may assign over them), `top` is read-only and
unconfigurable, and the window's `length`, its frame count, is 0. Document landmarks
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
returns false; prompt returns null. Cookies and the browser's script
scheduling belong to later work (geometry is § Synchronous geometry).

Listener records, handler text, timer records and their argument arrays, the
event path of a dispatch and the started-script table are charged to the
binding's budget like any other native record; a refusal is
`QuotaExceededError` before anything changes. Event state lives in engine
memory (§ Events).

## Events

`addEventListener`, `removeEventListener` and `dispatchEvent` are on every
node and on window. A listener is a function or an object with `handleEvent`;
the third argument is the old boolean `useCapture` or `{capture, once}`
(`passive` is accepted and changes nothing). `new Event(type, {bubbles,
cancelable})` and the old `document.createEvent(...)` with `initEvent` make
events. An event reads `type`, `target`, `srcElement`, `currentTarget`,
`eventPhase`, `bubbles`, `cancelable`, `defaultPrevented`, `timeStamp` (the
host's clock), `isTrusted`, and the writable old spellings `returnValue` and
`cancelBubble`. Host events also carry the mouse fields (`clientX/Y`,
`pageX/Y`, `screenX/Y`, `button`, `which`, `relatedTarget`) or the key fields
(`key`, `keyCode`, `charCode`, `which`) and the modifier flags, as data.

**Where the values live.** A target's listener list hangs off its wrapper
record, in the order listeners were added; window's off a record of its own.
Each listener's callback is a C-held engine value, the registry's second
list, released by drain. An Event's own state is engine memory traced by the
collector (`gc_mark`), because a script may keep an event long after its
dispatch: its finalizer frees engine storage and nothing native.

**Dispatch** is the DOM Standard's: the path is the target and its ancestors,
then window past the document (except for `load`); capture listeners top
down, then the target's capture listeners and its others, then bubbling up
when the event bubbles. A list is cut where it ended when its target was
reached, so a listener added during the dispatch waits for the next one; a
listener removed during it does not run (its record is swept when the last
dispatch leaves). `once` listeners are removed before they run. A listener
that throws is reported and the next one runs.

Two callers, two shapes. **The host** (`os64_dom_dispatch`) opens a libjs
task, runs each listener as one `os64_js_call` followed by a checkpoint, and
closes the task; its outcome is the task's first exception or rejection, a
sticky status when the runtime died under it, or a nested error (below).
**A script** (`dispatchEvent`) runs the listeners as nested engine calls with
no checkpoint between them, which is what DOM.md asks: a checkpoint follows a
callback that returns with no script on the stack. A nested listener's error
is kept in the binding (`os64_dom_take_report`) rather than thrown into the
script that dispatched, as the standard reports it; the host folds it into
the task's outcome when it opened the task through libdom. Every engine
entry the binding makes is inside a task or a turn: the host's own calls are
`os64_js_call` on one invoking function, and everything else runs under a
call or an evaluation that is already armed.

`os64_dom_listens(type)` answers whether anything could hear a type: a
listener or a handler property is counted as it changes, and on<type>
content attributes are counted once per tree version by one walk of the
document. A host asks before it builds anything, so a `mousemove` over a
page with no handler costs no allocation and no engine entry. That walk is
public as `os64_dom_handler_attributes(doc)`, a mask `os64_dom_handler_bit
(type)` reads, and needs no runtime: a host whose page has not run a script
asks it before making one, so a page with one `onclick` gets its runtime at
the first click.

**Event handlers.** For `click`, `mousedown`, `mouseup`, `mouseover`,
`mouseout`, `mousemove`, `keydown`, `keypress`, `keyup`, `input`, `change`,
`submit`, `reset`, `focus`, `blur` and `load`, elements, the document and
window have an on<type> property, and an on<type> content attribute is a
handler. A target's handler is one slot in its listener list, in the place
it was first set; a slot for an attribute the parser or innerHTML wrote goes
first (an element has its attributes before any script can reach it), and a
script's `setAttribute` makes its slot then. Window's slots come from the
body, which window predates: window takes in the body's attributes before
each listener added to it, and one found only at dispatch goes last, so
`<body onload>` runs after a `load` listener a head script added and before
one added once the body was parsed. The slot remembers the attribute
text it last saw, so a set, changed or removed attribute is noticed by
whichever route it changed. It compiles at the first dispatch or read that
needs it: `function on<type>(event){BODY}` inside `with(document)
with(form owner) with(element)`, HTML's three scopes. A body that does not
compile is reported once and stays absent until the attribute changes;
reading the property answers null. A handler that returns `false` cancels.
The body's window-reflecting handlers (`load`, `focus`, `blur`) are window's,
from `<body onload>` or `document.body.onload` alike.

The body is first checked by the intrinsic Function constructor, taken
before any page script could replace it. QuickJS builds that function by
joining strings, so a body written to close the function early in both
shapes (`} + (f(), function(){`) still compiles, where a browser that parses
a FunctionBody refuses it. It is the page's own code in its own realm: it can
change when that code runs, not what it may do. A compiled value that is not
a function is a compile failure.

## Timers

`setTimeout`, `setInterval`, `clearTimeout` and `clearInterval`, on window.
Ids count from 1 and are never reused within a page; 4096 may be live at
once, and one more throws `QuotaExceededError`. A string handler is a script
evaluated in global scope (the old web's `setTimeout("tick()", 1000)`); a
function is called with `this` as window and the extra arguments. A delay
wraps as a 32-bit integer and below zero is zero; a timer nested more than
five deep is clamped to 4 ms, as HTML clamps it. The table holds callbacks
and arguments, so it is the registry's third list.

The host drives it in its own clock (`options.now_ms`):
`os64_dom_timer_next` answers the earliest due time, and
`os64_dom_timer_fire` runs at most one due timer as one task. Two timers due
together fire in the order they were set. An interval is re-armed before its
callback runs, so a callback that clears its own interval wins; a callback
that clears the timer that is running finishes running.

## Asks

A script's task must not reach widgets or the model, so what a script asks
of the page is recorded and performed by the host after the task.

**Navigation**, one slot, the last ask of a task winning
(`os64_dom_take_navigation`): `location.href =`, `location = `,
`location.assign`, `replace`, `reload`, `hash =` (the address with its
fragment replaced), `history.back/forward/go` (`go(0)` reloads), a link's
`click()`, and a form's submission. Addresses are resolved exactly as a
link's are, by libpage's own steps with the base as the tree stands
(`os64_dom_resolve`, which a host uses for a script's `src` too); `hash =`
stays in the page. A taken ask's nodes come held. `location` reads the page's
address and its parts; `history.length` reads 1. `window.status` and
`defaultStatus` accept an assignment and read empty: the status line is the
browser's.

**Element actions.** `click()` is a synthetic click: the event, then the
element's activation unless a listener cancelled it. A checkbox or radio
changes before its click and is put back when the click is cancelled
(otherwise `input` and `change` follow); a link asks to be followed; a
submit button fires `submit` at its form and, uncancelled, asks for the
submission; a reset button fires `reset`. An element already being
clicked ignores another `click()` until its own finishes (HTML's
click-in-progress flag); another element can still be clicked from its
listener. `form.submit()` asks without a `submit` event, `requestSubmit()` with one, and `reset()` fires `reset`. A
reset not cancelled, `focus()` and `blur()` reach the host through
`options.activate`, which records them.

## Script elements

`os64_dom_script_kind` is HTML's type rule (type, else language, the sixteen
JavaScript MIME essences, `module`), and every caller asks it. ALREADY
STARTED is a held node in the binding's table: `os64_dom_script_start` marks
a script the host runs, a script innerHTML parses is born started, and a
clone of a started script is started. When a verb connects a classic script
that has not started (an `appendChild` of a `createElement('script')`), the
binding marks it and tells the host through `options.script_connected`;
moving a started script tells nobody. A host that sets no callback gets no
notices and no marks.

## `document.write`

The host's `options.write` is the whole of this library's knowledge of
parsing. `write` and `writeln` join their arguments, each converted as
`String()` would and normalized as every other string the binding hands
over (a NUL or a lone surrogate becomes U+FFFD), `writeln` adding a newline
even with no arguments; the joined text is one allocation charged to the
binding's budget, refused as `QuotaExceededError` before the host hears of
it, and borrowed by the host for the call. The host answers libhtml's
status. Only `OS64_HTML_BAD_ARGUMENT` — there is no insertion point: the
parse has ended, or the caller is not the script the parse is stopped at —
reaches the script, as `InvalidStateError` with one sentence: "document.write
after the parse has ended would replace the document, which this browser
does not do". Anything else, a refusal of the parse included, is the page's
and not the script's, and `write` returns `undefined`. A host with no
callback has no parser, and every write is that refusal.

`document.open()` asks the host with an empty write and answers the
document when there is an insertion point (an open over the running parser
changes nothing); otherwise it throws the same `InvalidStateError`, since any
other `open()` would make a new document. `document.close()` does nothing:
it acts only on a parser `open()` made, and none is ever made. Which writes
have an insertion point is the host's rule, not this library's (yonder's is
DOM_D9.md's: only the script the parse is stopped at, while it runs).

## Proof boundary

`tools/test_dom_host.sh` tests the actual target engine core and a separately
ASan-instrumented target-profile engine, with sanitized bindings/tree/state.
`tools/test_dom_events.inc` holds a case per rule of § Events, § Timers,
§ Asks, § Script elements and § `document.write`, and
`tools/test_dom_mutants.py --events` a mutant per rule. `domtest` exercises
the shared-library boundary and heap in ring 3. These are library proofs; yonder's loop is proven with its own
harness. Result counts are recorded in DOM.md after the final source freeze.

D6 holds wrapper identity keys, query roots and cached query answers. A
successful refresh holds its complete successor before releasing the old
answer; refusal preserves the old holds. Native holds survive drain and
engine finalizers and are released by `os64_dom_free` after engine destruction.
Copied JavaScript strings retain no native snapshot bytes. Hosts select fatal
or D8's opt-in reporting/reclaiming destruction at runtime creation. Both use
the same drain/destroy/free ordering; the reporting host consumes the teardown
report and logs/counts leaks. Independent D8 acceptance is required before
scripting is enabled for ordinary browsing.

## Synchronous geometry

`os64_dom_set_geometry` installs an owner-thread native provider separately from
the binding options. The provider supplies fresh layout at the current view
size and zoom, copies CSS-pixel values into `os64_dom_geometry_t`, and retains
no engine values. Missing or refused HTML geometry throws InvalidStateError.
Element exposes offset and client dimensions/positions, offsetParent,
and getBoundingClientRect. Rectangles are ordinary detached numeric snapshots;
Non-HTML elements return numeric zeros, a null offsetParent and a zero rect
without asking the HTML provider. SVG layout and getClientRects are outside
this slice. Layout counts and
elapsed microseconds are available through `os64_dom_geometry_stats`, including
failed attempts. Callback budget checks bracket native work and preserve the
turn's sticky limit classification. See [D10](../../../docs/design/pending/DOM_D10.md)
for provider semantics, stack measurements and the browser integration boundary.

## The global-miss census

`os64_dom_set_global_miss` tells an owner-thread hook each name a lookup on
`window` did not find: a bare name, `typeof`, `window.x`, `window['x']`,
`globalThis.x`. It does not hear `in`, `hasOwnProperty` or a descriptor
ask. The hook hears the name before the lookup answers, and the answer
(undefined, or the ReferenceError) is unchanged, so feature detection sees
what it always saw. It must not enter JS. The engine side is libjs patch
0008. Yonder's page file is the consumer
([YONDER_DIAGNOSTICS.md](../../../docs/design/pending/YONDER_DIAGNOSTICS.md)).

## D11 classic consumers

The bounded widget surface is specified in
[DOM_D11.md](../../../docs/design/pending/DOM_D11.md). Live image/form/control
collections and named properties use the same native tree and state as D3.
Stable inline style objects hold their elements and edit typed style attributes
through libgarb validation. Browser identity is a borrowed owner-thread provider;
active event values live in nested dispatch frames. Readonly scroll snapshots
extend the geometry result at its tail. Providers and consumers are rebuilt
from the matching public header.

The browser explicitly installs the engine's legacy function-arguments helper;
standalone contexts retain the upstream property. No finalizer owns native tree
storage. Detached image prefetch is deferred with Chris; displayed images use
the native picture-loading path. Source-preserving CSS text and attribute image
dimensions are bounded consumer implementations, not complete CSSOM/image
interfaces. Host acceptance and remaining J5/P5 work are in the design record.
