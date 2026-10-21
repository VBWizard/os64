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
| 0a the runner and the switch | Opus | Fable (the table), Chris | `userland` |
| 0b the tally and the replay corpus | Opus, **Built** | Chris | `userland`, beside 0a |
| 1a storage and the small globals | Opus | Chris | 0a's table |
| 1b the window's view, scroll and history | Opus | Chris | 0a's table |
| 2 fetch | Opus | Fable (origins and CORS), Chris | 0a's table |
| 3 ES modules | Opus | Chris; Fable if libjs's contract moves more than the brief says | 0a's table |
| 4 CSS batch A | Opus | Chris | 0a's mark in libgarb |
| 7 the bytecode cache | Opus | Chris | 0a |

Slices 5 (incremental relayout, then the frame), 6 (SVG) and 8 (generated
content) are not briefed here: 5 and 6 are Fable designs, written while
the fan-out builds; 8 shares libgarb with 4 and follows it.

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
so the PR's diff of that file is the proof. A test that passes by accident
of a bug is still a pass; a test that fails for a reason outside the slice
(needs `iframe`, needs `fetch`, needs a server) is listed in the as-built
section by the reason, in a count, not by name.

**The proof, in the house's shape.** A host harness under ASan and UBSan
(`tools/test_*_host.sh`, built with `tools/host_build.sh`), cases by hand
one per sentence of the contract, every allocation failed in turn where
the binding allocates (libdom's `d_alloc` is intercepted by the DOM host
for exactly this), and the scripted-page host (`tools/
test_yonder_scripts_host.sh`) green. Host-testable code is tested on the
host first; a QEMU boot is for what needs the wire or the disk. The
mutant harnesses are NOT part of this run unless Chris asks (they take an
hour each and the box is shared); say in the PR that they were not run.

**The harness budget.** One QEMU at a time per slice, always under
`timeout`, windowed only if Chris asks, serial to your scratchpad, your
own monitor port (0a 55570, 0b 55571, 1a 55572, 1b 55573, 2 55574,
3 55575, 4 55576, 7 55577), killed by PID when you are done. Poll for
`boot complete` only. The recipe is SUCCESSION.md's; the network boot is
`make run-net` (BROWSER.md § Process). Never read a speed off QEMU.

**What every PR states.** The WPT directory and its count before and
after; how many of the P5's page records name the slice's surface
(0b's tally, `tools/yonder_tally.py DIR --surface NAME...`; before 0b
lands, grep the records by hand, they are plain text); the corpus count before and after once kept pages exist; the P5
reading if the slice claims a speed (you cannot take it; say what Chris
should run).

**Comments say WHY and HOW, now.** No counts of other places, no "the only
caller", no history. Every comment the diff touches or makes false
elsewhere is re-read before the PR opens. `tools/stale_refs.sh` before any
commit that renames or removes a name.

## Slice 0a: the runner and the switch

**Build.** Four things, one PR.

1. **`tools/wpt_fetch.sh`**: a sparse, shallow checkout of
   web-platform-tests at ONE pinned commit (a variable at the top of the
   script; today's `main` is fine, written as a full SHA) into
   `.vendor/wpt` (already in `.gitignore`), of the directories a list in
   the script names: `resources`, `common`, `dom/nodes`, and the
   directories the other briefs name (add them now so the fan-out finds
   them: `webstorage`, `url`, `encoding`, `hr-time`, `user-timing`,
   `cssom-view`, `html/browsers/history`, `css/cssom`, `css/mediaqueries`,
   `fetch/api`, `xhr`, `html/semantics/scripting-1/the-script-element`,
   `css/css-logical`, `css/css-sizing`, `css/css-images`, `css/css-text`,
   `css/css-ui`, `css/css-overflow`). `git init`, `remote add`,
   `fetch --depth 1 origin <sha>`, `sparse-checkout set <dirs>`,
   `checkout FETCH_HEAD`. Re-running with the checkout present moves it to
   the pin without re-fetching what it has. The box reaches GitHub (checked
   2026-10-21).
2. **The runner, `tools/test_yonder_wpt_host.c` and `.sh`**, on the
   headless binding. The binding is `tools/test_yonder_scripts_host.c`:
   it `#include`s `userland/apps/yonder/yonder.c` with `main` renamed and
   fakes the OS (files, the work pool, fonts, the clocks) around it. Its
   shape to reuse, read before designing: `stream_window()` builds a
   window; `start_trip` + `yonder_mail_post_head` + `stream_post_body` +
   `stream_post_verdict` deliver a page as the worker would;
   `loop_settle()` turns the loop until nothing is owed; `script_land(tail,
   source)` and `sheet_land` answer a `<script src>` or sheet job by the
   tail of its address; `now_ms` and `clock_us` are the fake clocks a case
   moves. The fakes live inside that file as statics. Two ways to reuse
   them, your call, both used in the tree: include that file with its
   `main` renamed (how it includes `test_js_port_host.c`; its unused case
   functions then need `-Wno-unused-function` for the runner's unit under
   `-Werror`), or move the fakes into a shared `tools/yonder_host_fakes.inc`
   both files include (cleaner; touches a file four mutant harnesses name
   by path, so check `tools/test_*_mutants.py` still anchor, per
   `tools/anchorcheck.py` if it is in the kit, else by reading them).
   The runner: arguments `--wpt <checkout> --dir <dir> --expect <file>
   [--update] [--only <substring>]`. For each `.html` under the directory
   (not `-ref.html`, not under `support/` or `resources/`), and for each
   `.any.js` (which WPT runs through a generated wrapper: synthesise
   `<!doctype html><script src=/resources/testharness.js></script><script
   src=/resources/testharnessreport.js></script><script src=X.any.js>
   </script>` for the window variant, reading the file's `// META:
   script=` lines for extra scripts), deliver the page at
   `http://wpt.test/<dir>/<file>`, answer every script and sheet job whose
   path is under the checkout from the file, fail any other, then settle
   with the clock ADVANCED between idle turns (10 ms a turn, which is the
   tick) up to a virtual budget of 60 s so `step_timeout` and the harness's
   own timeout fire deterministically. The results: our
   `tools/wpt/testharnessreport.js` (served for `/resources/
   testharnessreport.js` INSTEAD of the checkout's empty one) calls
   `add_completion_callback` and appends an element, id
   `os64-wpt-results`, whose text is one line per test, `PASS|FAIL|
   TIMEOUT|NOTRUN <tab> <name>`, plus the harness status; the runner walks
   the tree for that id after settle (the fixtures' `probe_text_is` shows
   the walk). A page that never reports is `TIMEOUT` for the file. Output:
   one line per test, a summary (`dom/nodes: 412 pass, 93 fail, 8 timeout
   of 513`), and against `--expect`: new passes listed (add them), lost
   passes listed and EXIT 1. `--update` rewrites the file, sorted, one
   `<file>\t<test name>` per line. Document the runner in VERIFICATION.md
   where the host suites are listed.
3. **The switch and the table.** `os64_dom_options_t` gains `bool modern`
   (default true in `os64_dom_default_options`). `userland/libdom/
   modern.c` holds `d_modern_install(dom, ctx, global)`, called from
   `os64_dom_create` after `d_window_install`, which walks a static table
   `{ const char *name; int (*install)(os64_dom_t *, JSContext *,
   JSValueConst global); }` when the option is set and returns at once
   when it is not. The first row is the interface objects: cherry-pick
   `8a6cca6d` (`opus/arc-interface-objects`: `Element`, `HTMLElement`,
   `Node`, `Document`, `CharacterData`, `DocumentFragment`, the form and
   image interfaces as Illegal constructors over the binding's own
   prototypes, with `tools/test_dom_classic.inc` cases) and move its
   install from `d_classic_install` into the table's row; its cases then
   run with the option on and prove the names ABSENT with it off. In
   libgarb, `Prop` in `props.c` gains `bool modern` (false for every
   property today) and the parse is told whether marked properties are
   known: find the one place a declaration's name is looked up in
   `kProps` and give it the answer through the structure that already
   reaches it (the parse's options or environment; NOT a global), so a
   marked property with the switch off is unknown, which yonder already
   records as `MISSING css-property`. yonder: `yonder_settings_saved_
   modern()` reads `modern = on | off` from `yonder.conf` (absent is ON;
   a value that is neither is refused loudly on the status line and read
   as on), the Settings window gains the checkbox ("Modern web surface",
   Apply and Save as default like the others, so `yonder_settings_open`
   grows a parameter and a callback), and the one answer is handed to
   libdom (`options.modern`) and to libgarb at every parse. A page file
   fact line `modern: off` when it is off, so a record taken with the
   switch off is never mistaken for a regression.
4. **Acceptance.** Run the runner on `dom/nodes` before and after the
   interface objects; commit both expectation states (the first commit
   carries the "before"). Web.archive.org's player and DuckDuckGo's base
   bundle are the P5 acceptance (tell Chris what to open).

**Left to the builder.** The fakes' sharing; the runner's test
discovery details (WPT's `.window.js` and `.worker.js` variants are NOT
run: say so in VERIFICATION.md); how `.any.js` META lines are honoured
(`script=` yes; `global=` picks the window variant only; `timeout=long`
raises the virtual budget); whether the Settings checkbox needs a restart
or applies to the next page (next page is enough; say which).

**PR states.** `dom/nodes` before and after; the tally of records naming
`Element`, `HTMLElement`, `Node` (grep the P5 files; 0b's tool may not
exist yet).

## Slice 0b: the tally and the replay corpus — built

**Built, 2026-10-21.** MODERN_WEB.md § Slice 0b, as built, is the record of
what was built, the census, and where it departs from this brief. The brief
below is struck: kept as it was handed out, and no longer the specification.

**Build.** Four things, one PR, no libdom change.

1. **`tools/yonder_tally.py <dir>...`**: over page files
   (YONDER_DIAGNOSTICS.md's format; `address:`, `verdict:`, `MISSING <kind>
   <name> (<count>)`, `FAILED <what>: <message> (<count>)`), a ranked table
   of `MISSING` names by how many files name them and by total asks,
   split by kind; `--surface <name>...` prints how many files name ANY of
   the given names, which is the number every fan-out PR states;
   `--failed` ranks the FAILED messages with their addresses. Python 3,
   standard library, `-I`-safe (it reads files it was handed; the P5's
   records are data). Fetch the 401 records to a scratch directory to run
   it: the P5 is `ssh -o IdentitiesOnly=yes -o BatchMode=yes -i
   ~/.ssh/os64_models yogi@192.168.137.246`, files under
   `/home/tmp/yonder/backup/`, one `cat` per file (os64's sshd has no
   scp). Put the ranked table, as it stands, in the as-built section: it
   is the arc's census, quoted in MODERN_WEB.md § The tiers from memory.
2. **`keep_pages` in `yonder.conf`** (`yonder_settings_saved_keep_pages()`,
   default off, meaningful only with `diagnostics =` set; a Settings line
   beside the diagnostics one if there is one, else none). With it on,
   beside each page file `<name>.txt` yonder writes `<name>.html`: the
   page's bytes AS THE PARSER WAS FED THEM (the stream's body chunks after
   transfer decoding, before character decoding: `yonder.c`'s stream turn
   is where they pass, `g.stream`; DOM_D4.md § The stream), and a
   directory `<name>.d/` holding every script source and sheet the page
   fetched, each as the job's PRODUCT (a script's product is decoded
   UTF-8 text, `yonder_script_t`; a sheet's is whatever `sheet.c` hands
   the window; keep each as the window received it, so a replay feeds the
   same thing), named `<n>.js` / `<n>.css` with an `index` file of
   `<n>\t<kind>\t<url as the job carried it>` lines. The page file gains
   fact lines a replay needs to rebuild the head: `content-type:`,
   `charset:`, and `encrypted:` beside the `status:` it has. Writes follow
   the temp-and-rename rule; a page let go unshown is kept too (its record
   is written at departure already, `diag_leave`). Fonts and pictures are
   not kept (say so in the index's header line).
3. **The replay runner, `tools/test_yonder_replay_host.c` and `.sh
   <kept-dir> <out-dir>`**, on the headless binding (0a's brief § 2 says
   how the binding is reused; coordinate with 0a on the shared fakes by
   reading its branch, `opus/tmw-0a-runner`, before choosing; if 0a has
   not chosen yet, take the include-with-renamed-main route and let 0a
   converge). For every `<name>.txt` with a `<name>.html`: a window,
   `start_trip` to the recorded address, a head from the fact lines, the
   bytes posted in the stream's chunk size, the verdict, then settle with
   the clock advanced as 0a's runner does; script and sheet jobs are
   answered from `<name>.d/index` by exact URL, fonts and pictures
   refused; the record is rendered (`yonder_diag_render`) to
   `<out-dir>/<name>.txt`; one line per page `clean` or `N MISSING, M
   FAILED`; a summary `replayed P, clean C`. No network, ever: a job for
   an address the index lacks fails as a fetch failure does.
4. **`tools/yonder_corpus_diff.py <before-dir> <after-dir>`**: per page,
   the set of `MISSING kind name` and `FAILED what: message` lines (counts
   ignored); pages that GAINED a line are regressions, printed with the
   lines, exit 1; pages that lost lines are progress, printed; totals.
   This is what a fan-out PR runs when kept pages exist.
5. **The compile-time instrument** (MODERN_WEB.md slice 7 needs it): the
   page file's per-script line gains the script's evaluation time in
   microseconds (around `os64_js_run` in `scripts.c`, `os64_micros`), so
   slice 7's cache shows as that number falling. The harness's fake clock
   makes it deterministic in cases.

**Proof.** Scripted-page host cases: a page kept on the fake disk has the
bytes, the index and the facts the brief names; a page let go unshown is
kept; the setting off keeps nothing; a kept page replays to the same
record the direct load produced (the replay runner's own case set, built
from the fixture pages the scripted-page host already carries, run by its
`.sh`); the diff tool on two hand-made directories reports the regression
and the progress and exits as specified; the tally on three hand-made
records ranks as expected (a Python test file beside it, run by the `.sh`).

**Left to the builder.** The `.d/` file naming beyond `<n>`; whether a
sheet's product is bytes or text (read `sheet.c`); how a chunked body is
appended under temp-and-rename (append to the temp during the stream,
rename at the verdict; a page that never gets its verdict leaves the
temp, and the next write of that name replaces it).

**PR states.** The replay of the P5's existing records is impossible
(they carry no bytes): say so, and give Chris the two lines to put in his
`yonder.conf` on the P5 to start the corpus. The tally table.

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
  holds the map; FILES ARE YONDER'S: a provider in the shape of
  `os64_dom_cookies_t` (`os64_dom_set_storage`), with `load(opaque,
  origin, out image)` at first touch and `store(opaque, origin, image)`
  after a task that changed it, where the image is one byte buffer libdom
  serialises and parses itself, so the format is libdom's and yonder only
  moves bytes. yonder (`userland/apps/yonder/storage.c`) keeps
  `<profile>/storage/<origin-as-filename>` (the origin with every byte
  outside `[A-Za-z0-9.-]` percent-encoded), temp-and-rename, coalesced to
  at most one write a second plus one at the page's departure. **This
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
  same-origin or `SecurityError`. Back and Forward into a same-document
  entry do not fetch: they set `location`, fire `popstate` with the state
  as a task, and scroll to the fragment if any. `hashchange` on window
  when only the fragment changed, by any route. `history.length` counts
  them.
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
  and SYNC: a synchronous request runs the fetch inline on the window
  thread through `way_fetch_whole`, bounded by the task's budget, and
  writes a `sync xhr` fact line because it stalls the window; the old web
  is full of them and refusing is a half API), `setRequestHeader`, `send`,
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
  nothing else exists. Add: `os64_js_eval_module(runtime, source, length,
  name, outcome)` which compiles with `JS_EVAL_TYPE_MODULE |
  JS_EVAL_FLAG_COMPILE_ONLY`, then evaluates the module through
  `JS_EvalFunction` (its promise is drained by the ordinary job queue
  under the turn's budget; a rejection is the outcome's exception);
  `os64_js_set_module_loader(runtime, normalize, load, opaque)` installing
  QuickJS's `JS_SetModuleLoaderFunc` with the host's two callbacks
  (resolve a specifier against the importing module's name, which is its
  URL; and hand back a module's SOURCE for a resolved name, or NULL with a
  reason); `import.meta.url` set to the module's name through
  `JS_SetModuleLoaderFunc`'s normaliser plus the port's `js_module_set_
  import_meta` equivalent (read `quickjs-libc.c` upstream for the shape;
  the port does not link libc, so write the eight lines). AND the one
  thing the engine does not offer: **the static import list of a
  compiled module**, so the host can prefetch the graph before linking.
  QuickJS's loader callback is synchronous and is called during linking;
  with the sources not yet fetched it can only fail. So: a patch in
  `userland/libjs/patches/` adding `JS_GetModuleRequests(ctx, module,
  &count)` that returns the compiled module's request specifiers (the
  `req_module_entries` of a `JSModuleDef`), exposed as
  `os64_js_module_requests(runtime, source, length, name, callback,
  opaque, outcome)` which compiles with COMPILE_ONLY, calls back per
  specifier, and frees the module. Teardown: a compiled-but-unlinked
  module is freed with the context (check TEARDOWN.md's reclaim rules
  and add the case).
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
  event on the element and a page-file `FAILED` line. **Dynamic
  `import()`**: the engine calls the loader synchronously when the job
  runs; a module not in the map is fetched INLINE on the window thread
  through `way_fetch_whole` under the task's budget, and a fact line
  `inline import` goes in the page file because it stalls the window.
  That is the lean, not a ruling: a DEBTS row books the asynchronous
  loader (prefetching the graph when `import()` is seen is not possible
  without parsing the argument, which may be any expression).
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
binding's `max_bytes`); the patch's shape against the pinned QuickJS;
whether `JS_EvalFunction`'s promise needs a job to settle before the task
ends (it does when the module has a top-level await; the ordinary drain
covers it).

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
  a SHA-256 of the source (libtls has it; check the link); and
  `os64_js_run_bytes(runtime, bytes, count, name, outcome)` which refuses
  a blob whose tag is not this build's or whose digest is not the
  source's (the caller passes the source's digest it keeps with the
  cache entry), then `JS_ReadObject` with `JS_READ_OBJ_BYTECODE` and
  `JS_EvalFunction` under the same turn, budget and outcome rules as
  `os64_js_run`. QuickJS's reader TRUSTS its input: a blob is never read
  unless its digest matched, and that is the whole security story, stated
  in the contract: the cache directory is trusted as the binary is, and a
  tampered blob with a correct digest is the same threat as a tampered
  binary. Say so in DEBTS as a known limit of a cache under `/var/cache`.
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
without reading, a wrong digest refused, a truncated or bit-flipped blob
with a forced-correct digest under ASan to see what the reader does (if
it crashes, the digest is the guard and the test documents that; if it
reports, better); the cache host suite for the sibling entry's lifetime.

**Acceptance.** The P5's second visit to YouTube's front page: the
compile seconds gone from the page file's line; danlegt's scripts show
`cached`.

**Left to the builder.** The blob's framing bytes; whether the sibling
is one cache entry or a field of the response's (one entry: the cache's
rules already cover one); compression.

**PR states.** The host before-and-after; the P5 lines Chris should read.
