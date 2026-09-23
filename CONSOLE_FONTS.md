# CONSOLE_FONTS.md — changing the face a virtual terminal draws with

**Status: all six slices and the persistence are built** (§ Work, in
slices; § Persistence) — `vtfont` puts a bitmap or an outline face on the
virtual terminals, at a pixel size or at the largest size that gives the
grid you ask for, and `console.conf` makes it the face every boot prints
in from the post-boot tests on. A section moves into the present tense
when its slice lands, and this line changes with it.

## The one-paragraph version

The kernel console keeps drawing from a **bitmap** font, exactly as it does
today, but it learns two things: to accept a face whose cell is not 8x16,
and to accept one **at runtime** instead of only from a Limine module at
boot. The format it accepts is **PSF2**. A TrueType/OpenType face never
enters the kernel: a userland program renders it, at the size asked for,
into a PSF2 image and hands the kernel that. So there are two ways to get a
console font, and both arrive as the same bytes:

```
  terminus-32.psf ────────────────────────────┐
                                              ├──> /sys/console/font ──> kernel
  DejaVuSansMono.ttf + "24" ──> libos64 ──────┘        (PSF2 bytes)
                               (FreeType, ring 3)
```

"Resizable" is the second row: a PSF file is one size forever (Terminus
ships a separate file per size); an outline face rendered on request is any
size you name.

## Why the kernel takes a bitmap and nothing smarter

The kernel is built `-mno-sse` and never touches the FPU register file —
that is the invariant that makes eager `fxsave64` per thread correct
(CLAUDE.md § Floating Point). FreeType is a quarter of a million lines of
somebody else's code that assumes floating point and a heap with a
`realloc`. Neither belongs in ring 0, and the panic path must never depend
on either. A glyph table the kernel can index with a byte is the whole of
what a text console needs.

This is also simply how it has always been done. The VGA BIOS had a call to
upload glyph bitmaps into plane 2 so DOS programs could change the text-mode
font; Linux's `setfont` does the same through `KDFONTOP`. The console is a
dumb glyph table and something smarter fills it.

## Why PSF2

- **It is what we already speak, one revision on.** The boot face is PSF1
  (`zap-light16.psf`). PSF1's width is fixed at 8 by the format; PSF2 (1999,
  the Linux `kbd` project) carries width, height, glyph count and header
  size as fields. Everything else is the same idea: a header, the glyph
  bitmaps row-major and MSB-first, then an optional Unicode table.
- **The Unicode table is what makes any face but the boot one drawable at
  all — for BOTH character sets.** A cell holds a byte and the set it was
  written under; a font holds glyphs in whatever order its author liked.
  Only zap happens to keep Latin-1 in index order, so "the byte is the
  glyph index" is a fact about zap, not about Latin-1. `psf2_build_charmap`
  walks the table once and produces two rows of 256 glyph indices: Latin-1
  byte b is code point b, CP437 byte b is `os64_cp437_codepoint(b)`, and the
  first glyph to claim a code point keeps it. What no glyph claims is
  `PSF2_MAP_NONE`, which draws blank — except the three shades, the full
  block and the four halves, which `psf2_synth_block` makes to measure for
  any cell. **At 8x16 all eight are byte-identical to what the console draws
  today**, which is the continuity that matters and is stronger than a match
  against one header: five of them are bitmaps `os64/charset.h` supplies and
  three are glyphs the shipped face itself carries, and the host test asks
  the question the way the console asks it rather than pinning literals.
- **A face with no table is drawn as the identity** under both sets. Such a
  font is laid out in its own code page and nothing in the file says which.
- **A face that cannot draw printable ASCII is refused** (`PSF2_NO_ASCII`,
  naming the character). A console nobody can read a prompt on is an outage,
  and the refusal is cheaper than the reboot.
- **There is a shelf of them.** The `kbd` package's `consolefonts/`
  directory is a few hundred faces; Terminus (12 to 32 px, the usual answer
  on a high-DPI console), Spleen, Tamsyn, Cozette, GNU Unifont and our own
  zap family all ship as PSF. Most are `.psf.gz` — `gunzip` is already in
  /bin.

Limits the loader enforces (`kernel/include/psf2.h`), each refused by name
and with the number that broke it: magic `72 B5 4A 86`, version 0, header
size 32 or more and inside the image, width 4..64, height 6..128, glyph
count 128..65535, `bytesperglyph` equal to `height * ((width + 7) / 8)`,
total size ≤ 1 MiB, the promised glyphs all present, and a Unicode table
that is well-formed UTF-8 with an entry for every glyph. Overlong forms and
surrogates are malformed: a table that spells U+0041 three ways is claiming
'A' for a glyph its author hid.

## The door: `/sys/console/font`

A file, not a syscall — the `/proc/self/tty` doctrine (VIRTUAL_TERMINALS):
state a program sets by naming it is a file.

- **Write:** open "w", write the PSF2 image in as many pieces as you like,
  close. The bytes accumulate in a pending buffer owned by the handle and
  are JUDGED **at close**, which is `/sys/clipboard`'s shape and for the
  same reason: a half-written font must never reach the glass. A refused
  image changes nothing.
- **Judged at close, installed a moment later.** A sysfs close runs with
  that core's interrupts off, and it also runs inside the burial of a task
  that died holding the file. Neither is a place to allocate eight new
  grids and repaint a screen. So the close VALIDATES — parse, charmap, and
  whether the cell gives this screen a usable grid — and queues the result
  for **kworker**, which does the swap in task context with interrupts on.
  It is the arrangement a keystroke on a dark terminal already uses to get
  its shell, and the same early wake rousts the worker for both.
- **Read:** `source` (`boot` or `loaded`), `cell`, `glyphs`, and for a
  loaded face `table` and how many printable bytes of each set it left
  `unmapped`; the `screen` in pixels and the `grid` that cell makes of it
  (the dividend is published because `vtfont 100x40` chooses a size by
  it, and a quotient cannot be inverted to within a column); `pending`; and
  **`last`** — the verdict on the most recent offer, which is where a
  program learns WHY a font was refused (`refused: glyphs truncated (512)`)
  or that it is now on the glass (`installed: … 8 terminals reshaped, 1
  told; 0 history lines dropped, 0 rows clipped`). It is what a `vtfont`
  with no operands prints, and what it polls after a write. A swap reports
  there only while its offer is still the newest: if another font was
  offered while it ran, `last` stays with that one (whose refusal nothing
  would ever repeat), and `source`/`cell`/`grid` say what is on the glass.
- **Write the single word `boot`** to go back to the Limine-module face.
- **A cell has to leave a grid somebody can work at**: 40x10 at the least,
  512x256 at the most (the terminal's own fence) — or the grid the boot face
  made of this screen, where that is larger, because `boot` has to be able
  to come back. A 64x128 face on a 1024x768 screen is a valid font and a
  16x6 terminal, and is refused.

**One face for every VT**, in v1. The renderer is one object with one cell
size and eight terminals share it; per-VT faces would mean a cell size per
`tty_t` and a repaint that changes metrics on every VT switch. Nothing has
asked for it. (gterm windows are unaffected either way — their face is
fonts.conf's `terminal` role and lives in ring 3.)

## What happens at the swap

`console_font_sweep`, in kworker, in this order:

1. **The grids.** `tty_refont` on every VT — each under its own lock, one
   at a time, the focused terminal LAST. It plans the reflow, allocates
   outside the lock, plans again (output kept arriving), and swaps the ring
   in; the next section is what it preserves. It does not give up on a busy
   terminal — after a few passes it asks for the fence itself, which always
   fits. **All eight or none:** if a terminal still refuses, the ones
   already reshaped are put back and no face is installed.
2. **The face.** `renderer_face_install`, under `kRendererLock`, dealing
   with the text cursor first (hidden — or, while the GUI has the glass,
   dropped, since hiding would restore a strip of text terminal over the
   desktop): its save-under pixels are in the outgoing cell's
   geometry. Which glyph draws a byte was settled when the face was
   accepted — 2 x 256 bitmap pointers, a blank or a synthesized block where
   the face has nothing — so the blitter indexes and never decides.
3. **The picture.** Repaint whichever terminal has the glass NOW; focus may
   have moved during step 1.
4. Free the outgoing face, and raise SIGWINCH at every task seated on a VT
   that installed a handler, so husk redraws its line and scribe/top reflow
   exactly as they do in a gterm that was dragged.

Between 1 and 3 the focused terminal's cells and the face disagree, and a
write in that interval lands new-grid cells at old-face positions. It is
ugly for a few milliseconds and it is in bounds: every glyph write clips
to the framebuffer. Doing the focused terminal last is what keeps it short.

The outgoing face can be freed as soon as the install returns, because
every read of a glyph happens under the lock the install takes. **The boot
face is never freed at all**, and a panic goes back to it: `renderer_bust_lock`
reinstalls it before anything is drawn, overwriting the whole face struct so
a swap caught halfway cannot survive. A panic must not trust bytes that came
from ring 3. (Proved by loading a 16x32 face, filling the screen, and
injecting an NMI: the report arrives whole, in zap 8x16.)

**The one way that could still fail is an install in flight on another
core**: it holds the renderer lock with interrupts off, so the panic's freeze
cannot stop it, and it could write its face over the boot face the panic
just restored — or be halfway through the struct while the panic reads it.
So each side raises a flag of its own and THEN reads the other's, both with
locked instructions (a core's plain stores can wait in its store buffer past
its next read): at least one sees the other. An install that sees the panic
writes nothing; a panic that sees an install waits, bounded, for it to
finish before it writes or draws. An install turned away says so, and the
swap then frees neither face — the renderer may still hold the old one
until the panic has written its own. That argument is above
`renderer_bust_lock`; it is reasoned, not reproduced — the window is a
struct copy wide.

The GUI owns the glass on VT8; a swap while it is focused installs the face
and resizes the grids but skips the repaint, as any tty write does there.

## The contents survive — all of them (Chris's condition, 2026-09-20)

A font change shows the same screen and the same scrollback in a different
face. `tty_resize` cannot be the carrier, because its policy is the window
drag's: it copies `min(old cols, new cols)` cells of each line and the rest
is gone, and it sizes the ring as `rows * TTY_SCROLLBACK_SCREENS`, so fewer
rows means a shorter history. Both are right for a drag (the program inside
repaints) and wrong here, where the likeliest moment is just after boot
with the boot log on the glass and a larger face cutting 240 columns to 160.

`tty_reflow` shares `tty_resize`'s logical-line walk and replaces the
policy:

- **Narrower: a long line WRAPS onto continuation rows.** Trailing blank
  cells are not content and do not force a wrap.
- **One spare attribute bit marks a continuation row** (`attrs` uses two of
  its eight bits; the mark lives on the row's first cell). It is what makes
  the change reversible: going wider re-joins marked rows, so 16 → 24 → 16
  px returns the ring it started with. It is a kernel-side mark; the two
  ANSI attribute bits gterm reads are unchanged.
- **The reflow reports the LEAST ring that holds the content**, which going
  wider is fewer lines than the old ring had. Keeping the scrollback deep is
  the swap's job, not the reflow's: `tty_refont` allocates at least
  `rows * TTY_SCROLLBACK_SCREENS` whatever the plan asks for, because memory
  is the price and "grow rather than drop" is the house rule. Its one fence
  is `TTY_REFONT_MAX_LINES` (8192); history past that is dropped oldest
  first, counted, and named in the door's `last:` line.
- **The view stays on the line it was on.** Someone reading scrollback when
  the face changes keeps their place; `tty_resize`'s snap to the live
  screen is a drag's behaviour.
- **The cursor lands on the cell it was on**, which after a wrap may be a
  row further down.
- **Blank screen under the last text is not content.** Laying it out would
  push real lines into history to make room for nothing.
- **The screen never starts above where it started.** That would put
  history back on the live screen under a cursor that knows nothing of it.
  So when a narrower grid pushes the top of the screen into history, going
  wide again leaves it there: every line is still reachable with
  Shift+PgUp, but the live screen shows its lower part with blank rows
  beneath. A terminal cannot tell a screen that scrolled from one that was
  cleared, and only the first may be pulled back.

The reflow is `kernel/src/tty_reflow.c` — pure, two calls (`plan` counts
the lines so the caller can allocate, `run` lays them down), and held by
`tools/test_tty_reflow_host.sh` to the property rather than to pictures: a
grid is reduced to what a person would say is on it (the lines, trailing
bare paper trimmed, every cell with its colours and character set), and that
story must come out of every reflow unchanged, through chains of random
shapes and home again, with the cursor on the same cell of the same line.

**What it admits to losing, and counts.** `below_clipped`: text UNDER the
cursor that no longer fits beneath it on a shorter screen — a full-screen
program's picture, which that program repaints when SIGWINCH arrives. It
happens only with the cursor pinned to row 0, and the harness holds it to
that. `history_dropped`: oldest lines past the `max_total` the caller is
willing to pay for; the door chooses that number, and it is generous.

What it does not do: re-join lines the terminal wrapped while PRINTING
before this code existed — they were stored as separate rows with no mark.
They lose nothing; they stay in the pieces they were printed in. Marking
print-time wraps with the same bit is a follow-on, not part of this arc.

## Work, in slices

1. **The renderer stops knowing the number 8.** `FONT_WIDTH`/`FONT_HEIGHT`
   appear at 34 sites (29 in BasicRenderer.c, 2 in vt_select.c, the header).
   They become reads of the live face. The cursor save-under buffer and the
   glyph blitter's inner loop are the two that need thought rather than
   substitution: the blitter reads one byte per row today and must read
   `(width + 7) / 8`. No behaviour change; boots and looks identical.
2. **PSF2 loader + charset table builder**, as pure C with no kernel
   headers, so a `tools/test_psf2_host.sh` drives it under ASan against
   truncated, lying and oversized images before the kernel ever sees one —
   `ansi.c`'s arrangement, for `ansi.c`'s reason.
3. **`tty_reflow`: the reflow.** The wrap/re-join walk is written as a pure
   function over a cell array (old ring in, new ring out) so the host
   harness can drive it under ASan. Its acceptance test is the round trip:
   narrow, widen back, compare the rings cell for cell — with lines of
   every length around both column counts, a full history, a scrolled-back
   view and a cursor on a wrapped line.
4. **The door and the swap** (`console_font.c`, the sysfs node,
   `tty_refont`, `renderer_face_install`, the panic fallback). The
   fallback's acceptance test is a panic AFTER a font load, with the screen
   full — provoked with QEMU's `nmi`, since `TESTPANIC` fires at boot, before
   any face can be loaded. Five things it had to answer, found while
   reviewing the slices beneath it:
   - **The ring it allocates is its own policy.** `tty_reflow_plan` returns
     the least that holds the content; the swap asks for at least
     `rows * TTY_SCROLLBACK_SCREENS`.
   - **`view_offset` means the same thing on both sides.** The reflow
     refuses one larger than `hist_lines` and counts up from the live
     screen; `tty.c`'s field wants checking against that before it is
     passed.
   - **Hide the cursor before installing the face.** The save-under buffer
     holds pixels in the OLD cell's geometry, and `cursor_hide_locked`
     restores them through the NEW one.
   - **A face taller than the framebuffer makes `renderer_rows()` zero**
     and `scroll_framebuffer_full`'s `height - line_h` underflow. Nothing
     can produce that through the loader's fences; the door is where a
     height first becomes arbitrary, so it is the door's to refuse.
   - **`renderer_cell_w/h()` are unlocked scalar reads** for the text
     console's mouse. One mis-targeted click during a swap is the worst
     case, and that is the trade — a deliberate one, not an oversight.
5. **libos64: `os64_font_render_psf2`** (`os64/font_psf2.h`), written
   against the F1 backend table and nothing above it — a console font is
   one face and one glyph per code point, which is none of what the text
   layer exists for, and going straight to `lookup`/`render`/`glyph_view`
   is what lets the host harness inject a backend of its own. The cell is
   the face's advance by the face's line; glyphs 0..255 are Latin-1 in
   index order and the CP437 code points Latin-1 lacks follow; coverage is
   thresholded at the middle; a Unicode table names every glyph. **A code
   point the face lacks, or cannot draw at this size, or that the kernel
   draws better, is left OUT of the table**, so the kernel makes the block
   elements itself and draws the rest blank — a blank glyph that claimed to
   be the dark shade would be drawn as one (§ Known limits has the three
   rules). Refuses a proportional face, a fractional advance, a face that
   cannot draw all of printable ASCII at this size (naming the character),
   and a size whose cell the kernel would refuse (with the cell).
   `tools/test_font_psf2_host.py` hands what it writes to the kernel's own
   `psf2.c`: every pixel of every glyph of both sets against a fake backend
   with a known ink pattern, every allocation failed in turn, and with
   `--real` the fixture faces through FreeType at EVERY size from 8 to 72 —
   the six hand-picked sizes a first version checked all happened to dodge
   the breakage in between, which is the lesson. `/tests/psf2probe` is the
   worked example: face and size to `/sys/console/font`, or to a `.psf`
   file that `cp` can load later with no FreeType in the room.
6. **`vtfont`** (`userland/apps/vtfont`): `vtfont` (show), `vtfont boot`,
   `vtfont face.psf`, `vtfont DejaVuSansMono.ttf 24`, and **`vtfont
   DejaVuSansMono.ttf 100x40`** — the size a person actually has in mind is
   how much fits on the glass, so the grid form finds the LARGEST pixel
   size whose cell still gives this screen that many columns and rows
   (bisection over the sizes the kernel accepts; the cell grows with the
   size), and prints the size it settled on so it can be typed next time.
   A bare name is looked for in `/etc/fonts` with `.ttf`, `.otf` and `.psf`
   tried in turn. Which kind a file is comes from its first bytes, not its
   name; a bitmap asked for a grid answers with the grid it gives. What
   took is the KERNEL's to say: after the write it waits for the swap and
   prints the door's `last:` line, and its exit status is that verdict's —
   a failed close is not yet heard in ring 3 (DEBTS.md), and this is the
   program that does not need it to be. The door publishes `screen:` for
   it, because a grid search needs the dividend.

Slices 1–4 are kernel work on the glass and its lock, reachable from
interrupt and panic context. That is the category CLAUDE.md says earns an
outside review round; whether it gets one is Chris's call before slice 4
starts.

## Persistence

`console.conf` on the config ladder — `face = <name-or-path>` and `size =`
in the same two words `vtfont` takes at a prompt (`24`, or `100x40`; a size
beside a bitmap is ignored) — read by `vtfont --startup`, which the kernel
launches **right after logd, when the file resolves on the ladder**: the
earliest moment the root, `/home`, the ladder, kworker and the terminals
all exist, so every boot line from the post-boot tests on is in the chosen
face. (Beside cron it would buy nothing a crontab `@reboot` line does not;
this early it buys the readable boot, which at 1920x1080 is the point. The
lines before the root mounts stay in the boot face: that module is PSF1,
eight pixels wide by format, and teaching the module loader PSF2 is its own
slice.) The FILE is
the switch, not a boot token, because a token does not travel to a machine
that boots from its own disk, and every machine this is for does. Absent
file, and nothing is launched; a file with no `face` launches `vtfont`,
which reads it and keeps the boot face — silently for a file of comments,
and with a line naming every key it does not know for anything else,
because `font = …` is the natural slip and a boot that quietly stays 8x16
is the worst way to learn a key's name. No example file ships, for `mounts.conf`'s reason: a shipped copy
of comments would launch that no-op and announce it on every boot. The
whole file is

```
face = Uni2-Terminus24x12      # a name on /etc/fonts, or a path
size = 100x40                  # for an outline face: pixels, or the grid you want
```

Not a key in `fonts.conf`: that file's
decoder refuses unknown keys by design, and its three roles are ring-3
consumers with a transaction the console does not take part in. Not a kernel
reader either: the kernel asks the ladder only whether the file EXISTS,
because applying it may mean rendering an outline face, and FreeType lives
in ring 3.

The boot face therefore stays what every boot STARTS in, and the configured
one arrives before the post-boot tests print, with one line on the console
saying what took. A face that cannot load costs a line and nothing
else. The lifeboat honours the file too, if the ladder reaches a `/home`
that carries one: unlike cron, there is nothing a font can do behind your
back, and `vtfont boot` undoes it.

## Known limits, stated before anyone finds them

- **One bit per pixel.** The kernel blitter has no alpha, so coverage is
  cut at the middle, and a stroke thinner than a pixel that straddles two
  can vanish whole. Below about 11 px that takes printable ASCII with it
  (DejaVu's `'` and `|` at 10 px), and the converter REFUSES the size and
  names the character rather than ship a prompt with no bar in it; a glyph
  outside ASCII that vanishes is left unnamed and draws blank. From the
  mid-teens up the difference from a hand-drawn bitmap is cosmetic. An
  8-bit coverage format would be a PSF2 we invented and a second blitter;
  not proposed.
- **One face, no fallback chain.** A code point the chosen face lacks is
  not borrowed from another: the kernel synthesizes the blocks and the rest
  draw blank. DejaVu Sans Mono and Source Code Pro both carry all of CP437,
  so nothing has asked for more; the day a face with no box-drawing set is
  somebody's choice, fallback is F2's and the converter would move up to it.
- **Box-drawing joins take two rules, because the cell is not the line.**
  The backend delivers the ascender rounded up and the descender rounded
  down, each on its own, so the cell can stand a row or two taller than the
  line the face drew its box set to span (DejaVu at 26 px: a 30.3 px line
  in a 32-row cell), and the threshold drops the partly-covered edge rows —
  which broke every frame at 13 DejaVu sizes, 26 px among them, the P5's
  160x45. So, for the joining glyphs only: an edge pixel the face covered at
  all is ink when the pixel just inside it is ink (the face aimed the stroke
  at the edge; the rounding moved the edge), and a glyph that still does
  not reach an edge its name promises is left UNNAMED, because blank reads
  as missing and a frame with gaps reads as corruption. DejaVu at 9 px is
  the one size that rule still touches: eight box glyphs blank. Ship an
  outline face that draws its box set short of its own line and it will be
  blank frames, not broken ones.
- **The eight block elements are never the face's.** `█ ▀ ▄ ▌ ▐ ░ ▒ ▓` come
  through the threshold with seams between rows and shades at whatever
  density the designer liked, while CP437 art was drawn against the VGA
  ROM's dither — which is what `psf2_synth_block` reproduces, to measure,
  for any cell. The converter leaves them unnamed and the kernel draws its
  own.
- **A glyph can lose ink to the cell's edge**, and that is reported, not
  hidden: `info.clipped` counts only glyphs that do not join (DejaVu clips
  the tops of 19 accented capitals at 14 px, and half of Ñ's tilde at 42),
  and `psf2probe` prints it as a warning. Growing the cell to fit would
  change the line spacing at those sizes to save part of an accent.
- **No bold or italic face.** SGR bold stays a colour, as now.
- **A terminal draws 2 x 256 characters, whatever the face holds.** A cell
  is a byte and a set, so a 1,300-glyph Terminus shows the Latin-1 and CP437
  glyphs out of it and the rest are unreachable. That is the terminal's
  limit (no UTF-8 on the glass), not the loader's.

## The config program, and the TUI it is not

`vtfont` is a plain command over `console.conf`. A full-screen "os config"
program is a good idea with a missing prerequisite: os64 has three
full-screen programs (scribe, top, hexedit) that each hand-rolled their
screen handling, and a TUI toolkit should be lifted out of those three
rather than invented alongside a fourth. When that exists, the config
program is a face on files that already work — `console.conf` among them.
