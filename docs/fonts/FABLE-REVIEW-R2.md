# Font architecture review — Fable's verdict on R2

2026-09-16. Review of the F0 R2 candidate and the F1 candidate port against the
brief in [FABLE-REVIEW.md](FABLE-REVIEW.md). This is the design review the
brief asks for; it is not the F1 implementation review, and nothing here
authorizes a merge.

## What was reviewed, and what I ran

- Input manifest: `python3 tools/fonts/verify_review_inputs.py --f1
  /home/yogi/src/os64/.worktrees/freetype-backend` → `PASS: 74 selected F0/F1
  review inputs match the R2 snapshot`. Both trees at base `3b82356`, both
  uncommitted. No file in either tree changed during the review; this file is
  the only addition, and it is not on the manifest.
- Read: FONTS.md, F0-F1-DECISIONS.md, FONT_CONTRACTS.md, the three headers,
  FONTS_WORK_PLAN.md and all six packets, tools/fonts (fake backend, contract
  test, fixtures, checker); F1-REPORT.md, F1-REPLY-R1.md, UPSTREAM_REVIEW.md,
  FIXTURES.md, `port/backend.c` in full, `port/runtime.{h,c}`, `nonlocal.S`,
  `ftsystem.c`, `os64_ftoption.h`, `os64_ftmodule.h`, `shared.mk`,
  `exports.map`, the shared-build patch. Existing seams the design lands on:
  `kernel/src/shared_object.c` (scoped resolver, demand-paged cache),
  `abi/include/os64/appearance.h`, `kernel/src/appearance.c`,
  `userland/libos64/ui_session.c`, `ui_theme.c`, `conf.h`, `gterm.c`'s resize
  arm, `abi/include/os64/pty.h` and `charset.h`, Scribe's buffer.
- Ran myself, this machine, 2026-09-16:
  - F0: `python3 tools/test_font_contracts.py` → PASS backend table v1
    lifetimes/geometry/errors/allocation sweep; PASS 14 acceptance vectors;
    PASS strict freestanding target compilation.
  - F1: `python3 tools/test_freetype_host.py` (ASan+UBSan, -O2) → 943 checks,
    0 failures.
  - F1 leaf claim on the `.so` already built in that tree: `readelf -dW` shows
    SONAME only, no DT_NEEDED; `nm -D -u` empty; `nm -D --defined-only` is the
    one getter.
  - `diff` of F1's header copy against F0's R2 header: comment lines only, no
    declaration differs.
- Not run: F1's QEMU boots, the -O0 host pass, a fresh cross-build of F1, any
  P5 run. F1's preserved evidence was read, not reproduced.

## Verdicts

| Gate | Verdict |
|---|---|
| Backend | **APPROVED to freeze** at table revision 1, on one ruling (B1) written into contract, header comment, fake backend and F1 before the freeze commit is recorded. |
| Layout | **Declarations APPROVED to freeze** (`text.h`, `text_draw.h` as they stand). Rulings L1 and L2 go into FONT_CONTRACTS.md before F2 starts; L3 before F3 starts. W1 is accepted as the first slice; the unsupported-cluster picture is ruled (base glyph plus marker). |
| Configuration | **NOT APPROVED.** One blocking defect (C1). C2 is ruled (whole-file rejection). The fonts.conf grammar, path rule, Save rule and prepare-then-adopt rule are sound as proposed. |

Approving the backend closes nothing in layout or configuration. Approving the
layout declarations closes neither the terminal rule (L3) nor configuration.
Record the freeze commit for the backend only once B1 is in.

## Backend gate

### B1 — BLOCKING for freeze: the engine's own cap has two spellings

- **Trigger:** the engine memory cap (64 MiB default) is reached during
  `face_open` or `render`.
- **What each side does today:** the fake returns `LIMIT` without calling the
  allocator (`tools/fonts/fake_backend.c` `allocate`), and `contract_test.c`'s
  `failure_sweep` asserts exactly that: a budget refusal is `LIMIT` and the
  callback count is unchanged. F1's `engine_alloc` returns NULL for both a cap
  refusal and a callback refusal; FreeType turns that NULL into
  `FT_Err_Out_Of_Memory`, and `status_for` maps it to `NO_MEMORY`
  (`port/backend.c`). The contract only rules the creation case ("a positive
  cap too small for creation returns LIMIT") and then says limits and
  allocation refusal are distinct statuses.
- **Why it fails:** F2's cache decides between "evict and retry" and "fail the
  layout" on precisely this status. Written against the fake, F2 evicts on
  `LIMIT`; on the real engine a full cap arrives as `NO_MEMORY` and the layout
  fails while the cache holds masks it could have released. This is the
  mock-passes-engine-disagrees case the work plan names, and the moment to
  settle a status vocabulary is the freeze.
- **Correction:** pick one spelling and make both backends say it. My
  recommendation: `LIMIT` for the engine's own cap everywhere (the contract's
  word for a bound os64 chose), `NO_MEMORY` only when the callback returned
  NULL. F1's change is small: `engine_alloc` records why it refused, and
  `status_for` maps `Out_Of_Memory` through that record (cleared at each entry
  point). One F1 host check: fill the cap, render, expect `LIMIT` with the
  callback never called. One sentence on `memory_cap` in the header.

### Dependency direction — endorsed

`libfreetype.so` is a leaf; I verified it. `libos64 -> libfreetype` is a hard
`DT_NEEDED` edge with the `libpng -> libgzip` precedent, and the loader
resolves each object against itself and then its own dependencies in link
order (`shared_object_scoped_resolver`), so libos64's call to the getter
resolves and nothing inside libfreetype can reach libos64. A cycle is
impossible while libfreetype stays a leaf, and `--no-undefined` on its link is
what keeps it one: that flag is load-bearing and must survive integration.

The cost to a non-GUI process, read off the loader as it is today, is not the
418 KB. Library pages are demand-faulted into a cache shared by every task, so
`cat` touches none of them. What every spawn pays is closure work: one more
open of `/lib/libfreetype.so` with the identity revalidation the shared-object
arc added, one VMA, and one prelink slot (nine of sixty-four after this). F2's
measurement should be spawn latency of a trivial program before and after. If
it shows, the remedy is not a weak symbol (the loader has none, and no loader
change is in scope); the header already makes the table an injected input
(`os64_text_options_t.backend`), so the getter can be named from a GUI-only
entry point in libos64 rather than from the text core. Not blocking.

Hidden runtime: none. The port carries `memcpy`/`memset`/`memcmp`/`memmove`
hidden, and `-fno-tree-loop-distribute-patterns` is what stops GCC at -O2 from
turning the port's own `memcpy` loop into a call to itself (`shared.mk`). The
implementation review should keep a check that fails when that flag is
dropped, because the build succeeds without it and the loop only recurses at
run time. No mutable globals: `bss 0`, and `.data` is relocated `const` service
tables; separate engines are genuinely independent because each is its own
`FT_New_Library`. The private nonlocal return is armed and consumed inside one
upstream function, proven at -O2 under ASan; accepted as a private mechanism,
not a public `setjmp`.

### Lifetimes and budgets — sound

`render` copies coverage out of FreeType's single slot; `face_close` leaves
glyphs alone; the engine is `BUSY` while any glyph lives. F2 can therefore hold
masks in its cache with the engine owning the bytes and the context's
allocator counting them. Two consequences F2 must write down (notes, not
findings): the same bytes reach the context's allocator once, so F2 must not
add `engine_stats.live_bytes` to its own tally; and there are two thresholds
(engine cap, context cap), so F2 should pass its own total as the engine cap
and let one threshold fire.

### The six dispositions — accepted as contracts

1. `render(0)` refused, F2 draws its own marker: right, and it is what makes
   the marker's geometry independent of whichever font failed last.
2. GPOS coverage limited, zero ambiguous: right; the header says it now.
3. Unfitted kerning in both modes: right. The fact F2 needs beside it, from the
   pinned source: under `HINT_NORMAL` FreeType rounds glyph advances to whole
   pixels (`autofit/afloader.c:567`, `base/ftobjs.c:898`) while pair
   adjustments stay fractional, so pen positions are fractional in NORMAL
   mode too, and only `NONE` yields fractional advances. Nothing in the
   contract contradicts this; write it where F2 will read it.
4. Bytecode interpreter off, native CFF hinting kept: accepted on parser
   surface alone, with the two-hinter consequence documented.
5. Explicit `fvar`/`CFF2` probe: accepted. For the implementation review: a
   bare CFF file has no SFNT directory, so `FT_Load_Sfnt_Table` cannot see a
   table in it; confirm the CFF driver refuses a bare CFF2 in this
   configuration or the probe has a gap.
6. Name selection: accepted; F1's scoring matches the stated order.

### Optional (backend)

- `OS64_FONT_MASK_DIM_MAX` of 4096 at a 256-pixel em is a 16 MiB mask for one
  glyph; a bound of four em per axis (1 MiB at the largest size) keeps a
  hostile outline from pinning a quarter of the engine cap. Single-user
  machine, cap holds: preference, not a blocker.

## Layout gate

### L1 — ruling before F4: who owns row pitch

- **Trigger:** a 12-pixel document line containing one invalid byte, or one
  character served by a fallback face with a taller ascent.
- **Contract as written:** run ascent/descent are maxima of the participating
  faces including the marker's 8x16 metrics; line height is the maximum of
  participating line heights.
- **What fails if a consumer believes the run's box is its row:** that line is
  16 pixels tall and its neighbours 12; row pitch becomes content-dependent,
  and Up/Down with a remembered X, the scrollbar range and click-to-row all
  move when a marker appears. `os64_text_selection` returns the run's box, so
  a selection across lines has ragged rows.
- **Ruling to write:** the run's box belongs to the run. ROW PITCH belongs to
  the consumer and comes from the role's primary face (`fonts[0]` at its
  size); a run whose box exceeds the row is clipped to it, never re-pitched.
  F4 paints selection from the run's X values and the row's Y. No header
  change. Whether the marker keeps 8x16 or takes the primary em is cosmetic
  and optional.

### L2 — shared with B1

The engine-cap status vocabulary is the same ruling seen from F2's side.

### L3 — ruling before F3: box and block glyphs in a cell that is not 8x16

- **Trigger:** a user selects a terminal font that lacks U+2500..259F, at any
  cell size, and runs anything drawn in CP437.
- **Contract as written:** "existing procedural line/block glyphs remain
  available." What exists is `charset.h`'s five 8x16 bitmaps for the glyphs
  the console face lacks; the rest of CP437's high half maps to scalars the
  selected face may or may not carry.
- **What fails:** a missing box-drawing glyph falls back to the 8x16 bitmap
  face, centred in a 10x20 cell, and every join has a gap. The art form the
  CP437 charset exists for stops working the moment a font is chosen.
- **Ruling to write:** in grid mode, U+2500..257F and U+2580..259F missing
  from the selected face are drawn PROCEDURALLY at cell size (lines to the
  cell edges, shades as patterns) and never from the bitmap face; the marker
  is for everything else. It is an F2 grid-mode drawing rule that F3 consumes.

### W1 — accepted as the first slice

The decoder and cluster rules are precise and finite, and "an invalid byte is
a one-byte cluster; decoding resumes" is what keeps a damaged file editable
byte by byte. Two things to say out loud:

- **RULED by Chris, 2026-09-16:** an unsupported base-plus-mark cluster (the
  fixture `71 CC 81`, `q` with an acute) draws the BASE GLYPH followed by ONE
  marker for the mark sequence. It stays one editing cluster with the full
  source span and the same caret rules; only the picture changes, so the
  letter a person typed stays readable. The R2 text ("one visible marker per
  cluster") and the fixture vector "unsupported accent cluster" change to
  match: two placements sharing one cluster span, the base glyph at its own
  advance and the marker after it, byte span `[0,3)` on both. Not a header
  change.
- **Ambiguity to close in the text:** a non-Latin base (a digit, punctuation)
  followed by marks. Read literally the digit is not a base, so it renders and
  the marks are an isolated sequence drawn as a marker after it. Say so.

### Large lines

The one-line limits and "LIMIT, never a clipped prefix" are right for the RUN.
They are not an answer for the editor: a line the run refuses has no carets, so
it cannot be edited or split. F4's answer should be explicit windowed layout: a
bounded window of the line around the caret laid out as its own run with
visible "more" markers at its ends. That is not a silent prefix, it makes
editing inside a huge line possible, and it also bounds the per-keystroke cost
that the packet asks F4 to avoid. Deciding that before F4 starts is cheaper
than after.

### Notes for F2's implementer (not findings)

- The tab formula needs `floor`; C's `/` truncates toward zero, and a pen left
  of the origin is legal. The Python checker uses `//`.
- A caret's pixel X must use the same `floor((x+32)/64)` as paint or carets
  sit one pixel off glyph edges at some fractions.
- `os64_text_placement_t` is 48 bytes; a 1 MiB-glyph run is 48 MiB of
  placements plus 16 MiB of carets. That is inside the caps but not twice.
- Fallback faces opened at a size other than the primary's are legal by the
  header; F5's provider must open them at the role size or baselines will be
  right and x-heights wrong.

### Fixtures

The fourteen vectors are arithmetic consistency; they become an oracle when F2
runs them. The repeated-fractional-pairs vector and the recorded mutation check
are the right kind of test. F2 must add the generated W1 composition table from
the pinned Unicode data, overflow, clipping at surface edges, and cap
exhaustion with eviction.

## Configuration gate

### C1 — BLOCKING: "add `fonts.*` keys to the shared envelope" is refused by every reader that exists

- **Evidence, current tree:** `ui_theme.c`'s `theme_setting` sets `bad` for
  ANY key not in `kThemeKeys` and stops; `os64_ui_theme_decode_session`
  returns `BAD_SETTING`; `ui_session.c`'s `refresh_locked` then answers
  `APPLY_INVALID` and the reader keeps its last usable generation. Writers
  re-encode the whole payload from the struct (`os64_ui_theme_apply` →
  `os64_ui_theme_encode_session`), so a writer that does not know fonts DROPS
  every font line it did not decode.
- **Trigger:** one process publishes `fonts.ui.face = ...`. Every other GUI
  process on today's decoder stops following palette changes for the rest of
  the session, and the Workshop's next palette Apply resets the fonts to
  startup. "Workshop updates preserve font fields" cannot be true of the
  writer as it stands.
- **Why it is architectural:** every reader and writer is the one resident
  `libos64.so`, so a new libos64 fixes both halves at once. But the
  shared-object arc keeps a RETIRED libos64 alive in every process started
  before a refresh (the desktop, gterm, the Workshop), so mixed old/new is the
  normal state after `os64 refresh` until reboot. And the next envelope
  extension after fonts repeats this unless the envelope has a rule.
- **Correction, the compatibility and rollout rule the brief asks for:**
  1. **Envelope rule, in `ui_session`:** the store is LINES. A reader accepts
     any well-formed `key = value` line, applies the keys it knows and ignores
     unknown keys under a dotted namespace. A writer publishes by
     read-modify-write: take the current payload at the expected generation,
     replace only the lines of the component it owns (palette, treatment,
     fonts) and carry every other line through verbatim. Keep today's
     strictness for malformed lines and for the required palette keys of a
     full snapshot. Compare-and-publish stays atomic; no kernel change.
  2. **Rollout rule:** the first libos64 carrying the fonts envelope must be
     running in every appearance participant before anyone publishes
     `fonts.*`. Concretely, reboot after the refresh that lands it. Say so in
     the handoff and in APPEARANCE.md. After that, rule 1 makes the extension
     after fonts safe.
  3. **Size:** today's envelope is sixteen lines (fourteen colours plus two
     session metrics), roughly 400 bytes. Fonts add
     at most three roles of one face path, one size and two fallback paths at
     255 bytes each, about 2.4 KB worst case. It fits the 4096-byte payload
     with room; keep "validate the whole envelope against the cap" as the
     rule. No new transport.

### C2 — RULED by Chris, 2026-09-16: whole-file rejection stands

One bad line in `fonts.conf` (a typo in `document.fallback.2`) rejects the
whole candidate: all three roles use the compiled 8x16 defaults at startup, or
the previous usable state on a live change, and the diagnostic names the line.
This is the house doctrine (logd.conf, gui.conf: a broken file is a broken
file, loudly), and the terminal you would fix it with still works at 8x16.
"Roles are independent" in the contract means no hidden inheritance between
roles, not per-role acceptance of a broken file; the R2 text should say that
in one sentence so the two rules are not read as a contradiction.

### Sound as proposed

- **Ladder and paths:** `fonts.conf` through `os64_conf_find`; relative paths
  against the directory of the file that answered; `conf_find` refuses a `/`,
  so no slash path ever enters the resolver; Save writes the top of the
  ladder with ABSOLUTE paths, so moving the file cannot retarget it. The
  ladder survives Save.
- **Identity:** content identity per open, a copy per open, explicit reload.
  The cost is one copy of each font file per process (a few hundred KB per
  face); accepted.
- **Prepare then adopt:** with the PTY resize as the terminal's commit point.
  gterm's resize arm already treats a refusal as "keep the old grid"; font
  adoption has to be one transaction with it, and the packet says so.
- **Preparation can fail after publication:** honest. The process keeps its
  old state and there is no channel back to the Workshop, so the Workshop
  must not claim "applied everywhere". A product note, not a defect.

## Answers to the brief's table, in one line each

- Dependency direction: yes, as a leaf below libos64; cost is per-spawn
  closure work, to be measured (B, above).
- Lifetimes and limits: yes, once B1 gives the cap one spelling.
- Missing characters: yes; the marker is F2's and needs no `.notdef`.
- Fractional positioning: yes; NORMAL mode gives integer advances plus
  fractional kerning, NONE gives fractional both; paint rounds once.
- Encoding and editing: W1 accepted; unsupported clusters draw base plus
  marker (ruled); one ambiguity to close.
- Large input: the run rule is right; the editor needs windowed layout (L,
  above).
- Terminal: yes, with L3 written before F3.
- Names and selection: yes; labels are copies, never identities.
- Startup and live settings: the ladder survives Save; the envelope does not
  survive today's decoder (C1).

## What this review does not establish

No P5 run, no CPU-time bound for a hostile font, and a designed corpus is not
a search. The F1 implementation review remains, and it now carries: the B1
change, the bare-CFF2 question, the `-fno-tree-loop-distribute-patterns`
tripwire, and the surrogate, odd-length, truncation and duplicate-name
fixtures DECISIONS §6 already lists. The four `.log` evidence files are still
ignored by the repository-wide rule; whoever commits F1 must add them
explicitly.

— Fable 5.1
