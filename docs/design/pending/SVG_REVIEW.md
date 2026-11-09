# SVG.md — Fable's review of Quinn's revision

*2026-10-25. Read against `userland` at d922229c and Quinn's 5cce7686.
Every claim below about the baseline was checked in the tree, not
recalled. The review's shape: the rulings Chris owns first, then the
findings (each with an action), then what is confirmed and needs no work.
Quinn: this file is yours to commit with a disposition, as E05B was.*

## Rulings (Chris's)

**R1. Where the XML parser comes from — RULED by Chris, 2026-10-25: a
standalone library that libhtml USES, written in-house.** The profile
the design specifies — no external entities, no parameter entities, no
validation, the five predefined entities plus numeric references and
literal-valued internal `<!ENTITY>` declarations (finding 9), CDATA,
comments and PIs, namespace prefixes resolved — is the small part of
XML, and Expat's allocation suite has no context argument, which fights
the per-parser budgets the house charges everywhere. My first lean had
been an XML mode inside libhtml's tokenizer; Chris's shape is better,
for two reasons that are not preference. First, a library of its own
gets its own oracle: the W3C XML conformance suite (xmlconf, the
well-formed and not-well-formed cases) is to it what html5lib-tests is
to libhtml, and a tokenizer mode buried in libhtml could not be run
against it cleanly. Second, the reuse is not hypothetical: yonder
itself will meet `DOMParser` with `text/xml` and `image/svg+xml`,
`XMLHttpRequest.responseXML` and `application/xhtml+xml` pages, and
feeds (RSS has no namespace, Atom its own) are a Chris program waiting
to happen. **The one guard: the library emits EVENTS, not a tree.**
Start-element with the namespace-resolved name and attributes,
end-element, text, CDATA, comment, PI, and the declared encoding — a
push parser fed in bounded chunks, like libhtml's. libhtml's tree
builder consumes those events to make the one node type the cascade,
libdom and the SVG providers already take; a feed reader consumes them
to make whatever it likes. The moment the library grows a node type of
its own there are two trees in the house and every consumer needs an
adapter, which is the cost that makes "reusable" libraries unused.
Encoding: the parser reads the declaration and takes UTF-8 and UTF-16
itself; anything else is reported and the caller transcodes and
re-feeds, so the library depends on nothing. Entity expansion is
charged against the parse budget (an internal subset can still write a
billion laughs).

**R2. Five cuts, and text behind an outline export.** The revision cuts
6a–6e where the original cut 6a–6c, and moves all text to 6d behind a
glyph-outline export from the font engine, refusing a bitmap-only text
provider (§ Implementation stages). Slice order and scope are Chris's.
**My lean: accept the cuts as she drew them**, 6a–6c scheduled, 6d and 6e
booked and started only when a page asks (consumer-driven growth; the
acceptance icons have no text). Her reason for refusing bitmap text is
right: a text provider that cannot stroke or shear is half a feature,
and a half-feature that renders *something* is harder to see than one
whose absence is named.

**R3. Who builds — RULED: Quinn builds the whole of slice 6.** The
"Opus builds" lines in MODERN_WEB.md and MODERN_WEB_BRIEFS.md from
5cce7686 are stale and change in the disposition.

## Findings

Each is a change to the design, with the section it lands in.

1. **Cache by value, not by identity** (§ Caching and animation). The
   paint cache today keys a gradient tile on its resolved geometry and
   stops and a shadow on its shape and blur (`tile_kept`, `shadow_kept`
   in paint.c): no node pointer, no document version, nothing about who
   asked. The design's key — "document lifetime identity, root/instance
   identity, document version, style generation, font/resource
   generation, used viewport…" — is an identity model the cache does not
   use, and it invents two generations (style, font) that nothing in
   libgarb or the font engine publishes today. Key an SVG the way the
   cache keys everything else: a digest of the PREPARED SCENE — resolved
   geometry, resolved paints (colour, not `currentColor`), the used
   viewport, device scale, dark-pages state. Then GitHub's forty
   identical octicons are one entry instead of forty, a stylesheet or
   font arriving changes the digest by changing the scene (no generation
   counter to forget), and the dark-pages `currentColor` interaction is
   in the key because the resolved colour is. The cost is preparing the
   scene every paint to compute the digest; that is a tree walk and style
   queries, cheap beside rasterization, and a per-node memo (node →
   last digest, valid for one document version) is an optimization to
   add when the paint-cost instrument shows the walk, not a correctness
   key. Mine had the same identity model and was wrong the same way.

2. **The XML document mode mostly exists** (§ External documents and
   processing modes). `createDocument` in libdom
   (`implementation.c`) already builds a document node with an SVG
   element as its direct child — no `html`, `head` or `body` — and tags it
   `D_DOCUMENT_SVG`; libhtml's insert takes it. So "the existing
   three-value namespace enum and HTML-root invariants are not
   sufficient by themselves" overstates what is missing. What 6a must
   prove narrows to two things: a parser that PRODUCES that tree from
   bytes (R1), and libgarb cascading over a document whose root is not
   `html` — `:root` matching the `svg`, inherited root values, and
   `<style>` elements in the SVG namespace feeding the cascade. Strike
   the enum claim unless a namespace a renderer needs beyond HTML, SVG
   and MathML can be named; attributes already carry their namespace
   URI (`xlink:`, `xml:`, `xmlns:` — tree.c's adjust).

3. **6b does not wait on the XML-tree decision** (§ Implementation
   stages: "6b needs … a compatible shared XML-tree decision"). With
   finding 2, the decision is made: the tree is libhtml's node in both
   modes, which is what the providers take. 6b waits on libgarb's
   properties and the oracle extension, nothing else. Strike the
   dependency; it would hold the acceptance icons behind a parser they
   do not need.

4. **Name the miter mapping; make the factor a 6a check** (§ FreeType
   geometry contract: "select the join that matches SVG's miter-limit
   behavior and test it"). It can be named now, from ftstroke.h: SVG's
   `miter` becomes a BEVEL when the limit is exceeded, which is
   PostScript's rule and `FT_STROKER_LINEJOIN_MITER_FIXED`;
   `FT_STROKER_LINEJOIN_MITER_VARIABLE` (the alias `MITER`) clips the
   spike instead, which is SVG 2's `miter-clip`. Take SVG's join
   vocabulary at the geometry door (miter, miter-clip, round, bevel;
   `arcs` falls back to miter as SVG 2 allows) and map inside the
   adapter. What DOES need the proof stage: SVG's `stroke-miterlimit`
   is a ratio to the stroke WIDTH, FreeType's `miter_limit` is applied
   to the RADIUS ("multiplied by the radius gives the maximum size of a
   miter spike"), and the two measure the spike from different points —
   a factor-of-two error here is invisible on every join but the one at
   the limit. One reftest at the limit, in 6a, pins it.

5. **Dashing needs a flattening tolerance, and it is a device
   quantity** (§ FreeType geometry contract: "cut dashes in the
   appropriate user space, expand the stroke there"). A dash is cut by
   arc length, and arc length on a cubic is measured on a flattened
   copy. The design says where the cut happens (user space) but not
   how fine: flatten at a tolerance of (device tolerance ÷ the
   transform's largest singular value), so a path drawn at `scale(20)`
   does not show its polygon. One sentence; otherwise the first
   implementer flattens at a user-space constant and the zoomed icon is
   faceted.

6. **The geometry context keeps no ledger of its own** (§ FreeType
   geometry contract: "share the port's allocation/accounting
   implementation; use a dedicated geometry engine", and § Budgets:
   "FreeType allocation uses the same admission ledger"). The two
   sentences can be read as a second cap beside the SVG admission
   budget. Say it once: the geometry context allocates through the
   caller's hooks and carries no cap; the SVG context's 64 MiB ledger
   is the only one, and the port's live-byte accounting is what reports
   against it. Otherwise a render is admitted by one ledger and refused
   by the other, and `/sys`-style diagnostics show two numbers that do
   not add.

7. **The inline outermost `svg` with no `width`/`height` is 100% wide,
   not the default object size** (§ Viewports and paint resources:
   "apply the CSS default sizing algorithm rather than using 300 by 150
   for every missing-size combination"). Right, and the rule for the
   case pages actually ship — a logo as `<svg viewBox="…">` with no size
   attributes — is SVG's own: an absent `width` on an outermost svg is
   `auto`, treated as 100% (SVG 1.1 § 5.1.2; SVG 2 § 8.2 keeps it), so
   the used width is the containing block's and the height follows the
   `viewBox` ratio. The default object size applies to `<img src=svg>`,
   where there is no containing block to fill. Web semantics, so decided
   rather than asked: the reftests under `svg/coordinate-systems/` will
   hold the exact numbers.

8. **`switch` needs its inputs named** (§ Styles and instances: "`switch`
   selects according to conditional-processing attributes"). Say what
   the evaluator reads: `systemLanguage` against the page's language
   list (slice 1a's profile is where the browser's languages live),
   `requiredExtensions` non-empty is false, and `requiredFeatures` as
   WPT's `svg/struct/` switch tests pin it. Without the list the first
   implementation evaluates nothing and picks the first child, which is
   what the revision set out to forbid.

9. **Old Illustrator files use an internal DTD subset** (§ External
   documents: "reject internal DTD subsets in the initial profile").
   Illustrator exports of the 2000s declare `<!ENTITY ns_svg
   "http://www.w3.org/2000/svg">` in the internal subset and write
   `xmlns="&ns_svg;"`; Chrome renders them. Refusing the whole subset
   refuses those files. The profile can admit `<!ENTITY name "literal">`
   general entities with literal values (no parameter entities, no
   external identifiers) at a cost of a dozen lines, and the fixture
   corpus should hold one such file so the choice is measured either
   way.

10. **The oracle extension's shape** (§ Current integration points:
    "extend the intrinsic-size contract"). Name it: one verb answering
    width, height and ratio as three optionals (`has_width`,
    `has_height`, `has_ratio`), replacing the `(w, h)` pair for rasters
    too — a raster is the case where all three are present and the
    ratio is derived. Two verbs, one per picture kind, is the shape
    that drifts.

Text-only, for the disposition and not for Chris: the LeakSanitizer
paragraph in § Verification ("if sandbox thread attachment prevents
LeakSanitizer, rerun … through the approval mechanism") is operating
guidance for one model's sandbox, not design, and it will go stale with
the tooling; it belongs in the brief or VERIFICATION.md, with the
design keeping only "ASan/UBSan with leak detection, and the run is not
done without the leak verdict".

## Confirmed, no action

Not findings. Each is a departure from my 2026-10-21 design that the
revision gets right, listed so Chris can see what changed and why.

- **The stroke is expanded in user space, then transformed.** My verb
  stroked a device-space outline, and `scale(4,1)` on a round cap proves
  that wrong: the cap must become an ellipse. Her two-operation split
  (expand stroke → owned outline; rasterize with transform and clip)
  is the right door.
- **The destination is caller-owned.** image.h says `pixels` is
  `os64_malloc`'d and released by `os64_image_free`; my "host alloc"
  return would have broken that contract on the first free.
- **A `use` instance has its own inherited style.** My style provider
  keyed on the source node and would have coloured a symbol by its
  `defs` ancestry instead of by the `use` that referenced it.
- **CSS `transform` REPLACES the presentation attribute through the
  cascade.** I wrote "composed"; css-transforms-1 says the attribute is
  the property's presentation attribute, and the cascade picks one.
- **Processing modes.** An SVG reached through `<img>` or CSS fetches
  nothing and runs nothing. My design did not say so, and it is a
  security property, not a feature: an image document that could fetch
  is a cross-origin probe wearing a picture.
- **The edge-tolerance critique.** My "a pixel within one pixel of an
  edge is exempt" rule lets a 1 px stroke vanish without failing; a
  thin icon is all edge. Per-fixture tolerances plus negative controls
  that must fail is the right oracle.
- **FT_Outline's counts are `unsigned short`** (ftimage.h, 2.14.3) and
  a fill must not be split to dodge the limit: splitting changes
  winding.
- **A separate geometry table over revision 2 of the font table.**
  exports.map exports exactly one getter today and the leaf property is
  the thing shared.mk protects; a second getter beside the first keeps
  both, and a separate engine keeps SVG scratch out of the glyph cache.
- **The campaign line she corrected was mine** (540aad82: "the SVG
  paint properties libgarb already parses"). libgarb has no `fill` or
  `stroke`; my SVG.md said libgarb gains them, the campaign said it had
  them. Her commit fixed the stale one.
- **The alternatives table**, including the C++ note (the house rule is
  C), and the `<?xml` prefix not identifying SVG on its own.
- **Premultiplied inside, straight at the export**, with group opacity
  composited once; the overlap-must-not-darken test is the right one.
- **`object-fit` is not applied to an inline `<svg>`**; I had said it
  was. The viewport's own `preserveAspectRatio` governs there.

## Quinns disposition

2026-10-25. Checked against the baseline code, FreeType 2.14.3 headers/source,
SVG/CSS specifications and the XML conformance-suite documentation. These are
design dispositions; runtime proof remains in 6a and the subsequent stage gates.

**R1 accepted.** `libxml` is an in-house, standalone push parser emitting events.
It owns no application tree. `libhtml` builds the shared node type from those
events. [XML.md](XML.md) specifies callback lifetimes, encodings, namespaces,
budgets, the bounded entity profile and xmlconf reporting. This replaces Expat.

**R2 accepted as scheduling, not a claim about coverage.** Schedule 6a–6c.
Retain 6d/6e as explicit follow-on work, including text through outlines and
animation, with start/priorities chosen from real needs. The initial delivery
does not claim those capabilities or full modern-web SVG coverage.

**R3 accepted.** Quinn owns slice 6; campaign and brief ownership are corrected.

| Finding | Disposition and change |
|---|---|
| 1 cache by value | Accepted. Key resolved scene values and deduplicate identical icons. A digest is an index, not equality: compare canonical values and test forced collisions. Measure scene-preparation cost. A later preparation memo cannot rely on document version alone because stylesheet/font/resource changes need not change the DOM. |
| 2 existing XML document mode | Accepted for the SVG root; corrected the overstatement. Keep a narrower namespace requirement: `create_document` explicitly rejects no-namespace/arbitrary-URI elements, which RSS/Atom and foreign SVG metadata need. Attribute namespace storage does not fix element identity. Independently owned image documents also need more than a secondary document in a page's arena. |
| 3 inline dependency | Accepted. XML parsing/tree construction does not block 6b. Its geometry proof, SVG properties and extended intrinsic oracle are the relevant gates. |
| 4 miter mapping | Accepted and made explicit. The ratio is unchanged, not doubled: the SVG formula and the imported `FT_Stroker_Set` contract both use reciprocal sine of the half-angle. Width-to-radius conversion halves width only. Below/at/above-threshold tests still belong in 6a; do not treat formula inspection as a raster test. |
| 5 flattening | Accepted with an additional bound on accumulated length and dash endpoints. Device-error divided by largest singular value controls geometric flatness; flatness alone does not bound the arc length of a retracing curve. |
| 6 ledger | Accepted. One SVG admission ledger, geometry allocations through caller hooks, reporting counters without another cap or double charge. |
| 7 sizing | Accepted for the common viewBox-only inline case in a definite-width normal container. Rejected as a universal shortcut: no-viewBox, flex/grid, shrink-to-fit and constrained sizes need their CSS rules. An `img` has a containing block too; the distinction is how its image's intrinsic inputs participate in sizing. Contextual reftests remain required. |
| 8 switch inputs | Accepted. Name the host language list and extension support. SVG 2 removes `requiredFeatures` from conditional processing; retain a pinned compatibility test instead of silently inventing an SVG 1.1 feature table. |
| 9 literal entities | Accepted. Admit the older namespace-entity pattern under byte/work limits. Specify escaping and declaration grammar; this is not assumed to be a dozen-line feature. Nested general-entity declarations and markup-valued replacements are explicitly outside the initial text-literal profile. |
| 10 oracle result | Accepted. One result with optional width, height and ratio, shared with raster callers. |

The sanitizer operating instructions moved to the slice brief; the design
retains the leak-verdict requirement. Original review text above is preserved
as the record of what was reviewed, not silently rewritten to match this
disposition.

The xmlconf oracle needs one qualification to the hoped-for green first run:
this is a restricted, non-validating XML profile. In-profile passes/failures,
out-of-profile cases and unexpected outcomes must be separate. Rejecting a
well-formed file using unsupported DTD features is not a passing XML test.

Evidence for the qualifications:

- [Current createDocument implementation](../../../userland/libdom/implementation.c)
  and [shared node representation](../../../userland/libhtml/include/html/html.h).
- [FreeType stroker contract](../../../userland/libfreetype/upstream/include/freetype/ftstroke.h)
  and [implementation](../../../userland/libfreetype/upstream/src/base/ftstroke.c),
  compared with [SVG miter limits](https://www.w3.org/TR/SVG2/painting.html#StrokeMiterlimitProperty).
- [SVG integration sizing](https://svgwg.org/specs/integration/#svg-css-sizing)
  and [initial viewport negotiation](https://www.w3.org/TR/SVG2/coords.html#ViewportSpace).
- [SVG conditional processing](https://www.w3.org/TR/SVG2/struct.html#ConditionalProcessing)
  and [xmlconf scope](https://www.w3.org/XML/Test/).
