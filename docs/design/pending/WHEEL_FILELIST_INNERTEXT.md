# WheelEvent, FileList and innerText

Three rows of DEBTS.md § Classic browser scripting: "There is no
`WheelEvent`", "There is no `FileList`" and "There is no `innerText`". Each
is a missing name that stops a page that only wanted to *mention* it, and
each has a cheap half that ends the wall and a fidelity half after it. Four
slices:

| Slice | What | Size | Pays |
|---|---|---|---|
| **A** | The `WheelEvent` interface; `FileList` and `input.files` | XS + XS | the `FileList` row; the interface half of the `WheelEvent` row |
| **B** | `innerText` and `outerText`, the first cut | S | the first-cut half of the `innerText` row |
| **C** | `wheel` dispatched from the mouse, with passive listeners | S | the rest of the `WheelEvent` row. **Not scheduled** (Chris, 2026-11-19): no page in the corpus needs it, so it stays a DEBTS row and § 4 is its design when one does |
| **D** | `innerText`'s full algorithm, from layout | M | the rest of the `innerText` row (designed after B lands, from B's WPT numbers) |

The order is walls first. A ends YouTube's app-wide `ReferenceError` and
the form library's sign-in `ReferenceError`; B ends the watch page's
description `TypeError`; C and D are fidelity.

**History, because it rides along.** `mousewheel` was Internet Explorer 6's
(2001), and its `wheelDelta` of 120 a notch is `WHEEL_DELTA` from the
Windows of the 1996 IntelliMouse, picked so that finer wheels could report
fractions of a notch. Firefox answered with `DOMMouseScroll`, and DOM Level
3 Events made peace with `wheel` and `WheelEvent` (Firefox 17 in 2012,
Chrome 31 in 2013). `innerText` was Internet Explorer 4's (1997). Mozilla
refused it for nineteen years and offered `textContent` (DOM Level 3, 2004)
instead, until so many pages depended on it that Robert O'Callahan wrote
the first real specification in 2016 and Firefox 45 shipped it. `FileList`
is from the File API that Firefox 3.6 brought in 2010, which was the first
time a page could read a file the person picked without uploading it.

---

## 1. Slice A: the WheelEvent interface

UI Events § 3.6. `WheelEvent` inherits `MouseEvent`, the way `PointerEvent`
already does in `libdom/event.c`:

```webidl
[Exposed=Window]
interface WheelEvent : MouseEvent {
  constructor(DOMString type, optional WheelEventInit eventInitDict = {});
  const unsigned long DOM_DELTA_PIXEL = 0x00;
  const unsigned long DOM_DELTA_LINE  = 0x01;
  const unsigned long DOM_DELTA_PAGE  = 0x02;
  readonly attribute double deltaX;
  readonly attribute double deltaY;
  readonly attribute double deltaZ;
  readonly attribute unsigned long deltaMode;
};
dictionary WheelEventInit : MouseEventInit {
  double deltaX = 0.0; double deltaY = 0.0; double deltaZ = 0.0;
  unsigned long deltaMode = 0;
};
```

- **A new UI kind, `D_UI_WHEEL`.** Its prototype's parent is
  `MouseEvent.prototype` and its interface's parent is `MouseEvent`. `event.c`
  wrote "inherits MouseEvent" as `== D_UI_POINTER` in several places (the
  parent choice in `d_ui_events_install`, `ui_implements`, which kinds read
  `EventModifierInit` and `MouseEventInit` in `ui_read_init`, activation
  and `relatedTarget`'s retargeting). Those became one predicate,
  `ui_is_mouse(kind)` (MouseEvent or an heir), and one answer for which
  interface a kind inherits, `ui_parent(kind)`, so a third heir of
  MouseEvent cannot miss one.
- **The record.** `DEvent` gains `delta_x`, `delta_y`, `delta_z` (doubles) and
  `delta_mode` (uint32). Its defaults are zero.
- **The dictionary.** It is read as WebIDL reads an inherited dictionary:
  UIEventInit, then EventModifierInit, then MouseEventInit, then its own
  members in lexicographic order (`deltaMode`, `deltaX`, `deltaY`,
  `deltaZ`). The deltas are restricted doubles, so `NaN` and `Infinity`
  throw `TypeError` (`ui_double`, which exists already). `deltaMode` is
  converted with `ToUint32`.
- **The constants** go on both the interface and the prototype, as
  KeyboardEvent's `DOM_KEY_LOCATION_*` do.
- **Not added:** `document.createEvent("WheelEvent")` (DOM's table has no
  such row, and other browsers throw too); the legacy `wheelDelta`,
  `wheelDeltaX` and `wheelDeltaY` attributes (Chrome and Safari have them,
  Firefox does not, and no page has needed them yet); `mousewheel` and
  `DOMMouseScroll`.
- `modern.c`'s UI events row adds `"WheelEvent"`, so the census stops
  reporting it missing.

After A, a page can name `WheelEvent`, construct one, and dispatch its own,
but yonder still dispatches none (C does that).

## 2. Slice A: FileList and input.files

File API § 5.2 and HTML § 4.10.5.1.18:

```webidl
[Exposed=(Window,Worker), Serializable]
interface FileList {
  getter File? item(unsigned long index);
  readonly attribute unsigned long length;
};
partial interface HTMLInputElement { attribute FileList? files; };
```

- **FileList lives in `blob.c`**, beside `File`. No file can be picked,
  so every list is empty and the class carries no record: `length` is 0,
  `item(i)` converts its index and returns null, there are no indexed
  properties to serve, and `[Symbol.iterator]` is `Array.prototype.values`
  (`d_iterable(..., false)`). `new FileList()` throws `TypeError`
  ("Illegal constructor"). The picker's slice gives a list its Files, and
  its indices the way a NodeList's are served (`collection.c`); building
  that record now would be code nothing could exercise.
- **`files`, the getter.** For an input whose type is File Upload it returns
  the element's FileList, and the SAME object on every read: the object is
  made at the first read, held in the element's `d_more` record and traced.
  For every other type it returns `null`. A field whose type changes away
  from `file` and back has its list again: the list belongs to the
  element, and the type decides only whether it is shown.
- **`files`, the setter.** `null` does nothing. A value that is not a
  FileList is a `TypeError` (WebIDL's conversion of `FileList?`). A FileList
  set on a File Upload input becomes that input's list, so the next read
  returns that same object (A checks this against a Chrome capture before
  building it). On any other type it does nothing. Today the only FileList a page can hold is another input's
  empty one, but this is the shape `input.files = dataTransfer.files` needs
  later.
- `modern.c`'s Blob row adds `"FileList"`.
- **Still out:** actually picking a file (yonder's file dialog and what fills
  the list), and `DataTransfer`. Both keep their DEBTS rows.

**Proof for A:** constructor, inheritance, constants and conversion tests
beside the existing UI event tests, with `instanceof MouseEvent` and
`instanceof UIEvent` checked. A `files` test covers type file and type text,
same-object identity, `null`, a non-FileList, `item` out of range, an
iteration that yields nothing, and `new FileList()`. Then the WPT
`uievents` and `FileAPI` directories are run again and their newly passing
pages are recorded. In QEMU, yonder loads a page with
`value instanceof FileList` and `new WheelEvent('wheel', {deltaY: 3})` in
a script.

---

## 3. Slice B: innerText, the first cut

HTML § 3.2.7. The getter returns the element's text **as rendered**, not as
written. The setter turns text into Text nodes and `<br>` elements.
`outerText` returns the same text, and its setter replaces the element
itself.

### 3.1 What the first cut answers

The first cut follows the specification's "rendered text collection
steps", taking each element's facts from its computed style. Only text
boxes come from layout, and those are slice D's.

| The specification's step | First cut |
|---|---|
| The element is not being rendered (not in a document with a window, `display: none` on it or an ancestor), or there is no CSS | its `textContent` |
| A child with `display: none`, or inside one | nothing |
| `visibility` not `visible` | its own text left out; its children walked, since they can be visible |
| A Text node | its data, with white space handled as its parent's `white-space` says (§ 3.3) |
| `<br>` | `"\n"` |
| `display: table-cell`, not the last cell of its row | `"\t"` after it |
| `display: table-row`, not the last row of its table | `"\n"` after it |
| `<p>` | a required line break count of 2 before and after it |
| Block-level `display` (block, flow-root, list-item, flex, grid, table) or `table-caption` | a required line break count of 1 before and after it |
| `display: contents` | no breaks of its own; its children walked |
| Assembly | empty items dropped, leading and trailing counts dropped, each run of counts replaced by `"\n"` repeated as many times as the run's largest count |

"Last cell" and "last row" need no look-ahead. A `"\t"` is written after
every cell, and when its row ends, a `"\t"` that is still the row's last
item is taken back. Rows inside their table are handled the same way with
`"\n"`. The work stays proportional to the size of the subtree, even for
a large table.

**The walk is the DOM tree, not the flat tree.** The design first said
the flat tree, from memory of how browsers build `innerText` from their
layout trees, and promised captures before building on it. No Chromium
was at hand, so WPT's getter (`getter-tests.js`, "Shadow DOM contents
ignored") was the check, and it says the opposite: a host's shadow root
contributes nothing, and the walk is the element's own children, as the
standard's steps are written. A host's light children that a slot takes
are rendered and read; those no slot takes are in no flat tree, layout
gives them no style, and the provider calls them not rendered.

**Left to slice D:** `text-transform` (D takes the text as layout shaped
it, with the Unicode case mapping layout already does); the
`select`/`option`/`optgroup` exception; replaced elements and form controls,
whose children layout does not render (B skips the children of `img`,
`video`, `audio`, `canvas`, `iframe`, `object`, `embed`, `input`,
`textarea` and `svg`, which is right for every one of them, but by a list
rather than by layout's answer); `<details>` closed; `content-visibility`.

### 3.2 The seam: four facts per element, not a computed style

The `getComputedStyle` provider is the wrong tool here. It serialises every
property in libgarb's table as text, hundreds of strings for each element,
and `innerText` needs four facts. A narrow provider beside it answers
those four:

```c
/* innerText's facts about one element (HTML § 3.2.7), read from the page's
 * current layout. rendered: HTML's "being rendered" -- styled, and neither
 * it nor an ancestor display: none (display: contents is rendered: its
 * children have boxes). */
typedef struct {
    bool rendered;
    bool visible;                       /* visibility: visible */
    uint8_t display;                    /* OS64_DOM_TEXT_INLINE, _BLOCK, _TABLE, _TABLE_PART
                                         * (row and column groups), _TABLE_ROW, _TABLE_CELL,
                                         * _TABLE_CAPTION, _CONTENTS */
    uint8_t white_space;                /* OS64_DOM_SPACE_COLLAPSE, _PRESERVE, _PRE_LINE */
} os64_dom_text_style_t;
typedef bool (*os64_dom_text_style_provider_t)(void *opaque, const os64_html_node_t *element,
                                               os64_dom_text_style_t *out);
void os64_dom_set_text_style(os64_dom_t *dom, os64_dom_text_style_provider_t provider, void *opaque);
```

yonder's provider is `script_text_style`, beside `script_computed`. It makes
the layout current (`layout_current`, a no-op once current) and reads pass
1's `flow_style_t` through `f_style_of`, so each answer is a lookup, not a
serialisation.

**A read forces a layout when the page has changed since the last one**
(Fable's review, finding 1). The four facts are style facts and use no box
position, so in principle only pass 1 (styles) needs to be current.
libflow cannot give that today: pass 1 runs only inside `flow_layout*`
(`f_style_build` is internal), and its results live in the tree it
builds. A styles-only door means a public `f_style_build_reusing`, an
`FStyles` that yonder keeps beside the tree and invalidates as the page is
edited, and a provider that prefers it. That is a perf slice of its own,
and B does not include it: it is booked under DEBTS.md § Browser layout
("A read of `innerText` lays the page out when only its styles are
needed"), with the measurement it waits for. That number has to come from
the P5, since a QEMU timing under TCG measures the emulator. A page that
reads `innerText` once (YouTube's description does) pays at most one
layout, which `getComputedStyle` costs too. An element pass 1 never styled is
inside `display: none`, or is a child that layout does not render, so it
is not rendered. B confirms that in libflow before relying on it. A
provider that answers false (no current layout could be made) makes the
getter return `textContent`, because a getter that throws is worse than a
plain answer.

**Where no provider is installed** (the host replay, a document with no
window), every element is "not being rendered" and the getter returns
`textContent`. That is what the specification says a browser without CSS
returns.

### 3.3 White space

The walker keeps one piece of state: whether the last thing it wrote was a
collapsible space. Whitespace-only text inside a `TABLE`, `TABLE_PART` or
`TABLE_ROW` gives nothing, since CSS drops it there (CSS 2.1 § 17.2.1), so
it can never sit between a cell's `"\t"` and the row's end.

- **`COLLAPSE`** (`normal`, `nowrap`): each run of space, tab, LF and CR
  becomes one space, and a space that follows a pending collapsible space
  is dropped. This also applies across element boundaries, so
  `a <b> b</b>` gives `a b`.
- **`PRE_LINE`:** spaces and tabs collapse as above. Each LF stays a line
  break, and the spaces on either side of it are removed.
- **`PRESERVE`** (`pre`, `pre-wrap`, `break-spaces`): the text is copied
  as written.
- **Line edges:** a collapsible space just after a line break (a required
  count, a `<br>` or a preserved LF) or at the start, or just before one
  or at the end, is removed, which is how layout removes spaces at the
  ends of a line.

Soft wraps change nothing. Chrome's answer does not depend on where a line
happens to wrap, and the first cut matches it.

### 3.4 The setter and outerText

These do not depend on rendering, so B builds them fully.

- **`innerText = value`:** the value is split at CRLF, CR and LF. Each
  piece that is not empty becomes a Text node, and each break becomes a
  `<br>` in the element's node document. The children are then replaced
  with that fragment in one "replace all", so a MutationObserver sees the
  records `textContent`'s setter makes. Those are several records where
  DOM queues one, which is an older row (DEBTS.md: "A replace all is
  several mutation records, not one") that now names `innerText` too.
- **`outerText`, the getter** returns what `innerText` returns.
- **`outerText = value`:** with no parent it throws
  `NoModificationAllowedError`. Otherwise it builds the same fragment, adds
  an empty Text node if the fragment is empty, and replaces the element
  with it. Then the Text nodes at either seam are merged with their
  neighbours (the specification's "merge with the next text node", run for
  the next sibling and then for the previous one).
- **Where they live:** on `HTMLElement.prototype`, in both modes, beside
  `textContent`. The setters replace children through the same
  replace-all path `innerHTML`'s setter takes in `content.c`, so custom
  element reactions and mutation records come from code that already
  exists.

### 3.5 Shape and proof

- **The walk does not recurse.** It steps through the tree by its
  `first_child`, `next` and `parent` links, closing each element (its
  trailing breaks, its taken-back tab) as it climbs out, so its memory is
  the same whatever the depth (Fable's review, finding 4). It leans on no
  cap: the DOM verbs hold a document to twice its parse's `max_depth`
  (512 by default, so 1,024 levels), but `max_depth` is a parse option, and
  a walk that needed it would break the day a caller raised it. A host case
  walks a 50,000-level tree in a fifth of a second under ASan.
- **`libdom/inner_text.c` is pure C** over `os64_html_node_t`, a facts
  callback and an output buffer, with no QuickJS. `tools/test_inner_text_host.sh`
  drives it under ASan with libhtml parsing the input and a fake provider
  that reads each element's facts from an attribute. This is where every
  row of § 3.1 and every rule of § 3.3 gets its own case. Two more come
  from the review (finding 5). `<script>` and `<style>` inside the element
  give nothing, which is the UA sheet's `display: none`; in yonder that is
  checked in QEMU, because the host suite's fake provider has no UA sheet.
  And `innerText` read ON a `<script>` gives its source, because the
  element is not rendered, so its `textContent` is the answer.
- **WPT.** `html/dom/elements/the-innertext-and-outertext-properties/` is
  excluded today ("not P1-W's oracle"). B runs it. `getter.html` counts the first cut's passes, and D is judged by how many
  more it passes. `innertext-setter.html`, `outertext-setter.html` and the
  `innertext-with-white-spaces` variants are expected to pass in B.
- **QEMU:** a page with a hidden span, a `<br>`, two paragraphs, a table, a
  `<script>` and a `<style>` shows `innerText` next to `textContent`.
- **The host replay installs no provider**, so in a replay `innerText` is
  `textContent` (Fable's review, finding 2). A replay of the YouTube watch
  page proves only that the `TypeError` has gone, not that hidden text is
  left out. The replay's known differences in DEBTS.md (§ The replay
  corpus) gain that row. The real check of the description is a P5 run,
  which is Chris's.
- **libdom's own suite** (`tools/test_dom_inner_text.inc`, run alone with
  `tools/test_dom_host.sh --inner-text`) proves the door with a fake
  provider: `textContent` without one, the provider's answers with one, a
  provider that refuses, a detached element, a read on a `<script>`, and
  both setters (line breaks, `null`, markup kept as text, `outerText`'s
  merges and its empty Text node, `NoModificationAllowedError`).

---

## 4. Slice C: wheel, dispatched

*Not scheduled: no page in the corpus has failed for want of a dispatched
`wheel` (Fable's review, Scope), so Chris left it in DEBTS.md. This section
is its design for the day a page needs it.*

UI Events § 3.6.2. A wheel over the page fires `wheel` at the element under
the pointer. The event bubbles, is composed and is cancelable, and if
nothing cancels it, yonder scrolls. Pages that scroll themselves (maps,
carousels, scroll-jacking landing pages) listen for this and call
`preventDefault`.

### 4.1 Passive listeners come with it, or scrolling breaks

libdom reads only `capture` and `once` from `addEventListener`'s options.
That means every `preventDefault` counts. **Other browsers do not let a
wheel listener on the window, the document, `<html>` or `<body>` cancel
scrolling.** DOM § 2.7's "default passive value" makes those listeners
passive unless the page says `passive: false`, and a passive listener's
`preventDefault` is ignored. Many sites attach such a listener and call
`preventDefault` without thinking, and in Chrome nothing happens. If yonder
honoured it, wheel scrolling would stop working on those sites. So C
builds the following:

- **`passive` in the options**, read in WebIDL's order (`capture`, `once`,
  `passive`, `signal`). An absent `passive` takes the default passive value
  for its type and target, computed when the listener is added: true for
  `wheel`, `mousewheel`, `touchstart` and `touchmove` on the Window, a
  Document, a document element or a body element, and false otherwise.
  It is not part of the listener's identity, so `removeEventListener`
  ignores it, as DOM says.
- **The in-passive-listener flag** on the event while a passive listener
  runs. `preventDefault`, `returnValue = false` and a handler that returns
  false all do nothing while it is set.
- **`onwheel`** on HTMLElement, Document and Window, registered through the
  event handler machinery, so the default passive value applies to it as
  well. Libraries choose an API with `'onwheel' in div`, and before C,
  answering yes would have sent them to an event nobody fires.

Feature detection follows automatically: libraries test for passive support
with a getter on an options object, and from C on, libdom reads that
property.

### 4.2 The host's half

- **The ABI:** `OS64_DOM_EVENT_WHEEL`, with `os64_dom_event_t` gaining
  `delta_x`, `delta_y`, `delta_z` and `delta_mode`. A wheel event also
  reads the mouse fields.
- **The deltas:** `deltaMode` is `DOM_DELTA_PIXEL`, and each delta is the
  number of CSS pixels yonder is about to scroll: notches × 3 × `line_step`
  ÷ zoom, which is 60 a notch at a 16 px font. A page that cancels the
  event and scrolls by `deltaY` itself then scrolls at yonder's speed.
  Browsers and platforms already disagree about a notch's size (Chrome
  sends 100 on Windows), so pages cope with different numbers.
- **The axis follows what yonder scrolls** (Fable's review, finding 6).
  yonder scrolls sideways only for a tilt, which the packet reports as
  `dx` (`view_event`: "a tilt goes sideways"). Shift+wheel is not turned
  sideways. So `deltaX` comes from `dx` and `deltaY` from `dy`, and each is
  zero when its axis did not move. If yonder ever turns Shift+wheel
  sideways, the event's axis moves with the scroll, because both come
  from one computation.
- **The modifiers and buttons** come from the wheel packet (`ev->mouse`).

### 4.3 An input's lifecycle

The wheel joins yonder's input queue (`Input`, `inputs_run`) as `IN_WHEEL`,
with the node held and the deltas and modifiers carried on the input.

| State | Enters | Leaves |
|---|---|---|
| (scroll at once) | a notch over the view, with scripts off, with no `wheel` listener in the page (`yonder_scripts_listens`), or with the queue full | the same turn: today's `wheel_box` and then `scroll_to`, and no event |
| (scroll, then tell) | a notch over the view while every `wheel` listener in the page is passive (Fable's review, finding 3: none of them can cancel) | the same turn scrolls; a `wheel` event queued with the deltas it scrolled by, dispatched by `inputs_run`, its cancellation meaningless (all its listeners are passive) |
| queued | a notch over the view while the page listens | **coalesced**: another notch for the same node arrives before dispatch, and its deltas are added to this input (as `IN_MOVE` coalesces); **dropped**: the page is replaced (`inputs_drop`), and there is no scroll, because the page that would have scrolled is gone; **dispatched**: `inputs_run` reaches it |
| dispatched | `page_event` with `wheel` at the node | **cancelled**: done, no scroll; **not cancelled**: `wheel_box` at the input's point and then `scroll_to`, both with the event's deltas |

The third reason for scrolling at once matters. A full queue drops an
input today, and a dropped wheel input would lose a scroll the person
asked for, which is worse than losing the event.

**Not dispatched**, as now:

- **Ctrl+wheel** stays yonder's zoom and is claimed before the view sees
  it. Other browsers send it to the page first, and maps use it to zoom
  themselves. That becomes its own DEBTS row.
- **A wheel over yonder's own widgets** (a player's volume, a list
  control) is the widget's, as before.

**Latency.** The scroll waits for the page's wheel listeners only when one
of them is non-passive, which is the only case where waiting changes the
outcome. Telling the two apart is one more answer from the scripts host:
whether any non-passive `wheel` listener is registered in the page. libdom
counts those as listeners are added and removed. The finer test (whether
a non-passive listener is on THIS event's path, not just anywhere in the
page) is a DEBTS row: a page with one cancelling listener on a carousel
then waits on every wheel over the rest of the page too.

### 4.4 Proof

- libdom tests: the default passive value on each kind of target,
  `passive: false` winning over it, `preventDefault` ignored in a passive
  listener and honoured in a non-passive one, `onwheel` passive, and the
  options read in order (a getter log).
- WPT: `dom/events` (passive pages such as `AddEventListenerOptions-passive`
  and `passive-by-default`) and `uievents` are run again and their newly
  passing pages are recorded.
- A QEMU page that logs `wheel` and cancels it inside a box: the box does
  not scroll and the page still does. A document-level cancelling listener
  that the page does not mark `passive: false` does not stop the page
  scrolling. Driven through QMP's `input-send-event` wheel buttons.
- The P5: Chris scrolls a page with a non-passive wheel listener and says
  whether it feels the same. A page with only passive listeners scrolls
  as it does today by construction.

---

## 5. Slice D: innerText from layout (outline only)

D is designed after B lands, from B's `getter.html` numbers and from what
yonder's layout keeps. The direction: layout already knows each text
node's text after white-space processing and `text-transform`, and the
table structure of its boxes. A second provider, per Text node, answers
"the text your boxes show" (D's new seam), and B's walker uses that
answer in place of § 3.3's approximation when a provider gives one. The
assembly, the counts and the facts provider stay as they are. B's walker
is the fallback, not throwaway code. D also takes the
`select`/`option` exception, replaced elements by layout's answer instead
of a list, `<details>`, and `content-visibility`.

---

## 6. Review

A is two interfaces with no new seam: reviewed here, by one cold Opus
reader. B adds a seam between libdom and yonder, and C changes what
`preventDefault` means for every listener in libdom: each gets a Codex
round.
