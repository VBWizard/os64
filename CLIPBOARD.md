# CLIPBOARD.md — the system's clipboard, designed before line one

Ratified by Chris, 2026-08-20 evening, the night scribe shipped. The design
chapter pattern (PTY.md, RTL8125.md, SCRIBE.md): this gets agreed, then
built, slice by slice.

## The rulings

1. **ONE buffer.** X11 shipped two — PRIMARY (what you last selected,
   middle-click pastes it) and CLIPBOARD (what you explicitly copied) — and
   spent forty years watching users paste the wrong one. Plan 9 shipped one
   and called it **snarf** (rio's word; this document borrows it with
   affection). os64 takes Plan 9's side: selecting in a VT, Ctrl+C in
   scribe, and a pipeline redirect all feed THE SAME buffer. The cost is
   named, not hidden: an idle selection clobbers an explicit copy. Chris's
   ruling: "one buffer for both is perfect — there are real use cases where
   I want to do exactly that."
2. **It is a FILE: `/sys/clipboard`.** His words: "since there's one, it is
   *the system's clipboard*." Copy and paste without a single new verb:

       grep panic /home/os64.log > /sys/clipboard    # copy
       cat /sys/clipboard                            # paste

   Every text utility in the OS became clipboard-aware the day the file
   appeared — that is the entire argument for a file over a syscall, and
   it is the printat/tty doctrine's cousin: things that are content go
   through the byte-stream world, where pipes and redirection already work.
3. **History is a design constraint TODAY, a feature LATER.** Chris: "I
   don't need it yet. But there will come a time." The core is therefore
   shaped so history is "stop discarding," not "start rewriting" — see the
   entry model below.
4. **The build order** (ratified): kernel + file → scribe → gterm → the
   text VTs. Slices 1–4 are Claude's; the shell-side toys (a snarf/paste
   utility pair for pipelines that want to be explicit, names TBD) are
   Chris's, if and when he wants the joy.

## The entry model — history-shaped from birth

The kernel does NOT keep "a clipboard buffer." It keeps **snarf entries**:

    entry = { bytes (kmalloc'd), length, tick stamp, (type seat, see below) }

- A write to `/sys/clipboard` CREATES a new entry; the entry is **sealed at
  close** — a multi-write copy is one snarf, and readers see the previous
  newest until the seal. Entries are IMMUTABLE once sealed.
- Reading `/sys/clipboard` snapshots the NEWEST sealed entry.
- **v1 policy: seal the new, free the old** — one live entry, exactly the
  single-buffer semantics ruled above. **History is the day this line
  changes to "keep the last N"** plus an exposure path with its own name.
  `/sys/clipboard` itself stays a FILE forever — consumers must never
  find a directory where a file used to be; history arrives beside it,
  not inside it (name decided when it's built).
- Concurrent writers: two opens make two entries, newest-sealed wins.
  Last-writer-wins is the only honest policy a clipboard has ever had.
- Open mode "a" (append) is REFUSED loudly in v1: entries are immutable,
  and appending to history's future spine needs a consumer to justify it.
- Size: grow per entry, ceiling 16MB with a loud refusal (the never-drop
  doctrine's shape: generous, but a clipboard is not a filesystem).
- The **type seat**: entries carry room for a content-type tag, unset in
  v1 (everything is bytes-presumed-text). When images or richer types
  arrive, they ride the seat instead of forcing a second clipboard —
  X11's second-system mistake, declined in advance.

## Slice 1 rulings (2026-08-21, at the top of the build)

Three questions the design left open, decided before the first line:

1. **A pending copy whose writer DIES is sealed anyway.** The handle closer
   cannot tell "returned normally" from "was killed", and a killed
   `grep > somefile` leaves a partial file on disk without anyone finding it
   surprising. The clipboard does not invent a rule the rest of the tree
   doesn't have. (The alternative — mark handles abnormal during task
   teardown — is real work for a case where the user can just copy again.)
2. **Hitting the ceiling POISONS the copy.** The write that would cross 16MB
   is refused, every later write on that handle is refused too, and the seal
   publishes NOTHING — so the previous snarf survives untouched. A truncated
   paste that *looks* complete is exactly the failure "tripwires over
   silence" exists to prevent. And the refusal is LOUD ON THE GLASS, not
   only in the write's return value: whether the user finds out must not
   depend on the writing tool having good manners about return codes.
   (`cat` does check, as it happens, so the harness run printed both.)
3. **`snarf_entry_t` in the kernel, `/sys/clipboard` as the face.** Plan 9's
   word kept where only kernel code sees it — a hat tip, not a user-facing
   name.

And the home question, asked properly and answered: **/sys, not /dev.** Plan
9 kept snarf at `/dev/snarf`, but *their* /dev is the service namespace
(cons, draw, mouse, time — servers exposing interfaces). os64's /dev is the
narrower Unix reading, and devfs.c's own comment excludes us: "there is no
position, no snapshot, and no buffer — a device answers from its nature, not
from stored bytes." The clipboard is stored bytes with a length and a
position. /sys needed only its scope widened (hardware → machine state that
is not a process), which is a comment edit; /dev would have needed a
doctrine repealed. The day /dev grows into a service namespace, revisit.

## The consumers, in ratified order

1. **Kernel + `/sys/clipboard`** — the entry store, the synthetic file
   (sysfs grows its first WRITABLE file; /proc's ctl files are the
   precedent), tests, and libos64's `os64_clip_copy()` / `os64_clip_paste()`
   wrapping the file for apps that want a function instead of a path.
   **BUILT 2026-08-21** — `kernel/src/clipboard.c` (the store),
   `sysfs.c` (the node), `userland/libos64/clip.c` + `<os64/clip.h>`.
   Implementation notes worth keeping:
   - A reader points the synthfs snapshot head STRAIGHT AT the entry's
     immutable bytes and holds a reference — zero copy, even at 16MB — so
     read/seek/tell are the generic snapshot fops, unmodified. Only `close`
     is the clipboard's own, because it must release the reference (and,
     on the write side, seal). One overridden fop, not four.
   - `stat` reports the entry's real length: `ls -l /sys` shows the
     clipboard as the only node with a size, and `os64_clip_length()` asks
     the question without reading the bytes.
   - Mode `"a"` is refused by sysfs's existing one-character mode test, so
     `>>` fails at open with husk's "cannot create" — the loud refusal the
     design asked for, at no cost.
   - Harness-verified end to end: `ls /sys` lists it; `echo ... >` then
     `cat` round-trips; `grep Root /home/os64.log >` copies 23,440 bytes of
     pipeline output and `wc` agrees to the byte; a 4,915,924-byte copy
     exercises the doubling growth; an 18.75MB copy is refused on the glass
     with the previous snarf intact; `>>` refused; `e2fsck` clean on both
     root and /home afterwards.
   - **Chris's acceptance test, and the right way to test a ceiling: an
     INFINITE SOURCE.** `cp /dev/zero /sys/clipboard` — it accepted exactly
     16,777,216 bytes, refused the next write, and the clipboard still held
     the copy of `/husk.rc` he had put there beforehand. Two of the OS's
     own synthetic filesystems, one pouring into the other, and the limit
     held the line without a person having to construct a big file first.
2. **scribe** — Ctrl+C / Ctrl+X / Ctrl+V against the selection machinery
   that is already there. The first customer, and the moment "highlighting
   has nothing to DO yet" stops being true. **BUILT 2026-08-21**, same day
   as slice 1:
   - **The mechanism went into libui, the BINDING stayed in the app.**
     `os64_ui_textview_copy/cut/paste` (ui_text.c) know the selection and
     the buffer; scribe's own shortcut table knows which keys mean them,
     exactly like Ctrl+S. A toolkit that claimed Ctrl+C would be a toolkit
     the eventual terminal widget has to fight for its SIGINT.
   - **Cut copies FIRST and believes the answer**: if the clipboard refuses
     the copy, nothing is deleted. It is the only operation in the editor
     that could lose work, so it doesn't.
   - **Copy works on a READ-ONLY view** — the help page is quotable, and so
     is any future log viewer. Paste refuses there, as it must.
   - Both directions STREAM through the open handle: copy writes line by
     line (the newline between lines is manufactured by the copy, since the
     model holds lines and not terminators), paste reads 512 bytes at a time
     and inserts one RUN per line instead of one call per byte. libui
     allocates nothing, and a 16MB snarf never exists twice.
   - `os64_ui_textfield_paste` came along for the ride: a path or a search
     term pasted into the entry field. One line, said out loud — a field is
     one line. No field COPY (a textfield has no selection model; inventing
     one is a different slice with its own consumer).
   - CR is dropped on paste, alone or as half a CRLF: a buffer holds LINES,
     and a carriage return inside one is a fossil of a file format.
   - Verified by driving the GUI: three lines selected in scribe → "copied
     157 bytes" → `cat /sys/clipboard` on VT1 printed them; `echo PASTED
     FROM HUSK > /sys/clipboard` → Ctrl+V put it in the document (37→38
     lines, dirty star); Ctrl+X took it back out and husk read it again;
     Ctrl+V into the Open field took the line and not the newline; Ctrl+C
     on the help page copied 31 bytes and Ctrl+V there did nothing. A
     318,398-byte paste (the whole boot log) landed 4,629 lines in under
     six seconds. Suite 24+28 green, no faults.
3. **gterm** — drag-select in the terminal grid writes the snarf
   (select-IS-copy, the PRIMARY gesture); right-click pastes into the pty
   master as if typed. The daily driver gets the daily gestures.
   **BUILT 2026-08-21.** The gesture is gpm's (Alessandro Rubini, 1994) and
   xterm's before it, arriving in os64 the same day the clipboard did.
   - **A prerequisite the slice exposed, and paid**: the compositor had no
     pointer grab for client windows, so a drag that left the window lost
     its tail AND its button-up. Fixed in the compositor, where it belongs —
     see GRAPHICS.md § The implicit pointer grab. It cured a scribe bug of
     the same shape that Chris had already met and filed under "quirk".
   - **RULING (his, asked before the code): a pasted newline RUNS the line.**
     No bracketed paste. His words: "I am a smart guy. If I want to run
     multiple lines I copy multiple lines. If I want to review first, I
     review before copy or copy one line at a time, no newline." xterm's
     ESC[200~ wrapper (2002) exists to let a shell second-guess its user;
     os64 declines, and the DIVERGENCES row records it.
   - **Trailing blanks are not text.** A terminal row is padded to the full
     width with spaces nobody typed; the copy trims them per row, joins rows
     with `\n`, and ends without one — so a one-row copy pastes inline and
     lands on the command line *without* running.
   - **The highlight shows exactly what the copy will contain**: the same
     trim decides which cells invert, so the lit region hugs the text
     instead of running a white bar to the right margin. Inverse video, no
     theme colors, no agreement needed with whatever the program painted.
   - Wrapped long lines copy as TWO lines — the pty cell is `{glyph, color}`
     with no wrap bit. Named, not fixed: a bit nobody has asked for.
   - Selection is the VISIBLE GRID only (a pty has no scrollback yet — when
     gterm grows one, the anchor/end pair becomes scrollback coordinates and
     nothing else here changes). It dies on any keystroke, and when the grid
     generation moves, because a highlight is a claim about WHICH TEXT you
     have and those coordinates would now name something else.
   - Harness-verified by driving the mouse: single-row drag → `wc` says 25
     bytes, no padding, no newline; right-click → the line lands on husk's
     prompt un-run, and Enter proves it was really typed; a two-line snarf
     pasted → both commands executed (ruling A, demonstrated); a drag out of
     the window and released on the DESKTOP still published (the grab);
     multi-row copy round-trips through `cat /sys/clipboard` with its column
     alignment intact; scribe's cross-edge drag now scrolls and selects, and
     Ctrl+C took 2,011 bytes of it. Suite 24+28 green, no faults.
4. **The text VTs** — the deep slice, and the headline wish: highlighting
   on a text console auto-copies, right-click pastes into the console
   input ring. Needs three new things, all with named homes: the input
   fork's **text-VT arm** (mouse events currently stop at gui_owns_glass;
   the fork's comment has promised this arm since the VT8 chapter — gpm's
   lineage, Alessandro Rubini, 1994), grid **cell-inversion** for the
   highlight, and **injection** into console input. Details designed at
   the top of that slice, not here.
   **BUILT 2026-08-21** — `kernel/src/vt_select.c`. All three promised
   pieces, plus one nobody had listed: a text-mode **mouse pointer**, which
   is gpm's own answer — invert the character cell under it. No bitmap
   cursor, no theme, works on any glass that can paint a character.
   - **Chris asked the right question first: "shouldn't this be a mouse
     daemon?"** On Linux it was one, and the reason is worth keeping: the
     mouse arrived as a character device with no in-kernel consumer, so gpm
     read `/dev/mouse` in userland and handed the selection BACK through
     `ioctl(TIOCLINUX)` — the highlight and the paste were always kernel
     code. os64 already holds all three pieces on this side: the PS/2
     driver, the console's cell grid, the clipboard. A daemon here would
     mean inventing a `/dev/mouse` to export events and a control path to
     feed them straight back — two new ABIs to carry data out of the kernel
     and return it unchanged. The seam stays open if selection POLICY ever
     wants to live in userland (word/line modes were gpm's real value-add).
   - **No new thread, either.** The compositor already drains the one input
     ring every frame whether or not it owns the glass, so the text-VT arm
     is a ROUTING decision in `route_event_locked`, not a second input path.
     Events move state under kGuiLock; painting, copying and pasting all
     happen in the frame pass with that lock released — because painting
     takes the tty and renderer locks, and reaching those while holding the
     compositor's would invent a lock order nothing else in the system has.
   - **The overlay knows it is a lie.** It remembers which rows it inverted
     and repaints them from the grid next frame. If the terminal repainted
     itself first (its `generation` moved, or the scrollback view scrolled),
     the lie was already overwritten by the truth — so the selection is
     dropped rather than "restored" as stale glyphs over new output.
   - **The paste never drops a byte.** The console input ring is small and a
     snarf can be large, so the paste is fed in installments across frames
     (`tty_input_push_if_room`, new — it refuses a full ring instead of
     dropping, which the keyboard's own path cannot do: a keystroke has
     nowhere to wait, a pasted byte does). The entry stays REFERENCE-HELD
     while it dribbles — the refcounted-entry design from slice 1 earning
     its keep, since a copy elsewhere mid-paste cannot pull the bytes away.
   - Two supporting bits: `renderer_glass_putc_bg_locked` (a cell with its
     own background — put_char grew a two-color form) and
     `tty_visible_line` (the ring+scrollback math, named once instead of
     copied into a second reader).
   - **DEBUG_CLIPBOARD (bit 30, "CLIP") is earned here** — the promise in
     clipboard.c's header, kept: this consumer snarfs with no file in sight,
     so the bit went into CONFIG.h, klog_format.h's %g table and log.c's
     static asserts in one change.
   - Verified by driving the mouse on VT1: the pointer appears as an
     inverted cell and survives a flood of output under it; a drag lights
     the line in inverse video hugging the text; release copies it
     (`wc` says 41 bytes, exact, no padding, no newline); right-click types
     it back onto husk's command line; and then — the arc's closing move —
     **that same snarf pasted into gterm in the GUI.** Text console to
     window system, one buffer, no protocol. Suite 24+28 green, no faults.

## Deliberately not in v1

Clipboard history (the entry model is its runway), content types (the seat
is reserved), append mode, non-text payloads, a TUI widget kit (Chris was
"mostly" kidding; no consumers = nothing to build — but the VT selection
needs cell-inversion and an input route, not a toolkit).
