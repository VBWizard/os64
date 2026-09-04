# SCRIBE.md — the GUI text editor, designed before line one

The pattern PTY.md and RTL8125.md earned: this chapter gets ratified, then
code gets written. Rulings already made (Chris, 2026-08-20) are marked so;
everything else is design awaiting his eyes.

## What scribe is

`scribe <file>` opens a window and edits text in it. That sentence is the
whole program; everything below is about making it true without lying.

The lineage seat it fills: quill(1) holds ed's chair — the editor as a
conversation, 1969. scribe holds **Bravo's** (Xerox PARC, 1974, Lampson &
Simonyi): the screen-full-of-pixels-IS-the-document editor, the idea Simonyi
carried out of PARC and into Word. os64 has never had one. Between those two
chairs sits vi's — the terminal screen editor — which stays EMPTY for now,
deliberately: it belongs to the escape-sequence slice (the in-band doctrine
in syscall_numbers.h's printat comment), and nothing about scribe blocks it.

**RULED: modeless.** Click somewhere, type, the text appears — "for now" (his
words), meaning modal editing is not refused forever, it is just not v1. A
GUI's identity is direct manipulation; modes are how terminals coped.

**RULED: the name.** scribe — his pick. (vellum was offered and declined on
memorability grounds. The author of quill remembers quills.)

## v1 scope (RULED)

Open, edit, save, scroll, select. Explicitly OUT of v1, each with a reason:

- ~~**Clipboard**~~ — LANDED 2026-08-21, exactly as this line predicted:
  cut into its own arc (CLIPBOARD.md), designed once scribe ran, and then
  in-app cut/copy/paste rode it in. Ctrl+C/X/V; the mechanism is libui's
  (`os64_ui_textview_copy/cut/paste`), the keys are scribe's. The system
  seam this paragraph worried about turned out to be one FILE —
  `/sys/clipboard` — which is why there was no fetch protocol to design and
  no "who holds the selection" to litigate: nobody holds it, the kernel
  keeps the bytes, and husk shares the same clipboard scribe does.
- **Undo** — booked for slice two. It wants the buffer's edit operations to
  be journaled, so the v1 buffer API is shaped not to fight it (operations
  go through three choke points — see below — which is exactly where a
  journal attaches).
- ~~**Search**~~ — PROMOTED INTO v1 (2026-08-20, second reading): Chris named
  "finding things in os64.log" as a primary use, and search IS that feature.
  Ctrl+F reveals the path textfield repurposed as a search field; Enter jumps
  to the next hit and scrolls it into view. Linear scan over the loaded
  buffer — no index, no regex (booked), honest and fast enough.
- **Mouse wheel** — GRAPHICS #6, not scribe's to build.
- **Word wrap** — v1 clips long lines and scrolls horizontally to follow the
  cursor. Wrap is a REFLOW engine and reflow is where editors go to die
  young; the seam stays open.

## The split: what goes in libui, what stays in scribe

The reason this app is mine (RULED): it grows the toolkit. Three new
controls, each shaped for its NEXT consumer, not just for scribe:

1. **ui_textview** — a viewport over a text buffer: renders visible lines,
   owns the cursor and selection, translates clicks/keys/drags into buffer
   operations and cursor motion, keeps the cursor scrolled into view. It
   does NOT own the text. The buffer is the app's (scribe's), handed in as
   a small vtable (line count, get line, insert, delete, split, join). That
   split is what makes the control reusable: a future log viewer hands it a
   read-only buffer and gets scrolling and selection for free.
2. **ui_scrollbar** — vertical, draggable thumb, proportional. Textview and
   scrollbar are PEERS wired together by the app (the on_resize philosophy:
   layout is a call the app makes, not a policy libui holds).
3. **ui_textfield** — one line of editable text. Born for Save As, but this
   is really the FORM control every future dialog needs, which is why it is
   a separate widget and not a scribe special.

scribe itself keeps: the buffer implementation, file I/O, the button row,
and layout. Target: the app stays small enough to be an exemplar, like
uiprobe before it — the start-here table's next row.

## The buffer

An array of independently-allocated lines (pointer + length + capacity),
with a line-pointer array that reallocs by doubling. Not a gap buffer, not a
rope — and choosing boring on purpose: os64 files that matter here are
husk.rc, theme.conf, logd.conf, source files; a million-line pathology can
arrive with its own consumer someday. Three choke points mutate it —
`buf_insert(line, col, text)`, `buf_delete(line, col, n)`,
`buf_split/buf_join` — and every keystroke routes through them, which is
where slice two's undo journal will attach without a rewrite.

Files load whole and save whole (ext2 is write-through; a saved file is a
durable file). The ceiling is MEMORY-AWARE, not a magic number (second
reading's ruling driver: os64.log is a named primary use, sometimes tiny,
sometimes 136MB — a measured fact from the logging arc): scribe asks
os64_memory() and refuses only a file that genuinely will not fit alongside
its line table, and the refusal message says both numbers. Loading a big log
whole costs seconds and RAM the machine has; if that ever hurts, the buffer
vtable is where a streamed read-only backend slots in without touching the
textview (booked, unbuilt).

Save writes `<file>` directly. Crash-safety via write-temp-then-rename is
possible the day it matters — ext2 replacement rename is atomic, and the
policy-bearing syscall can require that guarantee — booked, not built: v1's
honest failure mode is the same as quill's.

## Chrome (RULED: button row, not menu bar)

One row of libui buttons: **Open**, **Save**, **Save As**. Open and Save As
reveal a ui_textfield for the path (type it; a file PICKER is a directory
browser and its own control — booked). "If we hate it we'll change it to a
menu bar" — his ruling, so the row is built as a widget strip that a menu
bar could replace without touching the textview.

Dirty state: the title shows `scribe — <file> *` when unsaved changes exist
(the window title is the one place the window system already owns). No
"are you sure" dialog in v1 — there is no dialog machinery and building it
for a guard rail inverts the priority; quill's user knows what Save is.

## Input it needs (all already delivered)

Printable keys arrive as key events; arrows and named editing keys already
survive the extended-scancode path (the resize arc's modifier work walked
this exact ground). Shift+arrows = selection. Click places the cursor, drag
selects — the grab machinery libui built for buttons covers this. Resize:
libui's on_resize re-runs scribe's layout; the textview shows more document.
scribe is thereby the first REAL consumer of the resize arc.

## The format seam (named 2026-08-20, deliberately not built)

Chris's forward flag: someday scribe (or its household) may want to DISPLAY
.md, old .doc, other open formats — "displaying what's in a file, not
displaying text forever." The design already leaves the right door open, and
this section exists so nobody paints it shut: ui_textview is THE PLAIN-TEXT
RENDERER, one model+view pair among possible several. The buffer vtable is
the model side of that seam; a future format arrives as its own loader and
its own view behind the same window, chrome, and file plumbing. What slice
one must therefore NOT do: let file-format knowledge leak into textview, or
let textview become the only way scribe knows how to fill a window.

## Slice two (booked, in rough order)

Undo (journal at the choke points) → ~~clipboard integration~~ (DONE
2026-08-21) → regex/case-fold search options → streamed read-only big-file
backend → file picker control → word wrap → other document formats (the seam
above). Each its own conversation.

Undo is now the head of this queue, and the clipboard sharpened its case: a
318KB paste is 4,629 lines of change that a single Ctrl+Z ought to take
back, and the three choke points are still where the journal attaches.
