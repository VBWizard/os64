# Quinn review — F4 checkpoint 2

**Latest: [second correction re-review](#second-correction-re-review--a690193).**
R1–R5, R7 and R8 are resolved. R6's original absent-engine case is corrected;
one P2 follow-up remains for transient recovery during paint. C2 acceptance
is pending that correction. Earlier reviews/evidence remain historical receipts.

2026-09-19. Reviewed **e289ca3** on `opus/font-widgets`, against the accepted
C1 implementation **f76093c** and its acceptance receipts d72dddf/eb66c85.
Replying to [F4-C2-REPORT.md](F4-C2-REPORT.md).

**Disposition: corrections required before C2 acceptance and building C3 on
these editor/adoption paths.** Five P2 findings are reproduced below. R5 is an
existing buffer-policy mismatch with F4's explicit acceptance requirement, not
an implementation regression introduced by this commit. C1's prior boundary
acceptance remains recorded; this review covers the new consumers of it.

The proportional-coordinate design, reuse of F2 runs, and fixture linking the
real Scribe are appropriate. Oversized-line windows and lazy extent calculation
are explicitly C3 and are not findings here. Every reproduction here uses short
lines, and the commit-phase problem is independent of scan performance.

## C2-R1 — P2: migrate textview Backspace/Delete to cluster spans

Location: `userland/libos64/ui_text.c:1105–1132`, `textview_event`.

The textview's Left/Right path uses `tv_step_caret`, but the actual Backspace
and Delete branches still erase exactly one byte. The textfield branch was
migrated; the document editor's sibling was not. Consequently the report's
claim that motion **and deletion** use the published cluster boundaries is
not true for Scribe's textview.

Reproduction through the real textview event handler and Scribe buffer vtable,
with `café` (hex `63 61 66 c3 a9`):

- Backspace at byte 5 leaves `63 61 66 c3`, caret 4.
- Delete at byte 3 leaves `63 61 66 a9`, caret 3.
- Both should leave `63 61 66`, caret 3.

These are ordinary successful operations, without failure injection. An
intact UTF-8 letter becomes malformed stored bytes from one editing key.
The existing run-step tests cannot establish what the editing handlers erase.

Required correction: derive the adjacent legal boundary from the run and erase
that full byte span, retaining the existing line-join behavior at line ends.
Keep mutation conditional on obtaining a valid span (see R2). Exercise the
actual editable textview handler, not only `os64_ui_run_step`.

Regression: Backspace and Delete over both precomposed and decomposed supported
accents, with the caret before/after them, plus line joins and selection deletion.
This follows `FONT_CONTRACTS.md`'s cluster-deletion rule and the F4 packet.

## C2-R2 — P2: do not turn layout failure into byte-wise editing

Locations: `ui_text.c:296–304`, `field_step`; `ui_text.c:988–1001`,
`tv_step_caret`; inspect the adjacent vertical-navigation fallback as well.

`field_run` and `tv_caret_run` return NULL when layout fails. Both stepping
helpers then fall back to offset +/- 1. An allocation failure does not change
the text's UTF-8 encoding or make each byte an editing cluster. The field
comment says this fallback happens only with no face, but the same path is
reachable with an installed outline face and NO_MEMORY.

Reproductions with DejaVu Sans installed:

- Set a textfield to `café` while denying its caret-layout allocation, then
  deny the next layout and press Backspace. It leaves `63 61 66 c3`, cursor 4.
  Safe outcomes are an unchanged field on refusal or a whole-cluster deletion.
- On an uncached textview line `café`, start at byte 3 and deny one layout
  allocation during Right. The cursor becomes **4**, inside `é`; the adjacent
  legal stops are **3 and 5**. A subsequent edit can now split the letter.

Required correction: preserve the distinction between absent rendering and
failed layout, and keep editing/navigation on legal W1 boundaries. Use a
profile-correct boundary map or refuse the operation without changing caret,
selection or bytes when its needed run cannot be prepared. Do not substitute
byte motion on NO_MEMORY/LIMIT. Check both field and view helpers and the
callers that change a line before seeking a pixel goal.

Regression: failure injection through cold and invalidated runs, actual
Left/Right/Backspace/Delete handlers, and vertical motion between differently
encoded lines. Verify stored bytes and caret legality, not merely font status.
All probes here are far below the run-size bound; this is not deferred C3 work.

## C2-R3 — P2: stage Scribe's extent before the infallible commit

Locations: `userland/apps/scribe/scribe.c:384–395`, `commit_font`;
`scribe.c:436–458`, `measure_widest` and `measure_help`.

`commit_font` calls `remeasure_extent`, which lays out every line and allocates.
This violates `FONT_PROVIDER.md`'s prepare/barrier/commit contract: commit must
not allocate or fail. Moreover, both extent scans first clear `g.max_width`
and skip failed measurements, so they do not retain the previous extent as
the comments claim. The new font can be reported as adopted while its bar is
computed from an incomplete or empty scan.

The probe invokes Scribe's actual `plan_font`/`commit_font`/`discard_font`
callbacks through the real UI consumer/coordinator. Its one-line document has
64 `W`s, wider than the view. A barrier marks where preparation has finished:

- Ordinary switch from 16px to 24px returns OK but performs **five allocations
  after the barrier**. Extent changes from **1024px to 1536px**.
- Deny the first post-barrier allocation: adoption still returns **OK**, but
  the extent and horizontal scrollbar total become **0**, while fresh
  measurement under the newly installed face is **1536px**. The scrollbar
  cannot reach the off-screen tail.

Required correction: measure/stage the candidate extent with the application
plan while errors can still refuse adoption; swap that scalar/state during
commit without layout or allocation. Treat the help-view scan the same way.
For other extent-remeasurement callers, compute a complete tentative result
and replace the live value only when the scan succeeds; resetting it then
skipping failures does not preserve state. C3 can later replace the scan with
its agreed lazy/unknown-extent mechanism, but moving failure into commit is
not an interim substitute.

Regression: count/deny allocations after all prepares complete while driving
the real Scribe callback. Inject failure during candidate extent preparation
and require old identity, extent, layout, selection and bytes to survive.
Cover help and ordinary document views, and success on a shrinking face.

## C2-R4 — P2: stage a textfield's scroll when its face or bounds change

Location: `userland/libos64/ui_text.c:428–431`, the textfield class's adoption
hooks; compare `field_keep_caret_visible` with textview's dedicated preparation.

The field uses the generic caption prepare/commit callbacks. Those replace
its run but do not recalculate `left_px` against the candidate face and staged
bounds. Neither its metrics callback nor paint restores caret visibility.
A successful font replacement therefore preserves a scroll measured in the
retired face and can hide the active insertion point until another input event.

Reproduction: a 100px-wide field (92px inner width), text `WWWWWWWW`, caret at
the end. Under DejaVu Sans 8px, caret X=64 and left=0 fit. Adopt the 24px face:
adoption returns OK, caret X becomes **192**, but left stays **0**. The caret
is outside the field. This is the field used by Scribe's open/save/find row.

Required correction: give field preparation owned candidate scroll state,
computed from its prepared run and planned rectangle. Commit it with the run;
abort must leave the old scroll/caret/bytes intact. Keep the existing rule that
first coherent paint needs no layout allocation.

Regression: focused long field, larger/smaller fonts and narrower staged
bounds, with unchanged source caret/text. Assert visible caret geometry after
commit, no allocation on first paint, and unchanged active scroll on refusal.

## C2-R5 — P2: preserve the original final-newline state on unchanged saves

Location: `userland/apps/scribe/scribe_buf.c:258–267,358–361`;
`userland/tests/scribefonttest/scribefonttest.c`, unchanged-save fixture.

**This predates C2.** The buffer's existing save policy emits LF after every
stored line, and its model does not retain whether the input ended in LF.
The F4 packet nevertheless explicitly requires byte-identical saves for
unchanged files, and C2 presents its single newline-terminated fixture as
that evidence. The existing behavior does not satisfy the broader requirement:

| Untouched input bytes | Saved bytes | Save status |
| --- | --- | --- |
| `61 62 63` (`abc`) | `61 62 63 0a` | success |
| empty | `0a` | success |
| `61 62 63 0a` | identical | success |

The probe uses actual `sbuf_load` and `sbuf_save` with in-memory syscall
adapters; it does not reimplement parsing or serialization. This is a storage
acceptance gap, not a new rasterizer bug and not a criticism of report wording.

Required correction: preserve the source's terminal-newline state, including
an empty file, so no-edit saves meet the accepted F4 byte-preservation promise.
Define the corresponding edit/split/join behavior in the buffer model and keep
its documentation accurate. This can remain a focused model correction rather
than a redesign of its vtable. If a different product policy is desired, it
needs an explicit coordinated contract decision; the existing single passing
fixture is not an exception to the font packet's requirement.

Regression: empty input, final-LF present/absent, CRLF, malformed UTF-8 and
embedded control bytes, comparing complete no-edit load/save bytes. Keep this
coverage when C3 adds omitted source windows.

## What the evidence establishes

- Read the C2 production/test changes, report, editor packet, and relevant
  F0/F2.5 editing/adoption rules. The production Scribe entry point passes no
  hooks; the fixture links its implementation rather than a duplicate editor.
- Independently built the real-backend O2 host suite and ran it with
  ASan+UBSan: **377 checks, zero failures**. The initial run with LeakSanitizer
  enabled hit this environment's ptrace restriction; the same built binary
  passes with `ASAN_OPTIONS=detect_leaks=0`, matching the reported command.
- Independently compiled and ran **seven focused probe modes** against the
  pinned real backend, libui, Scribe's real adoption callbacks, and its actual
  buffer implementation. The probe harness uses O1 with ASan+UBSan and the
  freshly built O2 backend objects. No sanitizer memory fault is needed for
  the observed semantic failures.
- File syscalls, memory-availability reporting and formatting are host adapters
  in the probe. The GUI main/event loop is not launched; editing probes call
  the real widget event handlers. Do not present these as new QEMU evidence.
- `git diff --check`: clean. No independent full cross-build, QEMU repetition,
  or hardware validation was performed. Opus's guest evidence remains his.
- No production fixes, commit, push, merge, or C2 acceptance were performed.

Durable artifacts:

- [Probe source](f4-evidence/c2-review/repro.c)
- [Portable runner](f4-evidence/c2-review/run.py)
- [Observed results](f4-evidence/c2-review/observations.txt)
- [Host-suite receipt](f4-evidence/c2-review/baseline-host.txt)

```sh
python3 docs/fonts/f4-evidence/c2-review/run.py --output /tmp/f4-c2-review
```

The default command rebuilds the real-backend host suite first. `--baseline`
reuses compatible freshly built objects from the same checkout. Probes print
observations; exit zero means they completed, not that the behavior passed.

## Suggested focus for the correction pass

The concrete coverage gap is between correct helpers and the actual consumers:
cluster-step tests did not test textview deletion; the UI's prepared paint did
not prove Scribe's application commit was allocation-free; one no-edit file did
not cover the buffer's EOF policy. The field's candidate scroll also needs its
own state preparation, not just a retained caption.

A task-local instruction would be sufficient: **For each delivered behavior,
trace the real event/callback path, check sibling consumers, and test failure
at that path's state-change boundary. Map each claim to a test of its actual
consumer, including byte-level results and commit-time allocation counts.**
This review adds no standing AGENTS.md/CLAUDE.md rule and makes no inference
about why a particular model missed those paths.

## Correction re-review — 2026-09-19

Reviewed the **uncommitted working-tree corrections over e289ca3**, not a new
commit. [Source hashes](f4-evidence/c2-review-r2/source-sha256.txt) identify the
production/test snapshot. The report's phrase “the commit after e289ca3” does
not identify the current Git state: HEAD is still e289ca3.

**Disposition: original C2-R1 through C2-R5 resolved; C2 remains pending the
three P2 corrections below. Do not build C3 on these paths yet.** Two of the
remaining defects were disclosed by Opus in his report; they are acknowledged
issues needing implementation, not unreported discoveries. Long-line windows
and lazy extents remain C3, as agreed.

### Original findings

| Finding | Disposition and independent evidence |
| --- | --- |
| C2-R1, textview deletion | Resolved. Backspace and Delete over `café` both leave `caf`, caret 3. The real Scribe suite also exercises decomposed accents and joins. |
| C2-R2, byte stepping on layout failure | Resolved for the reported paths. Boundary walking uses the W1 decoder without allocating. Refused-layout Backspace removes the whole letter; Right lands at 5, not inside `é` at 4. Vertical motion obtains its destination before changing document position/selection. The separate post-deletion field defect is C2-R7 below. |
| C2-R3, allocating during commit | Resolved. Scribe stages document/help extents before commit. Both original commit probes report zero allocation attempts after the barrier and the correct 1536px extent/scrollbar total. |
| C2-R4, field scroll on font replacement | Resolved. The 8px-to-24px switch stages left=102 for caret=192 in a 92px inner box; the caret stays visible. Scroll refusal/shrink cases are covered by the expanded suite. |
| C2-R5, unchanged save bytes | Resolved. Empty input, `abc`, and `abc\n` round-trip exactly in the original probes. Scribe's suite covers additional endings, malformed bytes, controls, and NUL. |

### C2-R6 — P2: preserve a usable editor when engine creation fails

Locations: `userland/libos64/ui_text.c:340`, `field_paint`; `:890`,
`textview_paint`; sibling caret visibility and hit-testing helpers.

**Confirmed as disclosed in the report.** Refuse context allocation after the
binding allocation succeeds. The binding retains an absent engine with status
NO_MEMORY, and the text renderer intentionally uses the legacy bitmap path.
With allocation available again, paint still produces:

- Focused field containing `abc`: 64 foreground pixels, **zero caret pixels**.
- Focused, selected textview: 574 foreground pixels, **zero caret pixels and
  zero selection-background pixels**.

The painter guards both overlays with `run != NULL`, although OK-with-no-run
is a supported rendering state. Horizontal caret tracking likewise does not
update; field clicks do not place the caret, and view clicks go to column 0.
A user sees editable text but cannot see or reliably position the edit point.
This is a C2 regression, not an oversized-line limitation.

**Ruling/required correction:** retain the usable bitmap fallback. In the
explicit OK-with-no-run case, derive caret/selection/hit-test/scroll geometry
from the bitmap painter's actual eight-pixels-per-byte mapping, with legal W1
source stops supplied by the allocation-free decoder. A multi-byte cluster
may occupy several bitmap cells; its edit endpoints must still be whole.
Do not substitute bitmap measurements when an installed face's layout returns
NO_MEMORY or LIMIT. Preserve that failure distinction in the field helper too.

Test both widgets with engine creation refused, including visible caret and
selection pixels, click placement, long-field horizontal tracking, and legal
motion/deletion. Also retain installed-outline layout-refusal tests so the
fallback correction cannot silently change the chosen font.

### C2-R7 — P2: settle the field caret after deletion joins clusters

Location: `userland/libos64/ui_text.c:406–429`, `field_event` deletion cases.

Insertion and paste snap the field caret after an edit, and the textview calls
`tv_settle_caret` after mutations. The field's Backspace/Delete siblings do
not. Deletion can create a cluster just as insertion or a line join can.

Start with valid UTF-8 `e1` followed by U+0301, hex `65 31 cc 81`. In W1 the
digit separates the Latin letter from the combining-mark cluster. Both of
these start at legal boundaries:

- Delete at byte 1 removes `1`, leaving `65 cc 81`, **caret 1**.
- Backspace at byte 2 removes `1`, leaving `65 cc 81`, **caret 1**.

The result is one `e`+accent cluster with legal endpoints **0 and 3**. The
next Delete removes only the accent; the next Backspace removes only the base
and leaves an orphan accent. These are successful edits with no allocation
failure and no malformed input.

Required correction: apply the same post-edit caret settlement to field
deletion that is already used by the textview and field insertion. Test both
keys, the immediately following edit, and malformed-byte sequences that become
valid when an intervening cluster is removed. Checking the boundary helper
alone does not exercise this mutation/settlement ordering.

### C2-R8 — P2: truncate field paste at source-cluster boundaries

Location: `userland/libos64/ui_text.c:541–585`, `os64_ui_textfield_paste`.

**Confirmed as disclosed in the report.** A field with capacity 5 contains
`abc` and has one content byte free. Paste valid UTF-8 `é` (`c3 a9`): the read
is limited to the room remaining, and the field accepts one byte, producing
`61 62 63 c3`, caret 4. A valid clipboard letter becomes malformed stored
text. Settling against the already-truncated destination cannot reconstruct
the missing source byte. Scribe uses this field for paths and search text.

This is the clipboard sibling of the corrected `textfield_set` truncation.
It predates these corrections, but leaves the C2 UTF-8 editing path incomplete;
the >1MiB C3 exception does not cover a two-byte paste.

Required correction: determine the last whole source cluster that fits before
publishing a truncated paste. Use bounded buffering/lookahead and account for
read-chunk boundaries, not just the destination prefix. Refuse a cluster that
does not fit; preserve the existing field suffix and legal cursor position.
Do not reject or rewrite originally malformed source bytes merely to make the
result valid UTF-8: W1's original-byte policy still applies. Pin capacity cuts
inside both encoded accents and decomposed clusters, plus a cluster crossing
the clipboard read chunk.

### Answers to all five requested rulings

1. **Decoder access: approve the current private reuse.** `ui_font.c` and
   `text_decode.c` build into the same libos64, so this does not cross a
   separately versioned library boundary. Reusing the actual W1 decoder is
   preferable to duplicating its rules. Keep `os64_ui_text_step/snap` as the
   UI convenience surface, retain the equivalence tests, and do not add an
   F2 public ABI merely for this consumer. C3 must still address repeated
   prefix scanning with revision-aware boundary caching, as its packet says.
2. **Absent engine: keep the bitmap editor usable.** Use its actual geometry
   and W1-legal editing endpoints as specified in C2-R6. Do not make font-engine
   allocation a new requirement for opening a usable editor.
3. **Font-change scroll: approve recomputing around the caret.** A deterministic
   caret-visible view in the candidate face is acceptable. Preserve source
   offsets/selection and keep all fallible calculation in prepare. No rescaling
   of the old pixel scroll is required for this release.
4. **File endings: approve the stated policy.** Untouched files save exactly;
   an existing empty/unterminated file stays unterminated unless editing adds
   a newline. Newly created files may retain Scribe's final-LF convention.
   Keep the byte-level tests through C3's windowing work.
5. **Focus-order fix: keep it in this branch.** Unhiding/laying out the field
   before prefilling and focusing is a small, directly relevant correction.
   It makes the actual editor's field evidence meaningful; no separate landing
   is required. Retain its provenance in the eventual commit description.

### Validation and scope of this re-review

- Independently rebuilt the real-backend widget host suite at O2 with
  ASan+UBSan: **988 checks, zero failures**.
- Independently rebuilt the new real-Scribe host suite at O2 with ASan+UBSan:
  **2,535 checks, zero failures**. This exercises `scribe_build`, dispatch,
  adoption, and the real buffer with host window/theme/file adapters.
- Reran all seven original probe modes against fresh backend objects; their
  reported defects are corrected. Compiled and ran the new supplemental probe
  with O1 ASan+UBSan and those O2 backend objects; it reproduces R6–R8 above.
  LeakSanitizer was disabled in these runs; ASan/UBSan remained enabled.
- Reviewed the additional load-preservation, short-read refusal, constructor,
  row-cache and field-focus changes and the relevant tests. No additional
  finding from those changes in this pass.
- `git diff --check` is clean. No independent full cross-build, QEMU session,
  O0 repetition, or hardware validation in this review. Opus's guest/O0
  receipts remain his evidence, not a claimed rerun by Quinn.
- Only review documentation and review probes/receipts were written. No
  production fixes, commit, push, merge, C2 acceptance, or C3 implementation.
  Earlier raw captures were preserved.

Receipts: [original probes after fixes](f4-evidence/c2-review-r2/original-probes-after.txt),
[widget suite](f4-evidence/c2-review-r2/widget-host.txt),
[Scribe suite](f4-evidence/c2-review-r2/scribe-host.txt),
[supplemental source](f4-evidence/c2-review-r2/repro.c),
[runner](f4-evidence/c2-review-r2/run.py),
[observed failures](f4-evidence/c2-review-r2/observations.txt).

```sh
ASAN_OPTIONS=detect_leaks=0 python3 tools/test_ui_text_host.py --real --output /tmp/c2-current
python3 docs/fonts/f4-evidence/c2-review-r2/run.py --baseline /tmp/c2-current --output /tmp/c2-r2-extra
```

The supplemental executable prints observations; exit zero means it completed,
not that its observed behavior meets the contract. Fix these three paths and
rerun the affected consumer tests and review probes before returning for C2
acceptance. No new standing agent-prompt rule is needed.

## Second correction re-review — a690193

2026-09-19. Reviewed **a6901938acd0471d36caa2997fea8fd5df612ad4** on
`opus/font-widgets`, a clean working tree before this review's documentation
and probes. This commit includes both correction rounds over e289ca3; the
earlier reviews were committed separately as 757492f.

**Disposition: one P2 follow-up to R6 remains before C2 acceptance/C3.**
R1–R5 remain resolved. R7 and R8 are now resolved. The exact R6 failure from
the previous review is fixed, but transient recovery exposes another case of
text and overlays using different geometry within the same paint.

### Corrections verified

- **R6, persistent engine absence:** both widgets now paint 32 caret pixels;
  the view paints 452 selection-background pixels. The shared geometry
  helpers cover clicks, vertical goals and horizontal caret tracking. The
  suite distinguishes absent-engine fallback from installed-face layout
  refusal, and tests nearest legal endpoints in the bitmap path.
- **R7:** removing the separator in `e1` + U+0301 settles the field caret at
  byte 3. The next Delete changes nothing; the next Backspace removes the
  whole cluster. The added malformed-byte junction case is also covered.
- **R8:** pasting `é` with one byte free adds nothing and preserves `abc`,
  caret 3. Reading available room plus four bytes is sufficient to decide
  the cut under the current W1 decoder: a cluster continuing past the cut
  needs to be recognized, not buffered to its eventual end. Tests cover
  encoded/decomposed accents, suffix preservation, short reads, original
  malformed bytes, and first-line-only paste.
- All seven first-review probe modes still show the corrected behavior,
  including zero commit allocations and exact unchanged saves.

### C2-R6 follow-up — P2: hold one rendering choice through a paint

Locations at a690193: `userland/libos64/ui_text.c:435–441`, `field_paint`;
`:1037–1060`, `textview_paint`; `ui_font.c`'s lazy binding and draw lookup.

The absence of a binding is retryable. If the binding allocation fails,
`role_layout` returns OK with no run and leaves `ui->font` NULL. A second
lookup in the same paint can then succeed and create the builtin W1 run.
The new geometry object records the earlier choice, while drawing or caret
placement independently resolves the choice again.

This reproduces with one denied allocation; the allocator is available for
the rest of the paint. It does not require an oversized line or a font switch.

**Textview:** display `café composed`, select source bytes [3,5), caret 5.
Keep a row slot allocated, start without a binding, and deny its allocation.
`tv_run` chooses byte-cell geometry, then `os64_ui_draw_text` successfully
creates and draws the W1 run. That run places byte 5 at local X=32, but the
overlay still treats five bytes as 40 pixels. With origin X=2:

- Actual painted caret: **42**, where the run says **34**.
- Selection's rightmost painted pixel: **41**, where the run's span ends at
  **33**. The highlight includes the following space although its source
  byte is not selected.
- A second paint of unchanged bytes corrects both positions to 34 and 33.

**Textfield:** the order is reversed. Drawing hits the refused binding and
paints `café` as five bitmap cells, ending at X=44 with inset 4. The subsequent
`field_geom` lookup succeeds and positions the caret from the four-glyph W1
run at **X=36**, inside the text that was just painted.

These results are captured in the
[supplemental observations](f4-evidence/c2-review-r3/observations.txt).
The retained run, canvas pixels, and next-paint control are measured by the
probe; the field's bitmap endpoint follows the actual five-cell draw path.
The supplied persistent-context-failure test cannot catch this because its
failed binding is retained and never retries during paint.

**Required correction:** resolve the rendering choice once for each painted
line/field, and use that exact run or bitmap choice for text, highlight and
caret. In particular, do not feed a resolved bitmap decision back into a
helper that may create a run before the paint finishes. Recovery on a later
paint is fine. Keep installed-face layout failure distinct from fallback.
Merely swapping the current measurement/draw order trades one sibling's
failure for the other.

Regression: deny the binding allocation once with cold field/view state,
then allow subsequent allocations. Check the first paint's glyph/selection/
caret alignment, followed by a successful unchanged repaint. Preserve the
persistent-no-engine and installed-outline-refusal cases already in the suite.

### Evidence and handback

- Independently rebuilt and ran the O2 real-backend widget suite:
  **1,018 checks, zero failures**.
- Independently rebuilt and ran the O2 real-Scribe suite:
  **2,585 checks, zero failures**.
- Reran both previous review runners against the freshly built backend
  objects; their recorded cases pass. The new probe also uses those objects.
  Probe C is built at O1. All these runs retain ASan+UBSan with LSan disabled.
- `git diff --check` is clean. No independent full cross-build, O0 run, QEMU
  repetition, or hardware test this round; Opus's receipts remain attributed
  to him. This checkpoint is not F4 completion or merge approval.
- No production edits, commits, pushes, or C3 work. All five previous rulings
  stand; no new design decision or user clarification is needed. Fix the
  single shared paint-consistency issue and return for C2 acceptance.

Durable evidence: [widget suite](f4-evidence/c2-review-r3/widget-host.txt),
[Scribe suite](f4-evidence/c2-review-r3/scribe-host.txt),
[original probes](f4-evidence/c2-review-r3/r1-after.txt),
[previous correction probes](f4-evidence/c2-review-r3/r2-after.txt),
[new probe](f4-evidence/c2-review-r3/repro.c),
[runner](f4-evidence/c2-review-r3/run.py).

```sh
ASAN_OPTIONS=detect_leaks=0 python3 tools/test_ui_text_host.py --real --output /tmp/c2-current
python3 docs/fonts/f4-evidence/c2-review-r3/run.py --baseline /tmp/c2-current --output /tmp/c2-r3-extra
```

As in the earlier review runners, zero exit status means the observations
completed, not that the observed behavior satisfies the contract. Prior raw
captures have not been overwritten.
