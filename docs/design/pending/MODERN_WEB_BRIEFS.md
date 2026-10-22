# MODERN_WEB_BRIEFS.md — handing out the Modern Web slices

Status: briefs for the slices of [MODERN_WEB.md](MODERN_WEB.md) that Opus
builds, written 2026-10-21 by Fable the morning Chris opened the arc with
a week's usage to spend and asked for the slices built side by side.
MODERN_WEB.md stays the design and the acceptance; this file says what a
builder who did not write that design needs in order to build one slice
right, with the seams named as they stand in the tree TODAY (every
function and file below was read, not guessed, on 2026-10-21 at
`userland` 44768c2f), and it names what is left to the builder on purpose.
When a slice is built, its `### Slice N, as built` section goes in
MODERN_WEB.md and its brief here is struck.

| Slice | Builder | Reviewer | After |
|---|---|---|---|
| 0a the runner and the switch | Opus | Fable (the table), Chris | **Built** (MODERN_WEB.md § Slice 0a, as built) |
| 0b the tally and the replay corpus | Opus | Chris | **Built** (MODERN_WEB.md § Slice 0b, as built) |
| 1a storage and the small globals | Opus | Chris | 0a's table |
| 1b the window's view, scroll and history | Opus | Chris | 0a's table |
| 2 fetch | Opus | Fable (origins and CORS), Chris | 0a's table |
| 3 ES modules | Opus | Chris; Fable if libjs's contract moves more than the brief says | 0a's table |
| 4 CSS batch A | Opus | Chris | 0a's mark in libgarb |
| 7 the bytecode cache | Opus | Chris | 0a |

| 5a–5d relayout, the observer, the frame | Opus | Fable (5b's rule, 5c's graft), Chris | [RELAYOUT.md](RELAYOUT.md) is the brief; 5a after 0a, 5b/5c in sequence, 5d after 5c |
| 6a–6c SVG | Opus | Fable (6b's door and backend verb), Chris | [SVG.md](SVG.md) is the brief; 6a after 0a, 6b after 0a's mark in libgarb, 6c after 6b |

Slices 5 and 6 are Fable designs (2026-10-21) and their documents are
their briefs: each names its cuts, the seams, the oracle and what is left
to the builder, so no second brief is written here. Slice 8 (generated
content) shares libgarb with 4 and follows it; its brief is written when 4
lands.

## What every slice does the same way

**Read first, in this order.** MODERN_WEB.md whole, § How the slices run
side by side twice. DOM.md § Where script runs, § The event loop and the
as-built sections at its foot (how a page arrives, what a task is, when
the microtask checkpoint runs, the teardown order). LIBDOM.md (the
binding's objects, budgets, errors and proof boundary). DOM_BRIEFS.md
§ What every DOM slice does the same way: its rules on the host harness,
the allocation-failure sweep, cases per contract sentence and the PR
shape apply here unchanged. CLAUDE.md's opening rules (the comment is
part of the code; a review round) and AGENTS.md. SUCCESSION.md § The
verification harness before any QEMU boot.

**The tree you work in.** Each slice has its own worktree under
`.worktrees/` on its own branch off `userland`, named for the slice
(`opus/tmw-0a-runner` and so on). Build and test there; never `cd` to
another worktree, never touch `userland` itself, never `git stash` (the
stash is shared across every worktree and every session), never force
push. Other builders are in the tree at the same time: a file outside
your row of MODERN_WEB.md's ownership table is edited only with a sentence
in the PR saying why.

**Branch and PR.** Commit on your branch, in small commits with the house's
commit messages (what and why, history in the message not the code).
Open the PR against `userland` with `gh pr create`, body in the house's
shape (what it builds, how it was proved, the numbers § What every PR
states asks for, open questions), ending with the attribution line.
Chris tests before anything merges; do not merge. A slice that must build
on an unmerged slice branches from that slice's branch and says so in the
PR body. Do not post `@codex review`; a Codex round is Chris's call.

**The design doc ships with the code.** The same PR adds a
`### Slice N, as built` section to MODERN_WEB.md in the shape of DOM.md's
as-built sections (what was built, the seams as they ended up, the
numbers, what was deferred and where it is booked), moves the slice's row
in the table at the top of this file to **Built** and strikes its brief.
A change to libdom's objects is recorded in LIBDOM.md; a change to libjs's
contract in `userland/libjs/CONTRACT.md`. A debt the slice creates is a
DEBTS.md row in the same PR. A deferral the builder plans is written in
the PR's open questions, not discovered in review.

**Whole API or none** (MODERN_WEB.md rule 1). A name joins `window` only
complete to its standard as the brief scopes it. If a member cannot be
built in this slice, the PR says which and why, and either the whole
interface waits or the gap is one a page cannot feature-detect its way
into (a method that throws a clear `NotSupportedError` is honest; a
method that silently does nothing is not).

**Every global is a row in the table** (slice 0a's `userland/libdom/
modern.c`). A slice adds its file and its row; it never installs a global
anywhere else, so `modern = off` removes all of it at once. A CSS property
this arc adds carries 0a's mark in libgarb's table for the same reason.

**The oracle.** Run `tools/wpt_fetch.sh` (0a's) to get the checkout under
`.vendor/wpt`; add your directories to its list. Run the WPT runner
against your directory before you write a line, and commit that count as
the directory's expectation file under `tools/wpt/` in your FIRST commit,
so the PR's diff of that file is the proof. **Two gates, not one**
(Quinn's finding 9: a regression baseline alone lets an interface ship
passing a tenth of its tests). The expectation file has three sections:
`required`, the tests that MUST pass before the interface's row joins the
table enabled, chosen by the builder from the directory as the ones that
exercise the interface's essential behaviour and named one by one, and
read by Fable or Chris in the PR; `passing`, every test that passes
(the regression baseline: losing one fails the run); and `excluded`,
each test the slice knowingly does not run WITH ITS REASON (`needs
iframe`, `needs a server`, `needs fetch`), so what was never exercised is
named, never a count. A test whose harness status is not OK (the harness
reported an error or timed out) is a failure whatever its subtests say.
A test that passes by accident of a bug is still a pass.

**The proof, in the house's shape.** A host harness under ASan and UBSan
(`tools/test_*_host.sh`, built with `tools/host_build.sh`), cases by hand
one per sentence of the contract, every allocation failed in turn where
the binding allocates (libdom's `d_alloc` is intercepted by the DOM host
for exactly this), and the scripted-page host (`tools/
test_yonder_scripts_host.sh`) green. Host-testable code is tested on the
host first; a QEMU boot is for what needs the wire or the disk. The
mutant harnesses are NOT part of this run unless Chris asks (they take an
hour each and the box is shared); say in the PR that they were not run.

**A tool that is a gate gets its format designed before it is built**
(ruled 2026-10-22, after five Codex rounds and five cold reads of #326
each found the replay corpus saying "clean" about something it had not
seen, the same kind each time). A file another tool judges a change by (a
record, an index, a corpus) has its grammar, its inputs and its versions
written down first, as REPLAY.md is for the replay: one vector file every
reader runs, every input named with its one source, and a hand mutant
per rule that the suite must fail. The same kind of finding twice means
stop and design.

**Three passes before Codex sees it** (ruled 2026-10-21 after #327's
first five Codex rounds found eleven legitimate things, nearly all of them
I/O and error-path edge cases a cold reader asks about and an author does
not). Before the PR opens: (1) YOUR OWN ADVERSARIAL PASS over the diff,
every error branch and every I/O path, asking of each "a short read, a
missing file, a directory where a file should be, a size past the cap,
out of memory right here, the same call twice": add the case or say in
the PR why it cannot happen. (2) A COLD READER: one fresh Opus agent, no
brief and no builder context, given only the PR's diff and the house's
review rules (CLAUDE.md's opening), asked for findings in Codex's shape;
you fix what it finds and say in the PR what it found. One agent, never a
fan-out. (3) FABLE'S READ, architecture and design, plus the files that
carry lifetime, concurrency or ring-boundary risk. Then Chris asks Codex
for round 1, and the rounds run by CLAUDE.md's rules.

**The harness budget.** One QEMU at a time per slice, always under
`timeout`, windowed only if Chris asks, serial to your scratchpad, your
own monitor port (0a 55570, 0b 55571, 1a 55572, 1b 55573, 2 55574,
3 55575, 4 55576, 7 55577), killed by PID when you are done. Poll for
`boot complete` only. The recipe is SUCCESSION.md's; the network boot is
`make run-net` (BROWSER.md § Process). Never read a speed off QEMU.

**What every PR states.** The WPT directory and its count before and
after; how many of the P5's page records name the slice's surface
(0b's tally; before 0b lands, grep the records by hand, they are plain
text); the corpus count before and after once kept pages exist; the P5
reading if the slice claims a speed (you cannot take it; say what Chris
should run).

**Comments say WHY and HOW, now.** No counts of other places, no "the only
caller", no history. Every comment the diff touches or makes false
elsewhere is re-read before the PR opens. `tools/stale_refs.sh` before any
commit that renames or removes a name.

## Slice 0a — built

The brief is retired in favour of [MODERN_WEB.md § Slice 0a, as built](MODERN_WEB.md#slice-0a-as-built),
which records the runner, the expectations' three sections, the table, the
libgarb mark and the yonder switch as they ended up, with the numbers. What
the fan-out needs from it: a global is a row in `userland/libdom/modern.c`
(`{names, install}`, the names being exactly what the installer puts on
window, which `tools/test_dom_modern.inc` holds it to); a CSS property is a
row of `libgarb/props.c`'s table with `modern` set; a WPT directory is a
line in `tools/wpt_fetch.sh`'s list and a `tools/wpt/expected/<dir>.txt`
whose `[required]` names the tests that must pass and whose `[passing]` is
committed before the code (`tools/test_yonder_wpt_host.sh <dir> --update`
writes it; `--modern-off` shows the required tests are the slice's own).

## Slice 0b — built

The brief is retired in favour of REPLAY.md, the contract a kept page
and a replay are held to, and [MODERN_WEB.md § Slice 0b, as built](MODERN_WEB.md#slice-0b-as-built),
which records the census, keep_pages and its index, the inputs a record
carries, the replay runner and the diff as they ended up, with the
numbers. What the fan-out needs from it: a PR's census number is
`python3 -I tools/yonder_tally.py DIR --surface NAME...` over the P5's
records (how many files and sites name any of the slice's names); once
kept pages exist, `tools/test_yonder_replay_host.sh KEPT OUT` replays them
into an empty OUT, and `python3 -I tools/yonder_corpus_diff.py BEFORE
AFTER` compares two replays made the same way: a regression exits 1, and
only a page replayed to a verdict is judged. A slice that adds something
pages fetch adds a `<kind>` line shape to the kept index
(`userland/apps/yonder/diag.h`) and its answer to the runner; a request the
index has no line for reads `REPLAY incomplete`, a corpus to record again.

## Slice 0c: what the corpus said first (document.implementation, javascript: links)

The first census-driven slice: not planned, found. On 2026-10-22, with
the two kept corpora replaying, apgfcu.com's LOGIN did nothing in yonder,
and its record said why: jQuery 3.5.1 dies at load with `TypeError:
cannot read property 'createHTMLDocument' of undefined`, and every script
after it with `jQuery is not defined`. **141 of the ~600 kept pages carry
that exact line**: apgfcu, synchrony, roundpoint, billerpayments and bing
among them, most of Tier 1's banking list, plus kernel.org, php.net and
the itch.io game sites. One missing object keeps every jQuery 3 site's
script world shut, and until it opens the census cannot see what those
pages want NEXT. Build this before any slice whose census number is
smaller, and after whatever is already open.

**Build**, a file in libdom and a row in `modern.c`, plus one yonder
change:

1. **`document.implementation`** (DOM § 4.5.1, the `DOMImplementation`
   interface), a `classic`-table object (it is in every browser that ever
   ran jQuery; nothing about it is modern): `createHTMLDocument(title)`
   returns a NEW document, not a fragment: `<!DOCTYPE html><html><head>
   <title>title</title></head><body></body></html>`, with `head`, `body`,
   `documentElement`, `createElement`, `createTextNode`, `querySelector`
   and `innerHTML` working on it, because jQuery's feature test is
   `createHTMLDocument("").body.innerHTML = "<form></form><form></form>"`
   and then `body.childNodes.length === 2`, and `jQuery.parseHTML` builds
   its nodes in such a document and ADOPTS them into the page (a node
   made in one document inserted into another must work: check `node.c`'s
   owner-document rule and say in the PR what adoption does). The new
   document has NO browsing context: no window, no scripts run in it, no
   fetches from it, `location` absent, and a `<script>` inserted there is
   born started (content.c's rule for innerHTML already). It is the
   page's memory: the same `garb_env_t`/arena rules as a fragment, freed
   with the page. `createDocument(ns, name, doctype)` returns an XML
   document of one root element or none; `createDocumentType` returns a
   doctype node; `hasFeature()` returns true (it is specified to). How
   libdom allocates a document today and whether a second one can share
   the page's arena is the builder's first read (`core.c`, `internal.h`).
2. **`javascript:` link addresses** (HTML § 7.4.2, "navigate to a
   javascript: URL"): a click on a link whose address's scheme is
   `javascript:` runs the rest of the address, percent-decoded, as a
   classic script in the page, and does not navigate. apgfcu's LOGIN is
   `href="javascript:toggleLogin();"`, the code IN the address. The
   standard's second half (a string result replaces the document) is
   DECLINED for the arc (DECLINED.md row, this slice): no page in the
   corpus wants it, and it would need a document replaced from a string.
   Where: yonder's `request_navigate` judges the request through libway,
   which refuses the scheme with "that is a javascript: address, which
   yonder does not fetch"; the click path runs the script BEFORE asking
   libway, through the same door an inline `<script>` runs through
   (`scripts.c`), so it counts on the record as a script (`script
   javascript:#inline-<n>`, or a spelling the builder chooses and states
   in YONDER_DIAGNOSTICS.md), its failure a `FAILED script` line, and the
   refusal line goes away. Form actions and `<iframe src>` with the scheme
   are NOT this slice. With `scripts = off`, the link does nothing and the
   status line says the scripts are off.

**Proof.** The libdom host suite: the jQuery feature test above, verbatim,
passing; a node made in the new document inserted into the page and
read back by the page's `querySelector`; a `<script>` set into the new
document's `innerHTML` never running; `createHTMLDocument()` with no
title (title element present and empty) and with one; `createDocument`
with and without a root; `hasFeature` true. A mutant per rule. The WPT
directory `dom/nodes` has the 282 tests that read `document.implementation`
(MODERN_WEB.md § Slice 0a, as built): the expectation file's `[required]`
grows by the ones this slice makes pass, stated in the PR. The scripted-page
host: a page whose link is `javascript:...`, clicked by the harness's
click, running the script (its side effect read back); one whose script
throws, the FAILED line on the record; one with scripts off, nothing. A
mutant for each.

**Census and corpus.** The PR states `tools/yonder_tally.py --failed`'s
count of `createHTMLDocument' of undefined` before (141 over the two
pulls) and after a replay of the kept corpora: the line must be gone and
the pages' records must say what they want NEXT, which is the slice's
real deliverable. The replay diff against the baseline (REPLAY.md § 7)
must show no regression; lines LOST on those 141 pages are expected and
are the point.

**Left to the builder.** Whether the new document shares the page's arena
or has its own (say which and why; freed with the page either way); the
record's spelling for a `javascript:` script; whether `createElementNS`
rides along (271 WPT tests want it; a separate slice unless it is a
one-line delegation, and say so).

**PR states.** The `[required]` tests gained; the tally line before and
after; apgfcu.com's LOGIN opening its dropdown on the P5, by hand, in the
PR body with what the record says after.

## Slice 1a: storage and the small globals

**Build**, each a file in libdom and a row in `modern.c`:

- **`storage.c`: `localStorage` and `sessionStorage`** (HTML § 12.2,
  the `Storage` interface: `length`, `key(n)`, `getItem`, `setItem`,
  `removeItem`, `clear`, and named property access, which is the exotic
  shape `DOMStringMap` already has in `mixin.c`). Keys and values are
  strings; the binding already replaces NUL and lone surrogates with
  U+FFFD at its boundary, do the same. Quota: 5 MiB per origin counted as
  the sum of key and value lengths in UTF-16 units times two;
  `QuotaExceededError` on the write that would pass it. The origin is the
  page's (`dom->url`, scheme, host, port; an opaque origin, such as
  `about:blank` or a `file:` page, gets a `localStorage` that works and is
  never written to disk, which is what browsers do for file pages). libdom
  holds the map; FILES ARE YONDER'S, and THE FILE IS THE SHARED STATE,
  not any window's copy (Quinn's finding 3: two windows on one origin,
  each flushing its whole snapshot, lose each other's writes; HTML defines
  local storage as one state shared by every window). So the provider is
  per-OPERATION, not per-image: `os64_dom_set_storage` installs
  `fresh(opaque, origin, &identity)` — asked at the first touch in each
  task, answering whether the store moved since the binding last read it
  (yonder answers from one `stat`: the file's size and modification time
  as its identity, which is one syscall per task that touches storage) —
  `load(opaque, origin, out image)` to read it when it has, and
  `commit(opaque, origin, changes)`, where `changes` is the binding's own
  list of sets and removes since its last commit, never its whole map.
  yonder (`userland/apps/yonder/storage.c`) commits by READ, MERGE, WRITE:
  read the file as it stands now, apply this window's changes on top
  (a `clear` is a change that removes every key present at read time, so
  a stale window's later commit cannot resurrect them), write by
  temp-and-rename. What can still be lost is one key written by two
  windows inside one coalescing interval, where the later commit wins,
  which is a browser's last-writer-wins per key. The image format is
  libdom's and yonder only moves bytes, as before. Commits are coalesced
  to at most one a second plus one at the page's departure; the quota is
  checked against the merged size at commit, and a commit that would pass
  it is refused with the keys it could not keep named in the page file.
  The file is `<profile>/storage/<origin-as-filename>` (the origin with
  every byte outside `[A-Za-z0-9.-]` percent-encoded). **This
  slice introduces `profile =`** (YONDER_HONEY_DO.md § Where yonder's
  files live: default `/home/yogi/.yonder`, created with its parents on
  first use, the status line says if it cannot): `yonder_settings_saved_
  profile(out, cap)` beside the diagnostics reader. `sessionStorage` is a
  per-window map by origin that yonder keeps across navigations in that
  window and hands to each page's binding; never a file. A Settings line
  "Clear page storage" empties the folder and the live maps. The `storage`
  event between windows is NOT built (one window cannot see another's
  write today): a DEBTS row, triggered by the first page that waits on it.
- **`performance.c`: `performance`** with `now()` (a double, ms since
  the page's time origin with fractional precision from `now_ms`;
  `options.now_ms` is whole ms today, so add a `now_us` beside it or
  accept ms resolution and say so), `timeOrigin`, `mark(name[, options])`,
  `measure(name, start?, end?)`, `getEntries()`, `getEntriesByName`,
  `getEntriesByType('mark'|'measure'|'navigation')`, `clearMarks`,
  `clearMeasures`, `toJSON`, and the legacy `timing` object (`PerformanceTiming`:
  `navigationStart`, `fetchStart`, `domLoading`, `domInteractive`,
  `domContentLoadedEventStart/End`, `domComplete`, `loadEventStart/End`,
  the rest 0) because the challenge scripts the recon saw read it. The
  page's milestones come from the host through the options (the stream
  knows when parsing ended and when `load` fired).
- **`url.c`: `URL` and `URLSearchParams`** (WHATWG URL) over libos64's
  `os64_url_parse`, `os64_url_absolute`, `os64_url_spell`
  (`os64/url.h`): constructor with base, `href`, `origin`, `protocol`,
  `username`, `password`, `host`, `hostname`, `port`, `pathname`,
  `search`, `searchParams` (live, two-way), `hash`, `toString`, `toJSON`,
  `URL.canParse`; `URLSearchParams` whole (`append`, `delete` with the
  optional value, `get`, `getAll`, `has`, `set`, `sort`, `toString`,
  `size`, `forEach`, `entries`/`keys`/`values`/iterator, constructed from a
  string, a record or a sequence of pairs, application/x-www-form-urlencoded
  serialising per the standard). libos64's parser is not the WHATWG
  parser: run `url/` first and let the expectation file say how far it
  is; a gap that is the parser's (IDNA, a percent-encode set) is a DEBTS
  row against libos64, not a special case in the binding.
- **`encoding.c`: `TextEncoder` and `TextDecoder`**: the encoder is UTF-8
  only by the standard (`encode`, `encodeInto`, `encoding`); the decoder
  takes a label (`utf-8`, `utf8`, `unicode-1-1-utf-8`, `windows-1252` and
  its `latin1`/`iso-8859-1`/`ascii` aliases, which the standard maps to
  windows-1252; libhtml's `encoding.c` has both decoders), `fatal`,
  `ignoreBOM`, and `decode(buffer, {stream})` with the streaming state.
  QuickJS has the typed arrays.
- **`screen.c`: `screen`** (`width`, `height`, `availWidth`, `availHeight`,
  `colorDepth`, `pixelDepth`, `orientation.type`) from values the host
  gives at creation (yonder reads the desktop's size from the GUI; the
  harness gives a constant), and **`devicePixelRatio`** on window: the
  zoom as a ratio (`g.zoom` thousandths over 1000), which is what Chrome
  reports under zoom.
- **`base64.c`: `btoa` and `atob`** (HTML § 8.3; the census puts `btoa`
  fifteenth by files, 161 across 11 sites, and no slice had it): `btoa`
  takes a string whose every code unit is below 256 and answers its base64,
  `InvalidCharacterError` otherwise; `atob` forgives whitespace as the
  forgiving-base64 algorithm says, refuses the rest with the same error,
  and answers a string of the bytes. One file, one row.

**Oracle.** WPT `webstorage/`, `url/` (the `.any.js` ones; `url/
resources/urltestdata.json` is loaded by `fetch` in some tests, which
wait for slice 2, and by `<script>` in others), `encoding/` (the
`textdecoder-*` and `textencoder-*` files; the big legacy-encoding
tables are out of scope and say so), `hr-time/`, `user-timing/`.

**Acceptance.** osdev.wiki's banner and menus (MediaWiki gates its loader
on `localStorage`), the transform demos Chris keeps; danlegt's 105
`localStorage` asks go quiet in its record.

**Proof.** The DOM host (`tools/test_dom_host.sh`) gains an `.inc` per
file with cases per contract sentence and the allocation sweep; the
scripted-page host proves the files (the fake disk is there), the quota,
the coalesced write, the departure write, `sessionStorage` surviving a
navigation in one window, and every name absent with `modern` off.

**Left to the builder.** The image format (any format that round-trips
every string; a length-prefixed record is the obvious one); `now()`'s
resolution; what `screen` says in the harness.

**PR states.** The five WPT directories before and after; the tally for
`localStorage`, `sessionStorage`, `performance`, `URL`, `URLSearchParams`,
`TextEncoder`, `TextDecoder`, `screen`, `devicePixelRatio`.

## Slice 1b: the window's view, the scroll and the history

**Build**, `userland/libdom/view.c` plus the `history` growth in
`window.c`, rows in `modern.c`:

- **`getComputedStyle(element[, pseudo])`**: a read-only
  `CSSStyleDeclaration` of the element's COMPUTED values, answered by a
  provider in the shape of the geometry one (`os64_dom_set_geometry` in
  `dom.h` is the model: owner thread, ensures a current layout, copies a
  snapshot, counts the forced layout against the task's budget). Define
  `os64_dom_set_computed(dom, provider, opaque)` where the provider fills
  a snapshot of every property libgarb knows for the node, as text per
  CSSOM's serialisation (colours `rgb(r, g, b)` / `rgba(...)`, lengths in
  `px`, keywords as written, `none`/`auto` as the computed value says).
  yonder answers from libflow's laid-out box for the node; read
  `libflow/style.c` and `flow.h` for what a box carries, and add the
  accessor libflow lacks with its own host case. A node with no box
  (`display: none`) answers what the cascade computed without layout,
  which libgarb's cascade can give. Pseudo-elements answer an empty
  declaration until slice 8.
- **The scroll and size family on `window`**: `scrollX`/`pageXOffset`,
  `scrollY`/`pageYOffset`, `scrollTo(x, y)` and `scrollTo({top, left,
  behavior})`, `scrollBy` both forms, `scroll` alias, `innerWidth`/
  `innerHeight` (the view in CSS px, unzoomed), `outerWidth`/`outerHeight`
  (the window's frame; `os64_gui_window_get_state` gives yonder its rect),
  `document.scrollingElement` (the root element in standards mode, `body`
  in quirks, as the standard says), and the `scroll` event on `window`
  and `document` dispatched as a task when the view scrolls by any cause.
  The engine already scrolls for fragment links (`yonder.c`, the fragment
  path): the setter goes through that door, clamped as it clamps.
  `behavior: smooth` scrolls at once; say so.
- **`history.pushState(state, unused, url?)`, `replaceState`,
  `history.state`, and `popstate`**: a same-document entry in libway's
  back-and-forward list (read `libway/session.c` for the list; an entry
  gains an optional state value, kept as the structured-clone-lite the
  binding can hold: a JSON serialisation of `state`, `DataCloneError` for
  what JSON cannot carry, which is the honest subset). The URL must be
  same-origin or `SecurityError`. Three routes, three event rules, and
  they are not interchangeable (Quinn's finding 11; routers call
  `pushState` precisely to change the URL WITHOUT a navigation): a STATE
  UPDATE (`pushState`, `replaceState`) changes `location` and `state` and
  fires NOTHING, not `popstate`, not `hashchange`, whatever the fragment
  did; a FRAGMENT NAVIGATION (a link to `#x`, `location.hash = `) adds a
  same-document entry, scrolls, fires `popstate` then `hashchange`; a
  TRAVERSAL (Back, Forward, `history.go`) into a same-document entry does
  not fetch, sets `location`, fires `popstate` with the entry's state,
  then `hashchange` if the fragment differs from the one left, then
  scrolls to the fragment. A fixture proves `pushState` to a different
  fragment fires neither event. `history.length` counts entries.
- **`matchMedia(text)`** → a `MediaQueryList` (`matches`, `media` as
  serialised by libgarb's own dump, `onchange`, `addEventListener('change')`,
  and the legacy `addListener`/`removeListener`), evaluated through
  `garb_media_text_matches(text, env)` (`garb/cascade.h`) with the
  `garb_env_t` yonder builds for the cascade (the view's size, dark pages,
  reduced motion; read how `sheet.c` builds it). Every list the page made
  is re-evaluated when the env changes (resize, zoom, the appearance
  switch), and a list whose answer changed gets a `change` event as a
  task with `matches` and `media` on it. The host tells libdom "the
  environment moved" through one call; libdom does the walk.

**Oracle.** WPT `cssom-view/` (the `scrolling-*`, `window-*`,
`scrollingElement*` files; element scrolling waits), `html/browsers/
history/the-history-interface/`, `css/cssom/getComputedStyle-*` and the
`computed`-named tests under `css/css-logical/parsing/` once slice 4
lands (they read computed values; note them for 4), `css/mediaqueries/`
(`matchMedia` tests).

**Acceptance.** grokipedia's contents links (`scrollTo`, `scrollY`,
`innerHeight`, `replaceState`); danlegt's `matchMedia` and
`getComputedStyle` asks go quiet.

**Proof.** The DOM host for the objects with a fake provider; the
scripted-page host for the real one (a layout answered, a scroll observed
in `g.view`, a resize changing `matches` and firing `change`, Back into a
pushed entry fetching nothing and firing `popstate`, every name absent
with `modern` off).

**Left to the builder.** Which properties the snapshot carries beyond
libgarb's table (none; the table is the list); how `state` is kept
across a reload (it is not; `null`, as the standard allows for a
reloaded entry whose state could not be kept, say so).

**PR states.** The four WPT directories; the tally for `getComputedStyle`,
`scrollTo`, `scrollY`, `innerWidth`, `innerHeight`, `matchMedia`,
`pushState`, `replaceState`.

## Slice 2: fetch

Fable reads the origin and CORS section of this slice's as-built, and
`libway/origin.c`, before it merges: a wrong answer there sends a
person's cookies to a site that must not have them. Write that section
first, as a design, and ask before the code if any sentence of it is
unsure.

**Build.**

- **The job, `userland/apps/yonder/fetch_job.c`**: a pool job in the
  shape of `yonder_script_job_t` (`script_job.h`): request URL, method,
  the request headers as one block, the body bytes and type, the
  credentials mode, the page's serial and a token, the page's
  `way_hooks_t` (cookies and the cache come through it as they do for a
  script); product: status, status text, the final URL, `redirected`,
  every response header (bounded: 64 KiB of headers, more is a failed
  fetch that says so), the body bytes (cap 16 MiB, `max_body`), the
  content type. Runs `os64_fetch_open` with `on_hop` deciding redirects
  by the request's `redirect` mode. Cancellation through
  `os64_work_cancel` as scripts have it. A hook in `yonder_scripts_options_t`
  beside `fetch`/`cancel` carries the request down and the product back
  up (`yonder_scripts_fetched`'s sibling).
- **`libdom/fetch.c`: `fetch(input, init)`** → `Promise<Response>`, with
  `Headers` (whole: construction from a record, pairs or another Headers;
  `append`, `delete`, `get`, `getSetCookie`, `has`, `set`, `forEach`,
  iteration, name validation and lower-casing, the forbidden request
  headers refused silently as the standard says), `Request` (`url`,
  `method` normalised, `headers`, `body` readers, `mode`, `credentials`,
  `redirect`, `signal`, `clone`), `Response` (`status`, `ok`,
  `statusText`, `headers`, `url`, `redirected`, `type` — `basic`, `cors`,
  `opaque`, `opaqueredirect`, `error` — `body` as a minimal
  `ReadableStream` whose reader delivers the whole body as one chunk then
  done, `bodyUsed`, `clone`, `text`, `json`, `arrayBuffer`, `blob` as a
  minimal `Blob` with `size`, `type`, `text()`, `arrayBuffer()`, and
  `Response.error()`, `Response.json()`, `Response.redirect()`),
  `AbortController` and `AbortSignal` (`abort(reason)`, `aborted`,
  `reason`, the `abort` event, `throwIfAborted`, `AbortSignal.abort()`,
  `AbortSignal.timeout(ms)` on the binding's timers; an aborted fetch
  rejects with the reason and cancels the job), `FormData` (`append`,
  `delete`, `get`, `getAll`, `has`, `set`, iteration; the multipart
  encoding libpage's `encode.c`/`submit.c` already write for forms),
  request bodies from a string, `URLSearchParams`, `FormData`, an
  `ArrayBuffer` or view, a `Blob`. Mixed content: an `https` page fetching
  `http` rejects with `TypeError` and writes a page-file fact, the
  question bar is for navigations, not scripts.
- **`libdom/xhr.c`: `XMLHttpRequest`** over the same job: `open` (async
  and SYNC: a synchronous request runs THE SAME JOB to completion on the
  window thread — `yonder_fetch_run` called inline with the same options,
  so the credentials rule, the redirect verdict and the CORS judgement are
  one code path whether the request is asynchronous, synchronous or a
  module's — bounded by the task's budget, with a `sync xhr` fact line
  because it stalls the window; the old web is full of them and refusing
  is a half API. NEVER `way_fetch_whole`: it replaces the caller's hooks
  with the page's cookie handling, follows redirects by its own rule and
  keeps no response headers (`libway/load.c`; Quinn's finding 2), so a
  request through it would be a second, less protected transport),
  `setRequestHeader`, `send`,
  `abort`, `readyState` and `readystatechange`, `status`, `statusText`,
  `responseURL`, `responseType` (`""`, `text`, `json`, `arraybuffer`,
  `blob`, `document` as `NotSupportedError` until the parser can be
  handed a string, say so), `response`, `responseText`,
  `getResponseHeader`, `getAllResponseHeaders`, `timeout` and the
  `timeout` event, `withCredentials`, `overrideMimeType`, the events
  `loadstart`, `progress` (one, at the end, with lengths), `load`,
  `loadend`, `error`, `abort` on the request and `upload` (the upload's
  `loadstart`, `load`, `loadend`), `onload` and the other handler
  properties.
- **Origins and CORS, `libway/origin.c` and `way/origin.h`**, pure and
  host-tested on its own: an origin from a URL (scheme, host, port,
  opaque for `file:`/`about:`), same-origin comparison, the CORS-safelisted
  request headers and methods, whether a request needs a preflight, the
  preflight request's shape (`OPTIONS` with `Access-Control-Request-Method`
  and `-Headers`, no credentials), the preflight response's judgement
  (`Access-Control-Allow-Origin` matching the origin or `*` without
  credentials, `-Allow-Methods`, `-Allow-Headers`, `-Max-Age` cached per
  origin, URL, method and headers for the lifetime it names, capped), the
  response's judgement for the actual request (the same allow-origin rule,
  `Access-Control-Allow-Credentials: true` required under `include`), the
  exposed response headers (the CORS-safelisted response headers plus
  `Access-Control-Expose-Headers`; everything else invisible to the page),
  `no-cors` producing an opaque response for a cross-origin request
  (status 0, no headers, no body), `redirect: manual` producing
  `opaqueredirect`, and the credentials rule across redirects (a redirect
  that leaves the origin drops `include` to the standard's tainting; libfetch
  already keeps `Cookie` and `Authorization` to the typed origin). The
  `Origin` header is sent on every CORS request and every non-GET.
  Credentials mode `same-origin` (the default) sends cookies only to the
  page's own origin.
- **The runner learns two origins**: 0a's runner serves `wpt.test` for
  scripts and sheets; this slice teaches it to answer fetch jobs from the
  checkout too, for `http://wpt.test/...` AND `http://wpt-alt.test/...`
  (the same files on a second origin, so the cross-origin cases without
  a server-side handler run). WPT's CORS tests mostly need `.py` handlers
  the runner cannot run; the CORS oracle is therefore `libway/origin.c`'s
  own host cases, one per sentence of the Fetch standard's § 3.2 (CORS
  protocol) and § 4 (the main fetch's response tainting), plus
  scripted-page cases that assert WHICH requests the page made (the
  preflight present or absent, the `Origin` header, the credentials) for
  canned responses the harness answers.

**Oracle.** WPT `fetch/api/headers/`, `fetch/api/request/`,
`fetch/api/response/`, `fetch/api/abort/` (the `.any.js` files without a
server), `fetch/api/basic/` where the runner can answer, `xhr/` (the
`.any.js` ones and the `.htm` files that need no handler), and the origin
suite above.

**Acceptance.** danlegt's chat (`fetch` on every page there), warpbox,
w3schools' tryit. **A QEMU `make run-net` boot that fetches a real
cross-origin JSON over slirp before the PR opens**, the house's rule for
anything wire-facing (the memory "QEMU navigate before P5").

**Proof.** The origin host (pure, ASan); the DOM host for the objects
with a fake job; the scripted-page host for the whole path with canned
heads and bodies, including the abort, the timeout, a sync XHR, every
`type` of response, and every name absent with `modern` off.

**Left to the builder.** The product's header storage; the `Blob` and
`ReadableStream` minimums beyond what is listed (keep them minimal and
whole: a `ReadableStream` with `getReader`, `read`, `cancel`,
`releaseLock`, `locked` and `tee` refused is a stream a page can use);
whether the preflight cache lives in libway or libdom (libway: it is per
origin, not per page).

**PR states.** The WPT directories; the tally for `fetch`,
`XMLHttpRequest`, `Headers`, `Request`, `Response`, `AbortController`,
`FormData`; the QEMU fetch's address and what came back.

## Slice 3: ES modules

**Build.**

- **libjs's module entry** (`userland/libjs/runtime/runtime.c`, `js.h`,
  `CONTRACT.md`). Today `evaluate` passes `JS_EVAL_TYPE_GLOBAL` and
  nothing else exists. Three facts about the pinned engine shape the
  contract, two of them from Quinn's probe (MODERN_WEB_REVIEW.md,
  findings 1 and 6): `__JS_EvalInternal` RESOLVES a module's imports
  before returning it even under `JS_EVAL_FLAG_COMPILE_ONLY` (quickjs.c
  near line 37166, beside upstream's own comment "could add a flag to
  avoid resolution if necessary"), and the loader it calls is
  synchronous, so compiling an entry whose imports are not fetched yet
  FAILS; a module with a top-level `await` can have an EMPTY job queue
  while its evaluation promise is still pending (it awaits a timer or a
  fetch), so "drained" is not "finished". Add, as one lineage:
  `os64_js_module_compile(runtime, source, length, name, &module,
  outcome)`, compiling with `JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_
  ONLY` and a new flag the patch adds, `JS_EVAL_FLAG_NO_RESOLVE`, under
  which the compiled module is kept UNRESOLVED behind a handle;
  `os64_js_module_requests(module, callback, opaque)`, the static import
  specifiers of that handle (`JS_GetModuleRequests`, the patch's second
  half, over the `req_module_entries` of a `JSModuleDef`);
  `os64_js_set_module_loader(runtime, normalize, load, opaque)` installing
  `JS_SetModuleLoaderFunc` with the host's two callbacks (resolve a
  specifier against the importing module's name, which is its URL; hand
  back a module's SOURCE for a resolved name, or NULL with a reason);
  `os64_js_module_run(module, outcome)`, which resolves (upstream's
  `JS_ResolveModule`) now that every source can be answered, evaluates
  through `JS_EvalFunction` and keeps the evaluation promise under the
  handle; and `os64_js_module_state(module, outcome)` — PENDING,
  FULFILLED or REJECTED with the rejection as the outcome's exception —
  which the host asks after every turn's drain, not only the first,
  because the promise settles whenever the thing it awaits arrives. A
  handle is released by the host (`os64_js_module_release`) or with the
  runtime, and a pending evaluation at teardown is torn down with the
  runtime under TEARDOWN.md's rules (add the case: an unresolved module,
  and a pending evaluation). `import.meta.url` is set to the module's
  name through the normaliser plus the port's `js_module_set_import_meta`
  equivalent (read `quickjs-libc.c` upstream for the shape; the port does
  not link libc, so write the eight lines).
- **yonder's loader, `userland/apps/yonder/modules.c`**, driven from
  `scripts.c` where `OS64_DOM_SCRIPT_MODULE` is recognised and skipped
  today (`scripts.c:579`, `yonder.c:7693`): a module script element
  (inline or `src`) starts a GRAPH: the entry's source (inline text, or a
  script job, `yonder_script_job_t`, which already fetches and decodes;
  modules are always UTF-8), its requests resolved against its URL
  (`os64_dom_resolve`), each fetched as a job unless already in the
  page's module map, recursively, until every source is in hand; then one
  task evaluates the entry through the module entry with the loader
  answering from the map. The map is per page and keyed by URL; a module
  fetched once is evaluated once however many import it (the engine does
  that; the map keeps the source until the runtime is finished with).
  Ordering per HTML § 4.12.1: module scripts are deferred by default
  (run after the parse, in document order, before `DOMContentLoaded`);
  `async` runs when its graph is ready; an inline module is deferred too;
  `nomodule` classic scripts are skipped now that modules run; `type=
  importmap` is recorded as `MISSING script importmap` and not run. A
  fetch failure anywhere in the graph fails the entry with an `error`
  event on the element and a page-file `FAILED` line. The entry's
  evaluation state is asked after every turn's drain (`os64_js_module_
  state`): a module awaiting a fetch or a timer stays PENDING across
  turns, is not reported as done, and is torn down with the page if it
  never settles. **Dynamic `import()`**: the engine calls the loader
  synchronously when the job runs; a module not in the map is fetched
  INLINE on the window thread under the task's budget, and a fact line
  `inline import` goes in the page file because it stalls the window.
  Inline means THE SAME TRANSPORT as every other script request — slice
  2's fetch job run to completion on the window thread, with its
  credentials, redirect and CORS policy — never `way_fetch_whole`, which
  replaces the caller's hooks with ordinary page cookie handling, follows
  redirects by its own rule and keeps no response headers
  (`libway/load.c`, Quinn's finding 2). Until slice 2 lands, the inline
  path is a classic script fetch, as the paragraph below says. That is
  the lean, not a ruling: a DEBTS row books the asynchronous loader
  (prefetching the graph when `import()` is seen is not possible without
  parsing the argument, which may be any expression).
- **`type=module` CSP and CORS**: module scripts are fetched with CORS
  (`crossorigin` honoured; cross-origin module without the allow header
  fails); until slice 2 lands, the job sends the request as a classic
  script's and the as-built says the CORS check is slice 2's to add to
  the job (coordinate: if 2 has landed, add it here).

**Oracle.** WPT `html/semantics/scripting-1/the-script-element/module/`
(the files that need no `fetch` and no `iframe`; the runner serves the
`.js` imports from the checkout). libjs's host suite gains the module
cases (`userland/libjs/tests/`; `VALIDATION.md` records them).

**Acceptance.** GitHub's page runs its scripts at all (its bundles are
all modules); the `MISSING script module` line vanishes from the records
that carry it (tally it).

**Proof.** libjs host: a module evaluates, imports resolve through the
loader, a cycle links, `import.meta.url`, a rejected top-level await
reports, the request list of a source matches its imports, teardown with
unlinked modules under the reclaim policy. Scripted-page host: a graph of
three fetched modules evaluates once in order, `async` and deferred order
against classic scripts per the standard, a failed import, a dynamic
import fetched inline, every module path absent with `modern` off
(modules are then recorded as MISSING as today).

**Left to the builder.** The map's memory cap (charge it to the
binding's `max_bytes`); the patch's shape against the pinned QuickJS
(the no-resolve flag and the request getter are one patch file, with
Quinn's probe from the review as its first test).

**PR states.** The WPT directory before and after; the tally for
`MISSING script module`; GitHub's record before and after on the P5
(Chris runs it).

## Slice 4: CSS batch A, the cheap thousands

**Build**, property by property, each a row in `libgarb/props.c`'s table
WITH 0a's `modern` mark, a grammar in `values.c` per the specification
named at the reader, its inherited flag and initial value, and the
consumer in libflow or yonder's painter:

- **Logical properties** (CSS Logical 1): `inline-size`, `block-size`,
  `min-/max-inline-size`, `min-/max-block-size`, `margin-inline-start/-end`,
  `margin-block-start/-end`, the `margin-inline`/`margin-block`
  shorthands, the same for `padding`, `border-inline-start-width/-style/
  -color` and the block ones with their shorthands, `inset-inline-start/
  -end`, `inset-block-start/-end`, `inset-inline`, `inset-block`, `inset`.
  Mapping: for `horizontal-tb` and `ltr` (which is every page in the
  census) inline-start is left, inline-end right, block-start top,
  block-end bottom; for `direction: rtl` inline flips (libos64 has bidi;
  check whether libflow carries `direction` per box; if it does, map by
  it, if not, map ltr and book rtl). Order: the standard says a logical
  and its physical twin are distinct properties whose later declaration
  wins at computed-value time; implement it as the cascade applying each
  in declaration order to the physical longhand, which gives exactly that
  for one writing mode.
- **`cursor`** (CSS UI 4): the keyword list whole, and `url()` values
  parsed and ignored with a fallback keyword; stored on the computed
  style; yonder's hit test reads it for the pointer shape THE DAY THE
  KERNEL HAS SHAPES (YONDER_HONEY_DO.md § 7; nothing is drawn yet, and
  the as-built says so). Parsing it is what makes 509 `MISSING` asks
  honest: the page said it, yonder knows it.
- **`aspect-ratio`** (CSS Sizing 4): `auto`, `<ratio>`, `auto <ratio>`;
  libflow's sizing: when exactly one of the box's width and height is
  definite, the other follows the ratio (box-sizing per the spec), for
  replaced and non-replaced boxes; `min-height: auto` interplay as the
  spec says for the common case.
- **`object-fit`** and **`object-position`** (CSS Images 3): the
  replaced content's placement inside its box for pictures (`fill`,
  `contain`, `cover`, `none`, `scale-down`; a position pair); the
  picture's paint (`paint.c`, `picture.c`) takes the computed rectangle
  and clips to the box.
- **`letter-spacing`** (CSS Text 3): a length added after every glyph but
  the last of a run; find where libflow shapes text (`libos64`'s text
  layout, `text.c`, `text_draw.c`) and whether a per-glyph advance delta
  exists; if not, add the parameter through the text API with a host
  case. `normal` is 0.
- **`text-overflow: clip | ellipsis`** (CSS Overflow 4): on a block with
  `overflow` other than `visible` and a line that overflows, the painter
  clips the line and draws `…` (U+2026 if the face has it, else three
  periods) at the end, replacing as many glyphs as it needs.
- **`word-break`** (`normal`, `break-all`, `keep-all`) and
  **`overflow-wrap`/`word-wrap`** (`normal`, `anywhere`, `break-word`)
  (CSS Text 3): libflow's line breaker gains the two rules (break
  anywhere; break inside a word only when it would overflow otherwise,
  and for `anywhere` count the soft wrap in min-content).
- **`outline`** (CSS UI 4): `outline-width`, `-style`, `-color`,
  `-offset` and the shorthand; painted OUTSIDE the border edge, offset by
  `-offset`, after the box's own paint, taking no layout space; `auto`
  draws as `solid`. The default sheet's focus ring, if it uses something
  else today, moves to `outline` (read `libgarb`'s default sheet).
- **`user-select`** (CSS UI 4; 627 files across 78 sites in the census,
  eighth among properties, and no slice had it): parsed and kept on the
  computed style (`auto`, `text`, `none`, `all`, `contain`) for the day
  text selection exists (YONDER_HONEY_DO.md § 7 reads it then); nothing is
  drawn differently now, as with `cursor`, and the as-built says so.

**Oracle.** The testharness tests under WPT `css/css-logical/parsing/`,
`css/css-sizing/parsing/` (the `aspect-ratio-*` files), `css/css-images/
parsing/` (`object-fit-*`, `object-position-*`), `css/css-text/parsing/`
(`letter-spacing-*`, `word-break-*`, `overflow-wrap-*`), `css/css-ui/
parsing/` (`cursor-*`, `outline-*`), `css/css-overflow/parsing/`
(`text-overflow-*`). The `-valid`/`-invalid` tests read `element.style`
and run today; the `-computed` tests read `getComputedStyle` and run once
slice 1b has landed (if it has, run them; if not, say which wait). The
reftests under those directories are not runnable yet (MODERN_WEB.md §
The oracle); the LAYOUT proof is libflow's own host suite
(`tools/test_libflow_*`, the `flowdump` fixtures) with a case per rule
above, and a yonder host case per painted thing (an outline's rectangle,
an ellipsis, a `cover` picture's clip).

**Acceptance.** The tally re-run: `letter-spacing` (44 asks on danlegt
alone), `object-fit` (53), `outline` (17), `cursor` (34), `word-break`,
`overflow-wrap` leave the records; the P5 beside Chrome on danlegt's shop
(the `aspect-ratio` cards) and on a page with a long nowrap line.

**Left to the builder.** The order the properties land in (one commit
each, parse first, consumer second, is the house's shape); whether
`letter-spacing` needs the text cache keyed on it (it does if the cache
keys on the run's text and face alone: read `text_cache.c`).

**PR states.** The WPT parsing directories; the tally for the whole list.

## Slice 7: the bytecode cache

**Build.**

- **libjs's bytes pair** (`runtime.c`, `js.h`, `CONTRACT.md`):
  `os64_js_compile(runtime, source, length, name, &bytes, &count,
  outcome)` — `JS_Eval` with `JS_EVAL_FLAG_COMPILE_ONLY` then
  `JS_WriteObject` with `JS_WRITE_OBJ_BYTECODE`, the blob prefixed by a
  BUILD TAG (a hash over the pinned upstream version string, the port's
  ABI id and the limits that shape bytecode, computed once at startup) and
  a SHA-256 of the SOURCE (libtls has it; check the link), the payload's
  LENGTH, and a SHA-256 of the PAYLOAD ITSELF — the source digest says
  which source this bytecode is for, the payload digest says the bytes
  are the ones written (Quinn's finding 5: a bit flip in the bytecode
  leaves the source digest true); and `os64_js_run_bytes(runtime, bytes,
  count, name, outcome)` which checks the framing (magic, tag, lengths
  within `count`), the tag, the source digest against the one the caller
  keeps with the cache entry, and the payload digest against the payload,
  and refuses on any mismatch WITHOUT handing a byte to the engine; then
  `JS_ReadObject` with `JS_READ_OBJ_BYTECODE` and `JS_EvalFunction` under
  the same turn, budget and outcome rules as `os64_js_run`. A refusal is a
  cache MISS to the caller (compile fresh, overwrite the entry), never an
  error the page sees. QuickJS's reader TRUSTS its input (its manual says
  so): the digests are an integrity check, not authentication; the cache
  directory is trusted as the binary is, and a tampered blob rewritten
  with matching digests is the same threat as a tampered binary. Say so
  in the contract and in DEBTS as a known limit of a cache under
  `/var/cache`.
- **The cache entry** (`libway/cache.c`, CACHE.md): beside a script
  response's entry, a sibling keyed by the entry's identity plus the
  suffix the cache's naming allows, holding the compressed blob
  (`libgzip`; if the tree's gzip only inflates, store it raw and say so)
  and the source's digest; written by a pool job after the window's
  first compile (the window compiles, hands the bytes to a job to
  compress and store, and never waits); invalidated with the response
  (the entry's own lifetime rules; a revalidated 304 keeps it, a new body
  drops it). Capped: an entry larger than the cache's per-entry limit is
  not stored, and the blob counts against `cache_mb`.
- **The script job** (`script_job.c`): when the response comes from the
  cache and a sibling blob exists, the product carries the blob and the
  digest instead of, or beside, the source (the source is still needed
  for `FAILED` lines' names and for a digest mismatch); `scripts.c` runs
  bytes when it has them and source otherwise, and the page file's
  per-script time line (0b's instrument) gains `cached` on a bytes run.
- **The number.** Before and after on the host (the scripted-page host
  with a 2 MB bundle fixture) and on the P5: YouTube's bundle (which dies
  on the DOM, but its compile is the cost: 4 to 5 s today), Google's,
  danlegt's. Chris runs the P5; the PR says what to open and what line to
  read.

**Oracle.** libjs's host suite: round trip gives the same result as a
source run (a fixture that computes and prints), a wrong tag refused
without reading, a wrong source digest refused, a truncated blob and a
bit-flipped blob each refused by the framing or the payload digest
without reaching the engine (every byte position flipped in turn on a
small blob, under ASan: the reader must never be entered), and the
reader's behaviour on a corrupt payload whose digests were REWRITTEN to
match, documented as what a trusted-directory compromise costs; the cache
host suite for the sibling entry's lifetime.

**Acceptance.** The P5's second visit to YouTube's front page: the
compile seconds gone from the page file's line; danlegt's scripts show
`cached`.

**Left to the builder.** The blob's framing bytes; whether the sibling
is one cache entry or a field of the response's (one entry: the cache's
rules already cover one); compression.

**PR states.** The host before-and-after; the P5 lines Chris should read.
