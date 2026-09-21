# Console fonts — Opus's review of slices 1 to 3

2026-09-20. Review of the runtime-console-font work on `fable/console-fonts`
against the design in [CONSOLE_FONTS.md](CONSOLE_FONTS.md): commits `296cc88`
(slice 1, the cell comes from the face) and `f3cd93e` (slice 2, the PSF2
loader), plus the uncommitted slice 3 (`tty_cell.h`, `tty_reflow.{h,c}`,
`tools/test_tty_reflow_host.{c,sh}`, the `tty.h` and `CONSOLE_FONTS.md`
edits). Branch base `9f971f8`.

One finding, with a fix I built and verified. Everything else is text or a
note for slice 4.

**This is a snapshot, not a document anyone maintains.** It describes the
three slices as they stood on the date above. Once finding 1 is applied it
is history; take the reasoning from here and the facts from the code.

## What was reviewed, and what I ran

- Read in full: `CONSOLE_FONTS.md`, both commit diffs, `psf2.{h,c}`,
  `tty_cell.h`, `tty_reflow.{h,c}`, both host harnesses, and the seams the
  work lands on — `BasicRenderer.{h,c}`, `vt_select.c`'s `cell_at`,
  `abi/include/os64/charset.h` and `ansi.h`, `tty.c`'s `attrs` consumers,
  `kernel/GNUmakefile`'s source globbing.
- Ran myself, this machine, 2026-09-20:
  - `tools/test_psf2_host.sh` → 31,371 checks, 0 failures; 221 PSF2 fonts
    from `/usr/share/consolefonts` loaded, 235 PSF1 skipped.
  - `tools/test_tty_reflow_host.sh` → 399,908 checks, 0 failures, 25 chains
    ended at an admitted clip.
  - `tools/test_ansi_host.sh` → `test_husk_prompt_host`, `test_husk_line_host`
    and `test_ansi_host` all pass, which covers the margins harness slice 1
    rewrote.
  - `make -C kernel` → up to date; `obj/psf2.c.o` and `obj/tty_reflow.c.o`
    both present, so `find src -name '*.c'` is picking the new files up.
  - `tools/stale_refs.sh` → two prose hits (`OS64_ANSI_ATTR_`,
    `OS64_CHARSET_`), both false: the names moved to `tty_cell.h` and still
    exist in the ABI headers.
  - QEMU (`tools/vmboot`): boots to the husk prompt, pre-boot tests 31
    passed / 0 failed, post-boot 32 passed / 0 failed, the glass is zap 8x16
    at 128 columns exactly as before. `/tests/ansiprobe cp437` draws both
    halves both ways — the shades, box-drawing and half blocks all render,
    which is the check the rewritten blitter inner loop needed.
  - My own probe (ASan + UBSan, `-O1`): 540,003 checks over 60,000 rounds of
    random grids with `TTY_ATTR_WRAPPED` injected into the **input**, which
    the shipped harness never does.
  - The synth-block claim, against the actual boot face: dumped
    `external/zap-light16.psf` glyphs 203, 160 and 219 (what
    `os64_charset_entry` maps CP437 `0xB0`/`0xB1`/`0xDB` to).
- Not run: the P5, a UEFI boot, a font actually loaded through the door —
  there is no door yet.

## Finding

### 1. `walk()` places the screen anchor before the cursor grows `need`, putting a history row on the live screen

`kernel/src/tty_reflow.c:69-83`.

The cursor's `need` growth happens *inside* the anchor loop, and `A_SCREEN`
is enum-ordered ahead of `A_CURSOR`. So when the screen's top row is a
**blank continuation row**, `A_SCREEN` is clamped by
`if (r >= need) r = need - 1` against a `need` that `A_CURSOR` is about to
enlarge, and the screen starts one row too high.

Reproduced:

    old: cols=10, logical 0 = "0123456789", logical 1 = blank+WRAPPED,
         hist_lines=1, cursor (0,0)
    reflow to 5 columns:
      row 0: 01234
      row 1: 56789  [wrapped]   <- screen top, cur_row=1
      row 2: .....  [wrapped]

`hist_lines=1, cur_row=1`. `56789` was history in the old grid and is now on
the live screen — which is what `tty_reflow.c:161` forbids in its own words,
"Never above where it started before — that would put history back on the
live screen". It is not only cosmetic: the next thing that paints or scrolls
consumes that row, so a history line is destroyed.

The trigger is a blank marked row at the screen top — a prompt whose cursor
parks just past the end of a wrapped line, then the head of that paragraph
ages into history, which is exactly the state § The contents survive says the
design deliberately leaves behind — **and** a paragraph length that is an
exact multiple of the new column count. Roughly one in `nc` per swap, on a
state that arises from ordinary use.

Hoisting the growth above the loop fixes it, and the `a == A_CURSOR` special
case goes away with it:

```c
// The cursor has to land on a row that exists, even where it sits past the
// end of the text (a prompt's trailing space). Asked BEFORE any anchor is
// placed: an anchor clamped against a need the cursor is about to grow
// lands on a row that is not its own.
anchor_t *cu = &lay->anchor[A_CURSOR];
if (!cu->placed && cu->row >= p0 && cu->row <= p1) {
        uint64_t off = (uint64_t)(cu->row - p0) * in->cols + cu->col;
        if (off / nc + 1 > need)
                need = off / nc + 1;
}
```

With that in place the case above gives `hist_lines=2, cur_row=0` — `56789`
stays in history — the shipped harness holds at 399,908 checks / 0 failures,
and my probe goes clean. I reverted the patch; the worktree is as I found it.

**Why the harness cannot see it, which is the more useful half.**
`same_story` derives the cursor as `hist + cur_row`, and this error shifts
both by one, so it cancels exactly. Two other gaps of the same shape are
worth closing while the case is being added as a fixture: the initial random
grids in `test_chains` never carry `TTY_ATTR_WRAPPED` (only post-hop ones
do), and `g.cur_row` is always the last row with text on it.

## Text, and one thing to move

- **The synth-block attribution is half true**, in § Why PSF2 and in
  `f3cd93e`'s message: "at 8x16 they are byte-identical to the bitmaps
  `os64/charset.h` ships". Only five are — `builtin[5]` is the dark shade and
  the four halves. The light shade, the medium shade and the full block come
  from **zap's own glyphs 203, 160 and 219**, which are `88 22…`, `AA 55…`
  and `FF…`; `psf2_synth_block` matches all three byte for byte. So the
  property is true, and in fact stronger than claimed — the continuity is
  with the glass, not with one header. It is worth correcting because someone
  spot-checking `charset.h` for U+2591 will not find it, and because
  `test_synth` pins those three to literals with nothing tying them back to
  the face.
- **`tty_refont` is named in the present tense and does not exist.**
  § The contents survive says "`tty_refont` shares `tty_resize`'s
  logical-line walk and replaces the policy"; the status line says slices 1
  to 3 are built, so the section reads as describing shipped code. What
  shipped is `tty_reflow.c`; `tty_refont` is slice 4.
- **"History capacity never shrinks" belongs to slice 4's allocator, not to
  the reflow.** `tty_reflow.h:49` is explicit that `lines_needed` is *the
  least* total that holds the content, and going **wider** that is far fewer
  lines than the old ring held. The bullet is a promise the swap has to keep
  (allocate at least `rows * TTY_SCROLLBACK_SCREENS`); today it reads as
  something the built code already does.
- **`print_at` reads `face.width`/`face.height` before taking
  `kRendererLock`** (`BasicRenderer.c:374-376`). Free today, because the face
  is immutable. Once slice 4 can swap it, `print_at` lays out on the old cell
  while `put_char` inside the lock draws with the new one — cosmetic only,
  since `put_char_colors` clips to the framebuffer, but it is two lines to
  move now rather than a puzzle later. Every other glyph-byte read is
  properly under the lock; I walked all of them.

## Not findings — what I checked and found sound

- **`psf2.c` cannot read outside its image.** `glyph_end` is bounded before
  `glyphs` is published, every product is bounded by a check above it,
  `utf8_one`'s `(end - p) <= need` is the right inequality, and overlong
  forms, surrogates and anything past U+10FFFF are all refused. A flagged
  table of zero length falls through to `build_charmap` and is refused there
  by the right name, as the comment says.
- **The reflow is memory-safe.** Writes are bounded by
  `fresh_total >= hist + nr`; reads are `(base + logical) % total_lines` with
  `logical <= hist_lines + rows - 1`, which the input validation makes fit.
  60,000 rounds with marks in the input found nothing beyond finding 1.
- **`below_clipped` really does imply `cur_row == 0`** — it follows from
  `top = min(total - nr, cur)`, so the doc's claim and the harness's
  assertion are both right.
- **`cur_row < nr` and `save_row < nr` always hold**, including down the
  `A_SAVED` fallback path, which is the one that can hand back a `new_row`
  past `new_rows_total`.
- **`TTY_ATTR_WRAPPED` = 0x80 is genuinely free and invisible to painting.**
  `os64_ansi_apply_attrs` tests only `REVERSE`, and VT cells never cross to
  ring 3 — `os64_pty_cell_t` is tied to `tty_cell_t` by a layout assert
  alone, so the mark stays kernel-side as the header claims.
- **Slice 1 is a faithful substitution.** `nglyphs`, `glyph_bytes` and the
  `charsize != 16` guard all carry the values they carried before; the
  `face->width != 8` bail ahead of `os64_charset_glyph` is the right guard
  for the byte-count sniffing that function does; `s_cursorSave` is sized and
  strided for the 64-pixel fence the two static asserts pin together. The
  QEMU boot and the CP437 probe confirm it on the glass.

## For slice 4

Not findings against this diff — things the door and the swap have to answer,
written down here so they are not rediscovered:

- **`max_total` has to be generous enough that `lines_needed` does not shrink
  the scrollback below `rows * TTY_SCROLLBACK_SCREENS`.** The reflow returns
  the minimum that holds the content; the ring policy is the caller's.
- **`view_offset` has to mean the same thing on both sides of the call.** The
  reflow refuses `view_offset > hist_lines` and counts up from the live
  screen; `tty.c`'s field wants checking against that before it is passed.
- **A face taller than the framebuffer makes `renderer_rows()` zero**, and
  `scroll_framebuffer_full`'s `height - line_h` underflows. Nothing can
  produce that today (`PSF2_CELL_H_MAX` is 128), but the door is where a
  height first becomes arbitrary.
- **`renderer_cell_w()`/`renderer_cell_h()` are unlocked scalar reads** for
  `vt_select.c`'s mouse. Worst case is one mis-targeted click during a swap,
  which is probably the right trade — but it is a deliberate one now.

## Verdict

Slice 1 is a clean substitution and is proven on the glass. Slice 2 is the
strongest piece of the three: pure, fenced, and held by a harness that feeds
it every prefix of a good font, 20,000 rounds of damage and 221 real fonts
off the shelf. Slice 3 is good work with one ordering bug that its own
harness is structurally unable to see.

Finding 1 is a real ring-0 correctness bug in a pure function, with a
two-line fix I verified; the rest is text. With that applied and the case
added as a fixture, I would call slices 1 to 3 done in-house. Slice 4 is the
one that earns an outside round — that is where the lock, the panic fallback
and the `max_total` choice land, and it is the category CLAUDE.md names.
