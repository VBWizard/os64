# MIX_BLEND.md — `mix-blend-mode` and `isolation` (slice 8d)

*Opus, 2026-11-01. Slice 8d of MODERN_WEB.md, after 8c (FILTER.md). A
userland design, its builder's (the 2026-10-11 ruling); written before code
because it changes how a group is laid onto the page.*

## 1. Why, measured

The census (740 page loads, 91 sites; GENERATED_CONTENT.md § 1) names
`mix-blend-mode` in **253 loads on 28 sites**, the last of slice 8's paint
properties. Pages use it for text and logos that invert over whatever is
behind them (`difference`), shapes that darken or tint a photo
(`multiply`, `screen`, `overlay`), and duotone pictures.

## 2. What 8d draws

- **`mix-blend-mode`** (Compositing and Blending 1 § 3.2, Level 2 for the
  `plus-` modes): `normal`, the twelve separable modes (`multiply`,
  `screen`, `overlay`, `darken`, `lighten`, `color-dodge`, `color-burn`,
  `hard-light`, `soft-light`, `difference`, `exclusion`), the four
  non-separable ones (`hue`, `saturation`, `color`, `luminosity`), and
  `plus-lighter`. `plus-darker` is read and named approximated: its
  definition differs between Level 2 drafts and one engine, and nothing in
  the census writes it.
- **`isolation`** (§ 3.3): `auto` and `isolate`, read; `isolate` makes a
  stacking context (§ 5 says what it does not yet do).
- **Layout effects**: a `mix-blend-mode` other than `normal`, and
  `isolation: isolate`, each make a stacking context. Neither makes a
  containing block.

## 3. How a blended element is drawn

A blended element is a GROUP (libflow's `is_group`), and its pixels are
recovered exactly as a filtered group's are (FILTER.md § 4): painted onto a
black and a white sheet in the one walk, each pixel's premultiplied colour
`Cs·αs` and coverage `αs` recovered from the two. No primitive changes.

At the close, instead of laying the layer over what is under it
(source-over), it is BLENDED with it (§ 9 of the spec, with an opaque
backdrop, which the window always is):

    result = (1 − α) · Cb + α · B(Cb, Cs)

`Cb` the window's pixel, `Cs` the layer's colour unpremultiplied, `α` its
coverage times the group's opacity, `B` the mode: per channel for the
separable modes, through `Lum`/`Sat`/`ClipColor` (§ 10.2) for the
non-separable ones. `plus-lighter` is `min(1, Cb + α·Cs)`.

**One layer, filtered then blended.** Filter Effects 1's order is filter,
clipping, masking, opacity, then blending, so an element with both a filter
and a blend mode opens one layer: the filter runs on it, and the close
blends it at its opacity. The verbs that open and close a filtered group's
layer become the verbs of any LAYER (`layer_open`, `layer_close` with the
filter list and the mode), and the painter's `layer_try` becomes
`layer_try`; a layer is opened when a filter is drawn or the mode is not
`normal`.

**The backdrop is the window.** The spec blends with the content behind
the element inside its isolated group: the nearest ancestor that makes a
stacking context. yonder paints every group but the laid ones in place, on
the window, so the backdrop it has is everything painted there before the
element. That is exact when the nearest stacking context is the root (the
page's content, the common case) or an ancestor with an opaque background
under the element, and differs only where an isolating ancestor leaves the
page showing through itself (§ 5).

## 4. Where it is drawn, and where not

On the window only, as a backdrop is (FILTER.md § 7). Inside a group
painted onto sheets — an opacity group laid through a matrix, a filtered
group, a text clip — the backdrop is the group's black and white sheets,
and a blend is not linear in its backdrop, so the two sheets would no longer
recover the group: there, and for an element that is itself drawn through
its matrix, the layer is laid with `normal`, counted
(`yonder_paint_layers_refused`, which the record names `layer: drawn
plain`, filters' refusals as well), and booked.

## 5. Not in 8d (DEBTS rows)

- **`isolation: isolate`, and an isolating ancestor generally, does not
  hide the page from a blend inside it**: the backdrop is the window, not
  the isolated group's own content (§ 3). It differs where the isolating
  element is transparent under a blended child.
- **A blend inside a group on sheets, or through a matrix**, is drawn
  `normal` (§ 4).
- **`background-blend-mode`** (blending an element's background layers with
  each other) is not read: nothing in the census writes it.
- **`plus-darker`** is named approximated and drawn `normal`.

## 6. Lifecycle

| Event | Style | Layer | Damage |
|---|---|---|---|
| a layout | `mix_blend`, `isolation` computed (`flow_style_t`); a stacking context; a group when the mode is not `normal` | — | as any group's: no spread of its own |
| a paint reaching the group's OPEN | read | two sheets opened, unless on sheets already or past the budget | — |
| its CLOSE | — | recovered, filtered when it has a filter, blended onto the window at its opacity | — |
| on sheets, through a matrix, past the budget | — | laid `normal`, counted | — |
| something under it changes | — | the part repaints in paint order, the group with it | the changed part, as today |

## 7. Proof

- **libgarb**: both properties read to their grammar, `plus-darker` named
  approximated (`declarations.txt`, `skips.txt`).
- **libflow**: a blended box and an isolated box make stacking contexts;
  a blended box is a group; `getComputedStyle` reads both back.
- **yonder, pure** (`filter.c`'s lay, host-tested pixel by pixel): each
  mode against WPT `css/compositing`'s reftest expectations where they give
  a colour, and against a reference written apart from the code for the
  rest; coverage and opacity scaling the blend; `normal` laying exactly as
  today.
- **The painter**: a blended group opens a layer and closes it with its
  mode; a filter and a blend share one layer; on sheets and through a
  matrix it is laid `normal` and counted.
- **WPT** `css/compositing` parsing pages (testharness), and no loss in
  `css/cssom`, `css/cssom-view` and `css/filter-effects/parsing`.
- **QEMU**: a page of every mode over a photo-like gradient, a
  `difference` heading over stripes, a filtered and blended card.

## 8. As built

- **libgarb** reads both properties (`kBlendModes`, `kIsolation`) and
  names `plus-darker` approximated.
- **libflow** keeps `flow_style_t.mix_blend` and `.isolate`; `f_blended`
  and `f_isolated` make the stacking context (and `f_blended` the group);
  `getComputedStyle` writes both back.
- **yonder**: the filter layer became the LAYER (`layer_open`,
  `layer_close` with the mode; `layer_try`, `blend_of` in the painter;
  `yonder_layer_recover`, `yonder_layer_lay`; the counter
  `yonder_paint_layers_refused`, named `layer: drawn plain` in the record).
  `yonder_layer_lay` keeps `normal`'s integer source-over and blends every
  other mode in doubles by § 10's functions. The pane lays a blend `normal`
  and counts it when the targets outside are sheets; the painter gives the
  root and `plus-darker` no blend.
- **Proved**: each mode against a reference written from the spec apart
  from the code (`~/os64-opus-kit/tools/blend_ref.py`) and WPT's
  `difference` case; coverage and opacity scaling a blend; the painter's
  layer, its mode, a filter and blend sharing one layer, the root and
  `plus-darker`; WPT `css/compositing/parsing` 25 passes, nothing lost in
  `css/filter-effects/parsing`, `css/cssom` or `css/cssom-view`.

