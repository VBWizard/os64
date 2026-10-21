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
`viewBox`, text as the Web face draws it, and the SVG paint properties
libgarb already parses. Oracle: WPT `svg/` (structure and painting; the
reftests when the runner can compare pictures). Acceptance: Google's and
GitHub's icons, suckless's logo. **Fable design** for the renderer's seam
(what libflow hands it, what it hands back, and where it runs), Opus
builds: [SVG.md](SVG.md) (2026-10-21), which makes an `<svg>` a replaced
box to the layout, puts the renderer in a library of its own that fills
and strokes through FreeType's rasterizer and stroker behind one new verb
on the font backend table, and cuts it into 6a (the reftest runner, which
slices 4 and 8 need too), 6b (the renderer's core) and 6c (gradients, text,
the picture path).

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
The tool reads `*.txt` in the directories it is given through the one
reader (below), names every one it cannot judge and answers 2, and imports
nothing outside the standard library.

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
says `kept: whole`, and only such a record is replayed. **Each record
CLAIMS its name as the load begins** (`yonder_diag_claim`): its file is
made only where nothing is, holding `verdict: unwritten` (which every
reader takes as unjudged) until the record replaces it, and no `.html` or
`.d` may be there whether or not this record keeps its page, so two
yonders on one directory, a `.txt` deleted from beside its kept files, or
a keep-off load landing on a kept page's number, never pair a record with
another's pages, and the bytes are never renamed over a `.html` already
there. A record that can claim no name has no claim, and nothing of it is
written; the record's write proves its claimed file is still there first.
No Settings line: there is none for `diagnostics` either.

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
plain lines a record keeps (`yonder_diag_head_fact`). **A RECORD THAT LOST
ANY LINE IS UNJUDGED**, whatever the cause (no room, no memory, a value
with no memory to keep): it says so with one marker, `REPLAY unjudged:
lines lost`, which every reader keys on. An earlier ruling judged a record
that ran out of room, on the grounds that two replays under the same caps
drop the same lines; that is false when the line past the cap is the one a
change made or mended (a P5 record says `failures not kept: 12728`), so it
was reversed the same day.

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
a job still waiting when the replay ends is incomplete. A kept line no job
took is incomplete too (`REPLAY incomplete never asked for <kind> <url>`),
so a script the page stopped loading is never read as a page that stopped
failing. A whole replay is written into an OUT-DIR with no records in it,
and refused otherwise; `--only` may add to one, and says so, and a page it
finds no longer kept replaces its earlier record with an unjudged one;
either is refused when OUT-DIR is KEPT-DIR. Records spell a token injectively: a `%`
that a reader would take for part of a spelling is written `%25`
(YONDER_DIAGNOSTICS.md § Built differently).
**One the index has no line for is not made up** either way: it stays
out, the record gains `REPLAY incomplete <kind> <url>`, a line that is
neither token, the page reads `incomplete`, and the summary is `replayed
P, clean C, incomplete I, unfinished U`. Pictures and fonts are never answered (not
refused: a refusal would put a FAILED line on every page with a picture),
so **a replay does not reproduce what a fetch it never performs
decided**. yonder writes those outcomes under kinds of their own: MISSING
`image` (a fetched picture's format), FAILED `picture-fetch` and FAILED
`font-fetch` (what a fetch, or the bytes it brought, came to). Every
replayed record says `REPLAY not reproduced: font-fetch image
picture-fetch`, a page reads clean or by its counts in the other kinds
only, and the diff compares only the other kinds, on both sides; the
readers refuse any other word on that line, which would hide a kind from
every comparison. A picture or a font decided without a fetch is
reproduced and compared: `MISSING picture-scheme`, `FAILED picture` for an
address refused or never asked for, and `MISSING font` (a family no face
of the page's names; one a face names is served by it, fetched or not). A
script that measures what a picture or a web font sizes measures it
absent.
An idle turn moves the window's clock a tick, until the page is quiet or
60 virtual seconds are spent; the microsecond clock moves 1 ms a read, so a
runaway script stops at the time limit. A navigation the page starts ends
the replay there (`REPLAY incomplete navigation <url>`): the next page is
not in the index, and a page left running would fire timers that never
fired where it was kept. **Only a page replayed to a verdict is judged**:
one that never arrived, asked for the next turn for ever, or had a job
out when its budget ended, gains `REPLAY unfinished <why>` and reads
`unfinished`. UNJUDGED (answer 2, and a record of the page saying so and
nothing else): a `*.txt` that is not a whole record, a record that lost
lines, was never written or was kept not whole, kept bytes that are gone,
a folder whose index is missing, unreadable, of another format, damaged
(a line number that is not a decimal number, which would otherwise become
a path, or a line saying neither `came` nor `failed`) or names a file
that is not there, a harness check that failed while the page ran, and a
record that cannot be written whole. An OUT-DIR or KEPT-DIR that cannot
be listed refuses the run before anything is written. The page is left as
yonder leaves it when it closes, which is the departure write.

**The diff** (`tools/yonder_corpus_diff.py BEFORE AFTER`) judges only
pages replayed to a verdict on both sides, comparing their record lines
with counts left out: a gained line is a regression (exit 1), a lost one
progress. A page whose after-record is unfinished is BROKEN, a regression
when the before-record reached a verdict, never progress; one whose
after-record is NEWLY INCOMPLETE is a gate, exit 1 under its own heading
(the corpus must be recorded again before the change can be judged on
it); one incomplete on both sides is listed to be recorded again; one that
reached a verdict where it had not is recovered, and not compared. A page
the after directory lacks is a regression. A directory it cannot list
answers 2, and so does any `*.txt` the reader cannot judge, each listed as
unjudged with why: a record cut short is never read as a page that
stopped failing. A record that says both `REPLAY unfinished` and `REPLAY
incomplete` is broken, with what it asked for as detail.

**Two invariants, held by structure rather than by each site** (Chris's
ruling, 2026-10-21: three Codex rounds had each found one of them reaching
a site it had not, because each was enforced by hand at every read and
write; a cold read then broke the first structure four ways, and the
second form below is the answer).

- *Only a page replayed to a verdict is judged as the page, and its state
  is in its record, whole or absent.* The runner decides each page's
  `RpState` (verdict, incomplete, unfinished, unjudged, in that order of
  precedence) in `rp_decide` alone, from its own run: whether it reached a
  verdict, whether page and index met one for one, whether a harness check
  failed, whether the window's record lost lines, whether the write
  faulted. It writes the state as the record's second line, `REPLAY state:
  <state> <why>`, before any token line, and ends the record with `end`,
  which both readers require. Every file it touches goes through
  `tools/test_yonder_replay_fs.{h,c}`, its own translation unit, whose
  writes go to a temporary file renamed into place (removed on any fault),
  and whose every call answers done, absent or a fault; the runner poisons
  the host's file functions after including the header, so a call around
  the door does not compile. `tools/yonder_records.py` is the one Python
  reader: `read()` answers a `Record` with the `State` its record says, or
  an `Unjudged` naming why (not UTF-8, a NUL, a byte order mark, a
  carriage return, no address, a verdict or state of no known spelling, a
  second verdict line, a record line with no count, counts that do not
  reconcile, a replay's record without its `end` or with lines past it,
  REPLAY lines that disagree with its state, a REPLAY line of no known
  kind, a stray word on the not-reproduced line). The runner's own reader
  of kept records uses the same grammar. The tally counts VERDICT records
  only, and the diff's headings follow `State`'s order.
- *Nothing is written under a name the record did not claim, and no
  record is paired with another's kept files.* `yonder_diag_write` and the
  keepers take a `yonder_diag_claim_t`, which only a successful exclusive
  `yonder_diag_claim` makes, and the claim requires no `.html` or `.d` by
  its name whether or not the record keeps its page; the claimed file
  reads `verdict: unwritten` until the record replaces it, and the write
  proves the claimed file is still there; the runner replays only a record
  that says `kept: whole`.

The sites they govern: the runner's reads of KEPT-DIR, each record, its
`.html`, its folder, its index and each kept file, its listing and making
of OUT-DIR, its write of each replayed record, and the self-test's own
fixtures; the tally's and the diff's records; and yonder's record file,
page bytes, kept scripts and sheets and index. The poison and the claim's
type govern them, so a site added later is held by them and not by this
list.

**Proof.**

| Check | Result |
|---|---|
| Scripted-page host (`tools/test_yonder_scripts_host.sh`): the keep cases (a windows-1252 page posted in 97-byte chunks with a NUL inside kept byte for byte; the index, its version, each file, a failed script's line and no file; the facts; a page let go unshown; a stream stopped before its verdict leaving nothing; a folder that cannot be made; a name another yonder took, bytes and a folder left from before, each passed over; an empty folder removed; a head fact kept past a full table; an address's own `%` written apart from the token spelling; every one of the claim's 64 names taken, with keep_pages and without, writing nothing of the page and saying so; with keep_pages off, a number beside another's kept bytes or folder passed over, the claimed file reading `verdict: unwritten` until the record replaces it; a record that lost a line saying so; `kept: whole` on a page kept whole; picture and font failures a fetch decided written as `picture-fetch` and `font-fetch`; the setting off keeping nothing) and the script time (a forced layout's 2,500 µs on the line) | 5,973 checks, zero failures |
| The replay's self-test (`tools/test_yonder_replay_host.sh`): the 29 fixture pages of `tools/yonder_loop_fixture`, `yonder_script_fixture` and `pages`, and ten hand-made (a sheet that imports another from its redirected address and is read under its label, a page in windows-1252 from an encrypted fetch, a text body, a file's page and its script and timer, an error page, a failed script with a module and a global, a page too deep to read to its end, a picture by itself, a 1,400-pixel window, scripts off, an address holding `M%49SSING` of its own beside MISSING), kept with keep_pages on, written to real files, replayed | 39 pages; every replayed record is the kept one (the paint stages aside: the processor's counter times them; both were read in the harness, which answers no picture or font either way); the corpus diff between the two finds nothing. Then, each as a corpus of its own: a script's index line taken out reads `unfinished` (the script blocked the parse) with its `incomplete` line and no FAILED line for it; a damaged index, a folder with no index, a record that lost a line (with no memory, or past a full table), and a `*.txt` that is not a record are unjudged (answer 2), each on a record saying so; no time reads `unfinished`; a script asked for twice and kept once is `incomplete` and the page `unfinished`; two async scripts kept in the reverse of the order they were asked for run in the kept order; a whole replay into a directory holding records is refused, and so is one into a directory that can be searched and not listed (mode 0333), writing nothing; a page that navigates stops there; a page kept with a picture whose fetch failed replays clean, its record saying what it does not reproduce, and the diff of the two reads neither progress nor regression. THE COLD READ'S ATTACKS, each a case: a harness check made to fail while a page replays leaves it unjudged on its record; a partial replay into the kept directory is refused; a kept script the page no longer asks for reads `incomplete`; a keep-off record beside another load's page bytes is passed over, and under `--only` an earlier record of it is replaced by an unjudged one; and the whole corpus written under `prlimit --fsize=2048` (SIGXFSZ ignored) answers 2 with every record whole, the one too big for the limit saying it could not be written, and no temporary left. The whole corpus given no time: its 6 unfinished pages are the diff's 6 broken, and it fails. A host file call in the runner around its door does not compile (`#pragma GCC poison`) |
| Hand mutants of the runner and the keepers (not the mutant harnesses) | caught: a sheet read without its label, the head's `encrypted`, `body` and status ignored, sheets left unanswered, the clock not moved, a file page's script refused, the view's width ignored and scripts-off ignored (both missed first, and the two hand-made pages that catch them were written for them), the bytes not kept, the rename skipped, a script not kept, the time not measured, the temporary not removed, the not-whole line not written |
| `tools/test_yonder_records.py`, `tools/test_yonder_tally.py`, `tools/test_yonder_corpus_diff.py` | 9, 13 and 18 tests pass: the reader names every malformation (empty, a byte order mark, carriage returns, a NUL, bytes or escapes that are not UTF-8, no address, cut after it, a verdict of nothing it knows or with a sign, two verdict lines, a line short of or past its verdict, a record line with no count, a REPLAY line on a record yonder wrote, a replay's record cut before its `end` or with lines past it, a state of nothing it knows, REPLAY lines that disagree with the state, a stray word on the not-reproduced line, a directory, a file it cannot read) and reads lost lines and a never-written record as unjudged; the tally counts verdicts only; the diff gates newly incomplete pages and compares pictures and fonts decided without a fetch |
| Guest (QEMU, GUI entry, headless): `yonder` started from `/home/gui.conf`, `/home/kp/kp.html` (a script, a sheet that imports another, a script that 404s) typed into it, `/home` read back with `debugfs` | `kept/file-0001.html` byte-identical to the page, `.d/` with `1.js`, `2.css`, `3.css` and the index's four lines, the failed one included; `e2fsck -fn` clean on `/home`. Replayed on the host: the same MISSING and FAILED lines, plus the timer's death at 300 ms, which the guest's arrival write predates. Not a P5 run |
| The P5's existing records | `replayed 0, clean 0, incomplete 0, unfinished 0; 1601 records are not kept pages`: the replay of those records is impossible, as the brief said; the tally reads all 1,601 as verdicts |
| Anchors of every mutant harness that names a file this slice touched | 1,009 anchors, all found once (one, `sheets-failure-holds-nothing`, kept by writing the sheet's line before its wait is counted down; five moved with the picture-fetch and font-fetch spellings, the same mutations at the new text). The mutant harnesses were not run |

**The corpus count** is zero pages until the P5 browses with
`keep_pages = on`; until then the tally is the number every PR states.

**Deferred**, each a DEBTS.md row (§ The replay corpus): a replay reads
the page in the harness's world (a bitmap face, the default zoom, height
and script time limit); keep_pages writes on the window's thread; a page
that navigates replays only as far as going; a replay does not reproduce
what a picture's or a font's fetch decides; a form's reply replays as a
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
