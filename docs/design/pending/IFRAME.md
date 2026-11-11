# IFRAME.md — a document inside a document: frames, realms and the window proxy

*Tier 1 of the Modern Web arc ([MODERN_WEB.md](MODERN_WEB.md)). Design
by Fable; code reconciliation by Quinn, 2026-11-09, against `userland`
`91ad9413` (through percentage heights, PR #517). This is a design, not
an implementation or an acceptance report. The navigation-history and
popup rulings below stand. The source map distinguishes existing code
from the ownership, realm and host-loop work still required. Historical
corpus counts are dated observations, not a new census.*

*HTTP/2 coordination was checked against Fable's design at `425e6c99`;
H2.0's bell and H2.2's connection cap are dependencies only where named
below, not capabilities claimed for this source baseline. The next arc
is frames, then Workers. Delivery uses short-lived, stackable PRs while
the `userland` checkpoint comparison is held stable (§ The cut).*

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
   global object, intrinsics and prototypes. Sharing one runtime across
   this tree is Yonder's implementation choice, not a requirement that
   HTML puts cross-origin documents in one agent. Documents that can
   synchronously share objects need coordinated execution; QuickJS
   contexts can share objects inside one runtime, but runtimes cannot.
   The runtime stays on its creating thread. Frames get no dedicated
   execution threads; their animation joins the window's rendering loop
   (§ Scheduling and animation). libjs gains realms; libdom
   gains the per-runtime CLUSTER that owns what is shared (the class
   registrations, the document-to-binding registry) and one binding per
   document; a new node wrapper uses its document's realm, whoever first
   asks for it. Adoption preserves an already-created wrapper's identity
   and prototype (§ Retained ownership).
3. **Script sees the window proxy and never the global object.** One
   proxy per browsing context, the same JavaScript object across every
   document that context shows, forwarding to the current document's
   global. `window`, `self`, `globalThis`, `frames`, the `this` of a
   sloppy function and of a top-level script answer that context's proxy;
   `top`, `parent` and `contentWindow` answer the appropriate context's
   proxy. Identity holds across the
   boundary (`parent.frames[0] === window` inside the frame is true, and
   `window.top === window` in a top-level page stays true). The engine
   substitutes the global object for an undefined `this` at
   `OP_push_this`; the proposed engine patch makes that substitute a
   per-context value the binding sets to the proxy. The rest of the
   global/WindowProxy behavior has its own proof in F1.
4. **Cross-origin access is the standard's list and nothing else.** Two
   documents are same-origin when their origins compare equal, exactly
   (scheme, host, port; an opaque origin only to itself). Across origins
   the proxy answers `window`, `self`, `location` (as a location proxy
   whose only verbs are setting `href` and `replace`), `close`,
   `closed`, `focus`, `blur`, `frames`, `length`, `top`, `opener`,
   `parent`, `postMessage`, and indexed child windows. Named child lookup
   is not a cross-origin grant. Implement HTML's WindowProxy and Location
   internal methods, including descriptor/enumeration behavior and the
   specified `then`/symbol fallbacks; a blanket "everything else throws"
   is not that contract (§ The cluster and the window proxy).
   `document.domain` never relaxes it (P1-W's `originAgentCluster` is
   true, the modern default).
5. **A frame lays out AFTER its parent, paints clipped inside its box,
   and is hit through its box.** An `iframe` is a replaced box whose
   size comes from CSS and its attributes and never from its content, so
   the parent's layout finishes without the child, and each frame's
   `flow_layout` then runs as a sibling call with the box's content size
   as its viewport. Nothing nests inside `flow_layout`, which is what
   lets yonder's one-at-a-time layout state stay one at a time. A frame
   whose layout inputs are unchanged can reuse its existing layout.
6. **A frame's navigation adds no session entry.** Back and forward move
   the top-level page only; a frame navigating (its `src` set, a link
   followed inside it, `location` assigned, `pushState`) replaces its
   own current entry and pushes nothing into the joint history; a
   same-document change does not replace the document. The standard's
   joint session history makes Back step through a page's ad rotations,
   which is the web's most-complained-about behaviour, and one person on
   one machine gets the useful half. DIVERGENCES.md row. Ruled (§ Rulings).
7. **A removed frame's backing is not freed under its running script.**
   Removal or navigation updates observable context state at the specified
   point; destructive host cleanup waits until the running task and its
   checkpoint have unwound. The task removing a frame may be its own
   script. A departed document keeps the backing needed by retained
   nodes, functions and native objects (the parent may have kept
   `contentDocument`, a Blob or a
   Canvas context). The existing wrapper registry holds values strongly;
   "free at the last wrapper finalizer" is not an implemented lifetime
   mechanism. F1 must prove the release model in § Retained ownership
   before F2 can retire child documents independently.
8. **An overrun has a runtime-wide fallback.** The script budget stays
   per task, including synchronous calls across realms. The
   [F.3 brief](IFRAME_F3_RETIREMENT.md#budget-and-interrupt-decision)
   selects the conservative fallback: an overrun retires the script tree,
   while ordinary document departure leaves siblings running. Per-child
   timeout recovery remains a separate extension requiring heap/native
   unwind proofs, interrupted-realm attribution and a defined boundary
   for retained calls. F.3 records the fallback in DEBTS/DIVERGENCES when
   implemented; it does not silently clear libjs's sticky failure latch.
9. **Out of this design, each with its row:** `window.open` to a new
   window and `opener` (P3a answers `null`, the blocked popup, and the
   status line says a popup was blocked; a second GUI window is the
   window model's design, not the frame's); `frameset` and `frame` (a
   layout mode of their own); transfer of `MessagePort` (channels and
   ports already exist, but port transfer is Workers W3);
   Content Security Policy beyond `frame-ancestors`; a third-party
   cookie policy (cookies keep today's by-destination rule until the
   SameSite row is paid, and this design records what that row needs
   from frames); `loading=lazy` (eager, which the standard allows).

## What the tree is today

The facts this design moves, checked at `91ad9413`. Paths below are under
`userland/` unless otherwise stated; function names are search anchors.

- **One Page per role.** `g.page` is on screen; `g.coming.page` waits for
  its sheets; `g.stream.page` is arriving; `g.finishing.page` points at
  one of them (yonder.c, `struct Page` and the anonymous `g`).
  `page_of_doc` and `scripts_for` find a page by comparing against
  exactly these slots, and the loop's deadline scans walk the fixed
  array `{g.page.scripts, g.stream.scripts, g.coming.page.scripts}`
  in the deadline/task paths. `g.finishing.page` can alias a Page already
  in a role: tree visitation must not schedule or free it twice.
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
- **Messaging exists for one window.** message.c is P2b's
  `window.postMessage` to the window itself: `post_message` sets
  `poster = target`. The clone (clone.c) is written in the poster's realm and
  read in the target's, the origin check runs at delivery, and the task
  is one per message, oldest first. clone.c's header says the record is
  no realm's. `MessageChannel` and `MessagePort` landed 11/07: a port's
  messages ride the same queue, a port in a transfer list is refused
  (its DEBTS row). F1 extends attribution, delivery and lifetime across
  realms using that machinery. `contentWindow`, `contentDocument`,
  `frameElement` (null), `opener` (null) and `window.open` (absent:
  DEBTS.md's frames row) are where 10/23 left them.
- **Still open from 0e:** P3a (`window.name`, `open` with a target,
  `stop`) and P3c (window named access) have not landed; their DEBTS
  rows stand ("`window.name` and `Node.baseURI` are absent"; "Window
  named access is absent"). The sentences § What the open slices do now
  gives them still bind whoever builds them, before or after F1.

- **Runtime state is more than a context.** `libjs/runtime/runtime.c`
  keeps one context, script host, module loader, rejection registry,
  console state and active outcome. `libdom/core.c`'s `d_context` compares
  the caller with that one context, and `d_callback` uses it too. F1 must
  separate runtime-wide state from realm-specific state and audit the
  binding entry points, not just permit a second `JS_NewContext`.
- **Retirement destroys the engine.** `apps/yonder/scripts.c`'s `retire`
  drops modules, drains the DOM, destroys the runtime and frees or retains
  the binding. `yonder_scripts_depart` calls it. `page_departs` addresses
  the root. None is a child-realm teardown operation as it stands.
- **Extra documents share an arena.** `libdom/implementation.c` makes
  windowless documents in the binding's existing libhtml arena;
  `document_take` changes node-document bookkeeping there. `node.c`'s
  `d_node` rejects a node not owned by `dom->document`. That is not
  adoption between independently allocated frame documents.
- **Wrappers and native bytes have owners.** `DValue` is strongly retained
  by the binding; its sparse fields are in `DValueMore` (`internal.h`).
  `kept.c`'s XHR/Blob bodies retain a `dom` pointer and can be released
  from an engine finalizer. `os64_dom_free` frees those owners under the
  existing engine-before-binding teardown contract. Sharing an engine
  requires a new lifetime proof, not an earlier call to that free.
- **Web Animations A is integrated.** `webanimation.c` supplies the
  document timeline and script effects; `animation.c` samples effects
  before rAF callbacks. `yonder.c`'s `animation_next` and `script_turn`
  address the shown root. Layout reads effects through `page_script_effects`;
  `tree_free` calls `yonder_scripts_tree_gone`. `scripts.c` can retain a
  drained binding while a layout tree still borrows its effects.
- **Memory policy and image ownership are already substantial.**
  `yonder_page_memory_split` supplies tree/cascade/DOM ceilings;
  `MEMORY_CENSUS.md` records owner accounts. Image I2/I3 share source
  selection and display ownership between markup and Canvas. Frames must
  preserve those accounts and providers, with the child viewport used
  for responsive image selection, rather than reintroduce native copies.
- **The context menu exists.** `context_menus_make`/`context_open` provide
  link, picture and download actions. The three frame-specific actions
  described below still need implementation and frame-aware hit results.

## The Page tree (yonder)

**The struct.** `Page` gains `parent` (the Page whose document holds the
frame element; NULL at a root), `frame` (the `iframe` node this Page
fills; NULL at a root), `first_child`/`next_sibling` (its frames, in tree
order), a reference to its stable browsing context, the
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

**Context identity is not a Page address.** Pages are copied and replaced
by the existing navigation paths (`arrive_now`, `stream_finish`). A
browsing context owns its stable WindowProxy and target name, with a
reference to its active Page; replacing the document changes that
reference, not the proxy identity. Retained old documents cannot become
the new document merely because a Page slot was reused. Resource deliveries
carry the owning document/load identity; check it before attaching a late
picture, font, script or response. Preserve the existing serial checks
while extending their lookup to the tree.

**Origin is document state.** Store the committed origin (tuple or opaque
identity), URL, base URL and relevant navigation policy separately.
`about:blank` and `srcdoc` can inherit an origin without having their
parent's URL; sandboxing can make an HTTPS document opaque. URL parsing
alone, as used by several existing callers, cannot answer these cases.
Audit fetch, storage, cookies, messaging, secure-context checks and proxy
access against that state; redirects must not preserve an obsolete
origin. Ancestor security affects secure-context eligibility too.

**Birth: a frame exists the moment its element is in a document.** When
an `iframe` element is inserted into a document that has a browsing
context (the parser's insertion, or a script's `appendChild`), yonder
makes its Page at once with an `about:blank` document (an empty
`html`/`head`/`body` from libhtml, no fetch), whose origin is the
creator document's origin unless sandboxing requires an opaque one, and
whose referrer/base policy comes from its creator. This is synchronous
because pages
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
`src` starts a load when its element is inserted. Attribute changes follow
HTML's processing rules, including reassigning `src`, removing it and
`srcdoc` taking precedence; a changed-string test is not that algorithm.
The load is a navigation through the pool as the
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
  an allowed top-navigation path a `_top` target or `top.location`
  assignment from inside is refused. `allow-top-navigation-by-user-activation`
  requires a real transient user-activation check; it must not grant
  unconditional navigation. Without `allow-popups` nothing changes, since
  popups are blocked anyway. Flags the slice cannot mean (`allow-modals`,
  `allow-pointer-lock`, `allow-presentation`, `allow-orientation-lock`)
  cannot grant an unimplemented capability. Inherited restrictions and
  the element's restrictions determine the new document's active flags;
  changing the attribute does not unsandbox the loaded document. F3's
  implementation checks HTML's flag propagation and navigation rules.
  Before F3, F2 refuses sandboxed frame loads rather than loading their
  content with the restrictions ignored.
- **`srcdoc`** is a document from a string: a stream fed from memory, no
  request, with the parent's origin unless the sandbox makes it opaque,
  and the inherited base URL used for relative references. That mechanism
  is also the cure DECLINED.md names for a `javascript:` link's string
  result, and this is
  where it is built (cut F3).
- **Nested depth and count are bounded**: a frame inside a frame inside a
  frame past depth 8, or a tree past 64 frames, gets an empty document
  and a `FAILED frame too-many` line. Pages that recurse into themselves
  exist; depth and live-count limits supplement allocation budgets and
  are stated in named constants. Count initial blank and loading frames,
  not just completed ones. Retained departed documents remain charged
  even after their active frame slots are released.

**The load event waits for the frames.** A document's `load` fires only
when its own sub-resources and every child frame's `load` have fired or
failed (HTML's "delay the load event"); the frame element fires `load`
after its document's. The condition yonder's `loaded` already computes
gains one clause: no child frame still loading. A frame that is refused,
over the bound, or whose fetch failed counts as finished.

**Departure.** Three doors, one cleanup rule: the Page is marked departing,
and destructive cleanup runs after the current task and its microtask
checkpoint. Observable removal/context changes are not deferred merely
to make reclamation easier; test immediate reads after `iframe.remove()`.
The doors are the element's removal from its document (the mutation
core's removal verbs and the parser's own), the frame's navigation (the
new document replaces the old on arrival, as the window's does), and the
parent's departure, which deactivates its children first, depth-first;
retained backing may outlive the active document tree. Departure drops the
frame's stream and cancels its tasks and requests. It detaches host
providers and releases display resources in their ownership order,
including layout trees' borrowed animation effects. It retains the
backing needed by surviving JS/native objects (§ Retained ownership).
It must not call today's `retire` unchanged: that destroys the shared
runtime. A frame departing while its own script is on the
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
parent, so a frame's frame follows. Reuse RELAYOUT.md's per-Page memo
with the child viewport/environment. A parent change that does not alter
the child's layout inputs need not rebuild the child's layout.

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
from disk through the same reader. The replay grammar also needs the
ownership/generation metadata described in § The oracle. A frame refused
by the server is recorded, not
kept.

**Target names.** A link or form inside a frame with `target=_parent` or
`_top` navigates that ancestor (subject to the sandbox); `_self` and no
target navigate the frame; a name that matches a frame's `name` in the
tree (nearest first, then the whole tree) navigates that frame;
`_blank` and an unknown name do what they do today. `window.open` with
`_self`, `_parent`, `_top` or a frame's name is the same navigation ask
(P3a already leans this way); anything else answers `null`.

**The right-click menu** (YONDER_HONEY_DO.md § 5, built; its verbs are
chosen by what is under the pointer) gains three verbs when the pointer
is over a frame: **Open frame here** (the window navigates to the
frame's current address, the frame's document becoming the page),
**Open frame in new window** (`yonder <address>`, the honey-do's rule),
and **Copy frame address**. Over a link inside a frame the link's verbs
come first and the frame's after. This is the door Chris asked for on
2026-10-23: a way to the page that is IN the frame that is not a click,
because once frames are real a click inside one is the frame's own.
These frame-specific verbs are F2 work on the existing menu. Hit testing
must return the owning Page as well as the node, so a link inside a child
uses the child's URL and policy. An initial blank or `srcdoc` document
has no independent fetchable address: opening its URL alone does not
reproduce its content or inherited security context. Offer address verbs
only when their action is meaningful; do not substitute the parent's URL.

## Scheduling and animation

Frames share the window thread for script, DOM, layout and painting.
Fetching and decoding keep using the existing worker services. No frame
owns a second timer thread or rendering loop. The host selects runnable
documents fairly, preserves task ordering within each source and performs
the shared runtime's microtask checkpoints at the specified boundaries.
A child that keeps posting messages must not starve its siblings or the
next rendering opportunity. A long synchronous call can still delay the
tree; Workers are the separate execution facility, not an iframe flag.

Extend the deadline scan and `script_turn` beyond the fixed root hosts:
include timers, posted messages, script/load deliveries, view steps,
observers, Web Animations, rAF, CSS motion and animated pictures. Take a
stable rendering-opportunity snapshot; callbacks can navigate, remove or
insert frames. New documents do not slip into an already collected batch,
and retired documents are checked again before dispatch. Sample time from
one monotonic clock and translate through the document's timing origin;
do not equate independently navigated documents' `performance.now()` values.
Keep Web Animations sampling before that document's rAF callbacks, and
preserve the existing microtask checkpoints and paint acknowledgement.

Layout/paint and Canvas damage use the owning Page and its viewport,
scroll, clipping and transforms. A dirty child does not require rebuilding
an unchanged parent's layout. Window occlusion and document visibility
need distinct treatment: a CSS-hidden iframe is not automatically a
`document.hidden` transition. Carry the existing hidden-motion policy
through the tree and test hide/show, nested frames and navigation while
an animation callback runs. Pin borrowed effect data until
`yonder_scripts_tree_gone` releases the corresponding layout tree.

## Retained ownership

F1 needs an explicit ownership table before independent realm retirement
is enabled: runtime/cluster; context and global; binding and wrapper
registry; document arena; host providers; and native payloads retained by
JS objects. Name the strong references, the point host activity stops,
and the point each allocation can actually be reclaimed. Today's
engine-before-binding teardown is the baseline to preserve for the root,
not a child-teardown recipe.

The table must cover nodes and listeners, custom-element callbacks,
Range/Selection, Canvas objects, XHR/Blob/ArrayBuffer backing (`kept.c`),
queued clones, and layout's borrowed Web Animation effects. A parent
holding a child-created object must not leave its finalizer pointing into
a freed binding. Keeping the whole retired binding until the runtime
dies is a possible conservative implementation, but then repeated frame
navigation retains memory: account for it, bound it, and state the limit
in the implementation PR. Do not claim last-reference reclamation without
implementing how the strong wrapper registry releases those references.

Cross-arena adoption is additional work in libhtml and libdom. Current
`createHTMLDocument`/DOMParser nodes inhabit the creator's arena, and
`d_node` checks that arena's ownership. F1 must define how a frame's node
can move without copying its JS identity, changing an existing wrapper's
prototype, losing listeners/custom-element state, or freeing its original
backing too soon. Document ownership and wrapper creation realm are
different facts. Prove adoption both ways, destination mutation, and
departure of either document; merely changing `DValue.document` is not
an implementation.

Memory limits apply to the frame tree as well as individual documents.
The shared engine heap remains one account. Establish aggregate admission
for tree, cascade, DOM, pictures, Canvas and kept bodies using the existing
owner counters and census; creating a frame must not multiply the root's
full allowances unchecked. Local limits may remain subdivisions. The F2
brief names the aggregate ceilings and refusal behavior before code, with
tests for many frames and repeated navigation retaining old objects.
Worker budgets remain a separate part of WORKERS.md.

## Realms (libjs)

**The contract change.** CONTRACT.md's "consumers must not create
additional contexts" becomes "consumers create realms through the
library and nothing else": `os64_js_realm_create(runtime, outcome)`
returns a realm handle wrapping a new `JSContext` in the runtime (the
intrinsics installed as the primary's are). The F.3 brief specifies a
retirement operation: no new document-owned task runs, and ineligible
jobs are suppressed before execution. The context and its native backing
remain retained until root teardown, within the cumulative slot and byte
limits in that brief; retirement does not release the context reference.
Direct calls to retained functions remain under the live caller's enclosing
budget and do not reactivate the departed document. The primary context remains available
through the existing single-document API. Realm-aware run, timer and
event entry points extend it; compatibility wrappers select the primary.

**`this` (the next patch in the series; 0018 at the revision, after the
two 0017s).** `OP_push_this` substitutes `ctx->global_obj` for
an undefined or null `this` in sloppy mode (quickjs.c, `CASE(OP_push_this)`). The patch
adds `JSValue this_substitute` to `JSContext`, initialised to the global
object, read there instead, and set by `JS_SetThisSubstitute(ctx, v)`.
The binding sets it to the window proxy; `JS_EvalThis` already lets a
top-level script's `this` be chosen, and `globalThis` is a plain property
the binding overwrites. The patch is one field, one read, one setter,
with the comment naming the frame design; select the patch number from
the series at implementation, not from this design's dated number.
This identifies one engine change, not proof that a one-field patch
implements WindowProxy. Test global declarations, eval, sloppy `this`,
borrowed accessors/methods, descriptors and prototype/extensibility traps;
identify any missing engine hooks before enabling the proxy.

**Context-sensitive services.** The library owns the runtime/context
opaque slots. Its realm API must expose the correct context while
preserving those slots, class-ID synchronization and the single-thread
contract. Module-loader callbacks are installed per runtime but receive a
`JSContext`; route them to that realm's module map and import map. Route
script attribution, rejection reporting, console state and output to the
proper host too. `d_context`, `d_binding` and `d_callback` must distinguish
the calling realm from the object/provider's owning document; replacing
one equality test with "same runtime" does not establish authorization.

**Budgets and outcomes.** The memory limit, job queue and stack limit stay
the runtime's. Cross-realm calls and checkpoints keep the enclosing task's
deadline and job count. F.3 retains runtime-wide sticky failures, including
execution/job limits and cancellation. A timeout in B called from A ends
the shared script runtime; ordinary departure uses the lifecycle API rather
than runtime cancellation. A local native admission refusal is distinct from
an engine allocation failure. `LIMIT` includes memory exhaustion and cannot
be made child-local by testing that status alone. The
[F.3 interrupt decision](IFRAME_F3_RETIREMENT.md#budget-and-interrupt-decision)
defines the audit and the additional proof required before a future split.

**Promise jobs** are the runtime's one queue and run in order whatever
realm queued them, which is the standard's "one event loop per agent".
A job of a retired realm must be filtered before it executes.
`JS_ExecutePendingJob` reports the context of the job it executes; checking
that output afterward is too late. F1 must provide a pre-execution
selection/filter hook or remove that realm's queued jobs safely in the
engine port, with tests for mixed live/retired jobs and their order. A
promise retained by another realm also needs an explicit settlement and
callback-lifetime test. Do not destroy the shared runtime to discard one
document's queue.

## The cluster and the window proxy (libdom)

**The cluster.** `os64_dom_cluster_t`, one per runtime, made by the first
`os64_dom_create` on that runtime and freed after the last binding: it
holds the class registrations (made once; today's refusal of a second
binding becomes a join), the registry from `os64_html_document_t *` to
its binding, and the list of departed documents waiting on their
wrappers. `os64_dom_create` takes the cluster's runtime and a realm and
makes one binding per document, as today's does for the page: its own
`window` (the global), `document`, `location`, `history`, `navigator`,
timers, listeners, requests, storage areas, providers. The fields are
largely per binding already, but class registration, context validation,
host attribution and retained ownership still require the changes above.

**A node's creation realm is fixed before it is wrapped.** Record its
creation realm slot in native metadata, including parser-created nodes that
script has not reached. Resolve that realm's binding through the cluster
when materializing the wrapper, not the node's possibly changed current
document or the binding in hand. So when the
parent's script walks `iframe.contentDocument.body.children`, the
wrappers it receives are the child's, with the child's prototypes, and
`child.contentDocument.body instanceof HTMLElement` is false in the
parent and true in the child, as in a browser. The node's owning document
can change before or after wrapper creation: adoption preserves the node's
creation realm and an existing wrapper's identity and prototype. The
[F.2 brief](IFRAME_F2_OWNERSHIP.md) gives the three-realm, never-wrapped-node
counterexample and proposed native lifetime model. Its arena, document bookkeeping and
lifetime follow § Retained ownership; the current windowless-document
adoption path does not move across arenas.

**The window proxy** is a class with exotic methods (the shape patch 0008's
global-miss hook and storage.c's legacy-platform objects already use):
one instance per stable browsing context, surviving document replacement
within that context. Its target is the
current document's binding; it forwards get, set, has, delete,
define and own-keys to that binding's global when the asking realm's
document is same-origin with the target's, and answers the cross-origin
algorithms (§ What this settles 4) otherwise. The caller is resolved from
the engine's execution context at the operation, with borrowed methods,
getters and callbacks tested across realms. `JS_GetRuntime` returns a
runtime, not a "current context"; the realm bridge must preserve HTML's
current/incumbent settings where each API requires them. Indexed children
are accessible across origins; named lookup follows the same-origin
Window named-access rules and is not another cross-origin grant.
The named lookup for everything else is P3c's. `window`, `self`,
`globalThis`, `frames` read through the proxy answer the proxy; `top`
and `parent` answer the ancestors' proxies; `frameElement` answers the
element when the parent's document is same-origin and `null` otherwise;
`opener` is `null`. `document.defaultView` answers the proxy. Every
place today's core.c installs the global as its own `window`, `self`,
`frames` (core.c) installs the proxy instead, and browsing.c's
`context_answer` answers `top`, `parent`, `length` and `frameElement`
for a frame's context as it answers them for a root today.

**`location` across origins** follows HTML's Location exotic methods:
`href` settable and `replace` callable, with the specified descriptor,
enumeration and fallback behavior; its URL is not readable. The
assignment is a navigation ask of that context, from
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
caller's proxy), `lastEventId` empty and `ports` empty. P2b has the clone
and queued delivery with the target also the caller (`post_message`
sets `poster = target`). F1 must derive the real poster and retain the
right origin, source proxy and receiving document across navigation.
Opaque-origin equality uses the opaque-origin token, not two serialized
`"null"` strings. Test navigation and departure between posting and
delivery; the source does not acquire the target's privileges.

**The `storage` event** is queued for other eligible documents sharing
the storage area; it is not a synchronous callback in the writing task.
Same-origin frames share the top-level context's sessionStorage, while
localStorage follows its origin key. Reuse the window's `yonder_storage_t`
provider and make changes visible coherently before the next reader,
including a synchronous same-origin cross-frame call in the same task.
The existing per-binding maps commit at settle points, so shared
in-process storage needs explicit synchronization, not just event firing.
No-op writes emit no event. The cross-window event half remains deferred.

## What the open slices do now

Source reconciliation at `91ad9413`; these are integration obligations,
not a claim that the remaining changes are one-line extensions.

- **0e P1-W — DONE as asked.** `frameElement`, `length`, `top`, `parent`,
  `origin`, `isSecureContext`, `originAgentCluster` are answered by
  browsing.c's `context_answer` from the window's browsing context, none
  installed as a constant; its comment names this section. F1 teaches
  that function a frame's context.
- **0e P2b — DONE as asked.** message.c's `MessageEvent` and message task
  have separate poster/target roles, presently the same binding;
  clone.c's record is runtime-owned. F1 supplies caller attribution,
  the second binding and their lifetime rules.
- **0e P3a — NOT LANDED.** When it is: `window.name` lives on the browsing
  context's name, and `open` with `_self`/`_parent`/`_top`
  is a navigation ask that takes a stable target context. DEBTS: "`window.name`
  and `Node.baseURI` are absent".
- **0e P3c — NOT LANDED.** When it is: named access on the window
  resolves frames by name first (the standard's order: the child
  contexts, then the tree's named elements), so the global-miss hook
  (patch 0008, `os64_dom_set_global_miss`) takes the context, not the
  global. DEBTS: "Window named access is absent".
- **Slice 3 (modules) — LANDED, map per host.** `struct yonder_modules`
  hangs off the page's script host with the runtime, the import map and
  the record's compile totals. A frame's realm needs a module map of its
  own and `import.meta.url` of its module. The host already owns its map;
  F1 makes the shared runtime's loader select that host by realm, rather
  than letting the last installed loader own all documents' imports.
- **RELAYOUT 5b/5c — LANDED as asked.** The cascade and the memo are per
  Page; a frame's box is replaced with no intrinsic size.
- **WIRE.md PR 1 — LANDED.** `Sec-Fetch-*` are sent per request kind
  (agent.c); a frame's navigation is `Sec-Fetch-Dest: iframe` with
  `-Site` from the initiator document and no `-User`, one more kind.
- **WIRE.md PR 2 and the SameSite row — the sentence stands.** A request
  carries its "site for cookies", the top-level page's site, beside its
  URL from the day the SameSite row is paid. Derivation must include the
  ancestor chain: the top-level site alone is insufficient for a child
  under a cross-site ancestor. Until then cookies follow the existing
  destination policy, with this gap explicitly retained in DEBTS.
- **Shadow DOM, Range, load events — landed after the list and touched
  it.** Focus records `(node, document)` already; a Range is one
  document's and never crosses; a document's `load` is owed through
  `os64_dom_readiness_after` for the document in hand. These are useful
  seams, but host routing, child load blockers, cross-realm wrappers and
  lifetime tests are still frame work.
- **Every yonder change from now on.** The Page in hand, never `g.page`,
  and no new file-level static that is really a page's. Root-specific
  browser chrome may deliberately use `g.page`; document operations must
  receive or resolve their owner. F2 audits both categories.

## HTTP/2 coordination

H2.2 places a shared connection admission queue beneath fetch: HTTP/1
requests for the same pool key queue behind the six-connection limit;
the later H2 transport multiplexes streams under its own limits. A
frame uses the existing fetch/pool path and owns its request lifetime,
not a private connection pool. Queued requests count as pending, and
cancelling a frame removes its queued/in-flight work without cancelling
a sibling's shared connection. Test many same-host frames when H2.2 is
available. F1/F2 do not depend on HTTP/2 protocol support.

If frame or Worker work needs a thread to wait for another, use H2.0's
`os64/bell.h` once landed. Its one-waiter/many-ringers notification wakes
a consumer to inspect its synchronized queue; rings are not payloads or
message counts. Keep payload ownership, cancellation and teardown in the
queue protocol. Do not introduce a new pipe-based wake for that use.
The window's existing GUI/ticker notification path need not be replaced
just to implement frames. WORKERS.md's pipe-backed waits must be reconciled
with the landed bell API when its implementation brief is cut.

## Workers

WORKERS.md (2026-11-07) gives a page dedicated workers: a thread and a
runtime of its own per worker, messages as packed clones through
mailboxes, fetches relayed through the window. It hangs off this design
at three points, stated here so neither doc bends the other:

- **A worker belongs to a document, and a document is a Page's.** The
  `Worker` constructor runs in a realm; the worker it makes is owned by
  that realm's Page (its origin, its base URL, its CSP when there is one),
  listed on that Page, and terminated when that Page departs — a frame's
  departure ends the frame's workers and leaves the parent's alone.
  Carry the document identity, not just the window identity, in the
  worker's fetch relay and queued delivery.
- **A worker has a separate runtime.** Its own realm executes on its
  worker thread; it does not join the frame tree's runtime/cluster. The clone's
  packed form (W1) is what crosses, so a message to a worker is bytes and
  a message to a frame is the record — the same writer, two readers.
- **A port's transfer** to a frame or to a worker is W3 and the same
  slice: message.c's refusal of a port in a transfer list is lifted once,
  for both.

## The oracle

web-platform-tests at the runner's pin (`a699fb16` at this revision).
Use the runner's two origins and extend its fixture serving to child
document requests. Audit redirects, response headers, dynamically served
resources and HTTPS needs per test; a local file mapping is not proof of
those wire behaviors. The directories below are the target set; the
window-object and messaging subsets are already fetched. Add missing
directories to `tools/wpt_fetch.sh`. Each implementation PR names its
REQUIRED files from the pinned checkout, records a fresh baseline and
has Fable read that selection:

- `html/browsers/origin/cross-origin-objects/` — the cross-origin list,
  property by property, with the exceptions. Select the complete security
  cases from the pin; this is the test of ruling 4 that the reviewer
  reads first.
- `html/browsers/the-window-object/` — the indexed and named frames
  and their property descriptors, `self-et-al` across
  frames, `frameElement`.
- `html/browsers/windows/` — browsing-context names, nesting, `top`,
  `parent`, the initial about:blank.
- `html/semantics/embedded-content/the-iframe-element/` — `src`,
  `srcdoc`, `sandbox`, `load`, `contentDocument`, the load-event delay.
- `webmessaging/` without ports — `postMessage` origin rules, `source`,
  `data`, `origin`; the with-ports pages excluded by name.
- `html/browsers/browsing-the-web/navigating-across-documents/` — the
  frame navigations; the pages that need `window.open` excluded by name.

Revisit the frame exclusions in `css/cssom-view` and `dom/nodes`, fetch's
`multi-globals` and `response-clone-iframe`, and `webstorage` event pages.
The old counts (nine view pages, 488 node failures) are historical, not
this refresh's measurements. A two-realm DOM harness can test F1 without
visible frames; WPT pages that create real iframe elements also need the
host integration from F2. Do not claim those passes at F1 merely because
the binding can construct a second realm.

The WPT, scripted and replay hosts include Yonder's loop, but their fake
wire, completion conditions and root-only observations still need frame
coverage. Record child URL, parent/document identity and load generation
in retained resources, so two frames using the same URL or navigating
repeatedly replay unambiguously. Update record grammar/parsers, fixtures
and backward compatibility together; this is more than an exclusion edit.

The host harnesses: the DOM host gains a two-document runtime (one
cluster, two realms, two bindings, no layout, no network) and proves the
proxy's forwarding, the cross-origin list with a mutant per property, the
wrapper-per-document rule, `postMessage` across realms with the clone,
and the retired-realm rules (a job of a retired realm never runs; the
ownership model retains backing while reachable objects need it and
reclaims it at its documented boundary). Exercise retained Blob bytes,
Canvas objects, functions and adopted nodes, not just `contentDocument`.
Use ASan/UBSan with leak checking enabled, including the supported
outside-sandbox run if sandbox attachment is denied. The scripted-page
host gains frames: the about:blank at
insertion, `contentDocument.write` into it, `src` loads answered from
the harness's fake wire, the load delay, the depth and count bound, the
server's refusal, each sandbox flag with a mutant, a frame removing
itself from its own handler and from a timer, the layout-after-parent
order with a viewport-driven media query, hit testing through the box,
and the record's folded counts. Add simultaneous parent/child animation,
child-only damage, stale network arrivals, aggregate-memory refusal,
repeated frame creation/removal, and fairness under a busy sibling. The
replay host covers nested/repeated-URL frames and navigation, including
an old corpus record with no frame resources.

## The cut

The mergeable slice map and status live in [IFRAME_TRACKER.csv](IFRAME_TRACKER.csv).
[IFRAME_TRACKER.md](IFRAME_TRACKER.md) defines its upkeep and the proposed
14-slice breakdown of the stages below. Detailed tasks and proofs belong
in each slice's implementation brief.

| Cut | Builds | Oracle | Owner |
|---|---|---|---|
| F1 | realms and realm-aware services in libjs; pre-execution job filtering; the cluster, cross-arena ownership/adoption, context validation, WindowProxy and Location security, cross-realm messaging; independent retirement only after its proof | two-document host cases, retained-object and adoption tests, interrupt/failure injection; WPT subsets runnable without the F2 host | Builder assigned per PR; **Fable reads** security and lifetimes; libjs owner confirms interrupt soundness; Codex review |
| F2 | stable browsing contexts and Page trees; per-frame navigation and response policy; shared scheduling/animation; aggregate admission; layout, paint, input, menus, diagnostics/replay; refuse sandboxed and unsupported srcdoc loads until F3 | host cases and newly runnable frame WPT files; mixed-origin/nested frames, animation and navigation in QEMU and on the P5 | Builder assigned per PR; **Fable reads** departure and scheduling; Codex review |
| F3 | the stream fed from memory (`srcdoc`, and the `javascript:` string result DECLINED.md names); `sandbox`'s flags; the `storage` event across the tree | `the-iframe-element`'s `srcdoc` and `sandbox` pages; `webstorage`'s event pages | Opus; Chris |

F1/F2/F3 are dependency stages, not three mandatory large branches. F1
also touches libhtml ownership and the script host, even before F2 changes
visible frame layout. Suggested reviewable cuts are:

1. Realm API and runtime-service routing, preserving the existing
   single-document entry points and behavior.
2. Binding/arena ownership, cross-document adoption and retirement proofs
   in a two-document harness.
3. WindowProxy/Location gates and cross-realm messaging; enable them only
   with the complete security and lifetime contract.
4. Host Page/context routing, then visible frame loading, rendering/input
   and replay in coherent increments. No shipped intermediate step may
   run a frame while silently ignoring its sandbox or framing policy.
5. F3's srcdoc, sandbox and coherent storage/event behavior.

Each cut is a short-lived PR intended for `userland`; a preparatory cut
can land without exposing incomplete frames. While the checkpoint's
comparison is frozen, leave `userland` and the comparison refs unchanged:
stack a dependent branch/PR on its predecessor. After the checkpoint,
land in dependency order and retarget the remaining PRs to `userland`,
checking their diffs and inherited claims after each merge forward.
No long-lived iframe integration branch is required. The builder writes
the concrete ownership/budget decisions and test set before its cut;
this refresh does not claim those outstanding proofs are completed.

Each PR states its applicable WPT counts before and after, the tally's
`iframe` count against the records, the corpus count, and for F2 the P5
on the Tier 1 acceptance pages beside Chrome, by outcome and not by name.
MODERN_WEB.md gains the as-built sections; DEBTS.md's frames row ("No
frames: an `iframe` has no document, and there is no `window.open`") is
split when F2 lands: embedded documents can be paid without marking
`window.open`, deferred sandbox/srcdoc behavior or joint session history
as implemented. Keep the remaining rows and explicit divergences visible.

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

The 2026-11-09 workflow ruling is short-lived, independently mergeable
cuts, stacked while the checkpoint comparison is frozen. Ownership and
aggregate-budget details called out above require implementation briefs
and proof; they are not silently settled by this document's older claims.


## Risks named

- **The proxy on every global access.** Each `window.x` read crosses one
  exotic get and one forward. The DOM host measures the scripted
  fixtures before and after; the budget is a few percent, and if it is
  more, the fix is caching the forward's target per call site, not
  exposing the global.
- **Realm retirement while objects live.** A retained function can enter
  its creation realm after its frame leaves. Retain valid backing and
  define which host activity is disabled; existing windowless documents
  are not a substitute for this lifecycle. Tests must distinguish memory
  safety, continued access to detached objects, and any deliberate
  departure-policy divergence.
- **Adoption across arenas.** The existing same-arena operation is not
  the required implementation. F1's ownership proof is a dependency of
  shipping frame documents that can exchange nodes.
- **The bound.** Depth 8 and 64 frames are chosen for the Tier 1
  acceptance pages using the original census (146 `iframe` lines across
  41 sites). Recheck those assumptions in F2; active frame count alone
  does not bound memory retained by departed documents.

## Source references for the implementation briefs

- [HTML agents and agent clusters](https://html.spec.whatwg.org/multipage/webappapis.html#agents-and-agent-clusters)
- [HTML WindowProxy and cross-origin operations](https://html.spec.whatwg.org/multipage/nav-history-apis.html#crossoriginproperties-(-o-))
- [HTML iframe processing and sandbox](https://html.spec.whatwg.org/multipage/iframe-embed-object.html#the-iframe-element)
- [QuickJS runtime/context and class contracts](https://bellard.org/quickjs/quickjs.html#Runtime-and-contexts)

Standards describe observable behavior; the source map above describes
what Yonder has built. A proposed API or a listed oracle is not evidence
that its behavior already exists or passes.
