# F4 checkpoint 1 — widgets on measured runs, and the consumer adapter

2026-09-17, Opus. Worktree `.worktrees/font-widgets`, branch `opus/font-widgets`,
foundation `788de9900282e941a7a93aa110ffa69154daa066` (F2.5), this checkpoint
`d11608b`, corrected after review.

> **Three review rounds are answered.** [Quinn's review](F4-C1-QUINN-REVIEW.md)
> raised R1 (P1) and R2–R4 (P2) with rulings on all four boundary questions,
> then R2a–R2c and R5 (P2) with a teardown-policy observation against the
> first fix round. Every finding reproduced here from her own runners before
> anything was changed. See **After the review**, **After the second review**
> and **After the third review** at the foot of this document.

**This is not F4 complete, and it is not asking to be merged.** It is the first
of three checkpoints (Chris's cadence: widgets, then editor + Scribe, then the
bounded long-line window). It is submitted for review NOW because it is where
the API SHAPE got decided, and checkpoints 2 and 3 are built directly on top of
it. A contract change costs hours today and a rewrite later. `F4-REPORT.md`
follows at completion with the packet's full evidence.

Reviewed against: the F4 packet, [FONT_PROVIDER.md](../../FONT_PROVIDER.md),
[OPUS-F4-HANDOFF.md](OPUS-F4-HANDOFF.md).

## What is delivered

Labels, buttons, checkbox captions and list rows measure and paint through F2
runs instead of multiplying by an 8x16 cell. A window owns one text context and
one immutable role set, and its font can be replaced through `os64_font_adopt`.

`ui_text.c` is untouched — textfields, textviews and Scribe are checkpoint 2.

New: `userland/libos64/ui_font.c`, `tools/test_ui_text_host.{c,py}`,
`userland/tests/uifonttest/`.

## The API, and the initializer names the handoff asks for

All in `os64/ui.h`. The generic consumer descriptor F5 depends on comes from:

| Call | What it is |
|---|---|
| `os64_ui_font_consumer(ui, out)` | **The adapter initializer.** Fills an `os64_font_consumer_t` whose `user` is the `os64_ui_t`. Hand it to `os64_font_adopt`. No barrier — F3 owns the one barrier a batch may have. |
| `os64_ui_font_context(ui)` | The window's `os64_text_context_t`, built on demand and owned by libui. F5 prepares its candidate set on **this** context so one window adopts from one context. |
| `os64_ui_font_bind(ui, set)` | The startup door: install a set outright, taking a reference. There is no old layout to preserve, so it cannot half-succeed. |
| `os64_ui_font_planner(ui, plan, commit, discard, user)` | The application's layout, planned against the candidate. |
| `os64_ui_font_metrics` / `os64_ui_font_row_height` / `os64_ui_control_min_height` / `os64_ui_text_measure` / `os64_ui_draw_text` | Measurement and painting. |
| `os64_ui_font_set` / `os64_ui_font_status` / `os64_ui_font_live_bytes` / `os64_ui_font_restamp` / `os64_ui_font_release` | The set, why there isn't one, the engine's live bytes, a manual restamp, teardown. |

The plan type behind the descriptor (`ui_font_plan_t`) is private to `ui_font.c`.
F5 never sees it, per font_adopt.h's rule.

**The planner contract.** `plan` returns `os64_font_status_t`: LIMIT when the
layout will not fit the content area, NO_MEMORY when staging could not be
allocated — different answers deserving different next moves. Anything but OK
fails the whole adoption with the old state intact. It measures through the
metrics calls, which report the **candidate** for the duration of prepare;
widget bounds still describe the installed face and only move at commit.
`commit` applies the staged layout and cannot fail; `discard` frees it.

## Four boundary questions

These are the decisions I would rather have argued now than inherited.

1. **Is the planner hook the right seam?** FONT_PROVIDER.md asked for "an
   F4-owned application planning hook or owned layout plan"; this is a
   registered triple on the UI rather than a parameter to the adapter. Its real
   workout is Scribe in checkpoint 2, so this round can validate its shape but
   not yet its sufficiency — if it is going to be wrong, it will be wrong about
   something Scribe needs and a fixture does not.

2. **The `metrics` callback on `os64_ui_class_t`.** It exists because
   `os64_ui_listbox_rows(list, theme)` is handed a THEME and needs a row pitch,
   and the theme's `font.w/h` are frozen at 8/16 by the handoff. Rather than
   break a public signature an application already calls, the listbox caches its
   own pitch and the class re-derives it. The cost: a fifth field in a public
   struct, so every class literal in the tree had to name it under
   `-Werror=missing-field-initializers` — including two in `appearance.c`.

3. **A one-row control's height now belongs to its class.** `label_metrics`,
   `button_metrics` and `checkbox_metrics` set `bounds.h`. This is a real
   behaviour change for an application that sized a label by hand: it will be
   overwritten when the widget joins a tree and again at each font change.
   I chose it over a separate "natural height" field after the separate field
   failed on the glass — `os64_ui_stack_vertical` writes `bounds.h` back, so
   "the app chose this" and "we computed it last time" were indistinguishable
   and labels stayed at their first face's height forever.

   The one instance in the tree today is `gclock`, which sets
   `gLblClockText.bounds.h = gUi.theme.font_h` by hand. Under the builtin face
   the class writes the same 16, so nothing moves; under a real face the class
   gives it the taller row it would have needed anyway. That is the change
   behaving well, not an argument that it always will.

4. **Per-window text context, or per-process?** I chose per-window: a window is
   the coherent adoption group, and the Appearance Workshop already draws a
   preview beside the live article. FONT_PROVIDER.md permits sharing a context
   across windows (only the SET must differ for a preview), and sharing would
   mean one glyph cache per program instead of one per window. If F5's preview
   story is happy with shared-context/different-set, per-process is cheaper and
   I would rather move before two more consumers exist.

## Cache and invalidation, as it stands

- **Runs are laid out per draw and released.** Widget captions are app-owned
  `const char *` that can change under libui at any moment, and F2's context
  already caches the expensive half — the rasterised glyph images — so a
  re-layout is re-placement, not re-rasterisation. A retained per-widget run
  cache would need an invalidation key over (bytes, length, set identity) and
  has no measured problem to solve yet. The DOCUMENT side is different and
  checkpoint 2 will need per-line runs keyed by document revision; that design
  goes in the F4 report.
- **The set is the invalidation unit.** Commit swaps the set, restamps every
  widget's cached geometry, then marks the whole window dirty: every run in it
  was laid out against the retired set.
- **Order is load-bearing.** Widget geometry is restamped BEFORE the
  application's commit callback. A layout arranges widgets by the heights they
  report; restamping afterwards arranges the window against the face it just
  stopped using.

## Deliberate behaviour changes

- **Tabs and control bytes.** The bitmap painter drew the PSF1 glyph at the byte
  index and advanced one cell. A run obeys F0: a tab advances to the next stop
  and draws nothing, any other control byte is one missing-glyph marker.
  Asserted in the host suite rather than hidden.
- **Widget text is UTF-8 W1**, per FONTS.md ("Terminal byte/charset
  compatibility remains distinct from Scribe's UTF-8 policy"). A raw Latin-1
  high byte in a caption is now malformed input and draws a marker. No shipped
  application has one — checked across `userland/apps` and `userland/libos64`.
- **Control minima come from the face.** `theme.button_h` (28) is the floor, not
  the answer; it was written for an 8x16 cell and clipped captions at 24px.
- **Metrics never build a font engine.** A widget re-derives geometry when it
  joins a tree, long before anything is painted. Metrics answer from the 8x16
  fallback, which is the same face the builtin set holds; the host suite asserts
  that equality rather than trusting it.

## Evidence

**Host** — `tools/test_ui_text_host.py`, which compiles the real `ui_font.c` and
provider against stubs for the heap and the window system:

```sh
ASAN_OPTIONS=detect_leaks=0 python3 tools/test_ui_text_host.py --real          # 85 checks
ASAN_OPTIONS=detect_leaks=0 python3 tools/test_ui_text_host.py --real -O 0     # 85 checks
ASAN_OPTIONS=detect_leaks=0 python3 tools/test_ui_text_host.py                 # 61 checks, fake backend
```

Covers: surface-exact identity against the bitmap painter over printable
samples and at six clip widths from 0 to 35; the contracted tab and control-byte
differences; builtin metrics and the fallback equality; proportional widths
differing per character under real DejaVu and Source Sans; row height growing
with nominal size and never equalling it; the adoption transaction — a refusing
planner leaves the set, the row and the widget geometry untouched, a committing
one installs the candidate and the staged layout; geometry restamped before the
application's commit; zero live engine bytes after release.

The restamp-ordering check was confirmed to FAIL against the original ordering
before the fix was kept.

**Neighbours, unchanged:** `tools/test_text_host.py` 7,161 checks (F2), and
`tools/test_appearance_host.sh` green including its LeakSanitizer pass — which
is what caught the engine being built at widget-attach time.

**Build:** full default-strict `make` and `make -C userland` clean, `git diff
--check` clean, `tools/stale_refs.sh` reports no retired names and no new
superlatives, `make fsck-ext2` green on root and /home.

**QEMU** (`/QEMU GUI Boot`, ext2 root, 1024x768, headless with screendumps):
`/tests/uifonttest` builds a panel, three labels, a checkbox, a listbox and two
buttons, and replaces the window's font through the production adoption path
with fixture bytes from `/tests/fonts`. Verified by eye and by Chris:

| Evidence | Shows |
|---|---|
| `f4-evidence/cp1-builtin.png` | builtin 8x16, row 16 baseline 12 — unchanged appearance |
| `f4-evidence/cp1-dejavu24.png` | DejaVu Sans 24px, row 29 baseline 23; proportional widths, controls sized to the face, list given the remaining height |
| `f4-evidence/cp1-sourcecodepro-24px.png` | Source Code Pro (OpenType/CFF) 24px |
| `f4-evidence/cp1-refused.png` | an armed refusal: face, layout and status retained, nothing half-applied |

**Not run:** P5 / real hardware. No kernel change is involved and nothing here
touches a syscall, but that is an absence, not a result.

## Shared hunks (Quinn owns final build/image integration)

- `userland/GNUmakefile` — one filename, `libos64/ui_font.c`.
- `userland/apps/appearance/appearance.c` — two `NULL`s, forced by the fifth
  field in `os64_ui_class_t` under `-Werror=missing-field-initializers`.
- `tools/test_appearance_host.sh` — the font sources plus a link-time stub
  returning the fake backend, since the widgets now measure through the
  provider and that harness has no FreeType.
- `tools/test_appearance_customizer_host.c` — releases the two bindings its
  windows acquire, so the suite's leak check stays meaningful.

## Known, and deliberately not done here

- `ui_text.c` is on the monospace grid. Textfields, textviews, Scribe, the
  document run cache and source-byte positions are checkpoint 2; the bounded
  >1 MiB line window is checkpoint 3.
- `theme.checkbox_size` is a fixed 16px square, so the indicator looks
  undersized beside 24px text. It is a theme metric, not a font metric, and
  raising it is a product decision rather than a silent redefinition.
- Window chrome stays bitmap: the kernel compositor paints titlebars from its
  own constants, which ui.h's scope note already records.
- The desktop will look MIXED until the remaining consumers migrate. Anything
  built from libui widgets came along for free — `gclock` and `uiprobe` are
  already measuring. What still paints its own text through `os64_draw_text`,
  and so is still 8x16: `grootmenu`, `glogo`, `fpu_orbit`, `appearance`'s two
  sample classes (its widgets are migrated; the gallery samples paint by hand),
  and the fixtures `gkeys`, `fpu_demo`, `texttest`, `windowmintest`. `gterm` is
  on that list too and is F3's, not mine.

## After the review

Quinn’s follow-up on this correction commit is in
[F4-C1-QUINN-REVIEW.md](F4-C1-QUINN-REVIEW.md#follow-up-on-5357245),
with per-finding dispositions and runnable evidence.

Quinn's four findings all reproduced on this machine from her committed
runner before a line was changed: the ASan use-after-free, the 32-against-96
measurements, the 24 pixels painted outside an 8-pixel row. The corrections:

**R1 — the allocator owner outlives what can call it.** `os64_ui_font_release`
returns a status. It drops the window's runs and its set, and then, if libui
built the context, tries to destroy it: **BUSY leaves the window entirely
intact** and the caller releases whatever it is still holding and calls again.
Freeing the binding while a candidate or a run survived meant those survivors
called `binding_free` on freed memory the moment they were released. A
borrowed context is never destroyed here at all — the application that made
it owns it, and supplied its allocator.

**R2 — a failure is a status, never a smaller font's number.** Measurement is
now `os64_ui_text_measure`, which returns `os64_font_status_t`; there is no
call that answers a width and swallows the reason it might be wrong. Painting
under a bound face that cannot be laid out **draws nothing** rather than
substituting 8x16 glyphs at 8x16 widths. And the runs a paint needs are
staged during PREPARE, through a new per-class trio (`prepare`/`commit`/
`discard`) that the tree walk dispatches: prepare may fail, commit may not,
and the first paint after a commit allocates nothing. The listbox stages the
rows it can show — its strings come from the application's callback, so a
single retained run was never going to be the shape. Text that changes under
a retained run is detected by comparing the run's own copied bytes against
the caller's, so an application that mutates a caption in place stays correct
without having to announce it.

**R3 — registration cannot fail for want of memory.** Quinn offered two cures
and the second is stronger, so the planner trio now lives in the `os64_ui_t`
the application already owns. Nothing is allocated, so nothing can be
silently not-registered; `os64_ui_font_planner` still returns a status, but
only to refuse a partial trio. Her `register` probe went from
`adoption=0 planner_calls=0` to a planner that is called.

**R4 — the row is the ceiling.** `os64_ui_draw_text` intersects the caller's
clip with the primary row's vertical bounds before painting. Horizontal clip
and overhang are untouched. Her probe went from 24 pixels outside a [20,30)
row to none.

**Q3 — the class reports, the layout allocates.** `natural_h` is back, with an
explicit `auto_h` policy rather than an inference from zero: a widget sizes
itself from the face until `os64_ui_widget_fixed_height` says otherwise, and
an application height then survives attach, relayout and font replacement
untouched. The class hook writes `natural_h` and never `bounds.h`.

**Q4 — contexts can be shared.** `os64_ui_font_borrow_context` lends an
application-owned context to a window; libui borrows and never destroys it,
and refuses to move a window that is already wearing something. A set
prepared on a different context is now **refused** by both `os64_ui_font_bind`
and the consumer's prepare — Quinn's `cross` probe had it binding happily and
then measuring every string in the bitmap cell, with a status of OK.

One bug of my own surfaced while testing the fix: the new row-clip early
return leaked the run it had just laid out, which the suite's own teardown
accounting caught.

### Evidence after the corrections

```sh
ASAN_OPTIONS=detect_leaks=0 python3 tools/test_ui_text_host.py --real        # 197 checks
ASAN_OPTIONS=detect_leaks=0 python3 tools/test_ui_text_host.py --real -O 0   # 197 checks
ASAN_OPTIONS=detect_leaks=0 python3 tools/test_ui_text_host.py               # 75 checks
python3 docs/fonts/f4-evidence/c1-review/run.py --output <dir>               # all six probes
bash tools/test_appearance_host.sh                                           # green, leak check included
ASAN_OPTIONS=detect_leaks=0 python3 tools/test_text_host.py                  # 7,161 checks (F2)
```

The suite grew from 85 checks to 197: a regression for each finding, in the
shape Quinn specified — retained sets AND runs across an attempted teardown;
allocation denial walked through measurement, painting and caption staging;
a partial callback trio refused; a small primary with a taller marker and a
surface-sized clip; automatic and explicit heights across attach, relayout
and replacement, with a padded control so the builtin face reproducing a
16px label cannot satisfy it; two windows sharing one engine with different
active sets; a foreign candidate refused.

Her runner is re-runnable against the corrected tree — its probe source moved
to the new API so the questions survived the change rather than the call
spellings — and `f4-evidence/c1-review/after-README.md` tabulates each probe
before and after. `make`, `make fsck-ext2`, `git diff --check` and
`tools/stale_refs.sh` are clean; the QEMU fixture was re-driven and is
unchanged on the glass (`cp1-dejavu24.png`, `cp1-refused.png` are from the
corrected build).

Still not run: P5 / real hardware.

## After the second review

Quinn’s re-review of this correction is recorded in
[the third-review section](F4-C1-QUINN-REVIEW.md#third-review-of-0cfb485),
including current dispositions and recovery of the original evidence.

Quinn's follow-up closed R1, R3 and R4, and reopened R2 in three specific
paths it had not reached. All five reproduced here from `r2-run.py` before
anything changed.

**R2a — the tab interval was still being substituted.** `faces_resolve`
returned void, so a layout it could not make fell back to the builtin cell's
eight columns and adoption reported OK. DejaVu at 16px has a five-pixel
space, so the correct interval is 40 and the substitute was 64 — wrong, and
persistent in the installed faces afterwards. Twelve of her thirteen denial
positions produced a successful adoption with the wrong stop. Resolution now
returns a status that binding and preparation both carry, and a bind resolves
into a local so a refusal leaves the window wearing what it had. The one
number that stayed is a **declared policy rather than an error path**: a face
whose space is genuinely zero wide cannot express a tab in spaces, and F2
requires a positive interval, so it gets the builtin cell's eight columns and
says so. That it is the same number the error path used to reach for is
exactly why the two had to stop sharing an exit.

**R2b — the button measured twice.** `button_paint` called
`os64_ui_text_measure` for its centring and ignored the status, which cost a
second layout on every paint and, when that allocation was refused, paired a
zero width with a retained run that painted perfectly well: the caption
silently went from centred to left-aligned, 876 pixels different. It now asks
`os64_ui_run_width` for the width of **the run the draw will use**, through
the same slot; both come from one layout and cannot disagree. My comment
claiming a caption that cannot be measured is one that will not be painted
was false the moment runs became retained, and is gone.

**R2c — list rows were staged against live bounds.** Widget staging ran
before the application's planner, so a list the new layout would make taller
prepared runs for the rows it could show *today*. Her probe: three visible
rows, one retained run, ten allocations in the first paint. **The transaction
is reordered** — natural metrics, then the planner, then the widgets' text —
and the planner publishes its rectangles through `os64_ui_widget_stage_bounds`
where preparation can read them with `os64_ui_widget_planned_bounds`. Live
bounds still do not move until commit, which now applies the staged
rectangles before swapping the staged runs. `os64_ui_stack_vertical_staged`
is the layout helper's staging twin, so an application writes its layout once.

**R5 — the listbox's own arrays were unreachable at teardown.** Generic
teardown could only see `run`/`run_staged`; a list keeps one run per visible
row in storage only its class knows about, so the context stayed BUSY
forever — 810,587 bytes, with no caller holding anything. Classes now have a
`destroy` hook that teardown walks, and the listbox frees both arrays.

**The teardown-policy observation was right, and the header was the thing
that was wrong.** BUSY never meant "unchanged window": by the time the
context is asked to die, its set and runs are already gone. The code is the
honest half, so the header now says release is destructive, that BUSY means
INCOMPLETE rather than refused, that the allocator owner is kept so external
survivors have somewhere to free themselves to, and that there is no way here
to ask "can I?" without committing to it.

### Evidence after the second round

```sh
ASAN_OPTIONS=detect_leaks=0 python3 tools/test_ui_text_host.py --real        # 269 checks
ASAN_OPTIONS=detect_leaks=0 python3 tools/test_ui_text_host.py --real -O 0   # 269 checks
ASAN_OPTIONS=detect_leaks=0 python3 tools/test_ui_text_host.py               # 75 checks
python3 docs/fonts/f4-evidence/c1-review/r2-run.py --output <dir>
bash tools/test_appearance_host.sh
```

The suite went 197 → 269. Quinn was right that it could not have caught R2c
or R5: it did not link `ui_list.c`. It does now, and the new checks drive the
**real class painters** rather than the draw helper — a button and a list,
each painted once normally and once with every allocation refused, required
to be pixel-identical and to allocate nothing. Each was confirmed to fail
against the unfixed code: reverting R2b fails 2 checks, reverting R2c and R5
together fails 7.

`f4-evidence/c1-review/r2-after-*.txt` holds her probes against the
corrected tree and `r2-after-README.md` tabulates them. Her `r2-repro.c`'s
resize planner moved to the staging call, which is the mechanism R2c asked
for — it had been writing live bounds at commit because there was nothing
else to write to.

One piece of damage worth recording: I overwrote her `r2-observations.txt`
and `baseline-host.txt` by copying a rerun's output over them. The tracked
file came back from git; the untracked one was rebuilt from the results
quoted verbatim in her review, and says at the top that it was.

`make`, `make fsck-ext2`, `git diff --check` and `tools/stale_refs.sh` are
clean — the last of those flagged an ordering comment of mine as a claim that
would go stale, and it was rewritten as a rule rather than a list of steps.
The QEMU fixture was re-driven and is unchanged on the glass; it now stages
its layout through the new call instead of applying it at commit.

Still not run: P5 / real hardware.

## After the third review

Quinn's third round closed R2a, the original R2c case and R5, accepted the
teardown policy as a lifecycle choice now that the header states it, and left
two P2s. Both reproduced from `r3-run.py` before anything changed.

**R2b follow-up — a width that does not exist is not a position.** Sharing one
run fixed the unchanged caption, but a caption that CHANGES needs a new
layout, and `button_paint` was still ignoring the status. On refusal the width
stayed zero and the old run stayed in the slot; then the draw's own retry
succeeded and painted the new caption at the place a zero width implied —
X=100 instead of X=52, 876 pixels wrong, after 14 allocation attempts. The
painter now believes the answer: no width, no caption this time round, and the
next paint puts it where it belongs. One attempt instead of fourteen.

**R6 — children belong inside the parent this layout is about.** The staged
stack read a child's planned height but took X, Y and width from
`parent->bounds` in both modes, so a planner that moved a panel and then
stacked it committed the panel's new rectangle with children measured from its
old one — `(6,6,188,29)` where `(46,26,148,29)` was right, the child landing
outside its own parent. It also meant a staged stack could not nest. Staging
now takes the parent's planned rectangle, immediate layout takes its live one,
and all three coordinates come from whichever was chosen.

### Evidence after the third round

The suite went 269 → 293. Both new checks were confirmed to fail against the
unfixed code: reverting them fails 1 and 4 checks respectively. The caption
check asserts the policy in both directions — a denied paint either matches
the correct one or shows no caption, never something in between, and the paint
after it is pixel-identical to a clean one. The staged-parent check covers the
abort path too: a refused adoption leaves every live rectangle untouched.

Quinn's two new probes against the corrected tree are in
`f4-evidence/c1-review/r3-after-*.txt` with `r3-after-README.md` tabulating
them; the round-2 probes were rebuilt and rerun alongside.

**Two things of mine that she had to repair, and what I did about them.** She
restored `r2-observations.txt` byte-for-byte from her own surviving capture,
replacing the copy I had rebuilt from her quoted results, and re-added two
receipts that were missing from my commit — `r3-provenance.md` records the
sources. She also had to repair a name collision: her probes `#include` this
harness whole to reuse its fixtures, so a `list_label` I added landed in their
namespace and her runner would not compile. I had then done it AGAIN in this
round with `ink_left`. The harness's collidable statics now carry a `ui_test_`
prefix and its header says why, so the next person does not have to find this
out from a build error in somebody else's tree.

`make`, `make fsck-ext2`, `git diff --check` and `tools/stale_refs.sh` are
clean; the QEMU fixture was re-driven and is unchanged on the glass.

Still not run: P5 / real hardware.
