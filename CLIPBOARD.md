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

## The consumers, in ratified order

1. **Kernel + `/sys/clipboard`** — the entry store, the synthetic file
   (sysfs grows its first WRITABLE file; /proc's ctl files are the
   precedent), tests, and libos64's `os64_clip_copy()` / `os64_clip_paste()`
   wrapping the file for apps that want a function instead of a path.
2. **scribe** — Ctrl+C / Ctrl+X / Ctrl+V against the selection machinery
   that is already there. The first customer, and the moment "highlighting
   has nothing to DO yet" stops being true.
3. **gterm** — drag-select in the terminal grid writes the snarf
   (select-IS-copy, the PRIMARY gesture); right-click pastes into the pty
   master as if typed. The daily driver gets the daily gestures.
4. **The text VTs** — the deep slice, and the headline wish: highlighting
   on a text console auto-copies, right-click pastes into the console
   input ring. Needs three new things, all with named homes: the input
   fork's **text-VT arm** (mouse events currently stop at gui_owns_glass;
   the fork's comment has promised this arm since the VT8 chapter — gpm's
   lineage, Alessandro Rubini, 1994), grid **cell-inversion** for the
   highlight, and **injection** into console input. Details designed at
   the top of that slice, not here.

## Deliberately not in v1

Clipboard history (the entry model is its runway), content types (the seat
is reserved), append mode, non-text payloads, a TUI widget kit (Chris was
"mostly" kidding; no consumers = nothing to build — but the VT selection
needs cell-inversion and an input route, not a toolkit).
