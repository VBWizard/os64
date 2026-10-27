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
`top`, as they do for a window with no frames: `self`, `frames`, `parent`
and `length` are [Replaceable] (a page may assign over them), `top` is
read-only and unconfigurable, and the window's `length`, its frame count,
is 0.
`top`, `parent` and `length` are accessors that ask the window's browsing
context (browsing.c, § P1-W's rows), which answers so for a root.
`window` and `document` are [LegacyUnforgeable] (core.c): own accessors of
the global with no setter, neither configurable, so an assignment is
ignored outside strict mode and a TypeError in it, and a page can neither
delete nor redefine them. `history` is a read-only accessor. Document landmarks
are read from the attached tree rather than trusted parser-time pointers.

Kind prototypes inherit Node. An HTML element's prototype is HTMLElement's,
which inherits Element's, or, for the elements with members of their own
(the form controls, option, optgroup, form, image, script, anchor, link,
iframe and template), a prototype that inherits HTMLElement's; an SVG element's is
SVGElement's, which inherits Element's; an element of any other namespace
(MathML) has Element's. A text node's prototype is Text's and a comment's
Comment's, each inheriting CharacterData's. The supported properties and
methods are placed on those chains:

| Kind | Surface beyond Node |
| --- | --- |
| Document | documentElement/head/body (body settable, HTML's setter), doctype, implementation, child-element access, ID/tag/class/name/selector queries, creation; with the Modern Web switch on, the state reads of § P1-Da's rows |
| Element | tagName, id/className, innerHTML/outerHTML, the attribute methods and `attributes` (§ Attributes as nodes), tag/class/selector queries, child-element access and element siblings, § The classic methods |
| Text and Comment (CharacterData) | data and element siblings; a Text's wholeText and splitText with the Modern Web switch on (§ 0f P1a's rows) |
| DocumentType | name, publicId, systemId |
| DocumentFragment | child-element access, ID and selector queries |
| HTML input/textarea/select/button | value, type (an input's state keyword, else "text"; a button's "submit", "reset" or "button"; "select-one" or "select-multiple"; "textarea") |
| HTML input | checked (every input's checkedness, which only a checkbox and a radio show and send) |
| HTML select | selectedIndex; with the Modern Web switch on, options and the rest of § 0f P1a's rows |
| HTML option and optgroup | with the Modern Web switch on, § 0f P1a's rows |

Node supplies tree navigation, ownerDocument, childNodes, nodeType/nodeName,
nodeValue/textContent and tree methods. A property absent from a node's chain
reads undefined and is absent from `in`; assignment may create an ordinary
expando. Borrowed methods and accessors still validate native receivers.
With the Modern Web switch on, the prototypes are named on window by
interface objects (§ The Modern Web's surface): DocumentFragment's
constructs an empty fragment, Text's and Comment's a node holding their
argument, and the rest are Illegal constructors. The surface is the one
this file lists.

Child collections and tag queries are native live objects with `length`,
`item(index)` and indexed access. A query refreshes against the HTML version;
its node-vector replacement is staged before publication. A held collection
therefore changes after a mutation. Numeric properties are read-only, including
definitions of future indices, and the object refuses preventExtensions so
live growth can continue. Nonnumeric strings and Symbols remain expando keys.
Every collection iterates as an array does (`[Symbol.iterator]` is
Array.prototype.values, WebIDL's rule for an indexed list).

`querySelector` and `querySelectorAll` match with libgarb's selectors, the
style sheets' own, over the receiver's descendants in tree order, with the
receiver as `:scope` (a document's is its root). A list that does not parse
throws a SyntaxError naming it. `querySelectorAll` answers with a STATIC
NodeList: an ordinary engine object, its matches fixed own indices and its
`length` fixed, with `item`, `forEach`, `entries`, `keys` and `values` (the
last four Array.prototype's). It is not a registry query, so the engine frees
it like any other value: a page that asks in a loop holds only what it keeps.

Template contents are a separate fragment:
ordinary children/textContent follow actual children, while innerHTML uses
that contents fragment. outerHTML is read-only in this slice.

Strings cross the native boundary as UTF-8. NUL and lone UTF-16 surrogates
become U+FFFD, while valid surrogate pairs retain their character; web
storage is the exception, keeping both exactly (WTF-8, § Slice 1a's rows). HTML names
are ASCII-folded where the operation requires it. Native placement validity
belongs to libhtml; bindings turn its status into an exception.
Element and attribute names use the DOM Standard's separate name-validation
rules. Nullable nodeValue/textContent map null and undefined to empty text;
innerHTML and CharacterData data map null to empty but convert undefined to
the literal string.

## The classic methods

What pages assume a document has (`mixin.c`), each the DOM or HTML
standard's steps over the verbs appendChild already uses, so a script these
connect runs and a moved control keeps its form:

- **ParentNode** (document, element, fragment): `append`, `prepend`,
  `replaceChildren`. **ChildNode** (element, text, comment, doctype): `before`,
  `after`, `replaceWith`, `remove`. Arguments are nodes or strings (a string
  is a Text node), and several are gathered into a fragment first, as the
  standard does, so a refused insertion leaves them in that fragment. Every
  argument is converted before the tree is read (a string's conversion
  runs script), and a lone node replacing an element's children is judged
  (`os64_html_may_insert`) before it is moved anywhere.
  A document keeps its `<html>` element (libhtml's rule, which every reader
  of the tree relies on), so `document.replaceChildren(...)` is refused as
  `document.removeChild(document.documentElement)` always was, and a
  document of its own's is refused the same way (libpage's children verb
  takes an element or a fragment); on an element or a fragment, where pages
  call it, it is the standard's.
- `contains`, `isConnected`; `matches` and `closest` (the selectors
  `querySelector` uses); `getElementsByClassName` (live; a quirks document
  compares classes without regard to ASCII case) and `getElementsByName`.
- `insertAdjacentHTML`, `insertAdjacentElement`, `insertAdjacentText`. The
  HTML is parsed in the context of the element it lands in, and its scripts
  never run, as innerHTML's do not.
- `getAttributeNames`, `toggleAttribute`, `hasAttributes`.
- Reflected attributes: `hidden`, `lang`, `dir` (its keyword, or ""),
  `tabIndex` (0 for an element that takes focus without one, else -1), a
  script's `src` and a link's and an anchor's `href` (resolved against the
  document's base, and as written when it does not resolve), a script's
  `type`, `defer` and `noModule`, and a link's `rel`. Script, anchor and
  link elements have prototypes of their own for these. `hidden`, `lang`,
  `dir` and `title` are HTMLElement's, and `tabIndex` and `dataset`
  HTMLElement's and SVGElement's, as HTML places them, not Element's: a
  framework that sets a member as a property only when it finds the setter
  below Element.prototype (Svelte 5) otherwise writes an attribute.
- `document.cookie`, through a provider the host installs
  (`os64_dom_set_cookies`): a read is the whole cookie-string it writes,
  asked again with more room while more did not fit, an assignment is handed to it whole as a Set-Cookie value. Which cookies a
  script may see or set (never an HttpOnly one) is the provider's: yonder's
  is libway's jar (`way_script_cookies`). With none the document is
  cookie-averse: it reads "" and an assignment does nothing.
- `classList`, a live DOMTokenList over the class attribute (`add`,
  `remove`, `toggle`, `replace`, `contains`, `item`, indices, iteration;
  assigning the list sets the attribute), a link's `relList`, the same list
  over `rel`, and `dataset`, a DOMStringMap over the `data-*` attributes
  (`data-foo-bar` is `fooBar`; setting and deleting a name sets and removes
  its attribute). Each element has one of each, made when first asked for
  and held, like its style, until the page goes. `supports(token)` is a
  TypeError on a classList, which has no supported tokens; a relList's are
  the link types yonder acts on, without ASCII case: `stylesheet`, and
  `modulepreload` with the Modern Web surface, which runs modules (a module
  loader asks before it preloads: Vite's first statement does).

## Mutation and control state

Property edits delegate to D3. Attribute writes, including id/className,
go through one door, `d_set_attr` (§ Attributes as nodes), to
`os64_page_node_set_attr`: it stages prospective attributes and
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
| A qualified name and namespace DOM does not pair | `NamespaceError` |
| An Attr another element holds, set on this one | `InUseAttributeError` |
| A node or an attribute the tree cannot hold as asked (an element in no namespace, an Attr whose namespace the tree would lose) | `NotSupportedError` |
| Wrong receiver or unsupported native argument | `TypeError` |
| Retired binding or nonempty file-input value assignment | `InvalidStateError` |

A refusal named `TypeError` or `RangeError` is the engine's own
TypeError or RangeError. Every other
named refusal is a DOMException (exception.c) with that name, its
message and its legacy `code`, made by `d_error` once the Modern Web's
DOMException row has installed the interface; with the modern switch
off there is no DOMException, and the refusal is an Error with own
name/message properties, as it was before Slice 2 built the interface.

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

`addEventListener`, `removeEventListener` and `dispatchEvent` are
EventTarget.prototype's, made once by `d_event_install`, and every target
inherits them: every node through Node.prototype, window through
Window.prototype (the global's own prototype, between it and
EventTarget.prototype), and each plain event target a Modern Web row makes
(`d_event_target`: an object that is neither, whose dispatch path is itself
alone, and whose prototype `d_event_target` makes inherit
EventTarget.prototype). The methods tell the target by `this`, and a bare
call is window's. Those levels are there with the Modern Web switch on or
off; the switch's last row names them `EventTarget` and `Window`, makes
`new EventTarget()` (and a subclass's `super()`) a plain target of its own
class with its entry in the object, and makes each interface whose
prototype inherits EventTarget.prototype inherit EventTarget, as WebIDL's
interface objects follow their prototypes. `new Window()` is a TypeError.
A listener is a function or an object with `handleEvent`;
the third argument is the old boolean `useCapture` or `{capture, once}`
(`passive` is accepted and changes nothing). `new Event(type, {bubbles,
cancelable})` and the old `document.createEvent(...)` with `initEvent` make
events; with the Modern Web switch on, `createEvent` reads DOM's table of
interface names (§ P1-Da's rows), and with it off makes a plain Event
whatever it is asked. An event reads `type`, `target`, `srcElement`, `currentTarget`,
`eventPhase`, `bubbles`, `cancelable`, `defaultPrevented`, `timeStamp` (the
host's clock), `isTrusted`, and the writable old spellings `returnValue` and
`cancelBubble`. Host events also carry the mouse fields (`clientX/Y`,
`pageX/Y`, `screenX/Y`, `button`, `which`, `relatedTarget`) or the key fields
(`key`, `keyCode`, `charCode`, `which`) and the modifier flags, as data.

**Where the values live.** A target's listener list hangs off its wrapper
record, in the order listeners were added; window's off a record of its own;
a plain target's off its entry, which is its object's opaque, or for a
MediaQueryList one its resolver makes when a listener is added, its
handler set or an event dispatched at it (a read of `onchange`, a removal
or a null handler makes none). That entry holds the list, and is let go,
unlinked and freed, at the next matchMedia or view step once no listener
or handler is left on it, so a list the page then drops is collected. A
class a page makes one of per request (an AbortSignal, an XMLHttpRequest
and its upload, a `new EventTarget()`) keeps the entry IN the object, the first member of its
state: it is on no registry, its listeners' callbacks are the object's to
trace (`gc_mark`) and free (finalizer), and the object goes with the
garbage whatever listens on it. Registration, the listener records, the
methods and the dispatch are the same for all three (internal.h, A PLAIN
EVENT TARGET); `d_event_handler_named` gives a plain target an `on<type>`
for any type, and a class may set `DValue.listeners_changed` to hear its
listeners come and go (the AbortSignal's GC clause is its one reader).
Each listener's callback is a C-held engine value, the registry's second
list, released by drain. An Event's own state is engine memory traced by the
collector (`gc_mark`), because a script may keep an event long after its
dispatch: its finalizer frees engine storage and nothing native.

**Dispatch** is the DOM Standard's: the path is the target and its ancestors,
then window past the document (except for `load`), and a plain target,
which is in no tree, alone; capture listeners top
down, then the target's capture listeners and its others, then bubbling up
when the event bubbles. A list is cut where it ended when its target was
reached, so a listener added during the dispatch waits for the next one; a
listener removed during it does not run (its record is swept when the last
dispatch leaves). `once` listeners are removed before they run. A listener
that throws is reported (with the switch on, as the window's error event,
§ P2a's rows) and the next one runs.

Two callers, two shapes. **The host** (`os64_dom_dispatch`) opens a libjs
task, runs each listener as one `os64_js_call` followed by a checkpoint, and
closes the task once what it left owed is settled (§ P2a's rows); its
outcome is the task's first exception or unhandled rejection, a sticky
status when the runtime died under it, or a nested error (below).
**A script** (`dispatchEvent`) runs the listeners as nested engine calls with
no checkpoint between them, which is what DOM.md asks: a checkpoint follows a
callback that returns with no script on the stack. A nested listener's error
is kept in the binding (`os64_dom_take_report`) rather than thrown into the
script that dispatched, as the standard reports it, and is the window's
error event at once; as the task closes (a task libdom opened, or a script
`os64_dom_script_ended` settles) it is the task's outcome under the task's
name when that is OK, else a line told to `options.failed`, and it is
cleared either way. Every engine
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
handler. With the Modern Web switch on, so do `scroll` and `resize`; window
alone has `onpopstate` and `onhashchange`, and a MediaQueryList `onchange`
(§ The view, computed style and the session's history), window alone
has `ononline`, `onoffline` and `onlanguagechange`, whose events nothing
here fires (§ P1-W's rows); window has `onerror`,
`onunhandledrejection` and `onrejectionhandled` and the document `onerror`,
whose events the binding fires for a script's failures (§ P2a's rows; an
`onerror` content attribute on any element is a handler for an `error`
event dispatched at it, though no element has the property); window has
`onmessage` and `onmessageerror`, whose events a posted message's task
fires (§ P2b's rows); and a document alone has `onreadystatechange`, `onvisibilitychange` and
`onprerenderingchange`, which HTML gives no content attribute, so no
element's attribute of that name is a handler (§ P1-Da's rows). With it
off none of these is a property, and
their content attributes are ordinary attributes that no event runs. A target's handler is one slot in its listener list, in the place
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
reading the property answers null. A handler that returns `false` cancels,
except window's `onerror` called for an ErrorEvent named `error`, which is
called with the event's message, filename, line, column and error rather
than the event, and cancels by returning `true` (HTML's special error event
handling); its attribute compiles as a function of `event`, `source`,
`lineno`, `colno` and `error`.
The body's window-reflecting handlers (`load`, `focus`, `blur`, and with the
switch on `scroll`, `resize`, `popstate`, `hashchange`, `online`, `offline`,
`languagechange`, `error`, `unhandledrejection`, `rejectionhandled`,
`message` and `messageerror`) are window's. For the ones every element has a
property for (`load`, `focus`, `blur`, `scroll`, `resize`) that is so from
`<body onload>` and `document.body.onload` alike; for the others it is so
from the body's content attribute only, since the body has no such IDL
property (`document.body.onpopstate` is an ordinary expando; DEBTS.md).

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
and arguments, so it is the registry's third list. The binding's own
timers (AbortSignal.timeout, an XMLHttpRequest's timeout, an idle
callback's timeout) are in the same table and draw from the same ids, and
`clearTimeout` and `clearInterval` pass over them.

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
`click()`, and a form's submission. With the Modern Web switch on,
consecutive `back/forward/go` in one task add up to one traversal of their
sum. Addresses are resolved exactly as
a link's are, by libpage's own steps with the base as the tree stands
(`os64_dom_resolve`, which a host uses for a script's `src` too); `hash =`
stays in the page. With the Modern Web switch on and a history host
installed, an address that moves only the fragment is no ask: the host makes
it at once (§ The view, computed style and the session's history), and one
it refuses is asked for instead. A taken ask's nodes come held. `location`
reads the page's address and its parts; `history.length` reads the host's
count of entries, and 1 with no host or the switch off. `window.status`
reads back the last string a script set, as HTML keeps it, and
`defaultStatus`, which no standard has, accepts an assignment and reads
empty; the status line itself is the browser's and shows neither.

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
JavaScript MIME essences, `module`, `importmap`; a type is trimmed only to
be asked whether it is JavaScript's, so `type=" module "` is none of
them), and every caller asks it. ALREADY
STARTED is a held node in the binding's table: `os64_dom_script_start` marks
a script the host runs, a script innerHTML parses is born started, and a
clone of a started script is started. When a verb connects a classic,
module or import map script that has not started (an `appendChild` of a
`createElement('script')`), the binding marks it and tells the host through
`options.script_connected`;
moving a started script tells nobody, and one connected to a document of
its own (§ Documents of their own) is marked and tells nobody. A host that
sets no callback gets no notices and no marks.

A host that runs modules asks two more things of the binding (MODERN_WEB.md
§ Slice 3, as built): whether a response's `Content-Type` names JavaScript
(`os64_dom_javascript_mime`, the same sixteen essences, as a module's
response must be), and the address of the classic script a name of the
binding's stands for (`os64_dom_script_address`, the inverse of
`os64_dom_script_name`), which a dynamic `import()` in that script
resolves against. And it parses an import map's JSON
(`os64_dom_import_map`, MODERN_WEB.md § Import maps, as built): as one
task named as the script, throwing what HTML's "parse an import map string" throws
(the JSON's SyntaxError; a TypeError for a top-level value, an `imports`,
`scopes` or `integrity`, or a scope that is no object), which the window
hears as a task's exception, and otherwise handing the host each entry
of `imports` and of every scope in JSON.parse's order, a value that is
no string as none. The normalizing, the merge and the resolution are the
host's. With the switch on, `HTMLScriptElement.supports(type)` answers
true for `classic`, `module` and `importmap`, the types this window runs,
and false for anything else.

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

## Documents of their own

`document.implementation` (`implementation.c`; DOM § 4.5.1) is on every
page, the switch off included: it is in every browser that ever ran
jQuery, whose `parseHTML` builds its nodes in a document of its own so
that the markup runs nothing and fetches nothing until it is put in the
page. One DOMImplementation per document, made when first asked for.
`createHTMLDocument(title)` makes `<!DOCTYPE html><html><head><title>
title</title></head><body></body></html>`, the title element and its text
only when a title is given; `createDocument(namespace, qualifiedName,
doctype)` an XML document holding the doctype and then the element
createElementNS would make, either absent when not asked for;
`createDocumentType` a doctype; `hasFeature()` answers true, as it is
specified to. The `DOMImplementation` interface object is a Modern Web
row.

**A new document lives in the page's arena** (libhtml's
`os64_html_create_document`): a second DOCUMENT node there, under the DOM
Standard's rules for a document's children and none of the window's own
(its element may be anything and may go). Its nodes are the page's nodes
to every verb, hold and wrapper, so one appended to the page is MOVED and
keeps its identity, wrapper, expandos and listeners: that is what
adoption does here. It is freed with the page and charged to its caps, as
a fragment is.

**Which document a node belongs to** is the document its tree hangs from;
a tree that hangs from none remembers the document it was made in, copied
from, or last taken out of, on its root's wrapper (`DValue.document`,
written by `d_made_in` and `d_leaving`). A root with no wrapper (a child
innerHTML replaced, never handed to a script) says nothing and reads as
the window's: a script reaches it only through a descendant it held
before. While no document of its own exists, every node is the window's
and none of this runs.

**It has no window.** Its `location` is null and assigning to it throws;
it is cookie-averse; `write`, `writeln` and `open` throw
InvalidStateError, since writing would open it; a link in it navigates
nowhere, a form in it submits nothing (not even the event), focus and
blur there reach no host, and a form's reset there fires its event and
resets no control, since the host carries resets out on the page's model
(DEBTS.md). A script connected to it is marked
started and runs nowhere, then or after it moves into the page, as a
script innerHTML parses is; a handler attribute in it is not compiled and
reads null until its element is the window's. Its body reflects no handler
onto window (HTML § 8.1.8.1): `onload` there reads null and an assignment
changes nothing. Scripting is off for its nodes (`d_scripting`), so
innerHTML and insertAdjacentHTML parse a `noscript`'s contents there as
markup. It is connected as DOM means it (`d_connected`: isConnected, a
`form` attribute's lookup), and not in the window's tree, which is what a
script must be in to run (`d_in_window_tree`).

**An XML document** keeps an element name's case at creation, and its
createElement makes HTML elements when it was made for the HTML namespace
and refuses otherwise. Past creation such an element reads as an HTML
document's would (`nodeName` uppercased, a tag query and attribute names
folded, selectors case-blind), since the binding asks an element's
namespace and not its document (DEBTS.md). libhtml's tree holds elements of the HTML, SVG and MathML
namespaces only, so an element in no namespace, or another, is a
NotSupportedError, from `createDocument` and from such a document's
`createElement`; `createElementNS` is not built. A new document is in
no-quirks mode in a browser; here a query or a style attribute in it reads
with the window's mode, which a quirks page's class selectors show.

## Attributes as nodes

`Element.attributes` (DOM § 4.9.1, NamedNodeMap) and its items, each an
`Attr` (§ 4.9.2), with `document.createAttribute`/`createAttributeNS` and
Element's `getAttributeNode`/`NS`, `setAttributeNode`/`NS` and
`removeAttributeNode` (`attr.c`). Classic surface, on every page, the switch
off included: it is DOM Level 1's, and jQuery 1's support probes read it at
load. The `Attr` and `NamedNodeMap` interface objects are the interfaces
row's.

**An Attr is a view of its element, not a tree node.** libhtml keeps an
element's attributes as a list of names and values, so an Attr is what
`style`, `classList` and `dataset` are: a registry entry whose `target` is
its element, given a qualified name and a namespace. The binding keeps one
per element and name, made when first asked for, so the same object comes
back for as long as the attribute exists (`attributes[0] ===
getAttributeNode('id')`). Its value is read from the element by name at
every read and written through the element's attribute; nothing is held
across page code. It is a Node: `nodeType` 2, `instanceof Node`, and
`Attr.prototype` answers every member `Node.prototype` has for itself
(`nodeName` its name, `nodeValue` and `textContent` its value, no parent,
siblings or children, `childNodes` an empty list, not connected,
`ownerDocument` its element's document or the one it was made in or left,
the insertion verbs HierarchyRequestError, `removeChild` NotFoundError,
`cloneNode` a detached copy, `contains` itself), and is an EventTarget
whose dispatch path is itself. A tree verb handed an Attr refuses it as DOM
does (HierarchyRequestError to insert one, NotFoundError as a reference or
to remove one, `contains` false); a `Node.prototype` member called on an
Attr itself is the TypeError "an Attr is a Node with no place in the tree"
(DIVERGENCES.md).

**The map holds no snapshot either.** `length`, `item(i)`, the indices
(live, read-only) and the names (each attribute's qualified name, an HTML
element's names with an uppercase letter left out, unenumerable, and
hidden behind a name the prototype chain has) walk the element's list at
each call. A walk that runs page code in its middle reads the list as it
stands at its next step; a name is looked for after the prototype chain is
asked, which may run page code. The map is iterable as an array. The last
element walked is kept as an array of its attributes, used only while the
element's list is the one copied (libhtml's version has not moved, the
list starts where it did and its last attribute is still last), so
`length` and the i-th attribute are a step each, and a walk over all of an
element's n attributes in one call, forwards or back (an iteration,
`Object.keys`, `for..in`, a countdown from `length`), takes a number of
steps linear in n; the DOM host holds each kind of walk to that
(`d_attr_steps`).

**An Attr leaves its element at one door.** Every attribute the binding
sets or removes goes through `d_set_attr`, and the parser and libpage
remove none, so a removal by any verb (`removeAttribute`,
`toggleAttribute`, a `dataset` delete, `removeAttributeNode`,
`removeNamedItem`, a replacement by `setAttributeNode`) copies the value
into the Attr before the attribute goes and detaches it: `ownerElement`
null, its last value kept, a value of its own taken from then on. A removal
with no room for that copy is refused whole (QuotaExceededError). A
detached Attr is charged to the binding's budget until the page goes, like
every entry. `tools/test_dom_host.sh` refuses a build that calls an
attribute verb anywhere else in libdom. **The backstop:** an attached Attr
whose attribute its element no longer has reads as detached and empty, and
the host is told the rule was broken (`options.broken`: yonder's record
says `FAILED page: an attribute left its element outside libdom`).

**Setting an Attr.** `setAttributeNode` takes a detached Attr onto the
element (its value set through the door; a new name goes last, a replaced
one keeps its place), answers the Attr it replaced, detached with its
value, or null; an Attr this element holds is answered as it is; one
another element holds is InUseAttributeError. libhtml's verbs set an
attribute by qualified name, keeping the namespace of one that is there and
giving a new one none, so an Attr is set only where the tree would keep its
name and namespace both: an Attr in a namespace where the element holds
that name in it, one in none where the element does not hold its name in a
namespace. Anything else is NotSupportedError (DEBTS.md). `createAttribute`
folds its name in an HTML document; `createAttributeNS` is DOM's "validate
and extract", split at the first colon as `createDocument` splits an
element's name.

## Proof boundary

`tools/test_dom_host.sh` tests the actual target engine core and a separately
ASan-instrumented target-profile engine, with sanitized bindings/tree/state.
`tools/test_dom_events.inc` holds a case per rule of § Events, § Timers,
§ Asks, § Script elements and § `document.write`, and
`tools/test_dom_mutants.py --events` a mutant per rule;
`tools/test_dom_implementation.inc` holds a case per rule of § Documents of
their own, and runs jQuery 3.5.1 and 1.12.4 whole;
`tools/test_dom_attributes.inc` a case per rule of § Attributes as nodes,
and `tools/test_dom_mutants.py --only attr-` a mutant per rule;
`tools/test_dom_window.inc` a case per rule of § P1-W's rows;
`tools/test_dom_document.inc` of § P1-Da's rows; and
`tools/test_dom_report.inc` a case per rule of § P2a's rows, and every
allocation a failure's events make refused in turn; and
`tools/test_dom_message.inc` a case per rule of § P2b's rows, the bounds
a page can size, and every binding and engine allocation of a clone, a
message and an idle callback refused in turn. `domtest` exercises
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

## The view, computed style and the session's history

Three owner-thread providers, each installed outside callbacks; dom.h
holds their contracts, `view.c` and `window.c` the binding's half. All of
it is the Modern Web arc's surface, in four rows of `modern.c`'s table (the
view's members with `onscroll`, `onresize` and window's `focus` and `blur`,
which do nothing, the window being the person's to raise; `matchMedia`,
`MediaQueryList` and `MediaQueryListEvent`; `getComputedStyle` and the
`CSSStyleDeclaration` interface, which names the style object's prototype
and which the computed declaration's inherits; and
`onpopstate`, `onhashchange`, `PopStateEvent` and `HashChangeEvent`, with
`pushState`, `replaceState`, `state` and `scrollRestoration` on history).
With the switch off none of those names exists, nor
`document.scrollingElement`, `document.defaultView` or an element's
`onscroll`; the arc's handler types' content attributes are not handlers
(§ Events); `history.length` reads 1, a fragment navigation is an ask, the
hash setter asks whatever it is given, and history deltas do not add up
(§ Asks): what the binding did before the arc.

**The view** (`os64_dom_set_view`) is read, never cached: `scrollX`/
`pageXOffset`, `scrollY`/`pageYOffset`, `innerWidth`/`innerHeight` and
`outerWidth`/`outerHeight` ask it at every read, and `scroll`, `scrollTo`
and `scrollBy` ask it to scroll (smoothly asked is done at once), adding
what its layout cost to the geometry counts. With no view they read zeros
and move nothing. `document.scrollingElement` is CSSOM View's: the root
element, or in quirks mode the body, and null when that body is
potentially scrollable (which the computed style says). `os64_dom_view_step` is
the host's rendering step: one event a call, `resize` at window, else
`scroll` at the document bubbling to window, else `change` at the first
MediaQueryList whose answer moved, until it answers false.

**matchMedia** keeps the query as written and serialises it once, in one
pass by libgarb (`garb_media_text_serialized`, into the binding's own
storage), for `media`; `matches` is judged at every read, against the
view (its size, the person's scheme and motion), the screen (the device-*
features and `resolution`, from `options.screen` and the zoom, as `screen`
and devicePixelRatio report them) and the document's scripting
(`options.scripting`). A list libgarb could not read whole (nested past
its depth, or no memory) is `not all`. A list nothing holds is freed by
the next matchMedia or view step; one listened to (`addEventListener`,
`addListener`, `onchange`) is held while a listener or a handler is left
on it, and let go at the next matchMedia or view step after the last one
goes (§ Events, Where the values live).

**getComputedStyle** (`os64_dom_set_computed`) answers a read-only
declaration whose properties are libgarb's table, which is therefore the
list a page enumerates, in CSSOM's order (lexicographic, prefixed names
last). Every read asks the provider for the whole element into one snapshot
buffer, so a held declaration is live and keeps no copy of its own; a
snapshot past the binding's budget is refused whole (QuotaExceededError).
An element outside the document and a pseudo-element answer an empty
declaration without asking. `document.defaultView` is window.

**History** (`os64_dom_set_history`) is the host's session: `pushState`
and `replaceState` give it the entry's address (one the document may
rewrite its own to, or SecurityError) and its state as JSON (dom.h § HISTORY says what JSON may
carry; DataCloneError otherwise, QuotaExceededError past 64 KiB or when the
host refuses), and fire nothing. `history.state` is that JSON read once,
the same object until the entry changes. A fragment navigation a script
makes goes to the host at once, and its `popstate` fires inside that
script, with the new entry's null state; the host queues the `hashchange`.
`location.hash` set to the fragment the address already has (as the URL
parser's fragment state writes it) navigates nowhere. A traversal is the
host's: it tells the binding (`os64_dom_history_moved`) before it moves its
own entries, then owes `popstate` and, when the fragment changed,
`hashchange`. An owed event carries what it was owed with
(`os64_dom_history_event_t`): a hashchange its two addresses, a popstate
the entry's state and name (`os64_dom_history_entry`), its state being
history.state itself only while that entry is still the current one.
`history.scrollRestoration` is the host's to read
(`os64_dom_history_manual`).

## The Modern Web's surface

`os64_dom_options_t.modern` is the Modern Web arc's kill switch
(MODERN_WEB.md, rule 4), set in `os64_dom_default_options`. Everything the
arc adds to window is installed by one table, `modern.c`'s, walked once at
creation after the classic window is whole: with the option clear the walk
does not run and none of those names exists, which is what a page's feature
detection needs to see. What goes is the table's names and what its rows
put on them (each prototype's `constructor` and Symbol.toStringTag, Node's
constants); the prototype chain's levels are the same either way. A row is
the names it puts on window and the
installer that puts them there; a slice adds its file and its row and
installs no global anywhere else.

The DOM host holds every row to its names (`tools/test_dom_modern.inc`):
window's own names with the option set, less those with it clear, must be
exactly the table's names. A name installed outside the table is on window
both ways, and a name a row installs without listing it is on window only
one way; either fails the suite, so a new row needs no case of its own for
the switch. A row that leaves an engine exception pending has failed
whatever it answered, and so does the creation: QuickJS's function
constructors define `length` and `name` without saying when that ran out
of memory.

Every property name libgarb reads is a member of the style object three
ways (CSSOM § 6.6.1): camel-cased (`marginInlineStart`, `WebkitTransform`),
webkit-cased for a `-webkit-` name (`webkitTransform`), and dashed as
written (`margin-inline-start`), for shorthands and aliases as well as
longhands and the flow-relative names (`garb_prop_name_at` lists them
all). The style object follows the same switch for the CSS the arc adds: a
name libgarb does not read with the switch off (`garb_prop_known`: a
marked property, or a shorthand whose longhands are all marked) has none
of those members while the option is clear,
and a declaration of it is read as one libgarb does not know
(`garb_reading_t.modern_off`), so `'aspectRatio' in el.style` and
`setProperty` answer as they did before the arc. A value reads back as it
was written, not in CSSOM's canonical form, and a shorthand's longhands do
not read back (DEBTS.md § Classic browser scripting).

The first row is the interface objects (`classic.c`'s
`d_interfaces_install`): `Node`, `CharacterData`, `Text`, `Comment`,
`DocumentFragment`, `Document`, `DocumentType`, `Element`, `SVGElement`,
`HTMLElement` and the HTML element interfaces that have a prototype of
their own, each named for exactly one of the binding's
prototypes (WebIDL § 3.7), so `instanceof` answers truly and a page may
patch `Element.prototype`. Each interface object inherits its parent's
(`Object.getPrototypeOf(HTMLElement) === Element`), as each prototype
inherits its parent prototype; `Node`'s is `Function.prototype`. `Node`
carries the node-type and document-position constants, on itself and its
prototype, as WebIDL puts a constant. Each prototype's own
`Symbol.toStringTag` is its interface's name, so
`Object.prototype.toString.call(document.body)` is `[object HTMLElement]`
(WebIDL § 3.7.3).
`new DocumentFragment()` makes an
empty fragment of the window's document, and `new Text(data)` and
`new Comment(data)` a node of it holding `data` ("" when it is undefined),
each with new.target's `prototype` (so a subclass makes its own kind; the
interface's own when that is not an object); any call of them without `new`
is refused by the engine itself, the functions being made as constructors
only (they find their binding through the anchor, which is its own class's
prototype in the binding's context); every other interface is an "Illegal
constructor", `Document` among them, since `new Document()` makes an XML
document. An interface
whose nodes share a broader prototype (`HTMLDivElement`, `SVGSVGElement`)
is not installed: naming the broader prototype for it would make
`instanceof` lie. Each prototype's `constructor` is its interface object; with the
switch off it is what it was before the arc (`Image` for the image
prototype, inherited from `Object.prototype` for the rest).

### Slice 1a's rows: storage and the small globals

Six rows, a file each (MODERN_WEB.md § Slice 1a, as built).

**`localStorage`, `sessionStorage`, `Storage`** (`storage.c`; HTML §
12.2). Each area is a map of strings for the page's origin (`d_origin`,
libway's `way_origin_serialize`:
scheme, host and a port that is not the scheme's own; anything else is an
opaque origin, `null`, whose areas work and are kept nowhere). Strings keep
every code unit, a lone surrogate and U+0000 included, as WTF-8: browsers
keep them, WPT's webstorage holds a binding to it, and pages that pack
binary into UTF-16 strings break otherwise. An area holds at most 5 MiB,
counted as the standard counts (two bytes a UTF-16 code unit of every key
and value), and 65,536 keys; a `setItem` past either throws
`QuotaExceededError` and changes nothing. Its memory is not the binding's
budget's. The object is WebIDL's legacy platform object with named
properties: a key is an own property unless the prototype chain has one by
that name, `[[Set]]` and a data `[[DefineOwnProperty]]` are `setItem`,
`delete` is `removeItem`, a Symbol is an ordinary property, and a lookup
that runs page code (a Proxy prototype's `has`) looks the key up again
after it. `key(n)`'s order is the map's: a new key goes last, and a removal
moves the last key into its place (the standard asks only that the order
hold while the keys do), so every change is a few steps.

**Where an area is kept is the host's** (`dom.h` § Web storage): a provider
per area (`os64_dom_set_storage`) answers `identity` (a value that moves
with the store), `load` (the store's image) and `commit` (this page's
change list, merged by the host onto the store as it stands;
`os64_dom_storage_merge` does the merge and `os64_dom_storage_walk` reads
the records). An area reads the store at the first touch in a task when
its identity moved, replays its own change list over it, and hands only
the change list back: a `clear()` is a removal of each key the page could
see. A commit answers OK; FULL (past the quota: nothing kept, the page sees
the store again); or FAILED (the area keeps its map in memory from then
on). Commits are coalesced (`os64_dom_storage_settle` between tasks,
`os64_dom_storage_due` for the host's clock, `OS64_DOM_STORAGE_COMMIT_MS`),
sooner when the change list would pass the quota or the keys' cap, and once
more at drain, the last. What a page writes never takes the change list
past its bound: a change it cannot make room for (no memory to write the
list down for the early commit) is refused with `QuotaExceededError` as a
full store's is. A `clear()` of a map another window grew past the keys'
cap can pass it, until the next commit. A host that unloads a page
(yonder's departure) drains it, which is that page's last commit.
`os64_dom_storage_forget` drops both maps for "Clear page storage". The
image format is `os64 storage 1\n`, then `+<key bytes> <value bytes>\n` and
the key, the value and `\n` for a set, `-<key bytes>\n`, the key and `\n`
for a removal.

**`performance`** (`performance.c`): `now()` and `timeOrigin`, coarsened to
100 µs, on the host's microsecond clock (`options.now_us`) against the
navigation's origin (`options.timing`, read as the binding is made); User
Timing's `mark`, `measure` (the L3 dictionary too), the entry lists and
`clearMarks`/`clearMeasures`, `detail` a structured clone (§ P2b's rows;
DataCloneError for what one refuses); the legacy `timing` and `navigation` objects
and a `navigation` entry, which read the host's milestones whenever asked.
The entry buffer is an engine object with no prototype that no page is
handed, counted in C. Nothing else is recorded; there is no
PerformanceObserver. `performance` and `screen.orientation` are plain event
targets (§ Events).

**`URL`, `URLSearchParams`** (`url.c`): parsed by the page's own link rule
(`os64_page_resolve_in` with no document), so a script and a link agree on
an address; every component and setter, `canParse`, `parse`, `toJSON`;
URLSearchParams whole, live both ways with its URL, sorted (stably, by
merge) in UTF-16 code units. Where libos64's grammar is not the URL
Standard's parser, the gap is the grammar's (url.c's header names them).

**`TextEncoder`, `TextDecoder`** (`encoding.c`): UTF-8 out (`encode`,
`encodeInto`); in, the encodings libhtml's label table names (UTF-8,
windows-1252 and its aliases, UTF-16 of either order), with `fatal`,
`ignoreBOM` and streaming. Another label is a RangeError.

**`screen`, `devicePixelRatio`** (`screen.c`): the display the host gives
(`options.screen`, device pixels) in CSS pixels at the page's zoom
(`options.zoom`), which is also `devicePixelRatio`; `screen.orientation`
never changes, and its `lock()` rejects.

**`btoa`, `atob`** (`base64.c`): HTML § 8.3, forgiving-base64, a string of
bytes either way, `InvalidCharacterError` before any work.

Every Array these rows answer DEFINES its elements, so a setter a page put
on `Array.prototype` neither runs nor swallows one.

### Slice 2's rows: a script's requests

Five rows (MODERN_WEB.md § Slice 2, as built; docs/design/pending/ORIGIN.md
is the record of the origin and CORS rules, which are the host's).

**`DOMException`** (`exception.c`): WebIDL's, `name`, `message` and the
legacy `code` and its constants, its prototype inheriting
`Error.prototype`. It is the binding's one: every refusal by a standard
name, the older rows' included, is one (§ Budgets and errors).

**`AbortController`, `AbortSignal`** (`abort.c`): `abort`, `timeout` (a
page timer), `any`, `reason`, `throwIfAborted`, `onabort`. A signal's
"source signals" and "dependent signals" are DOM's WEAK SETS: a link
between two signals is a C record on both lists holding neither object,
and each signal's finalizer unlinks itself from the other side, so a
collection may take them in either order and no engine internals are
read. Dependents are flattened (a dependent's dependents link to its
sources) and kept in link order, the order an abort runs them: the
source's steps and event first, then each dependent's. DOM's GC clause
keeps a dependent someone could still observe: while it has a live
`abort` listener or a pending algorithm and is not aborted, its links hold
it, and the hold is released at exactly three points, the dependent
aborted, its last abort listener gone (the listener-change hook), and its
source's abort. A signal's algorithms are C's: a fetch's abort by its
request's token and, once answered, the Response it errors, the two
knowing each other by C pointer both ways; the Response holds the signal
until its body settles.

**`Blob`, `File`, `FormData`** (`blob.c`): bytes and a type in engine
memory; `slice`, `text`, `arrayBuffer`, `bytes`; `File`'s name and
`lastModified`. `FormData(form)` reads the form's entries through libpage
(`os64_page_form_entries`, its model of the window's document: a form in
a document of its own answers none); a submitter is a NotSupportedError.
multipart/form-data is written here (an empty FormData is the close
delimiter alone, as browsers send it) and
read back by `formData()`, as is urlencoded.

**`fetch`, `Headers`, `Request`, `Response`** (`fetch.c`): the Fetch
standard's § 5. A header list is a C array, its sort-and-combine kept
until it changes, so a walk is n log n; the guards (request, no-cors,
response, immutable) ask libway's lists, and every header the binding
writes itself (a body's Content-Type, a redirect's Location) goes through
the guarded append, so a no-cors request never carries a Content-Type
that is not safelisted. RequestInit and ResponseInit are read in WebIDL's
order; a record init converts each key before its value. Bodies are
engine memory, read whole: a stream hands the body over as one chunk, a
body method locks it, and there is no `ReadableStream` global (a page
that detects one would find a half API). `integrity` fails closed.
`fetch()` hands the request to the host (`os64_dom_set_network`) and its
answer comes back as one task (`os64_dom_fetched`), copied into the
runtime's memory: a body past the runtime's limit is the sticky LIMIT, as
any allocation a script makes. A network error is `ok` false and nothing
else, and the page sees one fixed TypeError, "Failed to fetch".

**`XMLHttpRequest`, `XMLHttpRequestEventTarget`, `XMLHttpRequestUpload`,
`ProgressEvent`** (`xhr.c`, `event.c`): the XHR standard over the same
host request; the request out holds its object, as the standard keeps one
with pending activity. The host answers whole, so readystatechange to
LOADING fires for the one chunk and the one progress event at the end of
the body (the order WPT's event-order tests hold). A synchronous request
is the host's `run`, handed a `stop` that is the script task's own budget
(`os64_js_check_budget`): it ends when the task would, and a task it
outlives ends LIMIT, as any script out of time does. `responseType`
`document` and `responseXML` answer null.

The requests out are the registry's fourth list (`DRequest.held`), counted
by `os64_dom_registry_count` and cancelled with the host at drain. The
work a page or a server can size (a header list's sort, a server's block,
a signal's links: `AbortSignal.any` of distinct sources or of copies of
one dependent, a Request following a dependent) is linear or n log n, and
the DOM host holds it there by counting its STEPS (`d_request_steps`,
`d_signal_steps`) at sizes far apart, so a quadratic walk fails by count,
not by a clock. A dependent's links are made by the call that makes it,
with no page code between them, which is what makes a duplicate link one
comparison and a dependent source one walk (abort.c, `d_signal_follow`).
`d_signal_dependents` is the weak-set cases' count of what a collection
left linked.

### P1-W's rows: window's low-hanging fruit

Four rows (MODERN_WEB.md § P1-W, as built; WINDOW_DOCUMENT.md § P1-W is the
plan). Every yonder page is the one document of a top-level window the
person opened, never a frame and never a window a script opened, and each
member answers what HTML gives such a window (browsing.c's header has the
reason for each).

**The window as a browsing context** (`browsing.c`). `frameElement`,
`length`, `top`, `parent`, `origin`, `isSecureContext` and
`originAgentCluster` are each asked of one function of the window's
browsing context (`context_answer`) and none is installed as a constant,
as IFRAME.md § What the open slices do now rules, so a frame's context is
taught there; today every context is a root, and the answers are these.
`[SecureContext]` members ask the same context (`d_secure_context`).
`opener` null (an
assignment of anything but null makes it window's own data property),
`closed` false and `close()` a no-op (no page closes the person's window),
`moveTo`, `moveBy`, `resizeTo` and `resizeBy` no-ops that still convert
their arguments, `captureEvents` and `releaseEvents` no-ops,
`clientInformation` the navigator, `frameElement` null,
`crossOriginIsolated` false, `originAgentCluster` true, `origin` the page's
origin serialized (`d_origin`), and `isSecureContext` whether the page's
address is potentially trustworthy as libway judges it
(`way_url_trustworthy`, the browser's one copy of that judgement: about:blank
and about:srcdoc are, as Secure Contexts says, so a binding with no address
is; the name `localhost` is not). `ononline`,
`onoffline` and `onlanguagechange` are window's handler slots, whose events
nothing fires. `queueMicrotask(callback)` queues a job on the engine's one
job queue, in order with promise reactions; the checkpoint that drains the
queue calls it with `this` undefined, and a throw is that job's failure,
reported as the task's (the first one a task meets is its outcome) while
the jobs after it run, never thrown into the script that queued it.
**The BarProps** (`locationbar`, `menubar`, `personalbar`, `scrollbars`,
`statusbar`, `toolbar`, and `BarProp`): six objects, `visible` true.
**`external`** and `External`: `AddSearchProvider()` and
`IsSearchProviderInstalled()` do nothing.

**`crypto`** (`crypto.c`; Web Cryptography's Crypto interface) and `Crypto`:
`getRandomValues` fills an integer typed array of at most 65,536 bytes and
answers it (anything that is no view, or a view of a resizable buffer, is a
TypeError, as WebIDL makes one without `[AllowResizable]`; then a float
array or a DataView is TypeMismatchError, and more bytes
QuotaExceededError), and
`randomUUID`, [SecureContext], answers a version 4 UUID. `subtle` is absent
(WINDOW_DOCUMENT.md, ruling 1). The bytes are the host's
(`options.entropy`; yonder's is libtls's door, `os64_tls_entropy`, over
`/dev/random`), written into the binding's scratch and copied into the
array only when the host says every one is there: a host that refuses, or
has no hook, is an OperationError, and the page never gets zeros or a
short fill as randomness. A view is told by its class, and its buffer and
the buffer's resizability are read through the engine's own getters, taken
before any page ran (a resizable buffer's view keeps the length it was made
with in the engine's books, so it could not be filled whole), so nothing
here runs page code.

The classic fixes beside them are every page's, the switch on or off:
`window` and `document` [LegacyUnforgeable] (§ Objects and collections),
`history` a read-only accessor, `status` kept (§ Asks).

### The frames row

`frames.c`: the `HTMLIFrameElement` interface over the binding's iframe
prototype, `D_PROTO_IFRAME`, and `src`, reflected as `script.src` is (read
resolved against the document's base, set as written). It is the start of
the HTMLIFrameElement IFRAME.md shapes (§ The cluster and the window
proxy), a modern row of its own, so the modern switch takes it away whole,
and phase F1 adds the rest of its members to it. Setting
`src` sets the attribute, which libpage lists as the frame's link, and
loads nothing: a frame a script inserts and addresses this way, as scripts
most often do, is the stand-in a parsed one is. With the modern switch off
the prototype is there, as every kind's is, with HTMLElement's members
alone, so `src` set as a property is the script's own and the frame names
nothing, as before; `setAttribute` and `innerHTML` name it either way.

### P2a's rows: a script's failures

One row (MODERN_WEB.md § P2a, as built; WINDOW_DOCUMENT.md § P2a is the
plan): window's `onerror`, `onunhandledrejection`, `onrejectionhandled` and
`reportError`, the document's `onerror`, and `ErrorEvent` and
`PromiseRejectionEvent` (`report.c`, and the two interfaces in `event.c`).

**The binding is the runtime's script host** (libjs CONTRACT.md § A script
host), on every page and from the end of `os64_dom_create` to drain: libjs
hands it every exception it catches and leaves it the rejections a
checkpoint left unhandled. With the switch on it fires the window's events
for them; on or off, a rejection nothing handled is a failure the host
records (below). A host that runs a script with `os64_js_run` compiles it
under the name `os64_dom_script_name` gives it (`os64-script:<n>`, the
binding's own, standing for the script's address: where its response came
from, after any redirects, as HTML mutes by the response) and follows it with
`os64_dom_script_ended`, which settles what the script left in a task of
the host's name for it; a task the binding opens (a dispatch, a timer, a
fetch's answer) settles its own before it closes.

**When each event fires.** An exception the binding caught while a script
ran (a nested dispatch's listener, a handler's compile, `reportError`) is an
`error` event at once, as HTML's "report an exception" is; one libjs caught
is the event at the task's next turn between calls: after the listener or
timer callback that threw and before its checkpoint, or after the
checkpoint whose job threw. A script the host ran is the exception: libjs
opens no task while jobs wait, so the jobs a script that threw had queued
run before its `error` event, where HTML fires the event first (DEBTS.md;
a string timer the same). A rejection is unhandled when a checkpoint ends with it unhandled; its
`unhandledrejection` (cancelable) is fired after that checkpoint, oldest
first, each followed by a checkpoint, until a checkpoint leaves nothing. A
reported promise a handler reaches later has its `rejectionhandled` after
the checkpoint after the handler; one a handler reached during its own
`unhandledrejection` has none. All of it runs in the task the failure
happened in, under its deadline, so a listener that rejects for ever ends
as any loop does. While an `error` event is being fired, a second error is
reported to no page (HTML's error reporting mode).

**What an event says.** An ErrorEvent's message is `Uncaught ` and the
value as the binding reads it without running the page's code: a
DOMException's or an Error's name and message (read only through data
properties of ordinary objects and errors, so no getter and no proxy trap
runs), another object's message read the same way, a primitive as it is,
else a fixed phrase.
Its filename is the address of the script whose failure it is (the one
the host gave `os64_dom_script_name`), else the page's; its line and
column are the first placed frame of an Error's own stack, counted from
the start of the script the engine compiled (an inline script's from the
script, not the document), else where the running script is when the
binding is the one reporting, else 0. The place is read from the stack's
text; whose failure it is never is.

**Muted errors.** What a script from another origin than the page's
(HTML's muted errors; the host fetches every script without CORS) throws
or reports is told as `Script error.` with no filename, place or error,
and its rejections fire no event, not even a `rejectionhandled` when a
handler reaches one later, whoever's handler it is. One rule decides
whose failure it is: the script that was running when the value was
thrown, rejected or reported, as the binding knows each script, by the
name it compiled it under. A rejection and `reportError` are asked of the
engine's stack as they happen (QuickJS's `JS_GetScriptOrModuleName`, the
nearest frame of a script the binding named). An exception has left its
frames when it is caught, so it is the script the host was running, or
the one whose callback the binding called (a listener, a handler
property, a timer, a `queueMicrotask` job), each callback's script asked
as it was handed over: the name it was compiled under when that is one of
the binding's, else the script handing it over (a bound function, one
`eval` made). A handler compiled from an attribute, and a string timer,
are the page's, as HTML makes them. So a function of another site's that
the page's own code calls, and that throws through it, is the page's
failure, as HTML's "run a classic script" reports it, while a rejection
it makes is the other site's. The record keeps them all whole. The cost
of the names: a page's `error.stack` and a function's `fileName` say
`os64-script:<n>` where a browser names the address (DEBTS.md). A
PromiseRejectionEvent carries the promise and its reason.

**The record.** A rejection no `unhandledrejection` listener cancelled is
the task's failure: UNHANDLED_REJECTION, `unhandled rejection: <reason>`,
the reason read as an event's message is. The first a task meets is its
outcome, unless the task already failed; those after it are told to the
host through `options.failed`, the first sixteen one by one and the rest
as one counted line as the task closes (`and <n> more unhandled
rejections`), so what a page's flood costs the host is not the page's to
size, and none goes unrecorded. An
exception's outcome is libjs's diagnostic as before, whatever an error
listener did with its event. Each failure carries its stack: libjs's
diagnostic takes an exception's, and the binding takes a rejection's
reason's and an error's it reported itself (an Error's own `stack`, read as
a data property so no page code runs, `d_error_stack`) into the outcome's
`stack_trace`, and
gives it to `options.failed` beside the message. A host says where a
failure happened with `os64_dom_stack_frame` (a stack's first frame that
names a place) and `os64_dom_frame_script` (the address of the script
that frame's file names, or the page's own), and parses no engine text of
its own: yonder's record does (YONDER_DIAGNOSTICS.md).

**The interfaces.** `new ErrorEvent(type, {message, filename, lineno, colno,
error})` and `new PromiseRejectionEvent(type, {promise, reason})` read
EventInit's members, then their own in name order (a PromiseRejectionEvent's
`promise` a required object); their attributes are the event's own record,
traced by the collector. `reportError(e)` reports `e` as above and returns.

### P1-Da's rows: the document's state reads

Two rows (MODERN_WEB.md § P1-Da, as built; WINDOW_DOCUMENT.md § P1-Da is
the plan). The document row puts its members on Document's prototype and
nothing on window, so it lists no name; the DOM host's chain shape holds
it instead (`tools/test_dom_modern.inc`). Each member answers for the
document it is asked of: the window's, or a document of its own, which
answers what HTML gives a document with no browsing context (document.c's
header has every answer).

**The document's state** (`document.c`). `readyState` is the readiness the
host's own dispatches move, by one rule (`os64_dom_readiness_after`):
DOMContentLoaded at the document makes it `interactive`, load at window
`complete`, and `os64_dom_dispatch` moves it before anything else, heard
or not, firing `readystatechange` at the document as a task of its own
just before the event that moved it. A binding made late is told where the
document stands (`options.readiness`). `visibilityState` "visible",
`hidden`, `prerendering` and `wasDiscarded` false. `referrer` is
`options.referrer`, the binding's own copy. `activeElement` and
`hasFocus()` ask `options.focus`, the host's answer at every read; an
element it names that is not in the window's document is no focus, and no
focus is the body, else the document element, else null.
`currentScript` is the element the host names with
`os64_dom_set_current_script`, held while it is named. `URL` and
`documentURI` are the address location reads; `domain` its origin's host,
its setter HTML's checks (a SecurityError) and then nothing, the agent
cluster being origin-keyed. `compatMode`, `characterSet` (and `charset`,
`inputEncoding`) and `contentType` are libhtml's mode and encoding and
`text/html`; a document of its own is `CSS1Compat`, `UTF-8` and the type
its making gave it (`application/xhtml+xml`, `image/svg+xml`,
`application/xml`). `lastModified` is `options.last_modified` in the local
time zone, else the time it is asked. `title` is the first `title` element's
text, ASCII whitespace collapsed, an SVG document's own; its setter
converts the value, then replaces that element's text, making one in the
head (or, for SVG, first in the root) when there is none. `dir` reflects
the html element's, `rootElement` is an SVG root, `clear()` does nothing.
`onreadystatechange`, `onvisibilitychange` and `onprerenderingchange` are
the document's handler slots (§ Events).

**`CustomEvent`** (`event.c`) and its `detail` and `initCustomEvent`, the
detail held in the Event's own record and traced with it. With the switch
on, `document.createEvent` reads DOM's table without regard to ASCII case:
`Event` (and `Events`, `HTMLEvents`, `SVGEvents`), `CustomEvent`,
`HashChangeEvent` and `MessageEvent` (§ P2b's rows) are made as
themselves, a name off the table is a
NotSupportedError, `TouchEvent` too (a desktop does not expose the legacy
touch APIs), and the table's names whose interfaces are not built (MouseEvent,
UIEvent, KeyboardEvent and the rest) make a plain Event, as every name did
before (MODERN_WEB.md § P1-Da, as built, says why).

The classic fix beside them is every page's, the switch on or off:
`document.body`'s setter (HTML § 3.1.4), which replaces the body or appends
one to the root, through the verbs appendChild uses.

### P2b's rows: clone, message, idle

Three rows (MODERN_WEB.md § P2b, as built; WINDOW_DOCUMENT.md § P2b is the
plan): `structuredClone` (`clone.c`); `postMessage`, `onmessage`,
`onmessageerror` and `MessageEvent` (`message.c`, and the interface in
`event.c`); `requestIdleCallback`, `cancelIdleCallback` and `IdleDeadline`
(`idle.c`). Each file's header has its contract whole.

**The structured clone** is HTML's StructuredSerializeWithTransfer and
StructuredDeserializeWithTransfer, the binding's one copy of them:
`structuredClone`, `postMessage` and a performance mark's `detail` all go
through `d_clone_write` and `d_clone_read`. The record is no realm's: what
it holds (primitives, property keys as atoms, bytes) is the runtime's,
which every realm of a page shares, so the writer reads in the caller's
realm and the reader builds in the target's, each through the intrinsics
its own binding took before any page ran (`d_clone_setup`), as IFRAME.md's
postMessage across realms needs. Cloned: the primitives (a lone surrogate
kept), the Boolean, Number, BigInt and String objects, Date, RegExp (source
and flags; `lastIndex` 0 again), ArrayBuffer (a resizable one keeps its
maximum), every typed array and DataView (sharing their buffer as they
shared it), Map and Set (their entries in order, never their own
properties, empty or not), Error and its six native kinds (name,
message, and an own `cause` and `stack`), arrays (length, holes and
non-index properties) and ordinary objects (each own enumerable
string-keyed property read with [[Get]], so getters run, in
[[OwnPropertyKeys]] order, a key a getter deleted first skipped), Blob,
File and DOMException. A value met twice is one value in the copy. Anything
else is a DataCloneError: a Symbol, a function, a Proxy, a
SharedArrayBuffer (no page here is cross-origin isolated), a detached
buffer, a view out of bounds, a Promise and the other objects with
internal state, and every other platform object (`location`, `history` and
`navigator`, ordinary objects in this binding, among them). The transfer
list's ArrayBuffers (any other value, a duplicate or a detached one is a
DataCloneError, a primitive a TypeError) have their bytes copied as the
value is written and are detached only once the clone has succeeded whole
(structuredClone's copy made, postMessage's message queued), so a refusal
anywhere detaches none. A view a getter's resize left past its buffer is
cloned as it was, out of bounds over the buffer as it was. A
length-tracking view's copy tracks no length (DEBTS.md). The record is
engine memory, charged as the page's values are; both walks are
iterative, with an address-keyed memory that holds what it has met, so a
graph's depth costs no C stack and a page sizes linear work.

**Posted messages.** `postMessage(message, targetOrigin, transfer)` and
`postMessage(message, {transfer, targetOrigin})` post to the window whose
member it is, which is the caller's own: no other window exists for a page
here to name. `targetOrigin` is `*`, `/` (the default, the poster's
origin) or a URL whose origin it is (anything else a SyntaxError, before
the message is read); then the message is cloned in the poster's realm and
queued at the target window, one task each, oldest first. The host runs
them (`os64_dom_message_next` says when the oldest was posted, so a host
can order them among its timers by time; `os64_dom_message_fire` runs
one). A message whose targetOrigin the document's origin does not match
as the task runs, or that nothing could hear, is dropped without a word
and without entering the engine, at its own turn: a message behind it
posted after a timer fell due waits for that timer, which may add its
listener; else the clone is read in the target's
realm and a MessageEvent `message` fired at window, with its data, the
poster's origin, the poster's window as `source`, `lastEventId` "" and an
empty frozen `ports`; a copy the reader refuses with a catchable
exception fires `messageerror` instead, which no page reaches today (the
reader's only refusals in the realm that wrote the record are memory and
the deadline, which end the task; `message.c` says so). A message a message's task posts is a later task. The queue's
nodes are the binding's memory; the poster's window each message holds is
the registry's sixth list. **MessageEvent**'s constructor reads its init's
members in name order (`data`, `lastEventId`, `origin`, `ports`, `source`);
`ports` must be empty, there being no MessagePort, and `source` must be the
window or null; `initMessageEvent` too, and `createEvent('MessageEvent')`
makes one.

**Idle callbacks.** `requestIdleCallback(callback, {timeout})` answers a
handle and puts the callback on the window's list; `cancelIdleCallback`
takes it off. The host says when its loop is idle (`os64_dom_idle_run`),
and the binding runs one callback a call, as a task: when the page has no
timer due and no message queued, it opens an idle period if none is open
and one may start, every callback asked for until then becoming runnable,
and runs the oldest while the period's deadline has not passed. The
deadline is HTML's computeDeadline, read again at every
`IdleDeadline.timeRemaining()`: 50 ms after the period began, or the
window's next timer if that is sooner. A callback asked for during a
period's callback waits for the next period, which starts no sooner than
50 ms after the one it was asked in (Chromium's rule, so one that asks for
itself again runs once a period, never once a turn of the host's loop);
any other lets the next period start at once (`os64_dom_idle_next` says
when one may run). A timeout is a timer of the window's (§ Timers), so it
counts among the 4096 and wakes the host as a timer does, though no
`clearTimeout` reaches it (a timer the binding sets for itself is the
binding's to cancel); it calls the
callback, `didTimeout` true and `timeRemaining()` 0, unless a period ran it
first, which cancels the timer. A callback is called with `this` the
window; what it throws is its script's failure (§ P2a's rows). The entries
are one array in request order, so in handle order: a cancel or a timeout
finds its entry by binary search, and the holes run and cancelled entries
leave are closed up once they outnumber the entries. The entries are the
binding's memory (a page's flood of requests ends in QuotaExceededError);
their callbacks are the registry's seventh list.

### 0f P1a's rows: the node row and the select row

INTERFACES_FORMS.md is the plan (its § P1a); MODERN_WEB.md § Slice 0f, as
built, the record. The interface objects it added (`Text`, `Comment`,
`SVGElement`, `HTMLOptionElement`, `HTMLOptGroupElement`) are the
interfaces row's, above; two rows of their own carry the members.

**The node row** (`mixin.c` `d_nodes_install`, naming nothing on window):
`getRootNode(options)` on Node's prototype, the root of the node's tree
(no shadow trees, so `composed` changes nothing, though the options are
converted as WebIDL converts a dictionary), and on Attr's, whose shadow
answers it with the Attr itself; Text's `wholeText` (the data of the run
of text siblings it is in) and `splitText(offset)` (the offset in UTF-16
code units; the tail a new text node, the next sibling when there is a
parent; the node shortened first and given its data back if the insertion
fails).

**The select row** (`options.c`, which names `Option` on window): an
option's `value` (its attribute, else its text), `text` (its descendant
text with scripts left out, ASCII whitespace stripped and collapsed;
setting it is textContent's), `label`, `selected`, `defaultSelected` (the
`selected` attribute), `disabled`, `index` and `form`; an optgroup's
`disabled` and `label`; a select's `options`, `length` (settable),
`selectedOptions`, `multiple`, `size`, `item`, `namedItem`, `add` and
`remove(index)`, whose no-argument form is ChildNode's `remove()`. An
option's selectedness is libpage's control state
(`os64_page_node_option_selected`): in a select without `multiple` it is
whether the option is the one selectedIndex names (a one-line select's
first enabled option when none is selected), and setting it is HTML's
setter, selectedIndex set to it when on, and when off the option
deselected and the select left to fall back. A select's list of options
is libpage's walk (`os64_page_option_next`), which skips what HTML's list
of options skips. `options` and `selectedOptions` are live collections
(the collection kinds OPTIONS and SELECTED_OPTIONS, made once per select),
the second refreshed when control state changes as well as the tree;
`options` has HTMLOptionsCollection's prototype, over a collection's, with
`length` (settable: options taken off the end, or empty options appended,
at most 100,000 at once), `selectedIndex`, `add` and `remove`, and an
indexed setter (`options[i] = option`: null removes the option at `i`, an
index past the end appends after empty options fill the gap, one within
replaces), so its indices read as writable. Its interface object waits
for HTMLCollection's, which it inherits (INTERFACES_FORMS.md § P1b).
`new Option(text, value, defaultSelected, selected)` is the legacy
factory function: its `prototype` is HTMLOptionElement's, and every
argument is converted before the option is made. A select's own indices
(`select[0]`) are a DEBTS.md row (§ Classic browser scripting).

**The controls row** (`controls.c`, naming nothing on window): input's,
textarea's, select's and button's reflections, as one table of rows, each
a member, its content attribute, which of HTML's reflection rules reads
it (string, URL, boolean, enumerated with its missing and invalid
defaults, and the integer kinds with their defaults and range errors) and
the interfaces it is on; one accessor reads every row, and a row's member
called on another interface is a TypeError. Beside them textarea's
`defaultValue` and `textLength`, `autocomplete` through HTML's autofill
grammar, input's `width`, `height` and `indeterminate` (libpage's
`os64_page_node_set_indeterminate`), and every control's `form`
(`d_form_owner`) and `labels`: a live NodeList made once (the collection
kind LABELS), of the labels whose labeled control (`d_label_control`) the
control is, walked from its tree's root.
