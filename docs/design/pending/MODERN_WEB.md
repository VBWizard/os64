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
| `Page.loaded`, `g.replacing` | a page's navigation by address (`location.replace`, `location.href`) or a form its script sends replaces the page's own entry until the page has completely loaded, which in yonder is when its `load` task, and the navigation that task asked for, is over (`page_navigation_replaces`; HTML § 7.2.4, Location-object navigate, step 3, and § 4.10.22.3 Form submission algorithm, step 25). `replace()` replaces whenever it runs. No transient activation is weighed: a page is shown and its `load` runs in one turn, so a person's input has no task in that window, and a timer or a click after `load` pushes. For the page on screen (its `load` running) the entry replaced is the one on screen. A page not yet shown (its parse, its DOMContentLoaded, its wait for sheets) is the session's entry already, so the new page takes its place (`start_trip`): it comes as the replaced page came (a reload's as a refresh), at the entry a traversal was fetched for, with the entries its scripts made around it, and a traversal goes on as its own. A redirector reached by a link leaves the page before it behind; one reached by Back is replaced, so Back past it is the entry before it. A page not yet shown whose navigation waits on the question bar is asked as the page on screen's, and its place is not taken (DEBTS.md) |
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
history interface 52 → 54, each written into `[passing]`. Every one of the 22 expectation
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
list, one each) with their step bounds, 430 checks; libfetch's host 905
checks; the job's host (`test_yonder_fetch_job_host.sh`, 312 checks,
every allocation failed in turn and each one failed alone); the DOM
host 82,990 checks with 12,734 binding allocation sweeps (Slice 0d's
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
(XMLHttpRequest.send(document)).

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

Host, the base → the head: the DOM host 11,667 / 82,990 checks → 12,170 /
85,281 (allocation sweeps 1,029 / 12,734 → 1,080 / 13,081),
`tools/test_dom_window.inc` holding a case per rule; libway's origin, way
and way-fetch hosts, libfetch's host and its transport's, yonder's host,
its stream and fetch-job hosts, the webfont, os64get and html-dom hosts,
the replay proof, the WPT runner's self-test, the TLS input and engine
suites and both TLS audits, all 0 failed; `make -C userland` builds.

**The scope guard stopped one thing.** The scripted-page host fails at the
head, and only in `loop_view_refetch_short`, which fails every allocation
of a page's arrival in turn, its scripts' compiles included. At cut 441 the
engine's `add_closure_var` fails inside `resolve_scope_var`, and the
bytecode emitted anyway sends `resolve_variables` reading garbage: an ASan
SEGV in `find_private_class_field_all`. That is the third of the four
unchecked parser paths DEBTS books for libjs patch 0011 (the failed
closure-variable add, there recorded as reaching `stack underflow`).
P1-W's new atoms and objects moved the engine's arena boundaries so that a
cut now lands on it; the base passes (7,512 checks). It needs the engine
change patch 0011 makes, which the brief's scope guard keeps out of this
branch; nothing here works around it. With that one case skipped by
hand, the host is 6,701 checks and 0 failed, the end-to-end cases
(`tools/test_yonder_window.inc`) among them.

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
interface; `getRandomValues` filling a view of a resizable buffer, which
WebIDL refuses. Not booked, by ruling: `subtle` (ruling 1), and `close()`
(DIVERGENCES.md).
