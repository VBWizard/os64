# SVG same-document instances

2026-10-26. This 6b increment follows [strokes](SVG_STROKES.md), on
`codex/svg-instances`, based on integration merge `0f1abb7b` (PR #372).
Review targets `codex/svg-integration`. The hardware checkpoint is
`/tests/pages/svg-use.html`, **SVG: made to be reused**.

## Shared source, separate ancestry

`libsvg` adds `SVG_NODE_USE` and a provider `reference` callback. A displayed
use is an opacity/transform container whose referenced source becomes one
child. Source siblings and the use element's DOM children are excluded.
The callback returns zero for an empty reference. The source keeps its
snapshot-local identity; each visit receives a new instance identity and
its actual instance parent's identity/style. The provider structure grows;
rebuild consumers and install the matching library cohort together.

The ancestry check examines the edges from the current occurrence back to
its nearest matching ancestor. If that loop contains a reference edge, the
repeated occurrence is suppressed, including when an ordinary child closes
the loop. Its ordinary siblings remain eligible for traversal. A child-only
loop remains `SVG_PROVIDER_ERROR`, even when a reference above the loop led
to that subtree. Each reference, visit
and ancestry comparison spends work; depth, visits and allocations retain
the library limits. Missing callbacks and callback refusals unwind the
scene, owned paths/dashes and snapshot pin. Use containers participate in
isolated opacity just like groups.

Yonder keeps a growable, caller-ledger-owned table of source/parent/instance
records. Its maximum is 4,097 slots, including the zero sentinel. It grows
geometrically from 16 slots, charges replacement capacity before releasing
the old allocation, and frees the table when the snapshot is released.
It reconstructs inherited style along the instance chain, then the outer
SVG's HTML ancestry. It does not clone DOM nodes or make a shadow DOM.

Source declarations still win over inherited host values. `currentColor`
remains symbolic until each consumer's color is resolved. Non-inherited
opacity, transform, display and vector-effect reset per node. Explicit
CSS geometry inheritance follows instance ancestry too; percentages are
resolved in the destination viewport. Use x/y is a translation after the
winning CSS/attribute transform. Its width/height do not resize a referenced
path, shape or group. The root flow transform/opacity boundary is unchanged.

## References and CSS boundaries

Only fragment hrefs are admitted. The adapter decodes percent escapes,
looks up the first matching ID in document order, and can find artwork in
another SVG root. Template contents remain separate trees. An unresolved
ID, a first matching non-SVG node, an empty href or a self/DOM-ancestor
reference paints no referenced content. An active expansion cycle suppresses
the repeated occurrence while preserving non-cyclic sibling artwork.
An href attribute takes precedence over xlink:href even when empty.

External hrefs return `SVG_UNSUPPORTED`; the adapter issues no fetches.
Documents with nonempty HTML base href or xml:base are also refused when
resolving a fragment: supporting these needs URL resolution, not an
assumption that a fragment is document-local. The first resolution checks
the document for base changes; the pinned snapshot makes that check reusable.
ID scanning and parent climbing spend the adapter's four-million work budget.
Expanded visits remain capped at 4,096, including visits suppressed by style
or cycles; ancestry is capped at 128. A failed job publishes no output pixels.

Selector matching uses the source element's cascade, while inherited values
come from the instance parent. This is the selected Chromium-tested profile:
`defs rect` can style a referenced source, whereas `use rect` does not become
a new match. It is not a claim of complete SVG2 shadow-tree selector behavior.

`libgarb` exposes `garb_style_needs_substitution`, a conservative indication
that a matched non-custom declaration needed var()/env() substitution. It
includes declarations later superseded in the cascade and invalid substitutions.
An instanced source with that indication returns `SVG_UNSUPPORTED`, because
reusing its DOM-resolved value could import the wrong custom-property ancestry.
Ordinary use hosts can use DOM-resolved custom properties and pass their
computed paint down. Custom-property definitions alone do not trigger refusal.
Per-instance substitution, shadow-tree selector rematching, referenced
symbol/SVG viewports and clipping remain in [DEBTS.md](../../../DEBTS.md).

These semantics are grounded in SVG2's [use element and its shadow tree](https://www.w3.org/TR/SVG2/struct.html#UseElement).
The static profile does not expose SVG animated-length objects, instance
DOM APIs, hit testing or timed animation.

## Evidence and reproduction

The adapter harness compares reused artwork with explicitly expanded artwork,
including currentColor, source overrides, nested uses, transformation order,
CSS geometry inheritance, opacity, dashes, href precedence, percent decoding,
missing/duplicate IDs, cycles and excluded siblings/DOM children. Additional
checks cover cross-root reuse, source mutation, substitution/base/viewports
refusal, hidden external references, bounded exponential expansion and each
allocation failure in both admission and allocation modes. The window suite
checks source mutation repainting two SVG roots while a host color change
stays local. Leak detection stays enabled alongside ASan and UBSan.

```sh
ASAN_OPTIONS=detect_leaks=1 python3 tools/test_svg_core.py --output /tmp/svg-use-core
ASAN_OPTIONS=detect_leaks=1 python3 tools/test_svg_core.py --paint --output /tmp/svg-use-paint
ASAN_OPTIONS=detect_leaks=1 python3 tools/test_svg_core.py --yonder --output /tmp/svg-use-yonder
ASAN_OPTIONS=detect_leaks=1 bash tools/test_garb_host.sh
ASAN_OPTIONS=detect_leaks=1 bash tools/test_yonder_scripts_host.sh
python3 tools/svg_reftests/use.py --native /tmp/svg-use-yonder --output /tmp/svg-use-browser
```

The browser runner records its version and native input hashes. Integral
fixtures require exact browser pixels except the half-opacity overlap, whose
896 covered pixels allow one channel step for alpha rounding. Independently
shifted controls must fail the same gates. Native use/expanded pairs require
exact bytes, including both opacity cases. These selected fixtures do not
establish general SVG conformance or a page-scale performance guarantee.

## Recorded results

- Native core: **155,998 checks**.
- Painter: **1,241,506 assertions**, including allocation/cancellation sweeps.
- Adapter: **26 exact instance/expanded pairs**, **266 allocation cuts** in both
  refusal modes; the existing 35 stroke and 19 shape fixtures also pass.
- Window/scripts: **8,258 checks, zero failures**, including cross-root mutation.
- Full CSS host suite passes; libpage: **254,176 checks, zero failures** and
  **399 allocation cuts**. ASan, UBSan and LeakSanitizer enabled.
- Pinned Chromium **140.0.7339.16**, Playwright **1.55.0**: **78/78 expected
  outcomes**, including 26 rejected shifted controls. The
  [machine-readable record](../../svg-evidence/use-results.json) carries
  source/binary hashes and comparison bounds.
- Freestanding Yonder and flowdump build succeeds. The `eq` helper was renamed
  because emitting that symbol conflicts with an Intel-syntax assembler operator.
- Windowed QEMU on private root/home disks: all six cards, reversible mutation
  across SVG roots, scrolling and 400% zoom pass. The 25 installed guest files
  match the frozen delivery's hashes. Orderly shutdown completes and both
  filesystems pass `e2fsck -fn` with exit 0.
- P5: Chris confirmed repeated clicks on **Change the shared drawing** update
  the copies, including their cousins in the other SVG root. The 400% zoom
  check also passes. This is interactive hardware validation, not a measured
  frame-rate or timed-animation claim.

The descendant-cycle review regression first reproduced `SVG_PROVIDER_ERROR`
with no output pixels. The corrected traversal passes that example and a
second fixture retaining artwork before/after the closing child edge and
outside the expansion. Both match Chromium exactly. Native checks also reject
a child-only loop inside referenced content and sweep allocation/cancellation
failures through the mixed-edge cycle. The correction passes the freestanding
build; the guest/P5 captures below record the original gallery checkpoint.

Guest captures: [initial](../../svg-evidence/yonder-use-top.png),
[changed source](../../svg-evidence/yonder-use-changed.png),
[nested uses and dashes](../../svg-evidence/yonder-use-middle.png),
[cross-root copies and opacity](../../svg-evidence/yonder-use-bottom.png),
[400%](../../svg-evidence/yonder-use-zoom.png),
[restored](../../svg-evidence/yonder-use-restored.png),
[restored cross-root copies](../../svg-evidence/yonder-use-restored-bottom.png).

## P5 checkpoint

Install Yonder, flowdump and the matching shared libraries together. Open
`/tests/pages/svg-use.html` with Modern Web enabled; enable scripts for the
button. Card 1 starts with three colored diamonds. The button toggles them
to checkmarks and back; card 5's two copies should change with them. Colors
stay independent. Card 3 tests nested copies, card 4 inherited dashes, and
card 6 opacity applied to each overlapping pair. Scroll, switch page theme
and zoom to 400% to check repaint.
