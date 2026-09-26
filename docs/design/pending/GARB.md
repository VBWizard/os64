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
| Selectors | 3, and 4's `:is()`, `:where()`, `:not(<list>)`, `:has()`, `nth-child(… of S)` | 1 | Whole for Level 3; `:hover`, `:focus`, `:active`, `:focus-within`, `:focus-visible` and `:target` never match until a face restyles on them (booked); `:visited` never matches, by privacy as the browsers do; `:has()` matched by brute force; a namespace prefix other than `*` or none needs `@namespace` (booked) |
| CSS Cascading and Inheritance | 4 | 1 | Origins (user agent = libflow's chapter, author), importance, specificity, order of appearance, `style` attributes, `inherit`/`initial`/`unset`/`revert`, shorthands expanding to longhands, `@import`; Level 5's `@layer` booked |
| CSS Custom Properties | 1 | 1 | Whole: `--name` inherited, `var()` with fallback, cycles invalid at computed-value time |
| CSS Values and Units | 3, and 4's `min()`/`max()`/`clamp()` | 1 | `px`, `em`, `rem`, `ex`, `ch`, `%`, `vw`, `vh`, `vmin`, `vmax`, `pt`, `pc`, `cm`, `mm`, `in`, `q`; `calc()` |
| Media Queries | 4 | 1 | `@media` and `<link media>`: media types, `width`/`height` in both spellings (`max-width:` and `width <=`), `orientation`, `prefers-color-scheme` (light), `prefers-reduced-motion` (reduce), `and`/`not`/`only`/`,` |
| CSS Color | 4 | 1 | Named colours, `#rgb[a]`/`#rrggbb[aa]`, `rgb()`/`rgba()`/`hsl()`/`hsla()` in both syntaxes, `transparent`, `currentColor`. libflow's colours are opaque XRGB; alpha is honoured as opaque or fully transparent until pile 3 blends (booked there) |
| CSS 2.1 properties | — | 1 | Every property libflow's struct already holds, and their shorthands: `display`, `font`/`font-*`, `color`, `background`/`background-color`, `margin`, `padding`, `border`/`border-*`, `width`, `height`, `text-align`, `vertical-align`, `white-space`, `text-decoration`, `visibility`, `list-style`/`list-style-*`, `border-spacing`, `border-collapse`, `caption-side`, `float`, `clear` |
| New in libflow, still pile 1 | — | 1 | The cheap ones that make modern pages readable: `box-sizing`, `min-`/`max-width`/`-height`, `line-height`, `text-indent`, `text-transform`, `overflow` (clipping), `background-image`/`-repeat`/`-position` (Y5b's tiler), `white-space: pre-line` |
| Positioned layout | 3 | 2 | `position`, offsets, `z-index`, stacking contexts |
| Flexible Box Layout | 1 | 2 | Whole |
| Grid Layout | 2 | 2 | Whole, subgrid last |
| Backgrounds and Borders, Images, Transforms, Fonts (`@font-face`), Animations | 3/4 | 3 | Gradients, `border-radius`, `box-shadow`, `opacity`, `transform`, web fonts (packet 04's faces), animations on the Y5b ticker |

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
declarations), `garb_free`. A text dump of a sheet and of a cascade, for
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
to (the billion-laughs shape `var()` allows). The numbers are set in the
parser slice against the corpus, and each is a named constant.

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
| G4 | yonder fetches `<link>` sheets and `@import`s, waits for them, lays out again when a late one arrives | the guest, against a local server, and danlegt.com read as far as pile 1 carries it |
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
there, html5lib here) and compares every selector's validity,
specificity, pseudo-element and matched elements with cssselect2 — a
catalogue of 123 covering every feature and its refusals, and 400
generated per page from the page's own names: 3,138 comparisons, none
differ. 70 answers stand on the standard against the reference, each named
in the runner with its section: `:link` and `:any-link` are `a` and `area`
only, `:enabled` is form controls only, `type` is on HTML's list of
attribute values compared without case, `nth-child(… of S)` weighs its
argument (and cssselect2 matches nothing when S is a list), `:is()`'s list
forgives, a user-action pseudo-class may follow a pseudo-element, and the
pseudo-classes cssselect2 does not know (`:required`, `:optional`,
`:read-only`, `:read-write`), checked on a small page by hand. On
danlegt.com's own page (a local copy): 923 generated selectors, and all
896 of its sheets' real selectors — 878 valid, the rest its vendor
pseudo-elements, refused on both sides — agree.

## Booked before the first line

| Debt | Why it waits | Trigger |
|---|---|---|
| `:hover`, `:focus`, `:active` | a restyle on every pointer move is a relayout, and the face does not relayout on hover | pile 1 proven, and a page whose menus only exist on hover |
| `@layer` (Cascade 5) | Level 4 first; a layer is an order within an origin | the first page whose sheets use it |
| `:has()` by brute force | each candidate scans its subtree or its following siblings, a whole page for `:root:has(…)` | a page whose cascade is slow, measured |
| `@namespace` | a prefix other than `*` or none makes a selector invalid, so its rule drops | a page whose sheets declare one |
| wend honouring `display: none` | libgarb proven in one face before a second leans on it | pile 1 proven |
| User stylesheets | a person's own sheet is the user origin; nobody has asked | a person who asks |
| `unicode-range` | the draft reads it from component values, in `@font-face`, not as a token | pile 3's web fonts |
| Encodings beyond libhtml's | a sheet in ISO-8859-2 or Shift_JIS keeps its ASCII and loses the rest | the first sheet whose text is not ASCII and not UTF-8 |
| Alpha blending of colours | libflow's colours are opaque | pile 3 |
