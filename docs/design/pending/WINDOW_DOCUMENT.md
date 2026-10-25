# WINDOW_DOCUMENT.md — what `window` and `document` are missing, and the order to add it

*Slice 0e of the Modern Web arc ([MODERN_WEB.md](MODERN_WEB.md)): an
inventory, not an implementation. Written by Opus on 2026-10-23 at
`userland` 47e960c1 (slices 0a, 0b, 0c, 0d, 1a, 1b and 4 merged; slice 2 in
review and not counted). Nothing under `userland/` changed for it. It ships
with the first phase it plans. Every number below names its source; § Sources
gives each source's date, and § What could not be determined lists the gaps.*

## Summary

The spec IDL (webref, 2026-10-23) puts **242 instance members on `Window`**
and **235 on `Document`** that at least one shipping engine exposes (BCD
8.1.7). Each count includes the inherited EventTarget methods and every
`on<type>` handler. Event handlers are 128 of Window's members and 111 of
Document's. Another 10 Window and 13 Document members are in the IDL but
ship nowhere; they are listed in Appendix C and left out of every count.

A probe of the live binding (§ How the binding was measured) finds:

| | members | present today | classic | modern (switch on only) | Chrome-weighted | corpus-weighted |
|---|---:|---:|---:|---:|---:|---:|
| Window | 242 | **64 (26.4%)** | 38 | 26 | **52.1%** of 91 measured | **70.4%** |
| Document | 235 | **58 (24.7%)** | 54 | 4 | **24.9%** of 45 measured | **67.8%** |

There are two weightings because no one public source measures every member
(§ Usage, and what it can and cannot say):

- **Chrome-weighted** weights each member by Chrome's use counter: the share
  of page loads that touched it on 2026-10-21. Only 91 Window and 45 Document
  members have a counter or a named proxy. The common Document reads have
  none (`title`, `readyState`, `referrer`, `activeElement`, `currentScript`).
  So for Document this measure mostly reflects what Chrome happens to
  measure, which is the niche members.
- **Corpus-weighted** weights each member by how many of our 839 kept pages
  (163 sites) mention it in their own bytes, found by a static scan. It
  covers every member. It is our pages and not the web. The 839 are the
  three pulls of 2026-10-21, -22 and -22b together; the baseline pull,
  -22b, is one of the three (§ Usage, 4).

**The owner's target** is at least 50% of what pages regularly use, and
preferably 75%. Window already passes 50% on both measures, and Document
passes it on the corpus measure. Each interface reaches 75% on both measures
as follows:

- **Window:** P1-W, the low-hanging fruit, reaches 60.7% Chrome-weighted and
  76.2% corpus-weighted. P3a, the page-lifecycle events, `name` and `open`,
  reaches 76.7% / 84.7%.
- **Document:** P1-Da, the document-state reads, reaches 90.5%
  corpus-weighted on its own. The Chrome measure passes 50% at P1-Db (54.3%)
  and passes 75% only at P5 (77.8%). That jump at P5 comes from one
  feature-level number for `scrollend`.

The full table is in § Coverage by phase.

What blocks pages today is mostly not exotic, and the corpus says so plainly:

- `document.readyState` is absent. Every `if (document.readyState ===
  'loading')` gate takes the wrong branch and runs its "DOM is ready" code
  during the parse. 378 of 839 kept pages read it.
- `document.title`, `document.referrer`, `document.currentScript`,
  `document.activeElement` and `document.visibilityState` are absent too
  (229, 316, 271, 298 and 378 pages).
- `window.origin`, `isSecureContext`, `queueMicrotask` and `crypto` are
  absent (`crypto` is 49.2% of Chrome page loads by `getRandomValues` alone).
- `window.opener` is absent: 143 MISSING asks in 20 of the 198 page files
  the census tallied in the baseline pull's replay (2026-10-22b, at Slice
  0d).

Every one of these is low-hanging fruit: the state is already in the binding
or in yonder.

**The plan, in one line each** (§ The phase plan has each phase in full):

| phase | what | members | Window C / scan after | Document C / scan after |
|---|---|---:|---|---|
| P1-W | window's low-hanging fruit | 27 | 60.7% / 76.2% | — |
| P1-Da | document state reads | 25 | — | 42.1% / 90.5% |
| P1-Db | document collections and namespaced verbs | 10 | — | 54.3% / 92.8% |
| P2a | errors: `onerror`, rejections, `reportError` | 5 | 60.7% / 78.3% | 54.3% / 92.8% |
| P2b | `structuredClone`, same-window `postMessage`, `requestIdleCallback` | 6 | 66.8% / 82.4% | — |
| P3a | page lifecycle, `name`, `open`, `stop`, `storage` event | 8 | 76.7% / 84.7% | — |
| P3b | the window's screen position and `visualViewport` | 5 | 80.8% / 85.9% | — |
| P3c | legacy named access and `document.all` | 3 | 80.8% / 85.9% | 64.6% / 94.0% |
| P4a | TreeWalker, NodeIterator, `moveBefore`, CDATA and PIs | 5 (+1 static) | — | 64.7% / 94.7% |
| P4b | hit testing: `elementFromPoint` and kin | 3 | — | 70.4% / 95.2% |
| P5 | input and form events yonder does not fire | 20 | 82.1% / 86.0% | 77.8% / 95.2% |
| P6a–d | `trustedTypes`, `document.fonts`, `cookieStore`, `scheduler` | 4 | 86.0% / 88.5% | 86.5% / 95.6% |
| slices 2, 5d | `fetch`, `requestAnimationFrame`, `cancelAnimationFrame` | 3 | 92.6% / 94.7% | — |

What stays out after all of that is 78 Window and 98 Document members that
need a subsystem no slice plans (class d), `print()` (class a, in no phase:
§ Classification), plus 30 and 20 that are deliberately skipped (class e). The largest groups are Selection and Range,
editing, XPath, CSSOM style sheets, pointer and drag events, and media. Most
of the rest are event handlers, for the reason below.

**The handler hazard, which binds every phase.** A handler's existence is
how pages detect a feature. `'ontouchstart' in window` reads as "this is a
phone"; `'onpointerdown' in window` makes pointer-event libraries stop
listening for mouse events; `ontransitionend` makes a transition helper
wait for an event yonder never fires. So adding a handler before the
events it announces actively harms the pages that feature-detect, and **an
`on<type>` handler joins only with the events it names, in the phase that
makes yonder fire them**. That is rule 1 (whole API or none) at the
granularity of a single property. § The handler hazard has it in full.
P1-W's three handlers (`ononline`, `onoffline`, `onlanguagechange`) keep
the rule rather than break it: their events are owed only when
`navigator.onLine` or the person's languages change, and yonder has
neither to change.

## How the binding was measured

**The probe.** The probe ran in a scratch copy of the tree, not in any
tracked file. It added one case to `tools/test_dom_host.c` and was built by
`tools/test_dom_host.sh` (both of the script's profiles, plain and
sanitized). It makes a binding as `os64_dom_default_options()` makes one,
with the page at an `https:` address. It runs the script in Appendix E,
which walks the prototype chains of `window`, `document`, `location`,
`history` and `navigator` down to `Object.prototype`. For every own
property it records the level, whether it is data or an accessor, and the
type and value it reads. It ran twice, once with `modern` on and once with
it off.

- A member present both times is **classic**; one present only with the
  switch on is **modern**.
- The switch-on/switch-off difference is exactly modern.c's rows: the
  interface objects; the 1a, 1b and 0c rows; `document.scrollingElement`,
  `defaultView`, `onscroll` and `onresize`; and history's `pushState`,
  `replaceState`, `state` and `scrollRestoration`.

**What yonder adds beyond libdom.** `userland/apps/yonder/scripts.c` sets
the binding's options and providers: view, computed style, history,
geometry, user agent, cookies, storage and the global-miss hook. It installs
one global of its own: `console`, through libjs's
`os64_js_install_output`, which has 19 methods. Nothing else in yonder
defines a property on the global. The providers change what members
*answer* (the DOM host's view reads zeros, yonder's reads the window), not
which members *exist*. So the probe's member list is yonder's member list,
plus `console`.

**Stubs and partial members.** These were flagged from the probe's values
and the source:

- **Stubs** (they answer, but not as the standard does):
  - `window.status` reads `''` whatever was assigned; HTML keeps the last
    string set.
  - `confirm()` answers `false` without asking.
  - `prompt()` answers `null` without asking.
- **Constants that are correct for a window with no frames:**
  - `length` is 0; `frames`, `parent` and `top` are the window.
  - `focus()` and `blur()` do nothing, which HTML allows for a top-level
    window.
  - `document.close()`, `captureEvents()` and `releaseEvents()` do nothing,
    as HTML defines them.
- **Partial members:**
  - `document.body` has no setter.
  - `window.window`, `window.document` and `window.history` are writable
    data properties. A page can assign over them, where HTML makes the
    first two `[LegacyUnforgeable]` and the third a read-only accessor.
  - The global object has no `Window.prototype`, `WindowProperties` or
    `EventTarget.prototype` in its chain. So `window instanceof Window`
    throws: the corpus records `'Window' is not defined` in 3 page files.
- **Navigator** is outside this inventory, but the probe saw it carry only
  `userAgent`, `appName`, `appVersion` and `platform`.

**Node.** Document inherits Node's members, which are not counted above
because they are every node's. The probe sees 20 of Node's 29 present on
document. Absent: `baseURI`, `compareDocumentPosition`, `getRootNode`,
`isDefaultNamespace`, `isEqualNode`, `isSameNode`, `lookupNamespaceURI`,
`lookupPrefix` and `normalize`. All nine are pure algorithms over the tree
and belong with P4a, or with a Node inventory of their own.

## Usage, and what it can and cannot say

Each member carries up to four numbers. Appendices A and B have the columns.

1. **Chrome %**, from Chrome's use counters (chromestatus.com
   `/data/featurepopularity`, the day 2026-10-21). This is the share of
   Chrome page loads that touched the counter. Where Blink's generated
   counter exists (`V8Window_Opener_AttributeGetter`) it is the member's own.
   Hand-mapped counters are named in the note column:
   - `CookieGet` for `cookie`
   - `DocumentAll` for `all`
   - `Fetch` for `fetch`
   - `WindowPostMessage` for `postMessage`

   Some are **proxies**, also named in the notes. A proxy is a lower bound
   unless the note says otherwise:
   - `DOMStorageRead` counts any read of Web Storage, so it stands for both
     `localStorage` and `sessionStorage`.
   - `CryptoGetRandomValues` stands for `crypto`.
   - `UnprefixedRequestAnimationFrame` stands for `requestAnimationFrame`.
   - `SelectionAnchorNode` stands for `getSelection`.

   The four page-lifecycle handler rows carry Chrome's "a listener was
   registered by any means" counters, which already count in yonder, since
   `addEventListener` works. Those rows are weighted by them, but what their
   phase actually adds is firing the events.
2. **webstatus.dev**, marked **W**. This is Chrome usage per web-features
   *feature* (web-features 3.44.0 maps BCD keys to features), as the API
   served it on 2026-10-23. It is used only for the 17 members (19 rows, since
   `onscrollend` and `when` are on both interfaces) that have no counter and
   *are* their feature (`moveBefore`, `startViewTransition`,
   `onscrollend` and the like). Handler-to-feature mappings that pointed
   elsewhere (`onabort` to AbortController's feature) were rejected.
3. **HTTP Archive** was the brief's second choice. Its per-API usage lives in
   BigQuery tables (`httparchive.*`), which this session cannot query. The
   Web Almanac publishes only selected APIs, chapter by chapter, and none of
   them per member of Window or Document. **No HTTP Archive number is used.**
4. **Our corpus**, in two instruments, over two different sets of pulls.
   The corpus is pulled from the P5 as it grows, and the baseline a slice is
   judged against is one pull, 2026-10-22b, the third. Each count below
   names its pulls, so a later pull adds to the corpus without moving any
   number here:
   - **MISSING files/asks** (Window only) is the census tally over the
     baseline pull alone, replayed at Slice 0d: `python3 -I
     tools/yonder_tally.py --kind global --top 400
     ~/os64-corpus/replays/2026-10-22b_0d-after`. It counted 198 page files
     from 86 sites, and set aside 32 incomplete, 9 unfinished and 19
     unjudged records. The global-miss hook hears only lookups on `window`,
     so a missing *document* member is never a MISSING line.
   - **Scan pages** is a static scan of the 839 kept pages (163 sites) of
     all three pulls, 2026-10-21, -22 and -22b, together. Each page is its
     `.html` and every script kept beside it. A Document member counts when
     the bytes say `document.X` or `document['X']`. A Window member counts
     on `window.X`, `self.X` or `globalThis.X`, or as a bare identifier not
     preceded by `.` and not used as an object key. Bare identifiers are
     skipped for about 150 names too generic to trust (`name`, `open`,
     `close`, `status`, `top`, `print`, `focus`, most `on<type>` names); the
     scan script lists them all.

     The scan undercounts aliased access (`var d=document; d.title`) and
     overcounts a page's own function that happens to share a global's name.
     It is a rank, not a census: the Almanac's own capability counts are
     regexes over source, with the same caveat.

Neither source alone ranks everything, so the plan below is ordered by both.
The coverage tables report each weighting on its own and never blend them.

## Classification

Every shipped member yonder lacks falls in one class. Appendices A and B give
each member's class, phase and the reason.

| class | meaning | Window | Document |
|---|---|---:|---:|
| present | classic or modern today | 64 | 58 |
| (a) low-hanging fruit | reads or forwards state the binding or yonder already has, or a small self-contained algorithm | 28 | 35 |
| (b) medium | a new piece of state, a host hook or a new interface, no new subsystem | 37 | 22 |
| (c) planned slice | slice 2 (`fetch`), slice 5d (`requestAnimationFrame`, `cancelAnimationFrame`), Tier 1 (custom elements, CSP's `onsecuritypolicyviolation`) | 5 | 2 |
| (d) new subsystem | needs something no slice plans | 78 | 98 |

**`print()` is (a), in no phase.** HTML § 8.9.2's printing steps begin:
"The user agent may display a message to the user or return (or both).
For instance, a kiosk browser could silently ignore any invocations of the
print() method." A `print()` that returns there is conformant, and since it
returns before the steps that fire them, `beforeprint` and `afterprint`
never fire. It was classed (d) when the phases were counted, so no phase
holds it and the coverage tables leave it out (Window's are 0.01 points
C and 0.14 scan short of what it adds). Its two handlers stay (d),
printing: they announce a print path that a no-op does not have.
| (e) skip | obsolete, Chromium-only, harmful to expose, or meaningless for one user on one machine | 30 | 20 |

**The handler hazard.** Most (d) rows are `on<type>` handlers, and the reason
is the same for all of them. A handler's existence is how pages detect a
feature:

- Modernizr's drag-and-drop test reads `'ondragstart' in div`.
- Pointer-event libraries read `'onpointerdown' in window` and then stop
  listening for mouse events.
- Touch detection reads `'ontouchstart' in window`, which 204 of our 839
  kept pages mention. Desktop Chrome hides it without a touch screen, which
  is why it is (e) and not (d).
- Transition helpers wait for `transitionend` when `ontransitionend` exists.

So a handler joins only with the events it names, in the phase that makes
yonder fire them. That is rule 1 (whole API or none) at the granularity of a
single property.

**Rule 1 for the new interfaces (P1-W).** P1-W introduces three small
interfaces: `Crypto`, `BarProp` and `External`. `BarProp` and `External` are
whole as written. `Crypto` without `subtle` is what browsers show an
*insecure* context (`subtle` is `[SecureContext]`). On an `https:` page it
would be half an object.

- **The lean:** ship `getRandomValues` and `randomUUID` and leave `subtle`
  absent. Pages detect `crypto.subtle` separately, and 49.2% of Chrome page
  loads call `getRandomValues` while `SubtleCryptoDigest`, the most common
  subtle call, is 12.2%.
- **Ruled for the lean** (§ Rulings this needs).

## The phase plan

Each phase is one reviewable PR unless it says otherwise. Each adds rows to
modern.c's table, a row's installer putting members on window or on the
document's prototype as slices 1a and 1b did. So the kill switch takes the
whole phase away and the DOM host's name check (`tools/test_dom_modern.inc`)
holds it. The exceptions are corrections to members that already exist (the
`status` stub, the unforgeable `window` and `document`, `document.body`'s
setter), which are classic fixes and say so.

Each phase names its WPT oracle at the runner's pin (`a699fb16`). Most
directories are not in `tools/wpt_fetch.sh` yet, and the phase adds them.
Tests that need frames, `test_driver` input or a second window are excluded
by name with that reason, as slice 1b excluded 9 frame pages.

"C" and "scan" below are the coverage each interface reaches when the phase
lands (§ Coverage by phase). Each member list is ordered by usage, with the
Chrome % first, then scan pages, then (W) MISSING files.

### P1-W — window's low-hanging fruit (27 members)

Window to 60.7% C / 76.2% scan.

- `opener`: Chrome 51.6%, 19 pages, 20 files and 143 asks. It reads `null`,
  since no yonder window is opened by script, and is settable as HTML
  defines it.
- `crypto` with `getRandomValues` and `randomUUID`: Chrome 49.2%, 210 pages,
  34 files. The state needed is entropy, through a host hook over
  `/dev/random`, which is what libtls's `platform_inputs.c` reads.
- The six BarProps (`locationbar`, `toolbar`, `statusbar`, `menubar`,
  `personalbar`, `scrollbars`): Chrome 13.4% down to 4.8%. Each is
  `{visible: true}`.
- `closed` (false) and `close()` (a no-op: HTML lets a script close a
  top-level window whose session history holds one entry, and yonder lets
  no page close the person's window; DIVERGENCES.md).
- `clientInformation`: the navigator object, again.
- `moveTo`, `moveBy`, `resizeTo` and `resizeBy`: no-ops (CSSOM View). Only a
  script-opened window may be moved or sized.
- `captureEvents` and `releaseEvents`: no-ops, as on document.
- `queueMicrotask`: 329 pages, 18 files. It runs on the engine's job queue,
  which the binding's promise pump already drains.
- `isSecureContext`: 241 pages. It is true for a potentially trustworthy
  origin, read from the page's address.
- `frameElement`: 101 pages, 13 files. It is `null`.
- `origin`: 58 pages, 56 files. It is the serialisation of `d_origin`, which
  storage already keys on.
- `external`: 55 pages. `AddSearchProvider` and `IsSearchProviderInstalled`
  do nothing, as HTML's obsolete section defines them.
- `crossOriginIsolated` (false) and `originAgentCluster` (true).
- `ononline`, `onoffline` and `onlanguagechange`: handler slots for events
  that never fire on one machine. They are harmless to detect.
- Classic fixes in the same PR:
  - `status` keeps the last string set.
  - `window` and `document` become unforgeable.
  - `history` becomes a read-only accessor.

Oracle:

- Add `html/browsers/the-window-object`: `BarProp.window.js`,
  `closed-attribute.window.js`, `close-method.window.js`,
  `self-et-al.window.js` and `window-properties.https.html`. Its `open-close`
  and `named-access-*` pages wait for P3.
- Add `html/dom/self-origin.any.js`, `secure-contexts`,
  `html/webappapis/microtask-queuing`, and `WebCryptoAPI`
  (`getRandomValues.any.js` and `randomUUID.https.any.js` only, the rest
  excluded by name as `subtle`).
- `css/cssom-view/resizeTo-negative.html` is already fetched.

### P1-Da — document state reads (25 members)

Document to 42.1% C / 90.5% scan. Each line names the state the member reads.

- `readyState` and `onreadystatechange`: 378 and 113 pages.
  `loading`/`interactive`/`complete`, moved by the host's own
  `DOMContentLoaded` and `load` dispatches, heard or not, with
  `readystatechange` fired just before each.
- `visibilityState`, `hidden` and `onvisibilitychange`: 378, 233 and 7
  pages. They read `visible` and false.
  - A desktop minimize as `hidden`, and its event, is a later host hook.
- `referrer`: 316 pages. It is the trip's referrer (`trip.c`, libway's
  `way_referrer`), handed in through the options.
- `activeElement` and `hasFocus()`: 298 and 78 pages. They read yonder's
  `g.focus_node` through a host hook; the body when nothing has focus.
- `currentScript`: 271 pages. It is the script element yonder is running,
  set by `scripts.c` around `os64_js_run` and the microtasks the script
  queued, which HTML drains before putting it back.
- `title`: 229 pages. It is the first `title` element, the rule yonder's
  `title_of` already walks.
  - The setter makes one in `head`.
  - The window's own title follows it (Chris's ruling), through a GUI
    verb that renames a window and yonder's call to it, which are their
    own branch and PR (`opus/gui-window-title`), not P1-Da's: until it
    lands yonder names its window once (DEBTS.md).
- `prerendering` (false) and `onprerenderingchange`: Chrome 61.4%, 105
  pages.
- `domain`: Chrome 0.8% (the setter), 158 pages. It reads the origin's
  host. The setter runs its checks and then has nothing to relax, since
  there are no frames.
- `URL` and `documentURI`: 91 pages. `options.url`, or `about:blank` for a
  document of its own.
- `wasDiscarded` (false), and `compatMode` (libhtml's quirks mode, which
  `scrollingElement` already reads).
- `dir`, reflected from the root element.
- `characterSet`, `charset` and `inputEncoding`: libhtml's charset, under its
  Encoding Standard name.
- `contentType`: `text/html`, or the type `implementation.c` gave a
  document of its own.
- `lastModified`: the response's `Last-Modified`, else now.
- `rootElement` and `clear()`.
- Classic fix in the same PR: `document.body`'s setter.

Oracle:

- Add `html/dom/documents`: `dom-tree-accessors` for the `document.title-*`
  and `Document.currentScript` pages, and `resource-metadata-management`
  for `compatmode`, `lastModified` and `readyState`.
- Add `page-visibility` and `html/interaction/focus`.
- `dom/nodes`, already fetched: `Document-URL`,
  `Document-characterSet-normalization-*` and `Document-contentType/`.

### P1-Db — document collections and namespaced verbs (10 members)

Document to 54.3% C / 92.8% scan.

- `scripts` (Chrome 40.7%, 127 pages), `links`, `anchors`, `embeds`,
  `plugins` and `applets`. These are live HTMLCollections over the tree, on
  the machinery `images` and `forms` already use; `applets` is always empty.
- `createElementNS` (99 pages) and `getElementsByTagNameNS`. They use
  `implementation.c`'s validate-and-extract and its three namespaces.
- `importNode` (50 pages) and `adoptNode`. A document of its own already
  shares the arena and adopts on append.

Oracle:

- `html/dom/documents/dom-tree-accessors`: `document.scripts`,
  `document.links` and `document.embeds-document.plugins-01`.
- `dom/nodes`: `Document-createElementNS`, `Document-getElementsByTagNameNS`,
  `Document-importNode` and `Document-adoptNode`. These lift some of the 271
  `dom/nodes` failures slice 0a counted on `createElementNS`.

### P2a — errors (5 members)

Window to 60.7% C / 78.3% scan.

- `window.onerror` (160 pages, 15 files): an ErrorEvent at window for an
  uncaught exception, which the script host already knows, since it writes
  the record's `FAILED script` line.
- `onunhandledrejection` (110 pages, 14 files) and `onrejectionhandled`:
  PromiseRejectionEvent from the engine's rejection tracker
  (`JS_SetHostPromiseRejectionTracker`).
- `reportError` (114 pages, 10 files) goes through the same path.
- `document.onerror` is the handler slot on document.
- New interfaces: `ErrorEvent` and `PromiseRejectionEvent`.

Oracle: add `html/webappapis/scripting`, which holds
`processing-model-2/` (`runtime-error*`, `compile-error*` and
`unhandled-promise-rejections/`) and `reporterror.any.js`.

### P2b — clone, message, idle (6 members)

Window to 66.8% C / 82.4% scan.

- `requestIdleCallback` (Chrome 54.2%, 343 pages, 15 files) and
  `cancelIdleCallback`, with `IdleDeadline`. The callback runs when yonder's
  loop is idle, with a deadline from the host clock.
- `postMessage` (52.1%, 194 pages, 26 files), `onmessage` and
  `onmessageerror`: same-window only, a `MessageEvent` task carrying a
  structured clone. Its common use is the `setImmediate` trick. Messages to
  another window are (d).
- `structuredClone` (21.9%, 149 pages, 14 files), with `DataCloneError`
  for what HTML does not serialize and transfer of ArrayBuffers. The plan
  named the engine's object writer and reader, which `performance.c` used
  for plain data; as built it is the binding's own StructuredSerialize
  pair, which performance's `detail` now uses too (MODERN_WEB.md § P2b,
  as built, says why).

Oracle: add `html/webappapis/structured-clone`, `requestidlecallback` and
`webmessaging/without-ports`.

### P3a — page lifecycle and the window's session state (8 members)

Window to 76.7% C / 84.7% scan.

- `onpagehide` (71.4%), `onpageshow` (69.2%), `onbeforeunload` (59.1%) and
  `onunload`: PageTransitionEvent and BeforeUnloadEvent fired at yonder's
  arrival and at `page_departs`.
- `open` (3.9%, 264 pages, 14 files). `_self`, `_top` and `_parent` are a
  navigation ask; any other target answers `null`, a blocked popup, until a
  new-window design exists (d).
- `name` (61 pages): kept by the session across same-site navigations and
  cleared across sites, in yonder's trip/crumb state.
- `stop()`: a host hook to the stop yonder already has.
- `onstorage`: another yonder window committing the same origin. Slice 1a
  deferred it.

Oracle:

- Add `html/browsers/browsing-the-web/unloading-documents`.
- `html/browsers/the-window-object`: `name-attribute.window.js`,
  `window-open-*` and `open-close/`.
- `webstorage`'s `event_*` pages, already fetched.

### P3b — the window on the screen (5 members)

Window to 80.8% C / 85.9% scan.

- `visualViewport` (22.7% as a lower bound, 65 pages). With no pinch zoom it
  is the layout viewport, with `resize`, `scroll` and `scrollend` from the
  view step.
- `screenX` and `screenY` (17.7% and 17.1%), `screenLeft` and `screenTop`.
  The view provider gains the window's position, which the GUI already
  answers (`os64_gui_window_get_state`).

Oracle: add `visual-viewport`; `css/cssom-view/screenLeftTop.html` is already
fetched.

### P3c — legacy named access (3 members)

Document to 64.6% C / 94.0% scan.

- `document.all` (37.5%, 146 pages): an HTMLAllCollection with the engine's
  `[IsHTMLDDA]` slot (`JS_SetIsHTMLDDA` is in the pinned QuickJS). Absent, it
  already reads falsy as the real one does, so this serves only code that
  indexes it.
- Named access on Window (Chrome's `WindowProxyNamedGetter` is 19.0%),
  through the global-miss hook's engine patch.
- Named access on Document, through storage.c's legacy-platform-object
  pattern.
- The global's prototype chain, with the `Window` and `EventTarget`
  interface objects, belongs here too.

Oracle: `html/browsers/the-window-object/named-access-on-the-window-object`,
`html/dom/documents/dom-tree-accessors/nameditem-*`, and
`html/infrastructure/common-dom-interfaces/collections/htmlallcollection.html`.

### P4a — traversal and the missing node kinds (5 members, and one static)

Document to 64.7% C / 94.7% scan.

- `createTreeWalker` (80 pages) and `createNodeIterator`, with NodeFilter.
  NodeIterator's pre-removal steps need RELAYOUT.md's 5a mutation callback,
  so it lands after 5a or the phase splits.
- `moveBefore`.
- `createCDATASection` and `createProcessingInstruction`, which need libhtml
  node kinds.
- The static `Document.parseHTMLUnsafe`.
- Node's nine missing members fit here.

Oracle:

- Add `dom/traversal`.
- `dom/nodes`, already fetched: `moveBefore/`,
  `Document-createCDATASection` and `Document-createProcessingInstruction`.
- Add `html/webappapis/dynamic-markup-insertion/html-unsafe-methods`.

### P4b — hit testing (3 members)

Document to 70.4% C / 95.2% scan.

- `elementFromPoint` (11.8%, 66 pages), `elementsFromPoint` and
  `caretPositionFromPoint`.
- The host hook is the click path's hit test; CaretPosition also needs a
  text offset.

Oracle: `css/cssom-view`'s `elementFromPoint*`, `elementsFromPoint*` and
caret pages, already fetched.

### P5 — the input and form events yonder does not fire (20 handlers)

Window to 82.1% C / 86.0% scan; Document to 77.8% C / 95.2% scan.

- `dblclick`, `contextmenu`, `auxclick`, `wheel` (WheelEvent, cancelable,
  before the scroll), `mouseenter`/`mouseleave`, and `scrollend` (27.0% by
  its feature), each on window and document. The handlers come with the
  firing.
- `invalid` and `select`, and `formdata` once slice 2's FormData exists.
- This may be two PRs: mouse and wheel, then forms.

Oracle: add `uievents`, minus its `test_driver` pages, excluded by name, and
`html/semantics/forms/constraints`.

### P6 — four interfaces, one PR each

Ordered by usage:

- **P6a, `trustedTypes`** (Chrome 59.1% by `createPolicy`, 337 pages, 67
  files and 100 asks). It is the policy factory and the three Trusted types,
  with no enforcement until CSP (Tier 1). The sinks stringify them.
  Window to 85.0% C. Oracle: `trusted-types`, whose CSP-enforcement pages
  are excluded by name.
- **P6b, `document.fonts`** (31.5%, 52 pages). FontFaceSet and FontFace over
  yonder's webfont jobs, `fonts.ready` being the common use. Document to
  86.5% C. Oracle: `css/css-font-loading`.
- **P6c, `cookieStore`** (11.9%). It is `[SecureContext]`, over the jar's
  provider, with change events. Oracle: `cookiestore`.
- **P6d, `scheduler`** (9.6%, 111 pages). `postTask` and `yield`; it needs
  TaskSignal and so AbortSignal, after slice 2. Oracle: `scheduler`.

### What the plan leaves, and why

The members the slices own are (c): `fetch` (slice 2), and
`requestAnimationFrame` and `cancelAnimationFrame` (slice 5d), which take
Window to 92.6% C / 94.7% scan. Custom elements and CSP are Tier 1.

The (d) groups, by their largest Chrome number:

- `localStorage`-scale state: `caches` (29.0%, service workers),
  `indexedDB` (24.7%).
- Selection and Range: `getSelection` (7.2% or more), `createRange` (91
  pages).
- `speechSynthesis` (14.4%, audio).
- CSSOM sheets: `adoptedStyleSheets` (13.3%), `styleSheets`.
- Web Animations: `getAnimations` (5.1%).
- XPath: `evaluate` (3.5%).
- Editing: `execCommand` (1.6%, 116 pages), `designMode`.
- Fullscreen: `fullscreenElement` (86 pages).
- The Navigation API, printing, canvas's `createImageBitmap`, pointer lock,
  view transitions, picture-in-picture, and the handler families (pointer,
  drag, clipboard, CSS animation and transition, media).

Fullscreen has a cheap honest form that could come earlier:
`fullscreenEnabled` false, `fullscreenElement` null and
`requestFullscreen` rejecting. Whether that beats absence is a ruling.

The (e) skips, with their reasons in the appendix:

- touch handlers and `orientation`, which make a desktop browser read as a
  phone;
- device-motion and gamepad handlers (no such hardware);
- Storage Access (third-party frames only);
- Chromium-only APIs (`fetchLater`, file pickers, window management,
  private tokens, `fence`, `launchQueue`);
- `ariaNotify` (no assistive technology);
- the obsolete color reflections (`fgColor` and the rest).

## Coverage by phase

Cumulative, each interface separately. **unw** is members present over the
242/235 that ship. **C** is Chrome-weighted, over the 91/45 measured members.
**scan** is corpus-weighted, over all members.

| after | Window n | unw | C | scan | Document n | unw | C | scan |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| today | 64 | 26.4% | 52.1% | 70.4% | 58 | 24.7% | 24.9% | 67.8% |
| P1-W | 91 | 37.6% | 60.7% | 76.2% | 58 | 24.7% | 24.9% | 67.8% |
| P1-Da | 91 | 37.6% | 60.7% | 76.2% | 83 | 35.3% | 42.1% | 90.5% |
| P1-Db | 91 | 37.6% | 60.7% | 76.2% | 93 | 39.6% | 54.3% | 92.8% |
| P2a | 95 | 39.3% | 60.7% | 78.3% | 94 | 40.0% | 54.3% | 92.8% |
| P2b | 101 | 41.7% | 66.8% | 82.4% | 94 | 40.0% | 54.3% | 92.8% |
| P3a | 109 | 45.0% | 76.7% | 84.7% | 94 | 40.0% | 54.3% | 92.8% |
| P3b | 114 | 47.1% | 80.8% | 85.9% | 94 | 40.0% | 54.3% | 92.8% |
| P3c | 115 | 47.5% | 80.8% | 85.9% | 96 | 40.9% | 64.6% | 94.0% |
| P4a | 115 | 47.5% | 80.8% | 85.9% | 101 | 43.0% | 64.7% | 94.7% |
| P4b | 115 | 47.5% | 80.8% | 85.9% | 104 | 44.3% | 70.4% | 95.2% |
| P5 | 125 | 51.7% | 82.1% | 86.0% | 114 | 48.5% | 77.8% | 95.2% |
| P6a–d | 128 | 52.9% | 86.0% | 88.5% | 115 | 48.9% | 86.5% | 95.6% |
| slices 2 and 5d | 131 | 54.1% | 92.6% | 94.7% | 115 | 48.9% | 86.5% | 95.6% |

Unweighted coverage stays near half by design. 108 Window and 118 Document
members are (d) or (e), and 162 of those 226 are event handlers. The
unweighted ceiling of everything this plan and the arc's slices name is 133
Window and 117 Document members.

Two notes on reading the weighted columns:

- Document's C column moves on niche counters: `prerendering` 61.4%,
  `scripts` 40.7%, `all` 37.5% and `fonts` 31.5%. Its common members
  (`title`, `readyState`) have no counter at all. The scan column is the
  fairer one for Document.
- The scan weights include the (e) skips. Without them, Window's figures rise
  by about two points (today 72.3% rather than 70.4%).

## Rulings this needs

The lean for each is stated. All of them are Chris's.

1. **`crypto` without `subtle` on an `https:` page.** The lean is to ship
   it: the call pages make is `getRandomValues`, and `subtle`'s absence is
   detected separately. The alternative is to wait for a WebCrypto design
   over BearSSL (d). **Ruled for the lean** (2026-10-23); P1-W ships it.
2. **`createElementNS` for a namespace other than HTML, SVG and MathML.**
   The lean is to refuse it with NotSupportedError, as `createDocument`
   already does, rather than teach libhtml an element in an arbitrary
   namespace. DIVERGENCES.md gets the line.
3. **`window.open` to a new window.** The lean is that P3a answers `null`
   (a blocked popup) for any target but the page's own. A real new window
   needs a WindowProxy for a second browsing context, which is the iframe
   design's territory (Tier 1).
4. **Fullscreen's honest refusal before real fullscreen.** The lean is no:
   absence and refusal read the same to most pages. Without a reason to
   prefer the refusal, it waits for the real thing.

## What could not be determined

- **HTTP Archive per-member usage.** It is behind BigQuery and could not be
  fetched; § Usage says what was used instead.
- **A Chrome number for most common members.** 151 Window and 190 Document
  members have no counter, `title`, `readyState`, `setTimeout` and
  `addEventListener` among them. They are ranked by the corpus scan alone.
- **Whether a handler "ships" on Window and Document.** BCD records events,
  not GlobalEventHandlers members. A handler counts as shipped when BCD
  shows its event shipped on any interface. For the newest handlers
  (`oncommand`, `onbeforematch`) that is approximate.
- **webstatus.dev's dates.** The API serves a "daily" figure without saying
  which day; it is quoted as served on 2026-10-23.
- **Whether the classic stubs differ in yonder from the DOM host.**
  `confirm`/`prompt` are libdom's (`core.c`) and yonder sets no option that
  changes them; `alert` reaches yonder's `script_alert`. This was read from
  the source, not probed in the scripted-page host.

## Sources

| source | version / date | used for |
|---|---|---|
| webref `ed/idl` (the w3c/webref repository) | commit `91b1d4c9`, 2026-10-23T12:53Z; 354 IDL files | every member, its spec and its kind; interface objects exposed on Window |
| @mdn/browser-compat-data | 8.1.7, 2026-10-22 | shipped in at least one of Chrome, Firefox and Safari, desktop or mobile; deprecated flags |
| Chrome use counters (chromestatus.com `/data/featurepopularity`) | the day 2026-10-21, fetched 2026-10-23 | the C column |
| webstatus.dev API, with web-features 3.44.0 | as served 2026-10-23 | the W numbers |
| HTTP Archive / Web Almanac | not usable per member (BigQuery only) | nothing |
| yonder_tally over `2026-10-22b_0d-after`: the baseline pull alone, replayed at Slice 0d | 198 page files, 86 sites | MISSING files/asks |
| static scan of the three pulls 2026-10-21, -22 and -22b together | 839 pages, 163 sites | scan pages |
| the DOM host probe at `userland` 47e960c1 | 2026-10-23 | present, classic/modern, stub |
| WPT | the runner's pin `a699fb16` (directory names checked against it) | each phase's oracle |

No site name or address from the corpus appears in this document.

## Appendix A — Window members

Columns:

- **yonder**: present (classic, or modern for switch-on only), with
  "stub", "constant" or "partial" where the probe found one; else absent.
- **Chrome %**: § Usage 1. A **W** suffix is a webstatus.dev feature
  figure.
- **scan pages**: of 839, the three pulls together.
- **MISSING files/asks**: of 198 page files, the baseline pull's.
- **class**: (a)–(e) and its phase.

Members with Chrome numbers come first, ordered by them; then the rest by
scan pages.

| member | kind | spec | yonder | Chrome % | scan pages | MISSING files/asks | class | note |
|---|---|---|---|---|---|---|---|---|
| `location` | ro attr | html | classic | 96.03 | 635 |  |  |  |
| `top` | ro attr | html | classic | 80.25 | 254 |  |  | a read-only data property when measured; since P1-W an unforgeable accessor asking the browsing context (IFRAME.md's rule) |
| `localStorage` | ro attr | html | modern | 79.76 | 463 |  |  | proxy: DOMStorageRead counts any Web Storage read (both areas) |
| `sessionStorage` | ro attr | html | modern | 79.76 | 354 |  |  | proxy: DOMStorageRead counts any Web Storage read (both areas) |
| `fetch` | method | fetch | absent | 72.62 | 502 | 55/82 | c slice 2 | fetch, in review; counter `Fetch` |
| `innerWidth` | ro attr | cssom-view | modern | 72.25 | 456 |  |  | counter `WindowInnerWidth` |
| `parent` | ro attr | html | classic | 70.71 | 220 |  |  |  |
| `innerHeight` | ro attr | cssom-view | modern | 70.14 | 439 |  |  | counter `WindowInnerHeight` |
| `performance` | ro attr | hr-time | modern | 67.01 | 472 |  |  | PerformanceTimeline = any Performance Timeline use |
| `matchMedia` | method | cssom-view | modern | 66.00 | 395 |  |  |  |
| `requestAnimationFrame` | method | html | absent | 64.33 | 457 | 46/58 | c slice 5d | RELAYOUT.md 5d; counter UnprefixedRequestAnimationFrame |
| `devicePixelRatio` | ro attr | cssom-view | modern | 60.43 | 237 |  |  | counter `WindowDevicePixelRatio` |
| `pageYOffset` | ro attr | cssom-view | modern | 60.21 | 435 |  |  | counter `WindowPageYOffset` |
| `trustedTypes` | ro attr | trusted-types | absent | 59.08 | 337 | 67/100 | b P6a | Trusted Types without enforcement (no CSP yet): the policy factory and the three Trusted* types, sinks stringify them; proxy: trustedTypes.createPolicy |
| `requestIdleCallback` | method | requestidlecallback | absent | 54.23 | 343 | 15/17 | b P2b | IdleDeadline and a callback run when yonder's loop is idle, with a deadline from the host clock |
| `pageXOffset` | ro attr | cssom-view | modern | 54.05 | 393 |  |  | counter `WindowPageXOffset` |
| `postMessage` | method | html | absent | 52.08 | 194 | 26/41 | b P2b | same-window postMessage: a MessageEvent task to window carrying a structured clone; cross-window is (d); counter `WindowPostMessage` |
| `opener` | attr | html | absent | 51.60 | 19 | 20/143 | a P1-W | null: no yonder window is opened by a script (settable, [Replaceable]-like per HTML) |
| `crypto` | ro attr | webcrypto | absent | 49.19 | 210 | 34/87 | a P1-W | Crypto with getRandomValues and randomUUID over a host entropy hook (/dev/random, the source libtls reads); subtle is [SecureContext] and (d); proxy: crypto.getRandomValues calls; lower bound for the attribute |
| `scrollY` | ro attr | cssom-view | modern | 45.27 | 299 |  |  | counter `WindowScrollY` |
| `length` | ro attr | html | classic, constant | 44.55 | 17 |  |  | 0: no frames; since P1-W asked of the browsing context, not installed as a constant (IFRAME.md's rule) |
| `outerWidth` | ro attr | cssom-view | modern | 31.56 | 141 |  |  | counter `WindowOuterWidth` |
| `outerHeight` | ro attr | cssom-view | modern | 30.12 | 139 |  |  | counter `WindowOuterHeight` |
| `event` | ro attr | dom | classic | 28.99 | 171 |  |  | counter `WindowEvent` |
| `caches` | ro attr | service-workers | absent | 28.97 | 3 |  | d | service workers and their caches/push; counter GlobalCacheStorage |
| `indexedDB` | ro attr | IndexedDB | absent | 24.69 | 113 | 2/3 | d | IndexedDB: a database engine; proxy: any IndexedDB read |
| `scrollX` | ro attr | cssom-view | modern | 24.46 | 201 |  |  | counter `WindowScrollX` |
| `visualViewport` | ro attr | cssom-view | absent | 22.66 | 65 |  | b P3b | VisualViewport over the view provider (no pinch zoom: the layout viewport), resize/scroll/scrollend events from the view step; proxy: visualViewport.height; lower bound |
| `structuredClone` | method | html | absent | 21.93 | 149 | 14/24 | b P2b | the binding's StructuredSerialize pair (planned as the engine's object writer/reader; § P2b), DataCloneError for the rest, transfer of ArrayBuffers; counter `StructuredCloneMethod` |
| `customElements` | ro attr | html | absent | 19.60 | 52 | 4/4 | c Tier 1 | custom elements: named in MODERN_WEB.md (Tier 1 or end of Tier 0), not yet a numbered slice; proxy: customElements.define |
| `screenY` | ro attr | cssom-view | absent | 17.69 | 64 |  | b P3b | the window's position, which the view provider does not carry today (os64_gui_window_get_state has it); counter `WindowScreenY` |
| `screenX` | ro attr | cssom-view | absent | 17.15 | 62 |  | b P3b | the window's position, which the view provider does not carry today (os64_gui_window_get_state has it); counter `WindowScreenX` |
| `speechSynthesis` | ro attr | speech-api | absent | 14.42 | 15 |  | d | speech: an audio driver first (the media arc) |
| `screenLeft` | ro attr | cssom-view | absent | 14.06 | 23 |  | b P3b | the window's position, which the view provider does not carry today (os64_gui_window_get_state has it); counter `WindowScreenLeft` |
| `screenTop` | ro attr | cssom-view | absent | 13.96 | 25 |  | b P3b | the window's position, which the view provider does not carry today (os64_gui_window_get_state has it); counter `WindowScreenTop` |
| `locationbar` | ro attr | html | absent | 13.44 |  |  | a P1-W | BarProp {visible: true}: not a popup; counter `BarPropLocationbar` |
| `toolbar` | ro attr | html | absent | 13.40 |  |  | a P1-W | BarProp {visible: true}: not a popup; counter `BarPropToolbar` |
| `statusbar` | ro attr | html | absent | 13.12 | 8 |  | a P1-W | BarProp {visible: true}: not a popup; counter `BarPropStatusbar` |
| `menubar` | ro attr | html | absent | 13.12 |  |  | a P1-W | BarProp {visible: true}: not a popup; counter `BarPropMenubar` |
| `orientation` | ro attr | compat | absent | 12.44 | 17 |  | e | mobile-only (desktop browsers leave it out): presence reads as a phone; counter `WindowOrientation` |
| `cookieStore` | ro attr | cookiestore | absent | 11.88 | 14 |  | b P6c | CookieStore over the jar's provider; [SecureContext]; needs the jar's per-cookie fields and change events; counter `CookieStoreAPI` |
| `scheduler` | ro attr | scheduling-apis | absent | 9.59 | 111 |  | b P6d | Scheduler (postTask, yield) over the task loop; needs AbortSignal/TaskSignal, so after slice 2; proxy: scheduler.yield / postTask |
| `frames` | ro attr | html | classic, constant | 9.49 | 101 |  |  | the window itself: no frames |
| `closed` | ro attr | html | absent | 8.49 | 1 |  | a P1-W | false while the page is live; counter `WindowClosed` |
| `getSelection` | method | selection-api | absent | 7.21 | 119 | 4/4 | d | Selection and Range: no slice plans them; proxy: Selection.anchorNode read; lower bound |
| `fence` | ro attr | fenced-frame | absent | 6.44 W |  |  | e | Chromium-only or near-zero usage, meaningless for one user on one machine; webstatus.dev feature `fencedframe` (feature-level) |
| `clientInformation` | ro attr | html | absent | 5.78 | 1 |  | a P1-W | the navigator object, again; counter `WindowClientInformation` |
| `personalbar` | ro attr | html | absent | 4.82 |  |  | a P1-W | BarProp {visible: true}: not a popup; counter `BarPropPersonalbar` |
| `scrollbars` | ro attr | html | absent | 4.77 |  |  | a P1-W | BarProp {visible: true}: not a popup; counter `BarPropScrollbars` |
| `window` | ro attr | html | classic, partial | 4.18 | 40 |  |  | a writable data property; HTML makes it [LegacyUnforgeable] (non-writable, non-configurable); P1-W |
| `open` | method | html | absent | 3.89 | 264 | 14/14 | b P3a | '_self'/'_top'/'_parent' as a navigation ask; any other target answers null (a blocked popup) until a new-window design (d); counter `DOMWindowOpen` |
| `fetchLater` | method | fetch | absent | 3.41 |  |  | e | Chromium-only |
| `status` | attr | html | classic, stub | 3.41 | 8 |  |  | reads '' whatever was assigned; HTML keeps the last string set (a one-line fix, P1-W); counter is window.defaultStatus, the legacy twin |
| `self` | ro attr | html | classic | 3.02 | 114 |  |  |  |
| `focus` | method | html | modern, constant | 2.00 | 27 |  |  | does nothing (the window is the person's to raise; HTML allows it) |
| `createImageBitmap` | method | html | absent | 1.76 | 7 |  | d | canvas: no slice plans it; counter `CreateImageBitmap` |
| `launchQueue` | ro attr | web-app-launch | absent | 1.32 W |  |  | e | Chromium-only or near-zero usage, meaningless for one user on one machine; webstatus.dev feature `app-launch-handler` (feature-level) |
| `cancelAnimationFrame` | method | html | absent | 1.31 | 207 | 19/33 | c slice 5d | RELAYOUT.md 5d; proxy: a frame cancelled within its frame; lower bound |
| `close` | method | html | absent | 0.52 | 14 |  | a P1-W | no-op: no page closes the person's window, where HTML would let one close a window with a single history entry (DIVERGENCES.md) |
| `alert` | method | html | classic | 0.37 | 3 |  |  |  |
| `confirm` | method | html | classic, stub | 0.25 | 13 |  |  | answers false without asking |
| `navigation` | ro attr | html | absent | 0.23 W | 54 |  | d | the Navigation API: a second session model beside history; webstatus.dev feature `navigation` (feature-level) |
| `resizeTo` | method | cssom-view | absent | 0.15 | 4 |  | a P1-W | no-op: only a script-opened auxiliary window may be moved or sized (CSSOM View); WindowResize covers resizeTo and resizeBy |
| `resizeBy` | method | cssom-view | absent | 0.15 | 1 |  | a P1-W | no-op: only a script-opened auxiliary window may be moved or sized (CSSOM View); counter `WindowResize` |
| `print` | method | html | absent | 0.14 | 26 |  | a, no phase | a no-op, which HTML's printing steps permit (§ Classification); in no phase's count |
| `credentialless` | ro attr | anonymous-iframe | absent | 0.10 W |  |  | e | Chromium-only or near-zero usage, meaningless for one user on one machine; webstatus.dev feature `iframe-credentialless` (feature-level) |
| `moveTo` | method | cssom-view | absent | 0.08 | 21 |  | a P1-W | no-op: only a script-opened auxiliary window may be moved or sized (CSSOM View); WindowMove covers moveTo and moveBy |
| `moveBy` | method | cssom-view | absent | 0.08 | 2 |  | a P1-W | no-op: only a script-opened auxiliary window may be moved or sized (CSSOM View); counter `WindowMove` |
| `captureEvents` | method | html | absent | 0.08 | 1 |  | a P1-W | no-op, as HTML defines them (document has them already); counter `WindowCaptureEvents` |
| `documentPictureInPicture` | ro attr | document-picture-in-picture | absent | 0.05 W |  |  | e | Chromium-only or near-zero usage, meaningless for one user on one machine; webstatus.dev feature `document-picture-in-picture` (feature-level) |
| `getScreenDetails` | method | window-management | absent | 0.04 |  |  | e | Chromium-only or near-zero usage, meaningless for one user on one machine |
| `blur` | method | html | modern, constant | 0.02 |  |  |  | does nothing (HTML allows it) |
| `prompt` | method | html | classic, stub | 0.01 |  |  |  | answers null without asking |
| `showOpenFilePicker` | method | file-system-access | absent | 0.003 |  |  | e | Chromium-only or near-zero usage, meaningless for one user on one machine |
| `releaseEvents` | method | html | absent | 0.003 | 2 |  | a P1-W | no-op, as HTML defines them (document has them already); counter `WindowReleaseEvents` |
| `showSaveFilePicker` | method | file-system-access | absent | 0.001 |  |  | e | Chromium-only or near-zero usage, meaningless for one user on one machine |
| `queryLocalFonts` | method | local-font-access | absent | 0.001 |  |  | e | Chromium-only or near-zero usage, meaningless for one user on one machine |
| `showDirectoryPicker` | method | file-system-access | absent | <0.001 |  |  | e | Chromium-only or near-zero usage, meaningless for one user on one machine |
| `when` | method | observable | absent | <0.001 W |  |  | e | Observable (Chromium-only, ~zero usage); webstatus.dev feature `observable` (feature-level) |
| `setTimeout` | method | html | classic |  | 709 |  |  |  |
| `addEventListener` | method | dom | classic |  | 646 |  |  |  |
| `clearTimeout` | method | html | classic |  | 623 |  |  |  |
| `setInterval` | method | html | classic |  | 515 |  |  |  |
| `atob` | method | html | modern |  | 500 |  |  |  |
| `history` | ro attr | html | classic, partial |  | 487 |  |  | a writable data property; HTML makes it a read-only accessor; P1-W |
| `clearInterval` | method | html | classic |  | 445 |  |  |  |
| `btoa` | method | html | modern |  | 440 |  |  |  |
| `getComputedStyle` | method | cssom | modern |  | 435 |  |  |  |
| `removeEventListener` | method | dom | classic |  | 410 |  |  |  |
| `navigator` | ro attr | html | classic |  | 379 |  |  |  |
| `queueMicrotask` | method | html | absent |  | 329 | 18/26 | a P1-W | the engine's job queue (JS_EnqueueJob), which the binding's promise pump already drains |
| `document` | ro attr | html | classic, partial |  | 328 |  |  | a writable data property; HTML makes it [LegacyUnforgeable]; P1-W |
| `scrollTo` | method | cssom-view | modern |  | 307 |  |  |  |
| `dispatchEvent` | method | dom | classic |  | 301 |  |  |  |
| `isSecureContext` | ro attr | html | absent |  | 241 |  | a P1-W | the page's address potentially trustworthy (Secure Contexts § 3.1) as libway judges it for mixed content (`way_url_trustworthy`): about:blank and about:srcdoc, https, wss, data: and 127.0.0.0/8; the name localhost is not (ORIGIN.md's ruling), and a file is not (its origin is opaque) |
| `screen` | ro attr | cssom-view | modern |  | 223 |  |  |  |
| `reportError` | method | html | absent |  | 114 | 10/16 | b P2a | reports through the same error path as onerror |
| `frameElement` | ro attr | html | absent |  | 101 | 13/16 | a P1-W | null: a top-level window |
| `cancelIdleCallback` | method | requestidlecallback | absent |  | 74 | 12/12 | b P2b | IdleDeadline and a callback run when yonder's loop is idle, with a deadline from the host clock |
| `name` | attr | html | absent |  | 61 | 4/4 | b P3a | a string the window's session keeps across same-site navigations (cleared cross-site): yonder's trip/crumb state |
| `origin` | ro attr | html | absent |  | 58 | 56/61 | a P1-W | serialization of the binding's d_origin (storage.c), the origin storage already keys on |
| `external` | ro attr | html | absent |  | 55 |  | a P1-W | External: AddSearchProvider() and IsSearchProviderInstalled() do nothing (HTML § obsolete) |
| `scrollBy` | method | cssom-view | modern |  | 54 |  |  |  |
| `scroll` | method | cssom-view | modern |  | 38 |  |  |  |
| `crossOriginIsolated` | ro attr | html | absent |  | 19 |  | a P1-W | constant false: yonder sends no COOP/COEP isolation |
| `stop` | method | html | absent |  | 6 |  | b P3a | a host hook to stop the page load (yonder has a stop button) |
| `pushManager` | ro attr | push-api | absent |  | 5 |  | d | service workers and their caches/push |
| `setResizable` | method | window-management | absent | <0.001 | 1 |  | e | Chromium-only or near-zero usage, meaningless for one user on one machine; counter `WindowSetResizable` |
| `(named properties)` | getter | html | absent |  |  |  | b P3c | named access on the Window: the global-miss hook (libjs patch 0008) answering ids and names from the tree |
| `crashReport` | ro attr | crash-reporting | absent |  |  |  | e | Chromium-only or near-zero usage, meaningless for one user on one machine |
| `getDigitalGoodsService` | method | digital-goods | absent |  |  |  | e | Chromium-only or near-zero usage, meaningless for one user on one machine |
| `originAgentCluster` | ro attr | html | absent |  |  |  | a P1-W | constant true: origin-keyed agent clusters are the default; one page per window |
| `requestResize` | method | css-sizing-4 | absent |  |  |  | e | Chromium-only or near-zero usage, meaningless for one user on one machine |
| `viewport` | ro attr | css-viewport | absent |  |  |  | e | Chromium-only or near-zero usage, meaningless for one user on one machine |

Event handler IDL attributes (`on<type>`), grouped where they share a status and a reason. A number after a name is its scan pages (and, on window, `M` its MISSING files); Chrome percentages are given where a counter exists.

| handlers | yonder | class | note |
|---|---|---|---|
| `onblur` 39, `onchange`, `onclick` 26, `onfocus` 39, `oninput`, `onkeydown`, `onkeypress`, `onkeyup`, `onload` 105, `onmousedown`, `onmousemove`, `onmouseout`, `onmouseover`, `onmouseup`, `onreset`, `onsubmit` | classic |  |  |
| `onhashchange` 11, `onpopstate` 5, `onresize` 19, `onscroll` 37 | modern |  |  |
| `onlanguagechange`, `onoffline`, `ononline` | absent | a P1-W | handler slots (d_event_handler); events that never fire on one machine |
| `onerror` 160 M15 | absent | b P2a | ErrorEvent at window for an uncaught exception: the script host already knows (the FAILED script line) |
| `onrejectionhandled`, `onunhandledrejection` 110 M14 | absent | b P2a | PromiseRejectionEvent from the engine's rejection tracker (JS_SetHostPromiseRejectionTracker) |
| `onmessage` 15, `onmessageerror` | absent | b P2b | same-window postMessage: a MessageEvent task to window carrying a structured clone; cross-window is (d) |
| `onbeforeunload` 8 C59.11, `onpagehide` 39 C71.39, `onpageshow` 40 C69.15, `onunload` C2.60 | absent | b P3a | page-lifecycle events at yonder's arrival and departure (page_departs), PageTransitionEvent and BeforeUnloadEvent |
| `onstorage` | absent | b P3a | the storage event: another yonder window committing the same origin (slice 1a deferred it) |
| `onauxclick`, `oncontextmenu`, `ondblclick`, `onmouseenter`, `onmouseleave`, `onscrollend` C27.02 W, `onwheel` 7 | absent | b P5 | events yonder's input path does not fire today (dblclick, contextmenu, auxclick, wheel, mouseenter/leave, scrollend), each with its handler |
| `onformdata`, `oninvalid`, `onselect` | absent | b P5 | form events: constraint validation, FormData (slice 2), text selection in a control |
| `onsecuritypolicyviolation` | absent | c Tier 1 | CSP (Tier 1) |
| `onabort`, `oncanplay`, `oncanplaythrough`, `oncuechange`, `ondurationchange`, `onemptied`, `onended`, `onloadeddata`, `onloadedmetadata`, `onloadstart`, `onpause`, `onplay`, `onplaying`, `onprogress`, `onratechange`, `onseeked`, `onseeking`, `onstalled`, `onsuspend`, `ontimeupdate`, `onvolumechange`, `onwaiting` | absent | d | media events: the media arc (Tier 2) |
| `onafterprint`, `onbeforeprint` | absent | d | printing |
| `onanimationcancel`, `onanimationend` 1, `onanimationiteration` 1, `onanimationstart` 1, `ontransitioncancel` 1, `ontransitionend` 1, `ontransitionrun` 6, `ontransitionstart`, `onwebkitanimationend` 14, `onwebkitanimationiteration` 7, `onwebkitanimationstart` 14, `onwebkittransitionend` 8 | absent | d | CSS animation and transition events: a page waiting for transitionend stalls if the handler exists and the event never comes |
| `onbeforeinput` | absent | d | Input Events level 2 over the text editor |
| `onbeforematch`, `onbeforetoggle`, `oncancel`, `onclose`, `oncommand`, `onslotchange`, `ontoggle` | absent | d | element features yonder lacks: details/popover/dialog/invoker commands/hidden=until-found/shadow DOM |
| `oncontextlost`, `oncontextrestored` | absent | d | canvas: no slice plans it |
| `oncopy`, `oncut`, `onpaste` | absent | d | clipboard events |
| `ondrag`, `ondragend`, `ondragenter`, `ondragleave`, `ondragover`, `ondragstart`, `ondrop` | absent | d | drag and drop: the handlers are what pages detect DnD by |
| `ongotpointercapture` 7, `onlostpointercapture` 7, `onpointercancel` 7, `onpointerdown` 1, `onpointerenter` 7, `onpointerleave` 1, `onpointermove` 7, `onpointerout` 7, `onpointerover` 7, `onpointerrawupdate`, `onpointerup` 8 | absent | d | Pointer Events: exposing the handlers without firing pointer events makes pages that detect them stop listening for mouse events |
| `onpagereveal`, `onpageswap` | absent | d | view transitions |
| `onselectionchange`, `onselectstart` | absent | d | Selection and Range: no slice plans them |
| `onappinstalled` 1, `onbeforeinstallprompt` 1 C14.08 W, `onbeforexrselect` 6 | absent | e | Chromium-only or near-zero usage, meaningless for one user on one machine |
| `ondevicemotion` 1 C6.15 W, `ondeviceorientation` 7 C6.15 W, `ondeviceorientationabsolute` 7 C6.15 W | absent | e | no motion sensors on os64 |
| `ongamepadconnected` C0.001 W, `ongamepaddisconnected` C0.001 W | absent | e | no gamepad support |
| `onorientationchange` 15 | absent | e | mobile-only (desktop browsers leave it out): presence reads as a phone |
| `ontouchcancel` 1, `ontouchend` 181, `ontouchmove` 40, `ontouchstart` 204 | absent | e | desktop Chrome does not expose touch handlers without a touch screen; pages read 'ontouchstart' in window as 'this is a phone' |

## Appendix B — Document members

Columns as in Appendix A, without MISSING (the census hears only lookups on `window`). `parseHTMLUnsafe` and `parseHTML` are static members of the `Document` interface object and are not counted in the coverage.

| member | kind | spec | yonder | Chrome % | scan pages | class | note |
|---|---|---|---|---|---|---|---|
| `cookie` | attr | html | classic | 78.40 | 497 |  | counter `CookieGet` |
| `prerendering` | ro attr | prerendering-revamped | absent | 61.41 | 105 | a P1-Da | false: yonder never prerenders |
| `scripts` | ro attr | html | absent | 40.68 | 127 | a P1-Db | live HTMLCollections over the tree, the machinery images and forms already use; applets is always empty |
| `all` | ro attr | html | absent | 37.48 | 146 | b P3c | HTMLAllCollection with the engine's [IsHTMLDDA] slot (JS_SetIsHTMLDDA is in the pinned QuickJS); absent already reads falsy, as the real one does; counter `DocumentAll` |
| `fonts` | ro attr | css-font-loading | absent | 31.48 | 52 | b P6b | FontFaceSet and FontFace over yonder's webfont jobs (fonts.ready is the common use); counter `DocumentFonts` |
| `adoptedStyleSheets` | attr | cssom | absent | 13.25 | 12 | d | CSSOM style sheets (CSSStyleSheet, CSSRule, constructable sheets) over libgarb's sheets; counter `AdoptedStyleSheets` |
| `elementFromPoint` | method | cssom-view | absent | 11.82 | 66 | b P4b | a hit-test hook into yonder's layout (the click path's); CaretPosition needs a text offset |
| `forms` | ro attr | html | classic | 7.81 | 9 |  |  |
| `getSelection` | method | selection-api | absent | 7.21 | 5 | d | Selection and Range; proxy: Selection.anchorNode read (either getSelection); lower bound |
| `pictureInPictureEnabled` | ro attr | picture-in-picture | absent | 5.87 |  | d | video (the media arc) |
| `caretPositionFromPoint` | method | cssom-view | absent | 5.41 |  | b P4b | a hit-test hook into yonder's layout (the click path's); CaretPosition needs a text offset; counter `CaretPositionFromPoint` |
| `getAnimations` | method | web-animations | absent | 5.15 | 13 | d | Web Animations |
| `hasStorageAccess` | method | storage-access | absent | 4.46 | 7 | e | Storage Access: meaningful only in a third-party frame, and yonder has no frames (a top-level document always has access); counter `StorageAccessAPI_HasStorageAccess_Method` |
| `elementsFromPoint` | method | cssom-view | absent | 3.55 |  | b P4b | a hit-test hook into yonder's layout (the click path's); CaretPosition needs a text offset |
| `evaluate` | method | dom | absent | 3.49 | 38 | d | XPath 1.0: an engine of its own; counter `DocumentXPathEvaluate` |
| `images` | ro attr | html | classic | 3.36 | 116 |  |  |
| `hasUnpartitionedCookieAccess` | method | saa-non-cookie-storage | absent | 2.52 W |  | e | Storage Access: meaningful only in a third-party frame, and yonder has no frames (a top-level document always has access); webstatus.dev feature `non-cookie-storage-access` (feature-level) |
| `links` | ro attr | html | absent | 2.08 | 3 | a P1-Db | live HTMLCollections over the tree, the machinery images and forms already use; applets is always empty |
| `execCommand` | method | html | absent | 1.64 | 116 | d | editing (contenteditable); counter `ExecCommand` |
| `designMode` | attr | html | absent | 1.57 |  | d | editing (contenteditable); counter `DocumentDesignMode` |
| `pictureInPictureElement` | ro attr | picture-in-picture | absent | 1.33 |  | d | video (the media arc) |
| `startViewTransition` | method | css-view-transitions | absent | 1.30 W | 3 | d | view transitions; webstatus.dev feature `view-transitions` (feature-level) |
| `domain` | attr | html | absent | 0.76 | 158 | a P1-Da | the origin's host; the setter's checks with nothing to relax (no frames); counter is the SETTER (DocumentSetDomain) |
| `createAttribute` | method | dom | classic | 0.67 | 11 |  | counter `DocumentCreateAttribute` |
| `plugins` | ro attr | html | absent | 0.44 |  | a P1-Db | live HTMLCollections over the tree, the machinery images and forms already use; applets is always empty |
| `anchors` | ro attr | html | absent | 0.42 |  | a P1-Db | live HTMLCollections over the tree, the machinery images and forms already use; applets is always empty |
| `embeds` | ro attr | html | absent | 0.41 |  | a P1-Db | live HTMLCollections over the tree, the machinery images and forms already use; applets is always empty |
| `applets` | ro attr | html | absent | 0.40 |  | a P1-Db | live HTMLCollections over the tree, the machinery images and forms already use; applets is always empty |
| `pointerLockElement` | ro attr | pointerlock | absent | 0.36 |  | d | pointer lock (kernel mouse capture); counter `DocumentPointerLockElement` |
| `hasPrivateToken` | method | trust-token-api | absent | 0.31 | 1 | e | Private State Tokens: Chromium-only |
| `createAttributeNS` | method | dom | classic | 0.25 |  |  | counter `DocumentCreateAttributeNS` |
| `moveBefore` | method | dom | absent | 0.20 W |  | b P4a | insertBefore with the state-preserving move rules; yonder keeps node state on any move already; webstatus.dev feature `move-before` (feature-level) |
| `fragmentDirective` | ro attr | scroll-to-text-fragment | absent | 0.20 |  | e | text fragments: Chromium/Safari detection object, near-zero usage |
| `captureEvents` | method | html | classic, constant | 0.14 | 7 |  | does nothing, as HTML defines it; counter `DocumentCaptureEvents` |
| `parseHTMLUnsafe` | method | html | absent | 0.14 W |  | b P4a | static: a document of its own parsed from a string (implementation.c plus the fragment parser); webstatus.dev feature `parse-html-unsafe` (feature-level); static, on the Document interface object |
| `createExpression` | method | dom | absent | 0.14 |  | d | XPath 1.0: an engine of its own; counter `DocumentXPathCreateExpression` |
| `requestStorageAccess` | method | saa-non-cookie-storage | absent | 0.11 | 6 | e | Storage Access: meaningful only in a third-party frame, and yonder has no frames (a top-level document always has access); counter `StorageAccessAPI_requestStorageAccess_Method` |
| `hasRedemptionRecord` | method | trust-token-api | absent | 0.09 |  | e | Private State Tokens: Chromium-only |
| `createNSResolver` | method | dom | absent | 0.08 |  | d | XPath 1.0: an engine of its own; counter `DocumentXPathCreateNSResolver` |
| `exitPointerLock` | method | pointerlock | absent | 0.02 |  | d | pointer lock (kernel mouse capture); counter `DocumentExitPointerLock` |
| `exitPictureInPicture` | method | picture-in-picture | absent | 0.003 |  | d | video (the media arc) |
| `clear` | method | html | absent | 0.002 |  | a P1-Da | no-op, as HTML defines it; counter `DocumentClear` |
| `releaseEvents` | method | html | classic, constant | <0.001 |  |  | does nothing, as HTML defines it; counter `DocumentReleaseEvents` |
| `when` | method | observable | absent | <0.001 W |  | e | Observable (Chromium-only, ~zero usage); webstatus.dev feature `observable` (feature-level) |
| `createElement` | method | dom | classic |  | 662 |  |  |
| `getElementById` | method | dom | classic |  | 652 |  |  |
| `addEventListener` | method | dom | classic |  | 599 |  |  |
| `body` | attr | html | classic |  | 585 |  | its setter arrived with P1-Da, a classic fix: HTML's, which replaces the body element or appends one to the root |
| `querySelector` | method | dom | classic |  | 566 |  |  |
| `documentElement` | ro attr | dom | classic |  | 537 |  |  |
| `getElementsByTagName` | method | dom | classic |  | 523 |  |  |
| `querySelectorAll` | method | dom | classic |  | 496 |  |  |
| `createTextNode` | method | dom | classic |  | 423 |  |  |
| `head` | ro attr | html | classic |  | 418 |  |  |
| `removeEventListener` | method | dom | classic |  | 416 |  |  |
| `readyState` | ro attr | html | absent |  | 378 | a P1-Da | loading/interactive/complete, moved by the host's own DOMContentLoaded and load dispatches; readystatechange fired just before each |
| `visibilityState` | ro attr | html | absent |  | 378 | a P1-Da | 'visible'/false; visibilitychange when the desktop minimizes the window is (b) and can wait |
| `referrer` | ro attr | html | absent |  | 316 | a P1-Da | the trip's referrer (trip.c's leg.referrer, way_referrer), handed in options |
| `getElementsByClassName` | method | dom | classic |  | 301 |  |  |
| `activeElement` | ro attr | html | absent |  | 298 | a P1-Da | yonder's g.focus_node through a host hook (body when none); hasFocus whether the page is the one on screen (yonder does not follow its window's own focus: DEBTS.md) |
| `createEvent` | method | dom | classic |  | 295 |  |  |
| `location` | ro attr | html | classic |  | 282 |  |  |
| `currentScript` | ro attr | html | absent |  | 271 | a P1-Da | the script element the host is running, set around os64_js_run by yonder (scripts.c knows the node) |
| `hidden` | ro attr | html | absent |  | 233 | a P1-Da | 'visible'/false; visibilitychange when the desktop minimizes the window is (b) and can wait |
| `title` | attr | html | absent |  | 229 | a P1-Da | the first title element (yonder's title_of walks the same rule); the setter makes one in head; SVG root's title for an SVG document |
| `write` | method | html | classic |  | 182 |  |  |
| `dispatchEvent` | method | dom | classic |  | 147 |  |  |
| `createDocumentFragment` | method | dom | classic |  | 125 |  |  |
| `getElementsByName` | method | html | classic |  | 103 |  |  |
| `createElementNS` | method | dom | absent |  | 99 | a P1-Db | implementation.c's validate-and-extract and its three namespaces; any other namespace is refused as createDocument refuses it |
| `URL` | ro attr | dom | absent |  | 91 | a P1-Da | the document's address: options.url for the window's, about:blank for documents of their own |
| `createRange` | method | dom | absent |  | 91 | d | Selection and Range |
| `createComment` | method | dom | classic |  | 87 |  |  |
| `fullscreenElement` | ro attr | fullscreen | absent |  | 86 | d | Fullscreen (an honest refusal, fullscreenEnabled false, is possible earlier) |
| `createTreeWalker` | method | dom | absent |  | 80 | b P4a | TreeWalker and NodeFilter: a pure walk over the tree |
| `hasFocus` | method | html | absent |  | 78 | a P1-Da | yonder's g.focus_node through a host hook (body when none); hasFocus whether the page is the one on screen (yonder does not follow its window's own focus: DEBTS.md) |
| `wasDiscarded` | ro attr | page-lifecycle | absent |  | 78 | a P1-Da | false |
| `implementation` | ro attr | dom | classic |  | 76 |  |  |
| `exitFullscreen` | method | fullscreen | absent |  | 67 | d | Fullscreen (an honest refusal, fullscreenEnabled false, is possible earlier) |
| `styleSheets` | ro attr | cssom | absent |  | 56 | d | CSSOM style sheets (CSSStyleSheet, CSSRule, constructable sheets) over libgarb's sheets |
| `importNode` | method | dom | absent |  | 50 | a P1-Db | clone into, or move into, this document: documents of their own already share the arena and adopt on append |
| `open` | method | html | classic |  | 50 |  |  |
| `scrollingElement` | ro attr | cssom-view | modern |  | 47 |  |  |
| `compatMode` | ro attr | dom | absent |  | 41 | a P1-Da | libhtml's quirks mode (scrollingElement already reads it): BackCompat or CSS1Compat |
| `defaultView` | ro attr | html | modern |  | 23 |  |  |
| `timeline` | ro attr | web-animations | absent |  | 20 | d | Web Animations |
| `writeln` | method | html | classic |  | 16 |  |  |
| `close` | method | html | classic, constant |  | 14 |  | does nothing, as HTML defines it when no open() made a parser |
| `fullscreenEnabled` | ro attr | fullscreen | absent |  | 11 | d | Fullscreen (an honest refusal, fullscreenEnabled false, is possible earlier) |
| `doctype` | ro attr | dom | classic |  | 10 |  |  |
| `documentURI` | ro attr | dom | absent |  | 7 | a P1-Da | the document's address: options.url for the window's, about:blank for documents of their own |
| `dir` | attr | html | absent |  | 6 | a P1-Da | reflects the html element's dir (the classic reflection already exists for elements) |
| `adoptNode` | method | dom | absent |  | 5 | a P1-Db | clone into, or move into, this document: documents of their own already share the arena and adopt on append |
| `queryCommandSupported` | method | html | absent |  | 4 | d | editing (contenteditable) |
| `createNodeIterator` | method | dom | absent |  | 2 | b P4a | NodeIterator's pre-removal steps need RELAYOUT 5a's mutation-time callback |
| `lastElementChild` | ro attr | dom | classic |  | 1 |  |  |
| `(named properties)` | getter | html | absent |  |  | b P3c | named access on Document (forms, images, embeds, objects by name): storage.c's legacy-platform-object pattern |
| `activeViewTransition` | ro attr | css-view-transitions | absent |  |  | d | view transitions |
| `alinkColor` | attr | html | absent |  |  | e | obsolete body-attribute reflections; trivial if ever asked |
| `append` | method | dom | classic |  |  |  |  |
| `ariaNotify` | method | wai-aria | absent |  |  | e | no assistive technology on os64 |
| `bgColor` | attr | html | absent |  |  | e | obsolete body-attribute reflections; trivial if ever asked |
| `characterSet` | ro attr | dom | absent |  |  | a P1-Da | libhtml's document charset, the Encoding Standard's name |
| `charset` | ro attr | dom | absent |  |  | a P1-Da | libhtml's document charset, the Encoding Standard's name |
| `childElementCount` | ro attr | dom | classic |  |  |  |  |
| `children` | ro attr | dom | classic |  |  |  |  |
| `contentType` | ro attr | dom | absent |  |  | a P1-Da | text/html, or the type implementation.c gave a document of its own |
| `createCDATASection` | method | dom | absent |  |  | b P4a | libhtml node kinds it does not have |
| `createProcessingInstruction` | method | dom | absent |  |  | b P4a | libhtml node kinds it does not have |
| `customElementRegistry` | ro attr | dom | absent |  |  | c Tier 1 | scoped custom element registries, with custom elements |
| `fgColor` | attr | html | absent |  |  | e | obsolete body-attribute reflections; trivial if ever asked |
| `firstElementChild` | ro attr | dom | classic |  |  |  |  |
| `fullscreen` | ro attr | fullscreen | absent |  |  | d | Fullscreen (an honest refusal, fullscreenEnabled false, is possible earlier) |
| `getElementsByTagNameNS` | method | dom | absent |  |  | a P1-Db | implementation.c's validate-and-extract and its three namespaces; any other namespace is refused as createDocument refuses it |
| `inputEncoding` | ro attr | dom | absent |  |  | a P1-Da | libhtml's document charset, the Encoding Standard's name |
| `lastModified` | ro attr | html | absent |  |  | a P1-Da | the response's Last-Modified, else now, in the page's zone (yonder has the header) |
| `linkColor` | attr | html | absent |  |  | e | obsolete body-attribute reflections; trivial if ever asked |
| `parseHTML` | method | html | absent |  |  | d | static: the Sanitizer API; static, on the Document interface object |
| `prepend` | method | dom | classic |  |  |  |  |
| `queryCommandEnabled` | method | html | absent |  |  | d | editing (contenteditable) |
| `queryCommandIndeterm` | method | html | absent |  |  | d | editing (contenteditable) |
| `queryCommandState` | method | html | absent |  |  | d | editing (contenteditable) |
| `queryCommandValue` | method | html | absent |  |  | d | editing (contenteditable) |
| `replaceChildren` | method | dom | classic |  |  |  |  |
| `rootElement` | ro attr | SVG | absent |  |  | a P1-Da | the svg root element, else null |
| `vlinkColor` | attr | html | absent |  |  | e | obsolete body-attribute reflections; trivial if ever asked |

Event handler IDL attributes (`on<type>`), grouped where they share a status and a reason. A number after a name is its scan pages (and, on window, `M` its MISSING files); Chrome percentages are given where a counter exists.

| handlers | yonder | class | note |
|---|---|---|---|
| `onblur`, `onchange`, `onclick` 3, `onfocus`, `oninput`, `onkeydown` 30, `onkeypress`, `onkeyup`, `onload`, `onmousedown` 4, `onmousemove` 3, `onmouseout`, `onmouseover`, `onmouseup`, `onreset`, `onsubmit` | classic |  |  |
| `onresize`, `onscroll` | modern |  |  |
| `onprerenderingchange` C0.40 | absent | a P1-Da | false: yonder never prerenders |
| `onreadystatechange` 113 | absent | a P1-Da | loading/interactive/complete, moved by the host's own DOMContentLoaded and load dispatches; readystatechange fired just before each |
| `onvisibilitychange` 7 | absent | a P1-Da | 'visible'/false; visibilitychange when the desktop minimizes the window is (b) and can wait |
| `onerror` | absent | b P2a | a handler slot on document; the error events it hears are elements' load errors (P2 with the window's) |
| `onauxclick`, `oncontextmenu`, `ondblclick`, `onmouseenter`, `onmouseleave`, `onscrollend` C27.02 W, `onwheel` | absent | b P5 | events yonder's input path does not fire today |
| `onformdata`, `oninvalid`, `onselect` | absent | b P5 | form events |
| `onsecuritypolicyviolation` | absent | c Tier 1 | CSP |
| `onabort`, `oncanplay`, `oncanplaythrough`, `oncuechange`, `ondurationchange`, `onemptied`, `onended`, `onloadeddata`, `onloadedmetadata`, `onloadstart`, `onpause`, `onplay`, `onplaying`, `onprogress`, `onratechange`, `onseeked`, `onseeking`, `onstalled`, `onsuspend`, `ontimeupdate`, `onvolumechange`, `onwaiting` | absent | d | media events: the media arc (Tier 2) |
| `onanimationcancel`, `onanimationend`, `onanimationiteration`, `onanimationstart`, `ontransitioncancel`, `ontransitionend`, `ontransitionrun`, `ontransitionstart`, `onwebkitanimationend`, `onwebkitanimationiteration`, `onwebkitanimationstart`, `onwebkittransitionend` | absent | d | CSS animation and transition events: a page waiting for transitionend stalls if the handler exists and the event never comes |
| `onbeforeinput` | absent | d | Input Events level 2 over the text editor |
| `onbeforematch`, `onbeforetoggle`, `oncancel`, `onclose`, `oncommand`, `onslotchange`, `ontoggle` | absent | d | element features yonder lacks: details/popover/dialog/invoker commands/hidden=until-found/shadow DOM |
| `oncontextlost`, `oncontextrestored` | absent | d | canvas: no slice plans it |
| `oncopy`, `oncut`, `onpaste` | absent | d | clipboard events |
| `ondrag`, `ondragend`, `ondragenter`, `ondragleave`, `ondragover`, `ondragstart`, `ondrop` | absent | d | drag and drop: the handlers are what pages detect DnD by |
| `onfullscreenchange`, `onfullscreenerror` | absent | d | Fullscreen (an honest refusal, fullscreenEnabled false, is possible earlier) |
| `ongotpointercapture`, `onlostpointercapture`, `onpointercancel`, `onpointerdown`, `onpointerenter`, `onpointerleave`, `onpointermove`, `onpointerout`, `onpointerover`, `onpointerrawupdate`, `onpointerup` | absent | d | Pointer Events: exposing the handlers without firing pointer events makes pages that detect them stop listening for mouse events |
| `onpointerlockchange`, `onpointerlockerror` | absent | d | pointer lock (kernel mouse capture) |
| `onselectionchange`, `onselectstart` | absent | d | Selection and Range |
| `onbeforexrselect` | absent | e | WebXR |
| `onfreeze`, `onresume` | absent | e | Chromium-only page lifecycle |
| `ontouchcancel`, `ontouchend` 1, `ontouchmove` 1, `ontouchstart` | absent | e | as on window: presence reads as a touch device |

## Appendix C — In the IDL, shipped by no engine

By BCD 8.1.7 (no statement of support without a flag in Chrome, Firefox or Safari, desktop or mobile, or no BCD entry at all). Left out of every count.

- **Window**: `navigate` (css-nav), `portalHost` (portals), `minimize` (window-management), `maximize` (window-management), `restore` (window-management), `onsnapchanged` (css-scroll-snap-2), `onsnapchanging` (css-scroll-snap-2), `onfencedtreeclick` (fenced-frame), `oninstallresult` (install-element), `onportalactivate` (portals)
- **Document**: `namedFlows` (css-regions), `measureElement` (font-metrics-api), `measureText` (font-metrics-api), `permissionsPolicy` (permissions-policy), `modelContext` (webmcp), `getBoxQuads` (cssom-view), `convertQuadFromNode` (cssom-view), `convertRectFromNode` (cssom-view), `convertPointFromNode` (cssom-view), `onsnapchanged` (css-scroll-snap-2), `onsnapchanging` (css-scroll-snap-2), `onfencedtreeclick` (fenced-frame), `oninstallresult` (install-element)

## Appendix D — Interface objects on window

The interfaces and namespaces webref's IDL exposes on Window (1,072 names, aliases and legacy factory functions such as `Image` included), with BCD's shipping status and the probe's presence. The ECMAScript built-ins the engine puts on the global (`Array`, `Promise` and the rest; the pinned engine build has no `Intl`) are not WebIDL and are not listed. Ranked first: every absent shipped interface that the corpus's MISSING tally names or whose constructor Chrome counts (`V8Window_<Name>_ConstructorGetter`).

Present (39, plus `console`, which yonder's script host installs through libjs): `Attr`, `CSSStyleDeclaration`, `CharacterData`, `DOMImplementation`, `Document`, `DocumentFragment`, `DocumentType`, `Element`, `Event` (classic), `HTMLAnchorElement`, `HTMLButtonElement`, `HTMLElement`, `HTMLFormElement`, `HTMLImageElement`, `HTMLInputElement`, `HTMLLinkElement`, `HTMLScriptElement`, `HTMLSelectElement`, `HTMLTextAreaElement`, `HashChangeEvent`, `Image` (classic), `MediaQueryList`, `MediaQueryListEvent`, `NamedNodeMap`, `Node`, `Performance`, `PerformanceEntry`, `PerformanceMark`, `PerformanceMeasure`, `PerformanceNavigation`, `PerformanceTiming`, `PopStateEvent`, `Screen`, `ScreenOrientation`, `Storage`, `TextDecoder`, `TextEncoder`, `URL`, `URLSearchParams`.

| interface | spec | MISSING files/asks | Chrome % (constructor read) |
|---|---|---|---|
| `XMLHttpRequest` | xhr | 85/189 |  |
| `MutationObserver` | dom | 74/123 |  |
| `NodeList` | dom | 32/41 |  |
| `CustomEvent` | dom | 31/31 |  |
| `CSS` | cssom | 20/20 |  |
| `MessageChannel` | html | 19/36 |  |
| `PerformanceObserver` | performance-timeline | 17/26 |  |
| `PointerEvent` | pointerevents | 17/17 |  |
| `CSSRuleList` | cssom | 16/21 |  |
| `DOMException` | webidl | 16/24 |  |
| `DOMRectList` | geometry | 16/21 |  |
| `DOMStringList` | html | 16/21 |  |
| `DOMTokenList` | dom | 16/25 |  |
| `DataTransferItemList` | html | 16/21 |  |
| `FileList` | FileAPI | 16/21 |  |
| `HTMLAllCollection` | html | 16/21 |  |
| `HTMLCollection` | dom | 16/21 |  |
| `MediaList` | cssom | 16/21 |  |
| `MimeTypeArray` | html | 16/21 |  |
| `Plugin` | html | 16/21 |  |
| `PluginArray` | html | 16/21 |  |
| `PromiseRejectionEvent` | html | 16/25 |  |
| `SVGLengthList` | SVG | 16/21 |  |
| `SVGNumberList` | SVG | 16/21 |  |
| `SVGPointList` | SVG | 16/21 |  |
| `SVGStringList` | SVG | 16/21 |  |
| `SVGTransformList` | SVG | 16/21 |  |
| `SourceBufferList` | media-source | 16/21 |  |
| `StyleSheetList` | cssom | 16/21 |  |
| `TextTrackCueList` | html | 16/21 |  |
| `TextTrackList` | html | 16/23 |  |
| `TouchList` | touch-events | 16/21 |  |
| `Blob` | FileAPI | 13/19 |  |
| `AbortController` | dom | 12/14 |  |
| `IntersectionObserver` | intersection-observer | 12/16 |  |
| `FormData` | xhr | 11/20 |  |
| `Location` | html | 11/11 |  |
| `ReadableStream` | streams | 11/11 |  |
| `Request` | fetch | 11/11 |  |
| `DOMParser` | html | 9/13 |  |
| `Response` | fetch | 9/9 |  |
| `HTMLIFrameElement` | html | 8/8 |  |
| `EventTarget` | dom | 6/8 |  |
| `File` | FileAPI | 6/12 |  |
| `Window` | html | 5/5 |  |
| `BroadcastChannel` | html | 4/4 |  |
| `HTMLCanvasElement` | html | 4/4 |  |
| `SVGElement` | SVG | 4/4 |  |
| `HTMLEmbedElement` | html | 3/3 |  |
| `HTMLMediaElement` | html | 3/3 |  |
| `Headers` | fetch | 3/6 |  |
| `Notification` | notifications | 3/3 |  |
| `ResizeObserver` | resize-observer | 3/3 |  |
| `WebGLRenderingContext` | webgl1 | 3/3 |  |
| `WebSocket` | websockets | 3/14 |  |
| `Worker` | html | 3/3 |  |
| `AudioTrackList` | html | 2/2 |  |
| `CSSStyleSheet` | cssom | 2/2 |  |
| `ChannelMergerNode` | webaudio | 2/2 |  |
| `EventSource` | html | 2/2 |  |
| `FileReader` | FileAPI | 2/2 |  |
| `HTMLTemplateElement` | html | 2/2 |  |
| `HTMLUnknownElement` | html | 2/2 |  |
| `IDBDatabase` | IndexedDB | 2/2 |  |
| `IDBRequest` | IndexedDB | 2/2 |  |
| `IDBTransaction` | IndexedDB | 2/2 |  |
| `MessagePort` | html | 2/2 |  |
| `OffscreenCanvas` | html | 2/2 |  |
| `Path2D` | html | 2/2 |  |
| `SVGGraphicsElement` | SVG | 2/2 |  |
| `SharedWorker` | html | 2/2 |  |
| `TextTrack` | html | 2/2 |  |
| `TextTrackCue` | html | 2/2 |  |
| `XMLHttpRequestEventTarget` | xhr | 2/2 |  |
| `XMLHttpRequestUpload` | xhr | 2/2 |  |

The other 817 shipped interfaces exposed on Window, absent from yonder and named by neither source, by spec (`console`, which yonder's host installs, left out):

- html (116): Audio, AudioTrack, BarProp, BeforeUnloadEvent, CanvasGradient, CanvasPattern, CanvasRenderingContext2D, CloseWatcher, CommandEvent, CustomElementRegistry, CustomStateSet, DOMStringMap, DataTransfer, DataTransferItem, DragEvent, ElementInternals, ErrorEvent, External, FormDataEvent, HTMLAreaElement, HTMLAudioElement, HTMLBRElement, HTMLBaseElement, HTMLBodyElement, HTMLDListElement, HTMLDataElement, HTMLDataListElement, HTMLDetailsElement, HTMLDialogElement, HTMLDirectoryElement, HTMLDivElement, HTMLFieldSetElement, HTMLFontElement, HTMLFormControlsCollection, HTMLFrameElement, HTMLFrameSetElement, HTMLHRElement, HTMLHeadElement, HTMLHeadingElement, HTMLHtmlElement, HTMLLIElement, HTMLLabelElement, HTMLLegendElement, HTMLMapElement, HTMLMarqueeElement, HTMLMenuElement, HTMLMetaElement, HTMLMeterElement, HTMLModElement, HTMLOListElement, HTMLObjectElement, HTMLOptGroupElement, HTMLOptionElement, HTMLOptionsCollection, HTMLOutputElement, HTMLParagraphElement, HTMLParamElement, HTMLPictureElement, HTMLPreElement, HTMLProgressElement, HTMLQuoteElement, HTMLSelectedContentElement, HTMLSlotElement, HTMLSourceElement, HTMLSpanElement, HTMLStyleElement, HTMLTableCaptionElement, HTMLTableCellElement, HTMLTableColElement, HTMLTableElement, HTMLTableRowElement, HTMLTableSectionElement, HTMLTimeElement, HTMLTitleElement, HTMLTrackElement, HTMLUListElement, HTMLVideoElement, History, ImageBitmap, ImageBitmapRenderingContext, ImageData, MediaError, MessageEvent, MimeType, NavigateEvent, Navigation, NavigationActivation, NavigationCurrentEntryChangeEvent, NavigationDestination, NavigationHistoryEntry, NavigationPrecommitController, NavigationTransition, Navigator, NotRestoredReasonDetails, NotRestoredReasons, OffscreenCanvasRenderingContext2D, Option, Origin, PageRevealEvent, PageSwapEvent, PageTransitionEvent, RadioNodeList, Sanitizer, StorageEvent, SubmitEvent, TextMetrics, TimeRanges, ToggleEvent, TrackEvent, UserActivation, ValidityState, VideoTrack, VideoTrackList, VisibilityStateEntry, Worklet, XMLSerializer
- SVG (52): SVGAElement, SVGAngle, SVGAnimatedAngle, SVGAnimatedBoolean, SVGAnimatedEnumeration, SVGAnimatedInteger, SVGAnimatedLength, SVGAnimatedLengthList, SVGAnimatedNumber, SVGAnimatedNumberList, SVGAnimatedPreserveAspectRatio, SVGAnimatedRect, SVGAnimatedString, SVGAnimatedTransformList, SVGCircleElement, SVGDefsElement, SVGDescElement, SVGEllipseElement, SVGForeignObjectElement, SVGGElement, SVGGeometryElement, SVGGradientElement, SVGImageElement, SVGLength, SVGLineElement, SVGLinearGradientElement, SVGMarkerElement, SVGMetadataElement, SVGNumber, SVGPathElement, SVGPatternElement, SVGPolygonElement, SVGPolylineElement, SVGPreserveAspectRatio, SVGRadialGradientElement, SVGRectElement, SVGSVGElement, SVGScriptElement, SVGStopElement, SVGStyleElement, SVGSwitchElement, SVGSymbolElement, SVGTSpanElement, SVGTextContentElement, SVGTextElement, SVGTextPathElement, SVGTextPositioningElement, SVGTitleElement, SVGTransform, SVGUnitTypes, SVGUseElement, SVGViewElement
- webaudio (36): AnalyserNode, AudioBuffer, AudioBufferSourceNode, AudioContext, AudioDestinationNode, AudioListener, AudioNode, AudioParam, AudioParamMap, AudioPlaybackStats, AudioProcessingEvent, AudioScheduledSourceNode, AudioSinkInfo, AudioWorklet, AudioWorkletNode, BaseAudioContext, BiquadFilterNode, ChannelSplitterNode, ConstantSourceNode, ConvolverNode, DelayNode, DynamicsCompressorNode, GainNode, IIRFilterNode, MediaElementAudioSourceNode, MediaStreamAudioDestinationNode, MediaStreamAudioSourceNode, MediaStreamTrackAudioSourceNode, OfflineAudioCompletionEvent, OfflineAudioContext, OscillatorNode, PannerNode, PeriodicWave, ScriptProcessorNode, StereoPannerNode, WaveShaperNode
- webgpu (36): GPU, GPUAdapter, GPUAdapterInfo, GPUBindGroup, GPUBindGroupLayout, GPUBuffer, GPUCanvasContext, GPUCommandBuffer, GPUCommandEncoder, GPUCompilationInfo, GPUCompilationMessage, GPUComputePassEncoder, GPUComputePipeline, GPUDevice, GPUDeviceLostInfo, GPUError, GPUExternalTexture, GPUInternalError, GPUOutOfMemoryError, GPUPipelineError, GPUPipelineLayout, GPUQuerySet, GPUQueue, GPURenderBundle, GPURenderBundleEncoder, GPURenderPassEncoder, GPURenderPipeline, GPUSampler, GPUShaderModule, GPUSupportedFeatures, GPUSupportedLimits, GPUTexture, GPUTextureView, GPUUncapturedErrorEvent, GPUValidationError, WGSLLanguageFeatures
- css-typed-om (28): CSSImageValue, CSSKeywordValue, CSSMathClamp, CSSMathInvert, CSSMathMax, CSSMathMin, CSSMathNegate, CSSMathProduct, CSSMathSum, CSSMathValue, CSSMatrixComponent, CSSNumericArray, CSSNumericValue, CSSPerspective, CSSRotate, CSSScale, CSSSkew, CSSSkewX, CSSSkewY, CSSStyleValue, CSSTransformComponent, CSSTransformValue, CSSTranslate, CSSUnitValue, CSSUnparsedValue, CSSVariableReferenceValue, StylePropertyMap, StylePropertyMapReadOnly
- filter-effects (27): SVGComponentTransferFunctionElement, SVGFEBlendElement, SVGFEColorMatrixElement, SVGFEComponentTransferElement, SVGFECompositeElement, SVGFEConvolveMatrixElement, SVGFEDiffuseLightingElement, SVGFEDisplacementMapElement, SVGFEDistantLightElement, SVGFEDropShadowElement, SVGFEFloodElement, SVGFEFuncAElement, SVGFEFuncBElement, SVGFEFuncGElement, SVGFEFuncRElement, SVGFEGaussianBlurElement, SVGFEImageElement, SVGFEMergeElement, SVGFEMergeNodeElement, SVGFEMorphologyElement, SVGFEOffsetElement, SVGFEPointLightElement, SVGFESpecularLightingElement, SVGFESpotLightElement, SVGFETileElement, SVGFETurbulenceElement, SVGFilterElement
- webrtc (21): RTCCertificate, RTCDTMFSender, RTCDTMFToneChangeEvent, RTCDataChannel, RTCDataChannelEvent, RTCDtlsTransport, RTCError, RTCErrorEvent, RTCIceCandidate, RTCIceCandidatePair, RTCIceTransport, RTCPeerConnection, RTCPeerConnectionIceErrorEvent, RTCPeerConnectionIceEvent, RTCRtpReceiver, RTCRtpSender, RTCRtpTransceiver, RTCSctpTransport, RTCSessionDescription, RTCStatsReport, RTCTrackEvent
- webxr (21): XRBoundedReferenceSpace, XRFrame, XRInputSource, XRInputSourceArray, XRInputSourceEvent, XRInputSourcesChangeEvent, XRLayer, XRPose, XRReferenceSpace, XRReferenceSpaceEvent, XRRenderState, XRRigidTransform, XRSession, XRSessionEvent, XRSpace, XRSystem, XRView, XRViewerPose, XRViewport, XRVisibilityMaskChangeEvent, XRWebGLLayer
- dom (17): AbortSignal, AbstractRange, CDATASection, Comment, MutationRecord, NodeIterator, ProcessingInstruction, Range, ShadowRoot, StaticRange, Text, TreeWalker, XMLDocument, XPathEvaluator, XPathExpression, XPathResult, XSLTProcessor
- speech-api (13): SpeechGrammarList, SpeechRecognition, SpeechRecognitionAlternative, SpeechRecognitionErrorEvent, SpeechRecognitionEvent, SpeechRecognitionPhrase, SpeechRecognitionResult, SpeechRecognitionResultList, SpeechSynthesis, SpeechSynthesisErrorEvent, SpeechSynthesisEvent, SpeechSynthesisUtterance, SpeechSynthesisVoice
- usb (13): USB, USBAlternateInterface, USBConfiguration, USBConnectionEvent, USBDevice, USBEndpoint, USBInTransferResult, USBInterface, USBIsochronousInTransferPacket, USBIsochronousInTransferResult, USBIsochronousOutTransferPacket, USBIsochronousOutTransferResult, USBOutTransferResult
- streams (12): ByteLengthQueuingStrategy, CountQueuingStrategy, ReadableByteStreamController, ReadableStreamBYOBReader, ReadableStreamBYOBRequest, ReadableStreamDefaultController, ReadableStreamDefaultReader, TransformStream, TransformStreamDefaultController, WritableStream, WritableStreamDefaultController, WritableStreamDefaultWriter
- webcodecs (12): AudioData, AudioDecoder, AudioEncoder, EncodedAudioChunk, EncodedVideoChunk, ImageDecoder, ImageTrack, ImageTrackList, VideoColorSpace, VideoDecoder, VideoEncoder, VideoFrame
- webgl1 (11): WebGLActiveInfo, WebGLBuffer, WebGLContextEvent, WebGLFramebuffer, WebGLObject, WebGLProgram, WebGLRenderbuffer, WebGLShader, WebGLShaderPrecisionFormat, WebGLTexture, WebGLUniformLocation
- cssom (10): CSSGroupingRule, CSSImportRule, CSSMarginRule, CSSNamespaceRule, CSSPageDescriptors, CSSPageRule, CSSRule, CSSStyleProperties, CSSStyleRule, StyleSheet
- webxrlayers (10): XRCompositionLayer, XRCubeLayer, XRCylinderLayer, XREquirectLayer, XRLayerEvent, XRProjectionLayer, XRQuadLayer, XRSubImage, XRWebGLBinding, XRWebGLSubImage
- IndexedDB (9): IDBCursor, IDBCursorWithValue, IDBFactory, IDBIndex, IDBKeyRange, IDBObjectStore, IDBOpenDBRequest, IDBRecord, IDBVersionChangeEvent
- geometry (9): DOMMatrix, DOMMatrixReadOnly, DOMPoint, DOMPointReadOnly, DOMQuad, DOMRect, DOMRectReadOnly, SVGPoint, SVGRect
- bluetooth (8): Bluetooth, BluetoothCharacteristicProperties, BluetoothDevice, BluetoothRemoteGATTCharacteristic, BluetoothRemoteGATTDescriptor, BluetoothRemoteGATTServer, BluetoothRemoteGATTService, BluetoothUUID
- presentation-api (8): Presentation, PresentationAvailability, PresentationConnection, PresentationConnectionAvailableEvent, PresentationConnectionCloseEvent, PresentationConnectionList, PresentationReceiver, PresentationRequest
- webmidi (8): MIDIAccess, MIDIConnectionEvent, MIDIInput, MIDIInputMap, MIDIMessageEvent, MIDIOutput, MIDIOutputMap, MIDIPort
- webtransport (8): WebTransport, WebTransportBidirectionalStream, WebTransportDatagramDuplexStream, WebTransportDatagramsWritable, WebTransportError, WebTransportReceiveStream, WebTransportSendGroup, WebTransportSendStream
- mediacapture-streams (7): InputDeviceInfo, MediaDeviceInfo, MediaDevices, MediaStream, MediaStreamTrack, MediaStreamTrackEvent, OverconstrainedError
- svg-animations (7): SVGAnimateElement, SVGAnimateMotionElement, SVGAnimateTransformElement, SVGAnimationElement, SVGMPathElement, SVGSetElement, TimeEvent
- encrypted-media (6): MediaEncryptedEvent, MediaKeyMessageEvent, MediaKeySession, MediaKeyStatusMap, MediaKeySystemAccess, MediaKeys
- media-source (6): BufferedChangeEvent, ManagedMediaSource, ManagedSourceBuffer, MediaSource, MediaSourceHandle, SourceBuffer
- service-workers (6): Cache, CacheStorage, NavigationPreloadManager, ServiceWorker, ServiceWorkerContainer, ServiceWorkerRegistration
- trusted-types (6): TrustedHTML, TrustedHTMLParserOptions, TrustedScript, TrustedScriptURL, TrustedTypePolicy, TrustedTypePolicyFactory
- uievents (6): CompositionEvent, FocusEvent, InputEvent, KeyboardEvent, TextEvent, UIEvent
- web-animations (6): Animation, AnimationEffect, AnimationPlaybackEvent, AnimationTimeline, DocumentTimeline, KeyframeEffect
- webgl2 (6): WebGL2RenderingContext, WebGLQuery, WebGLSampler, WebGLSync, WebGLTransformFeedback, WebGLVertexArrayObject
- css-fonts (5): CSSFontFaceDescriptors, CSSFontFaceRule, CSSFontFeatureValuesMap, CSSFontFeatureValuesRule, CSSFontPaletteValuesRule
- edit-context (5): CharacterBoundsUpdateEvent, EditContext, TextFormat, TextFormatUpdateEvent, TextUpdateEvent
- entries-api (5): FileSystem, FileSystemDirectoryEntry, FileSystemDirectoryReader, FileSystemEntry, FileSystemFileEntry
- mediacapture-extensions (5): HTMLCameraElement, HTMLMicrophoneElement, HTMLUserMediaElement, MediaStreamTrackAudioStats, MediaStreamTrackVideoStats
- webxr-hit-test (5): XRHitTestResult, XRHitTestSource, XRRay, XRTransientInputHitTestResult, XRTransientInputHitTestSource
- clipboard-apis (4): Clipboard, ClipboardChangeEvent, ClipboardEvent, ClipboardItem
- credential-management (4): Credential, CredentialsContainer, FederatedCredential, PasswordCredential
- fs (4): FileSystemDirectoryHandle, FileSystemFileHandle, FileSystemHandle, FileSystemWritableFileStream
- gamepad (4): Gamepad, GamepadButton, GamepadEvent, GamepadHapticActuator
- geolocation (4): Geolocation, GeolocationCoordinates, GeolocationPosition, GeolocationPositionError
- hid (4): HID, HIDConnectionEvent, HIDDevice, HIDInputReportEvent
- orientation-event (4): DeviceMotionEvent, DeviceMotionEventAcceleration, DeviceMotionEventRotationRate, DeviceOrientationEvent
- payment-request (4): PaymentMethodChangeEvent, PaymentRequest, PaymentRequestUpdateEvent, PaymentResponse
- scheduling-apis (4): Scheduler, TaskController, TaskPriorityChangeEvent, TaskSignal
- web-nfc (4): NDEFMessage, NDEFReader, NDEFReadingEvent, NDEFRecord
- webauthn (4): AuthenticatorAssertionResponse, AuthenticatorAttestationResponse, AuthenticatorResponse, PublicKeyCredential
- accelerometer (3): Accelerometer, GravitySensor, LinearAccelerationSensor
- background-fetch (3): BackgroundFetchManager, BackgroundFetchRecord, BackgroundFetchRegistration
- cookiestore (3): CookieChangeEvent, CookieStore, CookieStoreManager
- css-animations (3): AnimationEvent, CSSKeyframeRule, CSSKeyframesRule
- css-conditional (3): CSSConditionRule, CSSMediaRule, CSSSupportsRule
- css-font-loading (3): FontFace, FontFaceSet, FontFaceSetLoadEvent
- css-mixins (3): CSSFunctionDeclarations, CSSFunctionDescriptors, CSSFunctionRule
- css-view-transitions (3): CSSViewTransitionRule, ViewTransition, ViewTransitionTypeSet
- fedcm (3): IdentityCredential, IdentityCredentialError, IdentityProvider
- fenced-frame (3): Fence, FencedFrameConfig, HTMLFencedFrameElement
- mediasession (3): ChapterInformation, MediaMetadata, MediaSession
- orientation-sensor (3): AbsoluteOrientationSensor, OrientationSensor, RelativeOrientationSensor
- push-api (3): PushManager, PushSubscription, PushSubscriptionOptions
- webcrypto (3): Crypto, CryptoKey, SubtleCrypto
- webrtc-encoded-transform (3): RTCEncodedAudioFrame, RTCEncodedVideoFrame, RTCRtpScriptTransform
- webxr-depth-sensing (3): XRCPUDepthInformation, XRDepthInformation, XRWebGLDepthInformation
- webxr-hand-input (3): XRHand, XRJointPose, XRJointSpace
- anchors (2): XRAnchor, XRAnchorSet
- compression (2): CompressionStream, DecompressionStream
- compute-pressure (2): PressureObserver, PressureRecord
- contact-picker (2): ContactAddress, ContactsManager
- css-anchor-position (2): CSSPositionTryDescriptors, CSSPositionTryRule
- css-cascade (2): CSSLayerBlockRule, CSSLayerStatementRule
- css-highlight-api (2): Highlight, HighlightRegistry
- css-masking (2): SVGClipPathElement, SVGMaskElement
- css-transitions-2 (2): CSSStartingStyleRule, CSSTransition
- cssom-view (2): CaretPosition, VisualViewport
- document-picture-in-picture (2): DocumentPictureInPicture, DocumentPictureInPictureEvent
- encoding (2): TextDecoderStream, TextEncoderStream
- event-timing (2): EventCounts, PerformanceEventTiming
- generic-sensor (2): Sensor, SensorErrorEvent
- ink-enhancement (2): DelegatedInkTrailPresenter, Ink
- layout-instability (2): LayoutShift, LayoutShiftAttribution
- long-animation-frames (2): PerformanceLongAnimationFrameTiming, PerformanceScriptTiming
- mediacapture-region (2): BrowserCaptureMediaStreamTrack, CropTarget
- mediastream-recording (2): BlobEvent, MediaRecorder
- navigation-timing (2): PerformanceNavigationTiming, PerformanceTimingConfidence
- observable (2): Observable, Subscriber
- permissions (2): PermissionStatus, Permissions
- picture-in-picture (2): PictureInPictureEvent, PictureInPictureWindow
- pointerevents (2): MouseEvent, WheelEvent
- resize-observer (2): ResizeObserverEntry, ResizeObserverSize
- screen-wake-lock (2): WakeLock, WakeLockSentinel
- scroll-animations (2): ScrollTimeline, ViewTimeline
- serial (2): Serial, SerialPort
- soft-navigations (2): InteractionContentfulPaint, PerformanceSoftNavigation
- storage-buckets (2): StorageBucket, StorageBucketManager
- touch-events (2): Touch, TouchEvent
- translation-api (2): LanguageDetector, Translator
- web-app-launch (2): LaunchParams, LaunchQueue
- web-locks (2): Lock, LockManager
- webvtt (2): VTTCue, VTTRegion
- webxr-lighting-estimation (2): XRLightEstimate, XRLightProbe
- webxr-plane-detection (2): XRPlane, XRPlaneSet
- window-controls-overlay (2): WindowControlsOverlay, WindowControlsOverlayGeometryChangeEvent
- window-management (2): ScreenDetailed, ScreenDetails
- writing-assistance-apis (2): CreateMonitor, Summarizer
- CSP (1): SecurityPolicyViolationEvent
- animation-triggers (1): AnimationTrigger
- audio-session (1): AudioSession
- background-sync (1): SyncManager
- battery-status (1): BatteryManager
- content-index (1): ContentIndex
- crash-reporting (1): CrashReportContext
- css-animations-2 (1): CSSAnimation
- css-cascade-6 (1): CSSScopeRule
- css-conditional-5 (1): CSSContainerRule
- css-contain (1): ContentVisibilityAutoStateChangeEvent
- css-counter-styles (1): CSSCounterStyleRule
- css-nesting (1): CSSNestedDeclarations
- css-properties-values-api (1): CSSPropertyRule
- css-pseudo (1): CSSPseudoElement
- css-scroll-snap-2 (1): SnapEvent
- css-transitions (1): TransitionEvent
- css-viewport (1): Viewport
- datacue (1): DataCue
- deprecation-reporting (1): DeprecationReportBody
- device-posture (1): DevicePosture
- digital-credentials (1): DigitalCredential
- element-capture (1): RestrictionTarget
- element-timing (1): PerformanceElementTiming
- eyedropper-api (1): EyeDropper
- fetch (1): FetchLaterResult
- gamepad-extensions (1): GamepadPose
- geolocation-element (1): HTMLGeolocationElement
- gyroscope (1): Gyroscope
- idle-detection (1): IdleDetector
- image-capture (1): ImageCapture
- input-device-capabilities (1): InputDeviceCapabilities
- intersection-observer (1): IntersectionObserverEntry
- intervention-reporting (1): InterventionReportBody
- is-input-pending (1): Scheduling
- js-self-profiling (1): Profiler
- keyboard-lock (1): Keyboard
- keyboard-map (1): KeyboardLayoutMap
- largest-contentful-paint (1): LargestContentfulPaint
- local-font-access (1): FontData
- login-status (1): NavigatorLogin
- managed-configuration (1): NavigatorManagedData
- manifest-incubations (1): BeforeInstallPromptEvent
- mathml-core (1): MathMLElement
- media-capabilities (1): MediaCapabilities
- media-playback-quality (1): VideoPlaybackQuality
- mediacapture-fromelement (1): CanvasCaptureMediaStreamTrack
- model-element (1): HTMLModelElement
- netinfo (1): NetworkInformation
- paint-timing (1): PerformancePaintTiming
- performance-timeline (1): PerformanceObserverEntryList
- periodic-background-sync (1): PeriodicSyncManager
- permissions-policy (1): PermissionsPolicy
- prompt-api (1): LanguageModel
- raw-camera-access (1): XRCamera
- remote-playback (1): RemotePlayback
- reporting (1): ReportingObserver
- requestidlecallback (1): IdleDeadline
- resource-timing (1): PerformanceResourceTiming
- saa-non-cookie-storage (1): StorageAccessHandle
- screen-capture (1): CaptureController
- scroll-to-text-fragment (1): FragmentDirective
- selection-api (1): Selection
- server-timing (1): PerformanceServerTiming
- shape-detection-api (1): BarcodeDetector
- storage (1): StorageManager
- ua-client-hints (1): NavigatorUAData
- url (1): webkitURL
- urlpattern (1): URLPattern
- virtual-keyboard (1): VirtualKeyboard
- web-based-payment-handler (1): PaymentManager
- web-otp (1): OTPCredential
- webidl (1): QuotaExceededError
- webrtc-identity (1): RTCIdentityAssertion
- websockets (1): CloseEvent
- xhr (1): ProgressEvent

Exposed on Window in the IDL but shipped by no engine BCD records, or not in BCD (140): `AmbientLightSensor`, `AnimationNodeList`, `Attribution`, `AttributionAggregationServices`, `AutofillEvent`, `Baseline`, `BluetoothAdvertisingEvent`, `BluetoothDataFilter`, `BluetoothLEScan`, `BluetoothLEScanFilter`, `BluetoothLEScanPermissionResult`, `BluetoothManufacturerDataFilter`, `BluetoothManufacturerDataMap`, `BluetoothPermissionResult`, `BluetoothServiceDataFilter`, `BluetoothServiceDataMap`, `CSSApplyBlockRule`, `CSSApplyStatementRule`, `CSSColor`, `CSSColorProfileRule`, `CSSColorValue`, `CSSContentsBlockRule`, `CSSContentsStatementRule`, `CSSCustomMediaRule`, `CSSHSL`, `CSSHWB`, `CSSLCH`, `CSSLab`, `CSSMixinRule`, `CSSOKLCH`, `CSSOKLab`, `CSSParserAtRule`, `CSSParserBlock`, `CSSParserDeclaration`, `CSSParserFunction`, `CSSParserQualifiedRule`, `CSSParserRule`, `CSSParserValue`, `CSSRGB`, `CSSSupportsConditionRule`, `CaptureActionEvent`, `CapturedMouseEvent`, `CrossOriginStorageManager`, `DeviceChangeEvent`, `DigitalGoodsService`, `EpubReadingSystem`, `Exception`, `FaceDetector`, `Font`, `FontFaceFeatures`, `FontFacePalette`, `FontFacePalettes`, `FontFaceVariationAxis`, `FontFaceVariations`, `FontMetrics`, `GPUBufferUsage`, `GPUColorWrite`, `GPUMapMode`, `GPUShaderStage`, `GPUTextureUsage`, `Global`, `GroupEffect`, `HTMLInstallElement`, `HTMLPortalElement`, `HandwritingDrawing`, `HandwritingRecognizer`, `HandwritingStroke`, `InstallResultEvent`, `Instance`, `LanguageModelParams`, `LanguageModelToolCall`, `LanguageModelToolError`, `LanguageModelToolSuccess`, `ML`, `MLContext`, `MLGraph`, `MLGraphBuilder`, `MLOperand`, `MLTensor`, `Magnetometer`, `MathMLAnchorElement`, `MediaStreamTrackHandle`, `Memory`, `ModelContext`, `Module`, `MulticastController`, `NamedFlow`, `NamedFlowMap`, `NavigationEvent`, `NodeFilter`, `PerformanceContainerTiming`, `PermissionsPolicyViolationReportBody`, `PointerTimeline`, `PortalActivateEvent`, `PortalHost`, `PreferenceManager`, `PreferenceObject`, `Proofreader`, `ProximitySensor`, `RTCRtpSFrameDecryptor`, `RTCRtpSFrameEncryptor`, `Rewriter`, `SFrameDecryptorStream`, `SFrameEncryptorStream`, `SFrameTransformErrorEvent`, `SVGMatrix`, `ScriptingPolicyReportBody`, `SequenceEffect`, `ShadowAnimation`, `SmartCardConnection`, `SmartCardContext`, `SmartCardError`, `SmartCardResourceManager`, `SpeechGrammar`, `TCPServerSocket`, `TCPSocket`, `Table`, `Tag`, `TestUtils`, `TextDetector`, `ToolActivatedEvent`, `ToolCancelEvent`, `UDPSocket`, `USBPermissionResult`, `UncalibratedMagnetometer`, `ValueEvent`, `WebAssembly`, `WebKitCSSMatrix`, `WebTransportWriter`, `WorkletAnimation`, `Writer`, `XRBody`, `XRBodySpace`, `XRGPUBinding`, `XRGPUDepthInformation`, `XRGPUSubImage`, `XRMediaBinding`, `XRMesh`, `XRMeshSet`, `XRPermissionStatus`.

## Appendix E — The probe

Run by the DOM host (one case added in a scratch copy, as § How the binding was measured says), with `modern` on and then off, and the result read back from `globalThis.__probe`. A later phase re-measures with the same script.

```js
(function () {
  function repr(val) {
    var t = typeof val;
    if (val === null) return ['object', 'null'];
    if (t === 'function') return [t, 'len=' + val.length + ' name=' + val.name];
    if (t === 'object') {
      var s;
      try { s = Object.prototype.toString.call(val); } catch (e) { s = '?'; }
      return [t, s];
    }
    if (t === 'string') return [t, JSON.stringify(val.slice(0, 50))];
    return [t, String(val)];
  }
  // One line per own property at every level of obj's chain above
  // Object.prototype: label, level, the level's toStringTag, name,
  // data(W)/accessor(G,S), and what reading it through obj answers.
  function dump(obj, label) {
    var out = [], level = 0;
    for (var p = obj; p && p !== Object.prototype; p = Object.getPrototypeOf(p), level++) {
      var tag = '';
      try { tag = p[Symbol.toStringTag] || ''; } catch (e) {}
      Object.getOwnPropertyNames(p).forEach(function (n) {
        var d = Object.getOwnPropertyDescriptor(p, n);
        var kind = (d.get || d.set) ? 'acc' + (d.get ? 'G' : '') + (d.set ? 'S' : '')
                                    : 'data' + (d.writable ? 'W' : '');
        var r;
        try { r = repr(obj[n]); } catch (e) { r = ['throws', String(e).slice(0, 60)]; }
        out.push([label, level, tag, n, kind, r[0], r[1].replace(/[\t\n]/g, ' ')].join('\t'));
      });
    }
    return out.join('\n');
  }
  globalThis.__probe = [dump(globalThis, 'window'), dump(document, 'document'),
                        dump(location, 'location'), dump(history, 'history'),
                        typeof navigator !== 'undefined' ? dump(navigator, 'navigator') : ''].join('\n');
})();
```
