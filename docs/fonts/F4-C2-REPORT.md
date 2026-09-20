# F4 checkpoint 2 — the editor on measured runs, and the real Scribe under test

2026-09-19, Opus. Worktree `.worktrees/font-widgets`, branch `opus/font-widgets`,
built on the C1 boundary Quinn accepted at `f76093c` (recorded as `d72dddf`).

Quinn’s review of e289ca3 is recorded in
[F4-C2-QUINN-REVIEW.md](F4-C2-QUINN-REVIEW.md), with reproduced findings,
correction requirements and runnable host evidence.

**Quinn C2 acceptance (2026-09-19), 4cee352:**
[current disposition and evidence](F4-C2-QUINN-REVIEW.md#c2-acceptance--4cee352).
All eight findings, including R6's transient-recovery follow-up, are resolved.
No new findings in this pass. Opus may proceed to C3 from the accepted commit
on `opus/font-widgets`. This is checkpoint acceptance, not F4 completion or
merge authorization.
All five [previous rulings](F4-C2-QUINN-REVIEW.md#answers-to-all-five-requested-rulings)
stand.

> **Ready for acceptance review.** Every finding across the three rounds
> (C2-R1..R8, and the R6 follow-up) reproduced here from Quinn's own runner,
> with her numbers, before anything changed. All are corrected in the
> commit that carries this revision of the report, which sits on top of her
> review records. The body below describes e289ca3 as submitted. **After the
> review** covers round one, **After the re-review** covers R6–R8, and
> **After the second re-review**, at the foot, covers the follow-up and how
> to rerun everything. Her rulings on the five questions are applied as
> given.

**Not F4 complete, and not asking to be merged.** C3 — the bounded window for
lines over 1 MiB — is next. `F4-REPORT.md` follows at completion.

## What is delivered

**The textview's coordinate system is pixels.** It used to be visual columns:
a tab was eight of them and every other byte one, and `vcol_of`/`byte_of`
converted between that space and byte offsets. Under a proportional face a
column is not a length — `iiii` and `WWWW` are the same four columns and very
different distances — so that machinery is deleted and the run answers every
question instead. `left` became `left_px`, `goal_vcol` became `goal_x`.

**One run answers the caret, the hit-test, the selection and the step**, through
four accessors on the run the view paints (`os64_ui_run_caret`, `_hit`,
`_selection`, `_step`). That is what makes it impossible for the caret, the
highlight and the glyphs to describe different text. A selected line is drawn
twice from that one run — ordinary colours, then the selection colours clipped
to its span — so the second pass costs no layout.

**Motion is by cluster.** Left, Right, Backspace and Delete step between the
boundaries F2 publishes, so a caret never lands inside `é` and Backspace removes
the whole letter. That now holds in the textfield too, whose Backspace used to
remove one byte — tearing a two-byte UTF-8 letter in half from a single key.

**The view a font change will show is worked out in preparation.** Every pixel
position the editor holds is in the units of the face being retired; the
caret's byte offset is the one position a face change cannot disturb, so
`textview_prepare` recomputes the view around it — vertical scroll, horizontal
scroll and the remembered Up/Down X — and stages runs for the lines that view
will show. Commit only moves those into place. That is why the first paint
after a font change allocates nothing even when the view has to move.

**Scribe has an adoption planner**, which is production code F5 will call, not a
test hook. `compute_layout` is one pure function that both the live layout and
the planner use, so the window a font change stages is the window a resize
would draw. Buttons are sized from their measured captions — the old fixed
60/60/84 pixels were written for an 8x16 cell, and "Save As" at 24 pixels is
wider than 84 on its own. When a face gets bigger the **status line** gives way
first, because it is the one thing on that row whose text can be cut without
losing a control. When even the buttons will not fit, the answer is LIMIT: a
window that hides its own Save button to make a font fit has not fit it.

**A font change while help is open** was the case Quinn named, and it is subtler
than it looks. The document waiting behind the help page has a saved scroll and
extent in the pixels of a face that is gone by the time help closes. Its caret
and selection are byte positions and come back exactly; `help_leave` measures
the rest afresh rather than restoring numbers in the wrong units.

## The fixture: the real Scribe, not a lookalike

Quinn's ruling was that `/tests/scribefonttest` drive the actual Scribe. So
scribe's `main` became `scribe_main(argc, argv, hooks)`, and `/bin/scribe` is
now a two-line `scribe_main.c` that passes no hooks. The fixture links scribe's
own objects (the `sshtest` precedent in `userland/GNUmakefile`) and passes hooks
of its own: `ready` runs once the window and document exist, and `key` is
offered each key ahead of scribe's shortcuts. **Production scribe has no test
keys at all** — they are unreachable from its main.

- `scribefonttest --selftest [file]` drives itself and prints PASS/FAIL.
- `scribefonttest [file]` is yours: **Alt+1..4** builtin / DejaVu Sans / Source
  Sans 3 / Source Code Pro, **Alt+= / Alt+-** size in steps of 4, **Alt+b** a
  96-pixel face scribe's planner must refuse.

The self-test makes its edits by dispatching key events through `os64_ui_dispatch`
— the editor's own front door — rather than writing the buffer behind its back.
Its checks cover all four of Quinn's cases:

| Case | What is checked |
|---|---|
| Switching with unsaved edits | bytes, caret, selection, focus and the unsaved flag identical across two switches to different families |
| Widest-line shrinkage | a smaller face gives a smaller extent, and that extent equals a fresh measurement of every line |
| Help, and a font change while it is open | help re-measured under the new face; on return the document's caret and bytes exact and its extent re-measured |
| Refused adoption | a 96-pixel face returns **LIMIT** specifically — scribe's layout, not something upstream — with the editor unchanged and still taking typing |

Plus **byte-identical save**: an unchanged file loaded and saved through
scribe's own `sbuf_load`/`sbuf_save` compares equal to the original.

## Evidence

**Guest** (`/QEMU GUI Boot`, ext2 root, headless with screendumps):

| Evidence | Shows |
|---|---|
| `f4-evidence/c2/selftest-pass.png` | `scribefonttest: PASS, 33 checks, 0 failed` |
| `f4-evidence/c2/selftest-catches-mutation.png` | **the fixture fails when it should** — with the selection deliberately dropped at commit it reports `FAIL, 33 checks, 2 failed`, naming the two cases that guard it |
| `f4-evidence/c2/production-scribe-builtin.png` | `/bin/scribe` starting on the builtin face with a real 187-line document — the production smoke test |
| `f4-evidence/c2/scribe-dejavu20.png` | the real scribe under DejaVu Sans 20: buttons re-measured, the status line giving way, the horizontal bar re-measured in pixels |

The mutation run matters more than the pass: a fixture that cannot fail is not
evidence. It was reverted immediately after.

**Host:**

```sh
ASAN_OPTIONS=detect_leaks=0 python3 tools/test_ui_text_host.py --real        # 377 checks
ASAN_OPTIONS=detect_leaks=0 python3 tools/test_ui_text_host.py --real -O 0   # 377 checks
ASAN_OPTIONS=detect_leaks=0 python3 tools/test_ui_text_host.py               # 75 checks
python3 docs/fonts/f4-evidence/c1-review/r4-run.py --output <dir>            # Quinn's 134, all pass
bash tools/test_appearance_host.sh
ASAN_OPTIONS=detect_leaks=0 python3 tools/test_text_host.py                  # 7,161 checks (F2)
```

293 → 377 checks. The editor's: every legal boundary on `iiii WWWW` round-trips
through caret → hit → caret; `WWWW` is measurably wider than `iiii`; `é` steps two
bytes as one cluster; adoption stages the visible lines and the caret's, so the
real textview painter allocates nothing on its first paint; a textfield
Backspace over `é` removes both bytes and nothing else, and a click lands on a
published boundary.

All of Quinn's C1 probes rerun against this tree and still hold — her round-4
supplemental suite included, which confirms the C1 closures survived C2.

`make`, `make fsck-ext2`, `git diff --check` and `tools/stale_refs.sh` are clean.

**Not run:** P5 / real hardware.

## A bug Chris found in testing, and why the suite missed it

With a selection ending mid-line, the WHOLE line took the selection colour,
while only the selected characters took the selection's ink — the unselected
tail showed ordinary black glyphs on a blue highlight
(`f4-evidence/c2/selection-bug-as-reported.png`). The old per-byte painter lit
only selected bytes, so this was new in C2.

The cause was the first of the two paint passes. It filled the line's box with
`lit ? text_sel_bg : text_bg`, written to stop it painting over a highlight
pre-filled just above it — but the second pass already fills the selected span
with the selection colour itself, so the pre-fill was never needed, and the
workaround for it painted the whole line. The pre-fill is gone and the first
pass always uses the paper's own colour.

**The suite had a selection test and did not catch it**, because it checked the
selection RECTANGLE's arithmetic and never looked at pixels past the selection's
end — which is exactly where the bug lived. The new check paints a line selected
up to its middle and requires the highlight colour inside the span and none
after it. It was confirmed to fail against the old line and pass against the
fix (376 checks with one failure, then 377 clean). Fixed on the glass in both
faces: `selection-fixed-builtin.png`, `selection-fixed-dejavu.png`.

The bug reached both programs that use the widget — scribe, and the Appearance
Workshop's preview pane — and so does the fix.

## Shared hunks

- `userland/GNUmakefile` — one `APP_EXTRA_OBJS` line linking scribe's objects
  into `scribefonttest`.
- `tools/test_appearance_host.c` / `test_appearance_customizer_host.c` — release
  the fonts their UIs acquire (a textview now retains a run for its caret's
  line), and one field rename (`left` → `left_px`).
- Quinn's review runners (`run.py`, `r2-run.py`, `r3-run.py`, `r4-run.py`) link
  `ui_text.c`, because the key decoder they reached through a harness stub
  lives there and the stub is gone.

## Known, and deliberately not done here

- **Lines over 1 MiB** are C3: the bounded source window, W1-boundary window
  ends, non-document "more" indicators and lazy extents.
- **The whole-document extent scan is unmeasured on a large file.**
  `measure_extent` (at e289ca3, `measure_widest`) lays out every line once —
  at load, after a paste, and, since the corrections, in a font change's
  plan. It is correct and it is what scribe has always done in columns;
  whether it is fast enough on a multi-megabyte log is a number I have not
  taken. C3's lazy extent is the natural place to replace it, and I would
  rather measure first.
- ~~`os64_ui_textfield_set` truncates at `cap - 1` bytes~~ — fixed in the
  corrections: it now cuts at the last cluster boundary that fits.
- **Widgets retain runs now**, and every leak-checked harness that builds a UI
  has had to release its fonts. That has come up three times; C3 will be the
  fourth.

## After the review

Quinn filed five P2 findings against e289ca3. Each reproduced from her
runner, `f4-evidence/c2-review/run.py`, with exactly her recorded numbers
before a line changed. Her probes against the corrected tree are in
[`c2-fixes/c2-review-after.txt`](f4-evidence/c2-fixes/c2-review-after.txt),
tabulated in that directory's README.

Her closing advice was to map every claim to a test of its actual consumer,
and that is the shape of what follows: each fix below is pinned by a check
that drives the real widget handler or the real Scribe callback. Each check
was then shown to **fail** with its fix reverted (under *Proof the tests can
fail*).

### C2-R1 — the editor Scribe uses deletes whole letters

The body's claim that Backspace and Delete remove clusters was true of the
textfield and false of the textview: it still erased one byte, so `café`
became `caf` plus half an `é`. Both editors now erase the span between the
caret and the boundary beside it — both bytes of `é`, all three of a
decomposed `e`+U+0301, or nothing. Line joins at either end are unchanged.

### C2-R2 — a refused layout is refused, never turned into bytes

The stepping helpers fell back to offset ± 1 whenever a run was missing,
which covered "no face" and "layout refused" alike. Now:

- **Boundaries come from the bytes.** `os64_ui_text_step` and
  `os64_ui_text_snap` (new, `os64/ui.h`) walk F2's own decoder, the one F2
  lays a line out with. A run's carets are exactly that decoder's cluster
  ends, whatever the face, so this gives the run's answer without a run: no
  layout, no allocation, no way to fail. Left, Right, Backspace and Delete
  use it in both editors. `os64_ui_run_step`, C2's run accessor for the same
  question, is gone. The host suite pins the equivalence byte by byte on
  nine strings — precomposed, decomposed, two marks on one base, a mark with
  no base, malformed and truncated UTF-8, tabs and controls, three-byte
  scalars, empty — in the bitmap face and an outline face.
- **Motion that needs pixels asks first.** Up, Down, PgUp, PgDn and a click
  work out where the caret would land *before* anything moves. If that
  line's run cannot be laid out, the key refuses. The caret, the selection
  and the scroll all stay where they were. Quinn's view-oom probe now lands on
  5, not 4.
- **No face at all** (a window whose engine could not be built) is a separate
  case with its own answer. Up and Down keep the byte column, snapped to a
  boundary of the destination line, and a click goes to the start of its
  line.

Siblings found tracing the same paths, fixed with it:

- A refused **insert**, **split** or **join** no longer moves the caret past
  bytes that are not there, and a paste stops at the first refused insert
  rather than pasting around the gap.
- `os64_ui_textview_goto` snaps its column to a boundary, and
  `os64_ui_textview_select` widens a range to whole clusters: a search match
  is a byte count and can end inside a letter.
- **After any edit the caret is settled** out of any cluster the edit formed
  around it. Typing `e` in front of a combining mark makes one letter. So
  does joining a line that ends in `e` to one that starts with the mark. The
  caret goes after that letter in both editors.

How `ui_font.c` reaches F2's decoder is the first item under *For your
ruling*.

### C2-R3 — the extent is staged in the plan

`commit_font` measured every line, which allocates, after the barrier. Worse,
the scan reset the extent to zero first and then skipped the failures, so a
refusal inside commit left an adopted face with a scrollbar total of 0.
Scribe's plan now measures the candidate extent of the document — and of the
help page when it is up — while a refusal can still turn the change down.
Commit only assigns numbers.

- **All or nothing.** `measure_extent` writes its answer only when every line
  measured. Its other callers (load, paste, opening help) keep the value they
  had when it refuses, rather than a partial maximum.
- **The document behind help** is measured by the same plan and committed
  with it. `help_leave` no longer measures anything; it only finds the scroll
  again around the caret.
- **The first paint after a change allocates nothing**, anywhere in the
  window. Quinn's commit probe went from 5 allocations after the barrier to
  0.

### C2-R4 — the field's scroll is staged with its run

The textfield has its own `prepare`/`commit`, and `left_staged` beside
`left_px`. Prepare works out the scroll from the candidate's run and the
rectangle the planner staged. Commit moves it into place with the run, and
an abort leaves the old one standing.

**A policy change, in both editors:** the scroll a font change stages is
worked out afresh — the scroll this caret would get in a freshly opened field
or document — rather than starting from the old number. The old scroll is in
the retired face's pixels and is not a position in the new one. Keeping it
and moving only as far as the caret needed left a shrunken field with the
caret at its left edge and every letter scrolled off.

**Found on the glass, and older than F4.** Scribe's Open/Find/Save As field
never took focus. `enter_mode` focused it *before* the layout that un-hides
it, and since 2f14781 (the appearance work) `os64_ui_set_focus` refuses a
hidden widget, so typing went into the document. The same ordering computed
a prefilled path's scroll against the field's old width, which is how the
save-before-quit prompt at 28px lost its caret past the right edge
(`c2-fixes/prefill-caret-lost-before-reorder.png`). `enter_mode` now lays the
bar out first. After it, the field takes focus and keeps its caret through
16 → 28 → 12 px (`c2-fixes/field-*.png`).

### C2-R5 — a file saves as it loaded

`sbuf_t` gained `terminated`: whether a newline followed the last line. The
buffer is the file's bytes split at LF plus that bit, and nothing else is
interpreted. An untouched file therefore saves byte-for-byte: no final
newline, empty, CRLF, malformed UTF-8, control bytes and a NUL are all tested.
Every edit is the byte edit it looks like:

- Enter at the end of an unterminated last line adds the newline it lacked.
- Joining two lines removes exactly one newline.
- Typing into an empty file gives a file with no final newline.
- A file scribe creates from nothing ends in one, as before.

SCRIBE.md says so.

Two load siblings, both able to lose work:

- **A failed load kept nothing.** `sbuf_load` freed the open buffer before
  parsing the new file, so running out of memory halfway left a partial
  document where the user's unsaved edits had been. It now builds the new
  document beside the old one and swaps only when whole.
- **A short read loaded part of a file as if it were all of it.** That
  buffer was marked unchanged, and the next save would have written the part
  over the whole. A read that stops short of the size is now a failed load.

### Also fixed while tracing

- **The textview laid most rows out twice per paint.** A row without a
  retained slot was laid out into the caret's slot, evicting the caret line's
  run. The draw then laid it out again, and the selection pass a third time.
  A view that never adopted a face had no slots at all, which means
  production Scribe on the builtin face did this for every row on every
  paint. Rows now get slots on demand and borrow from them, and a repaint of
  unchanged text allocates nothing (pinned).
- `os64_ui_textfield_set` cuts over-long text at a cluster boundary, judged
  against the whole text (the known gap above).

### Seen, and not changed

*As submitted for re-review. The first and last items became C2-R6 and C2-R8
there and are now corrected; see* After the re-review.

- **With no face at all, the editors draw no caret and no highlight.** It is
  the second item under *For your ruling*.
- **A multi-line selection delete is not atomic.** If its final join is
  refused, the bytes are right but an extra line break remains. This predates
  F4: the buffer vtable has no transaction.
- **A field's scroll follows edits and font changes, not a live resize.**
  Also older than F4.
- **A field paste at capacity can cut a UTF-8 sequence**, because the
  clipboard read is byte-limited.

### For your ruling

*All five are ruled in
[the re-review](F4-C2-QUINN-REVIEW.md#answers-to-all-five-requested-rulings),
and each ruling is applied; see* After the re-review.

1. **The route to F2's decoder.** `ui_font.c` includes `text_internal.h` to
   reach `text_decode`, because F2's public surface has no layout-free
   boundary call. The alternative is a public F2 entry point, which would be
   additive but is your surface to extend. I took the private route because
   it leaves F2's contract as you froze it. Say which you prefer and it
   moves.
2. **What a window with no face draws.** When a window's engine could not
   be built, text draws in the bitmap cell but the editors draw no caret and
   no highlight. Both came from the run, which that state does not have.
   Motion and deletion there are legal: bytes stay whole and the caret stays
   on a boundary. But nothing shows where the caret is. This regressed in C2,
   since the column-based editor drew both. The fix is a design choice:
   answer carets and selections from the bitmap cell's geometry (eight
   pixels a byte, which is what that painter draws), or treat an engine that
   cannot be built as a window that cannot open.
3. **The scroll a font change stages.** Both editors now work it out afresh
   around the caret. C2's textview kept the old scroll if the caret still
   showed, and its field kept it regardless (R4).
   The old number is in the retired face's pixels. Keeping it is how a field
   that shrank ended up with every letter scrolled off its left edge. If you
   would rather keep more of the old view — rescaling it, say — that is a
   different rule, and a small change.
4. **The file-ending rule R5 needed.** Your review said a different product
   policy would need an explicit decision. This is not a different one: an
   untouched file saves byte-for-byte, as the packet requires. It is the
   rule for edits, stated so it can be accepted rather than assumed. Every
   edit is the byte edit it looks like, so typing into an existing empty
   file gives a file with no final newline, while a file scribe creates from
   nothing ends in one — the only place scribe chooses an ending itself.
5. **A Scribe fix that predates F4 rides on this branch.** The `enter_mode`
   reorder fixes a field that never took focus, on `userland` since
   2f14781. R4's evidence needed a field that takes focus, so it came along.
   Chris has seen it. It is one hunk if it should land separately.

### Tests

- `tools/test_ui_text_host.py --real`: 377 → **988** checks at O2 and O0.
  The fake-backend run is still 75.
- **New: `tools/test_scribe_host.{c,py}` — 2535 checks.** It drives the real
  Scribe on the host. `scribe_build` (split out of `scribe_main`, see Shared
  hunks) builds the real window on a canvas. Keys go through
  `os64_ui_dispatch` to the focused textview, font changes through libui's
  consumer and the real coordinator, and files through `sbuf_load` and
  `sbuf_save` over an in-memory disk. Three sweeps refuse each allocation in
  turn:
  - Every allocation in a font change. Each change either succeeds with the
    complete new extent, or leaves face, layout, extent, caret, selection
    and bytes exactly as they were.
  - Every allocation in a Down. The caret ends on a boundary and either moved
    or did not.
  - Every allocation in a load. The load either succeeds, or the open buffer
    and its unsaved edit survive.
- Guest: `scribefonttest --selftest` **PASS, 41 checks**. That is C2's 33
  plus eight file endings saved untouched through the real disk.
- Quinn's C1 runners: every output identical to her after-records, including
  r4's 134 supplemental checks.

### Evidence after the corrections

```sh
ASAN_OPTIONS=detect_leaks=0 python3 tools/test_ui_text_host.py --real        # 988 checks
ASAN_OPTIONS=detect_leaks=0 python3 tools/test_ui_text_host.py --real -O 0   # 988 checks
ASAN_OPTIONS=detect_leaks=0 python3 tools/test_ui_text_host.py               # 75 checks
ASAN_OPTIONS=detect_leaks=0 python3 tools/test_scribe_host.py                # 2535 checks
ASAN_OPTIONS=detect_leaks=0 python3 tools/test_scribe_host.py -O 0           # 2535 checks
python3 docs/fonts/f4-evidence/c2-review/run.py --output <dir>               # = c2-fixes/c2-review-after.txt
python3 docs/fonts/f4-evidence/c1-review/r4-run.py --output <dir>            # 134 supplemental, = r4-supplemental.txt
bash tools/test_appearance_host.sh                                           # green
ASAN_OPTIONS=detect_leaks=0 python3 tools/test_text_host.py                  # 7,161 checks (F2)
```

`test_scribe_host` `#include`s the libui suite for its allocator, face loader
and `CHECK`, the same way your probes do, so the libui suite's new statics
keep the `ui_test_` prefix or a name no probe uses. All of your runners
build and run against this tree.

On the guest (`/QEMU GUI Boot`), `scribefonttest --selftest` prints `PASS,
41 checks, 0 failed` on VT1. The field sequence behind the `c2-fixes`
screenshots:

1. Run `scribefonttest /tests/fonts/LICENSE-DejaVu.txt`.
2. Press Ctrl+O and type a long path.
3. Press Alt+2, then Alt+= three times.
4. Press Alt+- four times.

`make`, `make fsck-ext2` and `git diff --check` are clean. `tools/stale_refs.sh`
reports only this document's own mentions of the two retired names,
`measure_widest` and `os64_ui_run_step`, which are here to say what replaced
them.

**What this round touches:**

- `userland/libos64`: `ui_font.c`, `ui_text.c`, `include/os64/ui.h`
- `userland/apps/scribe`: `scribe.c`, `scribe_buf.c`, `scribe_buf.h`
- `userland/tests/scribefonttest/scribefonttest.c`
- `tools/test_ui_text_host.c`
- new: `tools/test_scribe_host.{c,py}`
- `SCRIBE.md`
- `docs/fonts/f4-evidence/c2-fixes/`
- one line of your `c2-review/repro.c`

### Proof the tests can fail

Each fix was reverted on its own and the suites rerun. Every mutant fails:

| Mutant | Caught by |
|---|---|
| backward step by one byte | boundary equivalence |
| field Backspace by one byte | field edits, including under refusal |
| field truncation by byte | the six-byte field |
| no caret settle after typing | combining-mark junction, field and view |
| field commit drops the staged scroll | field scroll: caret visible at 24px |
| field prepare keeps the retired scroll | field scroll: 0 once the text fits |
| view prepare keeps the retired scroll | view scroll: 0 once the line fits |
| Up/Down never refuse | keys without layout; Scribe's Down sweep sees no refusal |
| no row slots in paint | repaint of unchanged text allocates |
| view Backspace / Delete by byte | Scribe deletion, precomposed and decomposed |
| caret past a refused insert | Scribe: refused insert mid-line |
| extent measured in commit | allocations after the barrier; sweep extents |
| extent skips failed lines | sweep: adopted extent ≠ fresh measurement |
| help commit forgets the document's extent | help: saved extent, extent after leave |
| save always adds LF | every no-final-newline case |
| failed load keeps the partial file | load sweep |
| short read accepted | short-read load |

Two tests did not catch their mutants the first time, and were fixed before
this was written:

- The field shrink check only asked that the scroll *decrease*, which the
  old policy also does. It now requires 0 once the text fits.
- The refused insert sat at the end of the line, where the caret settle
  clamps the mutant's overshoot back to the right answer. It now sits
  mid-line.

### Shared hunks, this round

- `docs/fonts/f4-evidence/c2-review/repro.c`: one call renamed
  (`measure_widest()` → `measure_document()`) so Quinn's probe builds.
- `scribe.c`: `scribe_build` is `scribe_main`'s widget construction moved
  into its own function, so the host harness builds the real window without
  a window system. `scribe_main` calls it; behaviour is unchanged.
- `SCRIBE.md`: the save rule.

**Not run:** P5 / real hardware.

## After the re-review

Quinn's [correction re-review](F4-C2-QUINN-REVIEW.md#correction-re-review--2026-09-19)
resolved R1–R5, ruled on all five questions, and left three P2s. All three
reproduced from her `c2-review-r2/run.py` with exactly her numbers before a
line changed. Her probe against the corrected tree is in
[`c2-fixes/c2-review-r2-after.txt`](f4-evidence/c2-fixes/c2-review-r2-after.txt):

| Probe | re-review | corrected |
|---|---|---|
| field Delete joining an accent | caret 1 inside `é`; the next Delete took only the accent | caret 3; the next Delete, at the end, takes nothing |
| field Backspace joining an accent | caret 1; the next Backspace left an orphan accent | caret 3; the next Backspace takes the whole letter |
| field with no engine | 0 caret pixels | 32 |
| view with no engine | 0 caret, 0 selection pixels | 32 caret, 452 selection |
| capacity paste of `é`, one byte free | `616263c3`, a letter cut in half | nothing pasted: `616263`, caret 3 |

**Her correction to this document:** the round-one callout said the
corrections were "the commit after e289ca3" while they were still in the
working tree. They are committed now, and the callout names the commit that
carries this revision rather than guessing at a hash.

### C2-R6 — a window with no face is still an editor

Her ruling was to keep the bitmap editor usable, with its real geometry,
without letting a refused layout borrow that geometry. Both editors now ask
one place, `line_geom_t` in `ui_text.c`, where a line's letters are: the
caret's X, the offset under a pixel, the span of a selection.

- **Under a face**, the answer is the line's run, as before.
- **With no face at all**, the answer is the bitmap cell, which is what that
  painter actually draws: eight pixels a byte. The stops are still the
  decoder's letter edges, so `é` covers two cells, a click inside it goes to
  whichever edge is nearer (a tie to the later one, as a run's does), and
  the caret is never between its bytes.
- **A refused layout gets no answer at all.** `tv_run` and the field's
  `field_geom` keep the difference: OK with no run is the cell's case; any
  other status draws no caret or highlight, places nothing and scrolls
  nothing.

The caret, the highlight, clicks, the remembered Up/Down lane and horizontal
scrolling all go through it, in both editors.

### C2-R7 — the field settles its caret after a deletion too

The field settled after typing and pasting, but not after Backspace or
Delete. Removing the `1` from `e1` + U+0301 leaves the e and its accent
touching, which makes one letter with the caret inside it. The settle now
runs once after every key the field handles. Motion already ends on a letter
edge, so for motion it is a no-op.

### C2-R8 — a paste into a full field is cut where a letter ends

`os64_ui_textfield_paste` reads the clipboard's first line up to the
field's free room **plus four bytes**. No UTF-8 scalar is longer than four,
so that is enough for the decoder to see whether the last letter that fits
continues: the rest of an `é`, or a combining mark after an `e`. It then
takes the whole-letter prefix that fits. A letter that doesn't fit is left
behind whole. The text after the caret is kept, and malformed bytes paste as
they came (her original-bytes rule).

This replaces a loop through a 256-byte stack chunk with one buffer of
`room + 4` bytes, bounded by the capacity the application gave the field.
The clipboard comment above it had said libui allocates nothing here "same as
everywhere else in this library", which was already false. It now says what
is true: the clipboard is never held whole.

### Tests, this round

- `tools/test_ui_text_host.py --real`: 988 → **1018** at O2 and O0; the
  fake-backend run is still 75.
  - **No engine**, with the engine refused after its binding was made, as
    her probe does it. In the field: the caret pixel after three cells; a
    click on either side of `é`'s middle; Right and Backspace over it; forty
    W's scrolling the caret into view. In the view: a highlight exactly four
    cells wide and nothing past it; the caret at the fifth cell; clicks
    inside `é` landing on 3 or 5, never 4; an Up/Down lane; a long line
    scrolling to its end.
  - **The other state:** DejaVu installed with every allocation refused.
    Zero caret and zero highlight pixels in both editors, so the cell's
    answers cannot stand in for a font nobody chose.
  - **R7:** both keys; the key after each; malformed `C3 1 A9` becoming a
    whole `é` once the digit goes.
- `tools/test_scribe_host.py`: 2535 → **2585**.
  - The textview's half of R7, through Scribe's own editor.
  - **R8 paste cuts:** one byte free against `é` and against e + U+0301;
    exact room; precomposed and decomposed cuts mid-paste with the field's
    suffix kept; both again with the clipboard arriving **one byte per
    read**; a malformed byte kept; first line only.
- **Every new fix, reverted on its own, fails the suites:**
  - no cell answers
  - a refused field layout treated as no face
  - the view painting its caret without geometry
  - a cell click always taking the earlier edge
  - no field settle
  - no view settle
  - byte truncation in the paste
  - a paste with no lookahead
- Guest: `scribefonttest --selftest` still **PASS, 41 checks**
  (`c2-fixes/r2-selftest-pass-41.png`). Production Scribe's Ctrl+O field takes
  focus and typing (`c2-fixes/r2-production-scribe-field.png`).
- Quinn's C1 runners: every output identical to her after-records. Her
  round-one C2 probes: identical to `c2-fixes/c2-review-after.txt`.

```sh
ASAN_OPTIONS=detect_leaks=0 python3 tools/test_ui_text_host.py --real --output <dir>   # 1018
ASAN_OPTIONS=detect_leaks=0 python3 tools/test_ui_text_host.py --real -O 0             # 1018
ASAN_OPTIONS=detect_leaks=0 python3 tools/test_ui_text_host.py                         # 75
ASAN_OPTIONS=detect_leaks=0 python3 tools/test_scribe_host.py                          # 2585
ASAN_OPTIONS=detect_leaks=0 python3 tools/test_scribe_host.py -O 0                     # 2585
python3 docs/fonts/f4-evidence/c2-review-r2/run.py --baseline <dir> --output <out>     # = c2-fixes/c2-review-r2-after.txt
python3 docs/fonts/f4-evidence/c2-review/run.py --output <out>                         # = c2-fixes/c2-review-after.txt
ASAN_OPTIONS=detect_leaks=0 python3 tools/test_text_host.py                            # 7,161 (F2)
bash tools/test_appearance_host.sh                                                     # green
```

`make`, `make fsck-ext2` and `git diff --check` are clean. `stale_refs.sh`
reports names retired in these corrections that survive only in prose: the
review record, quoting the code it reviewed, and this document, saying what
replaced them. No code or comment uses one.

Her `c2-review-r2` probe calls the libui suite's `ui_test_field_key` and
`ui_test_field_delete`. They keep those names, and the new paste tests live
in the Scribe harness because both her probes define `os64_open` themselves.

**Not run:** P5 / real hardware.

## After the second re-review

Quinn's [second re-review](F4-C2-QUINN-REVIEW.md#second-correction-re-review--a690193)
of a690193 resolved R7 and R8, and R6's persistent no-engine case. It left
one follow-up, which reproduced from her `c2-review-r3/run.py` with exactly
her numbers.

**What was wrong.** A binding that could not be made is tried again at the
next lookup, and each editor's paint looked the rendering up twice. The
textview chose its geometry once, then drew through `os64_ui_draw_text`,
which looked again, found the binding available, and drew a run under a
caret placed in bitmap cells. The field did the same the other way round:
bitmap text, then a caret from a run. Recovery at the NEXT paint was always
right. The mismatch lived inside one paint.

**The correction: a painter chooses once, and draws what it chose.**

- `os64_ui_run_resolve` (new) is the one lookup: the slot's run, a fresh
  layout retained in the slot, OK with nothing when the window has no face,
  or a refusal. After an OK answer the slot is the choice. A bitmap answer
  now empties the slot rather than leaving whatever it held.
- `os64_ui_draw_run` (new) draws exactly a given choice, the run or the
  bitmap cell, and looks nothing up.
- `os64_ui_draw_text` is now the two together. It is unchanged for a
  painter that only draws, which is what the label, the checkbox caption,
  list rows and your C1 probes use it for.
- The textview resolves each row once, then draws both passes and places
  the caret from that one choice. The field resolves before drawing,
  instead of after.
- **The same pattern was in `button_paint`,** from C1. It asked
  `os64_ui_run_width` for the width, then drew through `os64_ui_draw_text`,
  so a caption with a multi-byte letter could be centred by bitmap cells and
  drawn by a run. It now draws its slot with `os64_ui_draw_run`.

As you asked, swapping the order was not the fix. The field's paint now
resolves first, but what makes it right is that the draw no longer looks
anything up.

Her probe against the corrected tree is in
[`c2-fixes/c2-review-r3-after.txt`](f4-evidence/c2-fixes/c2-review-r3-after.txt).
The paint that meets the refusal is now wholly bitmap, and no run is made
partway through it. Its caret at 42 and the highlight ending at 41 are
where the bitmap cells it drew put them. The next paint is wholly run: 34
and 33, as before. The field's caret lands at 44, the end of the text it
drew.

### Tests, this round

- `tools/test_ui_text_host.py --real`: 1018 → **1042** at O2 and O0; the
  fake-backend run is still 75.
- **One rendering per paint, for all three painters** (textview, field and
  button). `é` is what tells the two renderings apart: two bitmap cells,
  one glyph in a run. For each painter, three paints are compared pixel for
  pixel:
  - a paint with the binding refused throughout, which is all bitmap;
  - a paint by a window whose binding already exists, which is all run;
  - a paint that meets **one** refused binding, and the repaint after it.

  The first two differ. The one-refusal paint must equal the all-bitmap
  one, and its repaint the all-run one. The test needs no coordinates. For
  the record, it also checks the view's caret and highlight positions in
  the one-refusal paint (42 and 41).
- **Four mutants, each failing that test:**
  - the view's plain pass drawing through a second lookup;
  - the view's highlight pass drawing through a second lookup;
  - the field drawing before it chose;
  - the button drawing through a second lookup.
- The persistent no-engine tests and the installed-face refusal tests
  stand unchanged and pass.
- `tools/test_scribe_host.py`: still **2585**. Guest: `scribefonttest
  --selftest` still **PASS, 41 checks** (`c2-fixes/r3-selftest-pass-41.png`).
- Each earlier probe matches its record:
  - your round-one C2 probes: `c2-review-after.txt`
  - your round-two probe: `c2-review-r2-after.txt`
  - your C1 runners: their after-records

`make`, `make fsck-ext2`, `git diff --check` and `stale_refs.sh` are clean.

```sh
ASAN_OPTIONS=detect_leaks=0 python3 tools/test_ui_text_host.py --real --output <dir>   # 1042
ASAN_OPTIONS=detect_leaks=0 python3 tools/test_ui_text_host.py --real -O 0             # 1042
ASAN_OPTIONS=detect_leaks=0 python3 tools/test_scribe_host.py                          # 2585
python3 docs/fonts/f4-evidence/c2-review-r3/run.py --baseline <dir> --output <out>     # = c2-fixes/c2-review-r3-after.txt
```

**Not run:** P5 / real hardware.
