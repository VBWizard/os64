# YONDER_DIAGNOSTICS.md — the page file and the badge

Status: BRIEF, written 2026-10-08 by Fable at Chris's request, for the
builder of the slice. Opus builds, Fable reviews. The "as built" section
joins this file with the code, in the slice's PR. It runs after D11
(#229) and before the performance slice, so the first P5 run after it
lands is the first census of the Modern Web arc.

**Why.** When a site misbehaves on the P5 there is no way to tell whether
yonder did something wrong or the page asked for something yonder does not
have. The status line says one thing and the next hover erases it. This
slice makes yonder write down, per page, what the page asked for that is
not there and what went wrong, in a form that answers one grep. It is also
the Modern Web census: the files accrue from daily use, and a tally of
them ranks what to build first.

## Obligation 1: the page file

- `diagnostics = <directory>` in yonder.conf. Absent means off. Yonder
  makes the directory if its parent exists; otherwise the status line
  says so once per run and nothing is written. Chris points it at
  /tmp/yonder for a day and at /home/yonder/pages to keep.
- One file per page load: `<host>-<seq>.txt`, the sequence per run,
  zero-padded to four digits so `ls` sorts in browse order. The first
  line carries the full address; the name only has to be findable.
- Written when the page arrives, again when its last picture and font are in, and
  when it is left (navigation, reload, close), because the asks keep coming after load: timers,
  clicks, a script that dies on hover. The departure write is complete.
  Write a temp beside it and rename over, the `os64_conf_write` shape.
- **The keyword rule.** Two uppercase tokens, `MISSING` and `FAILED`,
  begin the record lines and appear nowhere else in the file. A file with
  neither says the page used nothing yonder knows it lacks and nothing
  went wrong: a layout fault on such a page is OURS. The verdict is on
  line two: `verdict: clean` or `verdict: 7 MISSING, 2 FAILED`.
  `grep -L 'MISSING\|FAILED'` lists the clean files, `grep -l FAILED` the
  ones to read. The proof asserts the tokens are exclusive to record
  lines.
- `MISSING <kind> <name>`, once per name, with a count: a global reached
  for that is not there (bare `fetch`, `window.fetch`, `self.fetch`), a
  script yonder refuses to run (`type=module`, a worker, a service worker
  registration), an element whose content is not drawn (canvas, video,
  audio, SVG with Modern Web off, a custom element), a CSS property, at-rule or function the cascade
  skipped, a font family with neither a page face nor a system family
  (the resolver's: PILE3.md § Web fonts).
- `FAILED <what>: <message>`: a script that died with its message and
  source name, one stopped at the execution limit, out of memory in
  layout or reading, a link refused and why, a font file that did not
  fetch, unwrap or open, and `FAILED unsupported: canvas`, once a page
  however many canvases it has, where scripting is on (DEBTS.md, "Loud
  for canvas only", for the kinds only counted). Inline SVG render refusals
  produce `FAILED svg`; supported inline SVG is not counted as missing. Everything the status line
  says today and then loses.
- Plain lines for the rest: final address after redirects, time to
  arrive, bytes, scripts by source name with outcome, layouts and their
  time including those scripts forced, images, sheets.
- **The instrument changes nothing the page sees.** A global miss is
  recorded at the engine's miss on the global object and the answer
  stays what it is (undefined, or the ReferenceError). The known-names
  approach, installing accessors for `fetch` and friends that record and
  return undefined, is REFUSED: `'fetch' in window` and `hasOwnProperty`
  would say yes, and pages feature-detect with exactly those. If the hook
  needs the engine, it is patch 0008 in the series, with context, under
  `tools/test_js_prepare.py`'s rules. Property misses on DOM objects are
  NOT recorded (a page's own `el.flag` misses legitimately); a missing
  DOM method shows up as FAILED when called, and silent feature
  detection on an element is the stated gap.
- The counters live whether or not the setting is on; the setting decides
  the file.
- Existing `--script-audit` log lines stay as they are, so the log's
  timeline still correlates with the file.

## Obligation 2: the badge

- At the right end of the status bar, present only when nonzero, per
  page: `MISSING 7  FAILED 2` (ASCII; the terminals are Latin-1). The
  same tokens as the file, so what you see is what you grep.
- The existing status text keeps the rest of the bar; the badge is its
  own label at the right edge, so hover addresses never push it around.
- Clicking it does nothing in this slice. Showing the current file as a
  page in yonder is a later, small slice.

## Proof

- Scripted-page host: a dying script writes a FAILED line and the verdict
  counts it; a timer dying after arrival changes the departure write and
  not the arrival one; `typeof fetch` records MISSING global fetch and
  the page's path is unchanged (`'fetch' in window` is false, the
  oracle); a clean page writes `verdict: clean` and no token anywhere; a
  module script and a canvas each record their kind; setting absent
  writes nothing while the badge still counts; directory with no parent
  says so once.
- Token exclusivity: the harness scans every file the cases wrote for the
  tokens outside record lines.
- Mutants: the departure rewrite skipped; a miss counted twice; the
  tokens leaking into a plain line.
- Guest: `/diag.html` on the harness server, then `cat` the file.

## Record

DOM.md gets a row pointing here. The "as built" section of this file
ships in the slice's PR with the code, and names anything above that was
built differently and why.

## As built

Status: built by Opus, 2026-10-08, for Fable's review.

**Where it lives.** `userland/apps/yonder/diag.c` is the record and its
file: pure but for the writer, with no clock and no window. Each `Page`
owns a `yonder_diag_t` from the start of its navigation. `start_trip` and
`open_local` begin it, the stream carries it, `stream_finish` hands it to
the page, and `page_clear` writes it and frees it, which makes that the
departure write. A struct copy of a Page borrows the record and never ends
it. A load that never became a page writes its record from `stream_drop`.
The arrival write is in `arrive_now`, after `load` has run. When the page's
last picture is in, shown or not, `reaped` writes it again
(`diag_pictures_in`), because pictures arrive after the page does.

**The engine hook is patch 0008**, `JS_SetGlobalMissHandler`. It is
reached through libdom's `os64_dom_set_global_miss`, which yonder's script
host installs when it makes a runtime. A QuickJS lookup has two ends that
say "not found". One is `JS_GetPropertyInternal`'s. The other is the
interpreter's inline field read (`GET_FIELD_INLINE`), which never calls it
when the chain is plain, so `window.fetch` reached only that one. Both
call `js_hear_global_miss`. The name arrives in a stack buffer, so hearing
it allocates nothing, and the lookup's answer follows unchanged.
`tools/test_js_prepare.py` pins both hunks' placement, and an exact-context
drift check covers the inline one.

**The cascade's half is in libgarb**: `garb_cascade_skips`, a tally the
build keeps of each property with no reader, each at-rule it keeps no rule
of, each function no reader accepts in a declaration it dropped, and each
value it dropped or approximated. A tally that finds no room never marks
the cascade incomplete. `MISSING font` is the font resolver's
(PILE3.md § Web fonts), not this tally's.

**A file, in the shape the build writes** (`/tests/pages/diag.html`, left
by Reload, so this is the departure write; the times are an example, and
a timer's task keeps the plain `failed`, since it evaluates no script):

    address: file:///tests/pages/diag.html
    verdict: 7 MISSING, 2 FAILED
    MISSING script module (1)
    MISSING global fetch (1)
    MISSING global nothere (1)
    MISSING element canvas (1)
    MISSING css-property aspect-ratio (1)
    MISSING css-function color-mix (1)
    MISSING css-at-rule keyframes (1)
    FAILED script file:///tests/pages/diag.html#inline-1: ReferenceError: 'nothere' is not defined (1)
    FAILED script file:///tests/pages/diag.html#timer-1: TypeError: cannot read property 'x' of null (1)
    script file:///tests/pages/diag.html#inline-1: failed in 1840 us
    bytes: 967
    final address: file:///tests/pages/diag.html
    body: html
    arrived after: 271 ms
    laid out at: 828 px in 70 ms
    written: when the page was left
    pictures: 0, 0 shown, 0 could not be read, 0 past the memory kept, 0 still coming, 0 not asked for
    sheets: 1, 1 ready
    script file:///tests/pages/diag.html#timer-1: failed

**A `javascript:` link's script** (MODERN_WEB.md slice 0c) is a script of
the page's, run as one task, and recorded as one: `script
javascript:#inline-<n>: ran in <n> us`, numbered apart from the page's
inline scripts (the first link followed is `#inline-1`, whatever the page
holds), and a failure `FAILED script javascript:#inline-<n>: <message>`.
A link whose script could not be queued (the page out of scripts, or
memory) says `FAILED script javascript:: the link's script could not be
queued`. The libway refusal such a link used to leave, `FAILED link
refused`, no longer happens.

A promise a task rejected and nothing handled by the end of its microtask
checkpoint, and no `unhandledrejection` listener cancelled, is a failure of
that task (MODERN_WEB.md § P2a): `FAILED script <the script's address, or
the event's or timer's name>: unhandled rejection: <reason>`, the reason
said as libdom reads it without running the page's code (an Error's name
and message, a primitive as it is). The task's first failure and sixteen
rejections after it are a line each, and the rest one line counting them,
`... : and <n> more unhandled rejections`; the status line says the task's
first failure, as it always has. A record finds a line by its key, so a
page that fails without end costs each failure a few probes.

A script's FAILED line says where it failed: the first frame of its stack
that names a place in a script, after the message (`scripts.c`'s
`yonder_scripts_failure`, over libdom's `os64_dom_stack_frame`). In the
failing task's own `src` script, classic or module, that is the line and
column, `TypeError: cannot read property 'length' of undefined, at Xa
1:23456`, which finds the place in a minified script that is one line
long; in another script's function, that script's address before them,
`at boom https://site/lib.js:2:7`; in one of the page's classic inline
scripts, `inline`, `at inline:3:1`; and anywhere else, the engine's own
name for the file (`at onclick onclick attribute:2:5`; an inline
module's item name; `<input>` for a function made by eval or `new
Function`). A script's own code and an anonymous function have no name
there. Text that did not parse is no script's place: `JSON.parse` given a
page of HTML fails at the line that called it, and so does an eval of
source that does not compile. A failure whose value has no stack (a
DOMException, a thrown primitive) says its message alone (DEBTS.md). The
same message at two places is two lines; the status line says the
message alone, as before.

### The page's fonts (S9a4, PILE3.md § Web fonts)

A page with `@font-face` rules, or one whose layout asked for a family,
gets plain lines for its fonts at every write, after `sheets:`:

    fonts: 3 faces (0 past the most a page keeps), 2 files: 1 open, 1 failed, 0 coming
    font face 1: Web 400 normal: https://site/w.woff listed (WOFF, 34120 bytes)
    font face 2: Body 400 italic U+0-7F: format(woff2) not read: b.woff2; https://site/b.ttf failed; https://site/c.ttf coming
    font face 3: Cjk 400 normal U+4E00-9FFF: not needed: its range is not in the page's text
    font family Web: the page's faces
    font family Body: the page's faces, none in the list, which begins with DejaVu Serif
    font family Arial: no face; the list begins with DejaVu Serif
    font family DejaVu Sans Mono: a system family

- **A face line walks its sources in order** up to the one the face is
  drawn from — every one of them when no file of the face's is kept (not
  asked for yet, never asked for, or each failed) — whole however long (only its ranges past the first 64 are
  counted, `and 4033 more`, not spelled). An open file says `listed` (the
  container and the bytes that came) when the face was in a list the
  layout of the tree on screen gave text — the text context then draws
  each glyph from the first font in the list that has it, so a listed
  face may draw none — or `came` with why it was not: `not listed since
  it came` before a layout has met it, a size refused for room or refused
  to open, or no text asked for it (the file may be another face's that
  shares it). Then `coming`, `came …, held for room to open` (the open
  waits for the room the tree on screen holds), `failed` (the reason is
  the file's `FAILED font` line — one the page's table of files had no
  room to keep among them), a `format()` or `local()` passed over, `not
  asked for yet` on a page whose tree on screen was laid out before it was
  shown (a page is laid out in its faces once it is) or for a source after
  one that failed since that layout (the next layout asks for it; a
  failed layout's held files fail without asking for one, so as not to
  lay the page out again at every batch), or `never asked
  for`: no text asked for its family, or the lists it was in had no place
  left for it (PILE3.md § Web fonts). A face that is passed over whatever
  its file says only that, in place of its sources: `not needed` when its
  ranges miss the page's text as it is at the write, `passed over` with
  the count when it has more ranges than a text font takes. A file
  several faces name is one file, and each face says how IT fared. The
  `fonts:` line counts files by their state — `open` is opened, not drawn
  — and says when more families were asked for than the 512 the record
  keeps, or when some were not kept for want of memory: the two are told
  apart. The section is written whole at each write, so a face a script's
  rebuild took away goes with it.
- **A family line is what the layout of the tree on screen found** for a
  family name it was asked, the best any of its runs found (a name one run
  listed a face of is the page's faces) — a layout that fails leaves the
  lines of the one still shown, and a name the page no longer asks for
  goes: the page's faces, the page's faces with none in the list yet, a
  system family by that name, or nothing — and the font the run's list
  begins with (the text context draws each glyph from the first font in
  the list that has it, so this is where the text starts looking, a page
  face the list puts first included). A page's arrival write has the families its layout asked for
  before it was shown. A page draws in none of its own faces until then,
  so a name they answer is the page's faces with none in the list there,
  and not MISSING; the layout on screen that follows says what they serve.
  A page let go before it was shown (its layout failed, or the navigation
  was abandoned) writes its fonts section from its own rules and the
  families its layout staged, with their MISSING lines — never from the
  files the page on screen fetched.
- The records: `FAILED font-fetch: <address>: <why>` for a file that did
  not fetch, unwrap or open (its fetch decided it, which a replay cannot
  reproduce: § The page kept), `FAILED font` for one never fetched (the
  page's table of files could not keep it, or the workers would not take
  it),
  and `MISSING font <family>` for a family with neither a page face nor a
  system family, its count the most lookups one layout made for it (each
  run of text in it, each time a list names it).
- **The second write waits for the last picture AND the last font**, and,
  as for pictures, for the layout a face that arrived owes (`written: when
  its pictures and fonts were in`) — and for anything that layout wanted
  in turn, a failed face's next source. A font that fails with no job to
  come back (no worker, a job the pool would not take, a held file
  refused again) owes the write too. The departure write is made before
  the page's fonts are let go, so it names them too.

#### The fonts record's state, and when each part is true

Written after six review rounds had each found a part of this the slice
had not decided. Each cell is a promise the code keeps, and each is
checked against the code before a review is asked for.

**Which moment the record describes: the tree on screen.** Nothing a
layout finds reaches the record before its tree is shown. Faces' use and
families are what the layout that made the shown tree found; files are
the shown page's, as they stand; a face's range is met against the
page's text as it is at the write.

| State | Begun | Changed | Made the record's | On a failed layout | Let go |
|---|---|---|---|---|---|
| Files (`s_web.files`) | a shown layout wants a face's source | fetch, hold, open, fail | as they stand, at every write | the files it wanted stay asked for | with the shown page (`webfonts_leave`, after the departure write) |
| Faces' use (`face_laying`) | every layout of the shown document (`webfonts_layout_begins`), whether or not its text asks for a font | each lookup of that layout | when its tree is shown (`page_show`) | the record keeps the tree on screen's | with the shown page |
| Families (`s_web.laying`) | as faces' use | each lookup: the best answer of the layout's runs, and its MISSING count; a name past the table, or with no memory, only flags it, as does a lookup that ran short before it could answer | with faces' use, MISSING lines written then and the badge told | as faces' use | with the shown page |
| An arriving page's families (`s_arriving`) | every layout of a page not yet shown that has a record | each lookup | at arrival (`arrive_now`), MISSING lines written then | its next layout begins them again | adopted, or written with the departure of a page let go unshown and freed with its record (`diag_leave`) |
| The characters (`s_web.chars`) | asked for by a lookup or a write | — | keyed by document, version and layout | — | rebuilt for whichever page asks |

**The writes.** Arrival: the adopted families, faces with no source asked
for yet. The second write: once nothing is out and the page is laid out
again — owed by the last picture or font in, by a font failure, and by
arrival itself for a page with faces, whose first layout on screen may
ask for no file at all. Departure: everything, before the fonts are let go. A page let go
unshown: its own rules and its staged families, never the shown page's
files.

**Caps: every one is said when it is passed.** Faces past 256 a page
keeps (the `fonts:` line counts them); files past 512 (a `FAILED font`
line); families past 512, or with no memory to keep (the `fonts:` line
says which, and that those names have no line here, a missing one
included — nothing about a layout reaches the record before its tree is
shown, so a name the census could not keep is written nowhere rather than
early); a table's 512 lines
(`not kept: N`); lost lines past the 4,096 the record can tell apart
(`, and more not counted`); a face's ranges past 64 (`and N more`).

**Short of memory, the record is best effort, and says when it is.**
What it writes is true; where memory ran out it says that something was
not kept — on the line it could not finish, or on its own line when it
could not even name what went — without promising to say which. Counts
stay exact while memory lasts and are marked as floors once it does not.

### What paints cost (the perf slice, YONDER_PERF.md)

The shown page's record carries two plain lines about its paints since it
was shown, or since its record was last written on asking:

    paint: 26 paints in 11.9 s (26 whole views), 10.9 Mpx of the view; 354 ticks
    paint stages: ms a paint (most): render 121.4 (130.5), view 119.5 (128.6), backgrounds 11.1 (11.6), pictures 2.9 (3.2), text 0.9 (1.0), groups 0.0 (0.0), shadows 0.0 (0.0), gradients 63.0 (69.6), warps 31.3 (36.8), publish 10.6 (11.6), ticks (a tick) 22.0 (27.1); window 1.9, the view's rest 10.1

- **`render`** is libui's whole paint, the root panel and the chrome
  included, and **`view`** the page's part of it; `window` is the
  difference. Inside the view: **`backgrounds`** (pictures behind boxes and
  the canvas), **`pictures`**, **`text`**, opacity **`groups`**, box
  **`shadows`** (their masks built and blurred, or copied out of the paint
  cache), **`gradients`** (computed pixel by pixel, or copied out of it),
  **`warps`** (turned boxes drawn through their matrix);
  `the view's rest` is the view outside those. **`laid`** is inside
  `shadows` and `gradients`: the part of them spent laying their pixels on
  the glass, the rest of each being working them out or copying them out
  of the cache. **`publish`** hands the
  damage to the compositor. **`ticks`** are per tick, not per paint: a
  tick's work finding what moved and marking it, before its paint.
  **`schedule`** is per call (handing the ticker its next deadline),
  **`tasks`** per script task (a script, a timer or an event, its run),
  **`layouts`** per layout. The `paint:` line has grown since this
  example: the layouts, how many of them were for the animations alone,
  and how many of those repainted only what they changed (YONDER_PERF.md §
  perf-4), how late frames were, the loop's longest wait, the
  script tasks by kind, the ticker's bells and its own counts, the
  deadlines already past when handed to it, by source (YONDER_PERF.md §
  Measure first), and the paint cache's counts: the blocks it found and
  made, what it let go for room, how often a picture was worked out
  without it and why, and what it holds against its budget
  (YONDER_PERF.md § perf-2).
- Timed with the processor's counter, its rate taken from the clock across
  the same span, so the numbers are the machine's own: the P5's on the P5.
  A span with nothing painted keeps every other count and its stages
  (`nothing painted in 10.0 s; ...`): a page whose timers compute without
  changing what is shown is still on the record. A span too short to
  time, or one the counter stepped back across, says so and keeps its
  counts without the times.
  An interval whose counter stepped back (the thread moved to a core whose
  counter is behind) counts as nothing, and the line says how many.
- **`d` in the page writes the record now** (`written: when asked`),
  keeping the counts since the last `d` as a numbered reading with where
  the page was scrolled — `paint reading 2: at y 2140: 96 paints in 10.1 s
  ... | ms a paint (most): render ...` — and starts them again. So each
  part of a page is measured on its own and every reading stays: scroll
  there, `d`, wait, `d`, scroll on, wait, `d`. The status line names the
  reading it wrote. `paint` and `paint stages` are the counts since the
  last `d` that reached its file: one that did not keeps its reading in
  the record, lets the counts run on, and says so on the status line.

### Built differently from the brief, and why

- **The sequence continues across runs.** A per-run sequence restarts at
  0001, and the second run into `/home/yonder/pages` would write over the
  first run's census, host for host. The first write of a run starts one
  past the highest `-NNNN.txt` already in the directory, so `ls` still
  lists in browse order across runs. Each load then CLAIMS its name as it
  begins (`yonder_diag_claim`, § The page kept, below): its file is made
  only where nothing is, and a number another yonder took meanwhile is
  passed over, so two yonders on one directory never write one name; a
  load that finds no name it can make in 64 has no file this time, and
  never writes over another's.
- **A load that never became a page has a file too**: no such file, a 404
  that was not a page, out of memory starting it, too busy to start. Each
  is `FAILED page: <the status line's sentence>`. "One file per page load"
  is read as one per load attempted, because those are the loads the
  status line used to lose.
- **The count is the last field, in parentheses, on every record line**,
  `(1)` included, so a census tally parses one shape.
- **The grep is exact, unanchored.** The verdict names only the tokens
  that have records (`verdict: 2 FAILED`, not `0 MISSING, 2 FAILED`), and
  DATA that spells a token has its second letter percent-encoded wherever
  it stands (`M%49SSING`, `F%41ILED`), in plain lines and in a record's
  kind, name, source and message alike. An address encoded that way spells
  the same address. So `grep -l MISSING` lists exactly the files with a
  MISSING record, and `grep -c FAILED` is the FAILED count, plus one for
  the verdict when there are any. A byte below 0x20, 0x7F and a backslash
  are written `\xHH` and `\\` on every line, so no message can begin a
  record line of its own. **The spelling is injective** (Codex on #326):
  a `%` of the data's own that a reader would take for part of a
  spelling, one beginning `%25` or one standing where `M%49SSING`'s or
  `F%41ILED`'s does, is written `%25`, so an address holding a literal
  `M%49SSING` is written `M%2549SSING` and reads back as itself, never as
  MISSING; every other `%` (`75%`, `%20`) is written as it is. A reader
  gets the data's bytes back by undoing `\\` and `\xHH`, then `M%49SSING`,
  `F%41ILED` and `%25` in one pass from the left (`tools/yonder_records.py`'s
  `unescape`, the replay's `rp_unescape`). Records written before this
  wrote a `%25` of their own as it was, so in those a literal `%25` reads
  back as `%`. **A `data:` address is shortened as a line is kept**
  (Chris, 2026-10-29), so for such a line the bytes back are the
  shortened ones: from the first `data:` that begins a word to its
  field's end, a run past 200 bytes keeps its first 100 and its last 40
  with `...(<n> more bytes)...` between. A data: URL is its document, so a
  module's name, a request's address or a failure's place can be
  kilobytes of percent-encoded script on one line. The page's own
  `address:` line is written whole, since a replay loads the page from it.
- **The kinds are** `global`, `script` (`module`), `element`,
  `css-property`, `css-at-rule`, `css-function`, `css-value` and `font`,
  and for pictures `picture-scheme` and `image`.
  `css-value` (Fable's round-one P3, ruled in by Chris) is `<property>:
  <words>` for a property libgarb reads, in two shapes. One is a
  declaration dropped though its value is bare words (`display: inline
  frob`). The other, worse because the page draws wrong, is a value read
  and laid out as something else: props.c's approximations,
  `display: flow-root`, `background-clip: text`, `conic-gradient`,
  `image-set`. A value holding a number or a string is the page's fault,
  and a prefixed word is a dialect. Fable's examples (`position: sticky`,
  `display: contents`, `overflow: clip`, `white-space: break-spaces`) are
  read and laid out today, so they correctly record nothing. A worker shows
  up as `MISSING global Worker`. A service worker registration is a
  property of `navigator`, so it falls under the stated gap and is
  `FAILED` when called unguarded.
- **Every global miss is recorded, including a page's own**
  (`if (!window.myApp) window.myApp = {}` gives `MISSING global myApp`).
  At the engine's miss, a platform name and a page's name cannot be told
  apart. A list of platform names would be the known-names list over
  again, the one the brief refuses. A census across pages tells them
  apart, because a platform gap recurs on every site that uses it and a
  page's own name does not.
- **The census's first finding was `self`.** libdom did not define it,
  so `self.fetch` threw "self is not defined". libdom now defines `self`,
  `frames`, `parent` and `top` as the window (a window with no frames is
  all four), so `self.fetch` records `MISSING global fetch`.
- **A prefixed CSS name (`-webkit-…`) is not counted.** Every engine
  passes over every other engine's dialect, and counting them would bury
  the standard names a page needs under the prefixes it ships for one
  browser.
- **Which CSS functions count as missing** is decided against
  `kFunctionsRead` in `cascade.c`, the function names some reader there
  accepts. A dropped declaration naming only those was written wrong,
  which is the page's fault and not a gap.
- **The tree and the cascade are looked at on each write**, at arrival and
  at departure, as whole tallies that keep the larger count, so looking
  twice does not count twice. A sheet that came and went between the two
  is not seen. `iframe` is not on the elements list: libpage makes it a
  link.
- **The FAILED vocabulary**: `page` (reading or fetching it, or a rule
  the script binding keeps found broken: `FAILED page: an attribute left
  its element outside libdom`, libdom's `options.broken`), `layout`
  (out of memory, or the layout stopped partway), `style` (a cascade of
  the page's sheets and `style` attributes came up short, out of room or
  nested too deep, so part of its styles were not applied: a condition,
  one line however many of the page's cascades meet it, a staged
  rebuild's and a measurement's among them — but for a measurement of
  the stream's parse so far, which has no record of its own yet, so the
  page's own cascade at arrival says it if it is still short),
  `controls`, `scripts`
  (no memory for the host), `script <source name>`, `link refused`,
  `form refused`, `navigation`, and `cookies` (`FAILED cookies: left out
  of a request to <host> (N)`: a request of the page, its own or one it
  made, carried fewer of the jar's cookies than matched it, for want of
  room in the Cookie line; one line a host, and N is the most one request
  left out, not how many requests did, as a whole tally's MISSING count
  is the most it has seen. A load or a fetch names only the host of its
  hop that left out the most, so another host its redirects passed
  through that also left some out gets no line from it). A person's own form left invalid, a
  control the page disabled and a reset are libpage answers rather than
  failures, and stay off the record. An event or a timer task has no
  source name, so it gets a script line only when it fails.
- **A table holds 512 distinct lines.** Past that, the rest are counted on
  a plain line (`missing names not kept: 3`) in words that are neither
  token — each line once, however often it is asked for (a condition
  looked at each layout, a section written again whole), until room is
  found for it. Past 4,096 such lines, or with no memory to keep one's
  key, a line cannot be told from one counted already, so the count stands
  and the line says `, and more not counted`; a line that could not even
  be named for want of memory is said on its own line (`some lines not
  kept: no memory to name them`). A plain line whose latest value had no
  memory to keep says `no memory to write this line` rather than standing
  empty or keeping the value before it.
- **A failed write is said once a run**, in the log and on the status
  line. A `diagnostics` that is not a full path is logged and ignored.

### Proof

| Check | Result |
|---|---|
| libdom host (`tools/test_dom_host.sh`): eleven lookup forms, each heard or silent as it should be, `'fetch' in window` false as the oracle | 3,120 target-core and 17,766 sanitized checks, zero failures |
| `tools/test_js_prepare.py`: the series, line shifts, three context-drift refusals | 5 tests pass |
| `tools/test_js_engine_host.sh` | pass |
| libgarb host (`tools/test_garb_host.sh`): `tools/garb_corpus/skips.txt`, the cascade corpus unchanged, every allocation sweep | 5 skips pages and 18 cascade pages, zero failures |
| Scripted-page host (`tools/test_yonder_scripts_host.sh`): the brief's cases, the token scan across every file the cases wrote (a token only at the head of its own records, or on the verdict after a nonzero count), the setting absent and the parent missing | 2,526 checks, zero failures; the heap ends empty |
| Mutants (`tools/test_yonder_diag_mutants.py`): the brief's three and fifteen more | 18 caught, none missed. The first run missed a token leaking into a fact's value; the address that spells both tokens was written for it. Round one added the token in record data, the verdict naming an empty token, and the two `css-value` shapes |
| Guest (QEMU, ext2 root, desktop): `/tests/pages/diag.html`, Reload, files read host-side with `debugfs` | Badge `MISSING 7  FAILED 2`; the page reads `fetch: undefined, in window: false`; the arrival file has seven MISSING lines and one FAILED; the departure write adds the timer's FAILED; a second boot's first page is `-0002`, so the first run's `-0001` is kept. Not a P5 run |

### The page kept, and what a replay needs (Modern Web slice 0b, 2026-10-21)

REPLAY.md is the contract a kept page and a replay are held to, and
MODERN_WEB.md § Slice 0b, as built, says what was built against it; this
is what it added to the page file and beside it.

**`keep_pages = on`** in yonder.conf, beside `diagnostics =` (it keeps
nothing without it, and says so in the log), keeps each page next to its
file `<name>.txt`:

- `<name>.html`: the bytes as the parser was fed them, after the
  transfer's decoding and before the characters'. They are appended to a
  temporary file as the stream takes them and renamed into place when the
  parse has had them all, so a page let go before then keeps nothing.
- `<name>.d/`: each script and sheet the page fetched, as the window
  received it, in the order each came back: `<n>.js` (a script's decoded
  source), `<n>.css` (the bytes a sheet was parsed from). A fetch that
  failed is a line with no file.
- `<name>.d/index`, written whole again with each new line. Its first line
  is `# yonder kept index 2`; more `#` lines say what the rest hold; every
  other line is `<n> TAB <kind> TAB` what that kind carries, numbered 1 to
  N in the order kept, a script's and a sheet's being `<url> TAB
  came|failed TAB <final> TAB <label> TAB <job>`, `<job>` the job's own
  identity (a script's token, a sheet's place in its page's table), so a
  replay gives two jobs of one address each its own outcome. The index
  grows by kind (a `fetch` line, a `storage` image), and the version moves
  when a shape that exists changes (2 added `<job>`). Fonts and pictures are not
  kept, and the header says so.

**Each name is claimed as the load begins** (`yonder_diag_claim`): the
record's file is made where nothing is (os64_open's `x`), no
`<name>.html` and no `<name>.d` may be there already, kept or not, and
with `keep_pages` the folder is then made; a number that fails any of
these is passed over for the next. So two yonders writing one directory,
a directory whose `.txt` was deleted and whose kept files were not, or a
keep-off load landing on a kept page's number, never pair a record with
another's kept files, and the bytes are never renamed over a `.html`
already there. A folder that kept nothing is removed when the record is
let go. The claim is what every write under the name takes
(`yonder_diag_claim_t`), so a record that could make none of 64 fresh
names has no claim: nothing of it is written, it is said once a run on
the status line and in the log, and no file of anyone else's is written
over. The claim writes a random nonce into its placeholder (`verdict:
unwritten <nonce>`) and every record it writes after (`claim: <nonce>`),
and the record's write proves the claimed file is still its own, by that
nonce, before replacing it: one gone, or claimed by another yonder after
it went, is not written over. A placeholder not known to be whole on
disk (no memory for it, a write or a close that failed) is therefore no
claim, since its name is not known to be this claim's: its file is
removed, or left as a file no reader takes for a page when the unlink
cannot remove it, and its number passed over.

A step of keeping that fails, or a fresh name whose folder cannot be
made, ends the keeping of that page,
is said once a run on the status line and in the log, and leaves the
plain line `kept: not whole: a step of keeping the page failed` on the
record. The scripts and
sheets kept before it stay, and so do the page's bytes if they were
already whole.

**A replay does not reproduce what a fetch it never performs decided**:
pictures and fonts are not kept and never fetched in a replay. So yonder
writes those outcomes under kinds of their own, MISSING `image` (a fetched
picture's format) and FAILED `picture-fetch` and `font-fetch` (what a
fetch, or the bytes it brought, came to); a replay's record names those
kinds on a `REPLAY not reproduced:` line, and their lines are judged on
neither side of a comparison (`tools/yonder_corpus_diff.py`). A picture or
a font decided without a fetch (`MISSING picture-scheme`, `FAILED
picture` for an address refused or never asked for, `MISSING font`) is
reproduced and compared.

**What a record is when it cannot be judged.** A claimed name holds
`address: ...` and `verdict: unwritten` until the record replaces it, so a
yonder that dies after claiming leaves a file no reader judges or counts
(the census tally names it as damage), never a page that met nothing. And
a record that lost ANY line (no room in its tables, no memory to keep a
line or a value) says so with one marker, `REPLAY unjudged: lines lost`,
beside the counts of what went; a replay and the corpus diff take it as
unjudged, since the line it lost may be the one a change made, while the
tally counts the lines it kept. A record whose window changed under its
load says so the same way, `REPLAY unjudged: settings changed during the
load`: a setting it records that is not what the claim wrote when the
record is written (the agent, scripts on or off, the modern switch, zoom,
appearance, motion, animations, the script time limit), or a page laid
out in another view than the claim's. A replay cannot read one page in
two windows. A view that moves after the page arrived is not marked: it
also moves when yonder's own bars come and go, which a replay reproduces.
A page kept whole says `kept: whole`, the kept page a replay replays (one
of version 1, recorded before that fact, it assumes kept whole when its
bytes are beside it, and says so), and every record says `bytes: N`, what
the parser was fed, and every kept page's says `kept lines: N`, from 0
at its claim, how many lines its index holds: on the host nothing else
binds a record to its kept files (REPLAY.md § 3 states that as the rule,
and DECLINED.md's row "Kept bytes bound to their record by length" why
no digest binds them by content).

**The facts a replay rebuilds the head from**, on every page's record now:
`final address` (written when the parse ends, so a page let go unshown
has one too), `body` (`html`, `text` or `image`: what libway judged the
body to be), and for a page from the network `content-type` and `charset`
as the server named them (empty when it named none) and `encrypted`
(`yes`/`no`), and its `status` (`0` when its protocol has none). And the
page's other inputs, written at the claim as in force for the load,
first: `agent` (the User-Agent the window's requests carry), `clock` (the wall clock
in epoch milliseconds, which a script's `Date` starts from) and `tz` (the
zone it reads local time in: the `TZ` in force, or `<n>m`, the machine's
configured minutes east, when there is none); and, at the head, before
the parser is fed a byte, `cookies` (the jar's `document.cookie` string
then, for the final address and the transport it arrived over, with
every Set-Cookie of the head's own hops heard: libway's script door, so
never an HttpOnly cookie; one past 1 MiB, or with no memory to hold it,
is not written, and the record says it lost lines; a load that never had
a head has none). The jar can move after the head and before a script
reads it (a script's own `src` response, a sheet a script waits for, the
page still on screen, an expiry), and a replay sees none of that
(REPLAY.md § 3's jar row). A record that says any of
those four is of VERSION 2 (REPLAY.md § 3), and a replay requires all of
them; a kept page recorded before them is of version 1, and a replay of
it takes its cookies and clock from its harness, and its agent, zone and
any window input its record does not say from what the person running
it states (`--window`), saying which on its record (REPLAY.md § 3). Then
the window
it was read in: `modern` (`on`/`off`), `laid out at` (the view's `<width>x<height> px`,
again at arrival with ` in <n> ms`), `zoom` (per mille), `appearance`
(`light`/`dark`), `motion` (`full`/`reduce`), `animate` (`on`/`off`: whether
animations run),
`scripts` (`on`/`off`) and
`script seconds`. These, `TLS`, `kept` and `kept lines` have room of their
own (24 lines, counted apart from the 512 the other plain lines may take:
`yonder_diag_head_fact`), so
a page with more plain lines than that never loses one of these; it does
lose the others, and is unjudged. A replay requires every one of them its
page needs (a file's page has no status, type, charset or encryption)
rather than assume any, reads the page in the window they say, and reads
every line of the record by one grammar
(`tools/yonder_record_grammar.txt` holds its vectors). Every record
yonder writes ends with the line `end`, which both readers require of a
replay's record and of a kept page's record of version 2 (one that says
`kept:` or `kept lines:`); a kept page of version 1 may lack it, and its
`kept: whole`, written last, stands for it.

**A script's line carries its evaluation time**: `script <address>: ran in
1520 us`, or `failed in ...`, the time `os64_js_run` took, compiling
included — or reading the script's kept bytecode instead, when it says
`ran in 80 us from its bytecode` (BYTECODE_CACHE.md). An event's or a
timer's task, and a script that never reached the engine (its fetch
failed), keep the plain `ran`/`failed`. QEMU's numbers are not a speed; the
P5's are.

**What a page's requests and modules cost** (2026-10-30, the measurement
BYTECODE_CACHE.md § 9's 7b was asked for): a script's request's line says
the body's bytes and the fetch's time, from its job's start to its answer —
`request GET <address>: 200 cors, 0 hops, 6718 bytes in 359 ms` — and the
page's module compiles are one line together, `modules compiled: 207 in
3167557 us, 8784400 bytes`, rewritten as each compiles (one line a module
would crowd a page of hundreds out of the record's facts). A module read
from its kept bytecode counts among them with its read's time, and the
line ends `, N from bytecode` when any was. A module the engine's own
loader compiles, a dynamic import's, is not in it. A request
the HTTP cache answered says so after its time (SCRIPT_CACHE.md): `, kept`
when it was served with nothing asked, `, confirmed` after a 304.

**Where an arrival went** (2026-10-30): beside `arrived after`, a
`navigation:` line gives performance.timing's milestones, each after the
navigation began — `navigation: first byte 410 ms, last byte 1630 ms,
parsed 2050 ms, DOMContentLoaded 6900 ms, load 7300 ms`: the document's
first and last bytes, its parse done, DOMContentLoaded (after the deferred
scripts and the module graphs have run), and load's end; `-` for one the
page never reached. It is how a page whose module costs fell to nothing
showed that its time was spent elsewhere. A request's line says, too, when
either is a millisecond or more, how long its job waited for a worker
(`, queued 840 ms`) and how long its answer then waited for the window
(`, heard 300 ms later`): time a request spent not being fetched.

**`yonder --exit-when-loaded <address>`** is the measuring run: yonder
leaves once the shown page has settled (streamed, its pictures and fonts
in, its record not owed, no script waiting) and no job has come back for
3 s, or after 2 minutes whatever the page is doing, and says which on its
way out. Leaving is the ordinary way out, so the page's record is written
whole. Started over ssh it opens its window on the machine's desktop as any
yonder does, and the shell has its prompt back when the record is written.

## Batch 1: what the first census taught the instrument

Chris's first P5 census (google.com, danlegt.com, theoldnet.com and its
web.archive.org pages, suckless.org, man.openbsd.org, doc.cat-v.org,
duckduckgo.com, github.com; tallied in MODERN_WEB_CENSUS.md, bugs in
YONDER_BUGS.md) found three things a page asked for that the record could
not see, and three ways it told less than it could. A clean verdict is
now worth trusting.

- **A value read and laid out as something else is recorded.** `float`
  and `clear` join props.c's approximations (`MISSING css-value float:
  left`): libgarb reads them and libflow lays a float out in the flow, so
  suckless.org's sidebar page read clean while its text sat a page down.
  They leave the list the day floats are laid out. `@supports
  (float: left)` answers false meanwhile, which is what the list means. A
  float an HTML attribute asks for (`<img align=left>`) is not counted:
  it never reaches the cascade.
- **An HTTP error page is a failed load.** Every page from the network has
  a `status:` line, and one at 400 or above is `FAILED page: HTTP 429 Too
  Many Requests`; web.archive.org's rate-limit page read clean.
- **A picture in a format yonder does not decode is `MISSING image
  <format>`**, named by its first bytes (`yonder_diag_image_format`:
  `svg`, past an XML prolog, `webp`, `avif`, `ico`, `tiff`, or
  `unknown`), with its address on a plain line. A picture that failed
  any other way is `FAILED picture-fetch` when its fetch decided it, and
  `FAILED picture` when it was never fetched (Batch 3; § The page kept).
- **A script is recorded by its whole address.** The engine's source name
  stops at OS64_JS_SOURCE_NAME_CAP (128); the host keeps each `src`
  script's address and the record names the task's script by it
  (`yonder_scripts_task_address`).
- **An old IE hack is a dialect**, like a prefixed name: a `css-value`
  word holding a control character (`display: none\9`) is not counted.
- **"not a function" names the method** (libjs patch 0009): a method a
  call fetched and found not callable is remembered, and a call reads
  `'bogus' is not a function` only when it is proved to be that fetch's
  call: same frame, slot and value, nothing but straight-line bytecode
  between the fetch and the call, and the fetched value never popped on
  the way. `o.bogus()` and `document.foo(1, el)` are named; a call whose
  arguments call or branch keeps the plain message, so a name is never
  told of another call (Quinn, #239, three rounds). A page sees the longer message, and nothing else changes.
- **`bytes:` was checked** after a P5 file matched the previous page's
  size exactly: two navigations in one window count their own bodies, and
  a case now guards it. The P5's match was most likely chance (the page
  varies around that size).

## Batch 2: what the census asked for most

The second census pass (danlegt.com, warpbox.dev, google.com again) put
three things at the top of the FAILED lines. None was new to the record;
each was a page dying on something that browsers have had since the 2000s.

- **An address is as long as 8 KiB** (url.h's OS64_URL_REF_MAX, the
  request line Apache and nginx take). The path, a resolved reference,
  libdom's OS64_DOM_URL_MAX, the request a fetch writes and the response
  line a `Location:` arrives on all follow it, so Google's 3,666-byte
  `/xjs/` bundle is fetched, and its record line names it whole.
- **`console` is the Console Standard's whole namespace** (libjs
  CONTRACT.md § Capabilities), not `log` alone: danlegt.com's
  `stamps.js` died on `console.assert`. Its lines go where `log`'s
  always went, the browser's standard output.
- **`querySelector` and `querySelectorAll`** (LIBDOM.md § Objects and
  collections), matched by libgarb's own selectors: they were ten of
  warpbox.dev's eleven failures and three of danlegt.com's seven. A
  selector that does not parse throws a SyntaxError naming it, so the
  record does too.
- **The rest of the classic DOM pages assume** (LIBDOM.md § The classic
  methods), which a census would only have found one script death at a
  time: `getElementsByClassName`/`ByName`, `matches`/`closest`,
  `append`/`prepend`/`before`/`after`/`replaceWith`/`remove`/
  `replaceChildren`, `contains`/`isConnected`, `insertAdjacent*`,
  `classList`, `dataset`, the attribute conveniences, and the reflected
  `hidden`, `lang`, `dir`, `tabIndex`, a script's `src` (wiki.osdev.org's
  Cloudflare loader set it and connected an empty script) and an anchor's
  and a link's `href`.

## Batch 3: what the census showed, and what a page needs to be read

- **An invisible control that is no field is not drawn** (Chris's ruling,
  POSITION.md ruling 6 amended): MediaWiki's menu checkbox, once #246 gave
  it its true size, was drawn as a band across every Vector page. Its
  widget stays and paints nothing (`unpainted_class`), so a click, its
  focus and Space still tick it, with the events a script listens for.
- **A frame stands in for its document**: an iframe or a frame is drawn as
  a pale panel naming where its document is, and a click there follows it
  (it always did; libpage makes a frame a link). The record counts
  `MISSING element iframe`, `frame` and `embed`; an object shows its
  fallback and is not counted. A frame a script inserts is the same
  stand-in, its `src` set as a property too since the frames row
  (LIBDOM.md § The frames row); a frame is never unsupported. A canvas
  stands in where scripting is on, "Canvas: not supported" in its corner
  inside a thin outline, with no panel: what is under a canvas shows.
- **A picture asked for by itself is shown** on a page of its own
  (YONDER.md § A picture asked for by itself), where it said "not a page".
- **`document.cookie`** (LIBDOM.md § The classic methods): MediaWiki's first
  inline script died reading it, and took the wiki's module loader with it.
- **Every picture that was not shown is named by its address**, and the
  file is written again when the page's last picture is in. Chris found
  that danlegt's status line said 81 of 82 while its record said
  `pictures: 82, 0 shown`: that was the arrival write, from before any
  picture had come, and the failed picture had no line naming it. Now a
  picture that would not fetch, came with an error, or would not decode
  is `FAILED picture-fetch: <address>: <why>`. The worker writes the why
  (`yonder_picture_t.why`): `HTTP 404`, `did not fetch (tls-failed)`,
  `could not be read`, `would not decode (malformed image)`. An error's
  body is no longer decoded as the picture: an HTML 404 page used to be
  counted as `MISSING image unknown`. A format yonder does not decode
  stays `MISSING image <format>`, with a plain line `picture <address>:
  svg, which yonder does not decode`. One never handed to a worker is
  `FAILED picture` too (`no worker to fetch it`, `no memory to ask for
  it`, `the workers would not take it`), and one past the memory a page's
  pictures may keep is named on a plain line, since that is a limit kept
  and not a failure. A body yonder had no memory to hold reads `did not
  fetch (no-memory)`, never `(ok)`. A picture the page or its sheets name
  that never reaches the table is on the record too, ONCE however often a
  rebuild or a layout meets it (a set of them, `unasked_first`): one in a
  scheme nothing here fetches is `MISSING picture-scheme data`, counted,
  and an address that was refused is `FAILED picture` as written, with
  the refusal. Met with no picture out, it writes the record then.
  However the pictures settle, if one moved the page the record waits
  until the page is laid out again (`diag_pictures_in` leaves it
  `record_owed`, `pictures_settle` writes it), so it says what that
  layout came to. The
  `pictures:` line counts those (`not asked for`) and the ones a page left
  before they arrived (`still coming`). A layout that stopped partway is a
  condition, so it is one `FAILED layout` however often the file is
  written (`yonder_diag_failed_seen`). suckless.org's logo is an SVG; that
  is its "1 of 2".
