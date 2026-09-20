# F4 — font-aware widgets and Scribe

2026-09-19, Opus. Branch `opus/font-widgets`, worktree `.worktrees/font-widgets`,
foundation `788de9900282e941a7a93aa110ffa69154daa066` (F2.5). The assignment is
`docs/fonts/OPUS-F4-HANDOFF.md` on the coordinator branch `codex/font-provider`,
and the acceptance checklist is the [F4 packet](04-widgets-editor.md).

**Quinn correction re-review (2026-09-19):**
[F4-QUINN-REVIEW.md](F4-QUINN-REVIEW.md#correction-re-review) accepts C3/F4
within the agreed packet scope. Both P2 findings are resolved; fresh host
suites, the original reproductions, combined-path probes and strict rebuild
pass. [Independent receipts](f4-evidence/c3-review-r2/README.md). The six
design rulings remain approved. The implementation is still uncommitted.

**Status: both corrections are made and accepted by Quinn.** Each
finding was reproduced from her runner with her numbers before anything
changed, and her probe passes against the corrected tree: zero allocations
after the barrier, committed spans equal to the staged ones, a caret on the
glass, and a help round trip that comes back to the same window. **After
Quinn's review**, at the foot, is the round in full.

Chris tested C3 in QEMU on 2026-09-19 — the long-line demo, and a 104 MB
file of his own, which is one line because every byte in it is a zero — and
ruled the extent question in **The whole-document extent** below. The
implementation sits in the worktree on top of `8f09ae8`; the commit is his
to authorize, so this report names no SHA for it yet. F4 is not asking to be
merged. The checkpoint reports hold the full record of the first two rounds,
including the reviews that shaped them. This report covers checkpoint 3 in
full and gathers what the handoff asks the final report to carry.

## Identities

| Checkpoint | What | Implementation | Acceptance record |
|---|---|---|---|
| C1 | widgets on measured runs, the whole-window consumer adapter | `d11608b`, corrected through `f76093c` | Quinn accepted `f76093c`, recorded in `d72dddf`; [F4-C1-REPORT.md](F4-C1-REPORT.md) |
| C2 | textfield, textview and Scribe in pixels; the real Scribe under test | `e289ca3`, corrected through `4cee352` | Quinn accepted `4cee352`, recorded in `8f09ae8`; [F4-C2-REPORT.md](F4-C2-REPORT.md) |
| C3 | lines over a megabyte through a bounded window; clusters longer than a run | working tree on `8f09ae8` | this report |

## Where checkpoint 3's code is

| File | What C3 put there |
|---|---|
| `userland/libos64/ui_font.c` | the anchor scanner behind `os64_ui_text_step`/`_snap` (`scalar_start`, `certain_edge`, `edge_at_or_before`), and the bounded `ui_text_cluster_after`/`_before` |
| `userland/libos64/ui_internal.h` | those two helpers' declarations and their cost |
| `userland/libos64/ui_text.c` | the row as a span plus decorations (`line_geom_t`, `EDGE_*`), the window (`tv_span_from`, `tv_origin_around`, `tv_window`, `tv_settle_window`), the retained budget (`tv_budget`, `tv_row_live`) and `tv_row_geom`'s LIMIT halving, the re-run staging pass (`tv_pass`), the decoration painter, and the row/shown width calls |
| `userland/libos64/include/os64/ui.h` | `window_bytes`, `win_line`/`win_from`, `win_budget`/`win_budget_staged`, `OS64_UI_TEXTVIEW_WINDOW`, and the three width calls |
| `userland/apps/scribe/scribe.c` | the lazy extent, the document's window saved and restored around help, and `scribe_open`/`scribe_save_as` for the fixture |
| `userland/tests/scribefonttest/scribefonttest.c` | the 2.5 MB round trip in `--selftest`, `Alt+w`, `--long-demo`, `--log-demo`, `--time-open` |
| `tools/test_ui_text_host.c` | the boundary fuzz, the 12-pixel row contract, `long_lines_are_windowed` |
| `tools/test_scribe_host.c` | `scribe_edits_a_long_line`, `scribe_extent_is_lazy`, `scribe_limit_window_font_change`, `scribe_help_keeps_the_window`, and the extent expectations the ruling changed |
| `DEBTS.md`, `SCRIBE.md` | the background extent measurer Chris booked |

## API initializer names

Everything is in `os64/ui.h`. The C1 report has the full table and the
planner contract.

- **`os64_ui_font_consumer(ui, out)`** — the whole-window adapter initializer
  the handoff asks for. It fills an `os64_font_consumer_t` for
  `os64_font_adopt`. The plan type behind it, `ui_font_plan_t`, is private to
  `ui_font.c`.
- **`os64_ui_font_context(ui)`** — the window's text context. F5 prepares its
  candidate on this context.
- **`os64_ui_font_bind(ui, set)`** — the startup door.
- **`os64_ui_font_planner(ui, plan, commit, discard, user)`** — the
  application's layout, planned against the candidate. Scribe's planner is
  production code.
- **C2 added:**
  - `os64_ui_run_caret`, `_hit`, `_selection` and `_step`: one run answers
    every question.
  - `os64_ui_run_resolve` and `os64_ui_draw_run`: one rendering per paint.
  - `os64_ui_text_step` and `os64_ui_text_snap`: letter edges with no layout.
- **C3 added:**
  - `OS64_UI_TEXTVIEW_WINDOW` and the textview's `window_bytes`.
  - `os64_ui_textview_row_width`.
  - `os64_ui_textview_shown_width`: the widest row on screen, from the
    runs the paint draws.
  - A changed `os64_ui_textview_line_width`: it now takes the view, and says
    through `*whole` whether the line has a width at all.

## Checkpoint 3 — long lines

### A line is shown through a window

A line longer than `OS64_UI_TEXTVIEW_WINDOW` (8 KiB) is never laid out
whole. Its row lays out a **span** of at most that many bytes, cut at W1
cluster edges.

- Wherever bytes are omitted, the editor draws a **decoration** that belongs
  to no byte:
  - an arrowhead ("more") for ordinary text;
  - a hollow box (the placeholder) for a cluster too long to lay out.
- The decorations are drawn in the window's chrome colours, never saved,
  never edited.
- Horizontal positions in such a row are measured from the row's own left
  edge. The width of what was omitted is unknown.

**Which span a row shows.** Every other long line shows its start. The
caret's line keeps a window origin (`win_line`, `win_from`), and keeps that
window while the caret is inside it and at least an eighth of a window from
an end with ordinary text omitted beyond. Otherwise the window is re-centred
on the caret:

- about half a window of whole clusters behind it;
- more behind it near the line's end, where the other half has nothing to
  show;
- less where a long cluster or the line's start is nearer.

So reaching an edge moves the window, and the caret stays where it is in the
document. A click on a decoration puts the caret against the omitted text,
and the window follows. The answer depends only on the bytes, the caret, the
kept origin and the budget, so a paint and the click after it agree.

**LIMIT, and why the budget is remembered.** A window the run refuses as too
big is halved until it fits, down to a floor of 64 bytes, rather than leaving
the line empty or uneditable. **The reduction is kept** (`win_budget`): LIMIT
depends on how full the text engine is, so the same line resolves a wider
span the moment other runs are released — and the span a row shows has to be
the span its retained run holds, because the caret, the scroll and the
decorations were all measured against it. A lookup therefore starts from the
budget in effect, never from the application's window.

**Preparation is where a wider window is tried again**, because it is where
a refusal still leaves the old face, the old spans and the old view
untouched. (An application that sets `window_bytes` asks for a different
window, and gets it afresh.) It starts from `window_bytes`, and a pass that ends on a
smaller budget than it began with is released and run again, so every staged
run holds a span of the same window. Commit installs that budget with the
runs. The default window is far below F2's limits; the halving is tested
with a window of 1.5 MiB, which F2 refuses, both on its own and across a
font change.

**The document is never windowed.** A selection may span omitted bytes.
Delete, copy and save act on byte offsets into the whole buffer. A decoration
standing for selected omitted bytes is lit with the selection. It is one
indicator for everything omitted on its side, so it lights if any of those
bytes are selected.

### Letter edges without the prefix, and without a cache

The packet asks for boundary information to be cached by document revision,
so that a keystroke does not rescan the line before the caret. **I met the
goal with no cache at all, and this is a deviation for Quinn's ruling.**

- **Why a scan normally starts at the line's start.** The decoder cannot run
  backwards: whether a combining mark starts a cluster depends on what came
  before it.
- **Why most positions need none of that.** Under W1 the only scalars that
  join the cluster before them are U+0300..U+036F. So the start of any other
  scalar is a cluster edge, whatever precedes it.
- **Finding a scalar start costs at most three bytes.** The nearest byte
  back that is not a continuation byte decides it.
- **How a step works.** `os64_ui_text_step` and `os64_ui_text_snap` back up
  to the nearest such certain edge and decode forward from there. On
  ordinary text that is a few bytes, whatever the line's length.
- **Nothing can go stale.** There is no revision to key on, no invalidation,
  and no cache to be wrong after an edit.
- **The one case that costs a cluster's length.** A step across a single
  cluster of megabytes, or an answer asked from inside one, walks that
  cluster, in bounded memory.
- **How far the long-cluster helpers look.** `ui_text_cluster_after` and
  `ui_text_cluster_before` recognize a cluster as too long after `limit`
  bytes of it, not at its end. So laying out a window never walks further
  than the window, however long any cluster is.

The evidence that it agrees with F2:

- A fuzz of 4,000 strings built from the byte patterns that make edges hard.
  These include marks at both ends of the range, bases that do and do not
  take marks, lone leads, stray continuations, overlong and surrogate
  encodings, invalid bytes and truncations.
- At every offset, both directions of step and snap equal the carets F2's
  run publishes.
- The bounded helpers equal the true cluster lengths.
- A one-megabyte mark cluster is covered too.

### A cluster longer than a window

A cluster is one base and marks, so it cannot be cut, and one longer than the
window cannot be laid out. It becomes the placeholder at the end of a span.

- **Caret stops.** The caret stops at the cluster's start and at its end, one
  step apart.
- **Editing.** Delete and Backspace remove the whole cluster.
- **Neighbours.** The clusters on either side are reachable as usual.

**Whether the line goes on past the cluster** would take a walk the cluster's
length to know in general. The line's own ends answer most of it in bounded
time: if the line's last cluster is short, it is not the long one, so the
line continues past the long one. The first cluster answers the same
question on the other side. Only then is a "more" arrow drawn beyond the
box. A long cluster at the line's very end leaves the answer unknown, and
unknown draws no arrow. That is the conservative case; the demo line
`abc e+marks xyz` shows the arrow, and a line that is nothing but the cluster
shows the box alone.

### Extents

A windowed line's width is **unknown**, and it is never measured or guessed.

- `os64_ui_textview_line_width` answers `*whole = false` for such a line.
- `os64_ui_textview_row_width` answers what the row actually shows.
- `os64_ui_textview_shown_width` answers the widest row on screen. It lays
  the rows out into the slots the paint draws from, so the paint that
  follows lays out nothing again.

**Scribe's horizontal bar is lazy** (Chris's ruling, below). It reaches the
widest row shown since the document or the face last changed, and grows as
rows come on screen and as edits widen them. No line is laid out for the bar
that has not been on screen. The caret's row counts too, so a long line's
window can always be scrolled across. The rest of a long line is reached by
moving the caret, which moves the window.

- **A load, a font change and help** start the extent over. At a font change
  it is read back from the runs adoption staged: the budget they were laid
  out with is committed alongside them, so the lookup resolves those same
  spans, finds those runs, and the commit lays out and allocates nothing.
- **Edits and pastes** grow it when the rows they widen are on screen, or
  when the view moves to them.
- **It never shrinks** until the next load or font change.

### The row contract

A 12-pixel primary row (DejaVu Sans at 9) holds a line of U+2603 markers,
each 16 pixels tall. The tests check four things:

- **Neighbours.** The rows beside it paint pixel for pixel as they do when
  that line is empty.
- **Vertical motion.** The caret moves exactly one pitch per line across it.
- **Clicks.** A click finds its row by the pitch.
- **Extent.** The marker line's extent equals its row's width.

The test catches removing the row clip. It catches it only when both clips
go: the textview's own and the run painter's. Either one alone still holds
the row, and C1's row-clip test catches the painter's alone.

## Cache and invalidation, all of F4

| What | Kept where | Valid while | Replaced when |
|---|---|---|---|
| Glyph images | F2's cache, in the window's context | F2's rules, not F4's | F2's rules |
| A widget caption's run | the widget's slot | the run's own copy of the bytes equals the caption | the bytes differ at the next lookup; a font change stages a new one in prepare and swaps it at commit |
| A textview row's run | one slot per visible row | the run's bytes equal the row's **span** | the span or its bytes change; the comparison costs at most one window of bytes, because a span is at most one window |
| The caret line's run | the widget's own `run` | as a row's | as a row's; motion needs it whether or not the row is on screen |
| The caret line's window | `win_line`, `win_from` | the caret is inside it and at least an eighth of a window from an end with ordinary text beyond | the caret leaves it or nears such an end; settled wherever the caret may have moved. Scribe saves and restores it around the help page, which settles the view's window onto the help document |
| The window budget | `win_budget` | until a LIMIT halves it, or the application changes `window_bytes` | preparation retries from `window_bytes` against the candidate face and commits what it took; nothing else reaches for a wider window |
| Letter edges | nowhere | always correct | nothing to replace; see above |
| Scribe's extent | `max_width` | grows as rows come on screen and as edits widen them | started over at a load, a font change and help; read back from the rows shown |

Nothing is keyed by an announced revision. Every retained run proves it
still matches by comparing its own copy of the bytes. The textview's model
belongs to the application, and a mutation the view is not told about must
not leave it drawing the old text.

## For Quinn: decisions to rule on

**All six are approved** in [her review](F4-QUINN-REVIEW.md); the cases stay
here as the record of what was asked and on what grounds.

1. **The anchor scanner instead of a revision-keyed boundary cache** (above).
   It is what the packet's sentence was for, with nothing to invalidate. If
   the packet's wording is a requirement rather than a means, this needs
   your ruling.
2. **The window is 8 KiB, not the 1 MiB run limit.**
   - **Memory.** Windowing only lines past the run limit would lay out lines
     up to a megabyte whole. A screen of forty such rows is forty megabytes
     of source text before a single glyph record, against the engine's
     128 MiB default.
   - **Keystrokes.** Every keystroke on such a line would re-lay-out all of
     it. At 8 KiB, a keystroke's layout is bounded, and so is each row's
     byte comparison.
   - **The cost.** A line between 8 KiB and 1 MiB is windowed too, and its
     extent is unknown rather than exact.
3. **The long-cluster placeholder is a decoration at the end of a span, not a
   box inline in the text.**
   - **Why.** A row has one run. An inline box would mean two runs and a
     gap, with caret, hit and selection geometry spanning all three.
   - **The consequence.** Text on the far side of a long cluster is shown
     once the caret crosses it. Before that, a "more" arrow beside the box
     says it is there, when that is known.
4. **`os64_ui_textview_line_width` changed signature.** It takes the view and
   answers `*whole`. That is a deliberate break: every caller has to decide
   what an unknown width means, instead of receiving a number that is not a
   width.
   - Your round-one C2 probe called the old form. You updated that one call
     in the review; it builds from the tree and its output is identical to
     your `c2-review-r4/r1-after.txt`.
5. **A decoration lights whole** when any of the bytes it stands for are
   selected, rather than proportionally. It does not know how many bytes it
   stands for.
6. **One accepted C2 check changed, by Chris's ruling below.** "Widest-line
   shrinkage" required the extent after a font change to equal a fresh
   measurement of every line. It now equals the widest row shown in the new
   face. A smaller face must still give a smaller extent, and the change
   must still allocate nothing after the barrier.

## The whole-document extent: measured, and ruled

C2 recorded Scribe's whole-document measurement as unmeasured, and said C3
was the place to decide it. Scribe laid out every line once at a load, a
paste and a font change, to know how far the horizontal bar should reach.
On a 100,000-line, 8.9 MB log, that measuring was nearly the whole cost of
opening the file:

| | host `-O2`, before | host `-O2`, after | QEMU, before | QEMU, after |
|---|---|---|---|---|
| read the file and split its lines | 31 ms | 31 ms | — | — |
| Open, builtin face | 31 + 781 ms | 26 ms | 34.0 s | 2.3 s |
| change to DejaVu Sans 16 with it open | 1,280 ms | 2.1 ms | 31.9 s | 0.35 s |
| Open under DejaVu Sans 16 | 31 + 1,274 ms | 31 ms | 37.5 s | 5.9 s |

The builtin face was no cheaper, because it is an F2 face too and each line
was a full layout under it. The last QEMU Open also frees the first copy's
100,000 lines. Before F4, Scribe counted columns: a loop over the bytes.

**Chris's ruling (2026-09-19): size the bar from the lines displayed**, as
described under **Extents** above. He also asked for a debt: a background
thread that re-measures the whole document whenever it is modified,
including when a file is opened. It is booked in `DEBTS.md`, GUI section,
and in `SCRIBE.md` § The buffer. That row names the two things to settle
first: whether a window's text context may be used from a second thread,
and how a pass measured against bytes that have since changed is discarded.

The trade the ruling accepts: a wide line nobody has scrolled to is outside
the bar until it is shown.

## Evidence

### Host

- **`tools/test_ui_text_host.py --real`:** 1,042 → **1,376** checks, at `-O2`
  and `-O0`. The fake-backend run is **75**. New in C3:
  - the boundary fuzz;
  - the 12-pixel row contract;
  - `long_lines_are_windowed`. It uses a model with five lines: short, a
    3 MB line of words, `abce` + 700,000 acutes + `xyz`, 20,000 `x`, and a
    line that is one 1.4 MB cluster. It checks:
    - exact bitmap widths, decorations included;
    - windows moving under forty steps each way;
    - clicks on a decoration;
    - End and Home;
    - a selection lighting its decoration;
    - crossing a long cluster;
    - Up and Down;
    - a font change with long lines on screen;
    - the default window at the end of a line;
    - LIMIT halving.
- **`tools/test_scribe_host.py`:** 2,585 → **2,470**, at `-O2` and `-O0`
  (2,413 before the review round's two regressions).
  The count fell because a font change now makes fewer allocations, 281.
  The test that refuses each allocation of a font change in turn therefore
  runs fewer rounds, and each round carries several checks.
  `scribe_extent_is_lazy` drives the real Scribe through 20,000 lines with
  one wide line far below the first screen:
  - Scribe's Open allocates what reading the bytes does, plus fewer than
    2,000 more. A layout per line would be twenty thousand more.
  - The bar leaves the wide line out until it is shown, reaches it when the
    view moves to it, and keeps that width after the view moves away.
  - Ctrl+V of a wider line, through Scribe's own shortcut, grows the bar.
  - A new document starts the bar over.

  The font-change tests now expect the widest row shown in the new face,
  and still no allocation after the barrier. The help test expects the
  waiting document to start over and grow again when help closes.
  `scribe_edits_a_long_line` drives the real Scribe:
  - A 2.5 MB line saves byte-identical untouched.
  - A keystroke deep inside it makes fewer than 64 allocations and lays out
    at most a window.
  - Typing, a split there and a character added at the new line's end save
    exactly.
  - A copy across omitted bytes puts the whole line on the clipboard.
  - A font change allocates nothing after the barrier.
  - An `e` with 600,000 acutes (1.2 MB, past F2's run limit) is crossed in
    one step and removed whole by Delete and by Backspace.
- **F2 `tools/test_text_host.py`:** 7,161, unchanged.
- **`tools/test_appearance_host.sh`:** green.
- **Each of these C3 mechanisms, reverted on its own, fails the suites:**
  - the certain-edge rule, in two ways;
  - no LIMIT halving;
  - long lines laid out whole;
  - the placeholder's width;
  - the selection not lighting a decoration;
  - no window margin;
  - no full backward budget at a line's end;
  - `prepare` laying out whole lines;
  - no "more" beyond a placeholder, on either side, and the arrow's two
    bounded checks;
  - the arrow drawn on the wrong side of the box;
  - both row clips removed together. The textview's clip alone is covered
    by the painter's, and the painter's alone fails C1's row-clip test.
  - for the lazy extent:
    - `shown_width` counting only the first row;
    - `shown_width` laying rows out in spare slots rather than the paint's,
      which allocates after the barrier;
    - the bar never growing;
    - a font change, a load, or help not starting it over, each on its own.
- **Quinn's probes against this tree:**
  - every C1 runner (`run.py`, `r2-run.py`, `r3-run.py`, `r4-run.py`): each
    output identical to its after-record;
  - C2 rounds two and three: identical to `c2-review-r4/r2-after.txt` and
    `r3-after.txt`;
  - C2 round one: identical to `c2-review-r4/r1-after.txt`, from the tree,
    with the one call you updated (decision 4). The probe seeds Scribe's
    extent with `measure_document()`. That function remains Scribe's Open-time reset
    of the extent, now meaning "start over from the rows shown". Under the
    lazy extent, its commit still makes no allocations and lands on 1536,
    as recorded.

### Cross-build

- `make` with the cross toolchain is clean, with no warnings in the F4
  sources.
- `make fsck-ext2` and `git diff --check` are clean.
- `tools/stale_refs.sh` reports no retired names and no new superlatives.

### QEMU

[f4-evidence/c3/README.md](f4-evidence/c3/README.md) lists every screendump
with the keys that produced it:

- the default window and a 64-byte window with "more" at both ends;
- selections across omitted bytes;
- the long cluster before, after, selected and deleted;
- production `/bin/scribe` on the same file;
- `scribefonttest --selftest` **PASS, 46 checks**. The five new checks are a
  2.5 MB line written, opened with Scribe's Open, typed into deep inside,
  split, extended, saved with Save As, and compared byte for byte through
  the real disk.
- the log timing before and after the lazy extent, and production Scribe
  with the log open.

For you to drive:

- **`scribefonttest --long-demo /home/long.txt`** writes the demo file.
  Open it with `scribefonttest /home/long.txt` or `scribe /home/long.txt`.
- **Alt+w** in the fixture switches to a 64-byte window, so both ends of a
  window fit on the paper.
- **`scribefonttest --log-demo`** and **`--time-open`** reproduce the timing.

### Hardware

**Not run.** Nothing in C3 has been on the P5.

## Test commands

```sh
ASAN_OPTIONS=detect_leaks=0 python3 tools/test_ui_text_host.py --real --output <dir>   # 1376
ASAN_OPTIONS=detect_leaks=0 python3 tools/test_ui_text_host.py --real -O 0             # 1376
ASAN_OPTIONS=detect_leaks=0 python3 tools/test_ui_text_host.py                         # 75
ASAN_OPTIONS=detect_leaks=0 python3 tools/test_scribe_host.py                          # 2470
ASAN_OPTIONS=detect_leaks=0 python3 tools/test_scribe_host.py -O 0                     # 2470
ASAN_OPTIONS=detect_leaks=0 python3 tools/test_text_host.py                            # 7,161 (F2)
bash tools/test_appearance_host.sh                                                     # green
bash docs/fonts/f4-evidence/c3/log-bench/run.sh                                       # the extent timing
make && make fsck-ext2 && git diff --check && bash tools/stale_refs.sh
```

In the guest: `scribefonttest --selftest` (46), `scribefonttest --long-demo
<path>`, `scribefonttest --log-demo <path>`, and `time scribefonttest
--time-open <path>`.

## Remaining application consumers

F4 migrates the toolkit and Scribe. It does not migrate the desktop.

- **Widgets, but no application planner.** `controlcenter`, `appearance`,
  `gclock` and `grootmenu` build libui widgets. Those widgets measure and
  paint through the window's set and restamp their own heights at a font
  change. None of these applications registers `os64_ui_font_planner`, so an
  arrangement they compute themselves is not re-planned when F5 changes
  their face. Scribe is the only application with a planner.
- **Direct 8x16 text outside libui.** `grootmenu`'s menu rows
  (`os64_draw_text` and cell arithmetic on `font_w`/`font_h`),
  and `appearance`'s two preview captions (`os64_draw_text_clipped`). The
  demos `fpu_orbit` and `glogo` draw with `os64_draw_text`. `gclock` still
  sizes its clock label by hand (`bounds.h = theme.font_h`); the label
  class now overwrites that height.
- **`gterm`** is F3 (Quinn).

## Shared hunks

For all of F4:

- `DEBTS.md` (C3): one row in the GUI section, the background extent
  measurer Chris asked to have booked.
- `SCRIBE.md` (C3): one paragraph in § The buffer, saying the scrollbar is
  sized from the rows shown and pointing at that debt.
- `userland/GNUmakefile`: `ui_font.c` in `LIBOS64_SRCS` (C1), and one
  `APP_EXTRA_OBJS` line linking Scribe's objects into `scribefonttest` (C2).
- `tools/test_appearance_host.c` / `test_appearance_customizer_host.c`:
  releasing the fonts their UIs acquire, and one field rename (C2).
- Quinn's C1 runners link `ui_text.c` (C2).

## Known, and not done

- **The whole-document extent.** The bar knows the rows shown, by ruling. The
  background measurer that would make it the whole document's is booked in
  `DEBTS.md`.
- **When a line's first or last cluster is itself too long to lay out,** a
  placeholder elsewhere on the line cannot cheaply tell whether it is that
  same cluster, so it draws no arrow. That is wrong only when the line has
  two such clusters with text between them. Knowing costs a walk the
  cluster's length.
- **The P5** — see Hardware.

## After Quinn's review

[Her review](F4-QUINN-REVIEW.md) of the C3 working tree required two P2
corrections and answered all six rulings. **Both findings reproduced from her
own runner, with her numbers, before anything changed:**

```
help round trip: caret 12100 -> 12100, kept origin 7904 -> 0, left 66547 -> 66547, caret pixels 38 -> 0
prepared caret run bytes=24576, prepared row run bytes=6144
LIMIT-window font adoption (deny commit=0): status=0, post-barrier allocations=10, extent=18874426, total=18874426, caret run bytes=786432, row run bytes=786432
caret pixels after adoption: 0
LIMIT-window font adoption (deny commit=1): status=0, post-barrier allocations=2, extent=0, total=0, caret run bytes=24576, row run bytes=6144
caret pixels after adoption: 0
```

### C3-R1 — the span a row shows is the span its run holds

**What was wrong, and it was mine.** `tv_row_geom` started from
`window_bytes` on every call and halved on LIMIT. I had written that the span
follows from the bytes, the caret, the kept origin and the budget — but LIMIT
is not a property of the bytes. It depends on how full the text engine is, so
preparation (the retired face's runs still alive) resolved a 24 KiB span
while commit (those runs released) resolved 768 KiB: a layout after the
barrier, and a caret placed against geometry the commit never saw. With
allocation refused after the barrier instead, the lookups failed, the extent
stayed cleared and the first paint drew no row at all. Both columns of her
table ended with a caret nobody could see.

**The correction: the budget is retained state, staged and committed like
everything else.**

- `win_budget` is the budget in effect. A LIMIT halves it and the reduction
  is kept, so every later lookup resolves the spans the retained runs hold.
- `os64_ui_textview_t` carries `win_budget_staged`; `textview_prepare` starts
  from `window_bytes` — the deliberate, fallible retry, where a refusal still
  means nothing changes — and **a pass that ended on a smaller budget than it
  began with is released and run again**, so the caret's row and every
  visible row hold spans of one window. Commit installs the budget with the
  runs.
- `tv_budget` also notices an application that changed `window_bytes` (the
  fixture's Alt+w), and starts that window afresh.

Her probe against the corrected tree:

```
LIMIT-window font adoption (deny commit=0): status=0, post-barrier allocations=0, extent=147514, total=147514, caret run bytes=6144, row run bytes=6144
caret pixels after adoption: 58
LIMIT-window font adoption (deny commit=1): status=0, post-barrier allocations=0, extent=147514, total=147514, caret run bytes=6144, row run bytes=6144
caret pixels after adoption: 58
```

Prepared and committed spans are now the same 6,144 bytes — and the caret
row's prepared span used to differ from the rows' (24,576 against 6,144),
which is exactly the inconsistency the re-run pass removes.

### C3-R2 — the document's window comes back from help

**What was wrong.** `help_toggle` saved the document's scroll, caret and
selection but not its window origin. Moving about the help page settles the
view's window onto the help document, so on return the long row chose a
different span while the restored pixel scroll still described the old one:
same bytes, different view, edit point off screen.

**The correction.** Scribe saves and restores `win_line`/`win_from` with the
rest. The budget is the face's rather than the document's, so it is restored
only when no font change happened while help was open; after one, the face
that arrived proved its own budget, and the scroll is found again around the
restored caret and window.

### Tests, this round

- **`tools/test_scribe_host.py`: 2,413 → 2,470**, at `-O2` and `-O0`.
  - `scribe_limit_window_font_change` — her case, both ways: a 2 MiB line in
    a 1.5 MiB window, the caret in the middle, adopting DejaVu Sans 24 with
    a barrier that records the staged spans, and again with the next
    allocation after the barrier refused. Zero allocations after the
    barrier, committed spans equal to the staged ones, a window smaller than
    the one asked for, a valid extent and bar total, caret pixels on the
    glass, and a first paint that lays nothing out.
  - `scribe_help_keeps_the_window` — a 20,000-byte line, the caret driven to
    a retained off-centre window, an unsaved edit and a selection, then help,
    a move inside it, and back; with and without a font change while help is
    open. Bytes, caret, selection and unsaved state return; so do the window
    origin and, without a face change, the exact scroll and caret pixels.
    With one, the caret is on the glass again.
- **Each correction, reverted on its own, fails those tests:** lookups
  restarting from `window_bytes`; commit dropping the staged budget;
  preparation keeping a pass that reduced it; help not saving the window
  origin; help not restoring it.
- The libui suite is unchanged at **1,376**, and her C1/C2 probe outputs
  still match their records.

Evidence: [f4-evidence/c3-fixes/](f4-evidence/c3-fixes/README.md) — her
probe before and after, the two guest screenshots of a help round trip that
now returns to the same window, and the self-test on the corrected build.

### Her rulings, recorded

All six approved: the anchor scanner (for the current W1 combining rules),
the 8 KiB default, the placeholder at the span edge, the changed
`line_width` signature, the whole-decoration highlight, and the visible-row
lazy extent. Her conditions are in the review and are what the two
corrections above implement.
