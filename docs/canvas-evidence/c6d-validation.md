# C6d — SVG fill queries and collectible points

The CD-ROM scratch application's progress check asks whether sampled pixels
belong to an SVG shape. C6d supplies that geometry query and its DOMPoint inputs.
The gallery is `tools/pages/canvas-hit-testing.html` (Inside the Lines).

## Contracts

- [SVG 2 fill membership](https://svgwg.org/svg2-draft/types.html#InterfaceSVGGeometryElement):
  local coordinates, current fill-rule, implicit closure and included boundary;
  visual effects do not affect membership, and nonfinite query coordinates are outside.
- [Geometry Interfaces](https://drafts.csswg.org/geometry/#DOMPoint): point defaults,
  writable/read-only coordinates, dictionary conversion, JSON and matrix-dictionary
  transformation. DOMMatrix interfaces and structured cloning are outside this cut.
- Shared SVG derivative roots and endpoint-arc conversion avoid a second path
  parser or a raster-dependent hit test. Bounded monotone intersection uses
  binary64 arithmetic and a documented relative boundary allowance.

## Evidence

Native SVG tests passed 181,624 assertions with ASan, UBSan and LeakSanitizer,
including both circle sweep directions, partial/rotated arcs, curve extrema, holes, implicit closure,
boundaries, extreme scales and cancellation at each visited work position.
The sandbox denied LeakSanitizer attachment with EPERM; the same test binary
passed outside it with leak detection enabled.

Focused DOM tests passed 426 checks per engine profile. They cover point
brands, inheritance, dictionaries and getter order, subclassing, matrix aliases,
query receivers, argument reentry and 20,000 collectible temporary points.
The complete target-engine DOM gate passed 27,866 checks with 3,212 binding
allocation sweeps, including the Modern Web global census and matching
prototype levels with the feature switched off. The fully instrumented engine
passed 169,008 checks and 26,246 allocation sweeps with ASan, UBSan and
LeakSanitizer enabled.

The local captured CD-ROM replay passed 432 checks with no tracked allocations
remaining. Unchanged captured rules, drawing and pointer scripts initialized
four sectors, accepted native pointer strokes, marked all four sectors revealed,
and invoked their completion callback. The fixture substitutes inert symbol
placeholders and omits external CSS; it makes no remote requests or submissions.
This establishes the replayed interaction, not whole-site acceptance.

The complete browser regression gate passed 15,931 checks. It includes current CSS fill rules, reentrant
attribute changes, hidden geometry, refusal of detached nodes without resolved styles, percentage bases, nested viewports
and outer SVG padding.

The gallery's native pointer/button and scheduled-frame tests pass. Its moving
light changes pixels while retaining the layout tree and DOM version. A
scratch stroke erases foil through the retained clip; a fresh-foil action
restores the bitmap. A visible QEMU run confirms the gallery, hover feedback,
scheduled motion and the scratch threshold completing its reveal.

P5 hardware acceptance is approved. The tester confirmed that the gallery
works correctly, including the scratch-off reveal and the self-animating scene.
The tested runtime cohort remains frozen at implementation commit `6dfcd23b`;
this acceptance update changes documentation only.

## Corrections caught during validation

A clockwise arc's re-evaluated endpoint could acquire a tiny nonzero Y value,
changing the shared-vertex winding count. Intersections now retain exact source
endpoints at parameter zero and one. Circle-grid tests cover both directions.

The initial SVG prototype refinement existed only with Modern Web enabled.
The full DOM gate caught the chain mismatch; prototype construction now occurs
in the classic node setup, with interface names and methods still gated.
Expected constructor/tag checks were updated for the refined SVG interfaces.

A circle fixture used an unquoted `r=10/>`, making its attribute value `10/`.
The test markup was corrected rather than weakening the SVG value parser.
