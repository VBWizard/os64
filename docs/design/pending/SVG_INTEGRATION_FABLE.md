# SVG integration — Fable's architectural read

*Fable 5.1, 2026-10-28, against `codex/svg-computed-style` at 12e091f2, the
integration branch as a whole, read by the seven boundaries
SVG_INTEGRATION_REVIEW.md names. Codex has reviewed the code; this is the
shape, not the lines. Findings are numbered; what I checked and found as
designed is listed after them without numbers.*

## Findings

**1. The scene is prepared on every paint, and the booked cure points the
wrong way.** Both routes do the same thing per paint of a box: build an
adapter, read the cascade for every element up its chain (`compute`), parse
every `transform` attribute, walk the document for every reference, prepare
the scene, serialize a key from it, and only then ask the raster cache. The
image route (`svg_picture_pixels`) also builds a fresh cascade of the
picture's document first. The window's thread does all of it. Only the
rasterization is cached. DEBTS books this (§ 6b "prepares synchronously",
§ 6c "preparation and cache misses are synchronous") and names an
asynchronous SVG worker as the direction. A worker moves the cost; it does
not remove it, and the cost repeats for nothing: the inputs do not change
between paints. The morning's finding on the wallpaper page is the
multiplier this needs to be read against: a ticking animation paints the
view 40 times a second, and every inline icon in the dirty rectangle is
prepared 40 times a second.

The lever is retention of the prepared scene, keyed by what it was built
from. libhtml already has `os64_html_version`, which moves on every change
a reader could see; a scene kept on the page (per SVG root) with the
version, the cascade's identity and environment, and the zoom it was built
at is valid until one of those moves. For the image route the document is
immutable by construction (IMAGE mode, no script, no mutation, own sheets),
so its cascade and scene are properties of the document and the view size:
prepare once at first paint, keep them with the Picture, and let the raster
cache be the second level it was designed to be. Size M. The raster cache,
its keys and its ownership rules stay exactly as they are; this adds a
level above them, not a change inside them.

**2. A reference is found by walking the whole document, and the walk is
per root per paint.** `fragment_target` starts at `doc->document` and
visits every node until the id matches, for every `use`, every gradient
`url()` and every `clip-path`, charged against a 4,000,000-step budget per
root. Its first walk cannot stop at the match, because it is also the walk
that looks for `<base href>` and `xml:base` (`base_checked`), so the first
reference of every render visits the entire page. An icon sprite page, the
common shape for `use`, has hundreds of roots each with one reference: roots
× page nodes per paint. libdom's `getElementById` walks the same way. One
per-document id index in libhtml, kept by the attribute and insertion verbs
and invalidated by version, serves both consumers; the base check is a
document property computed once, not a render's. Size S for the base flag,
M for the index. Finding 1 hides this until a script mutates the page;
then it is back at every paint.

**3. One unsupported visible node blanks the whole root, and the two
design documents disagree about whether it should.** `scene.c` ends the
scene with UNSUPPORTED at the first displayed node it has no kind for
(`text`, `image`, `foreignObject`, a pattern paint), and yonder paints
nothing and records `FAILED svg`. SVG.md's verification section says
"optional-feature omission reports a diagnostic and partial capability";
SVG_YONDER.md says "refuse the job without publishing partial pixels". The
code follows the second. The corpus cost is real: an icon with a `<text>`
label draws nothing, and the two pages I read with Chris today both carry
"inline SVG uses an unsupported feature". My lean, for Chris to rule: an
unsupported *element* is omitted and counted (the specification's own
handling of an element the renderer does not know is to not render it,
which is also what a browser does with a `<foreignObject>` it cannot lay
out), while an unsupported *value on a supported element* keeps refusing
the root, because drawing a supported shape with the wrong paint is a lie
about the art, and an absent label is not. Size S either way; the policy
is the decision.

**4. The raster is keyed by the clipped rectangle, so a partly visible
picture is re-rasterized at every scroll step.** `yonder_svg_render`
renders the intersection of the box and the paint clip, and the key
carries the target's origin and size. A large inline SVG half in view gets
a new key at each scroll position, misses every time, and fills the 32
entries with offsets of one picture. Render the whole box whenever its
pixel area is under the existing 8 Mpx cap and let the glass clip; key by
the box. Above the cap the clipped render stays, which is the case it was
written for. Size S.

**5. The cascade is now computed twice, and the newer copy is the better
home.** 12e091f2 gives libflow's computed style the 24 SVG properties:
CSS-wide keywords, inheritance, relative lengths at the declaring ancestor,
currentColor on the receiving element, URL origins. `svg.c`'s `compute` and
`apply` resolve the same semantics by their own walk for the renderer. Two
implementations of one cascade rule drift the day one gains a property the
other does not; the brief's own question is where the shared computation
should live. The provider should read resolved values from the style
record libflow already keeps for SVG descendants, and keep for itself only
what is SVG's: presentation attributes, the `transform` attribute, viewport
units, the instance ancestry of a `use`. Not for this merge; the moment is
the next property that has to be added to both. Size M.

## Checked and found as designed

- **XML events, not a tree (R1).** `xml.h` emits events with a
  context-carrying allocator and per-parser limits; libhtml's builder
  consumes them into the one node type. An internal entity's replacement
  text may not reference another entity and may not contain markup
  (`expand`, `replacement`), so there is no expansion bomb to budget; the
  budgets are there anyway. A parse that fails frees the partial document
  (`os64_html_xml_parser_finish`).
- **Image isolation.** The picture's cascade is built from the document's
  own `<style>` elements only; nothing on that route fetches; `script` and
  the animation elements are inert in IMAGE mode; a non-fragment reference
  resolves to nothing rather than to the network. The picture job parses
  and sizes on a worker, and libgarb has no mutable statics for that to
  trip on; rendering is on the window's thread.
- **Lifetime.** A render pins the document for exactly its own duration
  (`acquire`/`release`), the scene is released before the adapter returns,
  and the raster cache keeps keys by value with no pointer into the DOM, so
  destroying a document with a cached raster alive is safe. The window's
  cache dies with the page.
- **Keys by value.** The key serializes every item's resolved style, its
  path commands, its clip, the viewport maps, the options and the target;
  gradients and dashes are copied into the scene at preparation. A shared
  gradient changed after a hit changes the key.
- **Strokes.** The backend takes the user-space centreline and a planned
  map, so expansion happens in user space and the map follows; the
  `scale(4,1)` round-cap pitfall is the reftest's. Geometric bounds exclude
  stroke and effects (`svg_path_bounds`), as object-bounding-box
  coordinates need.
- **Instances.** A `use` naming its own ancestor resolves to nothing at the
  adapter and the scene classifies whole loops; gradient `href` chains are
  bounded by a seen list and the depth cap; instance style follows instance
  ancestry (`remember`, `compute`), with variable-dependent sources refused
  rather than guessed.
- **Canvas.** Path parse and lowering, path bounds, and both gradient
  samplers take numbers and a map with no DOM, cascade or allocator in the
  call; the document and reference semantics stay in the adapter.

## Order

Finding 3 is a ruling and costs nothing to make. Findings 1, 4 and 2 are
one performance slice in that order, measured on the P5 with the wallpaper
page and an icon-sprite page, after the branch lands; none blocks the
merge. Finding 5 is a direction for the next property, written down so it
is not rediscovered.
