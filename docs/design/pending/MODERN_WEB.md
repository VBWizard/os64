# MODERN_WEB.md — The Modern Web, which is yonder 2.0

*The campaign doc for the arc after yonder 1.0. Written by Fable on
2026-10-19, the morning Chris and Fable sized the risk, from the 10/07
agreement, the census on the P5, and the engine recon run that morning.
Read with [BROWSER.md](../../../BROWSER.md) (the campaign constitution,
§ Where the campaign stands), [DOM.md](DOM.md) (the binding and the event
loop), [JAVASCRIPT.md](JAVASCRIPT.md) (the engine port), and
[YONDER_HONEY_DO.md](YONDER_HONEY_DO.md) (the 1.0 batch that runs between the
arcs). Fable holds this document and the hard designs it names; Opus builds
the slices; Chris rules.*

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
   directory and its pass rate is the acceptance. A slice cannot regress a
   directory that passed. § The oracle, below.
3. **The corpus, before every merge.** Two instruments, because the
   P5's page files are RECORDS and not pages: each of the 401 under
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
between layouts.

**Slice 6: SVG.** Inline `<svg>` in the tree, and SVG as a picture
(`<img src="x.svg">`, the census's `MISSING image svg`): a renderer for
paths, the basic shapes, fills and strokes, transforms, `use`/`symbol`,
`viewBox`, text as the Web face draws it, and the SVG paint properties
libgarb already parses. Oracle: WPT `svg/` (structure and painting; the
reftests when the runner can compare pictures). Acceptance: Google's and
GitHub's icons, suckless's logo. **Fable design** for the renderer's seam
(what libflow hands it, what it hands back, and where it runs), Opus builds.

**Slice 7: the bytecode cache.** Per bundle, keyed by the cache's own entry
identity (the response's validators), capped in bytes, compressed with
libgzip, invalidated with the response. libjs gains a compile-to-bytes
and a run-from-bytes pair over the engine's own `JS_WriteObject` and
`JS_ReadObject`, tied to the engine's build so a cache written by one
build is never read by another. The timing instrument (compile time per
script, in the page file) lands in slice 0b so this slice's
before-and-after is a number on the P5. Opus.

**Slice 8: generated content and the rest of the census's CSS.**
`::before`/`::after` with `content` (144 asks, the icon fonts' whole
mechanism), `filter` and `clip-path` basics, `mix-blend-mode`, the
`will-change` values yonder draws nothing of (read, no effect). Opus.

**Tier 1 slices**, designed when Tier 0 is done and the P5 has been used
on it for a week: `iframe` and `postMessage` (**Fable design**, its own
document), CSP, referrer and cookie rules across redirects, `SameSite` and
the public-suffix list. Custom elements (`customElements.define`, GitHub's
`react-partial` and friends) join here or the end of Tier 0 as the census
says once modules run.

## How the slices run side by side (2026-10-21)

Chris opened the arc on 2026-10-21 with a week's model usage to spend and
asked for the slices to be built in parallel. The slices were cut
independent on purpose, so the plan is one builder per slice, each in its
own worktree off `userland`, each ending in its own PR. What follows is
what keeps eight builders from colliding in one tree, and it is as much
design as the slices themselves.

**The order.** Slices 0a and 0b first, side by side: they touch different
files. The fan-out (1a, 1b, 2, 3, 4, 7) starts once 0a's table exists,
because every one of them registers in it; a fan-out slice that opens
before 0a has merged branches from 0a's branch and says so in its PR
(DOM_BRIEFS.md's stacking rule). Slices 5, 6 and 8 wait: 5 and 6 are
Fable designs written while the fan-out builds, and 8 shares libgarb with
4.

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
| 7 | the bytes pair in `libjs`, `libway/cache.c` (a bytecode entry beside a script's) | `yonder/script_job.c` |

Landing order, a lean Chris moves: 0a, 0b, 1a, 1b, 4, 2, 3, 7. A later
slice rebases onto what landed before it; `settings.c` and `modern.c`
gain one reader or one row per slice, which rebases clean when each slice
adds rather than edits.

**What every PR states**, so Chris can judge it without re-deriving: its
WPT directory and its count before and after (the expectation file's
diff is the proof); how many of the 401 records named its surface (the
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
whole: a pass it reported prints as a FAIL that says so. Only a page run
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
`attributes`, which the binding lacks.

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
| `os64_dom_storage_t` | a provider per area: `identity` (asked at a task's first touch; a value other than the last loaded means the store moved), `load` (the image, asked again with room, a few times, when it outgrew the room), `commit(changes, last)` (this page's change list only, merged by the host onto the store as it stands; `last` is drain's) |
| the statuses | OK; FULL (past the quota: nothing kept, the page reads the store back); FAILED (the area keeps its map in memory for the page's life); BUSY (another window has the store: the changes are handed over again a second later, or at drain; at drain itself they are lost and the provider says so) |
| commits | coalesced to one a second (`os64_dom_storage_settle`, `_due`, `OS64_DOM_STORAGE_COMMIT_MS`); sooner when the change list would pass the quota or the keys' cap; at `os64_dom_storage_flush`; once more at drain, the last |
| `page_departs()` (yonder.c) | the one place the page on screen departs: where every navigation's page is first parsed (`stream_parser`) and as the window closes. It flushes, so the next page's scripts read what the last wrote; a navigation that never becomes a page is not a departure |
| the store | `<profile>/storage/<origin>`, the origin with every byte outside `[A-Za-z0-9.-]` as `%XX`; read, merged and published by temp and rename under `<store>+lock` (an exclusive create holding `<task> <second>`), broken when its task is gone, a zombie, this window's own, or ten seconds old; a live one waited for 200 ms (2 s at the last commit, which nothing on screen waits for); the store's identity read again before the rename, and a moved store merged onto again. What the second look cannot see is an unlocked writer's same-size image in the same second, or one landing between the look and the rename (DEBTS) |
| the image format | `os64 storage 1\n`, then `+<key bytes> <value bytes>\n` key value `\n` for a set, `-<key bytes>\n` key `\n` for a removal (an image holds sets only); `os64_dom_storage_merge` and `os64_dom_storage_walk` are libdom's, and yonder only moves bytes |
| `options.now_us`, `options.timing` | the host's microsecond clock, and the navigation's origin and milestones (`os64_dom_timing_t`), read as the binding is made and whenever a script asks |
| `options.screen`, `options.zoom` | the display in device pixels, and the zoom in thousandths when asked |
| the plain EventTarget | `d_event_target(prototype, class)` gives a class's objects the three EventTarget methods, `d_event_handler` an `on<type>`, `d_dispatch_plain` a dispatch whose path is the target alone; performance and `screen.orientation` use it |

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

Host: the DOM host 3,810 / 22,416 → 6,602 / 34,924 checks, with 5,160
allocation sweeps; the scripted-page host 5,888 → 6,007; all 0 failed. The
mutant harnesses were not run (the briefs' rule).

**The cold read** found eight, all fixed: performance's private buffer
reachable through an `Array.prototype` setter (P1), the departure commit
too late for the next page (P1), quadratic removals (P2), two yonders
merging onto one image (P2), a lookup stale after a Proxy trap, answered
arrays filled through setters, orphaned temps, and a comment (P3s). **The
design read** passed it with the WTF-8 ruling and the last commit's longer
wait.

**Stacked on this branch**, slice 2 (fetch) moves `d_origin` onto libway's
`way_origin_*` (Fable's ruling 8: the slice that lands second does it).

**Booked** (DEBTS.md § Classic browser scripting): the `storage` event
between windows; PerformanceObserver and the `resource` and `paint`
entries; `DOMException` with `code` and `structuredClone` (raised with
Chris as a small libdom slice); libos64's URL grammar gaps (userinfo, IPv6,
IDNA, `aaa:/b`, an opaque path's trailing space); the legacy multi-byte
decoders; a store's identity at whole-second resolution; sessionStorage's
memory per window; and what the store lock's second look cannot see.
