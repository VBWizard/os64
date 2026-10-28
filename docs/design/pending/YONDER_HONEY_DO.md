# YONDER_HONEY_DO.md — the batch between the arcs

*Chris's list, 2026-10-19, sorted by Fable the same morning with his
rulings folded in. Everything here is yonder 1.0: a bug, a missing verb, or
a piece of the window, and none of it is web-platform surface, so none of
it waits for [MODERN_WEB.md](MODERN_WEB.md). Opus builds it, starting now
and beside the perf slice's stragglers (YONDER_PERF.md § Then measure
again). Each row carries enough design direction to start from this file;
where Fable holds the design, the row says what is settled and what is
still to be written. Strike a row when it ships, with the PR.*

## The rules it runs under

- A userland-only item is its builder's to design and build (Chris,
  2026-10-11); the two kernel items are Fable's design.
- Each item is its own small PR to `userland`, Chris tests before it is
  committed, a Codex round is his call.
- Anything that writes a file publishes by writing a temp and renaming
  over, the config writer's rule, so a crash never leaves half a file
  wearing a real name.
- A setting name is compared without case; a value is verbatim; a key that
  is data (an address, a name) is verbatim too. CLAUDE.md § The config
  search path has the argument.

## Where yonder's files live (ruled 2026-10-19)

The batch gives yonder its first files that are the PERSON's rather than
the system's: favorites, history, cookies, and the downloads. Chris's
ruling: they live together, apart from everything else, under a folder he
can find, and the path is configurable. So:

- **`profile = <dir>`** in `yonder.conf` names the folder; absent, it is
  **`/home/yogi/.yonder`**. Chris has already made `/home/yogi` against the
  day os64 has logins, and a per-person folder under it is the shape that
  day will want. yonder creates the folder, and any missing parent, on first
  use, and says on the status line if it cannot. The word is Netscape's
  (1997, Communicator 4, when one machine began to hold more than one
  person's bookmarks), and it is what every browser since has called this
  folder.
- Inside it: `favorites`, `history`, `cookies`, and from the arc's slice 1a,
  `storage/<origin>` for the page storage APIs (a file per origin, built:
  MODERN_WEB.md § Slice 1a, as built). Each is named by its
  purpose, with no extension, as `/home/husk.rc` and the conf files are.
- **`downloads = <dir>`** is a second setting, default **`/home/yogi/downloads`**
  (the profile's sibling, not its child): a download is something the person
  asked for and will look for, and a dot-folder is where a program keeps its
  own memory. Same create-on-first-use rule.
- The cache stays where CACHE.md put it (`/var/cache`): it is the system's
  own and disposable, by the persistence doctrine, and nothing in it is the
  person's.
- Every window of yonder reads and writes the same files, which is what
  lets "open in new window" share your logins and your history. Two windows
  writing one file is the temp-and-rename rule's job; the LAST writer's
  whole file wins, which is right for favorites and history (each window
  appends to its own read copy and writes the merge) and is settled
  per-file in the rows below.

## The list, in order

Cheapest-with-visible-effect first. The order is a lean, not a ruling;
Chris moves rows.

| # | Item | Owner | Status |
|---|---|---|---|
| 1 | Links underlined | Opus | open |
| 2 | jQuery dies at load | Opus | open |
| 3 | A menu bar, with Favorites and History on it | Opus | open |
| 4 | ~~Persistent cookies, with a switch~~ | Opus | done (the login slice; § 4 has the as-built) |
| 5 | The right-click menu | Opus; Quinn (the save dialog) | open |
| 6 | Download | Opus | open |
| 7 | Selecting and copying text, and the I-beam | Fable design; Opus builds | open |
| 8 | The mouse wheel through vncd | Fable reads; Opus fixes | open |

### 1. Links underlined

The painter already draws underlines (`paint.c`, `FLOW_DECORATION_UNDERLINE`);
the default stylesheet never asks for one on a link. One rule,
`a[href] { text-decoration: underline }`, beside the link colour it already
sets, and `a[href] img` keeps no underline (the browsers' sheets agree).
A page's own `text-decoration: none` wins as it always did. Proof: the
libgarb corpus gains the rule; a yonder host case shows the decoration on a
link and not on a nameless anchor.

### 2. jQuery dies at load

On www.microsoft.com the page file shows `jquery-1.9.1.min.js` ITSELF
failing with `TypeError: cannot read property of undefined`, and every later
"jQuery is not defined" on that page, and on the others, is that one death
echoing. jQuery 1.9 (2013) touches only the classic DOM at load, in its
`support` probe: `createElement`, `innerHTML` with a mixed fragment,
`getElementsByTagName` on an element, `firstChild.nodeType`,
`element.style.cssText`, `cloneNode(true)`, `setAttribute("type", ...)`,
`createDocumentFragment`. One or two of those is missing or returns the
wrong thing. Direction: run that exact file under the headless binding
(`tools/test_yonder_scripts_host.c` runs page scripts on the host), read
the stack the engine gives, add the member WHOLE to libdom with its WPT
cases, and re-run the P5's page files for microsoft.com through the corpus
count. Do not special-case jQuery; the member it wants is one a thousand
pages want.

### 3. A menu bar, with Favorites and History

**Direction.** The menu is the WINDOW's, not the page's: a bar under the
address row, drawn with Quinn's popup primitives from the explorer arc
(`os64/ui_menu.h`; GUI_POPUPS.md under `docs/design/pending/explorer/` is
the design), with three menus to start: **Favorites**, **History**,
**Settings** (the existing settings window gains a menu entry and loses
nothing). Keyboard: the bar opens on Alt as the explorer's does; the menus
are what the person sees, the files are the truth.

**Favorites** is `<profile>/favorites`: one entry per line, the address
first and the name after it, separated by a tab, so scribe can edit it and
a line with no name shows its address. The menu lists it in file order
(the person's order, not ours), with "Add this page" at the top, which
appends. Netscape's 1994 `bookmarks.html` is the interchange format the
world still imports; an export to it is a later verb, not this slice.

**History** is `<profile>/history`: the visited list, NOT the back button
(libway already keeps back-and-forward per window). One line per visit,
newest last: the time (seconds since the epoch, as `date` prints it), the
address, the title. Appended at each page shown, read at start, capped at
a line count (`history_lines`, default 10,000) by dropping the oldest at
the next write. The menu shows the last twenty, newest first, and a
"Clear history" verb that empties the file. Two windows append to one
file; the temp-and-rename rule means each append is read-merge-write, and
a visit lost to a race is a visit, not a login, so no lock is built for it.

### 4. Persistent cookies, with a switch

**Direction.** The jar (`libway/jar.c`, YONDER.md § Y3b) lives in memory
today. It is written to `<profile>/cookies` on every change and read at
start, in **Netscape's `cookies.txt` format** (1994; tab-separated: domain,
include-subdomains, path, secure, expiry, name, value, with the
`#HttpOnly_` prefix curl and wget use), because that is the one cookie
file every tool on earth reads and writes, and interop is the only reason
os64 ever adopts a format it did not design. Session cookies (no expiry)
are never written. The switch is **`cookies = on | session | off`** in
`yonder.conf`: `on` is the default and keeps the file; `session` keeps
cookies in memory only, as today; `off` sends none, stores none, and
empties the file. A "Clear cookies" verb sits in Settings beside it
(YONDER.md booked both). The file is a file of logins, so it lives under
the profile and nowhere else, and the day the filesystem has modes it is
written closed. Two windows share one file; the write is the merged jar
after each change, read-merge-write under the temp-and-rename rule, and
the merge keeps the newest cookie of a name per domain and path.
Promoted from Chris's "less necessary" because a second window must
share the jar, and a shared jar is a file.

**Status: DONE, 2026-10-25 (the login slice, `opus/yonder-login`).** Its
trigger was reached the day Chris first signed in through yonder and
asked to stay signed in. As built:

- **The format is libway's** (`way/jar.h` § Kept across restarts): the
  jar does no I/O and is handed a file's text and asked for its own.
  Seven tab-separated fields as curl writes them, a leading dot and `TRUE`
  for a domain cookie, `#HttpOnly_` before an HttpOnly one's domain, and
  three comment lines on top, the first Netscape's own and the other two
  saying the file is one of logins and that Clear cookies empties it.
  SameSite has no field, so a cookie read from the file has none; the jar
  does not judge by it yet (YONDER.md's booked row). A cookie whose line
  the reader would not take back as the same cookie (a name, value,
  domain or path holding a byte that would break its line; a domain
  cookie on a single label, as an intranet host can set) is not written,
  and lasts as long as the window: the writer and a window following the
  file ask the same question (jar.c's `round_trips`), so following never
  lets go of a cookie for want of a line it could not have.
- **Session cookies are never written; expired ones are never written nor
  read**, and curl's session lines (expiry 0) are read as what they say, a
  session that has ended.
- **Reading is hostile-input reading**: a line past `WAY_JAR_LINE_MAX`,
  one with a NUL, one without exactly seven fields, flags that are not
  `TRUE`/`FALSE`, an expiry that is not a run of at most eighteen digits,
  a path that does not begin with `/`, an empty or doubled-dot domain, and
  every rule a Set-Cookie is held to (`judged()`, shared with the
  Set-Cookie door: sizes, control bytes, the `__Host-` and `__Secure-`
  prefixes, the domain rules) refuse the line, which is skipped and
  counted. A domain cookie on a single label is refused too: online only
  that host could have set it, and a file cannot say which host did. The
  jar's caps hold (`WAY_COOKIES_PER_DOMAIN`, `WAY_COOKIES_MAX`). A file
  Chris edits by hand is the nutty scenario; the reader is memory-safe
  under ASan with every fragment of a line and every allocation failed in
  turn, and asks nothing more of him.
- **The switch** is `cookies = on | session | off`, read by
  `yonder_settings_saved_cookies` (anything else is `on`, said on the
  status line), and Settings shows it as two boxes, Accept cookies and
  Keep them when yonder closes, with **Clear cookies** at the row's right.
  `off` is the jar refusing (`way_jar_refuse`: nothing heard, nothing
  sent), emptied, and the file emptied; `session` is the jar never
  written, and the file emptied as yonder leaves and as a `session`
  yonder starts — Firefox's "delete cookies when closed", so the box ticked
  again before then loses nothing, and a crash cannot leave the logins
  behind; Clear cookies empties the jar and the file in every mode, and
  the temps a dead window left beside it. The file lives in the profile
  folder and nowhere else (`yonder_cookies_new` takes the profile's path).
- **Two windows, one file**, as the row asked: every change to a cookie a
  file keeps (one with an expiry set, one set in such a cookie's place,
  one let go) is numbered in the jar, and a write reads the file into a
  jar of its own, lays this window's changes over it (`way_jar_overlay`),
  and publishes by a per-writer temp and a rename, the temp named and swept
  as page storage's are (the three helpers are storage.c's, shared). A
  write that another window's overtook between the read and the rename is
  read and merged again, up to three reads. After a write, and before
  every navigation when the file's size or time says another window
  changed it, the window FOLLOWS the file (`way_jar_follow`): it takes what
  the file holds and lets go the kept cookies the file no longer names, so
  a sign-in or a sign-out in one window is the other's at its next page. A
  change this window has not yet written outranks what the file says of
  the same cookie (name, domain, path and host-only flag), which is "the
  newest of a name per domain and path" told by order of change rather
  than by a clock the file has no field for. What cannot be told: two
  windows that change one cookie and write in the same instant, where the
  later rename wins; and two writes of one size within one second of the
  file clock, which the next change shows.
- **Writes are coalesced at the loop's turn**: a change makes a write due
  at once if none has been made in the last second, else a second after
  the last one (`YONDER_COOKIES_WRITE_MS`), and the ticker is handed that
  moment with the page's timers; a write the disk refused is tried again
  ten seconds on, and said once on the status line. A page setting
  cookies in a loop costs a write a second, not one a cookie (the host case
  sets a thousand inside a second and counts one write). What is owed when
  yonder closes is written as it leaves. A crash loses at most the last
  second's changes, or the last ten seconds' after a write that failed.
- docs/yonder.md tells Chris where the file is, that it is one file for
  every site, what it is a file of, and how to empty it.

### 5. The right-click menu

**Direction.** One popup over the page, from the same primitives as the
bar, opened on the right button over a link or a picture (`element_at`
already answers what is under the pointer; the menu's verbs are chosen by
what it answers: a link, a picture, a picture inside a link, or neither,
and over neither the menu does not open this slice). Five verbs:

- **Open in new window**: spawn `yonder <address>`. The new window shares
  the profile's files by the rule above. New window beats tabs for now;
  § New window before tabs.
- **Copy link** and **Copy image address**: a write of the absolute
  address to `/sys/clipboard`, the system's one clipboard
  (`docs/design/completed/CLIPBOARD.md`; libos64's `os64/clip.h` is the
  function form), so a gterm can paste it.
- **Save picture as** and **Save link as**: the download in § 6, through
  Quinn's save dialog, which she builds today on `os64/ui_browser.h`,
  Roam's directory browser made reusable for exactly this. The dialog
  opens on the `downloads` folder with the proposed name filled in.

### 6. Download

**Direction.** The stream libfetch already runs for pages and pictures,
aimed at a file instead of the parser. Destination the `downloads` setting,
or wherever the save dialog said. Name from `Content-Disposition` when the
server sends one (its `filename=`, with any path stripped and anything the
filesystem refuses replaced), and the last segment of the address otherwise,
`download` when there is none. A name already taken gets a numbered suffix
before its extension, never an overwrite. The bytes go to `<name>.part` in
the same folder and are renamed at the end; progress (bytes so far, and of
how many when the length is known) and the final size on the status line;
a failure leaves no `.part` behind and says why. The download runs on the
page's loop like a picture does, so the window keeps answering.

**No automatic download** (Chris, 2026-10-19). A left-click on something
yonder cannot show keeps today's behaviour, and its message changes from
"use os64get" to "right-click the link and choose Save link as", because a
message should name the door that exists.

### 7. Selecting and copying text, and the I-beam

Two halves, and Fable holds both designs. What is settled now, so Opus can
read it against the code before the design document exists:

**The selection model (yonder, libflow).** A selection is two positions in
the document's TEXT ORDER (a text box and an offset into its run), an
anchor where the button went down and a focus that follows the pointer,
never a pixel rectangle: that is what makes it survive a relayout, a
resize, a zoom and a scroll, and what makes the copied text come out in
reading order. `flow_hit` and the runs' own hit tests give the position
under the pointer; between the two positions, every text box in tree order
is selected whole, and the two end boxes partly. The painter draws the
selection as a layer AFTER the page (one rectangle per selected run, the
selection colour blended under the glyphs, which is what every browser
draws), so no paint-cache entry changes and the perf slice's work is
untouched. Copy (Ctrl+C, and a "Copy" verb on the right-click menu when
there is a selection) writes the selected text to `/sys/clipboard` in
document order, with a newline where a block boundary falls and a space
where inline boxes were separated by one. A click without a drag clears
it; so does showing another page. Pictures and controls inside the range
are skipped in the copy and drawn selected. What the design document
still has to settle: selection across the transformed and scrolled boxes
of Pile 3, the double-click (word) and triple-click (paragraph) steps, and
whether selection reaches into a `<select>` or a field (it does not, in
this slice).

**The pointer shape (kernel, ABI).** The compositor draws one fixed arrow
today (YONDER.md booked the hand pointer for this reason). The design is
a per-WINDOW shape, not per-region: a call for a window to name its
pointer (`arrow`, `ibeam`, `hand` to start, as an enum in the ABI header),
the compositor drawing the shape named by the window under the pointer,
and the arrow when it is over no window or a window that never asked. The
art lives in the kernel beside the arrow's. yonder then changes its shape
on every pointer move from its own hit test: the I-beam over text, the
hand over a link, the arrow elsewhere. Per-window is the whole cost of
per-region's benefit, because the program under the pointer already knows
what is there. Fable writes this half; Opus builds the yonder half with
the arrow in the meantime, since the drag and the copy do not need the
I-beam to exist.

### 8. The mouse wheel through vncd

Not yonder. The VNC pointer message carries the wheel as buttons 4 and 5
(up and down), each as a press-and-release pair, and vncd must turn each
press into the desktop's wheel event the way it already turns buttons 1
to 3 into clicks. Fable reads `vncd` and the desktop's wheel path to say
whether vncd drops them or the desktop does; the fix is small either way,
and Opus makes it.

## New window before tabs

yonder already opens as many windows as it is launched, so "open in new
window" is a spawn with the address, and it rides on the right-click menu
that four other verbs need anyway. Tabs are a real design: one window, N
pages, one visible, each with its own script runtime and loop, and a tab
strip in the chrome. They are worth doing once, properly, after the arc's
pages make a single window heavy enough that N windows is the wrong
answer. The one ruling that falls out of new-window TODAY is that the
cookie jar, the favorites and the history are shared between windows,
which is why they are files under the profile.

## Also in the batch, from elsewhere

Rows that were already booked and belong with this batch because they are
the same vintage, listed so one PR list covers the between-arcs work:

- The perf slice's stragglers (YONDER_PERF.md § Then measure again, and
  the uncommitted perf-6 record work): the P5 whole-views reading, the
  dirty-rectangle-list decision, "throttle, not freeze", the frame-timing
  switch, the paused-animation test.
- Transitions and `:hover` (PILE3.md § Transitions): the slice after the
  perf slice by Chris's 10/14 word.
- The fonts rows, last by Chris's 10/16 ruling: S9a5, S9b (WOFF2; three
  quarters of the census's web fonts ship WOFF2 only), named families and
  aliases in `fonts.conf`.
- The census's bugs that are ours (YONDER_BUGS.md on the local census
  branch): addresses over 1,024 bytes refused at every layer (Google's
  bundle has a 3,666-byte `src`), a `<select>` laid out as every option at
  once, load failures that live only on the status line instead of an
  error page.

## Added 2026-10-28, from a surf with Fable

Polish and tooling found while reading records beside Chris. None is
platform surface; each is small; none is scheduled.

- **The census should say whose global is missing.** `MISSING global`
  lists every name a script read as a global and found undefined, so
  `requestAnimationFrame` (ours to supply) and `jQuery`, `$`, `RLQ` (the
  page's own, unloaded because a loader died upstream) sit in one list
  with one spelling. The tally knows the platform's globals; a name not
  among them is the page's, and the record can say so, so a reader does
  not have to.
- **An agent preset per site.** A search engine served three tiers to the
  Chrome identity, full, lite, then quietly worse results, and good
  results to yonder's own name, because the Chrome agent with a non-Chrome
  connection reads as automated. The identity preset already carries its
  companion headers (W1); what is missing is choosing it by site, a Chrome
  identity for sites that gate on it and yonder's name for those that
  reward honesty. A settings row keyed by host.
- **A video's poster with a play badge.** DEBTS § Browser layout books
  video as having no stand-in. The polish shape: when a `video` or a
  player's `img` poster is in the markup, draw the poster at the box's
  size with a play badge over it, and nothing plays. A picture of the
  video is what a reader wanted from the page; the badge says why it does
  not move.
- **Split "laid out in".** The status line's number is the arrival's
  model refresh, cascade and layout in one figure; on 2026-10-28 it took
  an afternoon with per-second `/proc` logs to learn that 8 of 11.7 s were
  the heap under the cascade (HEAP_BINS.md). The line and the record
  should carry the three parts, and `/proc/<task>/heap` should carry
  `walk_steps` (the bins design has it).
- **`--script-audit` only as the first argument.** `yonder <address>
  --script-audit` switches nothing on and says nothing; the flag is read
  from `argv[1]` alone. Either take it anywhere or refuse it anywhere
  else, and say which in the help.
- **Placeholder text** (from 10/25): a text field never draws its
  `placeholder` attribute. Cheap, visible on every form.
- **A clickable FAILED badge** (Chris, 10/25): the status bar says one
  failure at a time and the next replaces it; a click on the badge should
  show them all.
- **An `img` with no `src` paints as a picture still coming**: booked in
  DEBTS § Browser layout the same day; listed here so the batch sees it.
