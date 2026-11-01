# C6c — retained SVG paths for Canvas

Baseline: `719dcb0f`, the integration merge of C6b PR #452.

## Implemented profile

`Path2D` accepts no argument, an SVG path string, or another Path2D. Canvas
`fill(path, rule)`, `stroke(path)` and `clip(path, rule)` consume these retained
paths independently of the context's current path. Transforms apply at draw
time; fill rules, pen state, shadows, saved clipping and composite operators
use the existing Canvas renderer.

The binding calls libsvg's public parser and arc lowering directly. Arcs keep
their exact SVG parameters until drawing chooses subdivisions against the
current transform at 1/128 device-pixel positional tolerance. Malformed SVG
retains the complete parsed prefix; resource failure publishes no path.
Copies share immutable native geometry. Mutation methods (`moveTo`, `rect`,
`addPath`, etc.) are outside this profile; later mutation support must preserve
independent-copy semantics. This is not a complete Path2D implementation.

`canvas_draw_path` is the native command-stream entry point. It shares import,
transform and rendering code with text contours, supports open subpaths, and
does not replace the current path. Failed preparation leaves pixels, clipping
and damage intact. The binding completes script conversions before syncing a
possibly resized canvas and lowering its path.

Native path allocations use the existing page ledger. The SVG owner bounds
input at 64 KiB, parsed/lowered commands at 4096 and work at 16,777,216 shared
SVG work units per operation. Wrappers remain in the bounded DOM registry
until teardown, as recorded in DEBTS.md. High-churn object creation will need
collectible wrappers. `libdom` now depends on `libsvg`; Canvas itself keeps its
parser-independent command-stream API. Path2D is declared in the Modern Web
feature table and is absent with that switch off.

## Automated checks

- Native Canvas/geometry: **1,000,232 assertions**, including retained paths
  at draw-time transforms, preservation of the current path, open strokes,
  clipping/save/restore and both allocation-refusal modes for fill/stroke/clip.
- Focused DOM Canvas: **420 checks in each engine profile**, including SVG
  prefixes, embedded NUL, empty/copy/subclass construction, fill rules,
  transformed arcs, shadows, wrong receivers, coercion that resizes the canvas,
  input limits and allocation-failure recovery.
- Complete DOM target profile: **31,686 checks, zero failures**, including
  3,850 binding allocation sweeps and the Modern Web global-name contract.
- Complete DOM sanitized-engine profile: **168,090 checks, zero failures**,
  including 26,094 allocation sweeps through installation and binding operations.
- Complete browser regression: **16,558 checks, zero failures** on the final
  implementation, including image-worker and layout refusal sweeps.
- The real gallery initializes, produces visible pixels in all four canvases,
  runs its buttons, erases foil through native captured pointer input and
  restores it with the reset control. **Zero failures** with tracked memory
  released. Its retained bitmaps were inspected visually.
- Native Yonder and matching shared libraries build successfully. QEMU renders
  the gallery and reveals the message through its scratch card.

Sanitizer runs retain ASan, UBSan and LeakSanitizer. The broader WPT Path2D
surface is not claimed as conformant by this slice.

## Target diagnostic

A private local fixture runs the unmodified public CD-ROM rules, scratch and
pointer modules with minimal sector markup. The four canvases initialize to
160×160 and each receives 25,600 nontransparent foil pixels through the real
`clip(path)`/`stroke(path)` sequence: **15 checks, zero failures**, no retained
tracked memory. Captures verified on 2026-11-01:

| Public module | SHA-256 |
| --- | --- |
| `cdrom-rules.js` | `8d6b25f441ef2412dcd1b74db5952c0d700aac6854d8df03c2a420c30ada9fa4` |
| `cdrom-scratch.js` | `9a999584212854ee934f63ca12a318f2855c482c3f3acd055eecfd17b74b9623` |
| `scratch-pointer.js` | `7e06c108a9bc25f9d1928ec10e013353970f04d4807632108b1183925ba62178` |

The sources are under `https://danlegt.com/static/js/scratch2000/`. The paint
and pointer modules still match the earlier captured inventory. The fixture
substitutes non-image elements for symbol images and omits the site's external
CSS, wallet, modal and surrounding application. It does not submit anything
to the public site and is not whole-site or scratch-completion acceptance.

The CD-ROM progress check additionally calls SVG `isPointInFill()` with a
`DOMPoint`. Those interfaces remain an explicit dependency before claiming
that ticket reveals correctly. Full wall, scratch and selected cursor-effect
acceptance remains C6 work.

## P5 handoff

`tools/pages/canvas-path2d.html`, “Paths Worth Keeping,” has transformed copied
stars, a fill-rule toggle, transformed SVG arcs and a pointer-driven scratch
card. Scripts and Modern Web must be enabled. Chris accepted the frozen
delivery from `545bd45c` on the P5: all four examples worked as expected,
including pointer-driven scratching. This records functional hardware
acceptance, not a timed performance measurement.
