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
- Written when the page arrives, again when its last picture is in, and
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
  registration), an element drawn as nothing (canvas, video, audio, svg,
  a custom element), a CSS property, at-rule or function the cascade
  skipped, a font face not fetched.
- `FAILED <what>: <message>`: a script that died with its message and
  source name, one stopped at the execution limit, out of memory in
  layout or reading, a link refused and why. Everything the status line
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
`@font-face` family. A tally that finds no room never marks the cascade
incomplete.

**A file, as the guest wrote it** (`/tests/pages/diag.html`, left by
Reload, so this is the departure write):

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
    script file:///tests/pages/diag.html#inline-1: failed
    bytes: 967
    final address: file:///tests/pages/diag.html
    arrived after: 271 ms
    laid out at: 828 px in 70 ms
    written: when the page was left
    pictures: 0, 0 shown, 0 could not be read, 0 past the memory kept, 0 still coming, 0 not asked for
    sheets: 1, 1 ready
    script file:///tests/pages/diag.html#timer-1: failed

### Built differently from the brief, and why

- **The sequence continues across runs.** A per-run sequence restarts at
  0001, and the second run into `/home/yonder/pages` would write over the
  first run's census, host for host. The first write of a run starts one
  past the highest `-NNNN.txt` already in the directory, so `ls` still
  lists in browse order across runs. Two yonders running at once share one
  sequence and can collide on a name; that is left alone.
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
  record line of its own.
- **The kinds are** `global`, `script` (`module`), `element`,
  `css-property`, `css-at-rule`, `css-function`, `css-value` and `font`.
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
- **The FAILED vocabulary**: `page` (reading or fetching it), `layout`
  (out of memory, or the layout stopped partway), `controls`, `scripts`
  (no memory for the host), `script <source name>`, `link refused`,
  `form refused` and `navigation`. A person's own form left invalid, a
  control the page disabled and a reset are libpage answers rather than
  failures, and stay off the record. An event or a timer task has no
  source name, so it gets a script line only when it fails.
- **A table holds 512 distinct lines.** Past that, the rest are counted on
  a plain line (`missing names not kept: 3`) in words that are neither
  token.
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
  any other way is `FAILED picture` (Batch 3).
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
  fallback and is not counted.
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
  is `FAILED picture: <address>: <why>`. The worker writes the why
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
  fetch (no-memory)`, never `(ok)`. A picture the page names that never
  reaches the table is on the record too: an address libpage refused is
  `FAILED picture` as written, with the refusal, and one in a scheme
  nothing here fetches is `MISSING picture-scheme data`, counted. The
  `pictures:` line counts those (`not asked for`) and the ones a page left
  before they arrived (`still coming`). A layout that stopped partway is a
  condition, so it is one `FAILED layout` however often the file is
  written (`yonder_diag_failed_seen`). suckless.org's logo is an SVG; that
  is its "1 of 2".
