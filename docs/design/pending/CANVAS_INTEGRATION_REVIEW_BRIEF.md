# Canvas integration: final architectural review

Prepared for Fable, 2026-11-03. Review branch: `canvas-integration`.
Destination: `userland`. Existing integration PR: #458.

Review completed at `2c2a7518`: [Fable's verdict](CANVAS_INTEGRATION_FABLE.md)
is to merge. [The response](CANVAS_INTEGRATION_RESPONSE.md) records each
finding and the subsequent userland S3 merge (`afad35b5`, merged as
`43b60349`). The original review scope and S2 evidence below are historical;
compare `afad35b5...canvas-integration` for the current candidate. The
combined S3/Canvas P5 check passed on 2026-11-04 at `94c1c55d`; final Codex
review of the merge and follow-ups precedes the userland merge.

Chris has accepted the Canvas 2D functionality and his browsing regression.
This review asks whether the combined architecture is ready to land in
userland: ownership, invalidation, scheduling, shared interfaces, and the
interaction between independently reviewed slices. Please inspect the code as
well as the designs; earlier slice approvals do not establish that the final
composition is correct.

## Candidate and comparison

The branch includes the accepted live-site follow-ups `575ff2ee` (a rainbow
trail whose round strokes shrink below the geometry grid) and `975d9c52`
(animation callback work counts toward the frame interval). These two fixes
have host regression evidence and P5 acceptance, but have not had a separate
Codex review.

The reviewed merge-forward incorporates userland through `8faadcf5` (Streams S2,
PR #457), including its byte-stream review fixes. It is conflict-free. Use
this pinned base so later userland work does not silently change the scope:

```sh
git diff --stat 8faadcf5...2c2a7518
git diff 8faadcf5...2c2a7518
git log --oneline 8faadcf5..2c2a7518
```

Read the combined head. The incoming Streams implementation is already in the
base; its integration with Canvas's DOM installation, task budgets, teardown
and fetched image/body use remains relevant.

## Scope and reading order

1. [CANVAS.md](CANVAS.md): reusable native drawing, browser bindings, persistent
   backing storage, pixel damage, images, text, shadows and Path2D.
2. [RELAYOUT.md](RELAYOUT.md): mutation readers, incremental cascade, style
   ownership, layout snapshots/grafting, full-layout fallback and frame delivery.
3. [POINTER_EVENTS.md](POINTER_EVENTS.md): native input, capture, compatibility
   mouse events, boundary updates, coalescing and lifecycle transitions.
4. [Integration regression](../../canvas-evidence/integration-regression.md):
   merge resolutions, host/native checks and the two final live-site fixes.
5. [Heap maximum review](HEAP_MAXIMUM_REVIEW.md) and
   [MALLOC.md](../completed/MALLOC.md): the already-reviewed userland allocator
   optimization, including the verifier follow-up agreed with Fable.

The arc includes more than a drawing API. Its production changes reach
`libcanvas`, `libdraw2d`, `libsvg`, FreeType's adapter, shared text services,
`libdom`, Yonder, incremental HTML/cascade/layout support, PNG export and the
userland heap. Kernel source is outside this arc's production changes.

## Architectural seams worth inspecting

| Seam | Questions and principal code |
| --- | --- |
| Shared drawing and geometry | Are Canvas and SVG coordinate spaces, stroke expansion, clips, coverage, shadows and compositing consistent? Check `libcanvas/path.c`, `shadow.c`, `libdraw2d`, and `libfreetype/port/geometry.c`. The new compact round-tip fallback must retain bounds, winding, budget/refusal handling and single-pass alpha. |
| Ownership and reset | Do backing pixels, ImageData, retained paths, gradients, prepared text and exported bytes have clear lifetimes? Do repeated width/height assignments reset correctly? Inspect `libcanvas/core.c`, `libdom/canvas*.c`, `image_data.c`, mutation dispatch and teardown. |
| Image acquisition and origin | Do natural dimensions, full-resolution Canvas sources, reduced display decodes, request-mode catalog keys and origin-clean propagation agree? Inspect `libdom/canvas_image.c`, Yonder `picture.c`, `picture_submit`, `canvas_source` and resource revisions. |
| Incremental layout | Can copied styles or grafted boxes retain old-arena pointers? Are mutation/resource changes sufficient to invalidate reuse, including filters and backdrop filters added on userland? Inspect `libflow/style_copy.inc`, `reuse_*.inc`, cascade readers and Yonder's rebuild/fallback paths. |
| Damage and scheduling | Do pixel-only updates avoid layout while filtered/blended ancestors still repaint correctly? Do callback batches, checkpoints, paint gates, covered windows and idle work share one coherent loop? Inspect `canvas_damage_mark`, `script_turn`, `motion_painted`, `libdom/animation.c` and `idle.c`. The 20 ms cadence now begins at batch start; the host retains a 10 ms post-paint gap. |
| Input lifetime | Can capture changes, DOM mutation, navigation or a refused dispatch leave stale targets or native actions in the wrong order? Inspect `libdom/pointer.c` and Yonder's pointer/input/default-action paths, including iframe navigation. |
| Budgets and failure | Do refused operations leave usable objects and avoid partial publication or leaked ownership? Is work charged where it occurs, including native drawing and shared heap bookkeeping? Existing allocation/cancellation sweeps are evidence to assess, not a substitute for this inspection. |

These are starting points, not a restriction on findings. Separate an actual
contract violation from a deliberately unsupported API, and identify any
uncertainty or missing evidence. Please put the review beside this brief as
`CANVAS_INTEGRATION_FABLE.md`, with concrete failure scenarios and code
locations for actionable findings.

## Acceptance and remaining boundaries

Chris's P5 regression found no remaining issues in the Canvas sites exercised.
Curran's HTML5 examples and the original Phoria Canvas 2D demos provide the
main live-site evidence. Danlegt's signature wall and feature-section rainbow
work; its scratch-off still encounters an Audio startup dependency. The
rainbow's stop-and-resume case is confirmed fixed. Phoria test8 progresses
faster after the pacing change, with qualified measurements in the evidence.

Known boundaries include arrow-key delivery to page scripts, responsive-image
`srcset` selection, Window named access for the Game of Life demo, Audio and
other site startup APIs, and WebGL 2 (used by Phoria2). Software-triangle
JavaScript throughput remains a performance follow-up. Acceptance does not
claim complete Canvas or browser conformance; the supported profiles and
remaining debts still apply.

The P5-accepted cohort predates the final Streams S2 merge-forward. Its final
host/native verification is recorded below; no fresh hardware pass of those
merged bytes is claimed. Review findings come before the final userland merge.
After this milestone, the agreed next work is keyboard input/Breakout, followed
by selecting one online Canvas 2D game and addressing its actual requirements.

## Reviewed S2 merge-forward verification

The combined tree passes:

- DOM fetch/Streams probes: 834 checks with the target engine and 3,753 with
  the sanitized engine, including allocation-refusal sweeps.
- DOM animation probes: 92 checks in each engine profile, including the
  callback-work/frame-deadline regression.
- Scripted-browser integration: 16,288 checks, including picture-worker
  allocation/cancellation and model/layout refusal checks.
- Native `libdom.so` build and its required library dependencies.

Host runs retain ASan/UBSan and leak detection. All checks above report zero
failures. Earlier full Canvas, SVG, layout, page and heap suites and native
QEMU checks are recorded in the integration evidence; they were not repeated
wholesale for the conflict-free Streams S2 merge.
