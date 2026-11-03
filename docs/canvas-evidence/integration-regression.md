# Canvas integration regression

2026-11-01. Candidate combines Canvas integration through `6850d607`, the
Phoria fixes `273651c7` and `82290ed4`, the reviewed heap change `b33e2be7`,
and userland through `17828106` (Streams S1). The merge remains on
`canvas-integration`; hardware regression and the final merge to userland
are separate acceptance steps.

## Reconciliation

- Preserve natural image dimensions for layout, including images whose pixels
  exceeded the kept-memory budget. Resolve hidden layout models by URL instead
  of using the shown model's image indices.
- Carry the Canvas request mode through userland's shared picture-submission
  helper. Display images may retain a reduced decode; Canvas requests retain
  natural-resolution default-frame pixels and their origin/CORS result.
  Picture arrivals and replacement pixels invalidate the layout resource
  revision used by incremental snapshots.
- Preserve both Canvas presentation and userland's filter/blend/backdrop paint
  callbacks. Filtered and blended Canvas ancestors use conservative viewport
  damage. Tests compare partial repaint with a fresh full repaint.
- Extend the layout style-copy/equality inventory for filter and backdrop-filter
  lists, including URL storage. The structure-size guard caught this required
  update. The restyle oracle frees the source tree before comparing computed
  properties with an independent full cascade, and sweeps allocation failures.
- Keep Streams alongside the Canvas/animation/pointer interfaces and retain
  both QuickJS patches numbered 0017: their filenames differ, and the explicit
  manifest applies both. Tests build the combined engine and bindings.

## Regression correction found during verification

The SVG adapter harness inherited shared draw2d sources from the painter test
and also supplied them itself. Its source inventory now avoids duplicate
compilation/linkage while keeping the production list as its source.

Once linked, its allocation-refusal sweep also failed on the pre-merge Phoria
branch. GDB identified the refused allocation as the cascade's optional HTML
mutation reader, introduced by incremental style work. Rendering can succeed
without that reader. The oracle now requires either a clean refusal or an
entire bitmap identical to the successful reference; it checks dimensions,
bytes, every pixel and released ownership. No SVG production change was made
for that test failure.

## Host checks

ASan, UBSan and LeakSanitizer remain enabled for the sanitizer suites. The
existing heap death-test harness runs without sanitizers by design; the new
heap maximum/corruption audit runs with them. GDB diagnostic runs alone
turned off leak checking because GDB uses ptrace; the verification runs retain
leak checks.

| Suite | Result |
| --- | --- |
| Canvas core | 240,341 assertions pass |
| Canvas paths, images, shadows and paints with production geometry | 1,133,636 total assertions pass, including the core checks |
| SVG painting | 1,835,108 assertions pass |
| SVG browser adapter | HTML, cascade, layout, paint, pixels, mutation, cancellation and refusal checks pass |
| Browser scripted-page integration | 16,251 checks, zero failures |
| Browser painter | 2,246 checks, zero failures; six reference paints match |
| Layout | 304,513 checks, zero failures; 12 corpus dumps match |
| Page model | 254,356 checks, zero failures |
| DOM target/sanitized profiles | 28,255 / 190,212 checks, zero failures |
| Existing heap tests | 215 checks, zero failures |
| Heap maximum and verifier | 43,855 checks, zero failures |

The DOM installation-refusal sweeps complete with 3,283 / 29,783 binding
allocation positions across the two profiles. Successful compilation of the
native browser, matching libraries and `malloctest` accompanies the host checks.

## Native checks and handoff

An isolated QEMU guest uses private disk copies and the candidate runtime
cohort. With `HEAPCHECK=1`, `malloctest`, `malloctest threads 4` and
`malloctest churn 3` return their expected success codes. The animation gallery
produces different moving frames and identical paused drawing regions. The
pointer gallery draws through native mouse events. SVG image rendering and
zoom, CSS filter drawing, navigation between galleries and all nine Streams
checks pass. Saved browser records are clean except the filter gallery's
expected unsupported `filter: url` case.

A kernel late-boot memory test reported a loss while the animated browser
started. The same test also fails with the previously accepted Phoria UV cohort
under the same launcher and kernel. With browser startup delayed until after
the late boot tests, the candidate passes the memory test with zero bytes lost
in both measurement windows;
the late phase reports six passed and zero failed (the network case skips
without a NIC). The busy runs do not isolate task teardown from other live
allocations. No kernel change was made. The userland heap verifier tests pass
in both cohorts.

The hardware handoff uses a frozen cohort with a SHA-256 manifest and the
relevant local galleries. Hardware regression should cover normal browsing,
links/forms, scrolling, window resize and zoom, Canvas drawing/animation,
SVG/raster images, navigation away from active scenes, and ordinary userland
applications sharing libos64. It is a functional regression pass, not a new
performance acceptance campaign. Hardware acceptance remains pending.

Software-triangle JavaScript throughput and previously recorded external-site
startup dependencies remain follow-ups. They are not new merge gates.

## Rainbow cursor follow-up

2026-11-03: the rainbow cursor on danlegt.com draws open paths with round
caps and joins. As its points converge on a stationary mouse, a nonzero
centerline becomes smaller than the native stroker's working grid. The
backend returned `UNSUPPORTED`; Canvas exposed `NotSupportedError`, and the
script stopped before scheduling its next animation frame. A standalone
converging polyline reproduces the refusal at length 0.0016384 pixels.

The shared geometry backend retains a round tip for a discarded, nonzero,
open round-cap/round-join contour whose quantized control points remain within
one working unit of its start. It appends the tip to the stroke's combined
outline, preserving transformed bounds, clipping and single-pass alpha
composition. Exact degeneracies, closed contours, square caps, miter joins
and discarded curves that drift farther retain their existing contract.

Validation: 1,234,865 Canvas assertions and 1,835,108 SVG painting assertions
pass with ASan, UBSan and LeakSanitizer. Backend ownership, cancellation,
limits and rendering checks pass. The added Canvas cases cover convergence,
overlap at half alpha, nonuniform scale, and each allocation refusal before
successful rendering. The native FreeType library builds; live-site P5
confirmation is pending. The delivery reuses the combined regression cohort
and replaces only `libfreetype.so`.
