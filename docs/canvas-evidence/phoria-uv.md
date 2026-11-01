# Phoria UV texturing: bounded clip and image work

Target: <https://kevs3d.co.uk/dev/phoria/test0uv.html>. The user reported an
apparently static scene while the browser occupied almost a full CPU thread.
The saved hardware record shows that callbacks were running: 327 paints over
181.6 seconds, 96% main-loop busy time, 333.8 ms average script-task time, and
0.9 ms average paint/publication time. No script failure was reported. The
half- and quarter-degree increments per callback made this low paint rate
particularly difficult to perceive as animation.

## Change

The renderer textures many small triangles. Previously, a clip retained a
canvas-sized coverage mask, and an image draw rasterized and composited against
canvas-sized masks even when that triangle admitted very few pixels.

Clips now retain immutable packed byte coverage with conservative device-space
bounds. A new clip intersects the previous bounds and multiplies coverage only
inside that intersection. Empty intersections retain an empty clip rather than
reverting to the unrestricted NULL state. Save/restore shares the immutable
object; reset and teardown release it. The shadow path reads the same bounded
coverage through the shared accessor.

Bounded image operations use a destination sub-surface clipped to the outline
and existing clip bounds. The additive `draw2d_paint_image_at` API samples using
the sub-surface's device-space origin and the original image transform. It does
not fold that origin into the matrix, which would change floating-point
evaluation order. Coverage and returned damage remain local to the supplied
surface; Canvas translates damage back to bitmap coordinates.

Gradient draws and operators that clear pixels outside the source retain full
coverage. Public Canvas types, drawing/work limits, origin-clean behavior,
self-copy snapshots and the frame pacing policy are unchanged. No per-site
exception or increased script budget is involved. The earlier basic-cube
performance investigation remains deferred at the user's request.

## Verification

- Production FreeType geometry and pinned math: 1,133,636 Canvas assertions,
  including 240,341 core assertions. Coverage includes all supported composite
  modes, nested/disjoint clips, fractional edges, transforms, smoothing, shadow
  clipping, clip restoration and allocation-refusal atomicity.
- New compact-vs-full comparisons cover 72 image cases. A small clip and image
  draw on a 2048 by 1024 canvas fit a 64 KiB extra allocation allowance.
- The image-region API has explicit tests for device-space origin, local damage,
  the work cap and nonfinite mapped bounds, with no write on refusal.
- Browser integration: 15,942 checks. ASan, UBSan and LeakSanitizer are enabled.
- The unmodified captured page is replayed for 100 frames against a reference
  backend with bounds reporting disabled, comparing each complete bitmap hash.
  All 100 hashes match. The reference uses full-target coverage for both clips
  and images.

An initial three-frame before/after replay reduced script/drawing work from
1.185–1.218 seconds to 0.164–0.165 seconds per frame, with the same final pixels.
These are sanitizer-host measurements, not a hardware frame-rate promise.
Native validation uses a frozen cohort with the matching `libcanvas.so` and
`libdraw2d.so`; the image-dimension fix and other runtime files are reused.
The original scene rotates in the native guest, with distinct before/after
frames and a saved diagnostic record. Hardware acceptance remains pending.
