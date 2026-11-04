# Canvas C2 validation

C2 connects the retained 2D bitmap to libdom and Yonder. The gallery is
`tools/pages/canvas-pixels.html`; enable Scripts and Modern Web.
P5 validation passed: all four gallery interactions work at every tested zoom
from 25% through 400%. `c2-bundle.sha256` identifies the frozen cohort used
in QEMU and on the P5.

## Automated checks

- Native canvas: 166,244 assertions, zero failures, ASan/UBSan/LSan.
- Full DOM suite during integration: 20,224 target-engine checks and
  131,008 sanitized-engine checks, zero failures. Allocation sweeps included.
- After the final source-alpha and argument-count corrections: focused
  Canvas/ImageData suite, 110 checks per engine profile, zero failures.
- Final scripted Yonder integration: 11,728 checks, zero failures, including
  scrolled pixel damage, same-value resets, unchanged layout for pixel writes,
  overlapping content and concurrent CSS size changes.
- Final painter suite: 2,030 checks, zero failures; six corpus paints match.
- Cross build of Yonder and its shared libraries succeeds with warnings as errors.
- Leak checking remains enabled in sanitizer runs.

## Upstream baseline

WPT pin: `a699fb16767be39ba3e61a8e8b6bef60383f08ee`.
The seven expectation files under `tools/wpt/expected/html/canvas/element/`
record 302 passing subtests. They also record 231 failing subtests and one
timeout across the broader families; this is a partial Canvas implementation.
The pattern animation timeout awaits the image/pattern path. Missing paths,
transforms, strokes, gradients and later drawing-state fields account for many
of the failures. Unsupported color profiles and the large ImageData exception
limit are recorded in DEBTS.md. Required core cases were also checked with
Modern Web disabled to establish that they depend on this implementation.
The broad baseline preceded the final putImageData overload-count correction;
the focused final suite covers rejection of four, five and six arguments.

## Interactive checks

The frozen cohort contains Yonder, its 21 matching shared libraries and the
gallery. Thirteen unchanged binaries were reused after comparing allocated
ELF sections against the prior SVG release cohort. Guest-installed files were
read back and compared byte for byte before boot.

QEMU checks cover palette switching, scrolled ImageData undo, same-width reset
and repaint, and moving the same canvas between containers. Screenshot pixel
comparison confirms reset clears the bitmap, repaint restores the prior
pixels, and moving retains identical displayed pixels. A QEMU-discovered
page/screen-coordinate error in damage invalidation is covered by a host
regression and the final scrolled interaction checks.

At 25% all four cards remain visible; at 400% the enlarged retained mosaic
paints correctly within the scrolled viewport. The P5 hardware check also
passed all four samples at every zoom, including repeated rapid palette
switches.
