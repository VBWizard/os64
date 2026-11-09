# GITHUB_WALLS.md — what GitHub's pages need from yonder, in order

*Fable 5.1, 2026-11-04. Written for Opus (and any session Chris hands a
slice to) after a day of reading three GitHub pages the way a debugger
would: the record yonder wrote, the page's console captured over ssh, the
live chunks the stack named, and one QEMU run with a stack-printing
console. Chris asked for the list, the reasoning, and my answer to "the
next wall is Shadow DOM".*

## 1. The three pages, and what actually kills each

The pages: the pull-request LIST (`/pulls`), the REPO HOME (`/`), and a
PULL REQUEST (`/pull/<n>`), all on Chris's own repository. Three
different React apps, one wall.

**The pull-request list.** Shows for two seconds, then "Unable to load
page. This page is unavailable due to a system error." That text is
React Router's error boundary. The record cannot show the cause: a
caught render error is `console.error`, which is yonder's STDOUT, not the
record, and the global-miss table only counts (`MISSING global Intl (2)`).
The console (captured by `yonder <url> > file` over ssh) says
`React Router caught the following error during render TypeError: cannot
read property 'AS' of undefined`; a QEMU run with the stack printed puts
it in the route classifier called from the dashboard layout.

The mechanism, which repeats on every GitHub React page and is worth
learning once: the pull-requests ENTRYPOINTS module is one webpack module
a megabyte long. Its export getters (`n.d(t,{...})`) and its function
declarations are HOISTED above its body; its `var X = n("id")` bindings
are assigned in body order. The body constructs
`new Intl.DateTimeFormat(void 0, {dateStyle:"medium", timeStyle:"short"})`
at MODULE SCOPE, half a megabyte before it assigns the stacks-route
binding. libjs (QuickJS) has no Intl, so the module's init aborts there;
webpack's production require keeps the half-made cache entry; the first
throw is swallowed by route preloading; every later require gets a module
whose functions exist and whose later bindings are `undefined`. At render
the classifier reads `.AS` off one of them. The error-state component is
a function declaration in the same broken module, which is why the
fallback can paint at all.

**The repo home.** Shows for half a second, then "It looks like your
network connection is down." The network was fine: all 125 requests on
the record came back 200/204. The boundary's test is literally
`error instanceof Error && !navigator.onLine`; libdom's Navigator had no
`onLine` that day (Slice B's second PR paid it), so the check read
`undefined` and every render error on this app was labelled "offline".
The error it mislabels is Intl again, by a
shorter road: this app is Vite-built ES modules; the banners chunk
constructs `new Intl.DateTimeFormat("en-US", {dateStyle:"long",
timeZone:"UTC"})` at module scope and is STATICALLY imported by the
layout, repo-layout and global-nav chunks. In ESM a module whose top
level throws takes every importer down with it, no partial exports.

**A pull request.** Text shows, goes blank after ten seconds, "Unable to
load page" after minutes. Four things, in the order they happen:
1. The server-rendered conversation paints.
2. The entrypoints module requires the relative-date module 59 KB into
   its body; that module's first statements are three
   `new Intl.DateTimeFormat(...)`; the entrypoints module aborts with 294
   of its bindings unassigned. (This time the record names it: `FAILED
   script pull-requests-*.js: 'Intl' is not defined, at qB`.)
3. React mounts the conversation's four partials (Body, Sidebar,
   Timeline, TimelineActions) over the server HTML — THAT is the text
   disappearing. They suspend on data, and the fetched-fragment loads
   behind them fail and retry with backoff — that is the long blank.
4. The render reaches a component on an unassigned binding (four
   candidates read `.w`: the commit-sha link, the merge-conflicts panel,
   the pull-request row); React Router logs `cannot read property 'w' of
   undefined`; the boundary paints.

The fragment loads fail on something new: GitHub inserts every piece of
fetched server HTML with `template.innerHTML = html;
document.importNode(template.content, true)`. libdom has no
`importNode`. 68 deaths on one visit (twelve `include-fragment`s, the
rails partials, the timeline's deferred items). They do not cause the
error; they stretch the blank from seconds to minutes, and once Intl is
in they are the next thing between the person and the timeline.

## 2. The list, ranked, with what each costs the person

| # | Need | What it costs today | Pages | Booked? | Size |
|---|---|---|---|---|---|
| 1 | **Intl** (DateTimeFormat, NumberFormat, RelativeTimeFormat, ListFormat, Collator, PluralRules; `Locale` probed) | Every GitHub React app dies before drawing. Any React route that formats a date at module scope, on any site | all three | **no** | M, libjs |
| 2 | **`document.importNode`** (`adoptNode` beside it) | The PR page's deferred timeline and all fetched fragments; 68 deaths a visit | PR | **no** | S, libdom |
| 3 | **`navigator.onLine` = true** | Every error on the repo app misreported as "offline" | repo home | yes (Navigator members, S) | XS |
| 4 | **IntersectionObserver** (#460) and the **`CSS` namespace** (#464) | On the deploys GitHub served this afternoon, the repo app's entry and the behaviors bundle die on these at module level. The morning's deploy did not. GitHub ships new bundles every few hours | all | in PRs | merge |
| 5 | **`Event.composedPath`** | GitHub's delegated pointer and keyboard handlers | all | yes (XS) | XS |
| 6 | **`attachShadow` + `ShadowRoot`; `attachInternals` + `ElementInternals`** | GitHub's invoker polyfill wraps both off `HTMLElement.prototype` and calls the saved one; with neither present the wrapper calls `undefined`. Costs the relative-time element (timestamps stay raw instead of "3 hours ago") and whatever element definitions follow the throw in the github-elements bundle (unmeasured) | all | ElementInternals yes (M); attachShadow no | L (full) / M (minimal, § 4) |
| 7 | **TreeWalker + NodeFilter** | The focus walker on an animation frame; Lit sites elsewhere | PR | yes (S) | S |
| 8 | **`document.links`** | One idle link-scan task in the repo app | list | no | XS |
| 9 | Telemetry only, no visible effect: PerformanceObserver family (216 corpus records, 2 deaths), Worker (Datadog), indexedDB (query-cache persistence, guarded), MessageChannel, BroadcastChannel, ClipboardItem, HTMLDialogElement and ToggleEvent probes, trustedTypes, the collector's CORS-preflight refusal under yonder's own UA | all | partly | — |

Corpus breadth (the 10/25 pull, 879 records; "missing" = bare lookups
that failed, "died" = scripts that threw on it unhandled; a module-scope
throw caught by lazy loading, GitHub's case, is NOT in the died column):

| Global | Records missing it | Hosts | Scripts that died | Hosts |
|---|---|---|---|---|
| Intl | 47 | 23 | 12 | 6 |
| IntersectionObserver | 77 | 25 | 11 | 8 |
| CSS | 101 | 10 | 87 | 1 |
| PerformanceObserver | 216 | 23 | 2 | 1 |
| ResizeObserver | 30 | 12 | — | — |
| ShadowRoot | 18 | 5 | 5 | 1 |

## 3. Why Intl is the wall, and why no engine swap moves it

Intl is the odd one out on the list: it is part of the LANGUAGE standard
(ECMA-402), in every browser since 2012, so library authors call it at
module scope with no guard. That makes it the one hole where a page dies
before it has drawn anything.

Chris asked about QuickJS-ng. Checked: quickjs-ng's own docs say Intl is
unlikely ever to be added, for size; stock quickjs-ng has none today. The
one QuickJS variant with Intl (a React Native package) reads its locale
data from the host OS's CLDR database, which os64 does not have. Bellard's
tree, which libjs pins at 2026-06-04, is active and already carries what
were NG's headline advantages (WeakRef, FinalizationRegistry, iterator
helpers, column numbers in errors). Swapping would cost re-porting all 18
libjs patches against a tree two years diverged and revalidating
TEARDOWN.md and CONTRACT.md, for no Intl. Ruled with Chris: stay on
Bellard's QuickJS; Intl is ours.

**Shape I lean to:** self-hosted JavaScript installed at realm start, as
Streams S1 was built, with en-US data and the system timezone, sized to
what pages call. The subset is drawn from DEMAND, not the spec: census the
corpus's kept scripts for which constructors and which option bags they
pass (the three pages alone use DateTimeFormat with `dateStyle`/
`timeStyle`, the `year/month/day/hour/minute/timeZoneName` bag, a fixed
`timeZone:"UTC"`, `resolvedOptions().timeZone` and `.hourCycle`;
NumberFormat with `notation:"compact"`, `style:"currency"`,
`maximumFractionDigits`; RelativeTimeFormat with `numeric:"auto"`;
ListFormat `conjunction`; Collator `numeric, sensitivity:"base"`;
`"ListFormat" in Intl` and `"Intl" in window` probes). Design doc first —
this is libjs platform work, and the honest line between "formats like
Chrome" and "formats plausibly" is the doc's question. Fable reviews it.

## 4. On "the next wall is Shadow DOM"

Opus's trace of the github-elements and relative-time errors is exactly
right, I traced the same wrapper on 11/01 and again today. Real Shadow
DOM is the real fix, it is large, and a design doc before code is the
right call; I will review it. `composedPath` as a small slice first is
also right.

Where I push back is the word NEXT. Today's three pages say the React app
dies on Intl on every one of them, and Intl is what empties the page and
paints the error. What the shadow pair costs today is the relative-time
element (raw timestamps) plus whatever element definitions sit after the
throw in the github-elements bundle, which nobody has measured. Real, but
cosmetic beside items 1 and 2. Order: Intl, importNode, onLine, the
merges, composedPath, then the shadow pair.

Two asks for the Shadow DOM doc when it comes:

- **Ask what the consumers need before building encapsulation.**
  relative-time attaches a shadow root only to put its text in it. A
  first cut where `attachShadow` returns a ShadowRoot whose children lay
  out and paint as the element's own — no slots, no style scoping — would
  let GitHub's elements construct and render at a fraction of full Shadow
  DOM. Whether that is an honest cut or a trap is the doc's main question,
  and the corpus answers it: count who calls `attachShadow` and who then
  uses `<slot>`, `:host`, `::part` or `adoptedStyleSheets`.
- **`attachInternals` is a separate, smaller feature** (ElementInternals,
  form association, already booked at M) and should not be held hostage to
  Shadow DOM. GitHub's wrapper only needs it to EXIST on the prototype;
  the elements that call it want `setFormValue` and friends.

## 5. Slices, and what must stay with one developer

Chris may run several sessions. These groupings are what belongs
together; a slice split across two sessions is a review nobody can do.

**Slice A — Intl (one developer, libjs, design doc first).** The census
(§ 3), the doc, the self-hosted subset, its host test, and the DEBTS row
it pays. It touches libjs's realm start and the bytecode cache's view of
what is "the runtime"; nothing else on this list touches libjs. Do not
split the census from the build: the census IS the spec.

**Slice B — the small libdom holes (one developer, one PR or two).**
`document.importNode` + `adoptNode` (the clone-with-ownership door;
`cloneNode`'s machinery plus the ownerDocument move, and the custom
element upgrade an imported tree gets); `navigator.onLine` and the rest
of the cheap Navigator constants in its DEBTS row; `document.links` (an
HTMLCollection, `anchors` and `forms` are its siblings in the spec and
cost nothing more); `Event.composedPath` (with no shadow trees it IS the
propagation path, and must be rebuilt the day Slice D lands — say so in
its comment); and **an Error's stack in the console** (§ 6: yonder writes
an Error as its message only, where other browsers print `.stack` too;
Chris put it in this slice, 2026-11-04). These share no state with each
other, but they share the reviewer and they are each too small to be a
round of their own.

**Slice B's last task — TreeWalker, NodeIterator and NodeFilter** (moved
here from Slice D by Chris, 2026-11-04, after a corpus count). In the P5's
11/04 records `createTreeWalker` kills a social news site's feed (14
records) and one other site (1 record), and `NodeFilter` GitHub's focus walker
(12); across both corpus pulls 28 of 194 hosts call `createTreeWalker`, and
Lit's template clone is `importNode` followed by a walker, with no shadow
root involved. DOM § 6's walkers traverse one node tree: a shadow root is
no child of its host, so a walker never enters one, and shadow DOM does
not reshape them. Its own PR, after the others.

**Slice C — the merges (#460, #461, #462, #463, #464).** Already done,
Codex clean, waiting on Chris's P5 test. No developer needed; just the
order (each stacks on the last).

**Slice D — Shadow DOM + ElementInternals (one developer, design doc
first, Fable review).** The doc answers § 4's cut question with corpus
numbers before anyone writes `attachShadow`. ElementInternals can be its
own PR inside the slice, landing first. (TreeWalker and NodeIterator were
here, for a walker designed with shadow boundaries in mind; they moved to
the end of Slice B, which says why.)

**Slice E — PerformanceObserver as an honest stub (optional, XS).**
Accepts observers, `supportedEntryTypes` says what exists (`mark`,
`measure`, `navigation`), never delivers what libdom does not record.
Quiets 23 hosts of noise and the two unhandled rejections on every GitHub
page. Could ride with B. Built: it delivers the marks and measures libdom
records as they are made, and the navigation entry once the window's load
has run (LIBDOM.md, the PerformanceObserver paragraph).

## 6. Harness notes that cost an afternoon, so they cost nobody another

- **A caught render error is on STDOUT, not in the record.** On the P5:
  `yonder <url> > /home/tmp/x.txt` over ssh (the window appears on
  Chris's desktop; `--exit-when-loaded` leaves once quiet, which on these
  pages is BEFORE the React partials mount, so for the blank-page
  question run without it and let Chris close the window).
- **An Error's stack is not printed.** yonder's console writes an Error
  as its message only. A twelve-line libjs patch that sends `.stack`
  through `os64_serial_log` turned the pulls page into one QEMU run;
  reverted, not committed. Other browsers print the stack; I lean to
  making it a real change (Slice B could carry it).
- **`/QEMU GUI Boot` in limine.conf has `ROOT=` the FAT partition's
  GUID** (the lifeboat: no `/etc/certs`, so every https load reports "TLS
  trust could not open"). Booked in DEBTS § Build system, XS, low
  priority by Chris's call. `/RAMDisk Boot (GUI)`
  roots on ext2 and mounts the data disk's `/home`.
- **yonder's defaults in a fresh image are scripts OFF, diagnostics off,
  UA `yonder/1.0 (os64)`.** `/home/yonder.conf`: `scripts = on`,
  `modern = on`, `script_seconds = 60`, `diagnostics = /home/yonder-diag`
  (parent must exist). gui.conf `start` lines take no arguments; type
  into the GUI `husk` window. Under TCG the 9 MB module compile alone is
  47 s.
- **GitHub serves different bundles to different user agents and
  deploys within hours.** Download the chunk the stack NAMES; yesterday's
  asset list is already stale. The webpack module ids are stable across
  a deploy's chunks, the chunk names are not.
- **Cross-check a module-scope throw by position:** find the module
  header (`,ID(e,t,n){n.d(t,{`), the offending `new X(` offset, and the
  `var Y=n("id")` offset the render reads; if the throw is earlier, every
  later binding is undefined. That is the whole diagnosis of a "cannot
  read property of undefined" inside a webpack production bundle.
