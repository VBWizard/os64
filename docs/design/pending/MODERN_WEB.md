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
text out in the harness's bitmap face; `screen`, once built, will answer
the harness's size; a kept corpus holds the cookies a page's scripts
could read; keep_pages writes on the window's thread; a page that
navigates replays only as far as going; a replay does not reproduce what
a picture's or a font's fetch decides; a form's reply replays as a GET.

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
  where the fonts have no glyph for it). Where a line ends in it is ONE
  rule in libflow with two doors (`flow_line_cut`, beside the box tree it
  reads): what must end by the limit — the end edge less the ellipsis — is
  kept, the ellipsis starts where that ends, and nothing at or past it is
  drawn by the painter or hit by `flow_hit`, which answers the block there
  (the ellipsis stands in no inline box, as in Chrome). It lives in
  libflow and not the painter because hit testing is libflow's and the
  painter can call it unchanged; a rule written twice is two rules. A text
  across the limit keeps the characters that end by it
  (`flow_text_kept`), an inline box's piece ends where the kept content
  does, an atom at or past the cut is left out whole with everything in
  it. Spaces that hang at a pre-wrap line's end do not count as content
  passing the end. A line with no room before the ellipsis keeps its first
  character or atom, clipped rather than ellipsed, and the ellipsis
  follows it, clipped by the box with it. The end is where the block's
  scroll shows it, so content scrolled into view is drawn and a line
  scrolled to its end has no ellipsis. libflow's walk hands the painter
  the block's LINE boxes after each line's pieces, and the painter draws
  the ellipsis then, filled as the block's text is (`text_fill`); it works
  a line's cut once a paint (`LineCuts`), and `flow_hit` once a hit, never
  once a piece.
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
  picture not yet measured takes its other side from the page's ratio. A
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
| `flow_line_cut`, `flow_text_kept` (libflow) | where a line ends in its ellipsis: the one rule the painter and `flow_hit` both read |
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

Host: libflow 21,382 → 21,682 checks, yonder 1,716 → 1,869, the DOM host
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
