# GENERATED_CONTENT.md — `::before`, `::after` and `content` (slice 8a)

*Opus, 2026-10-31. Slice 8 of MODERN_WEB.md, the arc's last CSS piece:
generated content first, then the rest of the census's CSS. Designed and
built by Opus under the 2026-10-11 ruling (a userland-only design is its
builder's).*

## 1. Why, measured

`content` is the property yonder lacks that the most pages ask for: the
census (740 page loads from 91 sites, the P5's corpus on 2026-10-31) names
`MISSING css-property content` in **555 loads on 70 sites**, above
`transition` (554) and `filter` (473 loads, 63 sites). It is how icon fonts
draw (`.icon::before { content: "\f101"; font-family: icons }`), how
decorative shapes and clearfixes are made (`::after { content: "";
display: block }`), and how separators, required-field stars and quote
marks appear. The rest of slice 8, by the same count:

| Surface | Loads | Sites | Cut |
|---|---|---|---|
| `content` (`::before`/`::after`) | 555 | 70 | 8a |
| `filter` | 473 | 63 | 8c |
| `mix-blend-mode` | 253 | 28 | 8d |
| `backdrop-filter` | 190 | 41 | 8c, beside `filter` |
| `counter-reset` / `counter-increment` | 181 / 163 | 16 / 14 | 8b |
| `quotes` | 71 | 11 | 8b |
| `clip-path` (HTML) | 5 | 4 | booked |
| `will-change` | 0 | 0 | already read, nothing drawn of it |

## 2. What 8a does

- **`::before` and `::after` (and the legacy `:before`, `:after`)** on any
  HTML element whose children make boxes: not on replaced elements (`img`,
  `input`, `video`, `iframe`, `canvas`), not on an element whose display
  is `none`, and in 8a not on a table, a row group or a row (a cell and a
  caption have theirs; DEBTS.md). On an element whose display is `contents` the pseudo-element
  boxes are made where its children are, as CSS Display § 2.5 says.
- **`content`**: `normal` and `none` (no box: a pseudo-element without
  content does not exist), and a list of `<string>`, `attr(<name>)`,
  `open-quote`, `close-quote`, `no-open-quote` and `no-close-quote`,
  concatenated. `attr()` reads the originating element's attribute (the
  empty string when absent). The quote words take the `<q>` element's
  marks and nesting level, which yonder already draws, until 8b brings the
  `quotes` property. An item 8a does not draw (`counter()`, `counters()`,
  `url()`, an image function) is read, kept, and draws nothing, so the
  pseudo-element still exists and keeps its box (a clearfix or a coloured
  square needs no text); `/ <alt text>` is read and not used.
- **Any display and position the pseudo-element's style asks**: inline by
  default, and `block`, `inline-block`, `flex`, `grid`, `absolute`,
  `fixed`, floats, all through the same box generation an element child
  takes. That is the point of § 4.
- **Not in 8a** (booked, each a DEBTS row): counters and `quotes` (8b),
  images in `content` (`url()`), `::marker` and `::first-line`/
  `::first-letter` styling, and `getComputedStyle(el, '::before')`, which
  answers the empty declaration it answers today.

## 3. The cascade (libgarb)

Today `add_style_rule` files a selector only when its pseudo-element is
none (`cascade.c`), so a `::before` rule is parsed and never reached. 8a:

- **A rule's selectors for `::before` and `::after` are filed in tables of
  their own**, one per pseudo-element, keyed by the same id/class/type
  buckets as the element tables. A selector matches its ORIGINATING
  element (select.h: "its pseudo-element aside"), so the lookup is the
  element's own, in the pseudo-element's tables.
- **`garb_style_for_pseudo(cascade, element, pseudo)`** answers the
  pseudo-element's winners exactly as `garb_style_for` answers an
  element's: author origin, one per longhand, CSS-wide keywords as
  `GARB_V_WIDE`. Inline `style` never applies (it is the element's), and
  neither do SVG presentation attributes.
- **Computed eagerly, as the element's are, and only when a pseudo rule
  matched**: a slot keyed by (element, pseudo-element), so an element no
  `::before` rule names costs a failed lookup and nothing else. Custom
  properties inherit from the originating element.
- **`content`** is a new longhand, not inherited (`GARB_CONTENT`), its
  value a list of items (`G_CONTENT`).

## 4. The box (libflow)

**A pseudo-element is a synthetic element node owned by libflow**: an
`os64_html_node_t` made in the style pass's arena, its `parent` the
originating element, holding one text child with the resolved content.
It is inserted, in box generation only, as the originating element's
first child (`::before`) or last child (`::after`), and goes through
`flow_range` like any child: an inline pseudo-element becomes inline
items, a block one a block, an absolute one an absolute box. That is why
§ 2's displays come with no code of their own.

- **Its style is pass 1's**: `inherit()` from the originating element's
  `FStyled`, then its author winners (`garb_style_for_pseudo`) through the
  same `lay_sets`, then `finish()`. The element's `FStyled` keeps a
  pointer to each of its pseudo-elements' (node and style), NULL for none.
- **Its content is resolved once, in pass 1**: the items concatenated to
  one string (`attr()` read from the originating element), which becomes
  the synthetic node's text child. Text from content is `generated`, as
  `<q>`'s marks are, so `text-transform` and selection treat it as such.

### The boundary rule

**A synthetic node never leaves libflow.** A public `flow_box_t` made for
a pseudo-element carries `node` = its ORIGINATING element and
`pseudo` = which one (a new field, `GARB_PSEUDO_NONE` for every other
box). So a click on a link's `::before` lands on the link, as in every
browser, and nothing outside libflow ever holds a node no document owns.
What that asks of the readers of `box->node`:

| Reader | What it must do |
|---|---|
| `flow_box_for(tree, element)` and every walk that finds an element's box by its node | skip boxes whose `pseudo` is set: the element's box is its own (`getBoundingClientRect`, `offsetWidth`, scrollIntoView, a focused control) |
| hit testing (the topmost box under a point) | nothing: the pseudo box answers its originating element, which is the target |
| text extraction and selection | nothing outside libflow reads a box's text for either; were one to, it would skip generated text, as `<q>`'s marks would be |
| paint | nothing: a pseudo box is painted as any box of its kind |

Each site is named in § 7 as built, by audit of the readers of `->node`
outside libflow.

## 5. Lifecycle

| Event | Pseudo-element style | Synthetic node | Box |
|---|---|---|---|
| a style pass (`f_style_build`) | computed for each element a pseudo rule matched, `content` resolved | made in the pass's arena | — |
| a layout (`f_boxes_build`) | read | inserted first / last among the children | made by `flow_range` |
| the element's attributes change (`attr()`), or its class | recomputed by the next style pass | remade | remade |
| `content: none`/`normal`, or `display: none` on the pseudo-element | none: the pseudo-element does not exist | none | none |
| the originating element is `display: none` | not computed (its subtree has no style) | none | none |
| the pass's arena freed | gone | gone | gone with the tree |

Slice 5's incremental relayout (RELAYOUT.md, Quinn's 5b/5c) reuses styles
and boxes between layouts: a pseudo-element's style and node belong to the
originating element's style record, so whatever keeps or recomputes that
record keeps or recomputes them with it. That is the coordination point,
and it is said here so 5b and 8a meet on it rather than discover it.

## 6. Proof

- **libgarb**: the cascade dump shows a pseudo-element's winners under its
  originating element (`tools/garb_corpus/cascade.txt`); `content`'s
  reader accepts and refuses by its grammar.
- **libflow**: box dumps for an inline, a block, an absolute and a floated
  pseudo-element; `attr()`; quotes by level; `content: none`; a replaced
  element and `display: none` generating nothing; `display: contents`
  generating where its children are; layout dumps for a clearfix and an
  icon-font glyph.
- **yonder**: a page whose link's `::before` is clicked navigates; an
  element's `getBoundingClientRect` is its own box's; the record stops
  naming `content`.
- **WPT**: no expectation lost in any `css/` directory or `cssom`;
  `css/cssom`'s `serialize-values.html` passes its twelve `content` values
  (and `serialize-all-longhands.html` holds with `content` among the
  longhands). `css/css-content/` and `css/css-pseudo/` are not in the
  pinned checkout, and are mostly reftests the runner cannot judge.
- **The P5**: the census's top `content` sites before and after.

## 7. As built

- **libgarb**: `GARB_CONTENT` and its reader (`content`, `content_item`),
  modern-marked, 32 items at most; a rule's `::before`/`::after`
  selectors filed in an index of their own per pseudo-element (`Index`,
  `TARGET_*`), so an element's own lookup never steps over them and a
  pseudo-element's lookup runs only when its index holds a rule
  (`targeted`); its winners kept in the element's `Slot`
  (`pseudo[]`), answered by `garb_style_for_pseudo`. The dump prints them
  after their element (`p#y::before: ...`). `content`'s counters and
  pictures are named as approximated, so the record still says what 8a
  does not draw.
- **libflow pass 1** (`style_pseudos`, `style_pseudo`):
  - The winners go through the element's own `lay_sets`, then whatever
    else an element's child is given of it: the medium border and outline
    widths (`medium_widths`), an atomic inline's edge to the decorations,
    and the parent's animation moving by layouts when the pseudo-element
    inherits what it animates (`taken_from`).
  - The content is resolved once (`content_text`), and only for a
    pseudo-element whose display is not `none`. It is charged to the lists'
    budget, since one rule copies it into every element it reaches; past
    the budget the content is lost, and the pseudo-element with it.
  - A synthetic node is made in pass 1's arena. Its name is one of two
    static strings, so `f_pseudo_of` knows it by address. An author's
  `content` on a `q`'s pseudo-element, `none` included, replaces the mark
  `q` draws of its own (`FStyled.pseudo_declared`): a reset sheet's
  `q:before, q:after { content: none }` takes the marks away.
- **libflow pass 2**: `kids` and `sib` walk an element's children with its
  `::before` first and `::after` last. They do so in `scan`, in
  `flow_range`, in the root's container, and in the element, inline,
  inline-block, cell and caption containers.
- **The boundary** is `add()` in flow.c, the one constructor of public
  boxes: a synthetic node becomes its originating element, `pseudo` says
  which, and the box is never the element's first box (`flow_box_for`,
  `flow_image`). `flow_scroller_for` skips them too, and a
  pseudo-element's span holds only its own text for a text clip
  (`flow_visit_text_clip`).
- **The audit of `->node` readers outside libflow** (§ 4's table, as
  found): `geometry.c` (an element's fragments skip pseudo boxes),
  `attribute_picture` and `glass_backdrop` (a `background` attribute is
  the element's own box's), the two scroll-keeping sites (a pseudo
  scroller is not kept), and `anchor_of` (climbs past pseudo boxes and
  generated text); `paint.c`'s inline-block frame test and canvas owner
  hardened. Every other reader — hit testing, links, cursors, pictures,
  controls, painting — is right with the originating element. Nothing
  outside libflow reads a box's text for selection or copy.
- **What waits** is in DEBTS.md § Browser layout: counters and `quotes`
  (8b), pictures in `content`, `getComputedStyle(el, '::before')`,
  `::marker` and the line pseudo-elements, a pseudo-element's animations,
  and tables' own.

## 8. Slice 8b: counters and `quotes`

*Designed after 8a was built, on its branch.* `counter-reset` and
`counter-increment` are named by the census in 181 and 163 loads (16 and
14 sites), `quotes` in 71 (11). They are one piece because both are
**tree-order state**: a `counter()` in an element's `::before` and an
`open-quote` in it both depend on every element before it in the
document, not on its ancestors alone. That is the one new idea 8b brings
to pass 1, and § 8.4 says what it asks of slice 5.

### 8.1 What 8b does

- **`counter-reset`, `counter-increment` and `counter-set`** (Lists 3 §
  4): `none` or a list of names, each with an optional integer (0 for a
  reset and a set, 1 for an increment). A name may be any `<custom-ident>`
  but `none`. `counter-reset`'s `reversed()` is not read: a declaration
  with one is invalid, as in an engine that never had it, so an earlier
  declaration wins.
- **`counter(name[, style])` and `counters(name, joiner[, style])`** in
  `content`: the innermost counter of that name, or all of them from the
  outermost in, joined. With no style given the style is `decimal`. The
  styles spelled are `list-style-type`'s (decimal, the alphas and Romans,
  the bullets, `none`); any other name is spelled `decimal`, as an
  unknown `@counter-style` name is and as `list-style-type` already
  spells it.
- **`quotes`** (Content 3 § 4.1, inherited): `auto`, `none`, or pairs of
  strings. `auto` is the English marks (U+201C/U+201D, then
  U+2018/U+2019); other languages are not chosen. A quote word takes the
  pair at the current depth, and the last pair when the depth is past
  them. An `open-quote` deepens the depth after its mark, a `close-quote`
  shallows it before its mark, and a close at depth 0 draws nothing and
  stays at 0. `no-open-quote` and `no-close-quote` change the depth and
  draw nothing, and `quotes: none` draws nothing for either word.
- **`q`'s own marks** follow `quotes` and the depth as well, so a `q`
  inside a `q` takes the second pair, and a `q` after an author's unclosed
  `open-quote` takes the next one. They stop being pass 2's guess from
  the count of `q` ancestors.

### 8.2 The algorithm (Lists 3 § 4.4, Content 3 § 4.2)

Pass 1 already walks the elements in tree order. It carries a **stack of
live counters** (name, value, and the element whose subtree the counter's
scope ends with) and the **quote depth**, and at each element whose
display is not `none` it does this:

| Step | When | What |
|---|---|---|
| 1 | the element is styled | its `counter-reset`, then `counter-increment`, then `counter-set` (Lists 3 § 4's order) |
| 2 | then | its `::before`, if it exists: that pseudo-element's own counter properties, then its content resolved; or `q`'s own open mark |
| 3 | then | its children, each by these steps |
| 4 | finished, on the way up | its `::after` (moved here from step 2: it follows the children, so it sees their counters and quotes); or `q`'s own close mark |
| 5 | then | pop the counters whose scope ends with it |

- **A reset** pushes a counter scoped to the element's parent: the
  element, its descendants and its following siblings see it. A reset on
  a `::before` or `::after` is scoped to the originating element. When
  the innermost counter of that name has the same scope (a previous
  sibling made it), the reset replaces its value instead: siblings
  replace, never nest (Lists 3 § 4.4.2).
- **An increment or a set** changes the innermost counter of the name,
  and makes one first when there is none, as a reset would (Lists 3 §
  4.2). **A `counter()` or `counters()`** of a name with no counter makes
  one with value 0, scoped to its pseudo-element's originating element.
- **An element whose display is `none`** changes no counter, and neither
  does its subtree, which pass 1 never walks (Lists 3 § 4.5). Neither
  does a pseudo-element that does not exist (`content: none`).
- **Values** are 32-bit integers, and arithmetic past them stays at the
  limit.
- **The bound**: at most `F_COUNTERS_MAX` (1024) counters are live at once.
  Past that a reset makes none and a use reads 0. A page reaches it only
  by keeping a thousand counters in scope together, which is not a page
  anyone writes.

### 8.3 Where it lives

- **libgarb**: four longhands, `GARB_COUNTER_RESET`,
  `GARB_COUNTER_INCREMENT`, `GARB_COUNTER_SET` (not inherited) and
  `GARB_QUOTES` (inherited), all modern-marked. A counter list's items
  are pairs: a name, a KEYWORD as written (so it serializes as the
  identifier it is), then a NUMBER. `quotes`' items are its strings.
  `content`'s `counter()` and `counters()` now keep their arguments (the
  name, the joiner, and the style keyword lowercased, unless it is
  `decimal`, the style unwritten). They are read to their grammar and
  are no longer named as approximated.
- **libflow pass 1**: the stack and the depth live in the pass's `Ctx`.
  Each `FStyled` keeps the `quotes` value it inherited, and a `q` keeps
  its two marks as resolved text (`q_open`, `q_close`, NULL for none),
  which pass 2 draws in place of the marks it chose from the count of
  `q` ancestors. One spelling of a number in a counter style serves both
  markers and `counter()`, so a list's marker and a `counter()` in its
  style never disagree.

### 8.4 Lifecycle

| Event | Counters and depth | A pseudo-element's content | `q`'s marks |
|---|---|---|---|
| a style pass | begun empty and at 0, walked in tree order, gone at the pass's end | resolved at its step (§ 8.2) | resolved at steps 2 and 4 |
| an element before it changes (an attribute, a class, an inserted or removed element) | recomputed by the next style pass | may change: it depends on everything before it | may change |
| `display: none` on an element | its counter properties and its subtree's count for nothing | — | — |

**For slice 5** (RELAYOUT.md's restyle): 8a's note in § 5 holds, and 8b
adds a rule. A style pass that met a counter property, a `counter()` or
`counters()`, a quote word or a `q` marks itself **ordered**
(`FStyles.ordered`). Restyling only the changed elements of an ordered
pass is wrong, because elements after them depend on them. Whatever 5b
reuses, an ordered pass is restyled whole.

### 8.5 Not in 8b (DEBTS.md rows)

- **The `list-item` counter**: `counter(list-item)`, an author's counter
  properties naming `list-item`, and markers numbered by author counters.
  List markers keep pass 2's numbering, which already handles `start`,
  `value` and `reversed`.
- **`reversed()`** in `counter-reset`.
- **`@counter-style`**, and the predefined styles yonder does not spell
  (`lower-greek`, `armenian`, the CJK styles), which are spelled decimal.
- **`quotes: auto` by language**, and `match-parent`.

### 8.6 Proof

- **libgarb**: the four longhands' readers accept and refuse by their
  grammars (`declarations.txt`), as do `counter()`'s and `counters()`'s
  arguments; the cascade dump shows them on an element and its
  `::before`.
- **libflow**: box dumps for:
  - nested lists numbered by `counters(item, ".")`;
  - a sibling reset replacing rather than nesting;
  - an increment with no reset;
  - reset, increment and set in their order on one element;
  - an `::after` seeing its children's increments;
  - a pseudo-element's own increment;
  - `display: none` counting for nothing;
  - every spelled style;
  - quotes by pair, past the last pair, `none`, a close at depth 0, the
    `no-` words, and a `q` nested in an author's `open-quote`.
- **WPT**: no expectation lost; `css/cssom` gains what it serializes of
  the four longhands.
- **Mutants**: one per row of § 8.2's table and one per rule beneath it.
