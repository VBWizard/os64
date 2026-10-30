# MODERN_WEB.md — The Modern Web, which is yonder 2.0

*The campaign doc for the arc after yonder 1.0. Written by Fable on
2026-10-19, the morning Chris and Fable sized the risk, from the 10/07
agreement, the census on the P5, and the engine recon run that morning.
Read with [BROWSER.md](../../../BROWSER.md) (the campaign constitution,
§ Where the campaign stands), [DOM.md](DOM.md) (the binding and the event
loop), [JAVASCRIPT.md](JAVASCRIPT.md) (the engine port), and
[YONDER_HONEY_DO.md](YONDER_HONEY_DO.md) (the 1.0 batch that runs between the
arcs). Fable holds this document and the hard designs it names; Opus builds
the assigned slices, Quinn owns SVG slice 6; Chris rules.*

## What this arc is for

yonder is to be Chris's daily driver. The ruling that shapes everything here
was made on 2026-10-07 and is restated so nobody rebuilds the old method by
habit: **the modern web is built from the standards, not from the next site
that fails.** "A consumer finds it does not work" was the right method for
the old web, where a page asks for little and a gap is one line in a page
file. A modern page assumes a floor of forty things before its first line of
script runs, and finding them one broken site at a time means forty broken
sites. So the floor is built WHOLE, tier by tier, each piece proved against
the test suite the browsers themselves run, and Chris's own sites are the
ACCEPTANCE, not the search.

**The bar, in Chris's words (2026-10-19):** the worst few percent of pages
may misbehave in yonder, where "few" is low single digits. Fifty percent may
not. Every slice is judged against that sentence.

## The engine, measured

The arc's first stated risk was speed: QuickJS is an interpreter, Chrome
compiles to machine code, and os64 is C-only by ruling so no C++ engine is
ever an option. Before planning anything, the risk was measured
(2026-10-19; the harness is reproducible, see the session's recon notes in
the model memory). The same benchmark source, shaped like page work (JSON,
regular expressions, object churn, function calls, string building, a
virtual-DOM diff, closures), under four engines:

| Engine | Machine | Time |
|---|---|---|
| V8 with its JIT (what Chrome runs) | Ryzen 9 3900X, Linux | 1.66 s |
| V8 with the JIT off (Chrome's own "jitless" mode, which ships) | same | 2.89 s |
| QuickJS, the tree's pinned 2026-06-04, built `-O2` | same | 3.96 s |
| QuickJS under os64's own `js` runner | **the P5** (Ryzen 5 6600H) | **4.65 s** |

Three facts fall out, and they set the arc's shape:

1. **The tax is 2.8x, not the 10 to 50x first feared.** The gap is 24x on
   one column only, raw function calls (0.45 s against 0.02 s). JSON,
   regular expressions and strings are near parity because they are C in
   both engines. os64's port and libos64's heap add about 12% over the
   engine on Linux, so the engine is the cost and the port is not.
2. **Language coverage is not a risk.** The real bundles of GitHub (6.6 MB,
   all ES modules), Google (1.35 MB), YouTube (10.8 MB), PNC (2.4 MB), APGFCU
   (1 MB), danlegt, CNN, Wikipedia and Hacker News were fetched with a
   Chrome user agent: 23 MB of script, and QuickJS parsed every byte with
   zero syntax failures, React's 2.1 MB bundle included.
3. **Compile time is where the worst case lives, and a cache fixes it.**
   QuickJS compiles at 6 to 20 MB/s; YouTube's bundle costs 4 to 5 s on the
   P5 before a byte runs, Google's 0.17 s, PNC's 0.05 s. QuickJS reads its
   own bytecode 30x faster than it compiles (56 ms against 1,653 ms for
   YouTube), so a per-bundle bytecode cache makes the second visit cheap.
   The bytecode is three times the SOURCE size (33 MB for YouTube), so the
   cache needs a cap and probably compression, which the tree has.

Where the tax will be felt: single-page applications that re-render on
every scroll or keystroke live in the function-call column, and those will
feel sluggish rather than broken. That is the low-single-digit X, and it is
accepted.

**Never read an engine timing off the QEMU harness.** The harness runs
QEMU without hardware acceleration (the box has a KVM device, but the
harness does not use it), so the guest emulates the CPU instruction by
instruction and took the full 60 s budget on the benchmark the P5 finished
in 4.65 s. The harness proves behaviour; the P5 measures speed.

The second stated risk stands unmeasured: a bank's bot detection may refuse
an honest unknown browser whatever it implements. Reddit and Amazon refused
the recon's plain fetch on the user agent alone. No promise is made on
pnc.com or apgfcu.com until their scripts can run and the refusal, if it
comes, can be read.

## The rules that keep the other 99% safe

Chris's fear, stated plainly on 2026-10-19: a slice tested against one site
merges and the other 99% of sites stop working. He does not want a
long-lived branch (and said why), so the protection is built into how a
slice is made, not where it lands. Four rules.

1. **Whole API or none.** A page feature-detects: "if `localStorage`
   exists, take the modern path." A `localStorage` that exists and
   misbehaves is WORSE than none, because every page that used to take the
   fallback now takes the broken path. So a slice ships an interface
   complete to its standard, or does not ship the name at all. Half an
   object never lands behind the kill switch either; the switch is for
   finished surface that turns out to hurt, not for drafts.
2. **The oracle, before the sites.** Every slice names its web-platform-tests
   directory, and TWO gates read it (Quinn, 2026-10-21: a regression
   baseline alone lets an interface ship passing a tenth of its tests): a
   REQUIRED set, the tests named one by one as the interface's essential
   behaviour, every one of which passes before the interface's row joins
   the table enabled; and the baseline, every test that passes, which a
   slice cannot shrink. What a slice knowingly does not run is excluded
   BY NAME with its reason, never as a count. § The oracle, below.
3. **The corpus, before every merge.** Two instruments, because the
   P5's page files are RECORDS and not pages: each of the 1,601 under
   `/home/tmp/yonder/backup` names what a real page asked for and what
   failed (YONDER_DIAGNOSTICS.md), and nothing on the P5 keeps a page's
   bytes (the cache keeps sheets and pictures, never pages; CACHE.md § 2).
   So the census TALLY (slice 0b) ranks the records' `MISSING` names and
   says, for a slice's surface, how many records asked for it: the
   before-and-after that needs no replay, and the number that orders the
   slices. And the REPLAY CORPUS (slice 0b too) is what a merge is judged
   against: a `keep_pages` setting makes yonder write, beside each page
   file, the page's bytes and every script and sheet it fetched, so a
   kept page can be fed to the headless binding (the host harness that
   runs yonder's own loop and scripts without a screen,
   `tools/test_yonder_scripts_host.c`) with its sub-resources answered
   from disk and no network at all, and its record written again. The
   corpus number is how many kept pages replay clean, and the diff tool
   names every page whose record gained a `FAILED` or a `MISSING` line
   since the last run. A PR that lowers the count, or adds a `FAILED`
   anywhere, does not merge until it says why. The corpus grows by Chris
   browsing with the setting on; until it holds his sites, the tally is
   the number and the PR says so.
4. **One kill switch, not many.** `modern = on | off` in `yonder.conf`,
   with a box in Settings that writes it: it turns every global, element
   and CSS property this arc added off at once, so Chris can keep browsing
   on the P5 when a site regresses and flip it back when the fix lands. It
   is one table (the globals are installed from a table at runtime
   creation, and the CSS side has the same shape). **Absent the setting it
   is ON; present, the setting is obeyed** (Chris, 2026-10-19). A slice's
   surface joins the table only once it has passed its oracle and its
   corpus, so what the switch guards is finished work that turned out to
   hurt, never a draft. It is the only switch: per-feature flags multiply
   into combinations nobody tests. The existing `Run page scripts` switch
   (DOM.md ruling 1) stays the outer gate; a page that runs no script
   cannot miss `fetch`.

And the branch rule, for the record: every slice is a PR to `userland`, as
every slice has been. "2.0" is the name of the campaign, not of a branch.

## The oracle: web-platform-tests

An oracle is the thing that says whether we got it right without a person
judging the glass. libhtml had html5lib-tests, the parse fixtures every
browser runs, and parsing was not where the pain was BECAUSE of that. TLS
had RFC 8448's byte-for-byte transcripts, and TLS 1.3 shipped in a day. The
web platform's oracle is **web-platform-tests** (WPT): tens of thousands of
small HTML pages, maintained jointly by the Chrome, Firefox and Safari
teams and run by each of them nightly, each page asserting one behaviour
and reporting through one shared script, `testharness.js`.

WPT is built to be run by an unknown browser. The seam is a file called
`testharnessreport.js`, which the suite leaves empty for the vendor to fill:
whatever it does with the results is how that browser collects them. Ours
writes each test's name and verdict through the headless binding to the
host, the way the diagnostics page files are written today. So the runner
is:

- the suite at a pinned commit, OUTSIDE the tree: `tools/wpt_fetch.sh`
  checks out only the directories the slices name (a sparse, shallow
  clone; the whole suite is gigabytes) into `.vendor/wpt`, which
  `.gitignore` already covers. The pin is one line in that script; a
  slice that needs a newer directory moves the pin for everyone and says
  so in its PR. The tree carries no copy of the suite, only what it
  wrote against it;
- a host program on the headless binding that loads one WPT page from
  disk, answers the page's sub-resource fetches from the checkout (a WPT
  page loads `/resources/testharness.js` and its helpers by absolute
  path, so the runner answers script and sheet jobs whose address is
  under the checkout, and nothing else; the `fetch` cases come with
  slice 2, which teaches the runner to serve two origins), runs the loop
  until the harness reports done or a time budget ends, and prints one
  line per test. The results leave the page through the document itself:
  our `testharnessreport.js` writes each test's name and verdict into an
  element it appends when the harness reports completion, and the runner
  reads that element out of the tree once the loop settles. No new seam,
  and the same tree walk the fixtures already use;
- a per-directory expectation file, checked in under `tools/wpt/`,
  naming the tests that pass, so a slice's PR shows the directory's count
  going up and a regression shows as a named test going down. Browsers
  do exactly this, and call them expectations.

Three honest limits. WPT cannot judge fidelity (does the page LOOK right)
or feel (is it pleasant at the P5's speed); those remain Chris's, on the
P5, beside Chrome. WPT's `reftests` compare a page against a reference
page pixel for pixel, which the headless binding could do in a later slice
but slice 0 does not. And WPT assumes `fetch` and modules for some of its
own plumbing, so slices 1 and 2 run the subset that needs neither, and the
runner grows with the platform it tests.

## The tiers

Restated from 2026-10-07 with the census's numbers (the census is
MODERN_WEB_CENSUS.md, on a local branch until Chris publishes it; its
numbers are quoted here so this file stands alone).

**Tier 0: what every modern page assumes.** Storage (`localStorage`,
`sessionStorage`), `performance`, `URL` and `URLSearchParams`,
`TextEncoder`/`TextDecoder`, `screen`, `getComputedStyle`, the scroll and
history family (`scrollTo`, `scrollY`, `innerWidth`/`innerHeight`,
`history.pushState`/`replaceState` and `popstate`), `matchMedia`,
`fetch` and `XMLHttpRequest` with origins and CORS, ES modules,
`requestAnimationFrame`, `MutationObserver`, `FormData`, `AbortController`,
inline SVG, and the CSS still in Pile 3's queue. The census put inline SVG
first by sites (every Google and GitHub icon, 140 asks), modules second
(GitHub is all modules, so none of its scripts run today), `localStorage`
third (MediaWiki gates its whole module loader on it, and every transform
demo Chris tried needs it), logical properties fourth (1,700 declarations,
mostly aliases), `performance` fifth (Google's inline scripts die on it).

**Tier 1: banking.** `iframe` with `postMessage` (one window owns one page
today, so a document inside a document is the arc's biggest structural
change and gets its own design document before any code), Content Security
Policy, referrer and cookie rules across redirect chains, `SameSite` with
the public-suffix list (booked in YONDER.md). pnc.com and apgfcu.com are
the acceptance, with the bot-detection caveat above.

**Tier 2: media.** `video` and `audio` elements, an audio driver, software
codecs, Media Source Extensions. **YouTube lives here and is a separate
arc**, not a slice of this one.

## The slices, in order

Each slice names what it builds, its oracle directory, its acceptance
sites, and who holds the design. "Opus" means Opus designs and builds it
under the 2026-10-11 ruling (a userland-only design is its builder's);
"Fable design" means the design document is Fable's before Opus builds. Every
slice ends with the corpus number and the P5 beside Chrome.

**Slice 0a: the runner and the switch.** The WPT runner above, the
`modern` switch and its table (§ How the slices run side by side has the
table's shape), and the interface objects already written and parked on
`opus/arc-interface-objects` (`Element`, `HTMLElement` and the constructor
names on `window`) landed through the table as its first row. Oracle: WPT
`dom/nodes/` for the interface objects, and the runner proving itself on
`dom/` tests that already pass. Acceptance: web.archive.org's player and
DuckDuckGo's base bundle stop dying on `Element.prototype`. Opus.

**Slice 0b: the tally and the replay corpus.** The census tally over the
P5's page files; `keep_pages` in `yonder.conf`, which makes yonder write
a page's bytes and its fetched scripts and sheets beside its page file
(rule 3); the replay runner on the headless binding that feeds a kept
page back with its sub-resources answered from disk; and the diff tool
that names every page whose record got worse. Oracle: the runner replays
the fixture pages the scripted-page host already carries and writes the
records those cases expect. Acceptance: the P5 browsing with the setting
on for an evening yields a corpus that replays on the host. Opus, beside
0a: the two touch different files.

**Slice 1a: storage and the small globals.** `localStorage` and
`sessionStorage` (per origin, on disk under the profile folder,
`<profile>/storage/<origin>` — YONDER_HONEY_DO.md § Where yonder's files
live names the folder and its `profile =` setting, and this slice is the
setting's first consumer, since the between-arcs batch had not started
when the slices were handed out — with the 5 MB quota browsers use and a
Settings line to clear them), `performance` (`now`, `timing` as the
challenge scripts read it, `mark`/`measure`), `URL`/`URLSearchParams`,
`TextEncoder`/`TextDecoder`, `screen`, `devicePixelRatio`. Everything
here lives in libdom and files; nothing needs the loop or layout. Oracle:
WPT `webstorage/`, `url/`, `encoding/`, `hr-time/`. Acceptance:
osdev.wiki's banner and menus (MediaWiki's `isCompatible` gate), the
transform demos. Opus.

**Slice 1b: the window's view, the scroll and the history.**
`getComputedStyle` over libflow's computed values (a provider in the
shape of the geometry one, because the answer is the laid-out box's),
`scrollTo`/`scrollBy`/`scrollX`/`scrollY`/`innerWidth`/`innerHeight`/
`outerWidth`/`outerHeight` (the engine already scrolls the page for
fragment links), `history.pushState`/`replaceState`/`state`/`popstate`,
`matchMedia` over the media queries libgarb already evaluates, with
`change` events when the view resizes. Everything here needs the window:
its view, its loop, its back-and-forward list. Oracle: WPT
`cssom-view/` (the scroll and size members), `html/browsers/history/`,
`css/cssom/getComputedStyle-*`. Acceptance: grokipedia's contents links,
danlegt's `matchMedia` asks. Opus, beside 1a.

**Slice 2: fetch.** `fetch` with `Headers`, `Request`, `Response`, the body
readers (`text`, `json`, `arrayBuffer`, `blob` as bytes), `AbortController`;
`XMLHttpRequest` over the same machinery; origins, the same-origin rule,
and CORS (preflights, the allow headers, credentials mode) as the Fetch
standard states them; `FormData`. It rides on libfetch (the stream, the
cookie hooks, the cache) and the promise pump DOM.md already runs. Oracle:
WPT `fetch/api/` and `xhr/` (the subset the runner can serve; the
cross-origin cases need the runner to serve two origins, which it gains
here). Acceptance: danlegt's chat, and every page the census shows asking
for `fetch` (danlegt on every page, warpbox, w3schools). Opus, with Fable
reading the CORS section before code because a wrong CORS answer is a
credential leak, not a layout bug.

**Slice 3: ES modules.** `<script type=module>`, `import`/`export`, the
module graph fetched in parallel and evaluated in order, dynamic
`import()`, `import.meta.url`, `defer` and `async` ordering as the HTML
standard states it, `nomodule` honoured. QuickJS already parses and links
modules, but libjs evaluates only classic scripts today
(`JS_EVAL_TYPE_GLOBAL` is the one flag `runtime.c` passes), so the slice
adds the engine's module entry to libjs's contract: a module evaluation
whose imports are resolved by a host loader the runtime is given, under
the same budget, outcome and teardown rules as a classic run. yonder's
loader fetches the graph through the script jobs it has and evaluates in
the standard's order. Oracle: WPT
`html/semantics/scripting-1/the-script-element/module/`. Acceptance:
GitHub's page runs its scripts at all. Opus.

**Slice 4: CSS batch A, the cheap thousands.** Logical properties
(`inline-size`, `padding-inline`, the `margin-block` family and the rest,
as aliases for horizontal-tb, which is every page in the census), `cursor`
(509 asks; the pointer shapes themselves wait on the honey-do's I-beam
work in the kernel), `aspect-ratio` (danlegt's Scratch shop), `object-fit`
and `object-position`, `letter-spacing`, `text-overflow`, `word-break` and
`overflow-wrap`, `outline`. Oracle: WPT `css/css-logical/`,
`css/css-sizing/aspect-ratio/`, `css/css-images/object-fit*`,
`css/css-text/`. Acceptance: the census's CSS table, re-tallied. Opus.

**Slice 5: incremental relayout, then the frame.** D13 from DOM.md moved
INTO this arc and ahead of `requestAnimationFrame` on 2026-10-07, because
rAF on a page that relays out whole is danlegt at 16 ms per frame forever.
A layout that reuses the boxes a change did not touch; then
`requestAnimationFrame` on the loop's rendering step and
`MutationObserver` on the mutation core. Oracle: WPT
`html/webappapis/animation-frames/`, `dom/nodes/MutationObserver-*`.
Acceptance: the perf record's layouts-per-second on danlegt, unchanged
with rAF on. **Fable design**, because it changes what libflow's tree IS
between layouts: [RELAYOUT.md](RELAYOUT.md) (2026-10-21), which cuts it
into 5a (the change log in libhtml, `MutationObserver`, the instrument),
5b (the cascade updated in place, pass 1 reused), 5c (the layout memo)
and 5d (`requestAnimationFrame`).

**Slice 6: SVG.** Inline `<svg>` in the tree, and SVG as a picture
(`<img src="x.svg">`, the census's `MISSING image svg`): a renderer for
paths, the basic shapes, fills and strokes, transforms, `use`/`symbol`,
`viewBox`, gradients and the SVG properties added to libgarb. Oracle: WPT
`svg/` and CSS reftests, plus browser-reference fixtures. Acceptance:
Google's and GitHub's icons, suckless's logo, and the declared feature
coverage. [SVG.md](SVG.md), revised by Quinn on 2026-10-25 from Fable's
design, defines a separate `libsvg`, a FreeType geometry interface with
stroke expansion before transformation, and XML parsing for external SVG.
Quinn builds, with Fable reviewing the integration contracts and Chris
testing. [XML.md](XML.md) defines the in-house event parser consumed by
libhtml's tree builder. The cuts are 6a (reftests, geometry proof, XML parser
and tree/cascade integration proof), 6b (static inline core), 6c (external images, gradients and
caches), 6d (text and broader static graphics), and 6e (interaction and
motion). Schedule 6a–6c; 6d/6e remain follow-on coverage whose start is driven
by real needs. Static rendering comes first; retained geometry and explicit
invalidation support later animation. Rendering a set of icons does not
establish complete SVG compatibility.

**Slice 7: the bytecode cache.** Per bundle, keyed by the cache's own entry
identity (the response's validators), capped in bytes, compressed with
libgzip, invalidated with the response. libjs gains a compile-to-bytes
and a run-from-bytes pair over the engine's own `JS_WriteObject` and
`JS_ReadObject`, tied to the engine's build so a cache written by one
build is never read by another. The timing instrument (compile time per
script, in the page file) lands in slice 0b so this slice's
before-and-after is a number on the P5. Opus. *As built (7a, classic
`src` scripts; [BYTECODE_CACHE.md](BYTECODE_CACHE.md)):* a companion
file beside the script's cache entry, keyed by the decoded source's
digest rather than the validators, so a blob is only ever offered for
the text it came from; read only for a body the cache holds and removed
with its entry; one verb, `os64_js_run_bytecode`, not a pair; the
script's per-page name given at the read (patch 0015), since libdom reads
it back to mute another site's errors; and NOT compressed — libgzip
inflates at 68 MB/s against the engine's 600 MB/s read, so the inflate
would have cost more than it saved. 7b, modules, follows on its own path.

**Slice 8: generated content and the rest of the census's CSS.**
`::before`/`::after` with `content` (144 asks, the icon fonts' whole
mechanism), `filter` and `clip-path` basics, `mix-blend-mode`, the
`will-change` values yonder draws nothing of (read, no effect). Opus.

**Tier 1 slices**, designed when Tier 0 is done and the P5 has been used
on it for a week: `iframe` and `postMessage` (**Fable design**, its own
document), CSP, referrer and cookie rules across redirects, `SameSite` and
the public-suffix list. Custom elements (`customElements.define`, GitHub's
`react-partial` and friends) join here or the end of Tier 0 as the census
says once modules run. With modules and import maps in, the census said it
(2026-10-26, Chris's P5 records of GitHub): its bundles die on
`customElements`, then `MutationObserver` (5a), `IntersectionObserver` and
`ReadableStream`, and its loading placeholders stay where that code would
have filled them. Chris bookmarked custom elements to follow 0f.

**Browser-own, between slices (2026-10-28).** Two fixes that are not
platform surface but gate what the platform work can show on the P5, each
designed in HEAP_BINS.md and booked in DEBTS.md: the CSS parser's scratch
kept by its owner across a cascade's reads (§ Browser layout, size S,
libgarb) and size-class bins in the userland heap (§ Heap, size M, Chris's
malloc). The measurement: a 22,415-rule page spent 8 of its 11.7 s of
layout in the heap's first-fit walk, one walk per rule, and the window's
thread holds 7% of a core idle on the same walk. Order: the scratch first,
the bins when heap.c is opened; neither makes the other optional.

**Two slices the P5's pages promoted, and where they wait (Chris,
2026-10-29).** Each is a surface a real page died on, and each waits
behind what is left of Tier 0 rather than in front of it: the scratch and
the bins above, slice 5, slice 7, slice 8, then these two in this order.
A surface a page finds after this goes to the end of the list too, unless
Chris promotes it by name.

- **Range.** `Range`, `StaticRange` and `document.createRange` (DOM § 5),
  live boundary points that follow the tree on the document's listener
  (`os64_html_observe`, which MutationObserver already reads),
  `createContextualFragment`, and a range's `getBoundingClientRect` and
  `getClientRects` over 1b's geometry. Google's search box throws
  `'createRange' is not a function` as a query is typed, WPT's
  MutationObserver tests stop at it, and WINDOW_DOCUMENT.md counts it on
  91 pages. Selection stays class (d), for the window's selection model
  (YONDER_HONEY_DO.md's selection model). Oracle: WPT `dom/ranges/`.
  Opus.
- **Streams.** A `ReadableStream` a page builds itself, and bodies that
  stream, the Streams standard whole (DEBTS.md's row; a body is read whole
  today, `response.body` a one-chunk stream). A social site's sign-in
  pages die on `'ReadableStream' is not defined` before they draw, and
  GitHub's bundles name it after `MutationObserver`. A design pass before
  code, since it is the Streams standard. Oracle: WPT `streams/`. Opus.

## How the slices run side by side (2026-10-21)

Chris opened the arc on 2026-10-21 with a week's model usage to spend and
asked for the slices to be built in parallel. The slices were cut
independent on purpose, so the plan is one builder per slice, each in its
own worktree off `userland`, each ending in its own PR. What follows is
what keeps eight builders from colliding in one tree, and it is as much
design as the slices themselves.

**The order.** Slices 0a and 0b first, side by side: they touch different
files (both built 2026-10-21, PRs #327 and #326). The fan-out starts once
0a's table exists, because every one of them registers in it; a fan-out
slice that opens before 0a has merged branches from `opus/tmw-0a-runner`
and says so in its PR (DOM_BRIEFS.md's stacking rule). **Three builders at
a time, not six** (ruled 2026-10-21 after two builders spent a percent of
the five-hour usage window a minute: the weekly budget was the plan's
premise, the five-hour window is the real pacing limit, and a builder
paused by a wall mid-edit is worse than one that never started). The
first three: **1a, 1b and 4**, none of which needs fetch, each in its own
worktree off 0a's branch. The second three, when the first three's PRs
are open: **2, 3 and 7**; 3 after 2 if the window is tight, because its
inline import rides 2's transport. Then 5a and 6a, which 0a alone
unblocks, and 5b, 5c, 6b, 6c in their documents' order. Slice 8 follows 4.
Every builder reads MODERN_WEB_BRIEFS.md as it stands on
`fable/modern-web` AFTER Quinn's review (2512ba11 and later), not the
copy a 0a or 0b branch carries, which predates it.

**The table, which is the switch.** libdom gains `modern.c`: one static
table of rows, each a name and an installer, walked once at runtime
creation by `d_modern_install` when `os64_dom_options_t.modern` is set
(the default). A slice adds ONE FILE and ONE ROW: `storage.c` and
`{"localStorage", d_storage_install}`, and so on. With the option clear
the walk does not run, so the name does not exist, which is what a page's
feature detection needs to see (rule 1). The interface objects are the
first row. On the CSS side libgarb's property table marks each property
this arc adds, and a parse is told whether marked properties are known;
not known reads exactly as an unknown property does today, the
`MISSING css-property` line. yonder reads `modern` from `yonder.conf`
once at start (absent is on) and hands the one answer to both libraries
at creation; there is no global and no second switch.

**Who owns which files.** A builder edits what its row names and nothing
else without saying so in the PR; the shared files are listed so the
rebase order below resolves them rather than a merge fight.

| Slice | Owns | Touches shared |
|---|---|---|
| 0a | `tools/test_yonder_wpt_host.{c,sh}`, `tools/wpt_fetch.sh`, `tools/wpt/`, `libdom/modern.c`, the interface objects in `libdom/classic.c` | `dom.h` (the option), `yonder/settings.{c,h}` (`modern`), `libgarb/props.c` (the mark) |
| 0b | `yonder/diag.{c,h}` (bytes, kept sub-resources, compile time), `tools/test_yonder_replay_host.{c,sh}`, `tools/yonder_tally.py`, `tools/yonder_corpus_diff.py` | `yonder/settings.{c,h}` (`keep_pages`), the stream's arrival path in `yonder.c` |
| 1a | `libdom/storage.c`, `libdom/url.c`, `libdom/encoding.c`, `libdom/performance.c`, `libdom/screen.c`, `yonder/storage.c` (the files) | `yonder/settings.{c,h}` (`profile`), `modern.c` (rows) |
| 1b | `libdom/view.c` (scroll, size, `matchMedia`, the computed-style provider) | `libdom/window.c` (`history`), `yonder.c`'s view and resize paths, `modern.c` (rows) |
| 2 | `libdom/fetch.c`, `libdom/xhr.c`, `yonder/fetch_job.c`, `libway/origin.c` (origins, CORS) | `scripts.h` (the job hook), `modern.c` (rows), the WPT runner (two origins) |
| 3 | the module entry in `libjs` (`runtime.c`, `js.h`), `yonder/modules.c` (the graph) | `yonder/scripts.c` (the loader hook), `libdom/window.c` (script kind) |
| 4 | `libgarb/props.c`, `values.c` and `libflow` for the named properties | `yonder/paint.c` (`outline`, `text-overflow`) |
| 7 | `os64_js_run_bytecode` in `libjs`, `libway/cache.c` (a companion beside a script's entry) | `yonder/script_job.c`, `scripts.c`, `yonder.c` (the write job) |

Landing order, a lean Chris moves: 0a, 0b, 1a, 1b, 4, 2, 3, 7. A later
slice rebases onto what landed before it; `settings.c` and `modern.c`
gain one reader or one row per slice, which rebases clean when each slice
adds rather than edits.

**What every PR states**, so Chris can judge it without re-deriving: its
WPT directory and its count before and after (the expectation file's
diff is the proof); how many of the P5's records named its surface (the
tally); the corpus count before and after once kept pages exist; and the
P5 reading if it claims a speed. A builder commits on its own branch and
opens the PR; Chris tests before anything merges; Fable reads 0a's table
and slice 2's CORS section before those merge; a Codex round is Chris's
call per PR.

**The harness budget** (SUCCESSION.md's rules, restated because eight
builders share one box): the host harnesses prove the slice; a QEMU boot
is for what needs the wire or the disk (slice 2's fetch over slirp,
slice 1a's files under `/home`), one VM per slice at a time, under
`timeout`, on its own monitor port, killed by PID when done.

## Decisions reopened, and their state

From 2026-10-07, so they are not re-argued from scratch:

| Decision | State |
|---|---|
| D13 (incremental relayout) before `requestAnimationFrame` | ruled; slice 5 |
| The runtime's memory limit (D5b's profile) | becomes a per-page policy before Tier 1; the recon's 33 MB bytecode for one bundle says the limit is per PAGE, not per runtime |
| The 5 s per-task budget | measured after slice 0's instrument; the recon says compile of the largest bundle alone is 4 to 5 s on the P5, so the budget cannot include compile |
| The 128 KiB script stack | remeasured in D7c; unchanged until a WPT directory says otherwise |
| Scripts on by default | Chris's, after the gates; the kill switch does not change it |
| Not reopened | the engine, D4's worker/window split, the cookie jar, HTTP/1.1 |

## What this arc does not do

- **YouTube and media**: Tier 2, its own arc, after an audio driver.
- **Tabs**: a 1.0 question answered in YONDER_HONEY_DO.md (new window first;
  tabs when the arc's pages make a window heavy enough to need them).
- **Workers, service workers, WebAssembly, WebRTC, WebGL, notifications,
  push**: not asked for by the census's sites at a level that changes what
  they show. QuickJS has no WebAssembly, and that is a C-only consequence to
  state out loud if a site turns out to need it.
- **A JIT**: never. C-only is the ruling; the measurement above is why it
  is survivable.

## Ownership and review

- Fable holds this document, the hard designs it marks, and the review of
  each slice's design section before code. Opus designs the rest and
  builds all of it, from the briefs in MODERN_WEB_BRIEFS.md; Quinn reads
  this document before the fan-out starts (Chris's 2026-10-21 call) and
  builds what Chris hands her. Chris rules direction and tests before
  anything merges.
- A slice touching the kernel (the pointer shapes, a clipboard change, the
  compositor) goes to Fable's review by CLAUDE.md's "match the reviewer to
  the risk". Userland-only slices are reviewed by their owner and Chris;
  a Codex round is Chris's call per PR.
- Every PR states: the WPT directory and its count before and after, the
  corpus count before and after, and the P5 reading if the slice claims a
  speed.

## Review record

**Quinn, 2026-10-21, at `30833121`** ([MODERN_WEB_REVIEW.md](MODERN_WEB_REVIEW.md)):
eleven findings, all taken, the direction kept. Five P1: the pinned
QuickJS resolves imports even under compile-only, so modules need a
no-resolve flag and an explicit link step (slice 3, verified against
`quickjs.c`); a synchronous request through `way_fetch_whole` would be a
second, less protected transport, so sync XHR and dynamic `import()` run
the one fetch job inline (slices 2 and 3, verified against `load.c`);
per-window `localStorage` snapshots lose each other's writes, so the file
is the shared state and a window commits its own changes by read, merge,
write (slice 1a); `MutationObserver` cannot be rebuilt from a lossy
layout log, so the document gains one mutation-time callback and the
binding queues records as the standard does (RELAYOUT.md); a source
digest does not validate bytecode, so the blob carries a payload digest
and framing and a mismatch is a miss (slice 7). Six P2: a module's
top-level await outlives the drain, so evaluation has a state asked after
every turn (slice 3); the memo compared a counter every update moved, so
the cascade has a sheet generation and an update counter, and untouched
is a bit of its own beside steady (RELAYOUT.md); the replay index grows
with the features that fetch, and an unrecorded request is "incomplete",
not a failure (slice 0b); the WPT gate gains the required set and named
exclusions (rule 2, slice 0a); a standalone SVG picture owns its own
cascade (SVG.md); `pushState` fires nothing, fragment navigation and
traversal each their own events (slice 1b). The two builders already
running (0a, 0b) were told their changes the same afternoon.

## As built

### Slice 0a, as built

Built by Opus on `opus/tmw-0a-runner`, from MODERN_WEB_BRIEFS.md § Slice
0a and Quinn's finding 9 on the design review (the expectations' three
sections), which arrived while it was built.

**The runner.** `tools/wpt_fetch.sh` checks out WPT at `a699fb16` (shallow,
partial, sparse: the briefs' directories, `resources`, `common`, and the
suite's own `tools`) into `.vendor/wpt`, and builds WPT's `MANIFEST.json`
over what is checked out (about 20 s; again only when the pin or the list
moves). `tools/wpt/pages.py` turns the manifest's testharness entries under
a directory into the pages a window can load. The manifest was the brief's
first choice and it works on the sparse checkout; the filename rules the
brief gave are kept as the fallback for a box without Python, and on
`dom/nodes` the two agree on all 361 pages. `tools/test_yonder_wpt_host.c`
includes the scripted-page host whole for its fakes (its `main` behind
`YONDER_HOST_FAKES_ONLY`), and both build through
`tools/yonder_host_build.sh`, so a WPT page takes the path a network page
takes in yonder. The page is `http://wpt.test/<dir>/<page>`; scripts and
sheets under that origin come from the checkout, `testharnessreport.js`
from `tools/wpt/` (it writes the results into a `<pre
id=os64-wpt-results>` the runner reads from the tree, each test's message
included), and everything else, pictures and fonts too, fails as an
unreachable fetch. The window's clock moves 10 ms a turn once the loop is
idle, up to 60 s (180 s for a long-timeout page), and the microsecond clock
behind a script task's deadline moves 1 ms a read, so a script that never
ends is ended. Two runs of a directory print the same lines; `dom/nodes`
takes about 20 s.

**The expectations** are `tools/wpt/expected/<dir>.txt`, headed by the WPT
commit, in three sections: `[required]` (each must pass, or the run fails
naming it), `[passing]` (the baseline: a lost pass fails the run, a new one
is listed) and `[excluded]` (a page not run, with its reason). `--update`
rewrites `[passing]` alone. A page whose harness did not end OK fails
whole: a pass it reported prints as a FAIL that says so. (Since
2026-10-24, by Opus's ruling on P2a, matching wpt.fyi: a harness that
ended in ERROR on the page's own uncaught error or unhandled rejection,
which the report script tells by testharness.js's own status and
message, counts the tests the page completed; a timeout or any other
harness error still fails the page whole.) Only a page run
to a verdict may change the expectations, so `--update` writes last, and
only for a run with no fault, at least one page, and no page left unrun (a
source missing, too large or not read whole, or a file a page asked for
from the checkout not read whole, is an UNJUDGED page, whichever discovery
found it); a page name an expectation line cannot hold, a page outside the
checkout or listed twice, a directory that cannot be listed, and a missing
harness or report script stop the run before any page;
an expectation file that exists and cannot be read whole makes the run
unjudgeable too. `--modern-off` runs the
window with the switch off, to show the required tests are the slice's
own: green only when none of them passes, a LEAKED one failing the run.
`--selftest` holds the runner to `tools/wpt/selftest/` both
ways (file names and a fixture manifest) and the sections to their rules.

**What the builder decided.** `.window.js` tests are run (the brief said
not): WPT generates the same window page for them as for a `.any.js`'s
window variant, so nothing about them is out of reach. Not run, and
counted by reason on the summary: worker and shadow-realm variants, and XML
documents (`.xhtml`, `.svg`; 47 in `dom/nodes`), since yonder parses HTML.
`timeout=long` raises the runner's budget, though the harness inside yonder
still gives such a page 10 s: it reads `meta.name`, and the binding has no
`HTMLMetaElement`. The Settings box applies from the next page, not the one
on screen. The fakes are shared by inclusion, not moved: slice 0b's replay
runner can include the same file the same way.

**The table.** `userland/libdom/modern.c`: a row is `{names, install}`, the
NULL-terminated list of what it puts on window and its installer, walked by
`d_modern_install` at the end of `os64_dom_create` when
`os64_dom_options_t.modern` is set (the default). The DOM host holds the
rows to their names (`tools/test_dom_modern.inc`): window's own names with
the switch on, less those with it off, must equal the table's, so a slice's
row needs no case of its own to prove its switch. A row that leaves an
engine exception pending fails the creation (QuickJS's function
constructors swallow an out-of-memory in `length` and `name`; the full
installer sweep found one). The switch takes away the table's names and
what its rows put on them (each prototype's `constructor` and
Symbol.toStringTag, Node's constants), nothing else: the prototype chain's
levels, which this slice corrected (below), are the same with it on or off (Fable's
ruling on Codex's fourth round: the separate HTMLElement prototype fixes
the binding's structure and is not surface the arc adds, and no page
detects a feature by comparing prototypes). The DOM host holds that too.

**The first row: the interface objects**, cherry-picked from `8a6cca6d`
with Chris's authorship and then made whole to their own rule (one
interface per prototype of the binding's, or `instanceof` lies). HTMLElement
had shared Element's prototype, so an SVG element was an HTMLElement; HTML
elements now have a prototype of their own (`D_PROTO_HTML_ELEMENT`). The
anchor, script and link prototypes, which arrived after the parked commit,
get their interfaces. `Node` carries the node-type and document-position
constants on itself and its prototype, and each prototype's
`Symbol.toStringTag` is its interface's name, which is what
`Object.prototype.toString` reports. Each interface object inherits its
parent interface's, as its prototype inherits the parent prototype.
`new DocumentFragment()` constructs, and any call without `new` is refused
by the engine (a constructor-only function); `new Document()` is an Illegal
constructor. Setting an interface object's own [[Prototype]] met an engine
leak: QuickJS's `JS_SetPrototype` keeps its reference to the new prototype
when the shape copy it then needs runs out of memory, so the installer
gives each interface object a shape of its own first (`interface_parent`,
`classic.c`), which the full installer sweep proves.

**The CSS side.** `Prop`, the shorthand table and the alias table carry
`modern` (false everywhere today). `garb_read_declaration` takes a
`garb_reading_t {quirks, modern_off}`; `garb_env_t.modern_off` carries the
answer through a cascade (declarations, `@supports`, `var()`-held
properties, the skip tally); a marked longhand leaves an unmarked
shorthand's answer, and the shorthand reads no part of its value that only
the marked longhand takes, so `animation: spin` is invalid while
`animation-name` is marked and the switch is off, as it would be in an
engine without that longhand; libdom's style object has no member for a marked
property while the switch is off. `GARB_MODERN_PROBE` lets the host suites
prove the mark before anything is marked.

**yonder.** `modern = on | off` (`yonder_settings_saved_modern`; absent is
on, another word is on and said on the status line), a Settings box, the
switch captured per navigation beside `scripting` and kept by the page for
its life, handed to the script host and to the page's cascade, and a
`modern: off` fact line in the page file.

**Numbers.**

| | before | after |
|---|---|---|
| `dom/nodes` pass | 844 of 4397 | 858 of 4406 |
| harness errors (failing whole) | 69 | 67 |
| `[required]` | — | 14, all passing; none passes with `--modern-off` (a green run) |
| DOM host | 3734 / 21296 checks | 3810 / 22416, 0 failed |
| scripted-page host | 5842 | 5888, 0 failed |

The fourteen required tests are Node's eight constant tests, the two
`DocumentFragment` constructor tests, `DocumentFragment.prototype`'s
`getElementById`, `Node-cloneNode`'s `createDocumentFragment`, and the two
`getElementsByTagName('*')` tests, which read `child.ELEMENT_NODE`. The
parked shape, with no constants, had counted 905 on the runner's first
draft: Node-cloneNode's `createElement` tests passed only while
`Node.ELEMENT_NODE` was undefined, which skipped their checks of
`attributes`, which the binding lacked until Slice 0d.

Why the rest of `dom/nodes` fails, by the harness's messages: 488 read a
frame's document (`iframe`); 271 `createElementNS` and 213
`setAttributeNS`; 307 expect a `DOMException` (the binding throws named
Errors, LIBDOM.md § Budgets and errors); 282 `document.implementation`; 123
`MutationObserver` (slice 5); 94 `createProcessingInstruction`; 65
`DOMParser`; 94 CharacterData's `substringData`/`replaceData`/
`insertData`/`deleteData`; 16 `attachShadow`. None is excluded: they run
and fail, and the counts say why.

The census: of the 1601 page records now on the P5 (the brief counted
401), 181 name `Element`, `HTMLElement` or `Node` as a missing global (162
`Element`, 43 `HTMLElement` with 529 asks, 12 `Node`), and 218 name at least
one of this row's fifteen names. The interface names still missing most
often are `NodeList`, `HTMLCollection` and seven `SVG*List`s (37 records
each, one script), `HTMLIFrameElement` (23), `HTMLEmbedElement` and
`HTMLMediaElement` (21), `EventTarget` (14).

**Booked**: the interface objects for HTML elements that share
HTMLElement's prototype, and `Text`/`Comment` and their constructors, in
DEBTS.md § Classic browser scripting; the classic installers' trust in the
engine's function constructors, there too. The mutant harnesses were not
run (the briefs' rule); their anchors were checked against the changed
files.

### Slice 0b, as built

Built by Opus on `opus/tmw-0b-corpus`, from `fable/modern-web` at
`a5152d17` (`userland` 44768c2f and the arc's two docs commits), on
2026-10-21, with Quinn's finding 8 (MODERN_WEB_REVIEW.md) taken the same
afternoon. The brief is MODERN_WEB_BRIEFS.md § Slice 0b. **The contract
it is held to is REPLAY.md** (Fable, 2026-10-22, after five Codex rounds
and five cold reads found the same kinds of hole again): the grammar of a
record and an index, a replay's inputs and the versions of a kept page,
the index, what a replay reproduces and judges, one process per page, and
the proof. What follows is what was built against it.

**The census, as it stood on 2026-10-21.** The P5's records under
`/home/tmp/yonder/backup` are 1,601 page files from 268 sites, not 401
(401 is the number of lines os64's `ls` prints there, four names a line).
They were fetched one `cat` a file and tallied with
`python3 -I tools/yonder_tally.py DIR`. Read it with three things in mind:
FILES weight what was loaded often (www.bing.com is 174 of them,
danlegt.com 145), so the sites column is the fairer rank for a platform
name; a page's own globals rank high by files on one site (danlegt's
`HomeChat`, `Scratch2000` and `umami`, 142 files each); and the records span
several builds of yonder, so a name since built (`float` as a `css-value`,
before floats were laid out) still counts.

| files | sites | asks | MISSING global |
|---:|---:|---:|---|
| 466 | 37 | 46,635 | localStorage |
| 289 | 48 | 9,264 | XMLHttpRequest |
| 275 | 35 | 315 | module |
| 253 | 47 | 1,059 | performance |
| 252 | 34 | 324 | URL |
| 247 | 23 | 274 | screen |
| 246 | 29 | 479 | matchMedia |
| 232 | 43 | 546 | MutationObserver |
| 215 | 35 | 508 | fetch |
| 195 | 23 | 9,312 | ActiveXObject |
| 187 | 14 | 192 | getComputedStyle |
| 169 | 33 | 206 | requestAnimationFrame |
| 168 | 23 | 239 | setImmediate |
| 162 | 30 | 169 | Element |
| 161 | 11 | 208 | btoa |
| 146 | 39 | 229 | global |
| 144 | 23 | 241 | WebKitMutationObserver |
| 144 | 11 | 147 | TextEncoder |

(891 global names in all; the next rows are danlegt's own names.)

| files | sites | asks | MISSING css-property |
|---:|---:|---:|---|
| 919 | 129 | 49,437 | cursor |
| 865 | 110 | 27,133 | outline |
| 776 | 113 | 89,647 | content |
| 726 | 95 | 18,240 | letter-spacing |
| 717 | 86 | 13,699 | filter |
| 699 | 103 | 34,099 | transition |
| 650 | 78 | 7,857 | text-overflow |
| 627 | 78 | 8,753 | user-select |
| 585 | 72 | 22,465 | fill |
| 556 | 66 | 3,720 | overflow-wrap |
| 553 | 74 | 13,070 | outline-offset |
| 520 | 78 | 4,297 | word-break |
| 428 | 68 | 9,864 | object-fit |
| 425 | 50 | 5,915 | clip-path |
| 389 | 33 | 4,157 | stroke |
| 387 | 66 | 2,292 | word-wrap |
| 377 | 44 | 1,488 | backdrop-filter |
| 373 | 60 | 4,559 | appearance |
| 364 | 39 | 794 | scroll-behavior |
| 357 | 25 | 3,459 | margin-inline |
| 356 | 43 | 3,884 | aspect-ratio |

(377 property names in all.) The other kinds, by files: elements `svg`
496 (65 sites), `canvas` 162 (8), `iframe` 146 (41), then one site's custom
elements (GitHub's `react-partial` 53); at-rules `keyframes` 228 (48),
`property` 125 (13), `container` 116 (18); `MISSING script module` 259
(62); pictures `image svg` 198 (44), `picture-scheme data` 308 (20);
values `background-image: conic-gradient` 168 (6), `float: left` 107
(35), `clear: both` 100 (29); fonts `Arial` 209 (19), `Helvetica` 165
(10). The FAILED lines, ranked by `--failed`: a picture that would not
decode 269 files (19 sites), then the script deaths the globals above
explain: `'URL' is not defined` 155 (13), `'matchMedia' is not a function`
148 (7), `cannot convert to object` 144 (3), `'getComputedStyle' is not
defined` 143 (2), `'performance' is not defined` 133 (12), `cannot read
property of undefined` 130 (19), `'btoa' is not defined` 109 (2).

**The number a fan-out PR states** is `python3 -I tools/yonder_tally.py
DIR --surface NAME...`: how many files, and sites, name ANY of the
slice's names (on 2026-10-21, `localStorage sessionStorage` is 480 of
1,601 files, 42 of 268 sites). `--kind` narrows it to one kind; a name is
matched exactly. `--failed` ranks the FAILED lines with the pages they
were on, a picture's or a font's by its reason rather than its address.
The tool reads `*.txt` in the directories it is given through the one
reader (below). A census is not a diff: it COUNTS a record that lost
lines (what it kept is what the page asked for; partial is still asked)
and says on stderr how many records lost lines; it counts a replay's
record only when its page reached a verdict, and lists the others by
state on stderr; and a claimed name's placeholder that was never written
(`verdict: unwritten`) is damage, named, and the answer is 2, as it is for
any `*.txt` the reader cannot judge. It imports nothing outside the
standard library.

**keep_pages** (`keep_pages = on` beside `diagnostics =` in yonder.conf;
YONDER_DIAGNOSTICS.md § The page kept has the file shapes). The page's
bytes are kept in `stream_take`, the one place every byte the parser is
fed passes, so a body of any chunking and any encoding is kept exactly;
they go to a temporary file renamed into place at `stream_finish`, cut
short or not. The builder's call the brief left open: a page let go before
then keeps NOTHING and its temporary is removed with its record, rather
than left for a later write to replace, since a page without all its bytes
cannot be fed again as it was. Scripts are kept in `script_arrived` as the
decoded source the host is handed. A sheet's product is a parse, which
cannot be written, so `sheet.c` hands back, when the job says the window
keeps pages, the bytes it parsed and the charset its response named, and a
replay parses them again with the same call; the bytes are held only until
the window writes them. A fetch that failed is kept as a line saying so:
without it a replay could not tell a page's real 404 from a request the
corpus never saw. The keepers are diag.c's, beside the record they belong
to; a step that fails ends that page's keeping, is said once a run, and
leaves `kept: not whole` on the record; a page whose bytes were kept whole
says `kept: whole`, and a replay replays only such a record (or, of a
page kept before that fact existed, one whose bytes are beside it:
REPLAY.md § 3). **Each record
CLAIMS its name as the load begins** (`yonder_diag_claim`): its file is
made only where nothing is, holding `verdict: unwritten` (which every
reader takes as unjudged) until the record replaces it, and no `.html` or
`.d` may be there whether or not this record keeps its page, so two
yonders on one directory, a `.txt` deleted from beside its kept files, or
a keep-off load landing on a kept page's number, never pair a record with
another's pages, and the bytes are never renamed over a `.html` already
there. A record that can claim no name has no claim, and nothing of it is
written; the record's write proves its claimed file is still its own
first, by the nonce its claim wrote in the placeholder (`verdict:
unwritten <nonce>`) and in every record after (`claim: <nonce>`), so a
placeholder not known to be whole on disk (no memory for it, a write or
close that failed) is no claim, its name not known to be this claim's:
it is removed, or left as a file no reader takes for a page when the
unlink cannot remove it, and its number passed over.
No Settings line: there is none for `diagnostics` either.


**The record's inputs** (REPLAY.md § 3 is the table). `diag_begin`
writes them as head facts at the claim, as in force for the load: `agent`
first (`g.way.agent`), `clock` (`os64_time` in epoch milliseconds) and
`tz` (the `TZ` in force, or `<n>m`, the configured minutes east, when
there is none), then `laid out at` (the view's `<width>x<height> px`,
written again at arrival with the layout's time), and the rest of the
settings `kDiagSettings` names: `scripts`, `modern`, `zoom`,
`appearance`, `motion`, `animate` (not `animations`, which the census
already uses for how a page's animations fare) and `script seconds`.
`diag_cookies` writes `cookies` at the head, when the stream takes it
(or `open_local` makes a file's) and before the parser is fed a byte:
the string libway's script door, `way_script_cookies`, answers at that
moment for the FINAL address and the transport the page arrived over,
as the page's script host asks it, with every Set-Cookie of the head's
own hops already heard, so never an HttpOnly cookie (what the jar gains
after the head reaches no script in a replay: REPLAY.md § 3's jar row);
asked again with twice the room up
to 1 MiB, and a string that does not fit, or no memory for one, leaves
the record saying it lost lines.
`diag_write` compares each of `kDiagSettings`, the agent among them,
with what the claim wrote, and `diag_arrived` the layout's view with the
claim's (a view that moves after the arrival is not compared: yonder's
own bars move it, and a replay reproduces them); one that differs marks
the record `REPLAY unjudged: settings changed
during the load` (`yonder_diag_unjudged`; the first reason stands). The
head facts, `TLS` and `kept` have a room of their own, 24 lines counted
apart from the 512 the other plain lines may take
(`yonder_diag_head_fact`); `bytes` is a plain line. Every record ends
with `end` (`YONDER_DIAG_END`). The index is version 2: each script's
and sheet's line ends with the job's own identity (a script's token, a
sheet's place in its page's table).

**The readers.** `tools/yonder_records.py` is the one Python reader, which
the tally and the diff read records through; `rp_record_read` and
`rp_index_read` are the runner's. Each classifies every line by REPLAY.md
§ 2 (`classify()`, `rp_classify`), so a control byte anywhere, or a field
that decodes to a NUL, refuses a record or an index; both run every
vector in
`tools/yonder_record_grammar.txt` (the `escape:` vectors hold their two
unescapes equal, one pass from the left), and every number goes through
one parser each (`decimal()`, `rp_decimal`). A record is of version 2 when
it says any of `agent`, `cookies`, `clock` or `tz`; `end` is required of
one that is a kept page's, and of every replay's. Every page-written string either tool prints
goes through `display()`, and every one the runner prints through
`rp_display`, the same rule.

**The replay runner** (`tools/test_yonder_replay_host.{c,sh} KEPT-DIR
OUT-DIR`; its header says how it meets REPLAY.md). It includes
`tools/test_yonder_scripts_host.c` with `YONDER_HOST_FAKES_ONLY`, which
leaves that file's `main` out; two hooks there let it run the pool itself
(the scripted host's fake keeps 32 jobs of each kind and parks pictures
in the navigation's slot, which a real page overruns), and the file fake
serves a length. The build list is `tools/yonder_host_build.sh`, which
both suites source, so they link the same thing. Every file it touches
goes through `tools/test_yonder_replay_fs.{h,c}`, its own translation
unit, whose writes go to a temporary file made new (`O_EXCL|O_NOFOLLOW`),
flushed and renamed into place, whose reads refuse a symbolic link, and
whose every call answers done, absent or a fault; the runner poisons,
after including its header, the host's file functions that unit could
reach. **One process per page**: the parent reads each kept record to
decide whether it is a page to replay (`rp_is_kept`: one that says
`kept:`, or of version 1 one whose `.html` is beside it), forks a child for it, and waits for it ten
minutes at most (`rp_wait`): blocked in `waitpid`, which a 100 ms timer
interrupts so the clock can be read, retried on `EINTR` alone, the child
killed at its limit (SIGCHLD is reset to its default at start). It
removes the temporary a child killed inside its write leaves, and trusts
the child only when its exit status (`RP_CHILD_*`, 64 to 95) and its
record agree: the state, the defaulted line, and the clean flag against
the record's own lines of the kinds a replay reproduces; whether a page
let go of what it held the record does not say, and that flag can only
make the answer worse. Otherwise it writes the page unjudged itself,
saying why. The child (`rp_replay_page`) reads the page's inputs from
its record (`rp_window_of`): each is required (`kNeeded`) of a version
2 record. A version 1 record takes nothing silently: its window, agent
and zone, where it does not say them, are what the person states with
`--window KEY=VALUE,...` (`rp_window_state`; an item ends at a comma a
known key follows, so an agent string may hold commas), each named with
its value on `REPLAY inputs stated:`, and one neither said nor stated
leaves the page unjudged; its cookies and clock are the harness's, named
on `REPLAY inputs defaulted: cookies clock`; its absent `content-type`
or `charset` reads as empty, which is how its builds spelled empty; its
`kept lines` binds the index when it says one; and its jobs are matched
by kind and address in the kept order. A version 1 record that says no
`kept:` is a kept page when its `.html` is beside it, assumed whole, its
`bytes:` matching, and its replay record says `REPLAY kept: assumed
whole (version 1)`. A record and its index of different versions are
unjudged. It sets them where the window
reads them (`rp_apply_inputs`): the agent on the session, the cookie
string in the fake jar's script door, the clock the JS `Date` reads (to
the millisecond, and still), and the zone (a `TZ` into the environment,
or the configured offset with `TZ` unset). The page is fed, its jobs
answered from the index in the kept order, the clock moved a tick an idle
turn up to 60 virtual seconds, a navigation it starts ending the replay
there, and it is left as yonder leaves it when it closes (the header says
each step); `rp_decide` gives its state and `rp_compose` its record.

**The diff** (`tools/yonder_corpus_diff.py BEFORE AFTER`) judges only
pages replayed to a verdict on both sides, comparing their record lines
of the kinds a replay reproduces, with counts left out: a gained line is a
regression (exit 1), a lost one progress. A page whose after-record is
unfinished is BROKEN, a regression when the before-record reached a
verdict, never progress; one NEWLY INCOMPLETE exits 1 under its own
heading (the corpus must be recorded again before the change can be
judged on it); one incomplete on both sides is listed to be recorded
again; one that reached a verdict where it had not is recovered, and not
compared. A page the after directory lacks is a regression, one only the
after directory has is unjudged, and so is a pair of records of one name
and two addresses, or that took different inputs from the harness; a
directory it cannot list answers 2, and so does any `*.txt` the reader
cannot judge, each listed with why.

**The instrument** (slice 7's before-and-after): a script's line is
`script <address>: ran in <n> us`, the time `os64_js_run` took, compiling
included (`yonder_scripts_task_us`). On the host the clock moves only when
a case moves it, so the line is exact in the cases; in QEMU it is a number
and not a speed.

**Proof.**

| Check | Result |
|---|---|
| Scripted-page host (`tools/test_yonder_scripts_host.sh`): the keep cases (a windows-1252 page posted in 97-byte chunks with a NUL inside kept byte for byte; the index, its version, each file, a failed script's line and no file; the facts; a page let go unshown; a stream stopped before its verdict leaving nothing; a folder that cannot be made; a name another yonder took, bytes and a folder left from before, each passed over; an empty folder removed; a head fact kept past a full table; an address's own `%` written apart from the token spelling; every one of the claim's 64 names taken, with keep_pages and without, writing nothing of the page and saying so; with keep_pages off, a number beside another's kept bytes or folder passed over, the claimed file reading `verdict: unwritten` until the record replaces it; a record that lost a line saying so; `kept: whole` on a page kept whole; picture and font failures a fetch decided written as `picture-fetch` and `font-fetch`; the setting off keeping nothing; 512 ordinary facts and the head's together losing nothing, the record ending with `end`; the agent, the cookies a script could read, the clock and the zone on the record; a zoom, the scripts switch, the agent or the view changed before the page was left, and a page laid out in another view than its claim's, each marking it unjudged, and an unchanged window not; cookies the jar cannot hand over whole not written and the record saying it lost lines; the cookies taken at the head for the final address, so an http load redirected to https records its Secure cookie, a response's own Set-Cookie is on the record, and a cookie the page's first script sets is not; a claimed record removed from under its page not written again, nor one another yonder claimed after it went, by its nonce; a placeholder with no memory for its text, or whose write or close fails, removed and its number passed over) and the script time | 6,062 checks, zero failures |
| The replay's self-test (`tools/test_yonder_replay_host.sh`): the 29 fixture pages of `tools/yonder_loop_fixture`, `yonder_script_fixture` and `pages`, and twelve hand-made (a sheet that imports another from its redirected address and is read under its label, a page in windows-1252 from an encrypted fetch, a text body, a file's page and its script and timer, an error page, a failed script with a module and a global, a page too deep to read to its end, a picture by itself, a 1,400-pixel window, scripts off, an address holding `M%49SSING` of its own beside MISSING, the modern switch off, and a page read with every other input unlike the harness's, its agent, cookie string, clock, zone, view, zoom, dark pages, reduced motion and animations off, whose script writes each into its record), kept with keep_pages on in a child of their own, written to real files, replayed | 41 pages; every replayed record is the kept one (the paint stages aside, which the processor's counter times), the fixture pages having been kept with no input, as a replay reads them (REPLAY.md § 5); the corpus replayed again diffs clean, and replays the same from a runner started with SIGCHLD ignored. Then, each a corpus of its own: twenty-nine refusals, each checked by the reason its one rule gives (a damaged index line's number, a record that lost a line with no memory or past a full table, a `*.txt` that is not a record, a record cut before its `end` and one cut between its `kept lines:` and its `kept:`, a count of twenty digits, a quoted terminal sequence, index lines short of their fields or of a kind cut short, two sheets' lines numbered each as the other, a `kept lines` past UINT32_MAX and one short of its index, a `bytes` with something after its digits or a zero before them, an index of version 1 beside a record of version 2, an index of version 3, an empty index, an index with no version line, a carriage return in a fact, a control byte in an index field, a second `address:`, a second `kept:` after `kept: whole`, an index's version line again after its lines, a plain fact that is not UTF-8, a line past `end`, a second verdict, the claim's placeholder never replaced, an agent escaped to a byte that is no part of UTF-8); each of the nineteen inputs a record must say, taken out in turn; a child whose status names a state its record does not say, one that calls clean a page its record does not, one killed inside its write (its temporary removed), one that never ends (killed at its time), and two that exit outside the table (1, as a sanitizer does, and 97), each unjudged by the parent; two sheets of one address kept in the reverse order, each taking its own line; a kept page of version 1 judged with what was stated and defaulted named, one with no `kept:` judged as assumed kept whole and saying so, and one without a `zoom` nobody stated, unjudged; the page read with other inputs made version 1 and replayed in the window `--window` states, its script writing each stated input; `--window` taking an agent holding commas and refusing a key nobody states, a key twice, a view without its height and values no record says; a script asked for twice beside a version 1 index that kept it once, answered once; and the cases of the rounds before (a request the index lacks, no time (a corpus the diff compares with none given 60 s, by their budgets), two async scripts in their kept order, a navigation, a picture's failed fetch, a harness check failing, a partial run's rules, kept bytes of the wrong length, a record not UTF-8, a link planted at a temporary's name, a page whose replay dies or dirties the fakes); the whole-record vectors (a line a reader parses one value from, twice); and the child's elapsed wall-clock time with and without a borrow across a second, equal, backwards and past 2^32 ms |
| `tools/test_yonder_replay_mutants.py`, run by `tools/test_yonder_replay_host.sh --mutants` through `tools/mutants_parallel.py` | 108 mutants, 108 caught, each by its own case (its log names the case that failed): with `tools/test_yonder_diag_mutants.py`'s (56 caught of 56, which hold yonder's own page-file rules, § 1's placeholder that is not whole among them), one for every rule of REPLAY.md § 1-7. Each of the 19 inputs taken out of `kNeeded`; the record's inputs not read; each input not applied (all of them, and the agent, cookies, clock, zone, zoom, appearance, motion, animations and height alone); the defaulted line not written; an unknown index version read; two versions let be; the identity compare, the numbering check, the index's version line standing once and one-line-one-job dropped; `kept lines` not bound to the index; the leading-zero refusal, the UINT32_MAX bound, the control-byte refusal, the NUL-field refusal and the end rule each dropped in both readers; the C reader's whole-record UTF-8 check, lines past `end`, a second verdict and the placeholder each read; a second head fact and a second address read in the C reader, and in the Python reader a second head fact, a second state, and a second of each of the five `REPLAY` lines that stand once, each alone; a not-reproduced kind named twice read, in both readers; a field escaped to bytes that are not UTF-8 read, in both readers; the budget line unwritten, and not required by the Python reader; the parent's agreement check, clean-flag check, exit-table bound, `EINTR` retry, time limit, temp removal and SIGCHLD reset each dropped, and the time limit's elapsed milliseconds taken without the nanoseconds' borrow, or a backwards span read as a forward one; version 1's stated inputs taken from the harness (the zoom, the agent), an unstated input not refused, the stated and assumed lines unwritten, a page with no `kept:` not kept, its absent `charset` not read as empty, and `--window` splitting at every comma or taking a view without its height; yonder's claim proof and its nonce comparison, whole-cookies check, setting-change mark, scripts setting and arrival view check each dropped; the cookies not taken at the head, taken for the clear, taken again after the scripts at arrival, and not taken for a file's page; and the diff's defaulted-set and address pairing, the `--window` statements' pairing, the budgets' pairing, unfinished-is-broken, regression, broken, newly-incomplete, missing-from-after, only-after, not-reproduced and unjudged rules each dropped |
| `tools/test_yonder_records.py`, `tools/test_yonder_tally.py`, `tools/test_yonder_corpus_diff.py` | 13, 14 and 25 tests pass, every grammar vector among them |
| The P5's 1,601 census records | all read as verdicts by the new grammar; none is a kept page |
| Chris's kept pages of 2026-10-21 and 2026-10-22 (601 records), with the `--window` below | 2026-10-21: `replayed 341, clean 4, incomplete 107, unfinished 11; 341 of version 1, their inputs defaulted; 16 records are not kept pages; 10 unjudged; 2 did not let go of what they held` (every one assumed kept whole: none says `kept:`). 2026-10-22: `replayed 224, clean 15, incomplete 7, unfinished 4; 224 of version 1, their inputs defaulted; 4 records are not kept pages; 6 unjudged`. Every replayed record names what was stated and defaulted; the 16 unjudged lost lines (3 kept records, 13 the replay window's own); the 20 not kept pages have no bytes beside them; the two that did not let go are www.bing.com pages, a window leak on a real page the fixtures do not have |
| Anchors of every mutant harness that names a file this slice touched | 1,079 anchors, each found once |

**The corpus count**: 565 of the 601 records kept on 2026-10-21 and
2026-10-22 replay (436 to a verdict, 19 of them clean; 114 incomplete;
15 unfinished), 16 are unjudged and 20 are not kept pages, run with

    --window zoom=1000,appearance=dark,motion=full,animate=on,modern=on,scripts=on,script-seconds=5,laid-out-at=2526x1282,agent=<the Chrome on Windows preset>,tz=EST5EDT

The settings are the P5's yonder.conf as Fable stated them; the width is
what 536 of the records' `laid out at` lines say; the height (1282) was
inferred from the records' whole-view paint areas, and `tz` and the
agent were read from notes and yonder's own Chrome preset, because the
P5 did not answer that day. Chris confirms or corrects those three, and
both sides of any measurement are run with the same statement. A page
recorded from now on is version 2 and says them all.

**Deferred**, each a DEBTS.md row (§ The replay corpus): a replay lays
text out in the harness's bitmap face; `screen` answers libdom's
default, not the display the page was kept on; a kept corpus holds the cookies a page's scripts
could read; keep_pages writes on the window's thread; a page that
navigates replays only as far as going; a replay does not reproduce what
a picture's or a font's fetch decides; a form's reply replays as a GET.

### Slice 1a, as built

Built by Opus on `opus/tmw-1a-storage`, from MODERN_WEB_BRIEFS.md § Slice
1a (the reviewed copy on `fable/modern-web`), then read cold by a fresh
Opus (eight findings, all fixed on the branch) and read for design. Six
rows in `modern.c`, a file each in libdom, and yonder's store.

**What was built.**

- **`localStorage`, `sessionStorage`, `Storage`** (`storage.c`): the whole
  interface, and named properties as WebIDL's legacy platform object (a key
  is an own property unless the prototype chain has one by that name;
  `[[Set]]` and a data `[[DefineOwnProperty]]` are `setItem`, `delete` is
  `removeItem`, a Symbol is ordinary). The quota is 5 MiB counted as the
  standard counts (two bytes a UTF-16 code unit of every key and value),
  plus a cap of 65,536 keys, which bounds what the quota cannot (a
  one-character key costs the quota two bytes and the heap a record). An
  opaque origin (a file, `about:blank`) gets areas that work and are kept
  nowhere. `key(n)`'s order is the map's: a removal moves the last key into
  its place, so a change list of n removals (what `clear()` makes) is
  linear work.
- **`performance`** (`performance.c`): `now()` and `timeOrigin` coarsened
  to 100 µs; User Timing's `mark` and `measure` (the L3 dictionary too),
  `getEntries`/`ByType`/`ByName`, `clearMarks`/`clearMeasures`, `toJSON`,
  `detail` a structured clone of plain data through the engine's own
  writer; the legacy `timing` and `navigation` objects and the `navigation`
  entry. The connection phases yonder does not measure read as
  `fetchStart`, `secureConnectionStart` too for an https page.
- **`URL`, `URLSearchParams`** (`url.c`), over `os64_page_resolve_in` with
  no document, so a script's address and a link's agree: every component
  and setter, `canParse`, `parse`, `toJSON`, and URLSearchParams whole,
  live both ways with its URL and sorted stably in UTF-16 code units.
- **`TextEncoder`, `TextDecoder`** (`encoding.c`): UTF-8 out; in, UTF-8,
  windows-1252 and its aliases, and UTF-16 of either order (beyond the
  brief: WPT's `textdecoder-*` pages need it, and libhtml's label table
  gained the Encoding Standard's UTF-16 labels for it), with `fatal`,
  `ignoreBOM` and streaming.
- **`screen`, `devicePixelRatio`** (`screen.c`): the display in CSS pixels
  at the page's zoom; the zoom as the ratio, as Chrome reports it.
- **`btoa`, `atob`** (`base64.c`).
- **yonder**: `profile =` in `yonder.conf` (`yonder_settings_saved_profile`,
  default `/home/yogi/.yonder`, made with its parents when first needed, a
  refusal said on the status line), the store (`apps/yonder/storage.c`),
  Settings' "Clear page storage", the navigation's milestones marked as the
  stream reaches them, and the screen read from the desktop at start.

**The seams, as they ended.**

| Seam | What it is now |
|---|---|
| `os64_dom_storage_t` | a provider per area: `identity` (asked at a task's first touch; a value other than the last loaded means the store moved), `load` (the image, asked again with room, a few times, when it outgrew the room), `commit(changes)` (this page's change list only, merged by the host onto the store as it stands) |
| the statuses | OK; FULL (past the quota: nothing kept, the page reads the store back); FAILED (the area keeps its map in memory for the page's life) |
| commits | coalesced to one a second (`os64_dom_storage_settle`, `_due`, `OS64_DOM_STORAGE_COMMIT_MS`); sooner when the change list would pass the quota or the keys' cap, and a change it could not make room for (no memory to write the list) is refused as a full store's is, so what a page writes never takes the list past its bound (a `clear()` of a map another window grew past the keys' cap can, until the next commit); once more at drain, the last |
| `page_departs()` (yonder.c) | the one place the page on screen departs: where every navigation's page is first parsed (`stream_parser`) and as the window closes. It unloads the page, as HTML does: `yonder_scripts_depart` retires its runtime, so none of its queued scripts, timers or handlers runs again, and drain makes its storage's last commit then, so the next page's scripts read what it wrote. The page stays on screen, scriptless, until the new one is laid out; a navigation that never becomes a page is not a departure |
| the store | `<profile>/storage/<origin>`, the origin with every byte outside `[A-Za-z0-9.-]` as `%XX`; read, merged and published by temp and rename, which is atomic (a reader meets the old image or the new one); the store's identity read again just before the rename, and a store another window published since merged onto again. There is no lock between windows (Chris's ruling: one person on one machine), so two committing one origin in the same instant can lose one window's changes (DEBTS). Clear page storage removes each store with one unlink; a window mid-commit may then publish the store as it read it before the clear, with its own changes. The first touch of the profile removes the temps a crash left |
| the image format | `os64 storage 1\n`, then `+<key bytes> <value bytes>\n` key value `\n` for a set, `-<key bytes>\n` key `\n` for a removal (an image holds sets only; no store is no image, and a file without the magic line, an empty one included, is not one); `os64_dom_storage_merge` and `os64_dom_storage_walk` are libdom's, and yonder only moves bytes |
| `options.now_us`, `options.timing` | the host's microsecond clock, and the navigation's origin and milestones (`os64_dom_timing_t`), read as the binding is made and whenever a script asks |
| `options.screen`, `options.zoom` | the display in device pixels, and the zoom in thousandths when asked |
| the plain EventTarget | `d_event_target(prototype, class)` registers a class and makes its prototype inherit EventTarget.prototype, which holds the three methods, `d_event_handler` an `on<type>`, `d_dispatch_plain` a dispatch whose path is the target alone; performance and `screen.orientation` use it |

**The WTF-8 ruling.** The brief said to replace NUL and lone surrogates
with U+FFFD at storage's boundary, as the binding does elsewhere. Storage
keeps them exactly, as WTF-8, and the design read ruled that correct:
browsers keep them, WPT's webstorage holds a binding to it, and pages that
pack binary into UTF-16 strings (LZ-string's compressors) read back
garbage from a store that replaced them. The reason is written at the
contract in `dom.h`.

**Numbers.** WPT, before (the baseline commit) → after:

| directory | before | after | `[required]` | excluded pages |
|---|---|---|---|---|
| `webstorage` | 0 | 1,236 | 142 | 0 |
| `url` | 6 | 150 | 140 | 0 |
| `hr-time` | 0 | 8 | 8 | 0 |
| `user-timing` | 3 | 466 | 463 | 0 |
| `encoding` | 0 | 3,955 | 183 | 1,207 |

Every required test passes with the switch on and fails with it off. The
excluded pages are the legacy multi-byte encodings' (`legacy-mb-*`, the
ISO-2022-JP and Big5 encoders), out of scope as the brief says. What still
fails, by cause: `url`'s pages that load `urltestdata.json` with `fetch`
(slice 2), userinfo in `url-setters-stripping` (320, the grammar's), the
`storage` event and iframes in `webstorage`, `DOMException`'s `code` and
`structuredClone` throughout, PerformanceObserver, SharedArrayBuffer and the
streams. `dom/nodes` is unchanged (858).

**The census** (the P5's 1,601 page records, counted with slice 0b's
`yonder_tally.py`): 678 records, 92 of 268 sites and 62,138 asks name at
least one of `localStorage` (466 records, 46,635 asks), `sessionStorage`
(37, 13,366), `performance` (253, 1,059), `URL` (252, 324),
`URLSearchParams` (83, 131), `TextEncoder` (144, 147), `TextDecoder` (140,
141), `screen` (247, 274) or `devicePixelRatio` (54, 61); `btoa` 161
records and `atob` 53 besides.

Host: the DOM host from 3,810 / 22,416 checks to about 6,600 / 35,000,
with about 5,200 allocation sweeps; the scripted-page host from 5,888 to
about 6,000; all 0 failed (the PR states the counts at its head). The
mutant harnesses were not run (the briefs' rule).

**The cold read** found eight, all fixed: performance's private buffer
reachable through an `Array.prototype` setter (P1), the departure commit
too late for the next page (P1), quadratic removals (P2), two yonders
merging onto one image (P2), a lookup stale after a Proxy trap, answered
arrays filled through setters, orphaned temps, and a comment (P3s). **The
design read** passed it with the WTF-8 ruling and the last commit's longer
wait (since retired with the lock, below). **Codex's first round** found eight: six fixed (the change list's
bound under a busy store, the departed page's tasks, an empty file taken
for an image, Clear against a live commit, measure's negative duration,
`navigationStart` without a wall clock), and two declined on WebIDL's
text (`new URLSearchParams(null)` is `null=`; a sequence's conversion has
no IteratorClose), the second with a conversion-order bug found beside it
and fixed. **A second cold read** found five, all fixed: a quadratic
record conversion behind keys holding U+FFFD, Clear missing a window
making an origin's first store, a commit out of memory retried every
turn, and two comments. **Codex's second round** found three: the orphan
sweep's grammar and redirectCount fixed, an empty measure dictionary
declined on User Timing's step 1. **Its third round** found a race
between two windows breaking one crashed lock; Chris ruled the
cross-window lock out (one person on one machine), so a commit is read,
merge, second look, rename, with BUSY and the last commit's wait retired
and the same-instant loss booked.

**Stacked on this branch**, slice 2 (fetch) moves `d_origin` onto libway's
`way_origin_*` (Fable's ruling 8: the slice that lands second does it).

**Booked** (DEBTS.md § Classic browser scripting): the `storage` event
between windows; PerformanceObserver and the `resource` and `paint`
entries; `DOMException` with `code` and `structuredClone` (raised with
Chris as a small libdom slice); libos64's URL grammar gaps (userinfo, IPv6,
IDNA, `aaa:/b`, an opaque path's trailing space); the legacy multi-byte
decoders; a store's identity at whole-second resolution; sessionStorage's
memory per window; and two windows committing one origin's store in the
same instant.

### Slice 4, as built

Built by Opus on `opus/tmw-4-css`, from MODERN_WEB_BRIEFS.md § Slice 4,
then read cold by a fresh Opus (eleven findings, all fixed on the branch)
and read for design. Every property is a libgarb row marked `modern`, with
its grammar, and a consumer in libflow or yonder's painter; the cascade,
the style object and WPT all see it go with the switch.

**What was built.**

- **The flow-relative properties** (CSS Logical 1 § 4). Each
  flow-relative longhand, and each border side's flow-relative shorthand,
  is a NAME for its physical twin in a horizontal, left-to-right writing
  mode (`kLogical` in `libgarb/props.c`), so the cascade orders
  `margin-inline-start` against `margin-left` by importance, specificity
  and order as one property, which is what the standard's mapping comes to
  for the one writing mode libflow lays out. The fourteen two-sided ones
  (`margin-inline`, `padding-block`, `inset-inline`, `border-inline-width`
  and the rest) are new shorthands, each a pair of physical sides
  (`kPairs`). Quirks mode's unitless lengths are the physical names' alone.
  `direction` and `writing-mode` are not read (DEBTS.md § Browser layout).
- **`cursor`** (UI 4 § 5.1.1): every keyword, inherited, kept on
  `flow_style_t`. Pictures (`url()`, and `image-set()` read against its
  grammar) with their hotspots are read and dropped, so the fallback
  keyword is what is kept. Nothing draws a pointer shape yet (the
  honey-do's work).
- **`user-select`** (UI 4 § 6.1): read and kept; nothing selects
  differently.
- **`word-break` and `overflow-wrap`**, with `word-wrap` as its alias (Text
  3 § 5.1, § 5.4). `break-all` makes a break between every two clusters;
  `anywhere` and `break-word` make EMERGENCY ones, taken only where a line
  holds no other, cut at the clusters the run's carets give
  (`word_pieces`). Min-content counts `break-all`'s and `anywhere`'s and
  not `break-word`'s, as § 5.4 says; `Segs.measuring` says which is being
  worked. Each word's first caret is found by halving, so cutting is
  linear in the text (LAYOUT.md § Bounds). `keep-all` is `normal`, since
  libflow breaks only at spaces.
- **`letter-spacing`** (Text 3 § 7.2): a `<length>`; Text 4's percentages
  are refused. The text engine adds it (`os64_text_layout_t.letter_spacing`)
  after every cluster but a tab, the run's last included, so characters on
  either side of an element's edge are spaced as any two are. libflow
  leaves the spacing after a line's last character out of the line — in
  the fit, the fragment's width and min- and max-content
  (`trailing_spacing`). **That is a deliberate divergence from Chrome and
  Firefox**, which keep the trailing gap: § 7.2 says a UA must not append
  letter spacing at the end of a line. On the P5 it shows as a centred,
  letter-spaced heading sitting half a gap off where Chrome puts it;
  matching Chrome is a one-line flip, `trailing_spacing` answering 0.
- **`text-overflow`** (Overflow 4 § 4.1): `clip | ellipsis`. A block that
  clips (`overflow` not `visible`) and asks for an ellipsis has one laid out
  once in its own fonts (`flow_box_t.ellipsis`: U+2026, or three periods
  where the fonts have no glyph for it). Where a line ends in it, and what
  of each box on it is kept, is ONE predicate in libflow with two doors,
  beside the box tree it reads: `flow_line_cut` gives the limit (the end
  edge less the ellipsis) and where the ellipsis starts, and
  `flow_line_cut_keeps` is the one place the keep rule is written — an
  atom or a marker kept whole only when it ends by the limit, a text kept
  to its last character that does, an inline box's piece kept up to where
  the ellipsis starts. `flow_box_cut_away` asks it of a box and every box
  it is inside, so a box stacked out of a piece left out is left out too.
  The painter draws only what is kept, and `flow_hit` hits only that: it
  asks the predicate of each piece on a cut line and `flow_box_cut_away`
  of each layer, and past the ellipsis's start answers the block (the
  ellipsis stands in no inline box, as in Chrome). It lives in libflow and
  not the painter because hit testing is libflow's and the painter can
  call it unchanged; a rule written twice is two rules. An absolute box in
  an inline is out of flow, not on the line, and is not hidden: § 4.1
  hides a line's characters and atomic inline-level elements. Spaces that
  hang at a pre-wrap line's end do not count as content passing the end.
  A line with no room before the ellipsis keeps its first character or
  atom, clipped rather than ellipsed, and the ellipsis follows it, clipped
  by the box with it. The end is where the block's scroll shows it, so
  content scrolled into view is drawn and a line scrolled to its end has no
  ellipsis. libflow's walk hands the painter the block's LINE boxes after
  each line's pieces, and the painter draws the ellipsis then as it draws
  the block's text, its shadows and then its fill (`text_run`); a text
  clip's walk hands it the line
  too, so a `background-clip: text` heading's ellipsis takes the gradient,
  or the clip's solid pen where the face refuses the clip. Cuts are kept
  in a memo keyed by the line that grows (`flow_cut_memo_t`), one for a
  paint and one for a hit, so each line is worked once whatever order its
  boxes and layers are asked in; a hit works only the lines under the
  point or that a layer holding the point is inside.
- **`outline`** and its four longhands (UI 4 § 3), the shorthand's parts
  read in any order (`auto` is a style and a colour both, so `outline:
  auto solid` is read by backing up, `outline_rest`): a square ring of a
  border's styles round the border box, `outline-offset` past it, taking no
  room and counted in the box's ink (`flow_outline_rect`). `auto` is drawn
  solid. The width kept is the computed one whatever the style, so
  `inherit` passes it on; a style of none draws nothing. A negative offset
  is held so the ring's outside is never under twice its width each way
  (§ 3.5). It is painted LAST in its stacking context (CSS 2.1 Appendix E,
  step 10): `flow_visit_groups` hands each ring that meets the view to
  `flow_visitor_t.outline` in a layer of its own, after the context's
  members and before its group closes. The ring, not the box's overflow,
  is what is asked to meet the view. There was no default focus ring to
  move: `:focus` never matches, and yonder's widgets draw their own.
- **`object-fit` and `object-position`** (Images 3 § 4.5, § 4.6): a
  picture is placed by `yonder_object_rect` and cut to its content box.
  Images 4's pairs (`cover scale-down`) are not read.
- **`aspect-ratio`** (Sizing 4 § 4.1; a new value kind, `GARB_V_RATIO`). A
  block's height follows its width and its width a given height, measured
  on the content box under `auto && <ratio>` whatever box-sizing says (§
  4.1: "in all cases"). Whichever size the ratio gives, the box is held to
  its automatic minimum there where that minimum is auto and it does not
  scroll: at least its content's height, at least its min-content width,
  capped by its maximum (§ 4.3; `ratio_content_h`, `ratio_floor_w`). The
  limits travel across the ratio by one door, so every formatting context
  answers alike: a width made from a height takes the height held to its
  own limits first (`ratio_width_held`, CSS 2 § 10.3.2's order), and a
  width the ratio box leaves to itself is held to its own limits and to
  its definite min- and max-height carried across onto a min- or max-width
  the page left unset (§ 4.4, `width_limits`) — so a stretched block with
  `max-height: 80px` is as wide as that height makes it. A picture's own
  ratio loses to the page's unless `auto` is written beside it, and a
  picture not yet measured takes its other side from the page's ratio,
  held by the limits an arrived one is (`replaced_hold`, which every
  replaced box with a side given ends in, an `<input type=image>` before
  its picture comes too); with one side given and a ratio of its own (not
  `auto <ratio>`) it is fixed, so its arrival lays nothing out again —
  unless it is a flex item whose main size, basis or main minimum leaves
  the flex algorithm reading the picture's own size
  (`flow_replaced_fixed`, asked of the box). A
  float or an absolute box with a height and no width takes the ratio's
  width rather than shrinking to fit. An absolute box whose two sizes are
  left to it takes its BLOCK axis as the ratio-dependent one unless only
  its inline axis has an auto inset (Position 3 § 4.1), so four insets
  set do not stretch its height. A flex item with a definite cross size
  (given, or its single line's that stretches it, Flexbox 1 § 9.8) has its
  flex base size through the ratio (§ 9.2.3 B), under `flex-basis:
  content` even where its main size is given; its automatic minimum stays
  its content's, since § 4.5's transferred size suggestion is a replaced
  item's. A grid item under `normal` starts down its area and stretches
  across it: the inline axis keeps `stretch`, as Chrome and Firefox do for
  the `aspect-ratio` tile pattern (a grid of `aspect-ratio: 1` cards fills
  each column and is as tall as it is wide). A written `stretch` still
  stretches it down. Intrinsic widths follow a given height through the
  ratio. A table's inner boxes take none, as § 4.1 says, and a table's own
  box is booked.

The two grammar laxities WPT exposed in passing are fixed too: a radial
gradient's `at` is a full `<position>`, and each of `text-indent`'s
keywords may appear once.

**The seams, as they ended.**

| Seam | What it is now |
|---|---|
| `kLogical`, `kPairs` (libgarb) | a flow-relative longhand is its physical twin's name; a two-sided one is a shorthand of a physical pair |
| libdom's style object | every property name libgarb reads is a member three ways (CSSOM § 6.6.1): camel-cased, webkit-cased for a `-webkit-` name, and dashed as written, for shorthands and aliases too (`garb_prop_name_at`, `garb_prop_known`); a marked one goes with the switch |
| `flow_visitor_t.outline` | its last member: handed each ring last in its stacking context; NULL (as `flow_visit` passes it) means no outline step |
| the LINE visit | the walk (`flow_visit`, `flow_visit_groups`) hands a LINE box over only for a block with an ellipsis, after the line's pieces |
| `flow_line_cut`, `flow_line_cut_keeps`, `flow_box_cut_away`, `flow_cut_memo_t` (libflow) | where a line ends in its ellipsis and what of each box on it is kept: the one predicate the painter and `flow_hit` both read, and the memo each keeps its cuts in |
| `os64_text_layout_t.letter_spacing` | tracking after every cluster but a tab, the last too; trimming it at a line's end is the caller's |
| `width_limits`, `hold_width`, `ratio_width_held` (libflow) | the one door every width a ratio makes or holds passes through, in every formatting context |
| motion | `copy_prop` covers all thirteen longhands, so `same_value` does too; `letter-spacing`, `outline-width`, `-offset`, `-color`, `object-position` and `aspect-ratio` (by its logarithm, Values 4 § 5.7.1) interpolate, and the keywords are discrete at half way |

**Numbers.** WPT, before (the baseline commit, nothing built) → after:

| directory | before | after | `--modern-off` | `[required]` | excluded pages |
|---|---|---|---|---|---|
| `css/css-logical/parsing` | 150 | 269 | 155 | 114 | 16 |
| `css/css-sizing/aspect-ratio/parsing` | 11 | 18 | 11 | 7 | 1 |
| `css/css-images/parsing` | 348 | 287 | 271 | 16 | 7 |
| `css/css-text/parsing` | 224 | 268 | 254 | 14 | 30 |
| `css/css-ui/parsing` | 67 | 151 | 69 | 82 | 13 |
| `css/css-overflow/parsing` | 109 | 122 | 120 | 2 | 10 |

The 235 required tests pass with the switch on and all fail with it off;
none leaks. The `--modern-off` column is above "before" because the style
object's dashed and webkit-cased members and the two grammar fixes are not
the arc's surface and stay with the switch off. The excluded pages are
every page whose tests read `getComputedStyle`, which waits on slice 1b.

**css-images went DOWN, 348 → 287.** All 97 lost tests are
`conic-gradient` values in `-invalid` pages (95 in
`gradient-interpolation-method-invalid.html`, 2 in
`conic-gradient-calc-angle-percentage-invalid.html`). They passed only
because `background-image` was unreachable by its dashed name; once the
style object answered it, libgarb's ruling that a conic gradient is taken
by name (GARB.md's module table) makes every one of those values valid.
The 36 gained are the slice's 16 (`object-fit`, `object-position`) and 20
that are not the slice's surface and pass with the switch off too:
gradient positions (the dashed names reach them, and the radial `at` fix
holds them), `image-rendering`, and one conic case. One page,
`gradient-interpolation-method-valid.html`, times out: its thousands of
style writes run the runner's script clock out. It passed nothing before.

Most `-valid` tests that still fail fail on serialization. The style
object returns a value as it was written, not CSSOM's canonical form (`0`
for `0px`, `16/9` for `16 / 9`). It also cannot read back a shorthand's
longhands, and there is no `CSS.supports`. That is a libdom slice of its
own (DEBTS.md § Classic browser scripting).

**The census** (the P5's 1,601 page records): 985 name at least one of the
slice's properties as missing. By records: `cursor` 919, `outline` 865,
`letter-spacing` 726, `text-overflow` 650, `user-select` 627,
`overflow-wrap` 556, `outline-offset` 553, `word-break` 520, `object-fit`
428, `word-wrap` 387, `aspect-ratio` 356, `object-position` 296; some
flow-relative property in 512 records and 127,354 asks. danlegt's asks:
`object-fit` 7,537, `letter-spacing` 6,297, `cursor` 4,865, `outline`
2,438, `word-break` 572, `overflow-wrap` 286. Not in the brief and still
missing: the flow-relative radii (84 to 92 records each),
`scroll-padding-block` (51), `contain-intrinsic-block-size` (36).

Host: libflow 21,382 → 23,667 checks, yonder 1,716 → 1,914, the DOM host
22,416 → 38,935, garb declarations 632, text 7,369, the scripted-page host
5,888; all 0 failed. The mutant harnesses were not run (the briefs' rule).

**Booked** (DEBTS.md § Browser layout, unless named): `direction` and
`writing-mode`; `aspect-ratio` on a table's own box; the flow-relative
radii; an outline on an inline box, and one that follows `border-radius`;
`text-overflow` on an anonymous block's lines; `overflow-wrap` beside a
float; the `-computed` WPT pages once slice 1b lands; and the style
object's serialization (§ Classic browser scripting). CSS system colours
(`highlight` and the rest) are booked beside them, though older than the
slice: libgarb never read them. Noted and not booked:
`word-break: break-all` over a megabyte of text makes a segment per
character, the same class of cost libflow already pays per word.

### Slice 1b, as built

Built by Opus on `opus/tmw-1b-view`, from MODERN_WEB_BRIEFS.md § Slice 1b,
then read cold by a fresh Opus (nine findings, all fixed on the branch,
and a tenth the new cases found) and read for design. Four rows in
`modern.c`, `libdom/view.c` and the history half of `window.c`, libflow's
`computed.c`, libgarb's serializer for a media list, and yonder's session.

**What was built.**

- **The view** (`view.c`): `scrollX`/`pageXOffset`, `scrollY`/
  `pageYOffset`, `innerWidth`/`innerHeight` (the viewport less its
  scrollbars, what a media query reads), `outerWidth`/`outerHeight` (the
  window's frame, unzoomed), `scroll`/`scrollTo`/`scrollBy` (two numbers
  or a ScrollToOptions; `smooth` done at once; non-finite is 0; the
  host clamps), `document.scrollingElement` (CSSOM View's, quirks
  included), `onscroll`/`onresize`, and window's `focus()` and `blur()`,
  which do nothing (the window is the person's to raise; added when the
  corpus replay showed a site's analytics script reaching
  `defaultView.focus()`).
  `resize` at window and `scroll` at the document, bubbling, are fired by
  the host's rendering step.
- **`matchMedia`, `MediaQueryList`, `MediaQueryListEvent`**: `media` is
  CSSOM's serialisation of the list (libgarb's new
  `garb_media_serialize`/`_text_serialize`/`_text_serialized`), `matches`
  judged at every read, `change` when the answer moved since the last
  step (a change and its undoing owe nothing), `addListener`/
  `removeListener`, and a list nothing holds freed by the next
  `matchMedia`.
- **`getComputedStyle`**: a live, read-only declaration over libflow's
  `flow_computed_*` (resolved values: used ones off the laid-out box for
  the sizes, margins, padding, a positioned box's insets, line height and
  transform), camel, dashed and webkit spellings, `cssFloat`, indices and
  iteration in CSSOM's order; a pseudo-element and a detached element read
  an empty declaration. `CSSStyleDeclaration` is an interface on window,
  naming the style object's prototype, which the computed one inherits
  and shadows, so `instanceof` answers truly for both (added for the
  corpus replay's check that every interface the slice hands out is on
  window). `document.defaultView` is window (beyond the brief: it is how
  pages reach `getComputedStyle`).
- **History**: `pushState`, `replaceState` (an empty URL is none, whatwg/
  html#9343), `state` (JSON: plain data, finite numbers but -0, no holes;
  DataCloneError otherwise, QuotaExceededError past 64 KiB),
  `scrollRestoration`, `length` from the window's session, `onpopstate`,
  `onhashchange`, `PopStateEvent`, `HashChangeEvent`; the location hash
  setter's own rule (the fragment it already has, as the URL parser's
  fragment state writes it, navigates nowhere).
- **yonder**: the three providers, the session's entries (below), Back
  and Forward within a document with no fetch, and the owed events run
  before a page's other tasks.

**The seams, as they ended.**

| Seam | What it is now |
|---|---|
| `os64_dom_view_provider_t`, `os64_dom_view_step` | `read` (the view now, no layout) and `scroll` (a layout made current, then the view scrolled, its cost added to the geometry counts). The step fires at most one event a call — `resize`, else `scroll`, else the first list whose `matches` moved — and the host calls it until it answers false |
| `os64_dom_set_computed` | the provider puts each property of libgarb's table (`garb_prop_t`) as CSSOM serialises it; **the table is the property list**: a declaration's length, order and names are the table's, so slice 4's `-computed` WPT pages read exactly what libgarb models. Every read asks again for the whole element into one snapshot buffer; one past the binding's budget is refused whole |
| the three routes (`os64_dom_history_t`) | a STATE UPDATE (`update`: the address and its JSON, push or replace) fires nothing; a FRAGMENT NAVIGATION (`fragment`) adds an entry, scrolls, fires `popstate` inside the script that made it (HTML § Navigate to a fragment) and owes `hashchange` when the fragment moved; a TRAVERSAL is the host's: it tells the binding (`os64_dom_history_moved`) before its own entries move, then owes `popstate` and, when the fragment moved, `hashchange` |
| owed events (`os64_dom_history_event_t`) | carry what they were owed with: a hashchange its two addresses, a popstate its entry's state and name (`os64_dom_history_entry`); its state is `history.state` itself only while that entry is still the current one |
| entries (`way_crumb_t.document`, `.entry`, `.state`) | a crumb names its document (moving between two of one document's is no fetch), the entry itself (a fetched traversal finds its target by name wherever the list has moved it, the entries between keeping their own scroll and state) and its state, owned by the session |
| `stream_entry` | a Back, Forward or Reload that fetches gives the arriving document the entry's state before its first script (HTML § Restore the history object state); a host whose binding is not made yet keeps it for the binding |
| `g.traversing` | while a traversal is fetched, the page on screen's state updates and fragment navigations change the page and not the session (HTML § Finalize a same-document navigation drops a change whose document is no longer active) |
| `unshown_go` | `history.back()` or `forward()` past a page not yet shown is ONE rule wherever its arrival is (parse, DOMContentLoaded, waiting for sheets): its navigation committed when its response came (HTML § Finalize a cross-document navigation), so when there is an entry that far (§ Traverse the history by a delta) the page is never shown, a new way's entries go ahead (a traversal's are let go with it: DEBTS.md), and the window goes there from the page on screen, with no fetch when that is the page on screen; when there is none, nothing happens and the page arrives. A new way's page has nothing ahead, and neither has a page that pushed before it was shown, however it came: the push let go of what was ahead of its entry; a reload is the entry on screen again; a traversal's page is the entry it was fetched for (`Page.arrives_at`), which its arrival lands on whatever it pushed before it was shown. From `load` on, the page is the one on screen |
| `Page.loaded`, `g.replacing` | a page's navigation by address (`location.replace`, `location.href`) or a form its script sends replaces the page's own entry until the page has completely loaded, which in yonder is when its `load` task, and the navigation that task asked for, is over (`page_navigation_replaces`; HTML § 7.2.4, Location-object navigate, step 3, and § 4.10.22.3 Form submission algorithm, step 25). `replace()` replaces whenever it runs. No transient activation is weighed: a page is shown and its `load` runs in one turn, so a person's input has no task in that window, and a timer or a click after `load` pushes. For the page on screen (its `load` running) the entry replaced is the one on screen. A page not yet shown (its parse, its DOMContentLoaded, its wait for sheets) is the session's entry already, so the new page takes its place (`start_trip`): it comes as the replaced page came (a reload's as a refresh), at the entry a traversal was fetched for, with the entries its scripts made around it, and a traversal goes on as its own. A redirector reached by a link leaves the page before it behind; one reached by Back is replaced, so Back past it is the entry before it. A page not yet shown whose navigation waits on the question bar is judged and named as its own (the downgrade is from it, and a yes sends its address as the Referer), but its place in the session is the page on screen's: its place is not taken (DEBTS.md) |
| the switch | with it off: none of the four rows' names, nor `scrollingElement`, `defaultView` or an element's `onscroll`; the arc's handler types are not handlers as content attributes either; `history.length` is 1, a fragment navigation is an ask, the hash setter asks whatever it is given, and history deltas do not add up. What it does not reach: the window's own session (a fragment link's entry, Back within a document, the entries named by crumbs), which is yonder's own history, not web surface (the design read's ruling) |

**The scroll-restoration ruling.** The brief said a traversal within a
document "scrolls to the fragment". It restores the scroll position the
entry was left at, and the design read ruled the brief wrong: HTML's
"update document for history step application" runs "restore persisted
state", which restores the entry's scroll position data when its scroll
restoration mode is `auto` (HTML § Restoring persisted state), and that
is what browsers do. A page that sets `history.scrollRestoration` to
`manual` keeps its own.

**Numbers.** WPT, before (`2bbb9f60`, the baseline commit) → after, with
the switch on and off:

| directory | before | after | after, switch off | `[required]` | excluded pages |
|---|---|---|---|---|---|
| `css/cssom-view` | 217 | 229 | 217 | 8 | 9 (frames) |
| `html/browsers/history/the-history-interface` | 4 | 50 | 4 | 19 | 0 |
| `css/cssom` | 459 | 589 | 459 | 20 | 0 |
| `css/mediaqueries` | 0 | 117 | 0 | 13 | 0 |

Every required test passes with the switch on and fails with it off.
`dom/nodes` is unchanged (858). What still fails, by cause: frames (the
excluded pages, and history's pages that open one), `DOMException`'s
`code`, computed custom properties and shorthands, and the members of
cssom-view's element geometry outside this slice.

**The census** (the P5's 1,601 page records, slice 0b's
`yonder_tally.py`): 316 records, 57 of 268 sites and 9,668 asks name at
least one of the slice's names: `getComputedStyle` (187 records, 192
asks), `matchMedia` (246, 479), `innerWidth` (62, 2,552), `innerHeight`
(68, 2,385), `outerWidth` (14, 15), `outerHeight` (9, 10), `scrollY` (13,
15), `pageXOffset` (12, 1,984), `pageYOffset` (12, 1,986), `scrollTo` (1,
2), `CSSStyleDeclaration` (37, 42), `focus` (1, 6). The history members
are not window misses, so the census cannot count them.

**The corpus replay** (230 pages Chris kept on the P5, 222 judged, run by
the orchestrator) found four failure groups that could have been this
slice's, each traced to its cause: a CDN's bot challenge (five sites)
dies at `crypto.getRandomValues`, before and after, its timer then
calling a callback (`window.qpPe1`) that the dead script would have made;
the only change is the address, which now carries the challenge's token
because `replaceState` runs and no script `load` event puts it back (both
booked). React's `getActiveElementDeep` (two sites)
now reaches `instanceof window.HTMLIFrameElement`, slice 0a's booked
interface. A site's analytics script now reaches `""+document.referrer` (slice 1a's
booked row) and `document.defaultView.focus()`, for which this slice
added window's `focus`/`blur`; checking every interface the slice hands
out added `CSSStyleDeclaration`. Two sites' unjudged records
are the record's own capacity (its facts past `YONDER_DIAG_FACTS_MAX`).

Host: the DOM host 3,810 / 22,416 → 4,709 / 36,995 checks, with 5,715
allocation sweeps; the scripted-page host 5,888 → 6,361 (its refetch
sweep cuts an arrival only up to the page's first script, below); all 0
failed.
libgarb's harness took 27 minutes with the media serializer as first
built, its allocation sweep paying the cost that doubled at every bracket
level (the cold read's P1); in one pass it takes 10 seconds. The mutant harnesses were not
run (the briefs' rule).

**The cold read** found nine, all fixed: the media serializer's cost
doubling at every bracket level (P1); a fetched Back losing the entry's
state, `hashchange`'s addresses and `popstate`'s state read when the event
ran rather than when it was owed, a fetched traversal popping by count,
and `history.back()` from an arriving page landing differently by when it
ran (P2s); the switch leaking through content attributes, the hash setter
navigating to the fragment it had, and a multi-step traversal stamping the
live scroll on every entry (P3s). The new cases found a tenth:
`history.length` counting a page arriving by a traversal as a new entry.
**Codex round 1** found seven, all fixed but one that did not reproduce:
a delta of INT32_MIN negated past int32_t into an out-of-bounds index
(P1; every delta's magnitude is now int64_t, and the class audit found a
scroll rounded after its clamp); `history.forward()` from an arriving
page dropped (`unshown_go` now answers both ways); a refetched page that
pushed before it was shown losing its target (`Page.arrives_at`); the
computed fallback's CSS-wide keywords, and its 511-byte cut; and a quirks
body under a boxless root. The hidden descendant's `rem` already read the
root's font; a case pins it. Its adversarial pass found a page that
pushed and went back among its own entries keeping P ahead
(`way_forget_ahead`), and a QuickJS crash compiling under a failed
allocation (booked).
**The second cold read**, of round 1's fixes, found: `history.forward()`
from an arriving page that had pushed abandoning it, where the push had let
go of what was ahead and forward is nothing; and `history.length`, read
by such a page, still counting what its push let go of; and a new way's
page going back past its own entries putting them ahead of what its commit
had let go of; and a page not yet shown that navigated by address doing so
from the page on screen (`g.replacing`), so a JavaScript redirector's
`location.replace` erased the page its link was on, and a redirector
reached by Back left its entry behind, where Back reached it again.
It also found that a replay's session history and window frame are the
harness's, which REPLAY.md § 3 now states, and Back from an arriving page
measuring its target before a held push joins the session (booked).
**The third cold read** found that "not yet shown" is not "not yet
completely loaded": yonder shows a page before its `load`, so a redirect
or a script's form in `onload` still pushed, and Back from where it went
reached the redirector again (`Page.loaded`); and a page at its
DOMContentLoaded whose replacing navigation could not start arriving
without the entries its scripts made, which it now takes back.
It booked the question bar answered for a page not yet shown; the round
also booked a page not yet shown sending the page on screen's Referer, and
a fragment navigation before load pushing.
**The design read** passed it with the scroll-restoration ruling, the two
accepted departures from HTML's traversal queue below, and the session's
being outside the switch.

**Merged with 1a and 4**: a MediaQueryList is one of 1a's plain event
targets (`d_event_target`, `d_event_handler`, `d_dispatch_plain`, the
host's `d_host_dispatch` at a plain target's entry). 1a's helper gained a
per-class resolver for it: a list's entry is made only when a listener
is added, its handler set or an event dispatched at it (`media_entry`),
and let go once nothing listens on it, at the next matchMedia or view step
(`media_release`), so a list nothing listens to is not held, however it
was listened to before; performance and screen.orientation keep theirs as
their opaque. The
interface objects are 1a's `d_interface_new`; 1b's own installer is gone.
The two `DOMException` rows are one. Slice 4's thirteen longhands
(`cursor`, `user-select`, `word-break`, `overflow-wrap`, `letter-spacing`,
`text-overflow`, the four `outline-*`, `object-fit`, `object-position`,
`aspect-ratio`) are computed from libflow's style (`aspect-ratio` as the
page declared its pair), since libgarb's table is the property list and
each must serialise (WPT's `serialize-all-longhands`). At the merge, with
4's properties parsed: `css/cssom` 722, `css/cssom-view` 248,
`css/mediaqueries` 118.

**Codex round 2**, after the merge, found seven. Fixed: `font-family`
computing to libflow's names and its fallback, where the computed value is
the declared list as written (`flow_family_list_t.written`); an event's
init dictionary that is a primitive accepted, now a TypeError for every
Event interface through libdom's one gate (`d_dictionary`); the
`scripting` media feature fixed at `none`, now the document's
(`garb_env_t.scripting`); and the device-* features answering the
viewport, now the screen's (`garb_env_t.device_width`, from `s_screen` and
the binding's `options.screen`), which put `css/mediaqueries` at 120.
Booked: a grid template's resolved value as the used track sizes. Declined
as what-ifs or as WebIDL's own rule: an options getter that scrolls the
window, and `scrollRestoration` refusing an unknown word (WebIDL ignores
it).

**The fourth cold read**, of the merge and round 2, found six. A
MediaQueryList was held for the page's life by any target operation on it
(a read of `onchange`, a removal, a dispatch) and never let go when its
last listener went, so a hook that makes a list and adds and removes a
listener every render kept every list; now an entry is made only to add, to
set a handler or to dispatch, and `media_release` lets it go, in one walk
of the registry, at the next matchMedia or view step. `resolution`
answered 1 at any zoom; it is the zoom now, as devicePixelRatio is, and
libgarb knows the WebKit pixel ratio. A quoted family name that spells a
keyword is written quoted, and a keyword in its canonical lowercase. And
three texts: slice 4's `-computed` pages wait on `CSS.supports` (booked),
not on getComputedStyle; a comment that headed the wrong function; and
what matchMedia's answer reads.

**Booked** (DEBTS.md § Classic browser scripting): computed custom
properties and shorthands; `DOMException` (raised with Chris as a small
libdom slice); frames and `window.open`; an entry's state living only as
long as its crumb; history deltas adding up to nothing; `scrollRestoration`
per document; `matchMedia` writing `calc()` as given; the out-of-memory
`not all`; a second traversal stopping the first; a stopped traversal
collapsing held changes; Back from an arriving page with nothing on
screen; from the corpus replay, `crypto.getRandomValues` and a script
element's `load` and `error`; from Codex's round, QuickJS compiling a
parse that ran out of memory; and from the second cold read, Back from an
arriving page measured before a held push joins; and from the third, the
question bar answered for a page not yet shown, a page not yet shown's
Referer, and a fragment navigation before load; from Codex's second
round, a grid template's used track sizes; and from the fourth cold read,
`CSS.supports`. In § The replay corpus:
a replay's session history and window frame.

### Slice 0c, as built

Built by Opus on `opus/tmw-0c-jquery`, from MODERN_WEB_BRIEFS.md § Slice
0c (the reviewed copy on `fable/modern-web`): the first slice the corpus
named rather than the plan. Read cold by a fresh Opus (six P3s: five fixed
on the branch, the sixth Slice 0d) and read for design. jQuery 3 died at
load on `document.implementation.createHTMLDocument`, and every script after
it on `jQuery is not defined`; a banking site's login was a `javascript:`
link yonder refused.

**What was built.**

- **`document.implementation`** (DOM § 4.5.1; libdom `implementation.c`),
  classic surface, on every page with the switch off too: one
  DOMImplementation per document, made when first asked for, with
  `createHTMLDocument(title)` (the standard tree; a title element only
  when a title is given, the empty string being one), `createDocument
  (namespace, qualifiedName, doctype)` (an XML document holding the doctype
  and then the element createElementNS would make, the name held to DOM's
  "validate and extract"), `createDocumentType` and `hasFeature()` (true).
  `Document.doctype`, and DocumentType, a prototype of its own with `name`,
  `publicId`, `systemId` and ChildNode. The `DOMImplementation` interface
  object is a Modern Web row; `DocumentType` joins the interface row.
- **A new document lives in the page's arena** (libhtml's
  `os64_html_create_document`, a second DOCUMENT node there under the DOM
  Standard's rules for a document's children and none of the window's own;
  `os64_html_create_doctype` beside it). A node made in it and appended to
  the page is MOVED, keeping its identity, wrapper, expandos and listeners:
  jQuery.parseHTML's nodes are the page's, found by its `querySelector`.
  This is the brief's open choice, taken for that reason (a second arena
  would make adoption a copy, and jQuery appends the exact nodes it
  parsed). It is freed with the page and charged to its caps; a change in
  it moves the page's version, so yonder rebuilds the page's model after
  that task.
- **Which document a node belongs to** is the document its tree hangs
  from; a detached tree's root's wrapper records the one it was made in,
  copied from or last taken out of (`DValue.document`, written by
  `d_made_in` and `d_leaving`). `ownerDocument` was the window's for every
  node before.
- **A document with no window**: `location` is null (assigning throws),
  it is cookie-averse, `write`/`writeln`/`open` throw InvalidStateError
  (a write would otherwise have reached the page's own parser), a link, a
  form, focus, blur or reset in it asks nothing of the host, a script
  connected to it is marked started and never runs, a handler attribute in
  it is not compiled, its body reflects no handler onto window, and its
  fragment parses and serializations run with scripting off
  (`d_scripting`). DOM's connected (`d_connected`, any document) and
  "in the window's document" (`d_in_window_tree`, what a script must be in
  to run) are two names.
- **The second wall**, jQuery 1.12.4's support probe
  `c.type="checkbox",c.checked=!0`: `type` was an expando, the input stayed
  text, and libpage refused its checkedness ("Control property refused").
  Inputs, buttons, selects and textareas reflect `type`, and libpage keeps
  every input's checkedness, as HTML does (jQuery's `serializeArray` reads
  `.checked` of every field). Past that probe 1.12.4 reads
  `e.attributes[name]`: the acceptance is met for 3.5.1 and moves to Slice
  0d for 1.12.4.
- **`javascript:` links** (HTML § 7.4.2.3.3): `follow_link_asked`, which a
  person's click and a script's `click()` both reach, hands such an
  address to the page's script host before libway is asked
  (`yonder_scripts_address`): the address after its scheme percent-decoded,
  then its fragment as libpage hands it over (decoded already), read as
  UTF-8, queued as a classic script with no element and run as one task,
  recorded as `script javascript:#inline-<n>` (YONDER_DIAGNOSTICS.md). With
  scripts off the link does nothing and the status line says so. A string
  result replacing the page is DECLINED.md's.

**The seams, as they ended.**

| Seam | What it is now |
|---|---|
| `os64_html_create_document`, `os64_html_create_doctype` | libhtml's: a second DOCUMENT node and a doctype in `doc`'s arena and mark; never inserted, charged until `doc` is freed |
| `d_node_document`, `d_window_document` | the node's document (its tree's DOCUMENT root, else its root's record, else the window's), and whether that is the window's; while no document of its own exists, every node is the window's and neither walks |
| `d_made_in`, `d_leaving` | the record written as a node is made or copied, and as a binding verb (`d_remove`, `d_replace`, `d_replace_content`) takes it out; only a wrapped node keeps one |
| `d_connected`, `d_in_window_tree` | DOM's connected (isConnected, a `form` attribute's lookup, the controls and names collections), and the window's tree (a script's start) |
| `d_scripting` | the page's scripting, for a node of the window's document: innerHTML's parse and serialization, insertAdjacentHTML's parse |
| `yonder_scripts_address` | a `javascript:` link's code queued as a classic script (`MODE_ADDRESS`, no element) |

**Numbers.** WPT `dom/nodes`, the baseline (858 passes of 4,406) → 1,095
of 4,969, none lost: pages that died at `document.implementation` now run
to their end. `[required]` 14 → 21, the 7 that need the DOMImplementation
and DocumentType interface objects; `--modern-off` passes (21 required,
none passing with the switch off). The 230 the slice made pass that pass
with the switch off too are in `[passing]`: a ruling against the brief's
"`[required]` grows by the ones this slice makes pass", since
`[required]` means "needs the switch" and `document.implementation` is
classic. What still fails there: DOMException codes, `compatMode` and
`characterSet`, the HTML element interfaces, elements in no namespace.

The census, `tools/yonder_tally.py --failed`: `createHTMLDocument' of
undefined` was 141 records over the two older pulls (84 and 57) and 51 in
2026-10-22b's (26 sites); a replay of all three at this slice has none.
On 2026-10-22b's replay, `Control property refused` 11 → 0, `'jQuery' is
not defined` 47 → 8, `'$' is not defined` 33 → 6. The surface's own names
are missing in 4 of the 886 kept records (`DocumentType`, 4 sites;
`DOMImplementation`, none).

The replay diff (REPLAY.md § 7) of 2026-10-22b, the slice's base against
its head: 56 pages of progress (259 lines lost), and 54 pages
"regressions" (908 lines gained), which are THE NEXT WALLS: 50 of them
carried one of the two walls before, and the other 4 are one site whose
jQuery died elsewhere and now boots. What the gained FAILED lines ask for:
`getComputedStyle` (26 lines), `matchMedia` (14), `document.referrer` (11),
`getClientRects` (3), `fetch` (3), `DOMParser` (1), a module (1), and
reads of undefined behind them. Three pages are newly unfinished: the
scripts that now run ask for scripts in an order and number the kept index
does not hold, so a blocking request goes unanswered (to record again,
DEBTS.md § The replay corpus); 6 were unfinished before and are still, 33
are to record again and 15 unjudged on both sides. The older pulls,
replayed at the head only, read more pages incomplete for the same reason
(2026-10-21: 147 of 340; 2026-10-22: 33 of 223).

Host, the slice's base → its head: libhtml's verbs 540,023 → 540,042
checks, libpage 254,138 → 254,141, the DOM host 6,841 / 51,471 → 7,676 /
53,450 (allocation sweeps 561 / 7,913 → 653 / 8,187), the scripted-page
host 6,186 → 6,206; all 0 failed. `tools/test_dom_implementation.inc` holds a case per
rule of LIBDOM.md § Documents of their own and runs jQuery 3.5.1 and
1.12.4, upstream copies, licence headers intact, in `tools/fixtures/`
(1.12.4 stops at the next wall, `attributes`, and says so); the corpus's
own 1.12.4 build carries its vendor's additions and is not in the tree:
it runs when `OS64_JQUERY_AEM` names a copy. `tools/test_dom_mutants.py
--only impl-` runs a mutant per rule of the slice, 21 of 21 caught; the
other mutant harnesses were not run (the briefs' rule), their anchors
checked against the changed files.

**Review.** The builder's own pass counted a new document from its making
and walked to a root only for a wrapped node. The cold read found no P1
or P2 (isolation and memory held under an ASan churn of 300 documents)
and six P3s, five fixed: a new document's body reflecting its handlers
onto window, a `javascript:` fragment decoded twice, a displaced comment,
the two meanings of connected under one name, and the fragment parser's
scripting taken from the page rather than the context's document. The
sixth, `Element.attributes`, is Slice 0d. The design read passed the
shared arena. **Codex's first round** found four P2s: a creation's status
read in the same argument list that wrote it, fixed; a windowless form's
reset not carried out, declined under the one-user rule, with its comment
made true and the gap booked; createDocument's name rule, declined as a
change (the standard dropped XML's QName in 2025, and WPT's table at the
pin holds the split at the first colon), its comment made exact and the
rule pinned; and an XHTML document's elements reading as an HTML
document's, booked, since the fix reaches the selector engine.

**Booked** (DEBTS.md § Classic browser scripting, unless named):
`Element.attributes`, for Slice 0d; `createElementNS` and elements in no
namespace; a new document's quirks mode; a detached root that never had a
wrapper reading as the window's; `javascript:` through location
assignment, and a link whose fragment decodes to U+0000; a windowless
form's reset; an XHTML document's elements reading as an HTML
document's; the three
newly unfinished kept pages (§ The replay corpus). Declined: a
`javascript:` result that is a string (DECLINED.md).

### Slice 0d, as built

Built by Opus on `opus/tmw-0d-attributes`, from MODERN_WEB_BRIEFS.md § Slice
0d (Fable's design, the reviewed copy on `fable/modern-web`): the wall behind
jQuery 1.12.4. Its focusin support probe reads `e.attributes[name].expando`
at load, libdom had no `attributes`, and the library and every script after
it died on the kept pages that carry it. Read cold by a fresh Opus (one P2,
two P3s, all fixed on the branch) after the builder's own adversarial and
truth passes.

**What was built** (libdom `attr.c`; LIBDOM.md § Attributes as nodes).

- **The surface**, classic, on every page with the switch off too:
  `Element.attributes` (a NamedNodeMap: `length`, `item`, `getNamedItem`/
  `NS`, `setNamedItem`/`NS`, `removeNamedItem`/`NS`, indexed and named
  properties, iteration), `document.createAttribute`/`createAttributeNS`, and
  Element's `getAttributeNode`/`NS`, `setAttributeNode`/`NS` and
  `removeAttributeNode`. The `Attr` and `NamedNodeMap` interface objects
  join the interfaces row.
- **An Attr is a view of its element, not a tree node.** A registry entry
  whose `target` is its element, with a record (`DAttr`) holding its
  qualified name and namespace, kept in a table by element and name, one per
  name, so the same object comes back while the attribute exists. Its value
  is read from libhtml by name at every read and written through the door.
  It is a Node: `Attr.prototype` (a prototype kind of its own, a Node's)
  answers every member `Node.prototype` has with the standard's answer for a
  node with no place in the tree, and the DOM host fails the build of any
  Node member it does not shadow. It is an EventTarget whose path is itself.
  The tree verbs refuse one as DOM does; a `Node.prototype` member called on
  one itself is the TypeError "an Attr is a Node with no place in the tree"
  (DIVERGENCES.md).
- **The map trusts no snapshot.** Every read answers the element's list as
  it is at the call. The last element walked is kept as an array of its
  attributes, used only while the list is provably the one copied (the same
  element, libhtml's version unmoved, the same first and last attribute), so
  `length` and the i-th are a step and every whole walk is linear. Names are
  unenumerable, an HTML element's names with an uppercase letter unlisted,
  and a name is hidden by the map's own expando and by the prototype chain
  (WebIDL's visibility), both asked before the list is read.
- **Detachment at one door.** Every attribute libdom sets or removes goes
  through `d_set_attr` to libpage's verb; a removal copies the last value
  into the Attr before the attribute goes (refused whole, QuotaExceededError,
  when there is no room) and detaches it. The parser removes no attribute
  and libpage none of its own, and the DOM host refuses a libdom that calls
  an attribute verb anywhere else. **The backstop:** an attached Attr whose
  attribute is gone reads as detached and empty, and the host hears
  `options.broken`, which yonder records as `FAILED page: an attribute left
  its element outside libdom` (YONDER_DIAGNOSTICS.md).
- **Setting an Attr.** `setAttributeNode` takes a detached Attr on (a
  replaced attribute keeps its place), answers the one it replaced detached
  with its value, answers one this element holds as it is, and refuses one
  another element holds (InUseAttributeError). libhtml's verbs carry no
  namespace, so an Attr is set only where the tree keeps its name and
  namespace both; elsewhere it is NotSupportedError.
- **The names thrown**, all named Error objects through `d_error` for a later
  DOMException to fold: TypeError, HierarchyRequestError, NotFoundError,
  InUseAttributeError, NotSupportedError, NamespaceError,
  InvalidCharacterError, QuotaExceededError.

**The seams, as they ended.**

| Seam | What it is now |
|---|---|
| `d_set_attr` | the one door for every attribute libdom sets or removes; a removal detaches the Attr with its last value |
| `D_PROTO_ATTR` | Attr's prototype, a Node's, with no Node member installed by node.c: attr.c shadows them all |
| `d_is_attr`, `d_node` | an Attr is told from a tree node; `d_node` names it in its TypeError |
| `os64_dom.attr_table`, `attr_cursor` | the attached Attrs by element and name; the last element walked as an array of its attributes |
| `os64_dom_options_t.broken` | a rule the binding keeps found broken, told to the host (yonder: a `FAILED page` line) |
| `d_attr_steps` | the walks' steps, for the DOM host's linearity cases |

**Numbers.** WPT `dom/nodes` 1,097 → 1,229 passes of 4,969, none lost;
with the switch off, 1,144 pass. `[required]` 21 → 83: the 62 the slice
makes pass that need the switch (NamedNodeMap's method-names test, and
Node-cloneNode's 61, whose checks compare attributes and read Node's
constants). The other 70 pass with the switch off too and are in
`[passing]`. What still fails there wants `setAttributeNS` and its kin,
`createElementNS`, a DOMException, or a namespaced `setAttributeNode`.

The census: `NamedNodeMap` was a missing global in 76 of 886 kept records
(23 sites) over the three pulls; `Attr` in none.

The replay of 2026-10-22b, the slice's base against its head (REPLAY.md
§ 7). The 16 pages the wall held (14 that died in jQuery 1.x with `TypeError:
cannot read property of undefined`, one of them jQuery 1.11, and 2 with
`'Symbol.iterator' of undefined` from `Array.from(el.attributes)`) all lose
their death line. On judged pages: `cannot read property of undefined`
28 → 17, `'Symbol.iterator' of undefined` 2 → 0, `'jQuery' is not defined`
8 → 0, `'$' is not defined` 6 → 0, `'length' of undefined` 26 → 21, a
missing `NamedNodeMap` 16 → 0. What those 16 ask for next: XMLHttpRequest
first (jQuery's ajax probes, on most of them, and a FAILED line on the
second bank's page, which still does not finish its parse in the replay's
60 s), then `document.referrer`, `getClientRects`, a JSON string the engine
calls a bad control character, and globals of their own and their ads.
The replay diff: 40 pages of progress (73 lines lost) and 24
"regressions" (91 lines gained), every one of them a page that also
progressed, its scripts running further to the next wall (jQuery's
XMLHttpRequest probes, `DOMParser`, interfaces not yet built, a `data:`
picture). Beyond the 16, 12 pages lost `'length' of undefined` (a read of
`el.attributes.length`). One page is newly incomplete, its scripts now
asking for sheets the kept index does not hold (to record again); 9 were
unfinished before and are still.

Host at the head: the DOM host 10,765 and 73,193 checks (allocation sweeps 961 and 11,253), its attribute probe (`--attributes`)
1,921 and 2,663; the scripted-page host 6,857; all 0 failed. On 1,000 attributes
each whole walk takes 1,982 to 5,964 steps (n² would be a million): an
iteration, `Array.from`, `Object.keys`, `for..in`, an index loop on
`length`, jQuery's countdown from `length`, and named walks either way
round. Both jQuery 1.12.4 copies boot: upstream's fixture, and the second
bank's build, which is not in the tree and runs when `OS64_JQUERY_AEM`
names a copy. `tools/test_dom_mutants.py` gains a mutant per rule
(`--only attr-`, 31); the mutant harnesses were not run (the briefs' rule),
their anchors checked against the changed files.

**Review.** The builder's adversarial pass found single calls doing n² work
on an element with n attributes, out of the engine deadline's reach (every
index walked from the list's start, every property read scanning every
name, an Attr found by scanning its element's), fixed with the table and the
cursor, and the cases refused at each allocation. The truth pass fixed four
comments. The cold read found no P1 (lifetime, the door, allocation failure
and the jQuery probes held, under two 30,000-operation ASan fuzzers) and:
a P2, the walks still n² (`length` walked the list at every call, which the
array iterator reads at every step, and the cursor served forward steps
only, so jQuery's countdown walked from the start; the harness missed it
because the length walk counted no steps), fixed with the array cursor; a P3,
a name listed twice when an expando shadowed it (WebIDL's visibility step
2), fixed; and a P3 of text, two DEBTS counts already stale, now page names.

**Rulings.** The `Attr` and `NamedNodeMap` interface objects are the
interfaces row's, as every interface object has been since 0a; the surface
is classic. The 60 Node-cloneNode tests stay in `[required]`: they need
Node's constants, which the switch gates. A Proxy prototype's `has` trap is
called for a name the map does not list, which WebIDL's order would not do:
a what-if no page meets, left.

**Booked** (DEBTS.md § Classic browser scripting): no namespaced attribute
verbs, and an Attr's namespace kept only where the tree keeps it; Node's
missing `getRootNode`, `isEqualNode`, `isSameNode`, `compareDocumentPosition`
and `normalize`, which an Attr will answer when Node has them; an Attr kept
until the page goes, about 270 bytes of the default budget each. Not built:
the brief's answers for those five Node members, Node having none.

### Slice 2, as built

Built by Opus on `opus/tmw-2-fetch`, from MODERN_WEB_BRIEFS.md § Slice 2:
a script's requests. The origin and CORS rules were written first as a
design (`docs/design/pending/ORIGIN.md`), read by Fable before any code
(eight rulings, six requirements, A to F), and are kept there as built;
this section is the record of the slice around them. Read cold by a fresh
Opus after the build, and the findings fixed one commit each.

**What was built.**

- **`libway/origin.c`** (`way/origin.h`), pure judgements with no I/O:
  an origin and its serialization, same origin and the same-site
  stand-in, the method and header lists, when a request is preflighted,
  the preflight's lines and its judgement, the preflight cache, the hop's
  decision and response type, the redirect-taint, credentials across
  redirects and `Authorization`'s strip, the CORS check, the exposed
  headers, the `Origin` header and mixed content. Every walk over a list
  a page or a server sizes is n log n, a server's list capped at
  `WAY_CORS_LIST_MAX` elements, a longer one read as a list that does
  not parse. ONE origin for the whole browser (ruling 8): 1a's
  `d_origin` is `way_origin_serialize`, and libway's load (a load's
  redirect count) and yonder's quirks-mode sheets compare through
  `way_origin_same`; their private copies are gone, and with them a
  comparison that took `http://a/` and `http://a:80/` for two origins.
- **libfetch**: a method by name (any token; the library then follows
  no redirect and ends `REDIRECT_STOPPED`), the four representation
  headers a page may set, every head field heard (`on_header`),
  `identity` for a Range, a HEAD's reply and a 205 bodiless whatever
  their framing says, and 8 KiB of caller fields (the per-hop request
  moved off the stack into the fetch object).
- **`yonder/fetch_job.c`, the job**: `yonder_fetch_run` drives the Fetch
  standard's main fetch hop by hop, asking origin.c every question, with
  libfetch asked one hop at a time. An opaque body is never read; a
  `data:` URL is answered without the wire under the same `max_body`
  (16 MiB) as a network one, its text capped too (requirement A); the
  head is kept whole up to 64 KiB or the fetch fails and says so. A
  hop's `Set-Cookie` is heard inside the open, before the CORS check can
  fail it (requirement C). The product handed up is FILTERED: libdom
  never sees a response it may not expose, and a network error carries
  nothing. ONE path for every caller: `fetch` and an asynchronous
  XMLHttpRequest are pool jobs, a synchronous one runs the same function
  inline on the window. Script requests bypass the HTTP cache (ruling 5).
- **libdom's five rows** (LIBDOM.md § Slice 2's rows): `DOMException`;
  `AbortController`/`AbortSignal`; `Blob`/`File`/`FormData`;
  `fetch`/`Headers`/`Request`/`Response`; `XMLHttpRequest` with its
  upload and `ProgressEvent`. A body crossing into the engine is the
  runtime's memory like any allocation a script makes (requirement B,
  the sticky LIMIT, with a case). A network error tells the page one
  fixed thing, "Failed to fetch"; the reason stays in the page's record.
- **One DOMException.** Every refusal the binding throws by a standard
  name, the older rows' included (1a's QuotaExceededError, 0c's and 1b's
  InvalidCharacterError, HierarchyRequestError and the rest), is a real
  DOMException with its legacy code once the switch has installed the
  interface; TypeError and RangeError are the engine's own; with the
  switch off a refusal is an Error carrying its name, as before.
- **One EventTarget.** Every plain target registers through
  `d_event_target` with its resolver; where its entry lives (the
  object's opaque, a resolver's on-demand registry entry as 1b's
  MediaQueryList has, or in the object for a class a page makes per
  request: AbortSignal, XMLHttpRequest and its upload) is the class's
  choice, documented once in internal.h, and the listener records, the
  methods and the dispatch are the same for all three.
- **The WPT runner serves two sites** (`wpt.test`, its subdomains, and
  `wpt-alt.test`) from the one checkout, fills wptserve's host and port
  templates, and answers a fetch job's request as wptserve answers a
  static file, its `.headers` sidecars included; a `.py` handler is a
  network error. The page record has a line per request
  (`request GET <address>: 200 cors, 0 hops`, or a FAILED line with the
  reason).

**The seams, as they ended.**

| Seam | What it is now |
|---|---|
| `os64_dom_set_network` (`start`, `run`, `release`, `cancel`) | the binding's host for a script's requests: `start` submits one and answers an id (0 is a network error), `run` runs a synchronous one to its end on the window thread, handed the `stop` below |
| `os64_dom_fetched` | an answer, as one task: the bytes copied into the runtime's memory, the promise settled or the XHR's events fired, the microtasks run |
| `yonder_scripts_options_t.request/.run/.release`, `yonder_scripts_answered` | the same, through the scripts host; a page's departure retires its runtime, whose drain cancels the requests still out |
| `yonder_fetch_run`, `yonder_fetch_release` | the job, a pool job's `run` or called inline; its cancellation predicate is the pool's or the script task's |
| `way/origin.h` | every origin and CORS question, one copy; libdom's guards ask the same lists the job does |
| `way_preflight_t` | the browser's preflight cache, every page's, with its own lock |
| `DValue.listeners_changed` | the plain target's listener-change hook; called after every add, removal, handler set and once-removal; abort.c is its one reader |
| `d_signal_fetch_answered`, `d_signal_fetch_settled` | a fetch's abort algorithm and its answered Response, knowing each other by C pointer both ways |
| `tools/test_fetch_fake.inc` | libfetch faked by its contract, recording each request, for the job's host, the scripted-page host and the WPT runner |

**The locks.** A hop takes the jar's lock (choosing cookies, hearing
them) and the preflight cache's (asking, storing, clearing), each
briefly and NEVER one inside the other, and holds neither across
libfetch or across a call back into the job. A synchronous XHR runs the
job on the window thread, so nothing on the window thread holds either
while it runs: the window takes them only through those same calls.
libfetch's own credential rule (keep `Cookie` and `Authorization` to
the TYPED origin) never fires inside a job, because each hop is typed
afresh (ruling 2); the standard's rule replaces it there and libfetch
keeps its own for its other callers.

**A synchronous XMLHttpRequest and a script out of time are one
mechanism.** The run is handed a `stop` that is the script task's own
budget (`os64_js_check_budget`: the task's deadline, Settings' script
seconds, or libjs's cancel), asked by the job before every hop and by
libfetch before every wait. A request that outlives its task fails as a
network error and the task ends LIMIT/EXECUTION, as any script out of
time does; what ends a script ends its request, through the same door.
Nothing polls the window while it runs, so no event is taken early,
deferred or reordered: the person's input waits in the window's queue
as it does behind any running script. The bound is the task's budget
plus at most one of libfetch's waits (its 30 s idle deadline), since a
wait is not interrupted; booked.

**AbortSignal's weak sets.** "Source signals" and "dependent signals"
are DOM's weak sets: a link is a C record on both lists holding neither
object, each signal's finalizer unlinks itself from the other side (a
collection may take the two in either order), and no engine internals
are read (the build before this read QuickJS's private refcount).
Dependents are flattened and kept in link order, the order an abort
runs them. DOM's GC clause keeps a dependent someone could still
observe: while it has a live `abort` listener or a pending algorithm and
is not aborted, its links hold it, and the hold is released at exactly
three points, each pinned by a DOM host case: (1) the dependent is
aborted (by any source, so every other source lets it go); (2) its last
abort listener goes, which the listener-change hook reports and nothing
else reads; (3) its source aborts. Ten thousand dropped `any()`
dependents and ten thousand dropped Request signals leave the set empty
after a collection. A Response holds its request's signal until its body
settles, so an abort after the answer still errors the body. Making the
links is linear in what the page hands over: a dependent is linked only
by the call that makes it, with no page code between its links, so a
duplicate is one comparison (the source's last link) and a dependent
source is flattened once per new dependent. The DOM host counts the
steps (`d_signal_steps`): `AbortSignal.any` of 65,536 distinct signals
takes 131,072 (it took 2^32 when each link walked the dependent's
sources), 1,024 copies of a 1,024-source dependent 2,048 (538 million
before).

**Libjs patch 0010.** The merge of 0b, 1a, 4, 1b and 0c shifted the
scripted-page host's allocation numbering onto an upstream bug: a
failed allocation while the parser emits bytecode leaves the buffer's
sticky error unread, and `js_create_function` transformed the cut-short
bytecode (an ASan SEGV in 1b's `loop_view_history_short`, cut 17 of
138). Fable's ruling put the check at `js_create_function`'s entry,
before any transform and before the child loop; a failed emit is now an
out-of-memory exception. Two proofs: `test_js_runtime_host.sh
--compile-failure` cuts each allocation of a script with a nested
function on the target core (every cut is the value or an InternalError,
some "out of memory"; without the patch the cuts answer "bytecode buffer
overflow" and the case fails), and the scripted-page host is green,
with `loop_view_refetch_short` now cutting through its scripts' compile.
The instrumented engine reaches four more unchecked parser paths, one
an assertion; DEBTS.md's row replaced the one the patch paid.

**Numbers.** WPT, before the slice → its head (passes of tests run;
`[required]` is what needs the switch, none of it passing with the
switch off):

| Directory | Before | After | `[required]` | `[excluded]` pages |
|---|---|---|---|---|
| `fetch/api/headers` | 0 | 222 of 287 | 214 | 0 |
| `fetch/api/request` | 0 | 364 of 496 | 284 | 9 |
| `fetch/api/response` | 0 | 186 of 277 | 165 | 2 |
| `fetch/api/abort` | 0 | 65 of 79 | 63 | 4 |
| `fetch/api/basic` | 0 | 148 of 392 | 106 | 3 |
| `xhr` | 1 | 345 of 1,188 | 220 | 0 |

The excluded pages need what yonder has none of: service workers, the
Cache API, frames and other realms, MediaSource, a streaming upload.
Most of what still fails needs a `.py` handler, `ReadableStream`, a
document from a response, or the runner's port (DEBTS.md). Two
subtests left `[required]`: "Consume empty FormData request (and
response) body as text" expect no bytes, while every engine sends the
close delimiter for an empty FormData and fails them, as yonder now
does. The one
DOMException made other slices' pages pass: `dom/nodes` 1,097 → 1,401
(the pre-insertion checks, classList, createDocument's names), and with
Slice 0d merged, 0d's 1,229 → 1,537 (its attribute refusals become real
DOMExceptions too), `user-timing` 466 → 538, `css/cssom` 729 → 738, the
history interface 52 → 54, each written into `[passing]`. A script
that can fetch also ran other slices' pages to their end: `url`'s pages
that load their test data by fetch() (urltestdata.json, IdnaTestV2.json
and the like) stopped at "Loading data" before, and 4,074 of their
tests pass now (150 → 4,224), and 20 of `encoding`'s, which decode
through XMLHttpRequest, pass too (3,955 → 3,975). Every one fails with
the switch off ("'fetch' is not defined"), so they are in `[required]`
under a slice 2 heading. Every one of the 22 expectation
directories passes at the head, and with `--modern-off`.

The census, before the slice (the P5's kept pages of 2026-10-21 and
2026-10-22, 601 records from 130 sites): 346 records from 85 sites name
at least one of the seven, by name `XMLHttpRequest` 293 records (68
sites), `fetch` 147 (49), `AbortController` 38 (11), `Headers` 21 (4),
`Request` 17 (3), `FormData` 16 (6), `Response` 14 (1). The acceptance
on the P5 (the chat page the plan names, and the census pages asking
for `fetch`) is Chris's to run. In QEMU (`make run-net`, requirement D),
a page on one origin fetched JSON from a second with a preflighted
header (the OPTIONS asked for `x-probe` and was granted), an
XMLHttpRequest read the same answer, and a request to an endpoint
without `Access-Control-Allow-Origin` rejected with a TypeError; the
page record said `200 cors, 0 hops` for the first and named the CORS
refusal for the last.

Host, at the head, all 0 failed: origin.c's 151 cases (ORIGIN.md's
list, one each) with their step bounds, 432 checks; libfetch's host 907
checks; the job's host (`test_yonder_fetch_job_host.sh`, 319 checks,
every allocation failed in turn and each one failed alone); the DOM
host 83,004 checks with 12,734 binding allocation sweeps (Slice 0d's
included), `tools/test_dom_fetch.inc` holding the rows; the scripted-page
host with `tools/test_yonder_requests.inc` end to end. Mutants by hand,
one per finding fixed after the cold reads and Codex's rounds (each
named in its commit);
the mutant harnesses were not run (the briefs' rule).

**Review.** Fable read the design before code and ruled eight questions
(ORIGIN.md § The rulings) and six requirements, and ruled patch 0010.
The cold read found two P1s, a P2 and a P3, each fixed in its own
commit: a no-cors request could carry a body's non-safelisted
`Content-Type`, because the binding wrote its own headers past the
guard (every header the binding writes now goes through it, with a case
and a mutant); a network error's reason reached the page (it is the
fixed message now); a synchronous XMLHttpRequest had no stop, so a
server trickling bytes froze the window without bound (the mechanism
above, with a case and a mutant); and the preflight cache's lock
comment claimed more than the lock held. Merging the slices that landed
first raised seven questions the builder put to Fable, and all seven
stood: weak sets with no engine internals, honouring DOM's GC clause at
the three release points; one EventTarget; one origin comparison; one
DOMException; a network error that tells the page nothing; and the
synchronous request's stop as the task's own budget. A second cold read
found a quadratic in a signal's links (now linear, held by step counts)
and three P3s. Codex's first round found one P1 and eight P2s: the P1
(an interim head's fields) already held in libfetch and is now pinned;
four were fixed; two were declined with the standards' own words and
their rules pinned (a known zero is not lengthComputable; a data: URL's
`;base64` follows spaces only); two were declined as what-ifs. Its
second round found ten P2s, nine fixed (among them a preflighted URL
kept before its grants, the multipart parser's short headers, whole
delimiters and empty body, a body's stream closing at its one chunk,
the data: URL's MIME type serialized whole, the quoted-comma-aware
"extract a MIME type", and a Request's referrer started over by a
non-empty init) and one declined with WebIDL's enumeration setter rule.
Fable then read origins and CORS against the tree and approved them,
with two text items, both done: the "no Origin on HEAD" row was the
standard's own rule and went, and a redirect's Referrer-Policy was
booked. Slice 0d merged in after that (its attribute errors now real
DOMExceptions, with a case for their codes), and Codex's third round
found six P2s: five fixed (a multipart preamble skipped, a part header
read whole or the body refused, a body's type taken by "extract a MIME
type" serialized, an errored body's cancel() answering its error, a
URLSearchParams body's author charset said as UTF-8) and one booked
(XMLHttpRequest.send(document)). Its fourth round found four P2s, all
fixed: XMLHttpRequest's final encoding computed apart from its final
MIME type, as XHR says, and the final MIME type kept whole (a blob's
type and the override with every parameter); a multipart parameter's
quoted value passed over whole; and a urlencoded field's leading BOM
kept. Its fifth found three: Blob.slice()'s offsets converted as
[Clamp] long long before their sign is judged, an Accept the page set
empty sent empty (libfetch sends an empty Accept for one), and the
author charset rewrite made through libway's walk, so a quoted ';' is
the value's. Its sixth found three: each preflight branch cleared under
the redirect-taint it was made with (a later hop's taint had let an
earlier grant survive a network error), a body with no bytes a stream
closed at its creation, and a Request made from a Request taking the
input's body with its stream locked.

**Booked** (DEBTS.md § A script's requests, unless named): the same-site
stand-in; SameSite not enforced for script requests; the HTTP cache
bypassed; the synchronous request's bound; no `ReadableStream` or
streaming bodies; no document from a response; Subresource Integrity
failing closed; `FormData(form)` reading the window's forms only; an
address libpage's grammar refuses; a clone's body not reached by an
abort; an XHR the host would not start erroring inside `send()`; a
request the pool has no slot for; non-enumerable operations; QuickJS's
Proxy `ownKeys` duplicates; the WPT runner's port; a released reader's
`closed` not marked handled; a redirect's `Referrer-Policy` not applied;
`XMLHttpRequest.send(document)` sending the object's string.
QuickJS's four further parser paths (§ libjs) become libjs patch 0011, a
small PR of its own right after this slice merges (Fable's ruling); the
row stays until it lands. The deliberate departures from the standard are
DIVERGENCES.md § The browser's script requests.

### P1-W, as built

Built by Opus on `opus/tmw-p1w-window`, stacked on slice 2, from the P1-W
brief and WINDOW_DOCUMENT.md § P1-W (the plan, slice 0e's inventory, read by
Fable; its three fixes are this branch's first commit). The builder's
adversarial and truth passes are done; the cold read is next.

**What was built.** The 27 members of § P1-W, and the three classic fixes
(LIBDOM.md § P1-W's rows).

- **The window as a browsing context** (libdom `browsing.c`): `opener`,
  `closed`, `close()`, `clientInformation`, `moveTo`, `moveBy`,
  `resizeTo`, `resizeBy`, `captureEvents`, `releaseEvents`,
  `frameElement`, `crossOriginIsolated`, `originAgentCluster`, `origin`,
  `isSecureContext`, `queueMicrotask`, and the `ononline`, `onoffline` and
  `onlanguagechange` handler slots. Each answers what HTML gives the one
  document of a top-level window the person opened. `isSecureContext` is
  libway's judgement of the page's address, made public for it as
  `way_url_trustworthy` rather than copied (ORIGIN.md § 12; the name
  `localhost` is not trustworthy, as slice 2 ruled). `close()` never
  closes the window, where HTML would let a page close one whose history
  holds a single entry (DIVERGENCES.md). `queueMicrotask` is a job on the
  engine's one queue, so it runs in order with promise reactions, and a
  callback that throws is the failure of the task that drained it, never
  the script's that queued it, the jobs after it still running.
- **The six BarProps** and `BarProp`, `visible` true; **`external`** and
  `External`, whose two methods do nothing.
- **`crypto`** (`crypto.c`) and `Crypto`: `getRandomValues` and
  `randomUUID`, without `subtle` (ruling 1). The bytes come through a host
  hook, `options.entropy`; yonder's is libtls's `/dev/random` reader, made
  public as `os64_tls_entropy` so there is one reader, not two. The hook
  writes into the binding's scratch and the page gets the bytes only when
  the hook says all are there: a refusal (an unseeded pool refuses rather
  than waits) is an OperationError, never zeros.
- **The classic fixes**: `window` and `document` [LegacyUnforgeable], own
  accessors of the global with no setter, defined in one place (core.c);
  `history` a read-only accessor; `status` keeps the last string set.
- **The WPT runner** loads a page whose name says `.https.` over https, as
  WPT's own runner does, so a `[SecureContext]` member is there to test.

**The frame rule.** P1-W follows IFRAME.md § What the open slices do now
(Fable's frame design): `frameElement`, `length`, `top`, `parent`,
`origin`, `isSecureContext` and `originAgentCluster` are each asked of one
function of the window's browsing context (`browsing.c`'s
`context_answer`), none installed as a constant, so the frame slice
teaches that function what a frame's context answers. Today every context
is a root: null, 0, the window, the window, the page's origin, the page's
address judged by `way_url_trustworthy`, and true. `top`, `parent` and
`length` were core.c's data properties (the window itself and 0); they
are now accessors there too, every page's whatever the switch says, `top`
unforgeable and the other two [Replaceable]. `randomUUID`'s
`[SecureContext]` asks the same context (`d_secure_context`). P1-W's one
yonder change, the entropy hook, holds no page and adds no file-level
static (that document's rule for every yonder change).

**Classic or modern.** The 27 members are Modern Web rows, four of them:
they are globals the arc adds to window, which rule 4 says the switch takes
away. The three classic fixes are corrections to members every page has
had, and hold with the switch off, as 0c's and 0d's surfaces do. The
interface objects `Crypto`, `BarProp` and `External` are modern, each in
its members' row, as 1a's `Storage` is with `localStorage`, rather than in
the first row with the node interfaces: a row whose instances need the
prototype makes it.

**The seams, as they ended.**

| Seam | What it is now |
|---|---|
| `os64_dom_options_t.entropy` | `length` bytes (at most 65,536) of the host's randomness into the binding's scratch, true when all are there; NULL refuses every ask |
| `os64_tls_entropy` (libtls, `tls.h`) | `/dev/random` read until every byte is in, false for a source that cannot be opened or runs short; the TLS engine's entropy callback and yonder's hook both call it |
| `way_url_trustworthy` (libway, `origin.h`) | Secure Contexts' potentially trustworthy URL, the one copy: mixed content, the referrer's downgrade and `isSecureContext` ask it |
| `d_interface_class` (classic.c) | an interface whose objects the binding makes: its class, its held prototype and its Illegal-constructor interface object |
| `d_view_kind` (encoding.c) | WebIDL's ArrayBufferView told by class: a typed array's kind, a DataView, or neither |
| `D_EVENT_ONLINE`, `_OFFLINE`, `_LANGUAGECHANGE` | event kinds the binding names and nothing fires, so window has handlers for them and the body reflects them |

**Numbers.** WPT, the base (`a4dda013`) → the head (passes of tests run;
every `[required]` test fails with the switch off):

| Directory | Before | After | `[required]` | `[excluded]` pages |
|---|---|---|---|---|
| `html/webappapis/microtask-queuing` | 0 | 5 of 8 | 5 | 0 |
| `WebCryptoAPI` (`getRandomValues`, `randomUUID`) | 0 | 33 of 42 | 33 | 166 |
| `html/dom` (`self-origin.any.js`) | 0 | 1 of 1 | 1 | 247 |
| `html/browsers/the-window-object` | 74 | 75 of 659 | 1 | 0 |
| `secure-contexts` | 0 | 0 of 35 | 0 | 0 |
| `hr-time` | 8 | 9 of 19 | 9 | 0 |

What still fails: getRandomValues' nine "Large length" tests, which want
the QuotaExceededError interface WebIDL gave its own `quota` and
`requested` (DEBTS); `queue-microtask-exceptions`, which reports through
`window.onerror` (P2a); every page that opens a second window or a frame,
which is all of the-window-object's P1-W pages (`BarProp`, `closed`,
`close`, `self-et-al`) and all of secure-contexts; and
`window-properties.https.html`'s window members, which read
`Window.prototype` (P3c). WebCryptoAPI's other pages are SubtleCrypto's and
html/dom's other pages are later phases', each excluded by name. Every one
of the 27 expectation directories passes at the head, both ways.

The corpus, 2026-10-22b replayed at the base and the head
(`2026-10-22b_p1w-before` and `-after`): P1-W's names were MISSING in 89
of 202 page files (45 of 87 sites, 342 asks: `origin` 56 files, `crypto`
34, `opener` 20, `queueMicrotask` 18, `frameElement` 13,
`crossOriginIsolated` 1, `external` 1) and are in none; the FAILED lines
that named one (`'crypto' is not defined` 6, `'queueMicrotask' is not
defined` 3) are gone, and all FAILED lines 437 → 428. Replayed 239, clean
21, incomplete 24, unfinished 13 and 19 unjudged on both sides. The replay
diff: 89 pages of progress, and 13 "regressions". Five are noise: jQuery's
expando, a name made from `Math.random`, differs between any two runs, and
those pages are on the progress list too. Eight are next walls: scripts
that died at `crypto`, `queueMicrotask` or `origin`, or before them, now
run on to a later failure (a bundle that stopped at `require` stops at
`initCustomEvent`; a site builder that stopped at `crypto` and
`queueMicrotask` stops at its loader's initialization event; an identity
library stops at a read of `nonce` where it stopped at `_DumpException`,
having found `crypto`; an obfuscated
challenge script gets past its `crypto` check to a read of undefined).

Host, the base → the head: the DOM host 11,667 / 82,990 checks → 12,285 /
85,476 (allocation sweeps 1,029 / 12,734 → 1,093 / 13,109),
`tools/test_dom_window.inc` holding a case per rule; the scripted-page
host 7,547 checks with patch 0011a merged in; libway's origin, way
and way-fetch hosts, libfetch's host and its transport's, yonder's host,
its stream and fetch-job hosts, the webfont, os64get and html-dom hosts,
the replay proof, the WPT runner's self-test, the TLS input and engine
suites and both TLS audits, all 0 failed; `make -C userland` builds.

**What a read of `document` costs.** Making `document` and `history`
accessors put a getter on reads a page makes on nearly every line, and the
first getters went through the binding's whole callback check and, for
`document`, a wrapper lookup. Each now answers a value its function holds
(`d_held_getter`) after one check that the binding is open. A million bare
reads in the DOM host (`tools/test_dom_host.sh --window-reads`, the
target-core profile, the bindings sanitized as the host builds them):

| read | as first built | now |
|---|---|---|
| a plain global | 17.6 ms | 16.3 ms |
| `window` | 58.0 ms | 31.0 ms |
| `document` | 556.9 ms | 60.3 ms |
| `history` | 353.7 ms | 60.9 ms |

**The scope guard stopped the build once, and libjs patch 0011a is why it
could go on.** As first built, the scripted-page host failed in one case,
`loop_view_refetch_short`, which fails every allocation of a page's
arrival in turn, its scripts' compiles included: at one cut the engine's
`add_closure_var` failed inside `resolve_scope_var`, the bytecode was
emitted anyway, and `resolve_variables` read garbage (an ASan SEGV).
That was one of the unchecked parser paths DEBTS booked for libjs patch
0011; P1-W's new atoms and objects had moved the engine's arena
boundaries so that a cut landed on it, and the base passed. An engine
change is outside P1-W's scope guard, so the builder stopped and reported
it with nothing worked around. The path became patch 0011a (its own PR),
merged into this branch: the scripted-page host is green at the head,
`loop_view_refetch_short` and the end-to-end cases
(`tools/test_yonder_window.inc`) among it. The DOM host's engine sweep
still leaves out a handler compile, for `resolve_labels`, which patch
0011b owes (DEBTS).

**Review.** The builder's adversarial pass looked for page code reaching
the binding's state (a view's kind is its class, never its getters; the
entropy hook is copied out only whole), work a page can size (the fill is
capped before the host is asked; a microtask flood is the job cap's and
the deadline's, as a promise flood is), a page breaking the unforgeable
pair (delete, redefine, `let`, strict assignment, all pinned by cases), and
yonder blocking on entropy (an unseeded pool refuses). It found nothing to
change; the cases pin each answer. The truth pass corrected `close()`'s claim in
WINDOW_DOCUMENT.md (HTML does let a page close a one-entry window),
`isSecureContext`'s row there (it listed `localhost` and `file`), and
`os64_dom_listens`' comment in dom.h (the types it counts are no longer
only the ones a host dispatches).

**Rulings taken by the builder, for the cold read.** `print()` is class
(a) in WINDOW_DOCUMENT.md but in no phase: it was (d) when the phases were
counted, and P1-W was approved at 27; it is a one-line no-op for whichever
PR next touches window. `defaultStatus`, which no standard has, keeps
reading empty. html/dom is checked out whole for one test, its other 247
pages excluded by name: a later phase un-excludes what it names.

**Booked** (DEBTS.md § Classic browser scripting): the QuotaExceededError
interface. Not booked, by ruling: `subtle` (ruling 1), and `close()`
(DIVERGENCES.md).

### P1-Da, as built

Built by Opus on `opus/tmw-p1da-document`, from `userland` at `c8d87b53`
(#337 merged), from WINDOW_DOCUMENT.md § P1-Da (the contract) and Chris's
addition of CustomEvent. The builder's adversarial and truth passes are
done; the cold read is next.

**What was built.** The 25 members of § P1-Da, the classic fix
(`document.body`'s setter), and CustomEvent with createEvent's table
(LIBDOM.md § P1-Da's rows):

- **The document's state** (libdom `document.c`, a row of the Modern Web
  table that puts nothing on window): `readyState` and
  `onreadystatechange`, `visibilityState`, `hidden` and
  `onvisibilitychange`, `prerendering` and `onprerenderingchange`,
  `wasDiscarded`, `referrer`, `activeElement` and `hasFocus()`,
  `currentScript`, `title` and its setter, `domain` and its setter, `URL`,
  `documentURI`, `compatMode`, `dir`, `characterSet`, `charset`,
  `inputEncoding`, `contentType`, `lastModified`, `rootElement` and
  `clear()`. Each answers for the document it is asked of; a document of
  its own answers what HTML gives one with no browsing context
  (`complete`, `hidden`, no referrer, no focus, `about:blank`, its making's
  content type).
- **`document.body`'s setter**, HTML's: a body or frameset replaces the
  body where it is, or is appended to the root; anything else is a
  HierarchyRequestError (a non-HTML-element a TypeError), through the verbs
  appendChild uses.
- **CustomEvent** (`event.c`, a row of its own): the constructor, `detail`
  (held in the Event's own record and traced with it, never a property a
  page can reach) and `initCustomEvent`. **createEvent** reads DOM's table
  (below).

**The page in hand** (IFRAME.md § What the open slices do now). Every
per-document answer comes from the host of the page the asking document
is: the readiness is the page's script host's (`scripts.c`), the referrer
and Last-Modified come from the stream that is fetching that page
(`stream_request_facts`, from `g.stream`, which `scripts_host` already
reads for the timing), `currentScript` is set by that page's own `run_item`,
and the focus answer compares the asking document with the window's
focused one (`script_focus`), so a page not yet shown has no focus and no
`hasFocus()`. No new file-level static; the stream gained two fields (the
address its navigation was asked from, and for a Reload the reloaded
page's own referrer, which the page replacing it keeps though the Reload's
request names nothing; a Reload is always of the page on screen).

**How readyState tracks the host's dispatches.** One rule, one copy:
`os64_dom_readiness_after` (dom.h) says DOMContentLoaded at the document
moves `loading` to `interactive` and load at window moves either to
`complete`. `os64_dom_dispatch` moves the binding's readiness by it before
anything else, heard or not, and with the switch on fires
`readystatechange` at the document as a task of its own just before the
event, folding the two outcomes into one. yonder's host would not call the
binding for an event nothing listens to, so it moves its own readiness by
the same rule at every dispatch it is asked for, calls the binding when
the readiness moved even if nothing hears the event, and hands the
readiness to a binding it makes late (a page whose first script is a click
after its load is born `complete`). The divergence, booked: yonder runs a
page's deferred scripts before its DOMContentLoaded, so they read
`loading` where HTML's are `interactive`.

**createEvent's table, as built** (DOM § 4.5, matched without regard to
ASCII case, with the switch on):

| Asked | Made |
|---|---|
| `event`, `events`, `htmlevents`, `svgevents` | Event |
| `customevent` | CustomEvent |
| `hashchangeevent` | HashChangeEvent (its `oldURL` and `newURL` "") |
| `touchevent` | NotSupportedError: on the table only where the legacy touch APIs are exposed, which a desktop does not do |
| `beforeunloadevent`, `compositionevent`, `devicemotionevent`, `deviceorientationevent`, `dragevent`, `focusevent`, `keyboardevent`, `messageevent`, `mouseevent`, `mouseevents`, `storageevent`, `textevent`, `uievent`, `uievents` | a plain Event (the builder's ruling, below) |
| anything else (`popstateevent`, `progressevent`, `foo`, `UİEvent`) | NotSupportedError |

With the switch off `createEvent` makes a plain Event whatever it is
asked, as before the arc, so the refusal goes with the switch.

**Classic or modern.** The 25 members and CustomEvent are Modern Web rows:
with the switch off none exists and no `readystatechange` fires. The body
setter is a classic fix, every page's.

**The seams, as they ended.**

| Seam | What it is now |
|---|---|
| `os64_dom_options_t.readiness` | where the document stands as the binding is made; `LOADING`, the zero value, during the parse |
| `os64_dom_readiness_after` (dom.h) | the readiness rule, pure; the binding's dispatch and yonder's host both move by it |
| `os64_dom_options_t.referrer` | `document.referrer`, copied as the binding is made (`d_document_setup`) |
| `os64_dom_options_t.last_modified` | the response's Last-Modified in epoch seconds, `OS64_DOM_NO_DATE` for none |
| `os64_dom_options_t.focus` | the host's focused element in the window's document and whether the document has focus, asked at every read |
| `os64_dom_set_current_script` | the script the host is running, held while named |
| `way_head_t.last_modified` (libway) | the response's Last-Modified as written, copied from libfetch's head |
| `yonder_scripts_options_t.referrer`, `.last_modified`, `.focus` | the same, from yonder's page in hand |
| `D_EVENT_READYSTATECHANGE`, `_VISIBILITYCHANGE`, `_PRERENDERINGCHANGE` | the document's handler types, with no content attribute (`content_attribute` in event.c) |

**Numbers.** WPT, the base (`c8d87b53`) → the head, passes of tests run
with the same exclusions on both sides (every `[required]` test fails with
the switch off):

| Directory | Before | After | `[required]` | `[excluded]` pages |
|---|---|---|---|---|
| `html/dom` (its `documents/` pages now run) | 33 of 115 | 79 of 115 | 39 (+ P1-W's 1) | 230 |
| `page-visibility` | 0 of 8 | 8 of 8 | 8 | 8 |
| `html/interaction/focus` | 10 of 120 | 13 of 120 | 2 | 168 |
| `dom/events` | 167 of 461 | 170 of 461 | 2 | 114 |
| `dom/nodes` | 1,537 of 4,969 | 1,718 of 4,969 | 190 (+ 0a's) | 0 |
| `xhr` | 346 of 1,188 | 346 of 1,188 | 37 renamed | as before |

What the numbers say:

- html/dom's base passes include 21 of `document.title-05`'s and one of
  `-03`'s, which an expando passes with no title at all, and ten of
  `Document.body`'s classic getter; [required] leaves those out. What still
  fails there wants something else: `createElementNS` (P1-Db), the
  HTML element interfaces (`HTMLBodyElement`, `HTMLTitleElement`),
  `DOMParser`, an XML document in no namespace, and `Document.currentScript`'s
  page, whose harness times out on its `data:` and cross-origin scripts and
  its frame, and whose DOM-inserted inline script runs after the
  `appendChild` that connected it rather than inside it.
- html/interaction/focus's one P1-Da page (`document-level-apis`) runs 2 of
  3; the third is `focus()` taking effect inside its task (the scope guard,
  below). Its frame test never starts, and `document-has-system-focus`
  opens a window.
- dom/nodes' `Document-URL`, `Document-characterSet-normalization-*` and
  `Document-contentType/` pages, which the contract named, load frames,
  and run no test here; dom/nodes' gain is createEvent's table (Document-
  createEvent's aliases and refusals) and the documents of their own's
  content types. It also LOST nine: `Node-baseURI`'s tests passed while
  both `node.baseURI` and `document.URL` read undefined, and fail now the
  URL is there (DEBTS.md books `baseURI`).
- xhr's required tests are named by their page's title (`undefined
  (CONNECT)` while there was none); the expectation file names them by the
  titles they have now.

- Elsewhere, css/cssom-view gains one (`compatMode` in quirks mode) and
  encoding two (`characterSet` after a BOM and without sniffing).

Every expectation directory (30) passes at the head both ways.

The corpus, 2026-10-22b replayed at the base and the head
(`2026-10-22b_p1da-before` and `-after`). The records census window
globals only, so of P1-Da's names only CustomEvent can be MISSING: it was
in 46 page files (73 asks) and is in none. The FAILED lines that named a
P1-Da member are gone: `'initCustomEvent' is not a function` 11 → 0 (in 11
records; the banking SDK's common.js is in 3 of them and now runs) and
`'CustomEvent' is not defined` 16 → 0. Over the 232 pages judged on both
sides, FAILED lines 383 → 233. Replayed 239 → 232, clean 21 → 21,
incomplete 24 → 37, unfinished 13 → 13, unjudged 19 → 26. The diff: 57
pages of progress and 34 "regressions", all next walls (pages that died
at `currentScript`, `referrer`, `readyState` or `CustomEvent` now run on
into `ResizeObserver`, `IntersectionObserver`, `getClientRects`,
`ReadableStream`, `createElementNS`, `requestAnimationFrame` and the like;
two pages of one site now run their DOMContentLoaded handler on past the
replay's 5 s, where it died at once before). The seven newly unjudged
pages are one site's: its scripts now get far enough to run a timer about
every 100 ms for the whole 60 s replay, and each of its 617 timer tasks is
a fact line, past the record's 512 (`YONDER_DIAG_FACTS_MAX`), so the
record says it lost lines. 17 pages are newly incomplete (and 4 that were
are not): they ask for scripts their kept copies never fetched, because
they now get far enough to, and the corpus must be recorded again to judge
them.

Host, the base → the head: the DOM host 12,285 / 85,476 checks → 13,032 /
87,637 (allocation sweeps 1,093 / 13,109 → 1,161 / 13,400),
`tools/test_dom_document.inc` holding a case per sentence of document.c's
contract; the scripted-page host 7,601 → 7,629 checks,
`tools/test_yonder_document.inc` holding the end-to-end cases; yonder's
host 1,914 checks; the replay proof and the WPT runner's self-test pass;
`make -C userland` builds.

**The acceptance.** A banking SDK's common.js died at load on `TypeError:
'initCustomEvent' is not a function`. Its idiom (`createEvent('CustomEvent')`,
`initCustomEvent('x', true, true, {a:1})`, `dispatchEvent`) works in the
DOM host and end to end through yonder's loop, and the corpus's kept copy
of that file runs without the failure in all three records that had it.

**What the scope guard stopped.** Two things, each reported with nothing
worked around:

- **The window's title.** The contract said yonder's window title follows
  `document.title` after the task. It cannot: yonder names its window once,
  before any page arrives, and the GUI has no verb that renames a window
  (`SYSCALL_GUI_WINDOW_CREATE` takes the only title). That is a kernel GUI
  change. `document.title` and its setter are built, and the end-to-end
  case proves the title yonder reads of the page (`title_of`) is the one
  the script set; the title bar does not move here. Chris ruled that it
  follows: the GUI verb and yonder's call to it are their own branch and
  PR (`opus/gui-window-title`), which pays the DEBTS.md row.
- **`focus()` inside its task.** `activeElement` reads yonder's focus
  through the hook, but a script's `focus()` is an ask the window carries
  out after the task, so `el.focus(); document.activeElement === el` is
  false in that task. Making it synchronous means a script's task reaching
  the widgets, which the asks design keeps it from; that is more than a
  hook (DEBTS.md). `activeElement` also follows only the form controls the
  widgets focus.

`readyState` needed no more than a hook: the readiness move above is the
host's own dispatch function and its host struct.

**Review.** The builder's adversarial pass looked for page code reaching
the binding's state (detail lives in the Event's record; createEvent's
`HashChangeEvent` defaults are defined on the new object, as its
constructor does), a value's conversion running page code after the tree
was read (title's setter converted first; `dir`'s did not, and a
conversion that took the html element away left the setter writing to the
element it had found: fixed, with a case), work a page can size (title's
walk and text are linear, the table is fixed), the readiness skipping a
move nothing heard (yonder's host now calls the binding for a move), a
`readystatechange` that throws or runs out of time (its failure is the
dispatch's outcome; a sticky one ends the dispatch), the focus hook
answering for another page's document (it compares documents), and a held
`currentScript` outliving its binding (released at free). The truth pass
corrected WINDOW_DOCUMENT.md's two contract sentences (the title bar, and
"which the binding already sees"), the appendix's `hasFocus` rows, and
DEBTS.md's row that listed `document.title` and `document.referrer` as
absent.

A fresh cold read of `8db3cf0c` found no P1 or P2, and four P3s, each
fixed with a case where it is code: title's setter wrote an HTML title
inside a document whose root is in neither the SVG nor the HTML namespace
(it now changes nothing there); `initEvent` and `initCustomEvent` left a
host event's `isTrusted` true (DOM's "initialize" clears it); a Reload
lost the page's `document.referrer` (now kept, for the button, a form
sent again and `location.reload()`); and WINDOW_DOCUMENT.md's appendix
still said `body` had no setter. The arc lead's design read then ruled on
the two questions below: createEvent as built, and the title bar to its
own PR.

**Rulings taken by the builder, for the cold read.**

- **createEvent's unbuilt interfaces make a plain Event, not
  NotSupportedError.** DOM refuses a name on its table whose interface is
  not exposed, and Chris asked for the refusal. The corpus says
  `createEvent('MouseEvents')` is in 66 kept pages' scripts and is followed
  by `initEvent` 56 times (by `initMouseEvent` 23), which is how pages
  dispatch a synthetic click: today that works, and the refusal would turn
  every one into an exception. So the table's names whose interfaces are
  not built answer as every name did before, and a name off the table is
  refused; `TouchEvent` is refused because a desktop does not expose it
  (its success tells touch detection the machine is a phone: 15 kept
  pages ask). Flipping the ruling is one table column (`creatable` in
  event.c). The arc lead's design read agreed with it.
- **`hasFocus()` is "the page on screen".** yonder does not follow its GUI
  window's focus events; following them is a small yonder change no
  contract line names (DEBTS.md).
- **`lastModified` of a kept or replayed page is "now"**: replay records do
  not keep the Last-Modified header. A Last-Modified of exactly the epoch
  reads as none (yonder's options use 0 for none).
- **html/interaction/focus and dom/events are run whole but for their frame,
  test_driver and second-window pages**, excluded by name with that
  reason, so their baselines hold the event and focus machinery P1-Da
  sits on.

**Booked** (DEBTS.md § Classic browser scripting): the title bar,
`focus()` after its task, `hasFocus` and visibility without the window's
state, deferred scripts' `loading`, the referrer after a traversal,
`domain` without the public-suffix list, createEvent's plain Event for
unbuilt interfaces, and `Node.baseURI`.

### P2a, as built

Built on `opus/tmw-p2a-errors`, from `userland` `c8d87b53`, from the P2a
brief and WINDOW_DOCUMENT.md § P2a. The builder's adversarial and truth
passes, the cold read, and the answers to its six findings are done
(**Cold read**, below).

**Why.** Pages on which "nothing happens": a Run button that does
nothing, and a record whose every script "ran". The brief's premise, that
a rejection was dropped, was half right. libjs already judged the first
unhandled rejection of a turn or a task, and yonder wrote it as a FAILED
line with the bare reason (`FAILED script X: lost`, indistinguishable from
an exception's). What was silent: every rejection after the first in a
task, and every one in a task that also threw; no page could hear any of
its own failures. Both are answered here.

**What was built.** The five members and two interfaces of § P2a, one row
of the table: window's `onerror`, `onunhandledrejection`,
`onrejectionhandled` and `reportError`, the document's `onerror`, and
`ErrorEvent` and `PromiseRejectionEvent` (libdom `report.c`; the
interfaces and the handler rules in `event.c`; LIBDOM.md § P2a's rows has
every rule).

- **libjs's script host** (CONTRACT.md § A script host, the one change to
  the wrapper; QuickJS itself is untouched). `os64_js_set_script_host`
  gives a host every exception the wrapper catches, a hook for a handler
  added to a rejection it took, and one asked as a promise is rejected,
  whose answer the rejection carries to the host (the binding answers
  with the rejecting script); with a host set, no checkpoint
  judges a rejection, and `os64_js_take_rejection` hands them over, oldest
  first, only between calls with no job runnable. The tracker's list,
  walked for every rejection and every handler (n squared in a turn's
  rejections, bounded only by the deadline), is a table keyed by promise.
- **The binding is the host** on every page. An exception libjs caught is
  the window's `error` event at the task's next turn between calls; one the
  binding caught while a script ran (a nested dispatch, a handler compile,
  `reportError`) is the event at once; a rejection a checkpoint left
  unhandled is an `unhandledrejection` after it, each followed by a
  checkpoint; a reported promise a later handler reaches is a
  `rejectionhandled`, and one reached during its own event is not. All of
  it runs in the task the failure happened in, under its deadline (a
  listener that rejects for ever ends as any loop does). Window's `onerror`
  keeps HTML's special handling: called with five arguments, cancelled by
  `true`, compiled from the body's attribute as a function of five.
- **What an event says is read without page code**: an Error's or a
  DOMException's name and message through data properties of ordinary
  objects and errors only (no getter, no proxy trap), the place from the
  first placed frame of an Error's own stack (where the running script is
  when the binding reports and the value has none), the filename the
  address of the script whose failure it is, else the page's.
- **Muted errors.** What a script from another origin than the page's (the
  host fetches every script without CORS) throws or reports is told to the
  page as `Script error.` and nothing more, and its rejections fire no
  event, nor a `rejectionhandled` when a handler reaches one later. ONE
  rule, never parsed from stack text (Opus's ruling on the cold read):
  every script is compiled under a synthetic name the binding owns,
  `os64-script:<n>`, which it maps to the script's address and whether it
  is muted, and a failure is muted by the script that was RUNNING when the
  value was thrown, rejected or reported. A rejection and `reportError`
  ask QuickJS's public `JS_GetScriptOrModuleName` as they happen (libjs's
  new `rejected` hook asks it while the rejecting code is still on the
  stack); an exception, whose frames are gone when it is caught, is the
  script the host was running or the one whose callback the binding
  called, each callback's script read through `Function.prototype`'s own
  `fileName` getter as it was handed over, else the script handing it over
  (a bound function, one `eval` made). Muted promises the binding took are
  kept in a WeakSet of its own, so a later handler's `rejectionhandled` is
  swallowed. A handler attribute and a string timer are the page's, as
  HTML makes them; a function of another site's that the page's own code
  calls and that throws through it is the page's failure, as HTML's "run a
  classic script" reports it. The record keeps them all whole: it is the
  person's. Its cost: `error.stack` names `os64-script:<n>`, not the
  address (DEBTS).
- **The record.** A rejection no listener cancelled is the task's failure,
  `unhandled rejection: <reason>`; the first a task meets is its outcome
  (unless it already failed), and each after it reaches the host through
  `options.failed`, which yonder records under the same key. With the
  switch off no event is fired and every unhandled rejection is a line.
  The console the brief names is yonder's status line, which says a
  task's first failure as it always has.
- **yonder** runs each script under `os64_dom_script_name`'s name for it
  and follows it with `os64_dom_script_ended` (scripts.c's `run_item`),
  and gives libdom `options.failed`. A `src` script is named, muted and
  recorded by the address its response came from after any redirects
  (`yonder_scripts_fetched`), not the one asked for: a same-origin address
  that redirects to another site is that site's script.

The record line, for the brief's case (an async click handler that
throws):

    FAILED script click event: unhandled rejection: TypeError: in a click (1)

and for a script's: `FAILED script http://fixture.test/final#inline-1:
unhandled rejection: Error: from the script (1)`.

**The frame rule** (IFRAME.md § What the open slices do now, last
bullet). yonder's one change is in `yonder_scripts_t`, the page's own
script host: no `g.page`, no new file-level static. The events go to the
window of the binding whose runtime failed, which is the page whose script
it was.

**The seams, as they ended.**

| Seam | What it is now |
|---|---|
| `os64_js_set_script_host` (libjs `js_engine.h`) | a host's `exception`, `handled` and `rejected` hooks (`rejected` asked as a rejection is tracked, its answer the rejection's tag); with one set, rejections wait to be taken; NULL clears it, even on a failed runtime |
| `os64_js_take_rejection` | the oldest rejection still unhandled, as new references, with its tag; false inside a call, on a failed runtime, without a host, or with jobs runnable |
| `os64_dom_script_name` (libdom `dom.h`) | the name the host compiles a script under, `os64-script:<n>`, standing for its address |
| `os64_dom_script_ended` | what a script the host ran left owed, settled in a task of its name, folded into its outcome |
| `os64_dom_options_t.failed` | a rejection past a task's first failure, told to the host for its record |
| `D_EVENT_ERROR`, `_UNHANDLEDREJECTION`, `_REJECTIONHANDLED` | event kinds window has handlers for, which the body reflects and the binding fires |

**Classic or modern.** The seven names are one row. Being the script
host, and the record's rejection lines, are every page's: the record is
yonder's, not surface a page detects.

**Numbers.** WPT, the base (`c8d87b53`) → the head:

| Directory | Before | After | `[required]` |
|---|---|---|---|
| `html/webappapis/scripting` (new) | 39 of 271 | 111 of 277 | 72 |
| `html/webappapis/microtask-queuing` | 5 of 8 | 6 of 8 | 6 (+1) |
| `xhr` | 345 of 1188 | 347 of 1188 | +2 |
| `secure-contexts` | 0 | 1 of 34 | 0 |

Every required test fails with the switch off. What still fails in the new
directory: an inline script's line, counted from the script; a thrown
non-Error's line; `body.onerror` as a
property; `data:` scripts; the rejection pages that queue a task with
`<details>` toggle or a microtask with MutationObserver (the main
`promise-rejection-events.html` fails whole on them); readystatechange,
requestAnimationFrame, createImageBitmap; frames and workers.

**Four directories lost passes, and the runner's rule was ruled.**
Before P2a no harness could hear a page's uncaught error; now
testharness.js does, as in every browser, ends the page in ERROR, and the
runner failed such a page whole. As first built, five pages lost 330
passes, 9 of them required, none of it a change in what yonder does:

| Directory | Page | The page's own failure | Lost (required) |
|---|---|---|---|
| `dom/nodes` | `Element-classlist.html` | `createElementNS` absent (P1-Db) | 283 (0) |
| `dom/nodes` | `insertion-removing-steps/later-script-removed-by-earlier-script.html` | an inserted script runs after its insertion, not during it, so its own assertion fails | 2 (0) |
| `html/browsers/the-window-object` | `window-properties.https.html` | no frame (`contentWindow`) | 36 (0) |
| `css/cssom` | `getComputedStyle-detached-subtree.html` | no frame (`contentDocument`) | 1 (1) |
| `fetch/api/response` | `response-stream-disturbed-1.any.html` | a released reader's `closed` rejects unhandled (DEBTS; the binding cannot mark it handled) | 8 (8) |

Opus ruled, 2026-10-24, matching wpt.fyi, for the builder's lean: a
harness that ended in ERROR on the page's own uncaught error or unhandled
rejection counts the tests the page completed; a timeout or any other
harness error still fails the page whole. The report script tells the
two apart by what testharness.js reports, not by the wording: the
harness's status is ERROR and its message is the one testharness.js made
of an `error` or `unhandledrejection` event the page raised at window,
which capturing listeners of the report script's, running before
testharness.js's own, wrote down; it writes such a harness PAGE_ERROR,
and the runner says so on the page's HARNESS line and in its summary
(§ Slice 0a, as built, has the dated rule; three selftest pages hold the
page's own error, its own rejection, and a harness error that is neither).
Under it all five pages recover, all 330 passes and all 9 required tests
(response-stream-disturbed-1's eight too: its harness ends on the page's
own unhandled rejection, the DEBTS row's); no pass is lost in any of the
28 directories, each passing both ways; and three are new: xhr's
`thrown-error-in-events.html` pair (an error in a request's load listener
reaching `window.onerror`, P2a's, now required) and secure-contexts'
`basic-dedicated-worker.https.html` "HTTP worker", completed before its
page's own error.

The ruling also found a muting bug: a page whose address has a query the
address parser refuses (a space the WPT runner's variant addresses leave
unescaped) read no origin, so every script on it, its own included, was
taken for another site's, and xhr's timeout pages said `Script error.`.
An origin is read now from the address before its query and fragment,
and a name that is no address with a tuple origin is never another
site's.

**The corpus**, 2026-10-22b replayed at the base and the head
(`2026-10-22b_p2a-before` and `-after`): 10 page records now carry an
`unhandled rejection:` FAILED line (16 lines), every one a failure the
base already recorded under the bare message; none newly visible in this
corpus. FAILED lines 428 → 428 in 142 pages both sides; MISSING 9070 →
9052 (the row's names: `PromiseRejectionEvent`, `onerror`,
`onunhandledrejection`, `reportError`). The diff's 16 "regressions" are
those 6 relabelled records and 10 records of one site whose own
error-reporting code now runs, finds `onerror`, and reads three or four
globals of its own that are not there (no FAILED line). 9 records are newly
incomplete: they now ask for a navigation (a challenge script that gets
further) or a script the corpus did not keep, which a re-recorded corpus
answers. Replayed 239, clean 21, unfinished 13 and 19 unjudged on both
sides. Replayed again at the head after the cold read's answers
(`2026-10-22b_p2a-final`), the corpus differs from the first head's in no
line of any page: muting decides events, which no record holds, and no
kept page's record moved for the stack's new names.

Host, at the head: libjs's runtime host 1,154 checks (the script host's
cases among them, the `rejected` tag's) and its sanitized profile, 0
failed; the DOM host 13,294 / 90,070 checks, allocation sweeps 1,143 /
13,513, 0 failed, `tools/test_dom_report.inc` holding a case per rule of
§ P2a's rows and one per cold-read probe and Codex finding; the
scripted-page host 7,601 at the base → 7,678, 0 failed; yonder's host 1,914, 0 failed; the replay
proof, 42 fixture pages (`failures.html` new) kept and replayed the same;
the WPT runner's self-test, its three new pages among it; `make -C
userland` builds.

**Review.** The builder's adversarial pass looked for page code a failure's
description could run (getters, proxy traps, a page's `toString`, a page's
`Error`: each read is a data property of an ordinary object or error, and
the place-finding Error is the intrinsic constructor, taken before any page
ran), work a page can size (a thousand rejections or ninety throwing
microtasks in one task, each an event, linear; the wrapper's table), a page
that never stops failing (a rejection made in its own `unhandledrejection`
ends at the task's deadline), recursion (an error inside an error listener
reaches no page), a cross-origin script's failures (muted by its address
and by the frames of what it threw), and what the binding keeps (two lists
in the budget, released at drain, counted in the registry). It moved
muting from the script's address alone to the thrown Error's frames as
well, made a non-Error object's message readable, and stopped one proxy
trap that reading an error's place could reach. The truth pass corrected
dom.h's dispatch and take-report comments, the handler lists in LIBDOM.md
§ Events, and CONTRACT.md's rejection paragraph. (The muting this pass
built, by address and by an Error's frames, is the one the cold read
walked past; what replaced it is above, under **Muted errors**.)

**Rulings taken by the builder, for the cold read.**

1. An exception's FAILED line is written whatever an error listener did:
   a page that cancels its errors (`onerror` returning true) keeps them
   on the record. A rejection follows the brief: cancelled, no line. The
   lean is that the record is the person's, so neither should be
   hideable; the brief's rule for rejections is kept as given.
2. No element has an `onerror` property (`document.body.onerror` is an
   expando), though any element's `onerror` attribute is a handler for an
   `error` event dispatched at it: nothing fires `error` at an element yet.
3. A failure's events fire in its own task, where HTML queues one (DEBTS).
4. `onerror`'s location is the engine's (DEBTS): an inline script's line
   from the script, a non-Error's line 0.

**Cold read** (a fresh Opus, after the runner's ruling): one P1, three
P2s on muting, a P3 and a sixth, each answered in a commit of its own.

- **P1, a freeze a page sizes.** Every rejection past a task's first
  failure went to `options.failed`, and yonder's record found each line
  by walking every kept and lost line, all in C that the take loop runs
  with no script running, so no deadline ended it: a `setInterval`
  rejecting 200,000 promises froze the window about 22 s a task, for
  ever. libdom now tells a task's first sixteen one by one
  (`D_REJECTIONS_TOLD`, named once) and counts the rest into one line as
  the task closes, `and <n> more unhandled rejections`, computing no
  text for them; yonder's record finds every line by its key through an
  index of its own and counts its probes. The bound cases: 200,000
  rejections in one script call the host 17 times, the last line `and
  199983 more unhandled rejections`; 100,000 failures over 6,000 lines
  cost the record under eight probes each; a page's thousand rejections
  are 18 lines found in under 2,000 probes.
- **P2s, muting walked past** (one probe each, now a case each in
  `tools/test_dom_report.inc`'s muting cases): an address with `?(` in it
  left the stack's place unreadable, so the listener's Error was told
  whole; a value that is no Error (a thrown or rejected string) carries
  no stack, so it was told whole; and a muted rejection a handler
  reached later fired `rejectionhandled` with its reason. Opus ruled the
  rule above, **Muted errors**, recorded here and in LIBDOM.md: one
  rule, never parsed from stack text, by the binding's own names for
  scripts. No QuickJS change was needed: `JS_GetScriptOrModuleName` and
  `Function.prototype.fileName` are public, and the `rejected` hook is
  the wrapper's. CONTRACT.md's `handled()` text is made true for a
  rejection the runtime could not hold (an allocation refused in the
  tracker fails the runtime, and a failed runtime calls no hook). The
  cases: the `?(` address, the later-handled rejection (by its own
  script's listener and by the page), a non-Error thrown and rejected by
  a listener and thrown at a script's top level, `reportError` from the
  script and from its listener, a bound listener, a function `eval`
  made, a bound microtask and a bound timer, and a function of theirs
  the page calls rejecting: each "Script error." or no event. A
  same-origin script's listener error and rejectionhandled are told
  whole. A report sweep runs another site's script, so muting's own
  allocations are refused in turn. With each script known by name, an
  error event names the address of the script whose failure it is, a
  listener's and `reportError`'s included, so `reporterror.any.js`'s
  three value tests now pass and are required (scripting 108 → 111);
  the other 27 directories are unchanged, both ways.
- **P3, an error event after its microtasks.** A timer callback's
  exception is now fired between the call and the checkpoint after it,
  as a listener's was. A script the host ran is not local (libjs opens no
  task while jobs wait), so it is booked beside "A failure's events fire
  in its own task", and the WHEN paragraphs say so.
- **The binding's own report stood past its task** when the task had
  failed otherwise, to be taken by the next task under that one's name.
  `d_task_close` now settles it whatever the outcome: the outcome when
  that is OK, else a line through `options.failed`.

**Codex, round 1** (on 3dbf313a): three P2s, each fixed with its case.
A rejectionhandled fired in the same pass as the listener that earned it,
before the handler's reaction ran; the settle now fires those reached
before each checkpoint and leaves the rest for the pass after the next.
A task's first report was written down with JS_ToCString, which runs a
value's getters and toString; it is read as an event's message is. And a
same-origin `src` that redirected to another site was muted (and named)
by the address asked for; the response's address after its redirects is
now the script's. What else turns a page value into record text: libjs's
exception diagnostic, which converts the thrown value as it always has,
inside the wrapper's guard and under the task's deadline (runtime.c's
`diagnostic`); every reader of the binding's own is non-executing.

**Codex, round 2** (on 09433ee1): a P1 and a P2 fixed, a third declined
with evidence. The P1: a script that dispatched to another script's
listener came back with the task's last-called script naming the
listener's, so another site's script that dispatched to a page listener
and then threw was told whole. Every call into a page callback now goes
through one pair, `d_script_enter` and `d_script_leave`, which keeps all
the muting state and puts it back (a nested dispatch's listener, and the
error events fired at once, all of it; a host dispatch's listener and a
timer's callback, a task's own calls, keep the task's last-called script
for the checkpoint after them). The P2: two rejectionhandled events in
one pass fired back to back; an event that leaves a job runnable now
sends the rest to the pass after the next checkpoint, as an
unhandledrejection already waited. The third, the runner's classifier
reading a rejection reason's message twice, matters only for a reason
whose message changes between reads, and no page of the pinned WPT
makes one (the one `get message()` there is reportError's argument in
`reporterror.any.js`, which no rejection carries).

**Booked** (DEBTS.md): an error event's place; the events in the failing
task; a script's microtasks before its error event; the stack naming
`os64-script:<n>`; muted `crossorigin` scripts; a task's second exception; the body's
IDL for the three new handlers; and, for 0011b and Fable, an Error made by
code compiled under memory pressure reading past a heap block in
`build_backtrace`, which P2a's first engine sweep (compile and run both
inside the cut) reached and the reshaped one (compiled first) does not.
The released reader's `closed` row is re-stated: its cure is an engine
verb that sets a promise's handled flag, which is a libjs patch and
Fable's to rule.

### Loud elements, as built

Built on `opus/yonder-loud-elements`, from `userland` `c8d87b53` (it needs
none of P2a's record lines: the FAILED line is diag.c's, as every one is).
The builder's adversarial and truth passes, the cold read and the answers
to its findings are done (**Cold read**, below).

**Why.** Two things a page asked for that a person could not tell had
failed. A canvas laid its fallback out (usually nothing) and the record
only counted it; and a frame a script inserted named no document, where a
parsed one is a stand-in that takes a click to its document.

**A canvas** (libflow `boxes.c`, `layout.c`; yonder `paint.c`, `yonder.c`).
Where scripting is on a canvas is a replaced box, as in a browser that runs
scripts (HTML § 15.4.1), its own size its width and height attributes, 300
by 150 where one is absent or no non-negative integer, a CSS width keeping
their ratio as an img's own size does; its fallback is not laid out. With
scripting off it is its fallback, laid out as any element, an
inline-block's or an inline-flex's too: the painter draws a stand-in only
for the replaced box libflow says it made (`flow_box_t.replaced`), never
by the tag alone. yonder draws the box with a 1px grey outline and
"Canvas: not supported" in grey in its top-left corner, and NO panel (Opus
ruled, on the cold read): a canvas is transparent until drawn, and the
full-viewport fixed canvases of confetti, snow and cursor effects lie over
the page, which must show through; a frame keeps its pale panel. Over such
an overlay the label in its corner can still cover a few words of the page
there, which is accepted. The paint verb that draws both is now
`stand_in`, and it makes no label for a box with nothing in view. The page's record gets
one `FAILED unsupported: canvas (1)` line however many canvases it has, a
script's among them, beside `MISSING element canvas (N)` as before. Opus
ruled canvas alone loud (2026-10-24): video, audio, embed and svg stay
counted and silent, booked as one DEBTS row with their corpus counts. A
canvas a script sizes with `canvas.width = 800` keeps the size its
attributes gave it, since there is no HTMLCanvasElement to reflect `width`
and `height` (booked: HTMLCanvasElement is the canvas question's, which is
Fable's).

**A frame a script inserts** (libdom `frames.c`, the frames row). The cause
was libdom's: an iframe had no `src`, so `f.src = url`, the way scripts most
often address a frame, made a property of the script's own, and libpage,
which lists a frame's `src` attribute as its link, never saw it;
`setAttribute` and `innerHTML` already worked. Opus ruled the shape: the
iframe is a prototype kind of its own, `D_PROTO_IFRAME`, the
HTMLIFrameElement IFRAME.md shapes (§ The cluster and the window proxy),
and a modern row of its own puts the interface and `src` on it, so phase
F1 extends this one shape and the modern switch takes it away whole (rule
4; there is one kill switch).
With the modern switch off an iframe stays as it was, `src` a script's
own property, which the scripted-page host's cases hold both ways. A frame
is never unsupported: its stand-in takes a click to its document (Chris
verified the placeholder takes him to the source).

**Numbers.** WPT, the 27 expectation directories of this branch, both
ways, and the runner's self-test: one new pass, `dom/nodes`'
`Node-cloneNode.html` "createElement(iframe)", which checks the copy is an
HTMLIFrameElement; it fails with the switch off and is required. Nothing
else moved.

The corpus, 2026-10-22b replayed at `userland` (`2026-10-22b_p2a-before`,
the same commit) and at this branch (`2026-10-22b_loud-after`): 7 page
records of 5 sites gain the canvas line; FAILED lines 428 → 433, MISSING
lines 9070 → 9064. The diff's 12 "regressions" are those 7 and 5 records
whose scripts now get past `instanceof HTMLIFrameElement` (which threw "invalid
'instanceof' right operand" in 5 records, and `MISSING global
HTMLIFrameElement` was in 8) to their next wall: `createElementNS` in 2,
`HTMLStyleElement` in 1, a custom element and `PerformanceObserver` in 2.
Replayed 239, clean 21, unfinished 13 and 19 unjudged on both sides.

Host, at the head, all 0 failed: the DOM host, 12,522 / 85,602 checks
(iframe `src`, its interface, and both absent with the switch off); the
scripted-page host, 7,672 (a frame inserted every way, both ways; the
canvas line, once, with a script's canvas, none with scripts off and
none for an svg's canvas; the stand-ins painted through the real panes:
the page under a full-viewport canvas, the labels' text, and their cost);
yonder's host, 1,959 (the stand-in at a canvas's size, a bad attribute's
default, the fallback with scripting off, an inline-block's and an
inline-flex's fallback alone); libflow's, 23,779 (the sizes, the ratio,
the fallback, `display: contents`); the replay proof, a hand-made page of
canvases and an inserted frame replayed to the same record, held to
whole lines; wend and libpage; `make -C userland` builds. The corpus
replayed again after the cold read's answers differs from the first in no
line.

**Review.** The builder's adversarial pass looked for work a page can
size (the canvas tally is diag's existing walk of the tree; a label is laid
out only for a stand-in with something in view, so a page of empty
canvases costs none), page code the new member runs (`src`'s setter
converts its value as `script.src`'s does, and loads nothing), a frame's
address a script controls (a `javascript:` `src` was already a parsed
frame's link, and goes through the same door a link does), and sizes a
page gives (a huge width attribute is clamped as every integer attribute
is, and is a box's size as a CSS width already could be). The truth pass
corrected YONDER_DIAGNOSTICS.md's element and FAILED lists, diag_elements'
comment (a canvas and a frame are stand-ins, not nothing), and paint.h's
verb, renamed for what it now draws.

**Cold read** (a fresh Opus, on e76b1570): no crash, no out-of-bounds,
the hostile sizes clamped, iframe `src` as IFRAME.md shapes it. Its
findings, each answered in a commit of its own:

- A canvas made inline-block (or inline-flex, -grid, -table) with
  scripting off painted the stand-in over its fallback: the painter asked
  the tag. libflow now says which boxes are replaced
  (`flow_box_t.replaced`), and the painter asks that.
- An overlay canvas hid the page under the panel. Opus ruled a canvas's
  stand-in has no paper (above).
- The replay case was held to words, so `(3)` was met by the MISSING
  line; it now holds whole record lines (a list that begins with a
  newline), and forbids a frame's FAILED line.
- Nothing tested the real label path: the scripted-page host now paints
  through it, reads the labels' text, and checks an svg's canvas is no
  failure.
- Each visible canvas laid its label out every paint: once a paint now,
  and none where the top strip is out of view, bounded by a case.
- `display: contents` on a replaced element showed its content; it now
  computes to `none`, as CSS Display 3 Appendix B says.
- `canvas.width = 800` is booked (above), and the text findings (DEBTS'
  interface-object row, the one kill switch, `yet`) are fixed.

**Booked** (DEBTS.md § Browser layout): loud for canvas only; a canvas's
size a script sets.

### P2b, as built

Built by Opus on `opus/tmw-p2b-message`, from `userland` `bd7e1b75`, from
the P2b brief and WINDOW_DOCUMENT.md § P2b. The builder's adversarial and
truth passes and a cold read are done, the cold read's eight findings
fixed (§ Review).

**Why.** Three of the most-used members window lacked: `requestIdleCallback`
(Chrome 54.2%, 343 kept pages), `postMessage` (52.1%, 194; its common use
is the setImmediate trick, a page posting itself a message to run a
callback after the current task) and `structuredClone` (21.9%, 149).

**What was built.** The six members of § P2b and two interfaces, three rows
of the table (LIBDOM.md § P2b's rows has every rule):

- **`structuredClone`** (libdom `clone.c`): HTML's
  StructuredSerializeWithTransfer and StructuredDeserializeWithTransfer,
  the binding's one copy, which `postMessage` and a performance mark's
  `detail` use too. It clones what HTML serializes and this browser has:
  the primitives and their wrapper objects, Date, RegExp, ArrayBuffer
  (resizable ones keep their maximum) and every view, Map and Set, Error
  and its six kinds (with an own `cause` and `stack`), arrays, ordinary
  objects (getters run, in [[OwnPropertyKeys]] order), Blob, File and
  DOMException, a value met twice being one value in the copy; anything
  else is a DataCloneError. The transfer list's ArrayBuffers are copied
  into the record and detached once the clone has succeeded whole.
- **`postMessage`, `onmessage`, `onmessageerror`** and **`MessageEvent`**
  (`message.c`, `event.c`): HTML's window post message steps with the
  target the caller's own window, `targetOrigin` parsed before the message
  is cloned, one task per message queued at the target window and run by
  the host (`os64_dom_message_fire`), the targetOrigin checked as it runs,
  the clone read in the target's realm, `messageerror` when it cannot be
  (kept for frames: in one realm every refusal the reader can meet ends
  the task, so no page reaches it today).
  `MessageEvent`'s constructor, `initMessageEvent` and
  `createEvent('MessageEvent')`; its `ports` is an empty frozen array,
  there being no MessagePort.
- **`requestIdleCallback`, `cancelIdleCallback`** and **`IdleDeadline`**
  (`idle.c`): idle periods that start when the host says its loop is idle
  and the page has no task due (`os64_dom_idle_run`), every callback asked
  for until then runnable and run one a task, oldest first, while
  HTML's computeDeadline (50 ms, or the next timer) has not passed, read
  again at every `timeRemaining()`; a `timeout` is a timer of the
  window's, which calls the callback timed out if no period ran it first.
- **yonder** runs a page's posted messages and due timers by when each was
  queued, a timer first on a tie, in every page's turn (`queued_task`),
  the view's events first in a turn once a rendering opportunity while
  those keep coming (`yonder_scripts_view_due`), and the shown page's idle
  callbacks when its turn finds nothing else to run;
  the bell brings the loop back while one is due and the ticker is offered
  the time the next idle period may start.

**The frame rule** (IFRAME.md § What the open slices do now, "0e P2b";
§ The cluster and the window proxy, its `postMessage` paragraph). P2b's
same-window `postMessage` is that design with the target the caller:

- The clone writes in one realm and reads in another even when both are
  the primary. `d_clone_write(dom, ctx, ...)` reads in the poster's realm
  and `d_clone_read(dom, ctx, ...)` builds in the target's, each through
  the intrinsics its own binding took before any page ran
  (`d_clone_setup`, one per binding, so one per realm when bindings are
  per document); the record between them holds only what the runtime
  shares, primitives, atoms and bytes, never a realm's object.
- `MessageEvent` and the message task take the target window as a
  parameter. `post_message` names the target (the binding whose window's
  member was called) and the poster (the same binding here) as two things;
  the message is queued on the target's binding and its task opened there
  (`d_host_dispatch` on the target), the event made in the target's realm
  (`d_message_event_new(target, ...)`); what the poster gives the message
  is captured as it posts: its window as `source`, its document's origin
  serialized as `origin`, and `targetOrigin` resolved against it. The
  targetOrigin is checked against the target's document at delivery, as
  IFRAME.md says; `lastEventId` is "" and `ports` empty.
- `transfer` accepts ArrayBuffers and refuses anything else, ports
  included, as IFRAME.md's `postMessage` does.
- yonder's changes hold no page of their own: `queued_task` takes the host
  in hand, and `script_turn` and `pictures_schedule`, which are the shown
  page's by design, name it as they already did. No new file-level static.

**Decided against the plan, with why** (each for the cold read):

1. **The clone is the binding's own, not the engine's object writer and
   reader.** The plan named `JS_WriteObject`/`JS_ReadObject`, as
   `performance.c` used for plain data. The writer reads property slots
   directly, so a getter is "only value properties are supported" where
   HTML calls it (WPT's "Object with a getter that throws" and
   webmessaging's 026); it drops an array's non-index properties; and it
   cannot write a Map, a Set, a RegExp, an Error, a DataView, a Blob, a
   File or a DOMException. A `structuredClone` that refuses a Map is half
   an API (rule 1): a page that detects it and clones its state takes the
   broken path where its own deep copy worked. So the pair is written
   against HTML's algorithm, in C, over the engine's public API and
   intrinsics the binding holds; `performance.c`'s `detail` moved onto it
   (its own commit), so there is one copy.
2. **A callback asked for during an idle period waits 50 ms for the next.**
   HTML lets the next period start at once; Chromium starts its next long
   idle period at the end of the current one. Without the rest, a callback
   that asks for itself again runs once a turn of yonder's loop and holds
   it busy for as long as the page wants. Any other request lets the next
   period start at once.
3. **Messages and timers are run by when each was queued.** HTML leaves
   task sources' order to the browser; a fixed order (messages first)
   would let a page whose every message posts another starve its own
   timers, and the setImmediate trick is exactly that shape. Such a page
   still has its view's events (a resize, a scroll): they are asked for
   first in the turn once a rendering opportunity (16 ms) however many
   tasks are waiting, as HTML updates the rendering between tasks. Its
   idle callbacks may wait for as long as it posts, which HTML allows.
   (The view step's cadence is the cold read's finding 7.)
4. **A page's idle callbacks wait until it is shown**: only the shown
   page's turn is the loop being idle (DEBTS.md).
5. **Nothing is detached unless the whole clone succeeds.** HTML detaches
   the transfer list's buffers one by one as it serializes, so a refusal
   after (the last one detached already, a deserialization that fails)
   leaves some detached; here their bytes are copied as the value is
   written and they are detached once the copy is made (structuredClone)
   or the message queued (postMessage). Only a refusal can tell. (As first
   built the list was detached at the end of the write, before the read;
   the cold read's finding 1 moved it.)

**The seams, as they ended.**

| Seam | What it is now |
|---|---|
| `os64_dom_message_next` / `_fire` (libdom `dom.h`) | when the oldest posted message was posted (`options.now_ms`), and the oldest run as one task, a `message` or `messageerror` at window; one its targetOrigin or nobody hears is dropped without a task |
| `os64_dom_idle_next` / `_run` | when an idle callback may run (0 now, later only for ones an idle callback asked for, UINT64_MAX none), and the host's "my loop is idle": at most one idle callback as one task, refused while a timer is due or a message queued |
| `d_clone_write` / `d_clone_read` / `d_clone_free` (internal.h) | the structured clone's pair, each given its realm's binding and context; the record no realm's, read once |
| `d_clone_setup` | the intrinsics the pair calls, taken from a binding's global before any page ran, by each row that clones |
| `D_EVENT_MESSAGE`, `_MESSAGEERROR` | event kinds window has handlers for, which the body reflects and a message's task fires |
| `yonder_scripts_message_next` / `_fire`, `_idle_next` / `_run` (yonder `scripts.h`) | the binding's four, judged as a task's outcome |
| `yonder_scripts_view_due`, `YONDER_SCRIPTS_VIEW_CADENCE_MS` | whether the view's events have gone unasked for a rendering opportunity (16 ms), which puts the view step first in `script_turn` |

**Classic or modern.** All of it is the switch's: three rows, eight names.
With it off nothing is queued and no idle callback exists, and a body's
`onmessage` attribute is an ordinary attribute.

**Numbers.** WPT, the base (`bd7e1b75`) → the head (passes of tests run;
every `[required]` test fails with the switch off):

| Directory | Before | After | `[required]` | `[excluded]` pages |
|---|---|---|---|---|
| `html/webappapis/structured-clone` (new) | 0 of 152 | 128 of 152 | 128 | 1 |
| `requestidlecallback` (new) | 0 of 30 | 17 of 20 | 17 | 9 |
| `webmessaging/without-ports` (new) | 0 of 26 | 27 of 30 | 27 | 11 |
| `html/webappapis/scripting` | 111 | 116 | +5 | 0 |
| `dom/nodes` | 1719 | 1722 | +3 | — |
| `url` | 4224 | 4226 | +2 | — |

What still fails: in structured-clone, what this browser does not have to
clone (FileList, ImageData, ImageBitmap, MessagePort, OffscreenCanvas, a
transferable ReadableStream, WebAssembly's growable SharedArrayBuffer) and
the two transferred length-tracking views (DEBTS.md); in
requestidlecallback, `callback-timeout-when-busy`'s two tests, whose idle
callback runs in the 4 ms between a busy page's clamped timers (HTML
allows it; DEBTS.md), and `callback-timeout`, whose busy loop reads
`Date.now()`, which the runner's clock never moves; in webmessaging,
010.html's three tests that call helpers the page never defines; in
scripting's MessageEvent page, the tests that need MessageChannel or a
ServiceWorker. Excluded by name: frames (IFRAME.md), a second window,
requestAnimationFrame (slice 5d), MessageChannel and workers, a canvas,
and the WebIDL files the runner does not fetch. Every one of the 34
expectation directories passes at the head, both ways, with no pass lost.

**The corpus**, 2026-10-22b replayed at the base and the head (each in the
builder's scratch; the runner as in § Slice 0b): P2b's names were 89
MISSING lines at the base (`postMessage` in 28 files, 45 asks;
`structuredClone` 25, 38; `requestIdleCallback` 23, 25;
`cancelIdleCallback` 13, 13) and are in none; MISSING lines 9164 → 9100;
FAILED lines 238 → 238 in 128 pages both sides, none gained. The diff: 25
pages of progress (about 15 sites), and 10 "regressions" in 5 sites, each
the same one line, `MISSING global importScripts`: the setImmediate
polyfills (and libraries that pick a scheduler) find `postMessage` now and
then ask whether they are in a worker, which is the probe working, not a
failure. Replayed 232, clean 21, incomplete 46, unfinished 13 and 26
unjudged on both sides.

**Host**, the base → the head: the DOM host 14,052 / 92,372 checks →
15,153 / 100,629 (allocation sweeps 1,211 / 13,827 → 1,356 / 15,120), 0
failed, `tools/test_dom_message.inc` holding a case per rule, the hostile
cases (every intrinsic the clone calls replaced by the page first, getters
that reach back in) and the sweeps of a clone, a message and an idle
callback, binding and engine; the scripted-page host 7,864 → 7,925, 0
failed, its new cases the setImmediate trick, an idle callback, and a
page that posts itself messages for ever still hearing its resize, each
through yonder's loop; yonder's host, its stream and fetch-job hosts, the html-dom
host, the replay proof (43 fixture pages kept and replayed the same) and
the WPT runner's self-test, all 0 failed; `make -C userland` builds.

**Review.** The builder's adversarial pass looked for what a page can
reach and size: every intrinsic the clone calls is one the binding took
before any page ran (a case replaces each, and the copy is still made
without one trap firing); every copy is filled by definition, never by a
setter a page can shadow; no index or pointer into a growable array is
held across a page's code (the writer's frames are indexed, a getter that
clones, posts or transfers mid-clone is a case each); a graph's depth is
the writer's and reader's own stacks, never C's (thirty thousand deep
both ways, timed against fifteen thousand); a Map's entries come from its
own forEach; the memory holds what it has met, so a getter that frees an
object cannot let another take its address. Two proofs found two bugs,
each fixed in its own commit: the replay proof saw kept fixture pages
timed a millisecond from their replays (classes learned once a process,
so a page's engine calls, and the interrupt checks that read the clock,
depended on the page before it), and the DOM host's installer sweep saw a
refused allocation at setup fail the runtime (the first engine call
outside a task asks whether to stop); the classes are learned at a
binding's first clone now. The truth pass corrected WINDOW_DOCUMENT.md's
§ P2b and its appendix row (the plan's engine writer), LIBDOM.md's
handler lists, createEvent table, performance `detail` and proof
boundary, DEBTS.md's body-handler row, and retired its "no DOMException,
no structuredClone" row, both halves now paid.

The cold read found eight, each fixed in its own commit with a case and a hand mutant
that fails it, except where the fix is text:

1. A getter that resized a resizable buffer mid-clone made the reader's
   view refuse, after the transfer had detached the buffer. The view is
   made in a copy grown through ArrayBuffer's own `resize` and shrunk
   back, so it is out of bounds as the page's was; and nothing is
   detached unless the whole clone succeeds (decision 5).
2. A binding that took an exception could swallow the engine's
   uncatchable interrupt: the clone's DataView getters turned a deadline
   into a DataCloneError a page's catch heard. `d_exception_catchable` is
   asked before every such take; every `JS_GetException` in libdom was
   read and each fixed or left with its reason (the commit lists them).
3. An unheard message dropped those behind it past a timer whose task
   adds the listener; the queue now stops at one posted after the next
   timer.
4. `clearTimeout` of a guessed id cancelled an idle callback's timeout
   (or `AbortSignal.timeout`'s); a timer the binding sets for itself is
   flagged and only the binding cancels it.
5. `initMessageEvent` changed the event before converting its later
   arguments; it converts all eight first.
6. clone.c claimed every walk runs under the task's deadline; it is
   checked only where the walk calls into the engine (text, and a
   DEBTS.md row).
7. A page whose every message posts another starved the view's events;
   they are asked first in the turn once a rendering opportunity
   (decision 3).
8. Three hand mutants survived: `messageerror` no page can reach (said
   so in message.c, LIBDOM.md and here, the path kept for frames); a
   refusal as the writer's memory table grows, which the allocation
   sweep never met (its value holds more than 16 objects now); and
   finding 2's, covered by its case.

Codex's first round found one: an empty Map or Set with an own
enumerable property was walked as an ordinary object, because the writer
told a frame's kind by whether its entry list was NULL (a Map's expando
broke the record, a Set's became an entry). Each writer frame now carries
its kind, and only an object's or array's walks its own properties.

**Booked** (DEBTS.md § Classic browser scripting): a cloned
length-tracking view tracks no length; a transferred ArrayBuffer is
copied, not moved; a cloned DOMException's name and message end at a NUL;
an idle period can start between any two of a busy page's tasks; a page's
idle callbacks wait until it is shown; a clone of plain data is not
stopped by its task's deadline.

### Slice 3, as built

Built by Opus in the main session (no builder agent: Chris's rule for the
week of 2026-10-25), on `opus/tmw-3-modules` from `userland` `e372af4c`,
from MODERN_WEB_BRIEFS.md § Slice 3 and IFRAME.md's binding line for it
(the module map is per realm, the loader is handed the realm, and
`import.meta.url` is the document's).

**Why.** GitHub is all modules, so none of its scripts ran (its account
menu drew as a bare "Button" on the P5 on 2026-10-25, likely one of
them, unconfirmed until it runs there). 102 of the 2026-10-22b corpus's
kept records said `MISSING script module`.

**What was built.**

- **The engine half** (libjs CONTRACT.md § Modules; patch 0012). The pinned
  QuickJS resolves a module's imports as it compiles it, through a
  synchronous loader, so a browser could not compile an entry before
  fetching its imports. Patch 0012 adds `JS_EVAL_FLAG_NO_RESOLVE` and the
  request getters; makes handled the module body's promise the engine
  takes as the module's error (the host heard one error twice); and leaves
  no module a failed resolve marked resolved, a cycle's members included,
  since a page's map keeps them across a failed run (WPT found the SEGV a
  shared module made, the cold read the use-after-free a cycle made).
  libjs's module entry:
  compile unresolved under a name with an optional `import.meta.url`, read
  the requests, run once inside a task, ask the state after each drain (a
  top-level `await` settles when what it awaits arrives), release. A
  module's failure is its handle's and goes to the script host's
  `exception`, never to an unhandled rejection; a compile's syntax error
  is its outcome's alone, since no task is running to report it.
- **The page's module map** (`yonder/modules.c`, one per script host, so
  one per realm). A module is fetched as HTML fetches one: a CORS GET with
  same-origin credentials through the page's own request path (slice 2's
  fetch job, the answer told from a fetch()'s by `YONDER_MODULE_TOKEN`),
  ok, a 2xx, typed as JavaScript (`os64_dom_javascript_mime`), read as
  UTF-8 (`script_decode.c`, split out of the script job so both share
  it). Each is compiled as it lands and its imports fetched, each URL
  once; a graph is READY when everything it reaches is in. An inline
  module a running script connects (or `document.write` writes) cannot
  be compiled mid-call: it waits PENDING and the next step compiles it.
  The map keeps every source, because the engine asks the loader again
  for a module a failed run unlisted, and a dynamic `import()` of a module
  no graph reached is fetched inline under its task's budget (DEBTS).
- **Errors as HTML reports them.** A module not fetched or not JavaScript
  fails its graph and runs nothing. A syntax error is BROKEN, not failed:
  the run reaches it, the engine compiles it again from its source and
  throws, and the window hears the SyntaxError; an entry that is itself
  broken runs a one-line `import '<entry>'` in its place. A bare
  specifier throws its TypeError at the run. Each is on the record by its
  module's address.
- **The script host's order** (`scripts.c`). A parser-inserted module,
  inline or not, is deferred, among the defer scripts in document order; an
  `async` one and a script-inserted one run when their graph is ready; a
  module never blocks the parse; `nomodule` classic scripts are skipped
  (and marked started) where modules run; a module runs as one task named
  as the page's code (CORS-fetched, so never muted), with no
  `currentScript`, its owed error events settled after. `type=importmap`
  was on the record as missing until § Import maps, as built, registered
  it. A classic script's `import()` resolves against the script's own
  address (`os64_dom_script_address`), which is how a bundler's lazy
  chunk loads. With the switch off nothing of this exists and a module is
  missing, as before.
- **libdom** tells the host of a module a verb connects, as of a classic
  script (it skipped them, so a loader's module never reached yonder), and
  reflects a script's `type`, `defer` and `noModule`: a loader's
  `s.type = 'module'` was an expando, and its script ran as a classic one
  (the first draft's own test passed that way; the mutant harness caught
  it). `text` and `async` are DEBTS rows.
- **Module preloads**, added after the P5's first run. The P5's first
  module page (a Vite build) died on its first statement: Vite's
  modulepreload polyfill asks `link.relList.supports('modulepreload')`,
  found no `relList`, and reached for `MutationObserver`, which is slice
  5a's. `supports` answers true only for what yonder does, so the
  preload is real: a link's `rel` and `relList` are reflected (libdom,
  `relList` being classList's DOMTokenList over `rel`, with `supports`). At each of the stream's looks
  for its sheets, `yonder_scripts_preload` walks the tree for `<link
  rel=modulepreload>` whose `as` is a script's and begins each one's module
  in the page's map, which fetches its imports as it lands and runs
  nothing; a module script or import of it finds it fetched once.
  `supports` answers `stylesheet`, and `modulepreload` with the switch on.
  A link a script inserts after the parse, and the link's own `load` and
  `error`, are DEBTS rows.

**Decisions.**

1. **Modules from a `file:` page are refused**, as Chrome refuses them: the
   request path has no `file:` door, and a file page's origin is null.
   Pages under test are served over http.
2. **The map is the engine's backstop, not only its feed.** A failed
   resolve unlists every module no run has resolved, other graphs'
   included, so the loader answers for any name the map holds.
3. **A syntax error is reported, not swallowed** (decision of the build,
   against the first draft's FAILED): WPT's error pages and real sites'
   error reporting both expect `window.onerror` to hear it.

**Numbers.**

- **WPT** `html/semantics/scripting-1/the-script-element/module`: 0 → 101
  of 269 (99 `[required]`, each failing with the switch off; the two left
  out pass either way). The rest is DEBTS.md § ES modules (a script
  element's load and error events, `async = false` ordering, import maps,
  import.meta.resolve, JSON and CSS modules), workers and
  frames, and pages that need a `.py` handler or a server's delay. The
  preloads add two directories to `tools/wpt_fetch.sh` and to the
  expectations: `html/semantics/document-metadata/the-link-element` 1 → 2
  of 147 (`link-rellist.html`) and `preload` 20 → 22 of 811 (the two
  modulepreload pages that ask only that a preload not hold the window's
  `load`); the rest of `preload/modulepreload*` inserts its links by
  script and waits on their `load` (DEBTS). All 37 directories with
  expectations pass with no pass lost.
- **Hosts**, 0 failed: libjs runtime 1,267 target-core and 6,096
  instrumented checks, the module sweep 5 and 243 allocations; scripted
  page 8,063 (`tools/test_yonder_modules.inc` among them); yonder 2,028;
  fetch job 374; DOM 15,215 and 100,986 (the first one's installer sweep
  is one allocation shorter with the new members, 319 cuts for 320, and
  each cut carries its checks); replay self-test 43 pages, 0 problems; the
  libjs prepare, engine and target-audit suites (190 engine exports, 27
  runtime).
- **Mutants** (`tools/test_modules_mutants.py`): 37, each caught (two first
  missed, which is how libdom's unreflected `type` and unreported connected
  modules were found; one of the preloads' first did not build, the
  `-Werror` of an unused `dom`, and was reworded).
- **Corpus** 2026-10-22b: `MISSING script module` was in 102 kept records
  and is in none of the head's replay; 15 say `MISSING script importmap`.
  The replay can measure no more: it answers none of a page's script
  requests, a module's among them, so 85 of those 102 pages wait on their
  graphs and replay unfinished (DEBTS.md § The replay corpus). A corpus
  kept after this slice, its requests kept, is the measure.
- **QEMU**: a served module site (a graph of three, an async module, an
  inline one, dynamic imports from a module and from a classic script, a
  `nomodule` fallback) runs in HTML's order, each file fetched once. With
  the preloads, three Vite builds over the wire: the shop demo's React app
  renders its login page whole (it had died on the polyfill's first
  statement on the P5), and TodoMVC's Svelte and Vue builds get past the
  polyfill to the next surface each needs: the `Text` interface object
  and `SVGElement` (Vue's mount asks `instanceof SVGElement`). On the P5
  the shop logs in and reaches its inventory page; QEMU's keyboard route,
  which left focus on the page rather than in a field, reached react-dom's
  select-event code, which calls `window.getSelection` unguarded when the
  focused element is not a text field, and its absence unmounted the app.
- **P5**: GitHub's page, Chris's to run.

### Import maps, as built

Slice 3's DEBTS row, built at the end of the 0f page-fix batch because
GitHub's pages import `react` and its siblings by bare name (192 modules on
one of Chris's P5 records said "a bare name needs an import map"). HTML
§ 8.1.5 whole, as of the merging revision (any number of maps, before or
after modules load, each merged into the window's), except `integrity`,
which is read for its shape and dropped: yonder checks no Subresource
Integrity at all (DEBTS.md).

- **Two halves.** libdom's `os64_dom_import_map` is the JSON and the
  exceptions: "parse an import map string" up to normalizing, run as one
  task named as the script, so its SyntaxError, or a TypeError for a
  top-level value, an `imports`, `scopes` or `integrity`, or a scope that
  is no object, reaches the window as any task's exception does, and the
  map registers nothing. Every TypeError is asked before an entry is
  walked; normalizing throws nothing, so the answer is HTML's. A map that
  parses is walked to the host entry by entry, in JSON.parse's order.
  yonder's `importmap.c` is the rest, pure: "normalize a specifier key",
  "sort and normalize a module specifier map" and the scopes, the merge,
  the resolved module set, and "resolve a module specifier", which every
  import, static or dynamic, now goes through (`modules.c`
  `resolve_against`).
- **One spelling for an address.** A rule's key and a specifier meet only
  when they are spelled alike, so the map's keys and addresses, the base
  URLs it is asked against and every specifier go through libpage's rule
  (`os64_page_resolve_in`), as libdom's `URL` does. Until now an absolute
  specifier was kept as written while a relative one went through
  libos64's grammar, so one module spelled two ways was two modules: that
  DEBTS row is paid. What the grammar still does not do is what the map
  cannot (the URL grammar's row in DEBTS.md: `https:host`, extra slashes, a
  percent-encoded host, a backslash). An opaque `data:` URL is spelled by
  the same rule at any length (`os64_page_resolve_in_alloc`), since it
  carries its module in itself: a page's inline module written as one, past
  the 8 KiB an address is held to, was taken for a bare name.
- **A script's base URL is fixed when it is prepared**: an import map's,
  for its addresses, and an inline module's, which is its base for
  imports, its scope match and its `import.meta.url`. Both read the tree
  as it stood (`os64_page_base_in`). An inline module used the document's
  URL as its meta URL and the tree as it stood at each resolve: that DEBTS
  row is paid.
- **When a map registers.** A parser-inserted `<script type=importmap>` is
  a blocking item: the parse stops at it, it registers, and the parse goes
  on, as HTML executes one at once, so the modules after it resolve
  through it. One a script connects registers on the turn after that
  script's task and before the parse goes on: the stream now runs what a
  parser-blocking script connected before resuming, which is what its own
  comment always said and what a classic inline script connected that way
  now gets too. libdom reports a connected import map to the host
  (`OS64_DOM_SCRIPT_IMPORTMAP`, a script kind of its own), which it did not.
  The kind is HTML's type rule as it now stands (whatwg/html#13012): a
  type is trimmed only to ask whether it is JavaScript's, so `importmap`
  and `module` match as written, in any case, and `type=" module "` runs
  nothing, where libdom used to trim it into a module.
  `src` on one is a failure on the record, nothing fetched; the `error`
  event HTML fires at it is the script element's, which yonder fires for
  none (DEBTS.md). With the switch off a map is MISSING on the record, as
  a module is.
- **`HTMLScriptElement.supports(type)`**: true for `classic`, `module` and
  `importmap`, false for anything else, `speculationrules` included. A shim
  that loads modules itself (es-module-shims) asks it before taking over,
  and that takeover goes through blob: URLs, which the module map does not
  fetch.
- **The record.** A bare name nothing maps is a line of the importing
  module's, worded for a page with a map ("a bare name no import map
  maps") and one without ("a bare name needs an import map"); a rule
  blocked by a null entry says so. A rule the merge dropped is not on the
  record: HTML's warnings are a console's, and yonder has none.
- **Bounds.** A map holds at most 16,384 rules, all its scopes' together;
  an entry past them fails the parse with a RangeError (the page's map is
  too large, which is no reason to retire its runtime, as an engine's
  out-of-memory would). The resolved module set records at most 65,536
  resolutions, each once; past that a later map's merge could change one
  already made.
- **The merge and the resolution look up; neither scans.** A merge runs
  after the parse's task has closed, where no budget stops it, and HTML's
  merge compares every resolution made with every rule a map brings. Every
  rule that could decide a resolution has a key that is the specifier or a
  prefix of it ending in a slash, every scope that could apply has a
  prefix that is the base URL or such a prefix of it, and every key that
  begins with a specifier sorts just before it: each is a lookup in a
  sorted list, and a dropped rule is marked and the lists compacted once.
  Resolution reads the longest such key that is there, which is the first
  HTML's descending scan would meet.

**Decisions** (web semantics, mine to make):

1. **A connected map registers after the verb**, as yonder runs every
   connected inline script, not inside it: no script runs inside a verb
   here. The stream change makes "after the verb" mean "before the next
   markup", which is the part a page can see.
2. **A module compiled before a map registered resolves again at its
   run.** Its unresolved imports are not in its graph; when it runs, the
   engine asks the loader, which now finds the map's rule and fetches
   inline. HTML would have thrown. The page works where Chrome's would not,
   which is the direction to be wrong in, and a module before its own map
   is the page's mistake.
3. **Integrity is dropped, not refused.** A map with `integrity` registers
   whole; checking nothing is what yonder does with every script's
   integrity today.

**Numbers.**

- **WPT** `import-maps`, a directory added to `tools/wpt_fetch.sh`: 8 → 36
  of 98, 29 of them `[required]` and each failing with the switch off (the
  rest pass either way: a map never runs as a script, and a padded type is
  no map's). The module directory 101 → 104, `type.html`'s padded
  `module` types. The other 39 directories with expectations lose
  nothing. The runner learned two things wptserve does for a static file
  and nearly every import map page asks of one: the `sub` pipe
  (`?pipe=sub`, which fills a file's templates as a `.sub.` name does) and
  the `{{GET[name]}}` template, the request's own query parameter. Before
  it, every module the pages logged through `log.js?pipe=sub&name=B` logged
  the template, and the directory said 8 whatever yonder did.
- **WPT's data-driven cases**, which the directory's own page runs in an
  iframe through `import.meta.resolve` (yonder has neither), driven
  straight into `importmap.c` (`tools/wpt/importmap_cases.py`,
  `tools/test_importmap_host.sh`): 252 checks pass, the 11 that fail are
  the URL grammar's (`tools/wpt/import-maps-data.txt`), and 21 maps whose
  JSON libdom refuses are the DOM host's to hold. The same host holds the
  merge to HTML's own examples, holds 4,096 resolutions, a 4,096-rule merge
  under one 1,800-byte path and 4,096 resolutions through it to a CPU bound
  (2.4 s under the sanitizers; the first build's scan had not finished
  after 120), and fails each of a parse's, two merges' and their
  resolutions' 316 allocations in turn.
- **Hosts**, 0 failed: DOM 15,977 and 105,102 (the parse's walk order,
  every error it throws and the window's events for them, a page's
  `Object.prototype.imports`, a NUL, a refused entry, `supports`, the type
  rule; and every engine allocation of a parse that walks and of one that
  throws, 38 and 56, failed in turn); scripted page 8,149 (a bare name, a
  prefix and a scope through yonder's window, each module fetched once; a
  syntax error, a top-level array and a `src` map; two maps and a
  connected one merging; a `<base>` under the map and the inline module's
  meta URL); yonder 2,028.
- **Mutants** (`tools/test_modules_mutants.py`, its import maps group, and
  `connected-skips-import-maps` beside the modules' own): 32, each caught.
  Two were first missed. The verb fired the parse's error event itself
  and the task's checkpoint fires it again, so the first was dead code and
  went. The merge's "a specifier as a URL that is null or special" is
  equivalent: a prefix rule matches only a bare specifier or a special
  URL, so one kept over a non-special record changes no resolution; the
  harness says so where the mutant would be. Five first did not build (a
  name `-Werror` found unused) and were reworded. Codex's round added five
  (the lookups' runs, deciders and compaction, a prefix that never
  matches, the started mark) and retired one: the scopes' order is
  equivalent once a scope is only looked up. 36, each caught.
- **QEMU**, over the wire: a served site with two maps (the second's rule
  for `react` losing to the first's, its `extra` joining), a prefix rule,
  a scope that gives one widget an older React, an inline module and a
  dynamic import, each resolved as the maps say and fetched once, and
  `HTMLScriptElement.supports('importmap')` true. Its first run proved
  the prefix rule the hard way: the page imported `react/jsx-runtime` and
  the file was `jsx-runtime.js`, which the map sent to a 404, as Chrome's
  would.
- **P5**, Chris, on GitHub's pages: a repository's page fetches 118
  modules through its maps, each once; what stops its code next is
  `customElements`, then `MutationObserver` (slice 5a),
  `IntersectionObserver` and `ReadableStream`, and its placeholders stay
  gray where that code would have filled them.

**Codex, round 1**: three findings. Fixed: an import map that was the
page's first script was never marked started (the binding did not exist
yet when it was prepared), so a script could take it out, rewrite it and
register it again; it now makes the runtime as a module script does, and
the scripted host holds it. Fixed as a class: the merge compared every
resolution with every incoming rule, outside any task's budget, and every
resolution scanned the whole map; both look up now (§ the bullet above,
and the scale case). Booked in DEBTS.md by the triage rule (a hand-made
map): two scope keys that normalize to one URL are combined, where HTML's
later one replaces the earlier.

### Slice 0f, as built

Opened by Opus on 2026-10-25, stacked on slice 3's branch, after the P5's
first module pages each died on a name the binding lacked: the React shop
demo's inventory page on `select.options` (react-dom's option updater),
TodoMVC's Svelte build on `Text`, its Vue build on `SVGElement`. Chris
asked whether those were three names or the signs of families, and asked
for them inventoried and ranked as Window and Document were.

**The inventory**, [INTERFACES_FORMS.md](INTERFACES_FORMS.md), was one
worker agent's (the brief: owner-held, in the Opus kit), by 0e's method:
webref's IDL, BCD, Chrome's counters, webstatus.dev, a static scan of the
four corpus pulls (the P5's whole record directory pulled on 2026-10-25 as
one gzipped tar, 879 records), and a probe of the binding at slice 3's
head. Two families: the DOM's interface objects, which a page uses for
`instanceof` and prototype patches (934 shipping, 61 here), and the form
controls' members (215 shipping, 21 here). It plans P1a to P8 and asks
eight rulings, taken as its leans on 2026-10-25 (ruling 4, SVGElement's
`className`, passed to Quinn with slice 6).

**P1a, the three asks**, built by Opus in four parts:

- **Five prototype kinds** (libdom `node.c`): TEXT and COMMENT under
  CharacterData, SVG under Element, OPTION and OPTGROUP under HTMLElement,
  `d_proto_parent` an explicit table now. Their interface objects joined
  the interfaces row, and `new Text(data)` and `new Comment(data)`
  construct, subclasses included.
- **The node row** (`mixin.c`): `getRootNode` (and Attr's, which the
  shadow must answer), Text's `wholeText` and `splitText`.
- **libpage's option verbs**: an option's selectedness read and set by
  node (the `selected` setter, a single select's reset included), and the
  list-of-options walk and an option's select made public.
- **The select row** (`options.c`): the option's, optgroup's and select's
  members, `options` and `selectedOptions` as live collections (the second
  refreshed by control state too), HTMLOptionsCollection's prototype with
  `length`, `selectedIndex`, `add`, `remove` and the indexed setter
  (`options[options.length] = new Option(...)`, the old idiom), and the
  `Option` factory.

What it left is DEBTS.md § Classic browser scripting's (SVGElement's own
members, a select's own indices, `splitText` inside a pair) and
INTERFACES_FORMS.md's later phases (HTMLOptionsCollection's interface
object waits for HTMLCollection's in P1b).

**Numbers.**

- **WPT**: three directories added to `tools/wpt_fetch.sh` (same pin),
  each measured at slice 3's head and at this one:
  `html/semantics/forms/the-select-element` 26 → 119 of 436,
  `the-option-element` 3 → 94 of 108, `the-optgroup-element` 1 of 1 either
  way; `dom/nodes` 1,722 → 1,733 (the Text and Comment constructors,
  `rootNode`). All 40 directories with expectations pass with no pass lost.
- **Hosts**, 0 failed: DOM 15,610 and 102,668 (four assertions that named
  the old chain say the new one); libpage 254,153, its new option cases
  among them; scripted page 8,076; yonder 2,028.
- **Mutants** (`tools/test_interfaces_mutants.py`): 24, each caught. Two
  were first missed, and their cases were written: a `splitText` offset
  past a two-byte character (the emoji case split where either rule
  splits), and an option under a select that its list skips.
- **QEMU**, over the wire: TodoMVC's Vue build mounts (its header and
  new-todo field rendered, no failure); its Svelte build passes `Text` and
  stops at `document.createElementNS`, which every Svelte 5 element is
  made with: WINDOW_DOCUMENT.md's P1-Db, with `template.content` and
  `document.importNode` beside it. The shop's inventory page needs a
  login, which is Chris's to do on the P5.

**Codex, round 1**: two findings, both an option's dirtiness, both fixed.
Selecting one option of a single select made every option dirty, where
HTML dirties the chosen one only, so react-dom's `selected` then
`defaultSelected` left the first choice standing; and `new Option()`
went through the `selected` setter, dirtying an option HTML's factory
leaves clean (libpage `os64_page_node_init_option_selected`). The select
directory went 119 → 124 with them, `select-setter-dirtiness.html` and
`option-selectedness-script-mutation.html` among the five; three mutants
were added for the new rules, each caught.

### 0f's page fixes, as built

Opened by Opus on 2026-10-25, stacked on P1a's branch, after Chris ran
P1a's head on the P5 against the React shop demo and four TodoMVC builds
(Vue, Svelte, plain ES5, jQuery). Vue and jQuery worked and the shop took
an order; the ES5 and Svelte builds did nothing, and the shop's PDF button
said its control belonged to no form. In QEMU the shop's login fields
typed nothing. Each fix below is a
name or a rule one of those pages stopped on, found by its FAILED line, by
yonder's console, or by running the page's own bundle through the
scripted-page host and reading the tree it left:

- **A formless button is the script's** (yonder `form_send_as`). A button
  with no form, or of a type that submits nothing, does nothing by HTML's
  rule, and yonder said "that control belongs to no form" on every press,
  which is wrong on a page whose scripts act on it (the shop's PDF button,
  whose handler fails on WebAssembly, which QuickJS does not have: the
  failure is now the status line's last word). Without scripts the
  sentence stays, since it is then the reason nothing happened.
- **NodeList apart from HTMLCollection** (`collection.c`, `classic.c`):
  `childNodes` and `getElementsByName` are NodeLists over NodeList's
  prototype, and `NodeList`, `HTMLCollection`, `DOMTokenList`,
  `DOMStringMap` and HTMLOptionsCollection's interface objects are named,
  P1b's four list names built ahead (INTERFACES_FORMS.md § P1b). The ES5
  build's helpers stopped on `NodeList`.
- **DOMParser, for HTML** (`implementation.c`): `parseFromString` with
  `text/html` parses into a new document with scripting off; the XML types
  are a NotSupportedError (DEBTS.md). The ES5 build's next stop.
- **A text control's selection and the window's Selection**
  (`selection.c`, libpage `state.c`; P2's core, built ahead,
  INTERFACES_FORMS.md § P2). React's input tracking asks the focused field
  for `selectionStart`, and without it fell through to `getSelection`,
  which did not exist, so a keystroke in the shop's login field threw in
  React's handler and the field stayed empty (in QEMU). The selection is the scripts' record, kept in
  libpage's control state; yonder's field caret is not tied to it
  (DEBTS.md).
- **`document.createElementNS`**: every element Svelte 5 makes, and the
  shop's next stop once the field typed. The name is validated as DOM's
  2025 "validate and extract" has it; the namespace must be one the tree
  holds (HTML, SVG, MathML).
- **HTMLTemplateElement and a template's `content`**: Svelte builds its
  markup by setting a template's `innerHTML` and cloning its content.
- **HTMLElement's members where HTML puts them** (`mixin.c`, `classic.c`):
  `hidden`, `lang`, `dir` and `title` on HTMLElement's prototype,
  `tabIndex` and `dataset` on HTMLElement's and SVGElement's, not
  Element's. Svelte 5 sets a member as a property only when it finds its
  setter below Element.prototype, so `main.hidden = false` became the
  attribute `hidden="false"`, and the todo list it had just filled stayed
  hidden: Svelte's silent add.

**Numbers.**

- **WPT**, all 40 directories with expectations against P1a's head: one
  pass lost, `dom/nodes`' `Element-tagName.html` "tagName should be updated
  when changing ownerDocument", which asserts only `if ("DOMParser" in
  window)` and so passed while there was none; it parses `text/xml`, the
  DEBTS row. Gains: `dom/nodes` 1,733 → 2,440 (`createElementNS`'s tables
  and the template pages), `dom/events` +91 (a page built on
  `template.content`), `html/webappapis/scripting` +5 (DOMParser and
  template documents), `the-window-object` +9, `html/dom` +3, the option
  directory 94 → 98, the select directory 124 → 125 (P1a's Codex round took it to 124). `css/cssom`'s +216 are
  one module page's, which slice 3 made runnable and did not record; they
  are recorded here.
- **Hosts**, 0 failed: DOM 104,452 (its new cases a NodeList's and an
  HTMLCollection's prototypes, DOMParser's documents, the selections,
  createElementNS's rules, template content, and Svelte's setter walk
  over HTMLElement's members, which fails with them on Element's); libpage
  254,162; scripted page 8,102 (a formless button's click and its
  handler's failure, and its reason without scripts); yonder 2,028.
- **Mutants** (`tools/test_interfaces_mutants.py`, its fifth group): 25,
  each caught. Five were first missed, and their cases were written: a
  formless button's reason without scripts, a password input's selection,
  `createElementNS` asked of a document of its own and with an empty
  namespace, and a DOMParser document's script connected to the page
  (which the DOM host reports to its host rather than runs, so the check
  is the report's). A sixth, a start past the end kept by the selection's
  setter, was dropped as equivalent: the reader clamps it.
- **QEMU**, over the wire: TodoMVC's Svelte build adds, ticks off and
  filters (its checkboxes are unpainted, as Vue's are: the polish list).
  On the P5, Chris: the ES5 build now works as Vue's does, and the PDF
  button's failure is on the status line. The shop's sort `<select>` is a one-row listbox with no
  dropdown, which is yonder's next fix, not the binding's.

**Codex, round 1**: two findings, both fixed. DOMParser dropped a whole
document's `<html>` attributes (`lang`, a theme's class): the fragment
parse keeps only its root's children, so libhtml gains
`os64_html_parse_fragment_root`, the same parse returning that root as the
`html` element it is, which becomes the document's element. And a text
control's selection was set when its value could not be read (out of
memory), clamped to nothing and reported done: the value is read first
now, and its failure is the verb's, nothing changed (the binding's
QuotaExceededError). Cases in the DOM host, libhtml's fragment host and
libpage's tree suite (the last fails on the old code); three mutants for
the new rules, each caught.

**Codex, round 2**: three findings. Fixed: DOMParser's parse took the
hosting page's quirks mode, so a quirks page nested a table in a
paragraph; the kept root is parsed in no-quirks mode now, the mode the
document says it is in (a libhtml fragment-host case and its mutant).
Booked in DEBTS.md by the triage rule (hand-made, or a long and unlikely
chain): a prefixed name in `createElementNS` names the element whole, and
a kept selection is clamped when read rather than when a clean control's
default shrinks.

### 0f's P1b, as built

What P1b had left after the page fixes named the four lists
(INTERFACES_FORMS.md § P1b): EventTarget and Window.

- **One EventTarget.prototype** (`event.c` `d_event_install`) holds
  `addEventListener`, `removeEventListener` and `dispatchEvent`, which had
  been copied onto Node.prototype, the global and every plain target's
  prototype. Node.prototype inherits it; the global inherits
  Window.prototype, which inherits it; and `d_event_target` makes a plain
  target's prototype inherit it when its chain does not reach it already
  (Attr's does, through Node's). These levels are there with the switch
  on or off, so a page with it off finds the same methods by the same
  lookup, and a bare `addEventListener` is still window's.
- **The row** (`d_targets_install`, the table's last) names
  `EventTarget` and `Window`. `new EventTarget()`, and a subclass's
  `super()`, makes a plain target of its own class with its entry in the
  object, as an AbortSignal's is, so `class Bus extends EventTarget` works;
  `new Window()` is a TypeError. Every global interface whose prototype
  inherits EventTarget.prototype directly comes to inherit EventTarget as
  WebIDL's interface objects follow their prototypes (`Node`,
  `AbortSignal`, `XMLHttpRequestEventTarget`, …), which is why the row runs
  last. The engine's own `[Symbol.toStringTag]` of "global" on the global
  is deleted, so `String(window)` is "[object Window]".
- **Not built:** `WindowProperties` and named access stay in P3c, and
  `addEventListener`'s `signal` option and `passive` stay as they were
  (dom/events' failures say so).

**Numbers.** WPT, against userland 8688df17: `dom/events` +17,
`the-window-object` +61 (`window-prototype-chain.html` among them, all but
its WindowProperties tests); nothing lost there, in `dom/nodes` or in
`html/dom`. `dom/nodes`' `ParentNode-querySelectors-namespaces.html` passes
on userland without this branch and is not recorded here. Hosts, 0 failed:
DOM 15,682 and its allocation sweep 104,577 (a plain and a subclassed
target, the chains with the switch on, bare calls, the adopted interfaces,
the tags); scripted page 8,135. Mutants (`tools/test_interfaces_mutants.py`,
its P1b group): 14, each caught. QuickJS needed patch 0013 first:
`JS_SetPrototypeInternal` leaked its reference to the new prototype when the
shape update failed, which the allocation sweep found the first time libdom
reparented a prototype it had made (`userland/libjs/UPSTREAM_REVIEW.md`).

### 0f's P2, as built

What P2 had left after its core was built ahead in the page fixes
(INTERFACES_FORMS.md § P2): `setRangeText`, the host hook, and the oracle,
and on the way the rules for where a selection begins and how it moves.

- **Where a selection is** (libpage `state.c`). A control's selection
  begins collapsed at 0, as HTML's does, where it used to read the value's
  end; a value set that differs puts it at the new end and keeps it there;
  the same value set again leaves it; a clean control's default that
  changes leaves it where it was, and a read clamps it to the value as it
  stands and keeps the clamp.
- **`setRangeText`** (libdom `selection.c`), both of WebIDL's overloads:
  the replacement spliced into the value in UTF-16 code units (an end
  between the halves of a pair, which UTF-8 cannot split, is taken past
  the character), the value made dirty as a script's set makes it, and the
  selection placed by `select`, `start`, `end` or `preserve`. An
  IndexSizeError for a start past the end; a TypeError for two arguments
  or an unknown mode; no input event, as a script did it.
- **The select event.** A set that moves the selection, in extent or
  direction, queues `select` at the element: a binding timer due at once,
  which the host runs as the task HTML's "queue an element task" makes,
  trusted, bubbling and not cancelable. Three WPT pages wait on it, and
  each timed out whole without it. There is no `onselect` (the handler
  mask is full) and a burst is not coalesced: DEBTS.md.
- **The field's caret and the record are one** (yonder's forms, the host
  hook). A person moves a field's caret in bytes of its text; a script
  reads and sets the record in code units of the value. Before a script
  runs, a field whose caret moved since the two were last in step writes
  it into the record (`forms_flush`, which already wrote a person's text
  there); after a script's task, a record that moved moves the caret
  (`script_rebuild`), through libos64's new
  `os64_ui_textfield_select_range`. A password field shows a bullet a
  character, so its offsets count characters. Neither side moves the other
  until it moves itself: a field nobody has touched keeps the caret the
  widget gave it, at the end of its value, and a script reads 0 there, as
  HTML's record says; a widget made after a script moved the record (a
  password field become a text one) takes the record's selection. Making
  the caret follow the record from the first look put every untouched
  field's caret at its start, and a person who tabbed into a search box
  typed before its text; the scripted host's key-edit case caught it.

**Numbers.**

- **WPT**, two directories added to `tools/wpt_fetch.sh`:
  `html/semantics/forms/textfieldselection` 272 → 449 of 792, 388 of them
  `[required]` (each fails with the switch off); and
  `html/semantics/forms/the-input-element` 810 → 842 of 2,000, its 492
  `[required]` P1a's and the page fixes' as well as P2's, a list P3 grows.
  The other 41 directories with expectations lose nothing. What still
  fails in the first is named in its file: `onselect`,
  `requestAnimationFrame`, a clamp with no read between a shrink and a
  regrowth, a textarea's `defaultValue` (P3), a `form.reset()` the host
  carries out after the task, and testdriver's actions.
- **Hosts**, 0 failed: libpage 254,173 (where a selection begins, a value
  that differs and the same value again, a clean textarea's default, a
  clamp kept); DOM 15,771 and 105,296 (every `setRangeText` mode, its
  errors, code units, a textarea made dirty; the select event as a task,
  trusted, bubbling, not cancelable, and none for a set that moves
  nothing); scripted page 8,179 (a script's backward selection on the
  caret, a password's characters, a person's selection read, typing and
  moving back, a new widget taking a selection set before it, a rewritten
  value whose selection is restored); libui's
  text 152 and 1,642 (`select_range`, its clamp and its snap to a letter);
  yonder 2,028.
- **Mutants** (`tools/test_interfaces_mutants.py`, its P2 group): 26, each
  caught, three of them the core's re-anchored. Two were first missed: a
  selection end inside the replaced text was in no case, and is now; and
  the pull's first look at a field, written to note it and move nothing,
  was beaten by its mutant, which compared the record with where every
  record begins and so moved a new widget's caret to a selection a script
  had set before it was made. The mutant's rule became the code. A 26th
  holds what QEMU found (below).
- **QEMU**: a served page whose field rewrites its value on every input
  and restores the caret it read, as React's controlled inputs do: `abc`,
  Left twice and `x` give `AXBC` with the caret after the X. The first run
  read caret 2 and drew the caret at the end: the value's new text put the
  caret there, and the selection the script restored left the record
  where it was, so nothing told the field to move back. A field whose text
  the sync replaces now takes the record's selection again, and the
  scripted host holds it.

**Codex, round 1**: three findings, all fixed. A password field edited at
the end of its value whatever its caret said, which the hook made visible
(a script's selection in one, then a key typed, appended): yonder now
edits the value at the bullets' caret and selection, Backspace takes the
character before it, and the keys that are no character (arrows, Home,
End) move the caret instead of being swallowed. A script's
`setSelectionRange(0, 0)` on an untouched field moved no caret, because
the record held those numbers already: libpage now counts the record's
writes (`os64_page_node_selection_writes`) and the pull compares the count
too. And `setRangeText` converted its offsets before its replacement,
where WebIDL converts left to right. Three mutants for the new rules, each
caught: the group is 29.

**Codex, round 2**: one finding, fixed. A click at the end of a field
nobody had touched moved no caret (the widget shows it at the value's end
while the record is at 0), so nothing was published and the focus listener
read 0. A press on a field, or a focus a person's event moves to it, is now
the person's choice of its caret whether or not the caret moved, and the
next flush publishes it; a script's `focus()` chooses nothing. Two mutants,
each caught: the group is 31.

**Codex, round 3**: clean.

### 0f's P3a, as built

P3's first PR (INTERFACES_FORMS.md § P3): the members of input, textarea,
select and button. The form, fieldset, label, legend and output, and the
form's collections, are P3b's.

- **The reflections are one table** (libdom `controls.c`). Most of what the
  four controls add is a content attribute an IDL attribute reflects by
  one of HTML's rules (§ 2.6.1): a string; a URL, read resolved, and as
  written when it does not resolve; a boolean; an enumerated attribute,
  read as its canonical keyword, ASCII case-blind, with a missing and an
  invalid default of its own; and the integers, each with its default and
  its answer to a value out of range (a negative `maxLength` set throws, a
  zero `size` set throws, a zero `cols` set takes the default). A row is a
  member, its attribute, its rule and the interfaces it is on, and one
  accessor reads them all, so a member called on another interface is a
  TypeError, as WebIDL's brand check is. `formAction` reads the document's
  URL when its attribute is missing or empty. `name`, `type`, `value` and
  select's `multiple` and `size` were every page's or P1a's already.
- **The members that are no plain reflection**: textarea's `defaultValue`
  (its child text, set as textContent is) and `textLength` (code units of
  its value); `autocomplete`, read through HTML's autofill grammar
  (`section-*`, shipping or billing, a contact kind for a contact field, the
  field, `webauthn`; `on` and `off` alone, but not on a hidden input), its
  tokens lowercased; an image button's `width` and `height`, which are its
  attributes here and 0 on any other input (DEBTS.md); each control's
  `form`, its form owner; and `labels`, a live NodeList made once, of the
  labels in the control's tree whose labeled control it is, that tree's
  root included, so a label holding a control in no document labels it.
  `d_label_control` is HTML's labeled control, which P3b's
  `label.control` will answer from.
- **`indeterminate`** is libpage's (`os64_page_node_set_indeterminate`): a
  flag with no attribute, which a clone copies (WPT's `cloning-steps.html`
  says so where memory said otherwise). A click clears a checkbox's, a
  person's in yonder and a script's `click()` alike, and a cancelled click
  puts it back with the box. yonder draws it as a bar across the box, on a
  checkbox and on no radio, through libui's checkbox's new third look.
- **The form owner's rules**, which WPT's `form_attribute.html` caught: no
  ID is empty, so `form=""` names no form, even beside a form whose id is
  empty; and a control in no document belongs to its ancestor form
  whatever its form attribute says. libdom's `d_form_owner` and libpage's
  radio groups follow both; libpage's model already did, since its map of
  IDs holds no empty one.

**Numbers.**

- **WPT**: `html/dom/reflection-forms.html`, excluded until this phase,
  5,776 of 8,817, 2,095 of them `[required]`; four directories added to
  `tools/wpt_fetch.sh`, each with its `[required]` the passes that fail
  with the switch off: `form-control-infrastructure` 0 → 66 of 121,
  `the-textarea-element` 8 → 30 of 42, `the-button-element` 19 → 20 of 111,
  `the-label-element` 47 → 52 of 169; and P2's two, `the-input-element`
  842 → 916 of 2,000 and `textfieldselection` 449 → 461. Every page runs:
  the plan's exclusions by name (pickers, `commandfor`, testdriver's
  clicks) are not needed, since each reaches a verdict. The other 40
  directories with expectations lose nothing. What still fails
  in P3a's scope: the reflection pages' `"\0"` cases (a string the binding
  is handed loses its U+0000) and two `formAction` defaults that WPT's
  harness judges by `localName`, which Element lacks (both DEBTS.md); the
  rest is P3b's (`htmlFor`, `label.control`, fieldset, output, the form's
  members), P4's validity, or no interface's (meter, progress, object).
- **Hosts**, on `userland` with SVG's #368 and #369 in: libpage 254,184
  (indeterminate: default, published,
  the revision, a clone, a person's tick of a checkbox and not a radio, a
  script's checked set; and the empty form attribute in the model and a
  radio group), its 3 failures the sheet census's, which counts an SVG
  `style` since #368 while its case still expects six; DOM 15,783 and
  111,223, 0 failed (every rule of the table, the
  brand check, defaultValue, textLength, indeterminate through a click and
  a cancelled one, autocomplete's grammar, width and height, form by
  ancestor and attribute, labels live and in tree order, and with the
  switch off none of them); scripted page 8,258 (the bar drawn on a
  checkbox and no radio, a person's click clearing it, a cancelled one
  putting it back); yonder 2,028; libui's text 152 and 1,645 (the
  checkbox's third look, set without its callback and cleared by a click);
  appearance passes.
- **Mutants** (`tools/test_interfaces_mutants.py`, its P3a group): 32,
  each caught. One was first missed: libui's checkbox clearing its own
  third look on a click, which yonder's sync from the model clears again
  after it, so no yonder case could see it; libui's text host holds it
  now, and the harness runs a Python suite with its flags. A 33rd was
  dropped as no mutant at all: a guard in the model's form owner against
  an empty ID, which its map of IDs never holds; the guard went with it.
- **QEMU**: a served page with two checkboxes and a radio made
  indeterminate by script, a field and its label: the bar drawn on both
  checkboxes and not the radio; the page reading `maxLength`, `labels`,
  `form` and `size` as written; Tab and Space on the first box tick it,
  clear the bar, and its change listener reads `checked=true
  indeterminate=false`.

**Codex, round 1**: three findings. Fixed: an input's `formMethod` did not
know `dialog`, which `formmethod` shares with a form's `method` on every
submit button (WPT's table for input lists only get and post, so the
oracle never asked); and an empty `for` matched an empty `id`, where no ID
is empty. Booked: URL reflections resolve against the window's document,
not the element's own, which mixin.c's reflections share (DEBTS.md). Two
mutants, each caught: the group is 34.

### Element names, as built

What P3a found missing (§ 0f's P3a, as built): Element's `localName`,
`namespaceURI` and `prefix`, in the node row beside getRootNode
(libdom `mixin.c`, `element_names`). `localName` is the name libhtml keeps,
`namespaceURI` its namespace's (`os64_html_namespace_uri`) and `prefix`
libhtml's, each null for none; a binding document holds only HTML, SVG and
MathML elements, so `prefix` is null on every element a script can reach
today, and a prefixed name `createElementNS` is given keeps its prefix in
its name (DEBTS.md's row on prefixed names). With them WPT's reflection
harness sees `formAction`'s default as the document's URL, which P3a's two
failures waited on.

**Numbers.** WPT: 69 new passes in six directories, 68 of them
`[required]` (each fails with the switch off): `dom/nodes` +54,
`html/dom` +8, `dom/events` +4, `html/browsers/the-window-object`,
`the-option-element` and `the-select-element` +1 each; nothing lost. The
DOM host's chain shape and switch-off checks hold the three names; three
mutants in `tools/test_interfaces_mutants.py`, each caught. `css/cssom`'s
`serialize-all-longhands.html` "Computed style" fails on `userland`
without this change (SVG's computed longhands), and is left for its own
fix.

### 0f's P3b, as built

P3's second PR (INTERFACES_FORMS.md § P3): the form's own members and its
collections, and the label, fieldset, legend and output.

- **Four kinds of their own**: HTMLLabelElement, HTMLFieldSetElement,
  HTMLLegendElement and HTMLOutputElement, made by the interfaces row
  (`d_node_kind`, the classic interface table).
- **Their reflections are P3a's table's rows** (libdom `controls.c`): the
  form's `acceptCharset`, `action` (the document's URL when missing or
  empty, as formAction's), `autocomplete` (on or off, `on` by default),
  `enctype` and its `encoding`, `method` (get, post, dialog; `get` by
  default), `noValidate` and `target`; a fieldset's `disabled` and
  `name`, an output's `name`, a label's `htmlFor`, a legend's `align`; and
  P3a's `form` and `labels` reach a fieldset and an output.
- **The rest** (libdom `forms.c`, a row of its own): a form's `length` and
  `rel` and `relList` (mixin.c's, a link's, whose `supports` names nothing
  yonder acts on for a form); a label's `control` (P3a's
  `d_label_control`) and `form` (its control's form owner); a fieldset's
  `type` and `elements` (a live collection of its listed descendants); a
  legend's `form` (its fieldset parent's); and an output's `value`,
  `defaultValue`, `type` and `htmlFor`, a DOMTokenList over `for` (the
  DOMTokenLists now say which attribute they are over, `D_TOKENS_*`). An
  output keeps HTML's default value override on its wrapper's record.
- **A form's controls** (libdom `collection.c`): `elements` lists HTML's
  listed elements (fieldsets, outputs and objects with them, an image
  button left out, which a fieldset's `elements` keeps: `d_listed`, a
  classic fix both ways) and is an
  HTMLFormControlsCollection, whose `namedItem`, and named access on it and
  on the form, answer a RadioNodeList for a name two or more controls
  share (HTMLCollection's own `namedItem`, called on it, stays the
  first): a live NodeList, one a form and a name, whose `value` is its
  first checked radio button's and sets the first of that value. A
  NodeList has no named properties (`node_list`), which also corrects
  P3a's `labels` and `getElementsByName`.
- **The form as a legacy platform object**, with the switch on: `form[i]`
  is its i-th control; a control's name hides the prototype's member of
  that name ([LegacyOverrideBuiltIns]: an input named `submit` is
  `form.submit`, as in every browser); its own names are its indices and,
  not enumerable, its controls' ids and names, a name an index or an
  expando already spells listed once; no index is defined on it, one it has no control for yet
  included (it has no indexed setter), and an index or a name it has is
  not redefined or deleted.
- **Beside them**: Node's `baseURI` (and an Attr's), which WPT's form
  pages resolve their expectations against: the node's document's base
  URL with its fragment, which resolution never keeps (libpage's
  `os64_page_base_url_in`), a document a script made included, whose own
  `<base>` counts against its fallback, about:blank;
  `autocomplete="webauthn"`
  alone; and, classic fixes both ways, `requestSubmit(submitter)` held to
  HTML (no submit button: a TypeError; another form's: NotFoundError; the
  submission the submitter's), with a form in no document submitting
  nothing, not even its event.

**Numbers.**

- **WPT**: five directories join the run (`tools/wpt_fetch.sh`):
  `the-form-element` 114 passes (81 of them on `userland` before this
  change), `the-fieldset-element` 14 (9), `the-legend-element` 5 (0),
  `the-output-element` 2 (0) and `common-dom-interfaces/collections` 53
  (41); their `[required]` lines, 88, 5, 5, 2 and 37, are every pass that
  fails with the switch off, the earlier phases' among them. And
  `the-label-element` +20, `form-control-infrastructure` +40 and
  `html/dom`'s `reflection-forms.html` +508, all `[required]`. Nothing
  lost, here or in P3a's directories; with the switch off, no required
  line passes.
- **Hosts**, 0 failed, on `userland` with #374 in: DOM 17,215 and
  113,265 (the four kinds, every reflection rule above, `elements` and
  `length`, `form[i]`, RadioNodeList's identity and value, the legacy
  platform object, the output's override, `baseURI` and its fragment,
  `requestSubmit`'s refusals; and with the switch off, none of it); the
  scripted page 8,429; yonder 2,028; libpage 254,199 (the document base
  URL, a fragment's every source).
- **Mutants** (`tools/test_interfaces_mutants.py`, its P3b group): 30,
  each caught — a rule of the table (method's default, autocomplete's), an
  output's labels, the listed elements (a fieldset's image button too),
  form[i], the names over builtins, the own names (an index's or an
  expando's spelling once), redefinition (an index it has no control for
  too) and deletion, `baseURI`'s fragment (libdom's and libpage's) and a
  made document's base, the controls collection and RadioNodeList (made
  once, radios only, the value set, the base namedItem the first), a
  NodeList's names, the output's override both ways, a label's form, the
  submitter's two refusals and a detached form.

### The page's style sheets, as built — 2026-10-27

Not a phase of 0f's: the record of a real page's failure asked for it.
Google's sign-in script loads late CSS, counts `document.styleSheets` to
see its sheet arrive, and finds it by its `ownerNode`; yonder had no
`styleSheets`, so the script died reading `.length` of undefined
(replayed with the FAILED line's place, #381) and nothing it wires,
the Next button among them, was ever wired. Chris ruled the slice
2026-10-27, as long as it was not a whole day.

- **`document.styleSheets`** (libdom `collection.c`): a live
  StyleSheetList, made once a document, a collection of the query kind
  SHEETS. Which nodes have a sheet is libpage's rule, the one the cascade
  ranks by, now public (`os64_page_names_sheet`): a `style`, HTML's or
  SVG's, and a `link` whose `rel` holds `stylesheet` and not `alternate`,
  with an `href` and not `disabled`; neither of a type other than CSS.
  Its entries are sheets, not nodes; `length`, `item()` and its indices,
  iterable, no names.
- **A sheet** (libdom `sheets.c`, a row of its own): a CSSStyleSheet over
  StyleSheet, one a `style` or `link`, made once and kept on the node's
  record. It says `type`, `href` (a link's, resolved as the page resolves
  it; a style's null), `ownerNode`, `title` (null for none or empty),
  `disabled` (false), `parentStyleSheet` and `ownerRule` (null). A node
  that no longer names a sheet, or is in no document, has none (`sheet`
  null), and its sheet no `ownerNode`.
- **LinkStyle's `sheet`** on a link and a style, which made the style
  element a kind of its own: HTMLStyleElement (the interfaces row).
- What a sheet holds is not here: no `cssRules`, no `media` list, and
  `disabled` cannot be set (DEBTS.md).

**Numbers.** WPT: `css/cssom` +34, all `[required]` (StyleSheet's
properties, StyleSheetList, the document extensions, titles); and
`html/semantics/document-metadata/the-style-element` joins the run
(`tools/wpt_fetch.sh`), 16 passes, 14 of them `[required]`; nothing lost
in `the-link-element`, `css/cssom-view` or `html/dom`, and with the switch
off no required line passes. `css/cssom`'s `serialize-all-longhands.html`
"Computed style" fails on `userland` without this change, as P3a found.
Hosts, 0 failed: DOM 17,337 and 114,000 (the interfaces, the list live
and made once, a style's and a link's sheet, an alternate sheet and
another language naming none, a removed style's, the brand; with the
switch off, none of it). Mutants (`tools/test_interfaces_mutants.py`,
its group for this slice): 9, each caught.

### 0f's P4a, as built — 2026-10-28

P4's first PR (INTERFACES_FORMS.md § P4): constraint validation's API
whole, and every flag but range and step. A real page asked for it first:
Google's sign-in reads `input.validity.badInput` when Next is pressed, and
with no `validity` it threw on every press.

- **One judge** (libpage `submit.c`'s `judge`, public as
  `os64_page_node_validity`): every flag of a control from the node and
  the live state, so a control no model holds (WPT judges detached
  clones) is judged, and the same judge a submission's validation asks
  control by control. It says `valueMissing` (a field, while it can be
  typed into; a box, a radio group and a file whatever their mutability;
  a list's placeholder; a radio with no name is in no group, as browsers
  answer), `typeMismatch` (`email` and its lists, `url` as a scheme check:
  DEBTS.md), `tooLong`/`tooShort` on a person's edit, `badInput` (a
  person's text a number or a date type could not hold, recorded by the
  state and cleared by any other value and by a reset), and `customError`
  (a message kept on the node's record, kept by a reset, not copied by a
  clone). `patternMismatch` is the caller's: HTML's pattern is the
  engine's regular expression. `readonly` bars any input it is spelled
  on, as browsers do and WPT holds them to, though it takes effect only on
  the text-like types (a ruling made here: the model barred only those).
- **The binding** (libdom `validity.c`, a row of its own): the pattern
  compiled alone with `v`, then `^(?:pattern)$`, by the RegExp taken
  before any page ran; ValidityState, made once an element and read live;
  `willValidate`, `validity`, `validationMessage`, `checkValidity`,
  `reportValidity` and `setCustomValidity` on input, select, textarea,
  button, fieldset and output; a form's `checkValidity` and
  `reportValidity`; `invalid`, cancelable and not bubbling, and
  `oninvalid` (the handler mask is 64 bits now: the 33rd event kind).
- **Submission validates before `submit`**: `requestSubmit()` and a
  submit button's `click()` (window.c's `submit`), and a person's button
  and Enter (yonder's `page_validates`, through `os64_dom_form_validate`
  as a task whose validation is one call through the wrapper, so the
  pattern and the invalid listeners run under the task's budget, as a
  script's `checkValidity()` runs them; by the submitter libpage would
  take, now public as
  `os64_page_submitter`). It is validated once, as HTML does: what a
  submit listener changes after it cannot refuse the submission, so the
  model, told (`os64_page_activate_validated`), does not judge it again;
  and a form being submitted cannot be submitted again from its own
  invalid or submit listener (HTML's firing-submission-events flag). A
  report (ruling 5) is the host's
  (`OS64_DOM_ACTIVATE_REPORT`): the control focused, its sentence on the
  status line; the model's own refusal says the control's sentence too.
- **libjs patch 0014**: the pinned engine applied the `v` flag's class
  rules to every atom outside a class, so `new RegExp("a/b", "v")` threw
  and every pattern with a slash or a hyphen was no constraint. The rules
  now hold where ES2024 puts them (UPSTREAM_REVIEW.md).
- **Left**: range and step (P4b); a pattern in a submission the model
  judges alone; `:invalid`; an image button's events (DEBTS.md).

**Numbers.**

- **WPT**: `html/semantics/forms/constraints` joins the run
  (`tools/wpt_fetch.sh`): 755 passes of 921, 754 `[required]` (one passes
  either way); what fails is range and step, `:invalid`, an element in no
  namespace, `object`, `execCommand`, `getClientRects` and named access.
  `the-input-element` +17, `the-select-element` +11, `the-button-element`
  +11, `the-fieldset-element` +10, `the-textarea-element` +3,
  `the-form-element` +2, `the-output-element` +1 and the window object's
  `oninvalid` +1, all `[required]`. Nothing lost there, or in `html/dom`,
  `dom/events`, `dom/nodes`, `html/webappapis/scripting` and P3's other
  directories; `dom/nodes`' 14 and `dom/events`' 1 new passes are P3b's
  and the style sheets', recorded now. With the switch off no required
  line passes.
- **Hosts**, 0 failed: DOM 17,810 and 115,923; the scripted page 8,567;
  yonder 2,028; libpage 254,235; libjs's runtime 1,285 and 6,115.
- **Mutants** (`tools/test_interfaces_mutants.py`, its P4a group): 62,
  each caught — the judge (an unnamed radio, a group's tick, readonly's
  bar both ways and a select's, a fieldset's legend, a placeholder's text
  without its scripts, mutability for a field and not a box, an email list
  and a label, a URL's scheme and host, where a pattern applies and an
  empty value, an email list matched address by address, bad input's
  record, person, white space, reset and type change, the custom message
  kept, judged and said and cut between
  characters, a NUL in an address, a submission judging every flag and
  not again once validated, a submitter for a button or Enter alone); the
  binding (the pattern matched,
  anchored, compiled alone, with `v`; invalid's bubbling and
  cancelability; checkValidity's event; a barred control; ValidityState
  made once and `valid`; a barred control's message, and a long custom
  one whole; setCustomValidity; a form's own controls; the report;
  formnovalidate both in the binding and for the host; submit() and
  requestSubmit(); no submission inside a form's own, in the binding and
  for the host, its validation and its submit event; a person's
  validation under the task's budget; oninvalid); and
  yonder (an onsubmit attribute alone validated before it runs; a button
  and Enter validated, and
  not again by the model, the report's focus and sentence, the refusal's
  sentence). `tools/test_yonder_loop_mutants.py`'s two that this moved are
  re-anchored, and caught.

### 0f's P4b, as built — 2026-10-28

P4's second PR: `rangeUnderflow`, `rangeOverflow` and `stepMismatch`,
which P4a left reading false.

- **Each type read as a number** (libpage `value.c`'s `p_input_number`,
  HTML's "convert a string to a number", which `valueAsNumber` can share):
  a number and a range as themselves; a date as milliseconds since
  1970-01-01, a month as months since January 1970, a week as its
  Monday's milliseconds, a time as milliseconds since midnight, a local
  date and time as both. The year is read whole, not as the residue the
  syntax check keeps, through 275760, where a Date's range ends and a
  double still holds every millisecond (a later year the sanitizer keeps
  is judged by nothing: DEBTS.md). A number's and a range's strings are read by HTML's lenient
  rules for floating-point values, as range.c's sanitizer reads min, max
  and step: a stricter judge put a range's own value off its grid.
- **The flags** (`submit.c`'s `range_flags`, the judge's, so a
  submission's validation obeys them too): below a min that converts,
  above a max that does (a range's default to 0 and 100, and a range's max
  below its min taken as its min, as Blink takes it, since the value
  sits at the min and no slider moves it lower); a time whose max
  is below its min wraps past midnight and is out of range only between
  the two. The step draws a grid from its base (the min, else the value
  attribute, else the type's default, a week's the Monday 1969-12-29),
  each type's scale and default step applied, `any` drawing none; whether
  a value is on it is judged in exact decimal, the scale multiplied there
  too (`range.c`'s arithmetic, `p_step_aligned`), as `step=1e-12` against
  `-12345678.9` and a time's `step=16.1` need. A range's value, which its
  sanitizer chose on the grid and held as the nearest double, is on the
  grid when it is that double (a step of seventeen digits, or one finer
  than the value's precision, puts the point between doubles); a number a
  script or a person gave is judged as given, as WPT's `step=3e-15`
  against 17 asks. No step judges a range pinned at its min. A bound's time fraction is read at
  any length, its first three digits kept.
- **The sentences**: "Value must be greater than or equal to 2." for a
  number, "Value must be 2000-01-02 or later." for a date: a date's bound
  as the page wrote it, a number's as the number it was read as ("5px"
  says 5); and "Value must be 22:00 or later, or 02:00 or earlier." for a
  time whose range wraps, which no other type does.

**Numbers.** WPT: `constraints` +140, all `[required]`, 895 of 921; what
fails is `:invalid`, an element in no namespace, `object`, `execCommand`,
`getClientRects` and named access. Nothing new or lost in P3's and P4a's
other directories; with the switch off no required line passes. Hosts, 0
failed: libpage 254,277 (42 range and step cases across the types); DOM
17,810 and 115,923. Mutants (`tools/test_interfaces_mutants.py`, its P4b
group): 21, each caught — a date's, a month's, a week's and a time
fraction's conversion, the year's bound, a number read leniently, a
week's step base, a time's wrap and its sentence (and a date's that does
not wrap), `step=any`, the step base from the min and from the value
attribute, the exact remainder and the scale multiplied exactly, a
range's max below its min and its pinned value's step, a range's grid
point held as a double and only a range's, a bound's long time fraction,
and a number's bound as read.

### 0f's P5, as built — 2026-10-28

HTMLInputElement's value as a number: `valueAsNumber`, `valueAsDate`,
`stepUp(n)` and `stepDown(n)` (HTML § 4.10.5.4), over P4b's conversions.

- **One reader of min, max and step** (libpage `value.c`'s
  `p_input_limits`): the validity judge's `range_flags` and the steps read
  the same numbers, a range's defaults and its pinned max among them.
- **A number spelled for its type** (`p_input_spell`), Blink's rules: a
  date, a local date and time, a month and a week round to the nearest
  millisecond (or month), a time takes the floor of its place in the day,
  exact for any double (2.7343337071894478e26 is 10:54:10.944); a week is
  the ISO week holding the day; years 1 to 275760, the years a value's
  number is read through, and anything else is the empty value (Blink's
  last day is 275760-09-13). A month's Date is its first midnight.
- **The steps** (`range.c`'s `p_step_by`), in exact decimal over each
  double's shortest spelling, as WPT's 1.2 − 0.3 = 0.9 needs. Rulings, the
  engines over the spec's text where they agree and WPT with them: an
  off-grid value's move onto the grid is one of the n steps (Blink and
  Gecko); the guard against moving the wrong way reads the signed count,
  so `stepDown(-1)` moves up (both engines; the spec names the method);
  and a value that did not convert skips that guard (Blink, and WPT's
  `input-stepdown-02`). Where the engines part — Blink checks the count
  left after the snap — the spec and Gecko are followed: `stepDown()` on
  4 with min 5 and step 2 leaves 4.
- **The binding** (`controls.c`): `valueAsNumber` converts first, then
  refuses infinity (TypeError), then a type it does not apply to
  (InvalidStateError), NaN emptying the value; `valueAsDate` takes
  WebIDL's `object?` first (undefined is null, any other primitive a
  TypeError), then whether it applies, then a Date's time value through
  the `getTime` taken before any page ran (`d_intrinsic`, now shared with
  the validity row's RegExp), which refuses any other object. A value
  past a Date's range reads as null, not an invalid Date. `stepUp` and
  `stepDown` take `n` by ToInt32, and refuse `step=any` as having no
  allowed value step.

**Numbers.** WPT: `the-input-element` +235, all `[required]`, 1,168
passing; every case in the phase's oracle passes but
`datetime-local-valueasdate.html`, which reaches its input by named
access. Nothing new or lost in `constraints` (895) or `html/dom` (6,374);
with the switch off no required line passes. Hosts, 0 failed: libpage
254,352 (75 value-as cases); DOM 17,802 and 116,116. Read cold once: two
code findings (`undefined` and a primitive given to `valueAsDate`), one
edge (an invalid Date for late 275760), and four of text, all fixed.
Mutants (`tools/test_interfaces_mutants.py`, its P5 group): 28, each
caught — a time's exactness and its negative fraction, a day rounded and
floored, the last year spelled, a week's year and weekday, a month as a
count and as a Date, which types take a Date, NaN and infinity, the snap
as a step, the guard both ways, the max's grid clamp, no grid between
the limits, a carry across words, `step=any`, stepDown's sign, and the
binding's getTime, its order of errors, undefined, null, a Date past the
range, a number not a Date, and `n`'s default.
Two rules have no mutant a test can see: a spelled year below 1 (the
value sanitizer empties it either way), and the min's clamp onto a grid
it is off (only a range's default min can be, and its sanitizer
re-snaps). P4b's group was re-anchored onto `p_input_limits`.

### 0f's P6a, as built — 2026-10-28

P6's first PR: the element kinds whose members are reflections, every
interface of the plan's 36 a prototype (34 new; template and style had
theirs), and their members but the table kinds' tree verbs and a
map's `areas`, which are P6b's. The brief is in the opus kit.

- **HTML's element interface by tag** (`node.c`'s `d_node_kind`, a table
  where an `if` chain was): h1-h6 one kind, ins and del, q and
  blockquote, td and th, col and colgroup, the three table sections, pre
  with listing and xmp; the obsolete names HTML gives HTMLUnknownElement,
  and math and svg in HTML's namespace; a name libhtml does not know is
  HTMLUnknownElement unless it is a valid custom element name, which is
  HTMLElement.
- **A set of kinds is a uint64_t** (`D_KIND`): the 32-bit masks the tables
  built (`1u << D_PROTO_*`, and `1u << kind` over every kind) would have
  overflowed. A static assertion holds the kinds to 64 (P6a leaves 63);
  slice 6's SVG interfaces will need another representation.
- **One reflection engine.** controls.c's rules (`d_reflection_get`,
  `d_reflection_set`) serve `elements.c`'s rows too, with four more:
  LegacyNullToEmptyString (a body's colors, a table's), a long with its
  default (`ol.start` 1), a span's clamp to 1-1000 and a row span's to
  0-65534 (read clamped, set as any unsigned), and base's `href`, read
  against the document's own URL and that URL when it is missing.
- **One row per member and interface**, the WebIDL shape, so an
  accessor's brand is its own interface: div's `align` getter called on a
  p is a TypeError. (controls.c's shared rows are not; DEBTS.md.)
- Beside the rows: a title's `text` (its child text, set as textContent
  is), and a body's WindowEventHandlers, which are window's as its other
  window-reflecting handlers already were. An area's `href` is a row of
  its own, a URL as the anchor's is, so that it exists with the switch
  alone.
- **Left, and booked** (DEBTS.md § Classic browser scripting): an area's
  `relList` and the hyperlink members of an area and an anchor; a style's
  `disabled`, whose sheet is never disabled; a template's declarative
  shadow DOM members.

**Numbers.** WPT: `html/dom`'s eight reflection pages for these kinds,
excluded until the phase that names them, now run: 29,000 passes, 5,038
of them `[required]` (the rest pass with the switch off too, and 11 more
are subtests whose names hold a line break, which a line of the
expectation file cannot spell, so they cannot be required); what fails
of P6's own members is a NUL set through a string (U+FFFD, as every
reflection page has it) and every URL reflection, whose expected value
the harness builds from an anchor's `protocol` and `host`, which yonder
lacks. `dom/events` +56, `dom/nodes` +61 (four renamed: a heading now
prints as HTMLHeadingElement), `url` +1 and `xhr` +1, each `[required]`;
the other directories unchanged. `css/cssom`'s serialize-all-longhands
"Computed style" fails on the branch P6 is built on as well, unrelated.
Hosts, 0 failed: DOM 19,894 and 125,221, jQuery among them; yonder's
scripted pages 8,892. Read cold once: an area's `href` had joined a
row that exists with the switch off, a base without `href` kept the
document URL's fragment, and a second copy of the rules for parsing
integers (now one, `d_parse_integer`); four of text. All fixed. Mutants
(`tools/test_interfaces_mutants.py`, its P6a group): 17, each caught —
kinds in 32 bits, an unknown name and a reserved one, h6's and
listing's kinds, the span's clamp and the row span's floor, a long's
sign and its overflow, null as "null", a base's fragment, the brand,
a canvas's default, scope as a string, a title's text, the body's
handlers, and an area's `href` without the switch. Five anchors the
rewrites moved were re-anchored (one in `test_dom_mutants.py`, whose
harness does not build: DEBTS.md).

### 0f's P6b, as built — 2026-10-28

P6's second PR, and its last: what of the 36 interfaces is not a
reflection. A table's `caption`, `tHead` and `tFoot`, read and replaced
(WebIDL's TypeError for an element of another interface, HTML's
HierarchyRequestError for a section of the wrong tag), with their create
and delete verbs; its `tBodies` and `createTBody`; `rows` on a table and
a section and `cells` on a row, each with its insert and delete verbs
(an index below -1 is an IndexSizeError, and so is one past the count
for an insert, at or past it for a delete); a row's
`rowIndex` and `sectionRowIndex`; a cell's `cellIndex`; a map's `areas`.

- **The collections are live queries** (`collection.c`'s table kinds,
  `d_table_query`), made once and kept on the owner's entry
  ([SameObject]). A table's rows are HTML's three groups — a thead's,
  then the table's own and its tbodies', then a tfoot's — each in tree
  order, which a walk a group gives; a nested table's rows are its own.
- **A verb reads the collection a page reads** (`d_query_items`), so
  `insertRow(i)` and `rows[i]` cannot disagree, and changes the tree
  through `d_tree_change`, appendChild's door (scripts a moved caption
  carries connect as they would). What it makes joins the owner's tree
  at once, and with it the owner's node document, so a table in a
  document of its own fills that document.
- A table with no rows takes `insertRow`'s row in its last tbody, or in
  a new one; `insertRow(-1)` appends to the last row's parent, a tfoot's
  when it has rows.
- **Booked** (DEBTS.md): a collection's own property names list its
  indices and not its names.

**Numbers.** WPT: `html/semantics/tabular-data`, fetched for this phase,
+102, all `[required]`, 120 passing. What fails is elsewhere's: an element
in no namespace, DOMParser, a window's named access, a prefixed HTML
element (DEBTS.md), a collection's property names, a table of tens of
thousands of rows past the HTML arena, and `caption-methods.html`, whose page waits for an
iframe's srcdoc to load, which the runner does not, so the page fails
whole. `dom/nodes` +4 and `collections` +5, pages that reach a cell
through `tBodies`, `rows` and `cells`, each required; every other
directory unchanged. Hosts, 0 failed: DOM 20,075 and 126,128; yonder's
scripted pages 8,926; userland builds. Read cold once: a row's or cell's
index threw for a table no script had touched (its collection wanted the
owner's wrapper, which a parsed table lacks; eight of WPT's
`sectionRowIndex` subtests are that), `undefined` given to a part's
setter threw where WebIDL makes it null, and the table kinds' match
fell to the rows' rule for any kind not named; one of text. All fixed.
Mutants (`tools/test_interfaces_mutants.py`, its P6b group): 14, each
caught — the rows' groups and a foot's place in them, a nested table's
rows, a map's areas below its children, [SameObject], deleting the last
for -1, a row's new tbody, -1's parent, a head before the caption, a
body set as a head, the brand, a cell outside a row, an untouched
owner, and undefined as null. Two pieces the first run found no test
could see are gone: the before-itself guard (libhtml's insert already
reads a node as its own reference so) and a node-document record for
what a verb makes (it joins the owner's tree at once).

### 0f's P7, as built — 2026-10-28

The window's own objects as their interfaces': `location`, `history` and
`navigator` are a Location, a History and a Navigator. The brief is in
the opus kit.

- **One row reshapes what the classic install made** (`window.c`'s
  `d_window_objects_install`, after the history row), so the surface
  without the switch does not move. The classic install now makes each
  object over an empty prototype level of its own, as every node kind
  has a prototype either way, so the chains have the same levels with the
  switch on or off; the row makes each level its interface's prototype
  object.
- **History's and Navigator's members move to their prototypes**, the
  objects keeping none of their own, each named and an operation
  enumerable as WebIDL makes them (`go`'s length is 0: its delta is
  optional). **Location's stay on the location**, [LegacyUnforgeable]:
  unconfigurable, an operation unwritable, with its own `valueOf`
  (Object.prototype's) and `Symbol.toPrimitive` (undefined), as HTML's
  "Location object setup" defines them. Each interface object is an
  Illegal constructor.
- **Brand checks**: a Location's member called on anything but the
  location, and a History's on anything but the history object, is a
  TypeError (`d_same_object`), with the switch or without; Navigator's
  getters already checked.
- **The oracle needed two things of the runner**: WPT's `interfaces/`,
  now fetched, and `/resources/WebIDLParser.js`, which WPT's server
  rewrites to the parser's own file and the runner now serves so. With
  both, every directory's idlharness page runs, not only `html/dom`'s.
- **Booked** (DEBTS.md): the location's exotic internal methods
  (preventExtensions throws, an immutable prototype) and its
  `ancestorOrigins`; Navigator's missing members (`language`, `onLine`,
  `cookieEnabled`, NavigatorID's constants and the rest). Paid in
  passing: P6a had given the body WindowEventHandlers, and the DEBTS row
  that said it had none is now its remainder (`onerror`, and a frameset).

**Numbers.** WPT: `html/browsers/history/the-location-interface`, new
here, 42 passing, 22 `[required]` (with P7's, the Modern Web's earlier
fragment and history work); `html/dom`'s idlharness page, excluded until
this phase, +222. The idlharness pages elsewhere, which run now:
`css/cssom` +119, `css/cssom-view` +147, `encoding` +34, `hr-time` +25,
`html/webappapis/scripting` +281, `url` +50, `user-timing` +30, `xhr`
+139 (its timeout tests' sometime passes left out), and
`the-window-object` +1 (the location's unforgeable `hash`). Each fails
with the switch off; nothing lost (`css/cssom`'s serialize-all-longhands
loss predates this and is kept on record). Hosts, 0 failed: DOM 20,222
and 126,669; yonder's scripted pages 8,913; userland builds. Read cold
once: no code defect; the Modern Web table's promise that the chains'
levels are the same either way was broken (a prototype level made only
with the switch), now kept by the classic install's empty levels; wrong
section numbers, half-true comments, and the oracle's exotic Location
unbooked; all fixed. Mutants (`tools/test_interfaces_mutants.py`, its
P7 group): 10, each caught — the location's unforgeability, members left
on the objects, the level without the switch, the three brand checks,
the own valueOf, the names, enumerable operations, and go's length.

### Node's members, as built — 2026-10-28

A bridge between P7 and P8, for two corpus pages: one whose head
react-helmet keeps, comparing its tags with `isEqualNode`, which threw
there, and a sign-in page whose script stopped at
`compareDocumentPosition`. Node's
`isSameNode`, `isEqualNode`, `compareDocumentPosition` and `normalize`,
which DEBTS.md had booked together.

- **One function answers each comparison on Node's prototype and on
  Attr's** (`compare.c`, its own row), since either side may be an Attr,
  which libhtml holds as a name and value on its element: an operand is a
  tree node or an Attr's view (`d_attr_view`), the element standing in
  the tree for it. Attr's prototype keeps shadowing Node's whole.
- **Equality** is DOM's: names, namespace, prefix, attributes in any
  order, data, a doctype's IDs, then the children, walked side by side
  without recursion. A template's contents are not its children, so
  they are not compared.
- **compareDocumentPosition's one choice the standard leaves open**: two
  trees are ordered by their roots' addresses (a detached Attr's own
  entry standing for a root), consistent both ways round. Two Attrs of
  one element are in the order of its attribute list, as DOM's steps
  walk it.
- **normalize** (the node row, `mixin.c`; an Attr's does nothing) sets a
  run's first text node's data before removing the rest, through
  `d_tree_change`, so a failure part way loses no text.
- **Booked** (DEBTS.md): the row narrows to Node's three namespace
  lookups, and the CDATA section, processing instruction and Document
  constructor that WPT's `dom/common.js` needs before the
  compareDocumentPosition page runs at all.

**Numbers.** WPT `dom/nodes` +15 (Node-isEqualNode 6, Node-isSameNode
6, Node-normalize 3), each `[required]`, failing with the switch off;
every other directory unmoved (`css/cssom`'s serialize-all-longhands loss
predates this and is kept on record). Hosts, 0 failed: DOM 20,092 and
126,892; yonder's scripted pages 8,927; userland builds. Mutants
(`tools/test_interfaces_mutants.py`, its group for these members): 19,
each caught — equality's names, namespaces, attributes and their
namespaces and values, the children, a sibling more, an Attr's namespace
and value; the attribute list's order, the
disconnected order, an element containing its Attr, an ancestor first,
siblings' order, a null compareDocumentPosition argument, the Attr
shadow; and normalize's empties, its joins and its descent. An element's
prefix is compared and has no case: only an XML document's elements have
one, a script's having its whole name instead (DEBTS.md, a prefixed name).
Read cold once: no code defect; the order of two Attrs claimed as the
implementation's choice where DOM's steps fix it, the Attr-receiver
divergence not naming the comparisons, and equality's names and
namespaces untested; all fixed. The sign-in page's replay no longer
stops at compareDocumentPosition.

### 0f's P8, as built — 2026-10-29

The event interfaces (INTERFACES_FORMS.md § P8): yonder's mouse and key
events were Events with their fields as data, a page's `instanceof
MouseEvent` a ReferenceError, and `createEvent('MouseEvents')` a plain
Event with no `initMouseEvent`.

- **Seven interfaces in one row** (`event.c`, LIBDOM.md § the UI events
  row): UIEvent, and MouseEvent, KeyboardEvent, FocusEvent, InputEvent
  and CompositionEvent over it; SubmitEvent over Event. Each constructor
  reads its dictionary in WebIDL's order (EventInit's members, UIEventInit's,
  EventModifierInit's for a mouse or key event, then its own); each legacy
  init converts every argument before changing the event, and does nothing
  during the event's dispatch. An event's attributes are its record's, and
  each getter checks its interface. `createEvent` makes each as itself.
- **What yonder tells the binding grew** (`os64_dom_event_t`): FOCUS,
  INPUT and SUBMIT kinds beside MOUSE and KEY, a click's `detail`, the
  buttons held, focus's other control and a submission's submitter. A
  keypress's keyCode is its character's (97 for 'a', as Blink's is) where
  it was the key's (65), which `which` reads; a classic fix, the switch on
  or off.
- **A click's activation moved into the dispatch** (DOM § 2.9), because
  `dispatchEvent(new MouseEvent('click'))` is the common synthetic click
  and WPT asks it: a MouseEvent click a script dispatches activates its
  target, or for one that bubbles the nearest ancestor with an activation
  behaviour, its first half before the listeners and its second after the
  dispatch. `element.click()` is such a click; a host's click is the
  host's to act on, as before, and a plain Event named click activates
  nothing. With the switch off `click()` activates its element around a
  plain Event, as before the arc.
- **Left out on purpose**: PointerEvent (the handler hazard), WheelEvent,
  DragEvent and TextEvent, which nothing here fires.
- **Booked** (DEBTS.md): an edit's and a key's details the host does not
  give (inputType, data, code, location; a keyboard's click says detail
  1); a script-made MouseEvent's pageX without the scroll, and offsetX and
  the rest; an attribute's getter's name; and a link a script follows
  before its page is shown, which goes nowhere. That last one times out
  WPT's `Event-dispatch-click.html` whole, so the page is excluded, by
  Chris's ruling, and the DOM host holds its three required tests.

**Numbers.** WPT: `uievents` added to the fetch list (same pin), 29 → 163
passes of 377 at this branch's base and head; `dom/events` +43,
`html/semantics/forms/the-input-element` +48, `the-label-element` +34,
`dom/nodes` +21, `the-button-element` +17, `html/dom` +6,
`css/cssom-view` +5, `html/webappapis/scripting` +4, the module
directory +1, each of the 313 `[required]` and failing with the switch
off; every other directory unmoved but the known sometimes-passes,
measured over `userland` with SVG's first part merged in. Hosts, 0
failed: DOM 20,298 and 130,080; yonder's scripted pages 11,709; userland
builds. Mutants (`tools/test_interfaces_mutants.py`, its P8 group): 41,
each caught. Read cold once: the activation's ancestor was read after
the listeners unheld, so a listener that took its subtree out freed it
(ASan's heap-use-after-free once the test was written; it is held now);
an init's partly failed conversion, two dictionaries' order and an
infinite coordinate untested; two comments half true; all fixed.

### Slice 6a, XML foundation as built — 2026-10-25

The standalone `libxml` event parser is implemented in Quinn's SVG worktree.
It has no browser tree or runtime dependency, and no script-visible consumer.
[XML.md](XML.md#standalone-foundation-as-built--2026-10-25) records the API,
limits, explicit DTD/encoding profile, host sanitizer and leak evidence, and
xmlconf's supported/unsupported/excluded results. The root/userland makefiles
include its build, image installation membership and debugger symbols because
those shared inventories are required to deliver the library.

The native shared-tree increment is also built in libhtml: independently
owned XML documents, arbitrary/absent namespaces, processing instructions,
CDATA-to-text projection, clone/mutation ownership, and SVG-rooted selector
and custom-property cascade proofs. Its API and sanitizer/regression evidence
are in [XML.md](XML.md#native-shared-tree-as-built--2026-10-25). The first
[FreeType geometry experiment](SVG_GEOMETRY.md) and
[production adapter](SVG_GEOMETRY_BACKEND.md) are built. The
[comparison increment](SVG_REFTESTS.md) adds browser captures, native pixel
comparisons and selected WPT references; the two Chromium baseline failures
and unsupported runner profiles are retained explicitly. XML profile
limitations and browser consumers still to be built are recorded in DEBTS.md; the slice's
overall brief stays active.


### Slice 6b, path and scene foundation as built — 2026-10-26

`libsvg.so` now supplies owned SVG path parsing, transform-specific arc
conversion to the public geometry stream, and group/path scene preparation.
The provider contract preserves source and instance identities, inheritance
inputs, group opacity and snapshot lifetime. It has explicit processing modes,
shared owner admission callbacks and work/cancellation limits. The scene
interface is a source contract for integration, not a frozen browser ABI.

[SVG_CORE.md](SVG_CORE.md) records the contracts, measured precision and
coverage, limits and reproduction commands. The leak-enabled ASan/UBSan
harness uses production SVG, FreeType and pinned math sources; its combined
pipeline passes 23 allocation cuts in both refusal modes and 526 cancellation
cuts, with a fixture peak of 17,912 bytes. Sixty sampled ellipse/transform cases
stay below the requested 1/64-pixel positional error. Circle coverage exposes
a separate cubic-raster approximation envelope, documented with independent
polygon evidence and rejected wrong-geometry controls. The freestanding build
imports only math and depends on `libmath.so`.

No Yonder adapter, computed SVG properties, layout integration or compositor
is delivered here. Shapes, strokes/dashes, instances, nested viewports,
clipping and opacity painting remain in 6b; external images remain in 6c.
DEBTS.md records those gates. There is no new browser/WPT, P5/QEMU or
performance result, and no static-SVG capability is advertised to pages.


### Slice 6b, solid painter as built — 2026-10-26

`svg_scene_render` paints prepared paths/groups into caller-owned straight-alpha
ARGB. It applies solid fills and ordinary user-space strokes, sRGB source-over,
and isolated element/group opacity using 16-bit premultiplied scratch surfaces.
An optional geometry-table bounds tail measures actual expanded outlines for
clipped allocations. The new crop test exposed a half-grid rounding defect;
device ties now use one direction across zero, and the crop matches full output.

[SVG_PAINT.md](SVG_PAINT.md) records ownership, literal limits, conservative
per-call backend quotas, coverage envelopes and reproduction commands. The
ASan/UBSan/LeakSanitizer painting run passes 490,392 assertions, including
50 allocation cuts in both refusal modes and 1,240 cancellation cuts. The
original core/backend suites pass and the four earlier masks are unchanged.
Twenty-four native/Chromium comparisons pass; fourteen wrong controls are
rejected. Independent polygon, rectangle/ring and circle oracles distinguish
edge-coverage differences from compositing errors. The provisional comparison
threshold failures and measured replacement envelopes are retained explicitly.

This increment changes no Yonder files. The tree/style adapter, computed SVG
properties, sizing and browser paint connection remain, along with the remaining
6b features recorded in DEBTS.md. The connection gets its own PR and dependent
follow-ons stack while it is unmerged. There is no new P5/QEMU, performance or
broad WPT/conformance claim.

### Slice 6b, first Yonder connection — 2026-10-26

[SVG_YONDER.md](SVG_YONDER.md) connects real inline paths/groups to the HTML
DOM, garb cascade, replaced layout and window compositor. Presentation
attributes precede author stylesheets; viewBox mapping, currentColor, ordinary
strokes and isolated opacity reach the screen. SVG style elements participate
in stylesheet discovery, and ordinary document mutation rebuilds the artwork.

The first P5 fixture is `/tests/pages/svg-inline.html`. A private windowed QEMU
boot displays its landscape and opacity overlap, including zoomed repaint.
Host integration tests cover pixels, mutation, clipping, unsupported features
and 81 allocation cuts in both refusal modes; full-window tests exercise the
compositor. SVG/backend/output memory shares a 64 MiB Page subdivision, but
whole-page budget unification and caching remain debts. This is not completion
of 6b, external-image support, or a broad conformance/performance claim.

### Slice 6b, basic shapes — 2026-10-26

[SVG_SHAPES.md](SVG_SHAPES.md) extends the connection with rectangles, circles,
ellipses, lines and open/closed point lists. Typed geometry becomes native
paths; CSS geometry and presentation attributes use the existing cascade.
Nineteen shape/path pixel comparisons, allocator-failure sweeps, window mutation
checks and selected Chromium comparisons cover the increment. The new gallery
is `/tests/pages/svg-shapes.html`; its button toggles geometry and color.
This remains part of the static inline milestone, with its broader debts open.

### Slice 6b, same-document instances — 2026-10-26

[SVG_INSTANCES.md](SVG_INSTANCES.md) adds fragment references to paths, shapes,
groups and nested uses, with separate instance ancestry and bounded expansion.
The source keeps its identity; inherited color, paint and geometry come from
the copy's parent. A source edit repaints uses in different SVG roots. The new
`/tests/pages/svg-use.html` gallery demonstrates that with a reversible button.
Twenty-six exact expanded references, 78 expected Chromium outcomes,
allocation-failure sweeps and a real-window mutation regression cover this
increment. Per-instance variable substitution remains an explicit profile limit;
this does not complete static SVG.

### Slice 6b, referenced and nested viewports — 2026-10-27

[SVG_VIEWPORTS.md](SVG_VIEWPORTS.md) adds symbol/SVG instance sizing, nested
viewBox maps, percentage bases and rectangular overflow clips, including
rotated frames and nested opacity. `/tests/pages/svg-viewports.html` has six
cards and a reversible width toggle. Native flattened-reference comparisons,
mask allocation/cancellation sweeps and browser evidence cover this increment.
General clip paths and the remaining static SVG debts stay open.


### Slice 6b, clip silhouettes — 2026-10-27

[SVG_CLIPS.md](SVG_CLIPS.md) adds fragment clip-path resources with one direct
path/shape silhouette, clip-rule holes, transforms, ancestor intersections and
shared source mutation. `/tests/pages/svg-clips.html` moves a circular opening
across two drawings. Geometric unions, resource percentage
coordinates and references inside clip resources remain explicit follow-ups.
This is still inline work; the external SVG image route remains stage 6c.


### Slice 6b, object coordinates — 2026-10-27

[SVG_BOUNDS.md](SVG_BOUNDS.md) adds analytic path/subtree geometric bounds and
numeric objectBoundingBox clips. `/tests/pages/svg-bounds.html` demonstrates a
clip following a drawing's changing width, with independent curve, stroke,
group and transform cases. It records shared-consumer boundaries for gradients
and Canvas 2D through 6c. External SVG images remain the image-route milestone.


### Slice 6c, standalone SVG documents — 2026-10-27

[SVG_DOCUMENTS.md](SVG_DOCUMENTS.md) adds independently owned XML SVG trees,
embedded author sheets, isolated cascades and secure-static image-mode rendering.
Root opacity/transforms/clips belong to the image viewport. Namespace aliases,
resource isolation, allocation/cancellation cuts and native/browser references
are covered. Picture-job dispatch, intrinsic sizing, admission accounting,
gradients and caches remain separate integration work; this increment does not
make external SVG images load through Yonder's `<img>` path.


### Slice 6c, SVG image route — 2026-10-27

[SVG_IMAGES.md](SVG_IMAGES.md) connects file/HTTP(S) SVG documents to tagged
picture jobs, retained page ownership, default object sizing and size-aware
`<img>`/background painting. CSS viewport dimensions stay independent of zoomed
output pixels. Caller-bounded cascade and attribute-parse limits support
admission accounting. `/tests/pages/svg-images.html` exercises source changes,
shared URLs, contain/cover, private styles, static processing and tiled SVG.
Gradients, pixel-variant caches and corpus-led performance work remain in 6c.


### Slice 6c, linear gradient paint — 2026-10-27

[SVG_GRADIENTS.md](SVG_GRADIENTS.md) adds linear fill/stroke paint servers to
inline SVG and independent image documents. A numeric libsvg sampler accepts
resolved stops, maps and geometric bounds without a DOM or CSS dependency, so
a future Canvas 2D adapter can reuse it. The adapter handles local templates,
stop styling, coordinate units and pad/repeat/reflect spread.
`/tests/pages/svg-gradients.html` changes a sky and its shared consumers while
an external SVG postcard retains its private colors. Radial gradients,
raster-variant caching and corpus-led performance work remain in 6c.


### Slice 6c, radial gradient paint — 2026-10-27

[SVG_RADIAL.md](SVG_RADIAL.md) extends fill/stroke servers to centered and
eccentric radial gradients, focal circles, repeat/reflect, outside foci and
cross-type templates. A numeric circle-position query separates geometry from
SVG's tangent-edge coloring for a future Canvas caller.
`/tests/pages/svg-radial.html` moves a highlight across shared consumers while
an independent SVG keeps its own light. Raster caching and corpus-led
latency/fidelity gates remain in 6c.


### Slice 6c, raster-cache foundation — 2026-10-27

[SVG_CACHE.md](SVG_CACHE.md) adds value keys and bounded owned-pixel storage in
libsvg. Keys describe resolved scenes and render extents; full byte comparison
after digest lookup prevents collision reuse. Entry ownership is independent
of documents, scenes and temporary keys. Limits, eviction, failure/cancellation
and exact cached-versus-uncached output are covered by the painter harness.
Yonder adoption and corpus-led measurements remain separate 6c cuts; this
foundation does not change browser painting or require a hardware gallery.

### Slice 6, Part 1 closeout — 2026-10-29

[SVG_PART1.md](../completed/SVG_PART1.md) records the accepted static profile
through 6c and its final reconciliation with current userland. Browser raster
reuse, real-page sizing/metadata fixes, computed SVG style serialization and
partial-artwork diagnostics are implemented. The closeout distinguishes the
P5-accepted feature slices from combined-tree integration checks and retains
the broader graphics, performance and Canvas design work as follow-ons.
