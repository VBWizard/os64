# IFRAME.md — a document inside a document: frames, realms and the window proxy

*Tier 1 of the Modern Web arc ([MODERN_WEB.md](MODERN_WEB.md) § The tiers:
"`iframe` with `postMessage` ... the arc's biggest structural change and
gets its own design document before any code"). Written by Fable on
2026-10-23 at `userland` d53bb802, from the tree as it stands (every
function and line named below was read that day; the map is § What the
tree is today) and from the HTML standard's browsing-context, window
proxy and cross-origin-object sections. It is written before Tier 0 is
finished on purpose: slices 0e, 3 and 5c and the two WIRE.md PRs each
touch a seam a frame needs, and § What the open slices do now says what
each does differently so this design lands on a tree shaped for it. No
host names from the corpus appear here; counts only.*

*Revised 2026-11-07 at `userland` a0af6b11, when Chris asked for the
frames design and it turned out to exist. Nothing settled below changed:
the eight rulings and the three cuts stand as written. What changed is
the tree under them — 0e's P1-W and P2b, modules, RELAYOUT, WIRE,
Shadow DOM, Range, MessageChannel and the load events all landed on the
seams § What the open slices do now named, most of them the way it
asked — so § What the tree is today is re-read against today's code,
§ What the open slices do now says what each did, the patch number is
corrected (the series reached 0017 meanwhile), and § Workers says how
WORKERS.md, written the same day, hangs off a frame. The consumer that
reopened it: a search engine's unusual-traffic page, whose CAPTCHA is a
frame that speaks to its page only by `postMessage` (DEBTS.md's frames
row). Anchors below name functions, not line numbers; every one was
found in the tree that day.*

## What this settles

1. **A frame's content is a Page, and the window holds a TREE of Pages.**
   Today one yonder window owns one `Page` on screen and one in each of
   two waiting roles. After this, every `Page` has a parent (NULL at the
   root), the frame element it fills, and its child frames; `g.page`
   stays the root the address bar, the stop button, the history and the
   record belong to, and each waiting role is the root of its own tree.
   Everything a Page owns today (document, sheets, cascade, layout tree,
   pictures, scroll, script host, serial) a frame's Page owns the same
   way, so the frame's loading, layout and painting are the code that
   loads, lays out and paints a page, called once more per frame. No
   second kind of page exists.
2. **One engine runtime per top-level page, one realm per document.** The
   JavaScript runtime stays the top-level page's (its heap, its memory
   limit, its job queue, its teardown); each document in the tree gets a
   realm of its own, a QuickJS context in that runtime, with its own
   global object, intrinsics and prototypes. This is the standard's
   shape (one agent for the frame tree, one realm per window) and the
   only shape QuickJS allows for same-origin frames, where the parent's
   script holds the child's objects directly: a value belongs to its
   runtime, and two runtimes cannot share one. libjs gains realms; libdom
   gains the per-runtime CLUSTER that owns what is shared (the class
   registrations, the document-to-binding registry) and one binding per
   document; a node's wrapper is made in the realm of the node's
   document, whoever touches it, so `instanceof` answers per realm as it
   does in a browser.
3. **Script sees the window proxy and never the global object.** One
   proxy per browsing context, the same JavaScript object across every
   document that context shows, forwarding to the current document's
   global. `window`, `self`, `globalThis`, `frames`, `top`, `parent`,
   `frameElement.contentWindow`, the `this` of a sloppy function and of a
   top-level script all answer the proxy, so identity holds across the
   boundary (`parent.frames[0] === window` inside the frame is true, and
   `window.top === window` in a top-level page stays true). The engine
   substitutes the global object for an undefined `this` at
   `OP_push_this`; a one-line patch in the series (the next number when
   F1 is cut; 0018 at the revision) makes that substitute a per-context
   value the binding sets to the proxy.
4. **Cross-origin access is the standard's list and nothing else.** Two
   documents are same-origin when their origins compare equal, exactly
   (scheme, host, port; an opaque origin only to itself). Across origins
   the proxy answers `window`, `self`, `location` (as a location proxy
   whose only verbs are setting `href` and `replace`), `close`,
   `closed`, `focus`, `blur`, `frames`, `length`, `top`, `opener`,
   `parent`, `postMessage`, and the frames by index and name; every
   other get, set, call, delete or enumeration throws `SecurityError`.
   `document.domain` never relaxes it (P1-W's `originAgentCluster` is
   true, the modern default).
5. **A frame lays out AFTER its parent, paints clipped inside its box,
   and is hit through its box.** An `iframe` is a replaced box whose
   size comes from CSS and its attributes and never from its content, so
   the parent's layout finishes without the child, and each frame's
   `flow_layout` then runs as a sibling call with the box's content size
   as its viewport. Nothing nests inside `flow_layout`, which is what
   lets yonder's one-at-a-time layout state stay one at a time. A frame
   whose viewport did not change and whose document did not move is not
   laid out again.
6. **A frame's navigation adds no session entry.** Back and forward move
   the top-level page only; a frame navigating (its `src` set, a link
   followed inside it, `location` assigned, `pushState`) replaces its
   own current document and entry and pushes nothing. The standard's
   joint session history makes Back step through a page's ad rotations,
   which is the web's most-complained-about behaviour, and one person on
   one machine gets the useful half. DIVERGENCES.md row. Ruled (§ Rulings).
7. **A frame departs at the end of the task that removed it, and its
   document lives while anything still holds it.** Removal from the
   tree, a navigation of the frame, or the parent's own departure all
   schedule the departure; none runs it mid-task, because the task that
   removed the frame may be the frame's own script. A departed document
   keeps its arena while any wrapper of its nodes is alive in the
   runtime (the parent may have kept `contentDocument` in a variable);
   the last wrapper's finalizer frees it, and the runtime's teardown
   frees whatever is left.
8. **A frame's overrun retires the frame.** The script budget stays per
   task. When a frame's task exceeds it, the interrupt unwinds that task,
   the frame's realm is retired (no task of that document runs again, the
   document stays on screen as it is) and the runtime, the parent and
   the siblings continue; the top-level document's own overrun retires
   the whole tree, as today. This needs libjs's sticky-outcome rule to
   split (§ Realms) and the libjs owner's confirmation that an
   interrupted task leaves the heap sound, which upstream's interrupt
   design intends; failing that, any overrun retires the tree and the
   DEBTS row says so.
9. **Out of this design, each with its row:** `window.open` to a new
   window and `opener` (P3a answers `null`, the blocked popup, and the
   status line says a popup was blocked; a second GUI window is the
   window model's design, not the frame's); `frameset` and `frame` (a
   layout mode of their own); `MessageChannel` and `MessagePort`;
   Content Security Policy beyond `frame-ancestors`; a third-party
   cookie policy (cookies keep today's by-destination rule until the
   SameSite row is paid, and this design records what that row needs
   from frames); `loading=lazy` (eager, which the standard allows).

## What the tree is today

The facts this design moves, with where they live, so the builder and the
reviewer check the same things. Re-read 2026-11-07; the 10/23 line numbers
are gone, since 800 commits moved them, and each fact names its function.

- **One Page per role.** `g.page` is on screen; `g.coming.page` waits for
  its sheets; `g.stream.page` is arriving; `g.finishing.page` points at
  one of them (yonder.c, `struct Page` and the anonymous `g`).
  `page_of_doc` and `scripts_for` find a page by comparing against
  exactly these slots, and the loop's deadline scans walk the fixed
  array `{g.page.scripts, g.stream.scripts, g.coming.page.scripts}` in
  three places. One window. `g.page` is read 516 times in yonder.c.
- **One runtime per Page, one context per runtime, one binding per
  runtime.** `ensure_runtime` (scripts.c) makes the runtime at the first
  script; `struct os64_js_runtime` holds one `JSContext`
  (libjs/runtime/runtime.c); CONTRACT.md forbids consumers creating
  another; `os64_dom_create` refuses a runtime that already registered
  its classes and installs `window`, `self` and `frames` as the global
  itself (core.c). **Since 10/23:** `top`, `parent`, `length`,
  `frameElement`, `origin`, `isSecureContext` and `originAgentCluster`
  are answered by one function from the window's browsing context
  (browsing.c `context_answer`, P1-W, built as § What the open slices do
  now asked), so the frame slice teaches that function what a frame's
  context answers and reshapes no member.
- **A document knows no URL and no origin.** The URL is `way_page_t.url`,
  copied into the script host and the binding (`dom->url`); the origin is
  computed from it on every ask (`d_origin`, url.c; `request_origin`
  in yonder.c, which gives an opaque origin the page's serial). Every
  script request carries its page's origin and serial (`request_job`);
  pictures and web fonts match their arrival against `g.page_serial`,
  and web fonts and navigations read their referrer from `g.page`'s way.
  **Since 10/23:** WIRE.md's `Sec-Fetch-Dest`, `-Site`, `-Mode` and `-User`
  are sent (agent.c's header orders per request kind), computed from the
  requesting page; a frame's navigation needs `Sec-Fetch-Dest: iframe`
  and `-Site` from the initiator document, which is a destination kind
  and a document argument, not a new header.
- **An `iframe` is a link with a grey box.** libhtml parses it as raw
  text (tree.c); libpage lists its `src` as a link; libflow makes it a
  replaced box, 300 by 150 CSS px with a 2 px inset border (boxes.c,
  layout.c, style.c); yonder's frame stand-in draws "Frame: <address>"
  (yonder.c, the `frame_label` text) and a click follows the link in the
  whole window; `diag`'s nothing-list (`kNothing` in yonder.c) counts it
  under `MISSING element iframe`. **Since 10/23:** libdom has frames.c,
  `HTMLIFrameElement` with `src` alone, a modern row installed by
  `d_frames_install`; its header says it is this design's start. F1
  fills it in.
- **Layout and paint assume one page.** `lay_out_page_reclaim` fills the
  single static `s_env` and is not reentrant; `glass_image` and
  `frame_label` read `g.page`; `view_read` answers scroll only for
  `p == &g.page`; `s_measured` is one measurement cache for a page not on
  screen; the viewport is `g.view.bounds`; scroll is the single pair
  `g.sx`/`g.sy`. **Since 10/23:** RELAYOUT's memo and GARB's cascade are
  per Page as asked; Shadow DOM's hit testing and focus already record
  the document beside the node (`g.focus_node`/`g.focus_doc`), which is
  the half of "focus is one node in one document across the tree" that
  F2 needed; the other half is the Page.
- **The loop works on `g.page.scripts`.** `script_turn` and `page_event`
  address the shown page; the timer scan walks the fixed array above.
  Timers are per binding (`dom->timers`); the promise jobs are the
  runtime's. **Since 10/23:** the load events (LOAD_EVENTS.md) owe a
  script's and a sheet's `load` from the page's host, and
  `os64_dom_readiness_after` moves readiness for the document in hand;
  a frame's "delay the load event" is one more clause in the same
  `loaded` condition, as § The Page tree says.
- **Messaging exists for one window.** **Since 10/23:** message.c is
  P2b's `window.postMessage` to the window itself, written as this design
  asked — the poster and the target are two parameters that happen to be
  one binding, the clone (clone.c) is written in the poster's realm and
  read in the target's, the origin check runs at delivery, and the task
  is one per message, oldest first. clone.c's header says the record is
  no realm's. `MessageChannel` and `MessagePort` landed 11/07: a port's
  messages ride the same queue, a port in a transfer list is refused
  (its DEBTS row). So F1's "postMessage across realms" is the second
  realm and nothing else, as hoped. `contentWindow`, `contentDocument`,
  `frameElement` (null), `opener` (null) and `window.open` (absent:
  DEBTS.md's frames row) are where 10/23 left them.
- **Still open from 0e:** P3a (`window.name`, `open` with a target,
  `stop`) and P3c (window named access) have not landed; their DEBTS
  rows stand ("`window.name` and `Node.baseURI` are absent"; "Window
  named access is absent"). The sentences § What the open slices do now
  gives them still bind whoever builds them, before or after F1.

## The Page tree (yonder)

**The struct.** `Page` gains `parent` (the Page whose document holds the
frame element; NULL at a root), `frame` (the `iframe` node this Page
fills; NULL at a root), `first_child`/`next_sibling` (its frames, in tree
order), a `name` (the browsing context's name, the `name` attribute at
birth and `window.name` after; this is where P3a's `name` lives), the
`sandbox` flags in force, and a `Stream *stream` of its own (the
navigation arriving for this frame, if one is). `g.stream`'s fields that
describe an arriving page (`parser`, `serial`, `timing`, the mailbox's
generation, the kept-bytes state) become that `Stream` struct, allocated
per load; the root's is the one the address row, the stop button and the
question bar refer to, and `g.nav` keeps naming it. Every per-page static
named above (`s_env`, `s_laying`, `s_measured`, `g.sx`/`g.sy`, the
`g.page` reads in `glass_image`, `frame_label`, `view_read`, the referrer
and arrival-matching reads) moves onto the Page or into the call. The
rule for every builder from now on, in this slice and before it: **the
Page in hand, never `g.page`**. `g.page` means "the root the window
shows", and code that means "the page this node belongs to" asks
`page_of_doc`, which walks the trees.

**Birth: a frame exists the moment its element is in a document.** When
an `iframe` element is inserted into a document that has a browsing
context (the parser's insertion, or a script's `appendChild`), yonder
makes its Page at once with an `about:blank` document (an empty
`html`/`head`/`body` from libhtml, no fetch), whose origin is the
creator document's origin and whose `referrer` is the creator's URL, as
the standard's initial about:blank. This is synchronous because pages
depend on it: `iframe.contentDocument.write(...)` right after
`appendChild` is how old libraries and most ad code build a frame. The
Page is cheap; the REALM is not made until something needs it (the
first script in the frame, or the first touch of its `contentWindow` or
`contentDocument` from outside), so a page with forty empty frames costs
forty small documents and no contexts. The parser's insertion reaches
yonder through the stream's own tree walk after each slice (where
libpage finds sheets today, `os64_page_sheets_in`); a script's insertion
reaches it through the mutation core's insertion verbs, the same place
RELAYOUT.md's 5a change log is written.

**Load: `src` is a navigation of the frame.** A frame with a non-empty
`src` starts a load when its element is inserted, and again whenever
`src` is set to a different value (the standard's "process the iframe
attributes"). The load is a navigation through the pool exactly as the
window's is (`start_trip`'s judgement, `way_judge`, the mixed-content
rule against the frame's own document and the top-level page both: an
`http:` frame in an `https:` page is blocked, as browsers do), with these
frame-only steps once the head arrives:

- **May it be framed?** `X-Frame-Options: DENY` refuses; `SAMEORIGIN`
  refuses unless every ancestor document's origin equals the frame's;
  CSP's `frame-ancestors` directive (the one CSP directive this design
  parses; the rest is Tier 1's CSP slice) is checked against the
  ancestor chain, and when present wins over `X-Frame-Options`. A
  refused frame shows an empty document, fires `load`, and the record
  gets one `FAILED frame refused-by-server` line. Honouring this is not
  for our safety; it is the server's instruction about its own page, and
  a page that assumes it is never framed breaks in ways that look like
  ours.
- **`sandbox`.** The flags map onto doors yonder already has: without
  `allow-scripts` the frame runs no script (its Page is made with
  `scripting` off, the per-page switch); without `allow-same-origin` its
  origin is opaque (`request_origin` already makes one from a serial);
  without `allow-forms` a form inside it does not submit; without
  `allow-top-navigation` (and its `-by-user-activation` variant, treated
  as the plain one) a `_top` target or `top.location` assignment from
  inside is refused; without `allow-popups` nothing changes, since
  popups are blocked anyway. Flags the slice cannot mean (`allow-modals`,
  `allow-pointer-lock`, `allow-presentation`, `allow-orientation-lock`)
  parse and do nothing, named in DIVERGENCES. The flags are read when the
  navigation starts, not when the attribute changes, as the standard
  says.
- **`srcdoc`** is a document from a string: a stream fed from memory, no
  request, origin the parent's. That mechanism is also the cure
  DECLINED.md names for a `javascript:` link's string result, and this is
  where it is built (cut F3).
- **Nested depth and count are bounded**: a frame inside a frame inside a
  frame past depth 8, or a tree past 64 frames, gets an empty document
  and a `FAILED frame too-many` line. Pages that recurse into themselves
  exist; the bound is the only defence against them and is stated in
  one constant.

**The load event waits for the frames.** A document's `load` fires only
when its own sub-resources and every child frame's `load` have fired or
failed (HTML's "delay the load event"); the frame element fires `load`
after its document's. The condition yonder's `loaded` already computes
gains one clause: no child frame still loading. A frame that is refused,
over the bound, or whose fetch failed counts as finished.

**Departure.** Three doors, one rule: the Page is marked departing, and
`page_departs` for it runs at the end of the current task (the loop's
turn, after the task that set the mark and its microtask checkpoint).
The doors are the element's removal from its document (the mutation
core's removal verbs and the parser's own), the frame's navigation (the
new document replaces the old on arrival, as the window's does), and the
parent's departure, which departs its children first, depth-first, so no
child outlives the document that holds its element. Departure drops the
frame's stream, drains its binding (timers, listeners, requests, storage
commit, as `page_departs` does for the window), retires its realm, frees
its layout, pictures and sheets, and leaves the document to the rule in
§ What this settles 7. A frame departing while its own script is on the
stack is the case the end-of-task rule exists for; the harness carries it
(a frame removing itself from a click handler, and from a timer).

**Layout.** `lay_out_page_reclaim` runs for the parent with the frame
boxes sized by CSS and the `width`/`height` attributes (the replaced
oracle gains no frame case; the box's size is never the content's). When
the parent's tree is final, each child frame whose box content size
differs from its `laid_width`/`laid_height`, or whose document version
moved, is laid out with that size as its viewport and its own zoom
inherited from the window; `css_view` for a frame is the frame's size, so
media queries inside it answer for the frame (that is what the nine
excluded `cssom-view` pages measure). The order is depth-first after the
parent, so a frame's frame follows. RELAYOUT.md's memo is per Page and
needs nothing more: a frame is not part of its parent's tree, and a
parent change that did not resize the box leaves the frame's layout
alone.

**Paint and hit testing.** The parent's paint reaches the frame box and,
instead of today's label, paints the child: `yonder_paint` for the
child's tree with the box's content rectangle as the viewport, the
child's own scroll, clipped to the box and translated to it; the border
and background of the box are the parent's. A frame's paint cache is its
Page's. `flow_hit` at the parent lands in a frame box, and the hit
recurses into the child with the point translated, so hover, click, focus
and the wheel reach the child's document and its binding; the child's
`page_event` dispatch is the parent's code with the child's Page. The
wheel over a frame scrolls the frame while it can scroll in that
direction, then the parent (the chaining browsers do); `scrolling=no` and
`overflow: hidden` on the frame's root make it never scroll. Focus is one
node in one document across the tree (`g.focus_*` records the Page as
well as the node); Tab order crosses into a frame's focusable nodes in
tree order and out again. `frameborder=0` and the UA border stay as they
are.

**The record and the corpus.** The record is the top-level page's, as
today. A frame's lines fold into it: its `MISSING` and `FAILED` lines are
counted with the page's (one reader, one tally, no second grammar), and
one fact line `frames: <n> loaded, <m> refused` is added. `iframe` leaves
`diag_elements`' nothing-list; `frame` stays on it. With `keep_pages` on,
a frame's bytes are kept beside the page as a sub-resource of kind
`frame` with its address, so a kept page replays with its frames answered
from disk through the same reader; REPLAY.md's grammar gains that one
kind and nothing else. A frame refused by the server is recorded, not
kept.

**Target names.** A link or form inside a frame with `target=_parent` or
`_top` navigates that ancestor (subject to the sandbox); `_self` and no
target navigate the frame; a name that matches a frame's `name` in the
tree (nearest first, then the whole tree) navigates that frame;
`_blank` and an unknown name do what they do today. `window.open` with
`_self`, `_parent`, `_top` or a frame's name is the same navigation ask
(P3a already leans this way); anything else answers `null`.

**The right-click menu** (YONDER_HONEY_DO.md § 5, not built; its verbs are
chosen by what is under the pointer) gains three verbs when the pointer
is over a frame: **Open frame here** (the window navigates to the
frame's current address, the frame's document becoming the page),
**Open frame in new window** (`yonder <address>`, the honey-do's rule),
and **Copy frame address**. Over a link inside a frame the link's verbs
come first and the frame's after. This is the door Chris asked for on
2026-10-23: a way to the page that is IN the frame that is not a click,
because once frames are real a click inside one is the frame's own.
**These three verbs need no frames, only the menu**: today's stand-in
box already follows the frame's address on a click (paint.h's `frame`
contract, libpage's link), and the menu's `element_at` answers an
`iframe` as it answers a link, so they land with the menu's first slice,
whenever that is built, and keep working unchanged when F2 replaces the
box with the document. A frame whose `src` a script sets later has no
address until that script runs, and no verb can help it; that is the
script's problem and the record says which script.

## Realms (libjs)

**The contract change.** CONTRACT.md's "consumers must not create
additional contexts" becomes "consumers create realms through the
library and nothing else": `os64_js_realm_create(runtime, outcome)`
returns a realm handle wrapping a new `JSContext` in the runtime (the
intrinsics installed as the primary's are), and `os64_js_realm_retire
(runtime, realm)` marks it retired: no task of its own runs again, its
pending jobs are dropped at the next pump, and the context is released
(`JS_FreeContext` is a reference drop; functions made in the realm hold
it, so the context's memory goes when the last of them does, which is
the engine's own rule and needs nothing from us). The primary context
stays the runtime's first realm, so nothing that exists today changes
shape. `os64_js_run` and the timer and event entry points take a realm;
the existing calls pass the primary.

**`this` (the next patch in the series; 0018 at the revision, after the
two 0017s).** `OP_push_this` substitutes `ctx->global_obj` for
an undefined or null `this` in sloppy mode (quickjs.c, `CASE(OP_push_this)`). The patch
adds `JSValue this_substitute` to `JSContext`, initialised to the global
object, read there instead, and set by `JS_SetThisSubstitute(ctx, v)`.
The binding sets it to the window proxy; `JS_EvalThis` already lets a
top-level script's `this` be chosen, and `globalThis` is a plain property
the binding overwrites. The patch is one field, one read, one setter,
with the comment naming the frame design; series mechanics as the
series' latest.

**Budgets and outcomes.** The memory limit, the job queue and the stack
limit stay the runtime's. The task budget stays per run. The sticky
outcomes split: `FAILED_RUNTIME` (out of memory) and `HOST_FAILURE` stay
runtime-wide; `LIMIT` and `CANCELLED` become realm-level when the run was
a realm's task, and runtime-wide when it was the primary's. The
condition for the split is the one § What this settles 8 names: an
interrupted run unwinds through the engine's own exception path and
leaves the heap consistent, which is upstream's design for the interrupt
handler and is what every embedder's timeout relies on.
The libjs owner confirms it on the harness with a case that interrupts a
realm mid-allocation and mid-call and then runs the primary; or the
split is not made and the row says so.

**Promise jobs** are the runtime's one queue and run in order whatever
realm queued them, which is the standard's "one event loop per agent".
A job of a retired realm is dropped by the pump (the realm's mark is
checked before each job runs; the engine's `JS_ExecutePendingJob` reports
the job's context).

## The cluster and the window proxy (libdom)

**The cluster.** `os64_dom_cluster_t`, one per runtime, made by the first
`os64_dom_create` on that runtime and freed after the last binding: it
holds the class registrations (made once; today's refusal of a second
binding becomes a join), the registry from `os64_html_document_t *` to
its binding, and the list of departed documents waiting on their
wrappers. `os64_dom_create` takes the cluster's runtime and a realm and
makes one binding per document, as today's does for the page: its own
`window` (the global), `document`, `location`, `history`, `navigator`,
timers, listeners, requests, storage areas, providers. Everything in
`struct os64_dom` is already per document except the class ids, which is
why the cut is clean.

**A wrapper is made in the realm of its node's document.** `DValue`
already tags the node's document; the lookup becomes "the document's
binding, through the cluster", not "the binding in hand". So when the
parent's script walks `iframe.contentDocument.body.children`, the
wrappers it receives are the child's, with the child's prototypes, and
`child.contentDocument.body instanceof HTMLElement` is false in the
parent and true in the child, as in a browser. The nodes themselves are
one tree per document in one arena per document; adoption across
documents (`adoptNode`, an `appendChild` from another document, which
P1-Db builds) moves the node between arenas by the verb libhtml already
has for a document of its own, and the wrapper's document tag moves with
it.

**The window proxy** is a class with exotic methods (the shape patch 0008's
global-miss hook and storage.c's legacy-platform objects already use):
one instance per browsing context, held by the frame's Page and by the
root, surviving every navigation of that context. Its target is the
current document's binding; it forwards get, set, has, delete,
define and own-keys to that binding's global when the asking realm's
document is same-origin with the target's, and answers the cross-origin
list (§ What this settles 4) otherwise, throwing `SecurityError` through
the binding's DOMException for the rest. The asking realm is
`JS_GetRuntime`'s current context at the call, which the engine passes
to every exotic method. Indexed properties `0..length-1` and the frames'
names resolve to the child contexts' proxies, same-origin or not
(`length`, `frames[i]` and `frames.name` are on the cross-origin list);
the named lookup for everything else is P3c's. `window`, `self`,
`globalThis`, `frames` read through the proxy answer the proxy; `top`
and `parent` answer the ancestors' proxies; `frameElement` answers the
element when the parent's document is same-origin and `null` otherwise;
`opener` is `null`. `document.defaultView` answers the proxy. Every
place today's core.c installs the global as its own `window`, `self`,
`frames` (core.c) installs the proxy instead, and browsing.c's
`context_answer` answers `top`, `parent`, `length` and `frameElement`
for a frame's context as it answers them for a root today.

**`location` across origins** is a second small exotic object: `href`
settable, `replace` callable, nothing readable, everything else
`SecurityError`; the assignment is a navigation ask of that context, from
the asking document, subject to the target's sandbox when it is a `_top`
ask from inside.

**`HTMLIFrameElement`** (frames.c, today `src` alone) gets the rest of its
members: `src`, `srcdoc`, `name`, `sandbox` (a `DOMTokenList`,
the slice 0d shape), `allow` (reflected, no effect), `allowFullscreen`,
`width`, `height`, `referrerPolicy`, `loading`, `contentWindow` (the
proxy), `contentDocument` (the document when same-origin, else `null`),
`getSVGDocument()` (`null`). It stays the modern.c row `d_frames_install` installs, with the kill
switch taking the whole of frames away: with `modern` off an `iframe` is the box it is today, which is
what rule 4 asks (a frame that exists and misbehaves is worse than the
grey box every page already copes with).

**`postMessage(message, targetOrigin, transfer)`** on a proxy: the
message is cloned with the structured-clone writer in the caller's realm
and read in the target's (clone.c's pair, whose header says the record is
no realm's: P2b's `structuredClone` and `window.postMessage` use it with
one realm; this is the pair with two), `transfer` accepts ArrayBuffers
and refuses ports, as message.c does today (a port crossing to another
realm or a worker is WORKERS.md's W3); `targetOrigin` is `*`, `/` (the caller's origin) or a URL
whose origin must equal the target document's origin at delivery time,
or the message is dropped without an error; the delivery is a task in
the target document's queue dispatching a `MessageEvent` at its window
with `data`, `origin` (the caller's serialised origin), `source` (the
caller's proxy), `lastEventId` empty and `ports` empty. P2b built exactly this with the
target the caller (message.c `post_message`: poster and target are two
parameters, the origin check runs in the task, one task per message), so
this slice adds the second realm and nothing else.

**The `storage` event** (P3a's `onstorage`, deferred by 1a) fires at
every same-origin document in the tree other than the one that wrote,
on the write, because the stores are in-process; the cross-window half
stays the file-watch 1a deferred.

## What the open slices do now

Written 10/23 as one sentence a builder could obey, so the frame slice
would land on a tree shaped for it. Re-read 11/07: what each did.

- **0e P1-W — DONE as asked.** `frameElement`, `length`, `top`, `parent`,
  `origin`, `isSecureContext`, `originAgentCluster` are answered by
  browsing.c's `context_answer` from the window's browsing context, none
  installed as a constant; its comment names this section. F1 teaches
  that function a frame's context.
- **0e P2b — DONE as asked.** message.c's `MessageEvent` and message task
  take the poster and the target as two windows; clone.c's record is no
  realm's. F1 passes a second binding.
- **0e P3a — NOT LANDED.** When it is: `window.name` lives on the browsing
  context (the Page's `name`), and `open` with `_self`/`_parent`/`_top`
  is a navigation ask that takes a target context. DEBTS: "`window.name`
  and `Node.baseURI` are absent".
- **0e P3c — NOT LANDED.** When it is: named access on the window
  resolves frames by name first (the standard's order: the child
  contexts, then the tree's named elements), so the global-miss hook
  (patch 0008, `os64_dom_set_global_miss`) takes the context, not the
  global. DEBTS: "Window named access is absent".
- **Slice 3 (modules) — LANDED, map per host.** `struct yonder_modules`
  hangs off the page's script host with the runtime, the import map and
  the record's compile totals. A frame's realm needs a module map of its
  own (the standard's is per realm) and `import.meta.url` of its
  document: F1 makes `yonder_modules` per Page, which it nearly is.
- **RELAYOUT 5b/5c — LANDED as asked.** The cascade and the memo are per
  Page; a frame's box is replaced with no intrinsic size.
- **WIRE.md PR 1 — LANDED.** `Sec-Fetch-*` are sent per request kind
  (agent.c); a frame's navigation is `Sec-Fetch-Dest: iframe` with
  `-Site` from the initiator document and no `-User`, one more kind.
- **WIRE.md PR 2 and the SameSite row — the sentence stands.** A request
  carries its "site for cookies", the top-level page's site, beside its
  URL from the day the SameSite row is paid; until then cookies go by
  destination as today, and frames are why the field exists.
- **Shadow DOM, Range, load events — landed after the list and touched
  it.** Focus records `(node, document)` already; a Range is one
  document's and never crosses; a document's `load` is owed through
  `os64_dom_readiness_after` for the document in hand. None needs a
  change for frames; § What the tree is today says where each helps.
- **Every yonder change from now on.** The Page in hand, never `g.page`,
  and no new file-level static that is really a page's. (516 `g.page`
  reads in yonder.c at the revision; F2 is where they are paid, and no
  slice before it should add one.)

## Workers

WORKERS.md (2026-11-07) gives a page dedicated workers: a thread and a
runtime of its own per worker, messages as packed clones through
mailboxes, fetches relayed through the window. It hangs off this design
at three points, stated here so neither doc bends the other:

- **A worker belongs to a document, and a document is a Page's.** The
  `Worker` constructor runs in a realm; the worker it makes is owned by
  that realm's Page (its origin, its base URL, its CSP when there is one),
  listed on that Page, and terminated when that Page departs — a frame's
  departure ends the frame's workers and leaves the parent's alone. Until
  F2, every Page is a root and the list is the window's.
- **A worker is no realm.** It is a runtime on another thread; nothing in
  § Realms applies to it, and the cluster never sees it. The clone's
  packed form (W1) is what crosses, so a message to a worker is bytes and
  a message to a frame is the record — the same writer, two readers.
- **A port's transfer** to a frame or to a worker is W3 and the same
  slice: message.c's refusal of a port in a transfer list is lifted once,
  for both.

## The oracle

web-platform-tests at the runner's pin (`a699fb16`), served by the runner
at its two sites (a frame's load is a page fetch the runner already
answers; a cross-origin frame is the alternate site). Directories to add
to `tools/wpt_fetch.sh`, each with its REQUIRED set named file by file in
the PR from the directory's contents and read by Fable, and its baseline:

- `html/browsers/origin/cross-origin-objects/` — the cross-origin list,
  property by property, with the exceptions; `cross-origin-objects.html`
  is required whole. This is the test of ruling 4 and the one the
  reviewer reads first.
- `html/browsers/the-window-object/` — the indexed and named frames
  (`window-indexed-properties.html` required), `self-et-al` across
  frames, `frameElement`.
- `html/browsers/windows/` — browsing-context names, nesting, `top`,
  `parent`, the initial about:blank.
- `html/semantics/embedded-content/the-iframe-element/` — `src`,
  `srcdoc`, `sandbox`, `load`, `contentDocument`, the load-event delay.
- `webmessaging/` without ports — `postMessage` origin rules, `source`,
  `data`, `origin`; the with-ports pages excluded by name.
- `html/browsers/browsing-the-web/navigating-across-documents/` — the
  frame navigations; the pages that need `window.open` excluded by name.

Lifted from the existing exclusions when their cut lands: the nine
`css/cssom-view` pages (F2), `dom/nodes`' 488 frame-document failures
(F1 and F2), fetch's `multi-globals` and `response-clone-iframe` (F1),
`webstorage`'s `storage` event pages (F3). The runner's headless window
makes frames the way yonder does, so nothing in the runner changes but
the exclusion lists and the fetch list.

The host harnesses: the DOM host gains a two-document runtime (one
cluster, two realms, two bindings, no layout, no network) and proves the
proxy's forwarding, the cross-origin list with a mutant per property, the
wrapper-per-document rule, `postMessage` across realms with the clone,
and the retired-realm rules (a job of a retired realm never runs; a
departed document's arena outlives its binding while a wrapper lives and
is freed by the last finalizer, under the sanitizers, with a mutant that
frees early). The scripted-page host gains frames: the about:blank at
insertion, `contentDocument.write` into it, `src` loads answered from
the harness's fake wire, the load delay, the depth and count bound, the
server's refusal, each sandbox flag with a mutant, a frame removing
itself from its own handler and from a timer, the layout-after-parent
order with a viewport-driven media query, hit testing through the box,
and the record's folded counts. The replay host replays a kept page with
one kept frame.

## The cut

| Cut | Builds | Oracle | Owner |
|---|---|---|---|
| F1 | realms in libjs (the API, the `this` patch, the outcome split with its proof); the cluster, the binding per document, wrappers per document, the window proxy and the cross-origin list, the location proxy, `HTMLIFrameElement`'s members, `postMessage` across realms | the DOM host's two-document cases; `cross-origin-objects`, `the-window-object`, `webmessaging` without ports; fetch's lifted exclusions | Opus; **Fable reads** the proxy's gate and the realm and wrapper lifetimes before Codex; the libjs owner confirms the interrupt soundness |
| F2 | the Page tree in yonder: birth at insertion, the stream per frame, `src` loads, the server's refusal, the bound, the load delay, departure at task end, layout after the parent, paint clipped, hit testing, scrolling, focus, target names, the record and the kept frame, `MISSING element iframe` retired (the right-click verbs are the menu's, before or after this) | the scripted-page host's frame cases; `the-iframe-element` (less `srcdoc` and `sandbox`), `windows`, `navigating-across-documents`; the nine `cssom-view` pages and `dom/nodes`' frame failures lifted; QEMU `make run-net` loading a page with a same-origin and a cross-origin frame | Opus; **Fable reads** the departure order and the task-end rule before Codex |
| F3 | the stream fed from memory (`srcdoc`, and the `javascript:` string result DECLINED.md names); `sandbox`'s flags; the `storage` event across the tree | `the-iframe-element`'s `srcdoc` and `sandbox` pages; `webstorage`'s event pages | Opus; Chris |

F1 is libjs and libdom and touches no layout; F2 is yonder and needs F1
for a frame that runs script but lays out and paints a scriptless frame
without it, so the two may be built side by side off one branch and land
in order. Each is a PR to `userland` with the three passes, and Codex for
F1 and F2 (a security gate and a lifetime rule are what the budget is
for). Each PR states its WPT counts before and after, the tally's
`iframe` count against the records, the corpus count, and for F2 the P5
on the Tier 1 acceptance pages beside Chrome, by outcome and not by name.
MODERN_WEB.md gains the as-built sections; DEBTS.md's frames row ("No
frames: an `iframe` has no document, and there is no `window.open`") is
struck when F2 lands and the rows § What this settles 9 names are written
with F1.

## Rulings

Both ruled by Chris on 2026-10-23 as leaned, "until I actually use it
and have a chance to regret my decision": the day the P5 says otherwise,
the row reopens and this section says why.

1. **A frame's navigation adds no session entry** (ruling 6). The lean
   is stated there. The alternative is the standard's joint history,
   which costs a design of its own and gives Back the behaviour people
   install extensions to remove.
2. **`window.open` stays a blocked popup** through this design, with the
   status line saying so. The lean is yes; the alternative is a second
   GUI window per `open`, which is the honey-do's "new window before
   tabs" and a design about windows, not frames.

Everything else here is web semantics, decided and told.


## Risks named

- **The proxy on every global access.** Each `window.x` read crosses one
  exotic get and one forward. The DOM host measures the scripted
  fixtures before and after; the budget is a few percent, and if it is
  more, the fix is caching the forward's target per call site, not
  exposing the global.
- **Realm retirement while functions live.** A parent that kept a
  frame's function and calls it after the frame departed runs code
  whose document is gone. The binding's verbs on a departed document
  answer as `createHTMLDocument`'s windowless documents do today (no
  window, no location, no network, no timers), so the call fails
  honestly rather than touching freed memory; the harness carries it.
- **Adoption across arenas.** A node moved between documents is the
  verb libhtml has for documents of its own, now used across Pages that
  are freed at different times; the departed-document rule is what keeps
  the arena alive, and the mutant that frees it early is the proof.
- **The bound.** Depth 8 and 64 frames are chosen for the Tier 1
  acceptance pages and the corpus's heaviest pages (146 `iframe` lines
  across 41 sites, none near either); the constants say why, and the P5
  says whether they are right.
