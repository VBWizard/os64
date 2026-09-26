# YONDER.md — the graphical browser's face

*Written 2026-09-25 by Opus, the evening libflow's six slices were pushed.
The campaign is [BROWSER.md](../../../BROWSER.md), what a page means is
[LIBPAGE.md](../../../LIBPAGE.md), where its boxes go is
[LAYOUT.md](LAYOUT.md), and the gaps are the packets in
[docs/yonder/](../../yonder/README.md). This is rung 2 of yonder's ladder:
a window you can read the web in.*

## What it is

`yonder` is a libui program. It links libhtml, libpage and libflow
unchanged (and libfetch and the navigator library once there is a network
to reach) and adds exactly two things of its own: a PAGE VIEW widget that
paints a flow tree and scrolls it, and the window around it — a toolbar
(back, forward, reload, the address field) above the view and a status
line below it.

Nothing about a page's meaning or geometry is decided here. The view asks
libflow where a box is, libpage what a node means, and the navigator what
to do about a click; it decides only how things LOOK on the glass and how
a person moves around them.

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
| Y3 | The network, through the navigator: http and https, a click follows a link, back and forward, reload, a fragment scrolls to `flow_box_for` its target, the navigator's sentences on the status line | guest: the corpus pages live, a fragment link, a redirect, a refused scheme, back after a dead link |
| Y4 | Forms: every control libflow placed becomes a real libui widget at its box, moved when the page scrolls and hidden when it leaves the view (libui does not clip a child to its parent), submitted with GET through libpage's form model | a search form on the live web, typed and submitted |
| Y5 | Images: fetched in parallel, decoded, blended, the page laid out again when a size arrives | the corpus pages with their pictures |

Y1 needs none of the open packets, which is why it comes first. The rest
wait on what the packets list: Y3 on 06 (Y2) and on packet 07; Y4's POST
on 05; Y5 on 07's pool and on source-over blending.

**Y3 fetches on 07's pool and hears it through 07's doorbell**, so the
window keeps painting, scrolling and answering its close box while a page
loads. Should Y3 be ready before 07 is merged, the fallback is a fetch on
the UI thread whose `cancelled()` polls the window's own events without
waiting — Escape and the close box still end it; the page just does not
repaint meanwhile — booked below with 07's merge as its trigger.

## Booked, with their triggers

| Debt | Why it waits | Trigger |
|---|---|---|
| A hand pointer over links | the compositor draws one fixed arrow; a pointer shape is a kernel and ABI change | somebody misses it more than the status line answers it |
| Retitling a window | there is no call for it; a window is named at creation | a page opened from the address field wants its name |
| Unequal border widths, tested | the diagonal corner rule is exercised only where sides differ, and no producer sets one side alone until the cascade (a mutation to the corner arithmetic survives today for that reason) | the cascade |
| An inline box's borders | the public tree does not mark a span's first and last piece | a page that needs them |
| Layout time on big pages | Wikipedia (800 KB) lays out in 600 ms on the P5 (4 s under QEMU's emulated CPU), and the window blocks while it does | a page that makes a person wait: a libflow profiling slice, or layout on a worker once packet 07's pool is in |
| Links on a page from disk | a relative address does not resolve against a `file:` page | Y3, where pages come from the network |
| Serif, bold, italic | packet 04 | 04 merged |
| POST and cookies, logging in | packet 05 | 05 merged |
| A window that repaints while it fetches, IF Y3 arrives before packet 07 | the fallback above | 07 merged |
| Selecting and copying text | `flow_hit` and the run's own hit test give the pieces; the drag, the highlight and the clipboard are a slice of their own | the first time Chris wants to quote a page |
| Find in page | a walk of the tree's text boxes | same |
| Keyboard link navigation (Tab through links) | a focus ring over boxes | same |
| View source | a textview over the bytes that arrived | it is small; take it when a slice has room |
| Text zoom | a relayout at a scaled `viewport_font_px` | somebody squints |
| Tabs | one navigator session per tab is the design; the session exists first | after Y3 |
| The cascade | yonder's ladder rung 4, its own arc | the ladder gets there |

## Review record

*(empty — Chris reads first)*
