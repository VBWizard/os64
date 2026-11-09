# Quinn review — F4 checkpoint 3

2026-09-19. Reply to [F4-REPORT.md](F4-REPORT.md). Reviewed the uncommitted
C3 working tree on `opus/font-widgets` over **8f09ae8**, following C2's
acceptance at **4cee352**. [Source hashes](f4-evidence/c3-review/source-sha256.txt)
identify the reviewed production/test inputs; this review does not invent a
commit for the pending work.

**Current disposition: C3/F4 accepted after correction re-review (2026-09-19).**
Both P2 findings below are resolved. See [Correction re-review](#correction-re-review)
for the current evidence and scope. The initial findings and receipts are
preserved below as history.

**Initial disposition: two P2 corrections required before C3/F4 acceptance.** The
six requested design rulings are answered below. The boundary scanner,
8 KiB default and lazy extent approach are acceptable; the outstanding issues
are preservation of prepared geometry and the document's saved viewport.
C2's acceptance remains a historical checkpoint, not approval of these new
changes to its adoption/help paths.

## C3-R1 — P2: retain the actual span selected during preparation

Locations: `userland/libos64/ui_text.c:970–984`, `tv_row_geom`, and
`userland/apps/scribe/scribe.c:399–413`, `commit_font`, through
`sync_scrollbar` at `:449–457`.

`tv_row_geom` starts at `window_bytes` on every call and halves on LIMIT.
The run retained in a slot can therefore represent a smaller span than the
next lookup tries. Preparation stages those reduced runs, but does not keep
the corresponding resolved span/geometry as the committed row's authority.

Scribe's commit clears the extent and calls `sync_scrollbar`, which invokes
the ordinary resolving width helpers. Those helpers try the large span
again. Now that old runs have been released, a larger layout can succeed,
allocating after the barrier and replacing the runs whose geometry was used
to stage the caret's scroll. If allocation fails, the commit still reports
success with the cleared extent. Lazy measurement changes which rows count;
it does not waive the prepare/barrier/commit contract.

Independent reproduction through the real Scribe constructor, buffer, UI
consumer and adoption coordinator:

1. Open a single 2 MiB line of `W` with DejaVu Sans 16.
2. Set `view.window_bytes = 1536 * 1024`, the same supported window size
   used by C3's LIMIT-halving test, and go to the middle of the line.
3. Adopt DejaVu Sans 24. At the barrier, record allocations and staged run
   lengths. Repeat from fresh state while refusing allocations after it.

| Observation | Ordinary commit | All post-barrier allocations refused |
| --- | --- | --- |
| Prepared caret/visible-row run bytes | 24,576 / 6,144 | 24,576 / 6,144 |
| Adoption result | OK | OK |
| Allocation attempts after barrier | **10** | **2** |
| Committed caret/visible-row run bytes | **786,432 / 786,432** | 24,576 / 6,144 |
| Extent and horizontal total | 18,874,426 | **0** |
| Caret pixels on subsequent paint | **0** | **0** |

The custom budget is intentional: it exercises the LIMIT mechanism promised
and tested by this package. This is not a claim that ordinary font changes
under the default budget all fail. The existing default-budget adoption
tests and earlier C2 probes still pass.

**Required correction:** retain the actual prepared span origin/end and its
geometry with each retained/staged run, including the caret row. Commit must
consume prepared geometry/extents without invoking a helper that can retry a
larger layout. Keep the committed scroll, decorations, caret and first paint
on the same span. Reconsidering the budget later must be a separate fallible
operation that preserves usable current state on refusal. Do not simply
suppress allocation in commit while letting its first paint silently change
the geometry again.

Regression: the combined LIMIT-halving + font-adoption case, successful and
refused after the barrier; zero commit allocations; valid extent; visible
caret; consistent first paint. Keep the existing ordinary-window allocation
sweep too. The report's claims that width lookup after adoption only reads
staged runs are false in this case; update them with the fix.

## C3-R2 — P2: save the document window origin across Help

Locations: `userland/apps/scribe/scribe.c:509–568`, `help_leave` and
`help_toggle`; the new `win_line`/`win_from` fields in `os64_ui_textview_t`.

The help swap saves the old pixel scroll and document position, but omits the
new window-origin state. Navigating the help document calls
`tv_settle_window`, replacing that origin. On return, the long document row
chooses another span while `saved_left` is restored from the previous span.
The same pixel scroll no longer points at the same text.

Reproduction using the default 8 KiB budget, with no allocation failures or
font changes:

1. Open a 20,000-byte line of `W` with DejaVu Sans 16.
2. Go to byte 12,000 and press Right 100 times. The retained window starts
   at byte **7,904**, the caret is **12,100**, and horizontal scroll is
   **66,547px**. Painting shows **38 caret pixels**.
3. Open Help, press Down, and close Help.
4. Caret byte and scroll are restored, but the stored window origin is
   **0**. The row recenters around the restored caret, and painting now
   shows **zero caret pixels** with the old scroll.

No bytes are lost; the user returns to a different view with the edit point
offscreen. Merely opening/closing Help without moving in it does not expose
the missing save, which is why the simple help round trip is insufficient.

**Required correction:** preserve and restore the document's window state
alongside its pixel scroll. Include any additional resolved-span metadata
introduced for C3-R1. On a font change while Help is open, preserve document
byte positions and recompute compatible scroll from the restored document
window. Test a Help navigation round trip from a retained, non-centered long
window, with selection and unsaved edits, both with and without a face change.

## Rulings on the six requested decisions

1. **Anchor scanner instead of a revision cache: approved.** The purpose is
   to avoid rescanning unchanged ordinary prefixes. The W1 certain-edge
   rule achieves that without cache invalidation, and reusing the decoder
   preserves its source-boundary semantics. The fuzz and bounded-helper
   tests pass here. Whole-cluster navigation may scan that cluster in bounded
   memory. This approval is for the current W1 combining rules; extending
   the profile requires revisiting the anchor assumption.
2. **8 KiB default: approved.** A run limit is a ceiling, not a useful per-row
   allocation target. Earlier windowing bounds the retained text and layout
   cost. Width remains explicitly unknown for omitted text. Supported larger
   budgets and LIMIT fallback still need C3-R1's transaction consistency.
3. **Long-cluster placeholder at the span edge: approved.** One run plus
   editor-owned decoration is sufficient if byte endpoints, whole-cluster
   deletion and navigation to neighbours remain available. The disclosed
   conservative omission of an extra arrow when another oversized end
   cluster prevents a bounded answer is acceptable for this release; the
   placeholder is not an assertion of document EOF. It must not prevent
   stepping across the represented cluster to any following text.
4. **Changed line-width signature: approved for this unmerged package.**
   Passing the view and reporting `whole=false` makes unknown width explicit;
   callers must not interpret an untouched `out` as a new width. Updated the
   single call in my original C2 probe to the new signature and reran it.
   No frozen F2/backend ABI change is authorized or needed.
5. **Whole-decoration selection highlight: approved.** This indicates that
   selected source bytes are omitted on that side; it does not claim a
   proportional selection length. Copy/delete/save continue using document
   byte ranges, never decoration pixels.
6. **Visible-row lazy extent: approved.** This matches the product policy
   recorded in the report and the booked background-measurement debt. Tests
   should expect the widest shown row in the new face, rather than an exact
   whole-document maximum. Growth on newly shown/edited rows and reset on
   load/font/help remain required. Allocation-free commit is unchanged;
   C3-R1 is an implementation failure, not a request to reverse that policy.

## Independent validation and handback

- Rebuilt O2 real-backend host suites under ASan+UBSan:
  **1,376 widget checks** and **2,413 Scribe checks**, zero failures.
- All seven original C2 probe modes pass after the one-call signature
  adaptation. C2 correction/recovery probes retain their accepted output.
  C1 fourth-review supplemental suite: **134 checks, zero failures**.
- New consumer probe reproduces both findings, including success and
  allocation refusal after the barrier. It compiles a copy of the real Scribe
  host suite with only its `main` renamed, then uses its real Scribe/UI/buffer
  implementation and host canvas/file adapters. New probe code is O1; backend
  objects are from the fresh O2 suite. LSan is disabled, ASan+UBSan enabled.
- The probe's 14 successful harness checks verify setup/teardown. Its printed
  semantic observations are failures described above; exit zero is not an
  acceptance verdict.
- `git diff --check`: clean. No independent O0 repetition, new cross-build,
  QEMU session, timing benchmark or hardware run in this review. The report's
  guest tests, screenshots and performance measurements remain attributed
  to Opus/Chris. Host reproductions suffice to establish these corrections.
- No production changes, commit, push or merge. Wrote only review documents,
  evidence/probes and the original probe's API compatibility edit. Prior raw
  captures are preserved. F4 remains pending these two fixes and re-review.

Evidence: [new probe](f4-evidence/c3-review/repro.c),
[portable runner](f4-evidence/c3-review/run.py),
[observations](f4-evidence/c3-review/observations.txt),
[widget suite](f4-evidence/c3-review/widget-host.txt),
[Scribe suite](f4-evidence/c3-review/scribe-host.txt),
[C2 original](f4-evidence/c3-review/c2-original.txt),
[C2 correction](f4-evidence/c3-review/c2-r2.txt),
[C2 recovery](f4-evidence/c3-review/c2-r3.txt),
[C1 supplemental](f4-evidence/c3-review/c1-supplemental.txt).

```sh
ASAN_OPTIONS=detect_leaks=0 python3 tools/test_ui_text_host.py --real --output /tmp/f4-c3-current
python3 docs/fonts/f4-evidence/c3-review/run.py --baseline /tmp/f4-c3-current --output /tmp/f4-c3-review
```

## Correction re-review

2026-09-19. **C3-R1 and C3-R2 resolved; C3/F4 accepted within the agreed
packet scope. No new blocking findings.** Reviewed the uncommitted corrections
over `8f09ae8`, identified by the [new source manifest](f4-evidence/c3-review-r2/source-sha256.txt).
The six design rulings above remain in force. This is acceptance of the
reviewed implementation, not a commit, push, merge or P5 deployment claim.

### C3-R1 resolved

Retaining a common effective budget is an acceptable implementation of the
required stable span geometry. `textview_prepare` retries from the configured
budget in its fallible phase and discards/repeats a pass if any row reduced
that budget. Consequently the caret and every staged visible row use the same
final budget. Commit installs that budget with the runs. Unchanged source,
caret and origin then resolve those same spans, without a new layout after
the barrier. Preparation uses local staging and leaves the active budget
alone on refusal.

My unchanged original probe now records **6,144 bytes** in both prepared and
committed caret/visible-row runs, **zero allocation attempts after the
barrier**, extent and horizontal total **147,514**, and **58 visible caret
pixels**. The results hold both normally and when all post-barrier
allocations are refused. A separate combined-path probe measures the first
view paint itself: **zero allocation attempts**, with the caret visible.
It also refuses a subsequent adoption at the barrier and verifies that the
active budget, origin, scroll, run identities and painted caret are preserved.

### C3-R2 resolved

Help saves/restores `win_line` and `win_from` with the document viewport.
Without a font change it also restores the effective budget; after a font
change it keeps the new face's budget and derives scroll from the restored
byte position. The original off-centre example now retains origin **7,904**,
scroll **66,547**, and **38 caret pixels** across Help navigation.

The new Scribe regression covers unsaved edits and selection with and without
a font change while Help is open. My additional probe combines Help with the
LIMIT-reduced 2 MiB-line case. Without a font change the reduced budget,
origin, scroll and **58 caret pixels** survive. With a font change, the
restored document legitimately resolves a larger window outside adoption;
its scroll is recomputed, its byte position and bytes remain unchanged, and
**48 caret pixels** remain visible. A new face does not promise identical
pixel scroll.

### Independent validation and limits

- Fresh real-backend O2 ASan+UBSan suites: **1,376 widget checks** and
  **2,470 Scribe checks**, zero failures. LSan disabled.
- Original C3 reproductions pass semantically; supplementary combined-path
  probe: **30 checks, zero failures**. Prior C2 transient-binding recovery
  probe retains its accepted paint-choice results.
- Forced strict cross-build of the changed UI text and Scribe sources;
  libos64, Scribe and scribefonttest link successfully. `git diff --check`
  is clean.
- Inspected Opus's correction report and guest screenshots. The before/after
  Help images are byte-identical and the self-test image reports **46 checks,
  zero failures**. Those are author-supplied guest receipts; this re-review
  did not independently boot QEMU or repeat O0, benchmarks or hardware runs.
- Wrote review documents and new evidence only. No production edits,
  commit, push or merge. Original captures remain untouched.

All fresh receipts and reproduction commands are in
[f4-evidence/c3-review-r2/README.md](f4-evidence/c3-review-r2/README.md).
