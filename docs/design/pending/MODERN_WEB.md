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
   P5's page files are RECORDS and not pages: each of those under
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

### Slice 0b, as built

Built by Opus on `opus/tmw-0b-corpus`, from `fable/modern-web` at
`a5152d17` (`userland` 44768c2f and the arc's two docs commits), on
2026-10-21, with Quinn's finding 8 (MODERN_WEB_REVIEW.md) taken the same
afternoon. The brief is MODERN_WEB_BRIEFS.md § Slice 0b.

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
The tool reads `*.txt` in the directories it is given, passes over what is
not a page file, and imports nothing outside the standard library.

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
leaves `kept: not whole` on the record. **Each record CLAIMS its name as
the load begins** (`yonder_diag_claim`): its file is made only where
nothing is, with keep_pages its folder must be made fresh and no `.html`
may be there, and a number that fails is passed over, so two yonders on
one directory, or a `.txt` deleted from beside its kept files, never pair a
record with another's pages, and the bytes are never renamed over a
`.html` already there. No Settings line: there is none for `diagnostics`
either.

**The index grows** (Quinn's finding 8): its first line names the format,
`# yonder kept index 1`, and every line is `<n>`, `<kind>`, then what that
kind carries, so slice 2's `fetch` lines and slice 1a's `storage` image
are new shapes beside the script and sheet ones, the version moves only
when an existing shape changes, and the runner passes over kinds it does
not know.

**The facts** a replay rebuilds the head from are on every record now:
`final address` (written when the parse ends, so a page let go unshown has
one), `body` (libway's judgement, rather than the replay judging the
content type again), `content-type`, `charset` and `encrypted` for a page
from the network, and `scripts: off` when the page was read without them.
They, `status`, `TLS`, `laid out at` and `kept` have room past the 512
plain lines a record keeps (`yonder_diag_head_fact`). So a record that
ran out of ROOM for its other plain lines (9 of the P5's 1,601 say `facts
not kept`: acid3, Yahoo, jwz.org) still carries everything a replay needs,
and a replay is compared with another made under the same caps, which
drop the same lines: it is judged (ruled 2026-10-21). Only a record with a
line that had no MEMORY to be written (`no memory to write this line`),
which may be missing anything, is refused as unjudged.

**The instrument** (slice 7's before-and-after): a script's line is
`script <address>: ran in <n> us`, the time `os64_js_run` took, compiling
included (`yonder_scripts_task_us`). On the host the clock moves only when
a case moves it, so the line is exact in the cases; in QEMU it is a number
and not a speed.

**The replay runner** (`tools/test_yonder_replay_host.{c,sh} KEPT-DIR
OUT-DIR`; its comment is the full contract). 0a had not committed when
this was built, so it took the brief's include route: the runner includes
`tools/test_yonder_scripts_host.c` with `YONDER_HOST_FAKES_ONLY`, which
leaves that file's `main` out; two hooks there let it run the pool itself
(the scripted host's fake keeps 32 jobs of each kind and parks
pictures in the navigation's slot, which a real page overruns), and the
file fake serves a length. The build list moved to
`tools/yonder_host_build.sh`, which both suites source, so they link the
same thing; 0a can converge on it. A page is read in a window as wide as
the record says it was laid out, with a text context of its own; its jobs
are answered at the top of each turn in submission order, a script or a
sheet from the index by its exact address as it came or as it failed,
each line answering one job (a job past the lines kept for its address is
incomplete, never a second run of the first), and in the index's order,
which is the order the products reached the window where the page was
kept: a job whose line comes later waits for it, so two async scripts run
in the order they ran there whatever order the replay asked for them, and
a job still waiting when the replay ends is incomplete. A whole replay is written
into an OUT-DIR with no records in it, and refused otherwise; `--only`
may add to one, and says so. Records spell a token injectively: a `%`
that a reader would take for part of a spelling is written `%25`
(YONDER_DIAGNOSTICS.md § Built differently).
**One the index has no line for is not made up** either way: it stays
out, the record gains `REPLAY incomplete <kind> <url>`, a line that is
neither token, the page reads `incomplete`, and the summary is `replayed
P, clean C, incomplete I, unfinished U`. Pictures and fonts are never answered (not
refused: a refusal would put a FAILED line on every page with a picture).
An idle turn moves the window's clock a tick, until the page is quiet or
60 virtual seconds are spent; the microsecond clock moves 1 ms a read, so a
runaway script stops at the time limit. A navigation the page starts ends
the replay there (`REPLAY incomplete navigation <url>`): the next page is
not in the index, and a page left running would fire timers that never
fired where it was kept. **Only a page replayed to a verdict is judged**:
one that never arrived, or asked for the next turn for ever, gains `REPLAY
unfinished <why>` and reads `unfinished`. A `*.txt` that is not a record, a
record with a line that had no memory to be written, and an index the
runner cannot read (its format, or
a line number that is not a decimal number, which would otherwise become
a path) are unjudged and make the answer 2. The page is left as yonder
leaves it when it closes, which is the departure write.

**The diff** (`tools/yonder_corpus_diff.py BEFORE AFTER`) judges only
pages replayed to a verdict on both sides, comparing their record lines
with counts left out: a gained line is a regression (exit 1), a lost one
progress. A page whose after-record is unfinished is BROKEN, a regression
when the before-record reached a verdict, never progress; one whose
after-record is incomplete is listed to be recorded again, as newly
incomplete when the before-record was not, and is neither; one that
reached a verdict where it had not is recovered, and not compared. A page
the after directory lacks is a regression. A directory it cannot list or
a page file it cannot read answers 2, and so does a page file that is not
whole: the tally's reader requires the second line to be a verdict and the
record lines to be exactly the ones it counts, so a record cut short is
listed as unjudged rather than read as a page that stopped failing; the
tally answers 2 for one too.

**Proof.**

| Check | Result |
|---|---|
| Scripted-page host (`tools/test_yonder_scripts_host.sh`): the keep cases (a windows-1252 page posted in 97-byte chunks with a NUL inside kept byte for byte; the index, its version, each file, a failed script's line and no file; the facts; a page let go unshown; a stream stopped before its verdict leaving nothing; a folder that cannot be made; a name another yonder took, bytes and a folder left from before, each passed over; an empty folder removed; a head fact kept past a full table; an address's own `%` written apart from the token spelling; the setting off keeping nothing) and the script time (a forced layout's 2,500 µs on the line) | 5,904 checks, zero failures (5,842 before the slice) |
| The replay's self-test (`tools/test_yonder_replay_host.sh`): the 29 fixture pages of `tools/yonder_loop_fixture`, `yonder_script_fixture` and `pages`, and ten hand-made (a sheet that imports another from its redirected address and is read under its label, a page in windows-1252 from an encrypted fetch, a text body, a file's page and its script and timer, an error page, a failed script with a module and a global, a page too deep to read to its end, a picture by itself, a 1,400-pixel window, scripts off, an address holding `M%49SSING` of its own beside MISSING), kept with keep_pages on, written to real files, replayed | 39 pages; every replayed record is the kept one line for line (the paint stages aside: the processor's counter times them); the corpus diff between the two finds nothing. Then, each as a corpus of its own: a script's index line taken out reads `incomplete` with no FAILED line for it; a damaged index, a record with a line that had no memory, and a `*.txt` that is not a record are unjudged (answer 2), while one whose table was full is judged; no time reads `unfinished`; a script asked for twice and kept once reads `incomplete`; two async scripts kept in the reverse of the order they were asked for run in the kept order; a whole replay into a directory holding records is refused; a page that navigates stops there. The whole corpus given no time: its 5 unfinished pages are the diff's 5 broken, and it fails |
| Hand mutants of the runner and the keepers (not the mutant harnesses) | caught: a sheet read without its label, the head's `encrypted`, `body` and status ignored, sheets left unanswered, the clock not moved, a file page's script refused, the view's width ignored and scripts-off ignored (both missed first, and the two hand-made pages that catch them were written for them), the bytes not kept, the rename skipped, a script not kept, the time not measured, the temporary not removed, the not-whole line not written |
| `tools/test_yonder_tally.py`, `tools/test_yonder_corpus_diff.py` | 11 and 12 tests pass |
| Guest (QEMU, GUI entry, headless): `yonder` started from `/home/gui.conf`, `/home/kp/kp.html` (a script, a sheet that imports another, a script that 404s) typed into it, `/home` read back with `debugfs` | `kept/file-0001.html` byte-identical to the page, `.d/` with `1.js`, `2.css`, `3.css` and the index's four lines, the failed one included; `e2fsck -fn` clean on `/home`. Replayed on the host: the same MISSING and FAILED lines, plus the timer's death at 300 ms, which the guest's arrival write predates. Not a P5 run |
| The P5's existing records | `replayed 0, clean 0, incomplete 0, unfinished 0; 1601 records carry no kept bytes`: the replay of those records is impossible, as the brief said |
| Anchors of every mutant harness that names a file this slice touched | 334 anchors, all found once (one, `sheets-failure-holds-nothing`, kept by writing the sheet's line before its wait is counted down). The mutant harnesses were not run |

**The corpus count** is zero pages until the P5 browses with
`keep_pages = on`; until then the tally is the number every PR states.

**Deferred**, each a DEBTS.md row (§ The replay corpus): a replay reads
the page in the harness's world (a bitmap face, the default zoom, height
and script time limit); keep_pages writes on the window's thread; a page
that navigates replays only as far as going; a form's reply replays as a
GET.

**Cold read** (MODERN_WEB_BRIEFS.md § Three passes before Codex): a fresh
Opus read the whole diff with no brief and found nine things, all taken
and all above: pages that never arrived were read by the diff as progress
(the verdict rule, `REPLAY unfinished`, BROKEN); a page's own navigation
let it run on (stopped, incomplete); the diff's two incomplete directions
(newly incomplete, recovered); a tally path not there counted as nothing
(exit 2); the diff crashed on an unreadable page file (exit 2); a sequence
counted once could pair records with another's kept files (the claim); the
facts a replay needs could be lost past 512 plain lines (their own room,
and refusal only of a record that ran out of memory); an index number used as a path unchecked, and a non-record
`*.txt` skipped silently (unjudged); and two texts that were not the whole
truth (`diag_kept`'s comment, YONDER_DIAGNOSTICS.md's sample).
