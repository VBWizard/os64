# CONSOLE_FONTS.md — changing the face a virtual terminal draws with

**Status: slices 1 to 3 of 6 are built** (§ Work, in slices); the door and
the swap are design. A section moves into the present tense
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
  are validated and installed **at close**, which is `/sys/clipboard`'s
  shape and for the same reason: a half-written font must never reach the
  glass. A refused image changes nothing and the close reports the refusal;
  the reason (which limit, and the number that broke it) is logged under
  `DEBUG_SYSTEM`.
- **Read:** one line of text — source (`boot` or `loaded`), cell `WxH`,
  glyph count, whether a Unicode table came with it, and the grid that cell
  gives this screen. It is what a `vtfont` with no operands prints.
- **Write the single word `boot`** to go back to the Limine-module face.

**One face for every VT**, in v1. The renderer is one object with one cell
size and eight terminals share it; per-VT faces would mean a cell size per
`tty_t` and a repaint that changes metrics on every VT switch. Nothing has
asked for it. (gterm windows are unaffected either way — their face is
fonts.conf's `terminal` role and lives in ring 3.)

## What happens at the swap

Under `kRendererLock`, in this order:

1. Install the new glyph pointer, width, height and charset table.
2. Recompute `renderer_cols()` / `renderer_rows()` from the framebuffer.
3. Drop the lock; carry every VT's grid across with `tty_reflow` (next
   section), which keeps ALL of it, then raise SIGWINCH at every seated
   task that installed a handler, so husk redraws its line and scribe/top
   reflow exactly as they do in a gterm that was dragged. The tty-side
   glue that allocates the new ring and calls the reflow under `t->lock`
   is `tty_refont`, and it is what slice 4 builds.
4. Full repaint of the focused VT.

The old face's memory is freed only after step 4, and **the boot face is
never freed at all**: `panic()` and `renderer_bust_lock` switch the renderer
back to it before drawing. A panic must not trust bytes that came from
ring 3, and must not find a half-installed face if it lands mid-swap.

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
  the swap's job, not the reflow's: it allocates at least
  `rows * TTY_SCROLLBACK_SCREENS` whatever the plan asks for, because memory
  is the price and "grow rather than drop" is the house rule.
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
4. **The door and the swap** (sysfs node, pending buffer, the sequence
   above, the panic fallback). `TESTPANIC` after a font load is the
   acceptance test for the fallback. Five things it has to answer, found
   while reviewing the slices beneath it:
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
5. **libos64: `os64_font_render_psf2(face bytes, pixel height, out, cap)`**
   on top of F1/F2. Picks the cell from the face's advance and line
   metrics, renders code points for the 256 Latin-1 slots plus the CP437
   set, thresholds each F2 coverage mask to one bit, and writes the Unicode
   table. Refuses a proportional face the way the provider's terminal role
   already does. Host-tested against the fixture faces.
6. **`vtfont`** — Chris's program. `vtfont` (show), `vtfont face.psf`,
   `vtfont DejaVuSansMono.ttf 24`, `vtfont boot`. Slice 5 ships with a
   ten-line example that does the second and third.

Slices 1–4 are kernel work on the glass and its lock, reachable from
interrupt and panic context. That is the category CLAUDE.md says earns an
outside review round; whether it gets one is Chris's call before slice 4
starts.

## Persistence

`console.conf` on the config ladder (`face = <path>`, `size = <pixels>`,
size ignored for a `.psf`), read by `vtfont --startup`, launched once at
boot beside logd and cron. Not a key in `fonts.conf`: that file's decoder
refuses unknown keys by design, and its three roles are ring-3 consumers
with a transaction the console does not take part in. Not a kernel reader
either: rendering an outline face needs FreeType, so the thing that applies
the setting has to live in ring 3 regardless.

The boot face therefore stays what every boot STARTS in, and the configured
one arrives a moment after userland is up. The lifeboat entry does not
launch it.

## Known limits, stated before anyone finds them

- **One bit per pixel.** The kernel blitter has no alpha, so an outline
  face at 16 px renders rougher than zap, which was drawn for that grid by
  hand. From about 22 px up the difference stops mattering, and that is the
  range anyone reaches for an outline face in. An 8-bit coverage format
  would be a PSF2 we invented and a second blitter; not proposed.
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
