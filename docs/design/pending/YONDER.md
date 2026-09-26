# YONDER.md — the graphical browser's face

*Written 2026-09-25 by Opus, the evening libflow's six slices were pushed.
The campaign is [BROWSER.md](../../../BROWSER.md), what a page means is
[LIBPAGE.md](../../../LIBPAGE.md), where its boxes go is
[LAYOUT.md](LAYOUT.md), and the gaps are the packets in
[docs/yonder/](../../yonder/README.md). This is rung 2 of yonder's ladder:
a window you can read the web in.*

## What it is

`yonder` is a libui program. It links libhtml, libpage, libflow and libway
(and libfetch through libway) unchanged, and adds what a window needs of
its own: a PAGE VIEW widget that paints a flow tree and scrolls it, the
window around it — a toolbar (back, forward, reload, stop, the address
field) above the view, a question bar and a status line below it — and
the running of its fetches on a work pool (§ Y3).

Nothing about a page's meaning or geometry is decided here. The view asks
libflow where a box is, libpage what a node means, and libway what to do
about a click; it decides only how things LOOK on the glass and how a
person moves around them.

## The window

```
+--------------------------------------------------------------+
| [<] [>] [R]  [ http://68k.news/                            ] |
+--------------------------------------------------------------+
|                                                            |^|
|   the page view                                            | |
|                                                            | |
|                                                            |v|
+--------------------------------------------------------------+
| http://68k.news/article.php?a=...          (status line)     |
+--------------------------------------------------------------+
```

- **The status line is where a link says where it goes.** os64 has no
  pointer shapes (the compositor draws one arrow; there is no syscall to
  change it), so hovering a link changes the status line, not the
  cursor, which is what Mosaic did before anybody had a hand. It also
  carries the sentences: loading, an incomplete page, a refusal.
- **Hover needs yonder's own event loop.** libui hands a widget pointer
  moves only while a button is held, so yonder reads MOUSE_MOVE itself
  before `os64_ui_dispatch` — Scribe's shape for its shortcuts — and asks
  `flow_hit` what is under the pointer.
- **The window's title is the page's `<title>`** if the GUI can retitle a
  window; if it cannot, that is a booked ask, not a workaround.

## The page view

A custom libui class (the container pattern, `os64_ui_class_t`). It owns
the current `flow_tree_t`, a scroll position in page pixels, and the two
scrollbars, which are ordinary `os64_ui_scrollbar_t`s beside it.

**Painting is libflow's walk, drawn through four verbs**
(`userland/apps/yonder/paint.c`). `yonder_paint` takes the viewport in
page coordinates, calls `flow_visit`, and draws each box it is handed
through a small table — `fill(rect, colour)`, `text(box, clip, colour)`,
`image(box, content, clip)`, `control(box, content, clip)` — never
straight to `os64_draw_*`. In the guest the table draws, moving page
coordinates onto the glass; on the host it RECORDS, one line per call, so
the painter gets libflow's kind of proof (`tools/test_yonder_host.sh`): a
recording worked out by hand for each fixture, and a `.paint` regression
beside each corpus page's `.boxes` — its first screen at 800x600 and a
screen from its middle. Every fill is cut to the viewport by the painter,
and the executor cuts to the view's rect again, because libui passes a
paint hook no clip and its primitives clip only to the canvas.

What each kind of box draws:

- **The canvas first.** CSS 2.1 §14.2: the root's background paints the
  whole canvas, and when the root has none, the body's does. This is the
  rule that makes `<body bgcolor>` fill the window rather than the body's
  box, and 1998 depends on it. With neither, `env.paper`.
- **Block, table, caption, row group, row, cell:** the background over
  the border box, then the borders — the styles the struct can hold.
  `solid` is one colour a side; `inset`, `outset` and `groove` are the
  two-tone bevel (each channel halved toward white or toward black, the
  way every browser of the era drew `<table border>`). The sides are drawn
  a row at a time and meet on the diagonals, every pixel drawn once. `hr`
  needs nothing of its own: the Rendering chapter makes it an inset
  border. Rows are painted before their cells because the walk hands them
  over first, which is what makes Hacker News's `<tr bgcolor>` work.
- **Span:** its background, if it has one (`mark`). Not its borders: an
  inline box's borders open on its first piece and close on its last,
  which the public tree does not say (booked).
- **Text:** the run through `os64_text_draw` at the box's baseline in the
  style's colour, then its decorations from the box's `decoration` and
  `decoration_color`: underline one thickness below the baseline,
  line-through 3/10 of the size above it, a thickness of a sixteenth of
  the size and never less than a pixel.
- **Marker:** a numbered one is its text. Disc, circle and square are
  drawn as SHAPES, the way browsers draw them: the text engine's Western
  profile does not reach the geometric-shapes block, and a face without
  it would draw a missing-glyph box. A third of the size across, on the
  height a line-through takes.
- **Atomic:** a picture is a frame with its alt text until images arrive
  (slice Y5); a control is drawn inert until it becomes a real widget
  (Y4).
- **Nothing** for a box whose `visibility` is hidden (its children answer
  for themselves), and nothing for a line.

**Scrolling.** The wheel moves three lines of the default font a notch,
and a tilt moves sideways; the arrow keys move a line; Page Up and Page Down a view less one line of
overlap; Home and End the ends. The horizontal bar appears only when
`flow_width` is wider than the view. The layout width is the view's width
LESS the vertical bar, and the bar's space is always reserved, so a page
that grows past one screen does not change width, re-wrap, and shrink back
below it.

**Resizing lays the page out again, and keeps your place.** Before the
new layout, the view notes the node of the box at the top of the
viewport; after it, `flow_box_for` finds that node's new box and scrolls
there. The new tree is built BESIDE the old one and the old one freed
last (LAYOUT.md § Bounds). A layout that returns NULL keeps the old tree
and says so on the status line; an incomplete one is painted as far as it
goes, with the sentence. A resize is laid out ONCE, after the events that
arrived with it are drained: a drag sends a stream of them, and a big
page takes seconds to lay out under emulation. The status line says how
long the last layout took, because that number is the one to watch.

**A file names no encoding.** A page from disk has no Content-Type, and
one that declares no `<meta charset>` is windows-1252 by the standard's
default — which turned Hacker News's UTF-8 dashes into `â€“`. Bytes that
are valid UTF-8 and not plain ASCII are read as UTF-8, the file detector
browsers use; a `<meta>` loses to it only where the bytes already
contradict the `<meta>`.

**The window is named `<title> - yonder`**, fitted to the title's 31
bytes (the boundary refuses a longer one rather than cutting it) with an
ellipsis, and ASCII only, because the title bar is drawn in the kernel's
face. Named once, at creation: a page given on the command line is read
before the window exists; one opened later leaves the name as it was
(booked).

**Fonts.** Every page view shares ONE text context of its own, apart from
libui's chrome context, with a cache of opened faces on it keyed by
(family, bold, italic, size) — LAYOUT.md's arrangement. Until packet 04
lands, the resolver is `flowdump`'s: DejaVu Sans, or DejaVu Sans Mono for
the monospace generic, at the asked size. Serif, bold and italic are drawn
as the regular face until then, and packet 04's `os64_font_family_open`
replaces the resolver without the view noticing.

## Slices

Built the way libflow was: each slice finished, tested by its builder,
committed and pushed, the next stacked on it; Chris tests as he is able
and requests review when he is happy. A userland program is reviewed
in-house (CLAUDE.md's reviewer-to-risk rule).

| Slice | What | Proof |
|---|---|---|
| Y1 | The page view on a LOCAL FILE: `yonder /tests/pages/hacker-news.html`, or a path typed in the address field. The painter and its four verbs, the canvas rule, borders and bevels, decorations, markers as shapes, scrolling in both directions, resize with the anchor, the status line on hover | `tools/test_yonder_host.sh`: hand-computed recordings for fixtures and a corpus regression; guest screendumps read |
| Y2 | Packet 06: the navigator out of `wend` into its library | the packet's own evidence; `wend`'s acceptance passes unchanged |
| Y3 | The network, through libway on packet 07's pool: http and https, a click follows a link, back, forward, reload and stop, a fragment scrolls to its target, declared refreshes, POST (Quinn's packet 05 carried across from wend), the questions asked in the window | § Y3 below |
| Y3b | libway's cookie jar and `Referer`, on packet 05's hooks, for both browsers | libway's host harness; logging in to a real site |
| Y4 | Forms: every control libflow placed becomes a real libui widget at its box, moved when the page scrolls and hidden when it leaves the view (libui does not clip a child to its parent), submitted through libpage's form model, GET or POST | § Y4 below |
| Y5 | Images: fetched in parallel on the pool, decoded, drawn scaled and blended, the page laid out again when a size arrives | § Y5 below |
| Y5b | Moving pictures and pictures behind: GIF animation on a frame clock that sleeps when nothing can be seen, and the `background` attribute of the body and of tables, tiled | § Y5b below |

Y1 needs none of the open packets, which is why it came first. Packets 06
(Y2), 07 (the pool and the doorbell), 02 (blending) and 03 (GIF) are in;
05 (libfetch's request body and cookie hooks) is in review, and Y3 starts
once it merges.

## Y3 — the network

**The window never waits for the network.** A navigation is a job on
packet 07's pool (`os64/work.h`); the window keeps painting, scrolling and
answering its close box while a page loads, and hears the job through
07's doorbell (`on_doorbell`, `OS64_GUI_EVENT_DOORBELL`). One pool, made
at start with four workers, which Y5's pictures will share.

**What runs where.** The worker does `way_load`: the fetch, the parse, the
model — libfetch, libhtml and libpage are pure over their buffers and the
heap is thread-safe (07's audit). Everything else happens on the UI
thread: layout, because every page shares one text context and a context
is not for two threads at once; the history; the painting. The job's
product is the arrived `way_page_t` with its fetch verdict and sentence;
on the bell, the UI thread reaps it, lays it out BESIDE the page on
screen, and swaps only when the layout exists (the rule wend has always
kept, now in two faces).

**A load gets its own leg of the session.** `way_load` writes its
sentences into the session and reaches the person through its face, and a
worker must not share either with the UI thread. So libway splits the
session: the browser's identity (name, agent, accept) and the history stay
in `way_session_t`, owned by the UI thread; a load runs on a `way_leg_t` —
the identity by pointer, read-only, and its own face and its own sentence.
wend builds a leg for each load and runs it where it always has, so its
screens do not change; yonder builds one per job. (A leg of a journey, the
part between two stops.)

**The mailbox outlives whoever lets go of it last.** Progress and a
question travel between a job and the window through a MAILBOX, and the
job cannot own it: cancelling a job hands it to the pool's `release`,
which may free it at once — immediately, if the job had already finished
— and a lock protects concurrent access, not freed memory. So the mailbox
is its own object, REFERENCE-COUNTED, one reference held by the window's
navigation and one by the job; each side drops its own (the window when
the navigation is superseded or reaped, the job in `release`), and the
last to let go frees it and closes its answer pipe. Every mailbox carries
the GENERATION of the navigation that made it, and every bell the window
hears is checked against the current generation: a progress sentence or
a question from an obsolete job is dropped unread.

**Progress, cancel and the one navigation.** The job's face has three
answers:

- `progress` copies the sentence into the mailbox under its
  `os64_lock_t` and rings the PROGRESS bit; the status line shows the
  latest one on the next bell. Coalescing is right here: a person wants
  the newest byte count, not every one.
- `cancelled` is the POOL's predicate — the one `run` is handed — and
  nothing else. Stop, a new navigation and the close box cancel through
  `os64_work_cancel`, and the pool also cancels on its own infrastructure
  failures and at destroy; a private flag would hear the first three and
  miss the rest, and a fetch or a question still waiting at destroy would
  hang it. Only one navigation is in flight: starting another cancels the
  one before, and a reap for any id but the current one is released
  unread — a cancelled job can finish before it notices.
- `confirm` is the hard one, below.

**A question asked on a worker is answered in the window.** libfetch's
downgrade hop asks mid-fetch, on the worker. The worker publishes the
question in the mailbox with a QUESTION NUMBER, rings the QUESTION bit, and
waits on the mailbox's answer pipe — `os64_read_for` in 100 ms steps,
asking the pool's predicate between them, because 07's rule is that a
worker must never wait on something that cancellation cannot end. The UI
thread shows the QUESTION BAR — the question and two buttons, Yes and No,
above the status line — and writes the answer down the pipe TOGETHER WITH
the question's number; an answer carrying any other number is not an
answer to this question and the worker keeps waiting. Stop, a new
navigation and the close box CANCEL the job rather than answer it, and the
cancel ends the wait unanswered, which is No; a question replaced by a
newer one is answered No.

**A question asked on the UI thread is not a nested loop.** A form that
leaves https for http, or a refresh that does, is judged BEFORE any job
exists: `way_judge` returns the question instead of asking it, the window
shows the same bar, and the navigation starts only when Yes is clicked —
the bar's continuation is the request. So `way_may_go`, the blocking
convenience, stays wend's; yonder asks and acts in two steps.

**What makes an answer fresh** is the face's to say (libway's contract),
and a click is NOT fresh by itself: a doorbell can be handed over ahead
of mouse events queued before it, so a press made before the question
existed could land on a Yes that appeared under the pointer. So the bar is
born DISARMED (its buttons greyed). It arms only once the window has
PAINTED it and then found its event queue EMPTY — everything waiting
before that may have been done before the bar could be seen, so it is
dispatched to a disarmed bar first — and Yes answers only a press AND
release that both arrive while it is armed (`bar.c`, pure and
host-tested). The bar reads its own clicks before libui does, so a
button's own click never answers. Each bar is bound to its question's
number, so a bar replaced by a newer question cannot answer the newer
one. The keyboard's Enter is not bound to Yes; Escape is No.

**A question says nothing about how to answer it.** libway's questions end
at the question mark; wend adds its " (y/n) " (its row reads exactly as it
did) and yonder has its buttons.

**The toolbar.** Back, Forward, Reload and Stop beside the address field;
Stop is lit while a page loads and Reload while one is shown. The address
field takes
what `way_typed_address` takes, and a path that begins with `/` is still a
file, read as in Y1.

**Forward is libway's**, decided now that a face asks (packet 06 booked
it). The history grows a second stack: going Back pushes the page left
onto it, following anything new clears it, going Forward pops it. wend
has no Forward key and calls neither new function, so nothing changes for
it. The position in each crumb is yonder's scroll offset, checked against
the page that comes back before it is used.

**Links.** A click — press and release on the same link — asks libpage
(`os64_page_activate`): a FRAGMENT scrolls to `flow_box_for` its anchor
with no fetch; a NAVIGATE goes through `way_judge`; a refusal is the
status line's sentence. A control is Y4's; clicking one says so.

**Declared refreshes** are judged on arrival, as wend does:
`way_refresh_step`, a chain counted from the last navigation a person
started, a JUMP a scroll, a GO a new job that leaves no history entry. The
delayed-refresh sentence names wend's key today ("press g to go"); libway
takes the hint from the face instead (`session->delayed_hint`), so wend's
row reads exactly as it does and yonder's says what yonder offers.

**A text/plain page** arrives as bytes. It is laid out by handing libhtml
`<plaintext>` and then the bytes, in the encoding libway chose — the
standard's own element for "everything after this is text", so libflow
sees a `pre` and nothing new is needed.

**POST.** Quinn's packet 05 gives libfetch a request body and wend a POST
submission. Y3's rebase carries that behaviour into libway — `way_judge`
stops refusing POST, `way_load` takes the method and body — so both faces
have it, and Quinn reads the port. Three of her rules travel with it: the
explicit warning before a POST is REPLAYED unencrypted; the download
advice ("save it with os64get") judged on the FINAL method, because a
reply to a POST cannot be fetched again by an address alone; and the
request body kept alive for the whole asynchronous fetch — it belongs to
the job's input, released by the pool's `release` and not before.

**Evidence, as run.** In the guest, yonder walked the local rules server
that proved Y2 — links clicked, the toolbar clicked — and the server's log
matched wend's walk request for request: gopher and mailto refused in
yonder's words, a 404 shown as a page, text/plain drawn, a picture refused
with its os64get command, a redirector followed and skipped by Back, a
delayed refresh offered in the address field, a chain stopped at its sixth
page, a self-refresh refused; a fragment link scrolled with no request;
Back and Forward fetched what they should. `httptestd.py`'s `/stall`
(half a head, then silence) kept the window painting and scrolling while it
hung, and Stop ended it at once ("stopped"). Against the HTTPS test peer
(`tools/test_os64get_https_peer.py`, its roots and host names written into
the guest's /home), `/downgrade` put the worker's question in the bar;
Yes followed it to plain http, No stopped at the hop (libfetch hands the
redirect itself back as the page, as wend shows it), and closing the
window with the question up and its worker waiting shut it within three
seconds. On the host: the bar's arming fed the sequences that must not
answer (a press before the question, a press before arming with its
release after, a press on a replaced question); the mailbox whole until
its last holder lets go, then gone with its pipe; the answer bound to its
question's number; a cancelled wait ending as No; and libway's leg,
Forward and delayed hint. wend's walk again: identical. Not run in the
guest: a question asked on the window's own thread (a form or refresh
leaving https), for want of an https page with such a form — its judgement
is libway's harness's and its bar is the same bar.

## Y4 — forms

Mosaic drew its forms with Motif widgets embedded in the page; yonder
does the same with libui's. Chris's order: forms after pictures, before
cookies.

**One widget per control libflow placed**, made when the page arrives and
let go with it. They are children of the window's root, after the window's
own widgets, so a page's widgets leave together: libui has no call that
removes a widget, so the list is cut back to the window's own after
interaction is cancelled (focus, hover and a press grab must not outlive the widget
they point at) and each widget's retained text runs are released the way
libui's own teardown releases them (the class's `destroy`, then `run` and
`run_staged`) — a run left behind would hold the window's text context busy
for good.

| The control | Its widget |
|---|---|
| text, search, email, url, tel, number, and the date/time types; a `textarea` | a text field (a textarea is one line in this slice; booked) |
| password | a text field that only ever holds bullets: yonder takes the keys for it itself (below) |
| checkbox, radio | a checkbox; a radio's group is libpage's, and picking one refreshes the rest |
| submit, button, reset, `button` | a button labelled with its value |
| `select` | a list box, one row per option, the page's choice selected; it shows the rows its `size` asks for, else up to four — enough to pick with the pointer, since a drop-down is booked |
| file | a disabled button — nothing here can choose a file yet (booked) |
| hidden | nothing: libflow gave it no box |

**A control's box is the widget's size.** libflow's size for a control it
knows nothing about is a text field's — ten ems by a line — and a tick
drawn across that is a slab, a list one line high shows no rows at all. So
yonder answers libflow's `replaced_size` for them as it does for pictures:
a checkbox or radio is the theme's tick square; a select is its rows at the
list box's own pitch, as wide as its longest option in the window's face.
The page's own `width` and `height` still win, as they do for a picture.

**Where the widget is.** On every paint that moves the page — a scroll, a
relayout, an arrival — each widget is put at its control's box on the
glass. libui does not clip a child to its parent, so a widget whose box is
not WHOLLY inside the page view is hidden, and the painter draws the inert
frame in its place (the painter draws a control's frame only for a hidden
widget). Disabled controls get disabled widgets; a readonly one, a disabled
text field.

**The values are the page model's.** A checkbox, radio or list writes its
change to libpage at once (`os64_page_set_checked`, `os64_page_set_chosen`).
A text field has no change callback, so every text field's contents are
written to libpage (`os64_page_set_text`) before anything is sent — the
model is what a submission reads.

**Sending.** A button that submits activates its control
(`OS64_PAGE_ACTIVATE_CONTROL`); Enter in a text field is the standard's
IMPLICIT SUBMISSION (`OS64_PAGE_ACTIVATE_IMPLICIT`), and libpage decides
whether the form has a default button and whether a field blocks it. A
NAVIGATE goes through `way_judge` with `WAY_ASK_SEND` — the question bar
for a form leaving https — and POST travels as Y3 carries it. A refusal is
libpage's sentence on the status line, and an INVALID one puts the focus on
the control that failed.

**A password is never on the glass.** libui's text field does not mask, so
yonder keeps a password's value itself and feeds the field one bullet per
character: typed characters append, Backspace removes the last, Enter
submits, Tab moves on, and everything else — arrows, selection, paste — is
swallowed, because a field that holds only bullets cannot edit in the
middle of a value it does not show. The value reaches libpage like any
other text field's.

**Reload on the reply to a form sends the form again, and asks first.** A
POST's reply cannot be fetched again by its address (Y3's rule), and
whatever the form did — an order, a post — it does twice. So the page
keeps the request that produced it, MOVED out of the job when the reply
arrives, and libway says whether the reply was to a POST by the FINAL
method (`way_page_t.posted`): a 303 turned the POST into a GET, and that
page is an address like any other. Reload puts "Send the form again?" in
the bar — or libway's own warning, when the form would go out in the
clear, because a yes to that is a yes to sending. Back and Forward to such
a reply fetch its address, as wend does (booked).

**What a browser shows as text** (libway, riding along at Chris's ask):
`text/*`, and the application types that are text in all but name — JSON
and `+json` (what an API, and httpbin, answers a form with), JavaScript,
XML and `+xml`. JSON with no charset is read as UTF-8, its RFC's rule. This
is what arrives as the PAGE: yonder fetches no script a page names, so a
page's scripts never reach the glass.

**Evidence, as run.** The guest, against a local server that echoes what
it is sent: a page of every kind of control drawn as widgets at their
boxes (ticks square, the list showing its three options with the page's
choice selected, fields holding the page's values). A password typed and
shown as six bullets; L picked from a radio group (M cleared); `blue`
picked; the tick cleared; sent with the required field empty — refused,
"a field the form requires is not filled in", the focus on that field.
Filled and sent with Enter: the server logged
`/echo?who=Ada&pw=s3cret&size=l&colour=blue&needed=yes+please`, exactly —
the unticked box absent, the password whole. The POST form: body
`note=hello+there`, `application/x-www-form-urlencoded`. Reload on its
reply: the question; No — "The form was not sent again.", nothing in the
server's log; Yes — the same body posted a second time. A form posted to
`https://httpbin.org/post` from an http page: httpbin's JSON echo shown as
text, the field, length, type and yonder's agent in it. A UTF-8 JSON reply
with no charset: `café`, right. A tall page of fields scrolled one step:
the field crossing the view's top edge hidden and drawn as its frame, the
toolbar untouched. The host harnesses, unchanged in count: libflow 10485,
yonder 49, libway 75, wend's renderer 177954, none failed. Not run: an
https page's form sending to http (the question is `way_judge`'s, proven
in Y3), and a search on the live web.

## Y5 — pictures

Chris's order after Y3: pictures first (almost every page he visited had
them), then forms (Y4), then cookies (Y3b).

**What is fetched.** libpage's picture list (`os64_page_image`) is the
record: every `img` with a src that resolved, in tree order. Pictures are
kept per ADDRESS, not per element — forty spacer GIFs are one fetch — and
the page's pictures belong to the page: the page stays on screen, pictures
still arriving, until the next one replaces it, and then every one still
in flight is cancelled and the page takes its pixels with it. A picture
finished for a page no longer shown is let go unread (the page's serial
number rides in its job).

**Each picture is a job on the same pool** as the page, run by
`picture.c`: libfetch for the bytes (`os64_fetch_open`, the browser's
agent, an Accept of image types, libimage's 20 MiB file cap), then
`os64_image_decode`, which knows PNG, JPEG, GIF and BMP by their bytes
rather than by what the server claims. The product is the decoded image,
0xAARRGGBB; the job's input and product are released by the pool if the
page is left first. The DECLARED RESERVE is honest to the decoders'
defaults: 20 MiB of body and 128 MiB of decoding (libjpeg's memory cap,
which also bounds PNG's 16-megapixel raster and its inflate buffer), and
the pool's budget grows to 768 MiB so a page and four pictures — one per
worker — run at once. A picture over the decoders' caps is refused by them
and drawn as its placeholder.

**The size is the layout's question, the pixels the painter's.** libflow
asks the face for a picture's intrinsic size (`replaced_size`); yonder
answers from the pictures that have arrived, and "unknown" for the rest —
libflow then lays out the alt text, or the box the width and height
attributes give. When a picture arrives whose element lacks either
attribute, the page is laid out AGAIN (keeping the reader's place, as a
resize does), because its size may move everything below it. Relayouts are
coalesced: at most one per drained batch of doorbells, and while pictures
are still arriving, at most one a second — the last arrival always lays
out, so the page settles at its true shape. A picture whose element has
both attributes needs no relayout at all; the old web wrote them on most.

**Drawing.** The painter's `image` verb draws the picture into its box's
content rectangle, SCALED nearest-neighbour when the box and the picture
differ (a `width=` stretching a spacer, a thumbnail shrunk) and blended by
its alpha over what is beneath, one row of the visible part at a time
(`scale.c`, pure and host-tested). A picture that has not arrived, or will
not, keeps Y1's frame. A GIF's other frames are § Y5b's.

**Memory kept.** Decoded pictures stay while their page is shown, up to
256 MiB of pixels; past that, a picture is not kept and draws as its frame,
and the status line says how many were left out. Back and Forward fetch
again (there is no picture cache between pages yet, as there is none for
pages).

**Evidence, as run.** The host: the scaler and blender against hand-worked
pixels (an opaque copy, a 2x enlargement, a 3-to-2 shrink, a
half-transparent source over a known ground, a transparent pixel, a clip
cutting all four sides, a box hanging off the surface's top left). The
guest, a page served locally with a picture of each kind: a PNG with no
size given (the page laid out again around it), a half-transparent PNG
blended over a blue cell, a GIF scaled 2x, a JPEG scaled by `width=` with
its shape kept, a 1x1 spacer stretched to a 200-pixel bar, a BMP, a
missing picture showing its alt text, a broken one keeping its frame, and
one address used four times fetched once — "pictures 6 of 8". Then live:
Wikipedia's article on Mosaic over https, its screenshots of Mosaic drawn
from upload.wikimedia.org (its logos are SVG, which libimage does not
decode, and keep their frames); and a page left nine seconds into its
pictures, the next page's count untouched by the late arrivals.

## Y5b — moving pictures, and pictures behind

Chris's order after Y4: the old web's GIFs move (theoldnet's guestbook,
its Angelfire badge), and its pages sit on tiled backgrounds.

### Animation

**A GIF with more than one frame is kept as a SEQUENCE**
(`image/sequence.h`, GIF_ANIMATION.md): libimage's handle that decodes a
frame when it is asked for the next one, disposes the last and composites
onto one canvas, honouring the loop count. The job decodes a GIF as a
sequence first — the handle copies its input, so the fetched bytes and the
sequence together stay inside the declared reserve — and keeps it when
there is more than one frame; a still GIF is let go and decoded the Y5
way. So nothing changes for any picture that does not move. What a
sequence costs is counted against the page's 256 MiB as its canvas, its
expansion buffer, its restore rectangle and its copied input, which is
libimage's own account of what a handle owns.

**The window has no clock, so a TICKER gives it one.** yonder's loop
sleeps in `os64_gui_event_wait`, which takes no timeout, so a thread of
its own holds the next frame's deadline: it sleeps on a pipe with
`os64_read_for` until the deadline or until the window writes to say the
deadline moved, and at the deadline it rings the window's doorbell and
forgets it. The ring is the only thing it does; the window thread does
everything else, so no picture is touched by two threads. Rung, the window
advances every animation that is due AND ON SCREEN — some box showing it
meets the view — repaints only those boxes, and hands the ticker the
earliest deadline left among the ones on screen. Only those boxes, because
the view paints only its dirty part: the kernel takes exactly the
rectangle libui publishes, so nothing outside it is ever seen, and a
moving bullet costs its own sixteen pixels rather than the page (a full
repaint a frame cost a third of a core for three small GIFs). A picture scrolled away
is not advanced (decoding frames nobody sees is the waste the frame clock
exists to stop) and moves again when it is back; a window that is covered
(`OS64_GUI_WINDOW_COVERED`, re-read on the COVERED/UNCOVERED nudge, since
the flag is the truth) hands the ticker no deadline at all. A page with
nothing moving costs a sleeping thread.

**The timing is gview's and the browsers'**: a frame's delay starts when
it is shown; a delay of 0 or 10 ms is shown for 100 ms (what every browser
does with the GIFs that ask for "as fast as you can"); a finite loop count
is honoured, so a GIF that plays once stops on its last frame, as Chrome
stops it. A frame that fails to decode holds the last good one and that
picture stops, while the rest play on.

### Pictures behind

**The attribute, not the property.** The Rendering chapter maps the
`background` attribute of `body`, `table`, `thead`, `tbody`, `tfoot`, `tr`,
`td` and `th` to `background-image`. The cascade will bring the property
and its relatives (`repeat`, `position`); this slice brings what the old
web wrote.

**libpage lists them**, beside the pictures: WHERE A PICTURE COMES FROM IS
A FACT ABOUT THE PAGE, and a background's address is resolved against the
base like every other reference, so two resolvers never disagree about a
`<base>`. `os64_page_background(page, i)` names the element and its
resolved address; `os64_page_background_for(page, node)` answers the other
way. wend reads neither, because a terminal has no background to put one
in.

**Fetched with the page's pictures**, from the same table, per address —
a picture used as an `img` and as a background is one fetch — and animated
by the same machinery. An arrival never lays the page out again: a
background sizes nothing.

**Painted after the box's colour and under its content, TILED** from the
top-left of its border box across that box, blended by its alpha over the
colour (`scale.c`, beside the scaler, and host-tested the same way). The
body's background is the CANVAS's, as its colour already is (CSS 2.1
§14.2): tiled from the page's top-left across the whole page, so it
scrolls with the page, as the attribute always did.

**Evidence, planned.** The host: the tiler against hand-worked pixels
(whole tiles, a tile cut by the clip, a box that is not a multiple of the
tile, a transparent tile over a colour); libpage's background list for each
element, a relative address under a `<base>`, an empty attribute naming
nothing; the painter's recordings with the new verb. The guest:
theoldnet's four GIFs moving (bullet02 still waiting on libimage's row in
BROWSER_DEBTS.md), a once-through GIF stopping on its last frame, a
scrolled-away GIF frozen and resuming, a covered window going quiet; a
local page on a tiled body background with a cell of its own; and an old
page on the live web that was built on one.

## Booked, with their triggers

| Debt | Why it waits | Trigger |
|---|---|---|
| A hand pointer over links | the compositor draws one fixed arrow; a pointer shape is a kernel and ABI change | somebody misses it more than the status line answers it |
| Retitling a window | there is no call for it; a window is named at creation | a page opened from the address field wants its name |
| Unequal border widths, tested | the diagonal corner rule is exercised only where sides differ, and no producer sets one side alone until the cascade (a mutation to the corner arithmetic survives today for that reason) | the cascade |
| An inline box's borders | the public tree does not mark a span's first and last piece | a page that needs them |
| Layout time on big pages | On the P5, Wikipedia (800 KB) lays out in 600 ms; fetch.spec.whatwg.org (1.9 MB) takes 4 s to load and 6 s to lay out again at full screen (Chris, 2026-09-25) | a libflow profiling slice, with those two pages as its benchmark |
| Links on a page from disk | a relative address does not resolve against a `file:` page | Y3, where pages come from the network |
| Serif, bold, italic | packet 04 | 04 merged |
| POST and cookies, logging in | packet 05 | 05 merged |
| A multi-line textarea, a drop-down select, several choices in a multiple select, a file chooser | each is a widget libui does not have yet (a multi-line field sized to its box, a popup list, a multiple-selection list, a file dialog) | the first form that needs one |
| Back and Forward to the reply to a form | the history holds addresses, so going back to a POST's reply fetches its address, which a server may answer with something else; Chrome shows a "resubmit?" page there | a page where going back to a reply matters |
| SVG pictures | libimage decodes raster formats; SVG is a vector language with a renderer of its own | the modern web's logos, which are mostly SVG |
| `data:` pictures, and `background=` | a data: address needs no fetch but a decoder of its own; a background image is a fill the painter does not tile yet | a page that needs one |
| A picture cache between pages | Back and Forward refetch pictures as they refetch pages | back-and-forth on a slow link hurts |
| Layout on a worker | every page shares one text context, which one thread uses at a time; a worker would need its own, with its own fonts opened | a page whose layout makes the window stop answering for long enough to matter (fetch.spec.whatwg.org takes 6 s to lay out again at full screen on the P5) |
| Cookies and `Referer` | slice Y3b: libway's jar on packet 05's hooks. `on_set_cookie` carries whether the reply came over an encrypted connection (Quinn, 2026-09-25), so the jar enforces `Secure` itself — libfetch hands over the facts, libway owns the policy | packet 05 merged and Y3 in |
| Selecting and copying text | `flow_hit` and the run's own hit test give the pieces; the drag, the highlight and the clipboard are a slice of their own | the first time Chris wants to quote a page |
| Find in page | a walk of the tree's text boxes | same |
| Keyboard link navigation (Tab through links) | a focus ring over boxes | same |
| View source | a textview over the bytes that arrived | it is small; take it when a slice has room |
| Text zoom | a relayout at a scaled `viewport_font_px` | somebody squints |
| Tabs | one navigator session per tab is the design; the session exists first | after Y3 |
| The cascade | yonder's ladder rung 4, its own arc | the ladder gets there |

## Review record

*(empty — Chris reads first)*
