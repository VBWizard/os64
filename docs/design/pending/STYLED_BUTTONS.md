# STYLED_BUTTONS.md — a `<button>` the page draws

*Opus 5.5, 2026-11-08, for Chris. The cure for DEBTS's "Every `<button>`
is the toolkit's blue button" (Quick wins, High) and its Browser layout
row, written down before the code because it changes what libflow calls
a replaced element.*

## 1. What is wrong, and how much of the web it is

libflow lays out every `<button>` as a replaced box (`f_replaced`,
boxes.c), an empty slot like an `img`'s, and yonder puts a libui button
in the slot, captioned with the element's text, or with "Button" when it
has none (`button_caption`). Nothing the page put inside is laid out, and
nothing the page styled is drawn: not an SVG icon, a span, a background,
a border, a radius or a size.

**The corpus** (every pull to 2026-11-08, 333 hosts, the latest page
source of each; buttons that scripts build come on top): 204 hosts write
a `<button>` in their page source; **178 of them write one with an
element inside it**, and 132 an icon-only one, which today reads
"Button". Of 3,935 buttons, 3,002 hold elements (`svg` and `span`
nearly always) and 933 hold text alone.

## 2. The rule

**A `<button>` is laid out as any element, unless its content is text
alone and the page drew nothing of its box.** Then, and only then, it
is the toolkit's button, as today. Precisely, it keeps the toolkit face
when all of these hold:

- every child is text (or a comment), and the text is not all white
  space, since an empty button's "Button" names nothing the page said;
- it hosts no shadow tree, and the page's rules give it no `::before`
  or `::after` that draws (an icon font's glyph beside the words);
- no author declaration (a sheet or the `style` attribute) sets its
  `background-color` or `background-image`, a border's style, width or
  colour, or a corner's radius. A `revert` does not count: it hands the
  property back to the user agent, whose button it is.

This is CSS UI 4's "devolvable widget" rule in the shape os64 can draw:
in other browsers a button keeps its native look until the page styles
its background or border, and its content is always laid out. Here the
native look is the toolkit's button, which draws its own caption, so
the face is kept only where the caption is the whole of the content.

The decision is libflow's, because replaced or not is a question about
the box tree: `f_replaced` takes the element's style, and a style
records whether the author touched those properties (style.c can
already ask `author_sets` of a property; the new bit is set there). A
button laid out this way also gets its `::before` and `::after`, which
icon fonts draw with.

## 3. How a laid-out button is laid out (HTML's Rendering chapter, "The button element")

- It is a block formatting context's root, whatever its `display`
  (`flow-root`'s behaviour; `flex` and `grid` work as on any element).
  An inline one is an atom, as an inline-block is, and a `display:
  table` one is a block, so its box always carries its control.
- A block-level one whose `width` is `auto` is as wide as its content
  (fit-content), not as wide as its containing block.
- Its content is centred in the block axis when the box is taller than
  the content: every fixed-height icon button needs it. The auxclick
  demo's full-window button needs it too, and also a `height: 100%`
  that resolves, which waits on DEBTS's "A percentage height in normal
  flow is read as `auto`": until then that button is a full-width bar,
  its words centred and its click recolouring it.
- The Rendering chapter centres a button's text: `button { text-align:
  center }`, added to libflow's chapter. A toolkit-faced button already
  draws its caption centred, so nothing changes for one.

## 4. The widget stays, and stops painting

The libui button is still the control: it takes the click, the focus,
Space and Enter, and the submission, and it fires every event a script
listens for, as today. For a laid-out button it rides at the box
unpainted, through the `unpainted_class` door `forms_place` already
uses for an invisible control's gadget. Three small changes:

- `element_box` gives a block-level laid-out button's box its `control`,
  as an inline one's atom already has (otherwise `forms_place` finds no
  box and hides the widget);
- `forms_place` chooses `unpainted_class` when the box is not `replaced`;
- the painter calls the face's `control` hook only for a replaced box,
  and paints a laid-out button's background, border and children as any
  element's;
- `flow_box_covered`'s `draws_at` counts a control as drawing its whole
  box only when it is replaced: a laid-out button answers as any box,
  so a transparent one laid over a field does not hide the field;
- the click is dispatched at the element under the pointer when that is
  inside the button (the icon, not the button), and bubbles; what the
  click does (submit, reset) is still the button's. With the modern
  switch on, the pointer pipeline already dispatches it there; with it
  off, the widget's `button_clicked` now targets it the same way. A
  key's press is dispatched at the button.

A hit at the centre answering one of the button's own children is
inside it, so `flow_box_covered` keeps a laid-out button's widget.

## 5. Out of this slice

- **`<input type=submit|button|reset>` the page styles** stays the
  toolkit's button. Its caption is its `value`, not content, so laying
  it out needs an anonymous text box. That's a DEBTS row of its own.
- **`appearance: none`** is not parsed yet. A page that writes it nearly
  always sets a background or border too, which the rule catches.

## 6. The question for Chris

**An unstyled button that holds elements** (`<button><svg/></button>`
with no CSS) shows the browser's grey native face in Chrome, the icon
on top. Here the toolkit face cannot be drawn behind laid-out content
(libui paints its widgets over the page, so the face would cover the
icon). **My lean: draw no face, the icon alone, and book it.** Real
pages style these buttons (all of the code-hosting site's icon buttons
set their own background), so the case is rare, and a page whose icon
shows is far better off than one whose icon reads "Button".

## 7. How it is proved

- libflow host tests: the rule's cases, each a box-tree dump: text
  alone (replaced), text with a background (laid out), an SVG child,
  empty, `display: block` (fit-content), a fixed height (centred), the
  `style` attribute.
- yonder host tests: a laid-out button's widget is placed unpainted,
  a click reaches the page, the form submits.
- `tools/pages/buttons.html`, staged like the tier pages: measured
  checks with a pass count, and a row of buttons to look at and press
  (icons, a round GO, a toggle pair, a link-like one, the plain toolkit
  button, an unstyled icon button, and auxclick's button at a set
  height).
- On the P5: the code-hosting site's repo page shows no "Button"
  captions and its icon buttons draw their icons and click.
