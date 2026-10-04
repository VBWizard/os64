# GARB.md — the page's garb: yonder's cascade library

*Written 2026-09-26 by Opus, who owns it and builds it. Named by Chris —
"the tree is the page's body, the stylesheet its garb" — over `librill`,
confirmed by a d6 (he took even; it came up 2). LAYOUT.md's second
producer, promised there on the first page and designed here.*

## The ruling this rests on

1. **Standards, never a site.** libgarb implements named CSS modules at
   named levels (the table below), each WHOLE or with its missing parts
   written down. A page that renders wrong is a missing rule of a spec,
   found and named; it is never a page to be matched. danlegt.com — a 2026
   site dressed as 1998, 172 KB of CSS over 28 sheets, 198 properties — is
   the YARDSTICK for the ORDER the rules are built in, because a real page
   says which rules pages lean on (Chris, 2026-09-26: "we'll support some
   CSS standard, not a website's CSS implementation").
2. **The second producer, and nothing more.** LAYOUT.md ruled the cascade
   a SECOND PRODUCER of `flow_style_t`, arriving after the Rendering
   chapter with no field renamed. So the Rendering chapter stays where it
   is — libflow's user-agent origin, compiled by hand, section by section —
   and libgarb delivers the AUTHOR origin: what the page's sheets and
   `style` attributes say, cascaded, for libflow's style pass to apply
   after the presentational hints. libgarb decides WHICH declaration wins;
   libflow decides what the winner COMPUTES to (`em` against the font,
   percentages against the containing block, `inherit` against the
   parent), because that is what it already does for the chapter.
3. **LAYOUT.md's house rule**: pure computation, host harness, checked-in
   dumps. Sheets, a tree and the viewport in; cascaded values out; no I/O.
   Fetching a sheet is a face's job, like fetching a picture.

## Where it sits

```
  yonder (the face)   fetches the page's sheets on the pool (cookies,
                      Referer), waits for them before the first layout,
                      lays out again when a late one arrives
  libflow             pass 1 asks libgarb per element, after the hints:
                      the chapter (UA) → hints → libgarb's author values
                      → computed style
  libgarb             sheets parsed once; per layout: every element's
                      cascaded author declarations, var() substituted,
                      media queries judged against the viewport
  libpage             lists the page's sheets in document order: each
                      <style>'s text, each <link rel=stylesheet>'s
                      resolved address and media — where a sheet comes from
                      is a fact about the page, as a picture's is
  libhtml             the tree
```

wend reads no stylesheet in this design. A terminal cannot show most of
what CSS says, but it could honour `display: none` — the cookie banners and
menus it prints as text today — and that is booked for when libgarb is
proven, not designed around.

## The modules, and how much of each

Levels are the CSS Working Group's; "whole" means every rule of the module
that the properties libflow lays out can reach.

| Module | Level | Pile | How much |
|---|---|---|---|
| CSS Syntax | 3 | 1 | Whole: the tokenizer, the parser, error recovery exactly as written (an invalid declaration is dropped and parsing goes on — the rule that lets a 2026 sheet run in a browser that knows 1998's properties) |
| Selectors | 3, and 4's `:is()`, `:where()`, `:not(<list>)`, `:has()`, `nth-child(… of S)` | 1 | Whole for Level 3; `:hover`, `:focus`, `:active`, `:focus-within`, `:focus-visible`, `:target` and `:indeterminate` never match, and `:checked` follows the page's attribute and not a person's tick, until a face restyles on them (booked); `:visited` never matches, by privacy as the browsers do; `:has()` matched by brute force; a namespace prefix other than `*` or none needs `@namespace` (booked) |
| CSS Cascading and Inheritance | 4 | 1 | Origins (user agent = libflow's chapter, author), importance, specificity, order of appearance, `style` attributes, `inherit`/`initial`/`unset`/`revert`, shorthands expanding to longhands, `@import`; Level 5's `@layer` booked |
| CSS Custom Properties | 1 | 1 | Whole: `--name` inherited, `var()` with fallback, cycles invalid at computed-value time |
| Environment Variables | 1 | 1 | `env()` replaced as `var()` is; this machine defines none of the variables, so its fallback is what a page gets |
| CSS Values and Units | 3, and 4's `min()`/`max()`/`clamp()` | 1 | `px`, `em`, `rem`, `ex`, `ch`, `%`, `vw`, `vh`, `vmin`, `vmax`, `pt`, `pc`, `cm`, `mm`, `in`, `q`; `calc()`. Level 4's small, large and dynamic viewport units are the viewport's (yonder has no toolbar that comes and goes), and Containment 3's container units fall back to it as the specification says for an element with no container |
| Media Queries | 4 | 1 | `@media` and `<link media>`: media types, `width`/`height` in both spellings (`max-width:` and `width <=`), `orientation`, `prefers-color-scheme` (light), `prefers-reduced-motion` (reduce), `and`/`not`/`only`/`,` |
| CSS Color | 4 | 1 | Named colours, `#rgb[a]`/`#rrggbb[aa]`, `rgb()`/`rgba()`/`hsl()`/`hsla()` in both syntaxes, `hwb()`, `transparent`, `currentColor`. `lab()`, `lch()`, `oklab()`, `oklch()`, `color()` and `color-mix()` are booked: a declaration using one is invalid, so the page's fallback before it stands. A colour keeps its alpha into libflow (as how transparent it is, flow.h) and yonder lays it over what is really under it (PILE3.md § The blend) |
| CSS 2.1 properties | — | 1 | Every property libflow's struct already holds, and their shorthands: `display`, `font`/`font-*`, `color`, `background`/`background-color`, `margin`, `padding`, `border`/`border-*`, `width`, `height`, `text-align`, `vertical-align`, `white-space`, `text-decoration`, `visibility`, `list-style`/`list-style-*`, `border-spacing`, `border-collapse`, `caption-side`, `float`, `clear` |
| New in libflow, still pile 1 | — | 1 | The cheap ones that make modern pages readable: `box-sizing`, `min-`/`max-width`/`-height`, `line-height`, `text-indent`, `text-transform`, `overflow` (clipping), `background-image`/`-repeat`/`-position` (Y5b's tiler), `white-space: pre-line` |
| Positioned layout | 3 | 2 | `position`, the insets and `inset`, `z-index`, stacking contexts — POSITION.md's slices. All are read now, and `opacity` and `pointer-events` with them: relative and absolute are laid out (P1), fixed (P2), stacked by `z-index` (P3) and sticky (P4), and an opacity of 0 is not painted, while one between makes a group yonder composites whole (PILE3.md § The blend) |
| Flexible Box Layout | 1 | 2 | Whole — FLEX.md's slices. Every property is read, with `flex`, `flex-flow`, `gap` and `grid-gap`, and laid out: one line in all four directions (F1), and wrapping lines with `align-content` (F2); `@supports` says yes to both |
| Grid Layout | 2 | 2 | GRID.md's slice: every property but the `grid-template` and `grid` shorthands is read, with `grid-column`, `grid-row`, `grid-area` and the `place-*` shorthands, and laid out — tracks of every kind with `repeat()` and auto-repeat, placement by line, span and area, auto-placement with `dense` and by columns, alignment of items and of tracks; `@supports` says yes. Named lines in brackets, subgrid and masonry are booked there |
| Backgrounds and Borders, Images, Transforms, Fonts (`@font-face`), Animations | 3/4 | 3 | Gradients, `border-radius`, `box-shadow`, `opacity`, `transform`, web fonts (packet 04's faces), animations on the Y5b ticker. What G2b already decided about gradients: a linear or radial one is VALID when it fits Images 3's grammar (so the pre-standard `linear-gradient(top, …)` is not, and the colour before it stands), conic gradients and `image-set()` are taken by name, and a valid one draws nothing yet while the `background` shorthand still resets the colour |

## What goes in

- **The sheets, in document order**, from libpage: a `<style>`'s text, or
  a `<link>`'s bytes once the face has fetched them — and the address each
  came from, because a `url()` or an `@import` inside a sheet resolves
  against the SHEET, not the page. A `<style>` or `<link>` whose `media`
  does not match is kept and judged per layout, since a resize changes the
  answer. A `<link>` that never arrived is simply absent.
- **Each element's `style` attribute**, read off the tree, which outranks
  every sheet at the same importance.
- **The viewport**: its width and height in CSS pixels, for media queries
  and viewport units — so the cascade is PER LAYOUT, and a resize that
  crosses a breakpoint lays the page out with the other rules, as a
  browser does.
- **The document's quirks mode** (libhtml already reports it): quirks mode
  lets a page write a length without a unit and a colour without its `#`
  in a few properties, and matches class and id names without regard to
  case.

## What comes out

Per element, the author origin's cascaded declarations — one per property
that any author rule set, `var()` already substituted, parsed into typed
values (a length with its unit, a percentage, a number, a colour, a
keyword, a list, a `calc()` tree) — and libflow applies them after the
hints. Inherited custom properties are carried down the tree by libgarb,
because `var()` needs them resolved before a value can even be parsed.

The door is small, in LAYOUT.md's manner: `garb_sheet_parse` (bytes and
the sheet's address in, a parsed sheet out, pure), `garb_cascade` (sheets,
tree, viewport in, a cascade out), `garb_for(cascade, node)` (an element's
declarations), `garb_free`. A cascade pins its document snapshot until
`garb_cascade_free`; its document and parsed input sheets must outlive it.
This protects borrowed attribute bytes when the tree changes underneath
an older painted snapshot. A text dump of a sheet and of a cascade, for
the harness and a probe in the guest.

## The cost

A 2026 page brings thousands of rules and a thousand elements, and the
naive cascade is their product. So rules are BUCKETED by the rightmost
compound selector's id, then class, then tag, then everything else — the
browsers' rule hash — and an element tries only the buckets its own id,
classes and tag name; selectors match right to left. Bounded where libhtml
and libflow bound themselves, and refused out loud past a bound, never
silently: bytes of sheet per page, rules, `@import` depth, nesting of
`calc()` and of `var()` substitution, the length a substitution may grow
to (the billion-laughs shape `var()` allows). Each bound is a named
constant.

**What a parse costs, measured** (`tools/test_garb_host.sh`, `--cost` and
`--full`). Ordinary CSS — a Bootstrap-shaped sheet, long selector lists and
short declarations — costs about 16 bytes of arena for each byte of text to
parse, and about 19 once every rule's block has been read as its items,
which is what the cascade does. So the 96 MiB arena (`GARB_ARENA_MAX`)
holds about 6 MiB of such a sheet parsed, about 5 MiB with every block
read; the harness holds a 4 MiB sheet to coming back whole at under 22.
Denser CSS costs more — a sheet of nothing but `a{b:c}` about 59 bytes a
byte, 77 with its blocks read — so it fills the arena at under 1.5 MiB. A
sheet the arena cannot hold comes back INCOMPLETE WITH WHAT IT FINISHED:
the arena always keeps room to publish the lists still open, so filling it
refuses the next token, never the sheet's rules so far.

The costs are where they are because of three choices: a list is built on
a scratch stack and kept at its exact size (doubling in the arena left
every smaller copy behind, and cost three times as much), a component value
packs to 56 bytes, and a one-byte ASCII text is shared rather than kept.
Outside the arena, and freed when the parse ends: the tokenizer's code
points (4 bytes a byte of input, 32 MiB at `GARB_SHEET_MAX`), the decoded
text of a sheet given as bytes (up to 3 bytes a byte), and the scratch
stack, as deep as the lists open at once. What the faces reserve for a
parse on the pool has to count those too.

## What the faces owe

- **yonder fetches `<link>` sheets on the pool** exactly as it fetches
  pictures — one job per address, the browser's cookies and the page's
  Referer (`way_fetch_hooks`) — and `@import`s as a sheet reveals them.
- **A page's sheets are waited for**, as every browser waits, so the first
  thing on the glass is not a page without its garb: up to three seconds
  from the page's arrival, then the page is laid out with what came, and
  laid out again when the rest does (the relayout already coalesced for
  pictures).
- **A sheet that fails is a sentence**, like a picture that fails: the page
  says how many sheets it could not have.

## Slices

| Slice | What | Proof |
|---|---|---|
| G0 | This document; libpage's list of sheets (`<style>` and `<link rel=stylesheet>` in document order, resolved, with `media`) | libpage's harness and allocation sweep |
| G1 | The parser: CSS Syntax 3's tokenizer and parser, the sheet's object model (rules, selectors, declarations as component values), error recovery | a dump per sheet, hand-checked; the Syntax module's own examples; every allocation failed; a fuzzer against Python's `tinycss2` as the differential reference |
| G2a | Selectors: parsing from a prelude, specificity, matching against libhtml's tree, the rule hash's key, An+B | css-parsing-tests' An+B; a differential against cssselect2 over html5lib on the corpus pages |
| G2b | Values: the property table, each pile-1 property's grammar, shorthands expanded, lengths, `calc()`, colours | css-parsing-tests' colour files; grammar cases worked by hand |
| G2c | The cascade: the rule hash, importance and order, `style` attributes, custom properties and `var()`, media queries | a cascade dump per element for fixtures worked by hand |
| G3 | libflow applies it: every field it holds today from an author rule, `inherit`/`initial`/`unset`; `<style>` pages in yonder and in `flowdump` | libflow's harness with author sheets; the corpus unchanged where there is no CSS |
| G4 | yonder fetches `<link>` sheets and `@import`s, waits for them, lays out again when a late one arrives; an imported sheet stands in its importer's place at that `@import`, its rules BEFORE the importer's own (`garb_sheet_in_t`'s `via` and `parent`) | the guest, against a local server, and danlegt.com read as far as pile 1 carries it |
| G5 | The cheap new properties in libflow (the table's row) | libflow's harness |

Pile 2 and pile 3 are designed in their own sections when pile 1 is
proven, with danlegt.com re-measured then to order them.

**G1, as run.** `tools/test_garb_host.sh`, under ASan and UBSan:
css-parsing-tests (vendored, CC0) — all 177 parser cases pass, with 20
skipped by rule: 9 whose answer is a `unicode-range` token, which the
current draft no longer makes, and 11 in ISO-8859-2 and -5, which libhtml
does not read either. The suite predates two things the draft does — a
match operator is two delimiters, a declaration's value is trimmed — and
the runner brings its answers up to the draft before comparing. Then every
allocation failed in turn over the corpus sheet, nothing leaked.
`tools/test_garb_differential.py` against tinycss2 1.4: the corpus sheet,
its 21 blocks and 15,000 mutations of it agree; so do danlegt.com's 28
sheets (172 KB), their 1,117 blocks and 9,000 mutations of them (run from a
local copy, never checked in). One more draft rule the reference predates:
a top-level rule whose prelude begins `--x:` is thrown away. In the guest,
`/tests/garbdump /tests/pages/sweep.css` prints the sheet and every rule's
block.

**G2a, as run.** An+B: css-parsing-tests' 128 cases pass. Selectors:
`tools/test_garb_select.py` parses each corpus page on both sides (libhtml
there, html5lib here) and compares every selector's validity, specificity,
pseudo-element and matched elements with cssselect2 — a catalogue of 130
covering every feature and its refusals, and 400 generated per page from
the page's own names: 3,180 comparisons, none differ. 100 answers stand on
the standard against the reference, each named in the runner with its
section: `:link` and `:any-link` are `a` and `area` only, `:enabled` is
form controls only, `type` is on HTML's list of attribute values compared
without case, `nth-child(… of S)` weighs its argument (and cssselect2
matches nothing when S is a list), `:is()`'s list forgives, a user-action
pseudo-class may follow a pseudo-element, a pseudo-element inside `:not()`
and a `:has()` inside `:has()` are invalid, and the pseudo-classes
cssselect2 does not know (`:required`, `:optional`, `:read-only`,
`:read-write`, `:indeterminate`), checked on a small page by hand. On
danlegt.com's own page (a local copy): 923 generated selectors, and all
896 of its sheets' real selectors — 878 valid, the rest its vendor
pseudo-elements, refused on both sides — agree.

**G2b, as run.** Colour: css-parsing-tests' keyword, hexadecimal, hsl and
hwb files, 1,822 cases, all pass (channels to 1e-4; the suite prints six
decimals of its own arithmetic). Grammars: 157 declarations worked by hand
from the specifications (`tools/garb_corpus/declarations.txt`) — the box,
borders, sizes and `calc()`, colours, display, fonts and the `font`
shorthand, text, lists, tables, `background` with its layers and positions,
the CSS-wide keywords reaching every longhand of a shorthand, quirks mode's
unitless lengths — all pass; eight mutants of the grammar code are all
caught, three of them only after cases were added for them (a `+` without
white space before it, legacy `rgb()` mixing numbers and percentages, a
negative percentage). Real-world: danlegt.com's 3,766 declarations — 2,048
read, 1,655 name properties libgarb does not read yet, and the 63 held
over for the cascade are exactly those with `var()` or `env()`. That run
found four gaps before this commit: the dynamic and container units, env(),
and background layer lists.

**G2c, as run.** Media Queries 4: 55 queries worked by hand against a
stated viewport (`tools/garb_corpus/media.txt`) — types, `not`/`only`,
plain, `min-`/`max-` and range forms, the machine's own answers, and Level
4's third value (an unknown feature is UNKNOWN, and so is its negation).
The cascade: 13 pages worked by hand (`tools/garb_corpus/cascade.txt`) —
specificity over order, importance, the `style` attribute in all four
combinations, invalid declarations leaving the one before them, `var()`
with fallbacks, chains, a cycle and shadowing, the empty value and `unset`
on a custom property, env(), `@media`, `@supports` (grid and flex
containers supported) and a `style` element's `media`, the same page
narrower, combinators, pseudo-element and `:hover` rules reaching nothing,
quirks mode's case-free names and unitless lengths, a rule matched through
two of its selectors at the heavier. All pass; eight mutants of the
cascade and media code are all caught, five of them only after cases were
added for them. The allocation sweep now cascades the corpus sheet over a
page, 35 allocations each failed alone, nothing leaked. danlegt.com, its
28 sheets inlined in order, cascades in 0.03 s on the host: 471 elements
with author winners, hand-checked against the rules for the body and
`#motd`.

**G3, as run.** libflow computes the author winners after the chapter,
the hints and the quirks (`style.c`, "The page's own sheets"): every field
`flow_style_t` holds, the CSS-wide keywords (`revert` goes back to the
chapter's sheet, its link rule and its quirks — the user-agent origin —
without the hints), font-size and color first so every em and every
currentColor on the element sees them, lengths in every unit libgarb reads
(`rem` from the root's size, `ex` and `ch` as half an em, the viewport units
from the size the cascade was judged at), and `calc()` resolved at style
time into a fixed part and a percentage — which `flow_length_t` now carries
as `offset`, so `calc(100% - 2em)` survives to layout.
`tools/test_libflow_host.sh` adds 9 styled pages worked by hand (author over hints, the style
attribute, font sizes by %, em, rem, keyword and `larger`, the four
CSS-wide keywords, `var()` into `calc()` and `vh`, display and borders, a
border style alone at medium in currentColor, `@media` at two widths, a
quirks page's families), 3 pages laid out by hand for what a sheet asks
of layout that no attribute ever did — negative margins, collapsing to
the largest positive plus the most negative (CSS 2.1 § 8.3.1), and
inline-blocks that shrink to fit and sit on their last line's baseline
(§ 10.3.9, § 10.8.1; before, the one inline-block with content was a
marquee, which fills its line) — and a sweep that fails each of the style
pass's allocations with a cascade in force: all pass, and the 12 corpus
dumps — pages with no author sheet — are unchanged. yonder parses a page's `style`
elements once, cascades them again only when the view's size changes (and
relays out on a height change when there are sheets, for `vh` and the
media queries), and frees a cascade only with the tree that points into it.
In the guest, `/tests/flowdump` passes with an `h2` its own sheet hides,
and `yonder /tests/pages/garb.html` draws the showcase: cards in the
accent's borders at `calc(100% - 72px)`, centred by `margin: auto`, a
later class winning, `display: none`, square markers inside, a collapsed
table, a style attribute, a row of inline-block pills, a negative margin,
and the media query flipping between `flowdump garb.html 800` and `500`.

**G4, as run.** libgarb reads `@import` (`garb_sheet_imports`: the
address, a `layer` passed over, `supports()`, the media list; only before
every rule but `@charset` and a `@layer` statement), and the cascade takes
an imported sheet where its rules belong — before its importer's — applying
only when its own media and `supports()` hold and its importer applies.
`tools/garb_corpus/cascade.txt` adds 3 pages whose sheets `@import` sheets
supplied beside them (`@@SHEET <address>`), worked by hand: order against
the importer, a nested import, media at two widths, `supports()` both
ways, an import after a rule, an import under a print sheet; three mutants
of the new code are each caught. yonder fetches linked sheets and imports
on the work pool (`sheet.c`: `text/css` only, but for a same-origin sheet
on a quirks page; parsed on the worker in the protocol's charset, else the
document's), resolves an import against its sheet's address after
redirects, and never follows one already on its importing chain. A page
that names sheets waits for them, the old page on screen, for up to 3 s;
a sheet later than that is laid in when it lands; Stop shows the page with
what came. Fixing this found libpage refusing every relative reference on
a `file:///` page — `os64/url.h` reads its empty host as none — so a page
read from disk could not name a picture or a sheet beside it; libpage now
reads an empty file host as this machine (LIBPAGE.md § A), with 9 new
libpage checks. In the guest, against a local server: an imported sheet,
a nested relative import, both import loops fetched once, a `text/plain`
sheet and a print sheet ignored, an alternate never fetched, a 20 s sheet
shown bare at 3 s and laid in when it landed; `yonder
/tests/pages/garb.html` draws its linked sheet and that sheet's import from
disk; and https://danlegt.com/ fetches its linked sheets and is dressed by
them as far as pile 1 carries it. Stop during the wait is not proven in
the guest: the monitor's pace lands the press after the 3 s.

**G5, as run.** The cheap properties, in four commits. **G5a**
`box-sizing` and `min-`/`max-width`/`-height` (CSS 2.1 § 10.4: a width that
breaks a limit is worked again with the limit as the width set, so
`max-width: 40em; margin: 0 auto` centres; a min-height stops margins
collapsing through, § 8.3.1; a picture keeps its ratio in the dimension
the page left it, `img { max-width: 100% }`). **G5b** `line-height` (every
inline box's leading, split above and below, § 10.8.1), `text-indent`,
`text-transform` (ASCII and Latin-1, whose case changes keep their UTF-8
length, so selection offsets hold) and `white-space: pre-line`. **G5c**
`overflow`: every value but `visible` cuts what is inside to the padding
box — drawing, hit-testing and the page's extent — and a scrolling or
hiding box starts a block formatting context; `hidden`, `auto` and
`scroll` scroll it, and a person scrolls the last two (PILE3.md §
Scrolling boxes). **G5d**
background pictures from a sheet: libgarb's winners now say which sheet
each came from, so yonder resolves a url() against THAT sheet (after its
redirects), or the page's base for a `style` attribute, fetches it with the
page's pictures, and tiles it with `background-repeat` and
`background-position` (a percentage of the room the picture leaves). Host:
libflow 10507 checks (11 layouts and style dumps worked by hand across the
four), yonder 65 (a painted clip, the tiler's two new modes and the
placement arithmetic by hand — which caught a negative offset rounding the
wrong way), garb 15 cascade pages; the 12 corpus dumps unchanged. Guest: a
page whose linked sheet names pictures relative to itself draws its
checkered canvas, a `repeat-x` band, a `no-repeat` dot at `right 10px
center`, a 400px line cut at its 120px `overflow: hidden` box, and a
`style` attribute's picture resolved against the page.

## Booked before the first line

| Debt | Why it waits | Trigger |
|---|---|---|
| `:hover`, `:focus`, `:active`; `:target`, `:indeterminate`; `:checked` after a click | a restyle on every pointer move is a relayout, and the face does not relayout on hover; a tick a person makes lives in libpage's model, which `:checked` does not read, so `input:checked + label` does not follow a click | pile 1 proven, and a page whose menus only exist on hover |
| `@layer` (Cascade 5) | Level 4 first; a layer is an order within an origin | the first page whose sheets use it |
| `:has()` by brute force | each candidate scans its subtree or its following siblings, a whole page for `:root:has(…)` | a page whose cascade is slow, measured |
| `@namespace` | a prefix other than `*` or none makes a selector invalid, so its rule drops | a page whose sheets declare one |
| wend honouring `display: none` | libgarb proven in one face before a second leans on it | pile 1 proven |
| User stylesheets | a person's own sheet is the user origin; nobody has asked | a person who asks |
| CSS Nesting | a style rule's nested rules are parsed (G1) and not cascaded; only its declarations count | the first page whose sheets nest |
| `@layer`'s order | a layer's rules are cascaded as if unlayered, so a layered rule may beat an unlayered one it should lose to | a page whose sheets use layers against each other |
| Pseudo-elements' styles | rules for `::before` and the rest are matched and set aside: nothing generates their boxes yet | G5's generated content |
| `unicode-range` | the draft reads it from component values, in `@font-face`, not as a token | pile 3's web fonts |
| `lab()`, `lch()`, `oklab()`, `oklch()`, `color()`, `color-mix()` | each needs its colour space converted to sRGB and gamut-mapped (Color 4 § 13; `color-mix()` is Color 5, and mixes in one of those spaces) | a page whose colours are only written that way |
| A cache of sheets | a sheet is fetched again for every page that names it, and on every visit | a site whose sheets are slow to come again, measured |
| An imported sheet's Referer | it names the page, not the sheet that imported it | a server that refuses an import for it |
| More than 64 sheets on a page, or 16 `@import`s in one sheet, or an import chain 16 deep | the rest are not fetched; a chain deeper than the cascade's `NEST_MAX` is fetched but not applied, and the cascade says it is incomplete | a page that needs them |
| Every layer of a background | yonder draws one picture behind a box, so the first layer of a list is kept and the rest are only checked | a page whose look depends on a lower layer |
| Quirks mode's hashless colour (`color: ff0000`) | quirks mode's unitless lengths are read; its colours without a `#` are not yet | a quirks-mode page written that way |
| Encodings beyond libhtml's | a sheet in ISO-8859-2 or Shift_JIS keeps its ASCII and loses the rest | the first sheet whose text is not ASCII and not UTF-8 |
| Inline tables | `inline-table` is laid out as a table | pile 2 |
| `min-content`, `max-content`, `fit-content` sizes | libflow sizes a box by its container; the keywords read as `auto` (and as none on a limit) | pile 2, where flex and grid need content sizing anyway |
| `box-sizing` and the limits on tables and cells; §10.4's table for a picture held by two limits against its ratio | a table's width comes from its columns and a cell's from its column, and neither reads them yet; a picture is held by width then height, keeping its ratio in the dimension the page left it | a page whose tables or pictures read wrong for it |
| A percentage `height`, `min-height` or `max-height` | libflow sizes heights by content, so there is no containing block of known height for one to be a percentage of; each binds nothing | pile 2 |
| `vertical-align` by a length, and `text-bottom` | libflow aligns by keyword: a length or percentage is baseline, `text-bottom` is bottom | G5 |
| `white-space: break-spaces` | drawn as `pre-wrap`: a space it keeps at a line's end hangs instead of taking room | a page whose preformatted text reads wrong for it |
| `text-transform` past Latin-1, `ß`/`ÿ`, and `capitalize` across element boundaries; `text-indent`'s `hanging` and `each-line` | a case change that alters a letter's UTF-8 length would move every offset a selection maps through; a word's start is judged per text item; the two keywords are read and not drawn | a page in a script with case, or one that needs them |
| A string list marker, and counter styles beyond the ten | libflow draws its ten marker kinds; any other name is decimal, as Counter Styles 3 says of an undefined one, and a string is ignored | G5's generated content |
| A table column's `calc()` width | a column keeps a percentage's share of the table and no fixed part, so `calc(20% + 10px)` on a cell is 20% | a page whose tables are sized that way |
| `font-variant: small-caps` | read by libgarb and not drawn: it needs a face's small capitals, or capitals made smaller, and yonder has one face today | brief 04's faces |
| `background-size`, `-origin`, `-clip`, `-attachment`, and a gradient as a picture | a sheet's picture is drawn at its own size, positioned from the border box's corner, under the whole border box, and scrolls with the page; a gradient is no picture | pile 3 |
| The viewport's own overflow, and form controls inside a clipping box | the root's (or the body's) `overflow` is the viewport's and yonder always scrolls the page; a control is a widget of its own over the page and is not CUT by an ancestor's clip — a widget is whole or absent: one whose control's centre is clipped away is absent, since the pointer cannot reach it there (POSITION.md, ruling 9), and one whose centre shows is drawn whole, over the clip's edge | a page that hides its viewport's overflow on purpose (a modal), or a form inside a box that clips |
