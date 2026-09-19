# F4 checkpoint 2 — the editor on measured runs, and the real Scribe under test

2026-09-19, Opus. Worktree `.worktrees/font-widgets`, branch `opus/font-widgets`,
built on the C1 boundary Quinn accepted at `f76093c` (recorded as `d72dddf`).

Quinn’s review of e289ca3 is recorded in
[F4-C2-QUINN-REVIEW.md](F4-C2-QUINN-REVIEW.md), with reproduced findings,
correction requirements and runnable host evidence. C2 acceptance is pending.

**Quinn correction re-review (2026-09-19):**
[rulings and current disposition](F4-C2-QUINN-REVIEW.md#correction-re-review--2026-09-19).
The original five findings are resolved in the reviewed working tree over
e289ca3. C2-R6–R8 remain: usable engine-absent editor geometry, field caret
settlement after deletion, and cluster-safe capacity truncation during paste.
All five questions under *For your ruling* are answered in that review.

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
- **The load-time extent scan is unmeasured on a large file.** `measure_widest`
  lays out every line once, at load and at a font change. It is correct and it
  is what scribe has always done in columns; whether it is fast enough on a
  multi-megabyte log is a number I have not taken. C3's lazy extent is the
  natural place to replace it, and I would rather measure first.
- **`os64_ui_textfield_set` truncates at `cap - 1` bytes**, which can cut a UTF-8
  sequence short. F0 draws the resulting partial sequence as a marker and
  preserves the bytes, so nothing breaks, but the truncation is byte-level. No
  caller in the tree comes near its capacity.
- **Widgets retain runs now**, and every leak-checked harness that builds a UI
  has had to release its fonts. That has come up three times; C3 will be the
  fourth.
