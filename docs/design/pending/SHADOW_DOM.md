# SHADOW_DOM.md — shadow trees, slots and scoped style (GITHUB_WALLS Slice D)

*Opus, 2026-11-05. Reviewed by Fable the same day (SHADOW_DOM_REVIEW.md,
untracked: build toward it, ten findings, all folded in below); Chris
ruled § 12's three questions with Fable's leans. GITHUB_WALLS.md § 4
asked two things of this doc: answer the cut question with corpus numbers
before anyone writes `attachShadow`, and keep ElementInternals out of
Shadow DOM's way. § 1 and § 2 answer the first, § 9 the second.*

## 1. What pages ask for

The census (`~/os64-opus-kit/tools/shadow_census.py`, outside the tree)
read three corpus pulls as text: 193 hosts, 3,104 distinct kept scripts,
their page HTML and yonder's records. A host counts when any of its kept
scripts names the feature.

| Feature | Scripts | Hosts | Of the 53 `attachShadow` hosts |
|---|---|---|---|
| `attachShadow(` | 77 | 53 | — |
| `{mode: "closed"}` | 9 | 4 | 4 |
| `.shadowRoot` read | 188 | 101 | |
| `getRootNode(` | 127 | 89 | |
| `:host` in a sheet | 35 | 25 | 21 |
| `adoptedStyleSheets` | 26 | 19 | 16 |
| `new CSSStyleSheet` | 18 | 17 | 13 |
| `.assignedNodes(` / `.assignedElements(` | 21 | 15 | 9 |
| `::part(` / `part=` | 12 | 9 | 7 |
| a `<slot>` written | 8 | 6 | 6 |
| `::slotted(` | 7 | 5 | 3 |
| `delegatesFocus` | 4 | 3 | 3 |
| `slotchange` | 4 | 3 | 2 |
| `slotAssignment` (manual slots) | 0 | 0 | 0 |
| `attachInternals(` called (these three pulls; § 9 reads the whole corpus) | 0 | 0 | 0 |
| a declarative shadow root in the HTML | — | 1 | |

Libraries among the 53: Lit on 5, a Shady DOM polyfill on 3, Stencil,
FAST and GitHub's Catalyst on one each; 46 hosts write their own
elements or bring something the census does not recognise.

What the records say dies today: `MISSING global ShadowRoot` on 96
records across 10 hosts (an `instanceof ShadowRoot` that throws, mostly
inside a guard); `MISSING global CSSStyleSheet` on 4 hosts; one polyfill
script failing on `ShadowRoot` outright; and one site whose header and
footer are custom elements that call `attachShadow` in their
constructors, so the site draws with neither. GitHub's case is
GITHUB_WALLS § 4's: its invoker polyfill saves `attachShadow` and
`attachInternals` off `HTMLElement.prototype` and calls the saved one,
so `relative-time` and whatever its bundle defines after the throw never
construct. Most of the 53 hosts survive today because their scripts test
for the API first. That is a reason the work is safe to land, not a
reason it is unneeded: a test that fails takes the page's fallback, and
the fallback is what the person sees.

**The census is a floor.** The pulls it read kept classic scripts only;
module chunks have been kept since 2026-11-04 (#466), so a site whose
components ship as modules counts for nothing above. The first record
with its chunks kept (the P5's, 2026-11-05) is a social news site's
community page built entirely of Lit elements. It dies at Lit's
`createRenderRoot` calling `attachShadow`, so its feed element never
defines and never asks for more posts. Its four chunks carry about 300
`<slot`, 440 `:host`, 57 `::part`, 46 `slotchange` and 57
`assignedElements`/`assignedNodes`; its server markup puts `slot=` on
every part of every post, and the "more posts" loader sits in a slot.
§ 10's counts are re-run after the next pull, before anything there is
booked as waiting.

## 2. The cut question: a minimal ShadowRoot is a trap

GITHUB_WALLS § 4 proposed a first cut where `attachShadow` returns a
ShadowRoot whose children lay out and paint as the element's own, with no
slots and no style scoping, and asked whether that is honest or a trap.
The census says trap, for three reasons, each a page drawn wrong rather
than a page missing something:

1. **Style leaks out.** A component's sheet is written for its own tree:
   `button { all: unset }`, `div { display: grid }`, `* { box-sizing:
   border-box }`. Unscoped, those rules restyle the whole document the
   moment the first component connects. 21 of the 53 hosts write `:host`,
   which only means anything if sheets are scoped, and 16 adopt
   constructed sheets, which are scoped by construction.
2. **Light children land in the wrong place.** A host's own children
   render only through a slot. `<x-card><h2>Title</h2></x-card>` whose
   shadow tree is `<div class=frame><slot></slot></div>` draws the title
   inside the frame. With no slots the cut must either drop the title or
   draw it beside the frame, and the census's slot users (`<slot>`,
   `assignedNodes`, `::slotted` on 6, 9 and 3 of the 53) are exactly the
   elements that wrap content.
3. **Page rules leak in.** The other direction: the document's
   `.title { font-size: 3em }` reaching a component's `.title`. Less
   loud, as common.

Each of these is a regression on a page that works today, because today
those pages feature-test, take their fallback, and draw. So the cut is the
whole MODEL (trees, the flat tree through named slots, scoped style,
retargeted events) with the SURFACE trimmed by the census: manual slot
assignment (0 hosts), declarative shadow roots (1), `::part` (7), and
`delegatesFocus` (3) wait for their own PRs or their own rows (§ 10).
That is still most of the work, and it is the honest size of it.

## 3. The tree: a shadow root is a fragment the host owns

libhtml already holds the shape this needs. A `template`'s contents are a
fragment the element owns through `template_contents` and that is no
child of it (LIBDOM.md § Objects and collections); every walk of
children skips it because it is not on the child list. A shadow root is
the same arrangement with three differences:

- **The node.** An `OS64_HTML_FRAGMENT`, as DOM has it ("a
  DocumentFragment whose host is non-null"), so every place that treats a
  fragment as a fragment (an insertion moves its children, a query takes
  it as a receiver) treats a root the same with no change. The element
  points at it through a new `shadow_root` field; the root keeps its host
  in the fragment's private word, where a template's contents keep their
  template, and `os64_html_shadow_host` tells the two apart by which of
  the owner's pointers names the fragment. Its options (mode,
  `delegatesFocus`, `clonable`, `serializable`) are the bits of a new
  `shadow` byte, which sits in padding the node already had. The root
  lives in the host's document arena and counts against its budget like
  any node.
- **One verb.** `os64_html_attach_shadow(doc, host, options)` checks what
  DOM's "attach a shadow root" algorithm checks (the host is an HTML
  element on the standard's list or has a valid custom element name; it
  has no root already; its custom element definition, if it has one, does
  not list `shadow` in `disabledFeatures`), makes the root, and records it in the change log as a CHILDREN change of the host
  with no nodes added. Nothing removes a shadow root; it lives as long as
  its host.
- **Connected means the shadow-including root is the document.**
  `os64_html_shadow_including_root(node)` climbs `parent`, and from a
  shadow root to its host, until there is neither. Every check of "is
  this node in the document" moves to it. Today those checks climb by
  hand: libdom's `d_connected` and `d_in_window_tree` (window.c), the
  cascade's `connected` (cascade.c), about thirty-five climbs across
  libdom, libpage's state, libhtml's tree and yonder's geometry. Each one
  either moves to the shadow-including root or says why its question
  stops at the tree's own root (a form owner, a label, an ID lookup all
  stop at the tree's root, per the standard).
- **And every descent that means "everything under here" decides too.**
  The climbs are half of the shadow-including tree; the other half is
  every walk DOWN, and a shadow root is on no child list, so today's
  descents never enter one. In libdom they run on `d_walk_next`: an
  insertion's walk that queues `connectedCallback` and tries upgrades
  (custom.c), `customElements.define`'s walk of the document (a lazily
  defined element whose instances already sit inside other components'
  shadow trees must upgrade), `d_custom_cloned`, and the removal walk that
  queues `disconnectedCallback`. Outside libdom: whatever enumerates a
  page — pictures and frames to fetch, scripts to run, the sheet list
  (§ 6), form controls, Tab order, find, selection. Some descents must
  NOT cross: a mutation observer's `subtree`, `querySelector`,
  `contains`, `getElementsBy*`, live collections. libhtml offers the
  crossing descent once, in the standard's shadow-including tree order
  (a host's shadow root before its children:
  `os64_html_shadow_including_next`), so nobody writes it by hand.

**D2's first commit is two lists, climbs and descents** (Appendix A),
every entry ruled "crosses" or "stops, because …". It is the reviewer's
checklist for D2 and the commit Fable reads first.

Mutations inside a shadow tree are ordinary verbs on ordinary nodes, so
they enter the change log, mutation observers and the bounded readers
(cascade updates, layout reuse) exactly as light-tree mutations do, with
a target whose shadow-including root is the document. A reader that
decides "is this change in the document" by climbing to the document is
on the climbs list.

Clone: `cloneNode` copies a shadow root only when it is `clonable` (DOM's
"clone a node"), which is off unless asked for. Adopt moves the host's
root with it; `importNode` is a clone and follows `clonable`. Serialization: a host's `innerHTML` and `outerHTML` never
include its shadow tree; a ShadowRoot's own `innerHTML` reads and writes
its children, through the fragment parser with the host as context.
`getHTML({serializableShadowRoots})` waits for its own row.

## 4. Slots and the flat tree

**Assignment (DOM's "find a slot" and "find slottables", named mode).** A host's slottable children
(elements and text) each find the first `slot` in their host's shadow
tree, in tree order, whose `name` equals their `slot` attribute (the
default slot for none). A slot with nothing assigned shows its own
children, its fallback content. Assignment is a pure function of the two
trees, so it lives in libhtml beside the tree, computed per shadow root
and cached against the document's version: `os64_html_assigned_slot(node)`,
`os64_html_slot_assigned(slot, index)` and a count. Recomputing a root is
one walk of its tree for slots and one of the host's children.

**The flat tree** is what renders and what inherits: a host's flat
children are its shadow root's children; a slot's flat children are its
assigned nodes, or its fallback content when none; every other node's
are its own. libhtml offers it as a walk, `os64_html_flat_first_child`,
`os64_html_flat_next_sibling` and `os64_html_flat_parent`, so that no
caller writes the slot rule itself. A slot is `display: contents` by
HTML's UA sheet, so it makes no box of its own.

**`slotchange`** fires at a slot whose assigned nodes changed, once per
microtask checkpoint per slot, the way the standard's signal-slots list
queues it. It cannot ride the lazy cache: a recomputation happens when
somebody asks, and a page that changes a host's children and reads
nothing before the checkpoint must still hear it. So the slot-affecting
records § 5 lists mark their shadow root dirty AT THE MUTATION; the
microtask checkpoint recomputes every dirty root before it notifies
mutation observers, and signals each slot whose assigned list moved,
a slot's first assignment included. A read of assignment between the
mutation and the checkpoint recomputes too and keeps the before-list
for the checkpoint's comparison, so a read neither fires nor swallows
the event.

## 5. Rendering: box building and inheritance walk the flat tree

libflow builds boxes and computes inherited style by walking element
children (`boxes.c`, `style.c`'s element walk, the reuse snapshot). Those
walks move to the flat-tree functions. Selector matching does not: a
selector matches within one tree (§ 6), so libgarb keeps walking the
real tree for combinators and only the scoping rules cross.

What dirties layout grows by the slot rule. A change that can move an
assignment — a child added to or removed from a host, a `slot` attribute
set, a slot's `name` set, a slot inserted into or removed from a shadow
tree — changes the host's flat children even though the host's own child
list may not have changed. The cascade's and layout's change readers map
each such record to the host whose flat children it touches. That is
RELAYOUT.md's machinery (Quinn's), and D3 is reviewed against it; the
first version may answer "rebuild the host's subtree" for every
slot-affecting record and leave finer reuse to a measured later step.

Hit testing returns the innermost flat-tree node under the point, which
may be inside a shadow tree; § 8 says what a script outside sees.

## 6. Scoped style

**Where a sheet applies.** A sheet applies in one or more SCOPES: the
document, or a shadow root. yonder's sheet list (yonder.c's `Sheet`) is
collected from the document's `style` and `link` elements in tree order;
it becomes the shadow-including tree order, and a sheet found inside a
shadow tree applies in that root. A shadow root's `adoptedStyleSheets`
follow its own sheets; `document.adoptedStyleSheets` follow the
document's (CSSOM's "final CSS style sheets").

**One read per sheet, however many scopes adopt it.** Lit shares one
constructed sheet across every instance of a component (a `CSSResult`'s
`styleSheet`), and the record in § 1 builds ONE sheet of its whole
utility CSS and adopts it on the document and in every component root.
The cascade copies what it reads of each input sheet into its read arena
under a cap (cascade.h, `GARB_CASCADE_READ_MAX`), so one
`garb_sheet_in_t` per (sheet, scope) would read that sheet once per root
and come up short on exactly the pages this slice is for. So the shape
is: `garb_sheet_in_t` stays one per distinct sheet, read once and filed
once into the per-target index (`c->index[target]`, buckets keyed by the
rightmost compound); the cascade's input gains a list of scopes, each a
root node and the indices of the sheets it applies, in order; and a
bucket's entries are filtered at match time by whether the entry's sheet
applies in the element's tree, a bit per (scope, sheet) the cascade
builds once. A sheet adopted by a hundred roots costs one read and a
hundred bits.

**UA rules and presentational hints apply in every tree.** The UA origin
(`slot { display: contents }` among it) and an element's presentational
hints are not author sheets with a scope; they match every element
wherever it lives, as they do today, so they belong to no scope's list
and the filter above never applies to them.

**What a scoped rule matches** (CSS Scoping 1 § 3):
- an element in the scope's own tree, matched within that tree: a
  combinator never crosses a shadow boundary, so `.a .b` in a component
  cannot reach out to the page's `.a`;
- the scope's host, but only through `:host` and `:host(<compound>)`, the
  host being featureless otherwise (`:host-context()` is one engine's and
  no host in § 1 uses it, so it is left out);
- an element slotted into one of the scope's slots, but only through
  `::slotted(<compound>)`.

Rules whose subject is `:host` and rules ending in `::slotted()` are
filed in two more index targets beside the element's and the
pseudo-elements', consulted when matching a host (for the one root it
hosts) and a slotted element. `::slotted()` matches after flattening (CSS
Scoping): an element passed on through a forwarded slot (`<slot name=a
slot=b>`, common in nested components) is assigned to every slot on that
chain, so each tree along the chain consults its `::slotted()` rules for
it, innermost slot last.

**Tree-scoped names are global in this cut.** `@keyframes` and
`@font-face` names written in a shadow sheet should resolve in that tree
first (CSS Scoping § 3.5); here they join the document's one namespace,
the last definition winning as it does today. A DEBTS row with the slice.

**Who wins** (CSS Cascade 5 § 6.1, context): between declarations from
different trees, a normal declaration from the outer tree beats one from
the inner (the page can restyle a component's host), and an important
declaration from the inner tree beats the outer (a component can insist).
This sits above specificity, which only compares within one context.

**Inheritance follows the flat tree**: a slotted element inherits from its
slot, which inherits from its own flat parent, so a component's `color`
reaches the text it wraps.

**Constructed sheets.** `new CSSStyleSheet()`, `replaceSync`, `replace`
(a Promise) and the CSSOM members libdom's sheets already have (`cssRules`,
`insertRule`, `deleteRule`). A constructed sheet has no owner node and no
`@import` (CSSOM: `replace` and `replaceSync` drop them).
`adoptedStyleSheets` is an observable array: assigning or editing it
reorders the scope's sheets and requests a cascade.

libgarb's part: the scopes list and its filter, `:host`, `:host()` and
`::slotted()` in the selector parser and their two index targets, and the
context step in the cascade's comparison. The cascade
already walks the document to match; it walks every connected shadow tree
too, which is the cascade's share of the flat-tree change.

## 7. The script surface

Element: `attachShadow({mode, delegatesFocus, clonable, serializable})`
(`slotAssignment: "manual"` throws NotSupportedError until its row is
paid), `shadowRoot` (open roots only), `slot`, `assignedSlot`.
Text: `assignedSlot`. Node: `getRootNode({composed})`, `isConnected`
over the shadow-including root.

ShadowRoot, a DocumentFragment: `host`, `mode`, `delegatesFocus`,
`clonable`, `serializable`, `slotAssignment` (always `"named"`),
`innerHTML`, `activeElement`, `styleSheets`, `adoptedStyleSheets`,
`getElementById`, `querySelector`, `querySelectorAll`, `elementFromPoint`
and `elementsFromPoint` retargeted, and `onslotchange`. The `ShadowRoot`
interface on window, which is what the 96 `MISSING global ShadowRoot`
records ask for.

HTMLSlotElement: `name`, `assignedNodes({flatten})`, `assignedElements`.
`assign()` throws until manual slots are built.

CSSStyleSheet's constructor and the members above; Document's
`adoptedStyleSheets`.

**One switch covers every name a page can test.** Lit decides once, at
load: `window.ShadowRoot && ('adoptedStyleSheets' in Document.prototype)
&& ('replace' in CSSStyleSheet.prototype)`, and the 96 `MISSING global
ShadowRoot` records are guards that flip the day the global exists. So
the names come and go together, behind one libdom switch, off until D4
turns it on: `attachShadow`, `shadowRoot`, `slot`, `assignedSlot` (on
Element and Text), `getRootNode`'s `composed` member, `ShadowRoot`,
`HTMLSlotElement`, `CSSStyleSheet`'s constructor, both
`adoptedStyleSheets`, and `Event.composed`. A `yonder.conf` developer key
sets it before D4 does, so D3 can load real pages in QEMU, and the host
suites set it for their own cases. All of it is also behind the Modern
Web switch, as every surface since Slice 1 is.

## 8. Events

`composedPath()` already returns the dispatch's path (event.c's
`composed_path`, whose comment says shadow roots must change it). Shadow
trees bring the parts of DOM's dispatch algorithm that were vacuous
without them:

- **A slotted node's parent in the path is its slot.** DOM's "get the
  parent" sends a node assigned to a slot to that slot, so an event on a
  light child passes through the slot and the shadow tree around it, then
  the root, then the host. Components listen on their own shadow content
  for clicks on what they wrap; without this a click on slotted content
  is never heard inside the component. Its own host-suite case.
- **The path crosses a shadow root to its host** only when the event is
  `composed`. `Event.composed` and the `composed` init member arrive with
  this slice; UA-dispatched UI events (pointer, mouse, keyboard, focus,
  input) are composed, as the UI Events standard says.
- **Retargeting.** A listener outside a shadow tree sees `target` (and
  `relatedTarget`) as the host, never the node inside. The path holds
  each step's retargeted target, computed once per dispatch.
- **Closed trees hide.** `composedPath()` leaves out nodes inside a closed
  shadow tree the current listener is outside of.
- **Focus.** `document.activeElement` is retargeted to the host;
  `shadowRoot.activeElement` names the focused element inside.
  `delegatesFocus` waits for its own PR (§ 10).

The pointer dispatch (pointer.c) and the click synthesis take their
target from hit testing, which now returns flat-tree nodes, and retarget
like any other dispatch.

## 9. ElementInternals (D1): measured, and what the measurement leaves

Fable's finding 5: an `attachInternals` that appears flips feature tests
as `attachShadow` does, and a page that leaves its hidden-input fallback
for `setFormValue` loses the field from its submission, silently, while
form association is unbuilt. Chris ruled: if carrying `setFormValue` into
libpage's form data set is an S, D1 carries it and lands first; if more,
D1 lands with D4.

**Measured: M.** libpage models a control as one of four element kinds
chosen by tag (`os64_page_element_t`: input, button, select, textarea).
A form-associated custom element needs a fifth kind, which libpage, below
libdom, can only learn from libdom's definitions; its submission value
stored in libpage's state and turned into entries (`submit.c`'s entry
list, a FormData value being several); its form owner, which libdom's
`window.c` also decides by tag (`form_associated`); `form.elements`; and
the disabled and reset callbacks. Four layers. So by the ruling D1 lands
with D4.

**What the measurement leaves, for Chris at D4.** Re-reading the corpus
for every `attachInternals(` call: three sites, none GitHub's. GitHub's
polyfill installs its wrapper only when the method exists (`r &&
Object.defineProperties(...)`) or calls the saved one only when the page
calls `attachInternals`, so GitHub needs `attachShadow` alone; Fable's
reading agrees (`relative-time` calls `attachShadow`). The three sites
are an ARIA observer behind `typeof ElementInternals`, a framework's own
`attachInternals` method, and a lazy getter. So nothing in the corpus is
waiting on a bare ElementInternals, and the one guarded caller would
leave a working path for it. My lean for D4: leave ElementInternals out
of the slice and build it with form association, as its own row says,
rather than land it bare. If it does land with D4, it is what follows,
and a `formAssociated` definition's `setFormValue` throws
NotSupportedError naming form association, so the loss is loud.

- `HTMLElement.prototype.attachInternals()`: the standard's checks (a
  custom element, or one being defined, whose definition does not
  disable internals, and not already attached), returning an
  ElementInternals bound to the element;
- `ElementInternals.prototype.shadowRoot`: the host's root when its
  `available to element internals` flag is set (an `attachShadow` made in
  the constructor), else null;
- `form`, `labels`, `willValidate` and the rest of the form members
  answer for a non-form-associated element (null, empty, false) and
  `setFormValue`/`setValidity` throw NotSupportedError for an element
  whose definition is not `formAssociated`, as the standard says, so a
  page feature-testing them learns the truth.

Form association itself stays DEBTS' existing ElementInternals row (M,
measured above).

## 10. What waits, and where it is booked

The host counts are § 1's floor (classic scripts only); each is re-run
after the next corpus pull, which keeps module chunks, before its row is
booked. The record in § 1 alone carries about 57 `::part`.

| Waits | Hosts | Where |
|---|---|---|
| `::part` and `exportparts` | 7 | a DEBTS row with this slice; a selector and a cascade step on top of § 6 |
| Tree-scoped `@keyframes` and `@font-face` names | — | a DEBTS row with this slice (§ 6) |
| `:host-context()` | 0 | not booked: one engine's |
| `delegatesFocus` | 3 | DEBTS; needs the focus navigation yonder has (Tab order) to learn shadow trees |
| Tab order by the flat tree | — | a DEBTS row with this slice (A.2's `collect_node` row): the first cut tabs in shadow-including order |
| Declarative shadow roots (`<template shadowrootmode>`) | 1 | DEBTS' template row (line "A template has none of declarative shadow DOM's members"); a libhtml parser step |
| Manual slot assignment | 0 | DEBTS |
| `getHTML`, `serializable` serialization | — | DEBTS |
| Form-associated custom elements | 0 | DEBTS' ElementInternals row |

## 11. Proof

- **WPT.** `shadow-dom/` and `css/css-scoping/` join `tools/wpt_fetch.sh`'s
  sparse list; the testharness tests run under the yonder scripted-page
  host with expectations per directory, as the DOM directories do. The
  reftests in `css/css-scoping/` are the rendering proof, through the
  same reftest path the CSS directories use.
- **Host suites.** libhtml's tree host test gains the root, the verb, the
  shadow-including root and assignment; libdom's host suite gains the
  surface and the event path; libgarb's gains scoped matching and the
  context step. Each with allocation sweeps, as every native verb has.
- **The census again**, after D4, against the same pulls: the `MISSING
  global ShadowRoot` and `CSSStyleSheet` lines and the `attachShadow`
  failures should be gone, and any new failure on those 53 hosts is a
  finding.
- **On the machine.** GitHub's pull-request and repository pages in
  QEMU, then on the P5: `relative-time` writes "3 hours ago"; the site
  whose header and footer were missing draws them; and § 1's community
  page draws its feed and loads more posts, which exercises slots,
  `slotchange` and the shared sheet together, as `relative-time` does
  not.

## 12. Slices, and what Chris rules

Four PRs, stacked, merged in order, the switch (§ 7) off until the last:

- **D2a The tree, in libhtml**: its first commit is Appendix A's two
  lists; then the node, the verb, the shadow-including root and descent,
  every climb and descent ruled "crosses" moved onto them, assignment with
  its dirty-root marking, and the flat-tree walk. M.
- **D2b The surface and the events, in libdom**: § 7 behind the switch,
  § 8 whole (the slot half of the path, retargeting, closed trees,
  `composed`), `slotchange` at the checkpoint (§ 4), and libdom's
  descents. M.
- **D3 Rendering**: libflow's walks onto the flat tree, the slot rule in
  the change readers, the UA sheet's `slot { display: contents }`. Tested
  with the switch on, and in QEMU through the `yonder.conf` key. M, and it
  touches RELAYOUT's reuse, so Quinn reads it.
- **D4 Scoped style, and the switch on**: libgarb's scopes and their
  filter, `:host`, `:host()`, `::slotted()`, the context step, yonder's
  sheet list in shadow-including order, constructed sheets and both
  `adoptedStyleSheets`; then the switch goes on. M. ElementInternals
  (§ 9) lands here or not at all, Chris's call at D4.

Holding the switch off until D4 is the answer to § 2 applied to the
stack: until style is scoped, a page that feature-tests is better served
by its fallback than by shadow content drawn wrong.

**Ruled** (Chris, 2026-11-05, with Fable's leans): the whole model, the
surface trimmed by the census; D2 to D4 stacked and hidden behind one
switch, D2 split in two; D1 by size, which § 9 measures at M, so it
lands with D4; Fable reads D2a's two lists, Quinn reads D3, Codex reads
each PR.

## Appendix A. The climbs and the descents

*D2a's first commit, for Fable's read before any code. Against userland
473d6be7.*

How the inventory was made: every site in libhtml, libdom, libpage,
libgarb, libflow and yonder that walks `os64_html_node_t` up (`->parent`
in a loop, a root-of helper) or down (`->first_child`, or one of the
twenty walk helpers the layers wrote for themselves: `d_walk_next`,
`subtree_next`, `tree_next`, `next_node`, `next_el`, `next_element`,
`after_of`, `reuse_next`, `next_within`, `lifetime_next`, `next_in`,
`next_in_tree`, `text_next`, `query_next`, `following`, `preceding`,
`p_option_next`, `tree_root`, `root_of`, `dom_root`), found by pattern
(402 lines in 236 functions, the helpers' own bodies among them), then
read where the name did not say what the walk asks. Box-tree walks (`flow_box_t`) are not on it:
they walk what D3 builds. Child-list maintenance (`h_attach`,
`h_detach`) is not a walk.

Each entry is ruled one of three ways:

- **CROSSES**: the question is about the shadow-including tree, so the
  walk moves onto libhtml's `os64_html_shadow_including_root` (a climb)
  or `os64_html_shadow_including_next` (a descent, a host's root before
  its children).
- **FLAT**: the question is about what renders or inherits, so the walk
  moves onto the flat tree (§ 4's `os64_html_flat_*`). These are D3's, not
  D2a's.
- **STOPS, because …**: the standard scopes the question to one tree; the
  walk is unchanged and the reason is the one written here.

### A.1 Climbs

| Sites | Question | Ruling |
|---|---|---|
| libhtml dom.c `above`, `unit_root`, `depth_of` (and through them `d_ref`, `d_linking`, `d_unlinking`, `d_collect`, `d_detached`) | a node's reclamation unit; its depth against `max_depth` | CROSSES. These already climb from a template's contents to the template (`host`); a shadow root's host joins `host`, so the root lives and dies with its host, and depth counts through it because D3's flat walk descends through it |
| libhtml dom.c `may_insert`'s climb on `above` | is the parent inside the node being inserted | CROSSES: DOM's check is host-including, so a host may not be inserted into its own shadow tree |
| libhtml dom.c `root_of` in `split_records`, `d_parted`, `take_out`, `assert_records`, `d_root`; tree.c `tree_root` in `associate_form` | does a form owner share the node's tree | STOPS, because HTML resets a form owner that is not in the element's tree |
| libhtml fragment.c `h_parse_fragment`'s context climb | the fragment parser's context element | STOPS, because a ShadowRoot's `innerHTML` passes its host as the context explicitly |
| libdom window.c `tree_root` under `d_connected` and `d_in_window_tree`, and their callers: custom.c `children_changed` and `upgrade`, window.c `submit`, canvas.c `os64_dom_canvas_visit_damage`, view.c `d_computed_property` and `computed_empty`, pointer.c `connected` (its sixteen uses); libgarb cascade.c `connected` (and `mark_subtree`, `mark_changed`, `mark_children`, `garb_cascade_update` through it); yonder geometry.c `connected` and the snapshot's root check, yonder.c `node_in_document` and `asks_perform_with_fallback`'s root check | is the node in the document | CROSSES. DOM's "connected" is the shadow-including root being a document; these become one libhtml call |
| libdom implementation.c `d_node_document` | `ownerDocument`, by climbing to a document | CROSSES. Today a node in a shadow tree would climb to the shadow root, find no document, and answer the window's document by fallback, right by accident for the window's own tree and wrong for any other document's |
| libdom sheets.c `associated` | does a `style` or `link` have a sheet | CROSSES. A sheet's node in a connected shadow tree has a sheet; where it applies is § 6's scope |
| libdom compare.c `root_of`, `depth_of`, `is_ancestor`, `precedes`, `position`; collection.c `d_selector_test`; walker.c `inclusive_ancestor`, `retarget`, `following`, `walker_next` | `contains`, `compareDocumentPosition`, `closest`, TreeWalker and NodeIterator | STOPS, because each is defined over one tree; two nodes in different trees compare as disconnected |
| libdom window.c `d_form_owner` and `disabled`; classic.c `dom_root` (`form.elements`, named access); libpage state.c `tree_root` and `node_owner` (radio groups), core.c `p_ancestor`, submit.c `fieldset_disabled`; libgarb select.c `disabled` | form owner, radio group, fieldset disabling | STOPS, because HTML scopes each to the element's tree |
| libdom observer.c `add_transients`, `d_observers_changed` | which observers a mutation's `subtree` reaches | STOPS, because a mutation observer's `subtree` is the target's inclusive ancestors in its own tree |
| libdom event.c `activation_target` and `dispatch`'s two climbs; pointer.c `path_make` and `common` (the enter and leave chains) | an event's path | CROSSES by DOM's "get the parent": a slotted node's parent is its slot, a shadow root's is its host when the event is composed (§ 8; D2b) |
| libdom resize.c `node_depth`; libflow computed.c `declaration` and libgarb cascade.c `custom_value` (inheritance), flow.c `node_within` and `stacked_from`; yonder geometry.c's `offsetParent` climb; libpage core.c `in_hidden_subtree` | ResizeObserver's depth, inheritance, containing block and stacking context, `offsetParent`, hidden-ness | FLAT (D3). `offsetParent` then retargets, so a script outside never sees an inner node |
| libpage submit.c `directionality` | `dir=auto` and inherited direction | CROSSES, because HTML gives a shadow root its host's directionality |
| libpage core.c `option_step`, `os64_page_option_select`, `p_option_disabled`, state.c `option_select`; libdom options.c `options_add` | an option's select and optgroup | STOPS, because those are its tree parents |
| yonder svg.c `compute`, `reference`, `gradient_style` | SVG's inheritance and references in an SVG document | STOPS: yonder's SVG documents are trees of their own, with no shadow roots |

### A.2 Descents

| Sites | Question | Ruling |
|---|---|---|
| libhtml dom.c `lifetime_next` (`subtree_holds`, `d_pending_bytes`), `subtree_free` | holds and reclamation under a node | CROSSES, as a template's contents already do: a host's root is held and freed with it |
| libhtml dom.c `next_in` in `os64_html_clone` and `height_of` | a deep clone; a subtree's height | CROSSES for clone only into a `clonable` root (DOM's "clone a node"); height counts the root |
| libhtml dom.c `next_in_tree` in `split_records`, `d_parted` | form owner records under a moved node | STOPS, because form owners are per tree (A.1) |
| libhtml serialize.c `children`, `os64_html_serialize`; fragment.c `copy_result` | serializing and the fragment parser's result | STOPS, because a host's `innerHTML` and `outerHTML` exclude its shadow tree (`getHTML` booked, § 10) |
| libhtml core.c `os64_html_document_element`, dom.c `landmarks`, `child_of_kind`; libdom node.c `d_document_element`, `d_document_part`, view.c `root_element`, `body_element`, `view_member`, event.c `body_element`, document.c `title_element`, `title_text`, `title_set`; yonder.c `title_of`; libpage resolve.c `find_base` | the document's own landmarks: root, head, body, title, base | STOPS, because each is a child or descendant of the document in its own tree |
| libdom custom.c `children_changed` (the insertion walk that tries upgrades and queues `connectedCallback`; the removal walk that queues `disconnectedCallback`), `define`'s walk of the document, `registry_member`'s `upgrade(root)`, `d_custom_cloned`, `d_custom_adopted` | custom element reactions under a node | CROSSES, because the standard walks each one's shadow-including descendants (a lazily defined element whose instances already sit in other components' roots upgrades) |
| libdom window.c `subtree_next` in `d_script_mark_tree`, `d_script_copy_marks`, `d_script_collect`, `d_script_connected` | the scripts an insertion makes connected | CROSSES, because a script that becomes connected inside a shadow tree runs |
| libdom event.c `os64_dom_handler_attributes` | which event-handler attributes the page has | CROSSES: a handler on an element in a shadow tree is a listener the page has |
| libdom node.c `getElementById` (`method`), collection.c `query_next`, `query_step`, `refresh`, `d_select`; classic.c `next_node`, `d_form_control`, `named_property`, `d_classic_collection`; content.c `text_next`, `d_content_get`, `d_replace_content`; controls.c `d_child_text`, `d_label_control`; mixin.c `change`, `adjacent`, `normalize`, `reflect`; tables.c; options.c `text_step`, `option_text`; walker.c `walker_children`, `walker_siblings`, `iterator_traverse`; validity.c `d_form_validate` | `querySelector*`, `getElementsBy*` and live collections, `getElementById`, the document's collections and named access, `form.elements`, `textContent`, labels, `normalize`, table parts, option text, traversal, form validation | STOPS, because each is scoped to the receiver's own descendants or the document's own tree; a ShadowRoot is a receiver of its own for the queries (§ 7) |
| libpage core.c `collect_node`, `collect_model`, `collect_forms` | the page model: controls, links, frames, Tab order | CROSSES for gathering, in shadow-including order: a frame in a connected tree loads, and a control or link inside a component is one a person types into or follows. But what a person can REACH follows the flat tree: a node with no place in it (a light child assigned to no slot) renders nowhere, so the model counts it hidden, as it counts a node under `display: none`. Tab order by the flat tree (a component's own controls around the content it wraps, not before it) is its own DEBTS row with this slice, apart from `delegatesFocus`'s |
| libpage core.c `collect_ids` | the document's ids, for `#fragment` navigation and `form=` | STOPS for the document's map, because ids are per tree; a control in a shadow tree resolves `form=` in its own tree (libpage gains per-root lookup in D2a) |
| libpage core.c `walk_sheets`; yonder.c's sheet list | the page's sheets | CROSSES, each sheet carrying its scope (§ 6; D4) |
| libpage state.c `tree_next`, `os64_page_node_checked`, `os64_page_node_set_checked`, `p_radio_group_missing`, `select_assign`, `option_index`, `os64_page_node_selected_index`, `os64_page_node_value`, `p_state_set_value`, `attr_owner`, `attr_select_baseline`, `attr_selection`, `os64_page_node_set_attr`, `tree_plan_*`, `tree_radio_same`, `textarea_value_equal`, `tree_change`, `os64_page_node_clone` | control state: radio peers, selection, normalization after a tree change | STOPS, because each is per tree; a host inserted with a shadow tree brings controls whose groups are already their root's |
| libpage submit.c `text_blank`, `option_value_empty`, `contained_direction`; value.c `p_child_text_into`; core.c `gather` (under `p_subtree_text_into`), `p_subtree_text_into`, `first_legend`, `read_options` | an element's own text and children | STOPS: its own descendants |
| libgarb cascade.c `garb_cascade_limited`'s match walk, `subtree_next` in `mark_subtree` and `mark_children`, `garb_cascade_dump` | which elements the cascade matches and re-matches | CROSSES: every connected shadow tree is matched, by its scopes (§ 6; D4) |
| libgarb select.c `next_el`, `position`, `pseudo_matches`, `subtree_has`, `has_matches` | `:nth-*`, `:empty`, `:has()` | STOPS, because structural selectors are per tree |
| libgarb cascade.c `garb_parse_style_element` | a `style` element's text | STOPS: its own children |
| libflow boxes.c `after_of`, `kids`, `sib`, `count_items`, `element_box`, `table_part`; style.c `blank`, `next_element`, `f_style_build_reusing`; flow.c `motion_seen`, `draws_none`; reuse_snapshot.inc `reuse_next`, `reuse_snapshot_new`; dump.c `f_style_dump` | box building, style order, list counting, table parts, the reuse snapshot; pictures are fetched for the boxes this builds | FLAT (D3) |
| yonder.c `webfonts_chars` | the characters a web font must cover | FLAT (D3): the text that renders |
| yonder.c `diag_elements` | the record's element census | CROSSES: the record counts what the page has |
| yonder scripts.c `yonder_scripts_preload`, `inline_source`; yonder.c `sheet_inline`, `sheet_inline_matches`, `next_within`, `button_caption`; geometry.c `next_node` in `yonder_image_map_hit`; svg.c `node`, `fragment_target`, `gradient_paint`, `clip_source`; svg_document.c | the preload scan, an element's own text, a button's caption, `usemap`, SVG `url(#id)` | STOPS: the preload scanner reads what the parser wrote, and nothing parses into a shadow tree until declarative roots (§ 10); a button is no valid shadow host; `usemap` and `url(#id)` resolve in their tree |

### A.3 The second scan

Fable's second read found walks the first pattern missed: helpers outside
its twenty names (libgarb's `parent_el`, `prev_el`), single steps rather
than loops, and bare root climbs. The inventory is now made by a tool
whose pattern does not depend on a list of names:

```
python3 -I tools/walk_inventory.py                                          # the inventory
python3 -I tools/walk_inventory.py --check docs/design/pending/SHADOW_DOM.md   # exit 1 on an unruled walk
```

`tools/walk_inventory.py` prints every line in the six directories that
steps through `->first_child`, climbs (`X = X->parent`, or a loop whose
condition reads `->parent`), or calls a WALK HELPER: any function there
that returns a node pointer and whose body reads a tree link
(`->parent`, `->first_child`, `->last_child`, `->next`, `->prev`) or
calls another walk helper, found to a fixed point. Comments and helper
prototypes are skipped. Against 473d6be7 it prints 893 lines in 416
functions, 96 of them helpers, and `--check` finds every one of those
functions named in A.1, A.2 or the table below (a copy of this doc with
one name removed fails it). The check is by name: it proves no walk is
unruled, not that each ruling sits beside the right file, which is the
reader's half. D2a's last commit re-runs it against the code as D2a
leaves it.

| Sites | Question | Ruling |
|---|---|---|
| libgarb select.c `parent_el`, `prev_el` in `match_from` | a descendant, child or sibling combinator | STOPS: § 6's first rule, a combinator never leaves its tree |
| libgarb select.c `lang_matches`, `dir_matches` | `:lang()`, `:dir()` | CROSSES: HTML gives a shadow root its host's language and directionality, as A.1 rules for libpage's `directionality`; the two must agree |
| libdom canvas_text.c `text_method`, `canonical_font` | a canvas's inherited direction and font | CROSSES, the same rule |
| libgarb cascade.c `animation_names`; libflow style.c `cell_table`, `lists_above`, `sheet`, `hints`, `counter_list`, `open_element`, `f_style_beyond`, `f_pseudo_of`, `f_pseudo_origin`; computed.c `flow_computed_new`; flow.c `add`, `flow_clip_parent` (and yonder geometry.c's and `yonder_intersections`' uses of it); layout.c `paint_for`; reuse_match.inc `reuse_element`, `reuse_derived_equal`; boxes.c `scan`, `flow_range`, `loose_end`, `row_range`, `rows_range`; dump.c `element` | inheritance, the style walk's parents, list and table structure, clipping ancestors, the reuse match | FLAT (D3) |
| yonder pointer_input.inc `pointer_default` | is the hit node in the page's document, before its default action | CROSSES. Unmoved, every press inside a component loses its default action |
| yonder pointer_input.inc `pointer_turn`, `pointer_route`; yonder.c `element_hit`, `element_at` and their callers `link_at`, `view_event`, `view_context`, `hover` | the node under the pointer | FLAT: hit testing answers the innermost flat-tree node, and dispatch retargets (§ 8) |
| yonder.c `element_of`, `parts_what` | the element holding a text node, for an event and for the record | FLAT: text that is a child of a shadow root (what `relative-time` writes) climbs to no element today |
| yonder scripts.c `run_item` | is the script's element connected | CROSSES: the other half of `d_script_*` (A.2) |
| yonder svg.c `yonder_svg_contains` | does an inline `svg`'s chain end at the document | CROSSES. Unmoved, an icon inside a component is refused as unsupported |
| yonder svg.c `resource_transform` | `transform: inherit` | FLAT: inheritance |
| libdom attr.c `detach`, `attr_document`, `attr_member`; implementation.c `d_window_document`, `d_leaving`, `document_take`; mixin.c `text_member`, `d_base_uri`; options.c `options_length_set`, `d_options_set_index` (each through `d_node_document`) | an Attr's or a node's document | CROSSES, riding `d_node_document` (A.1) |
| libdom mixin.c `mixin_method`: `contains` | is the argument a descendant | STOPS (DOM's `contains` is per tree) |
| libdom mixin.c `mixin_method`: `getRootNode` | the node's root | STOPS without `composed`, CROSSES with `{composed: true}`: the option is read and ignored today; D2b makes it count |
| libdom node.c `d_node_clone` | `cloneNode` | CROSSES only into a `clonable` root, as libhtml's clone (A.2) |
| libdom pointer.c `changed`, `os64_dom_pointer_dispatch`'s `common` | is the captured or hovered node still connected; the common ancestor of down and up | CROSSES (connected), and the event path's rule (§ 8) |
| libdom classic.c `d_classic_match`, `node_names`; collection.c `matches`; controls.c `control_member`; forms.c `forms_member`; event.c `slot_compile`; options.c `option_member`; window.c `d_activation_after`, `element_method`; libpage core.c `form_owner`, `add_control`, submit.c `node_barred`, state.c `radio_group_member`, `attr_stage`, `attr_radio_group`, `tree_radio_trigger` | form owner, labels, datalist, radio groups | STOPS, riding the rulings of A.1 |
| libpage state.c `tree_plan_collect`'s `tree_root(...) == document` | is the control connected, before `form=` is resolved | CROSSES for the test (HTML resolves `form=` only for a connected element); the id lookup itself STOPS, per tree |
| libpage state.c `tree_plan_peers`, `tree_plan_normalize`, `tree_plan_select`, `tree_option_destination`, `os64_page_node_set_text`, the `option_selected` functions; core.c and submit.c's `os64_page_option_*` users | control state after a change, an option's select, a textarea's default value | STOPS: per tree, or the element's own children |
| libdom compare.c `trees_equal` | `isEqualNode` | STOPS: compares children, never a shadow tree |
| libdom document.c `active_element`, `document_member`, `d_body_set`; view.c `scrolling_element`, `scroll_owner`; event.c `attribute_source`, `slot_refresh`, `slot_attribute_pending`; implementation.c `document_member`; node.c `property`'s navigation; tables.c; walker.c `last_of` | landmarks, navigation, table parts, traversal | STOPS, riding A.2 (`activeElement`'s retargeting is § 8's) |
| libhtml dom.c `child_of_kind` in `may_insert` and `d_seat_html`; the fragment cases of `may_insert`, `d_seen`, `place`, `os64_html_insert` | the document's own children; a fragment's children moving on insert | STOPS. Whether a ShadowRoot passed to an insertion behaves as a DocumentFragment is DOM's pre-insert rule, decided in D2a's tests |
| libdom node.c `element_sibling`; options.c `option_at`, `option_count`, `options_remove`, `select_member`; tables.c `after_captions`, `create_body`, `create_part`, `insert_at`, `insert_row`, `set_part`, `table_member`; libpage core.c `os64_page_option_next`, state.c `os64_page_node_init_option_selected`, `os64_page_node_option_selected`, `os64_page_node_set_option_selected`, submit.c `select_missing` | element siblings, a select's options, a table's parts | STOPS: the element's own children and siblings |
| yonder geometry.c `yonder_geometry_snapshot` | the snapshot's "is it in the document" root check; its `offsetParent` climb | CROSSES for the root check (A.1); FLAT for `offsetParent` (A.1) |
| yonder svg.c `yonder_svg_render_image_tree`, svg_document.c `yonder_svg_document_parse`, `yonder_svg_document_size` | an SVG image's own document | STOPS: a tree of its own (A.1) |
| libflow boxes.c `f_boxes_build`, `inline_element`; computed.c `containing`, `positioned_block`; flow.c `within`, `filtered_open`; layout.c `definite_height`, `quirk_no_wrap_round` | box building, containing blocks, the quirks that read an element's ancestors | FLAT (D3) |
| Builders, set aside: libhtml dom.c `os64_html_create_comment`, `os64_html_create_doctype`, `os64_html_create_document`, `os64_html_create_element`, `os64_html_create_fragment`, `os64_html_create_text`, `node_new`, `clone_one`, `contents_new`, `data_new`; core.c `h_node`, `d_node_new`; fragment.c `os64_html_parse_fragment`, `os64_html_parse_fragment_root`, `pack_node`; tree.c's insertion modes (`adopt`, `adoption`, `body`, `clone`, `doctype`, `element`, `foreign`, `insert_character`, `insert_comment`, `process`, `raw_element`, `reconstruct`, `rooted`, `synthetic`) and xml.c `event`, `flush`; libdom implementation.c `create_document`, `create_document_type`, `create_element_ns`, `create_html_document`, `parse_from_string`, `document_made`; custom.c `d_custom_create`, `d_html_element_constructor`, `constructed_wrong`; classic.c `classic`, `character_constructor`, `fragment_constructor`; options.c `option_factory`; mixin.c `into_node`, `adjacent_steps`; content.c `d_content_set`; canvas.c `d_canvas_prepare`, `d_canvas_sync` (their `create` is the canvas's, not a node); clone.c `write_blob`, `write_error`, `write_value` (a structured clone's `data_new` is a value, not a node) | making nodes | not walks: they build a tree, and the parser builds the document's (nothing parses into a shadow tree until declarative roots, § 10) |
| Box walks, set aside: yonder geometry.c `next_box`, yonder.c `anchor_of` (and its callers `lay_out_shown`, `script_rebuild`), `reaches_box`, `canvas_damage_mark`; libflow flow.c `boxes_differ`, `flow_box_covered`, `flow_box_cut_away`, `pieces_holder`, `parts_in`, `stack_build`, `box_named`, `box_seen`, `flow_visit_text_clip`, `visit_clip_subtree`, `ink_grown`, `sticky_new`; layout.c `block_contents`, `cell_layout` (their `children` is layout's own); reuse_match.inc `reuse_match_box`; style.c `style_pseudo` (it writes a generated node's children) | the box tree | not node walks: D3 builds the boxes they walk from the flat tree |

Not on any list because nothing walks for them in yonder today:
find-in-page (yonder has none) and selection (libdom's selection.c walks
no tree). Each learns shadow trees the day it is built.

### A.4 Walks D2a adds

What D2a writes is held to the same check, so its own walks are ruled
here.

| Sites | Question | Ruling |
|---|---|---|
| libhtml dom.c `os64_html_attach_shadow`, `shadow_new`, `os64_html_shadow_including_parent`, and the rest of § 3's verb and walks | the shadow-including tree itself | the climb and descent that CROSSES means |
| libhtml shadow.c `slots_of`, `find_slot`, `slot_root`, `os64_html_assigned_slot`, `os64_html_slot_assigned`, `os64_html_slot_assigned_count`, `os64_html_flat_first_child`, `os64_html_flat_next_sibling`, `os64_html_flat_parent` | a shadow tree's slots; a host's children; the flat tree | STOPS for finding slots (a host's slots are in its own shadow tree, not in nested ones); these are the walks FLAT means |
| libgarb select.c `inheriting_parent_el` | the element `:lang()` and `:dir()` inherit from | CROSSES (A.3) |
| libpage core.c `tree_id` | an id in a shadow tree, for `form=` | STOPS: ids are per tree (A.2's `collect_ids` row) |
