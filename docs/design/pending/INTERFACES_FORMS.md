# INTERFACES_FORMS.md — the DOM's interface objects and the form controls' members, and the order to add them

*Slice 0f of the Modern Web arc ([MODERN_WEB.md](MODERN_WEB.md)): an
inventory, not an implementation, in the method and the format of
[WINDOW_DOCUMENT.md](WINDOW_DOCUMENT.md) (Slice 0e). Written on 2026-10-25 at
`opus/tmw-0f-interfaces` 0246cb7e, which is PR #352's head (slice 3, the
modules slice, with `link.rel` and `relList`). Nothing under `userland/`
changed for it. It ships with the first phase it plans. Every number below
names its source; § Sources gives each source's date, and § What could not be
determined lists the gaps.*

## Summary

Slice 3 made yonder run framework code for the first time, and the first
three framework pages tried on it each died on a name the binding lacks:

| page (a public demo) | framework | what it asked |
|---|---|---|
| a Vite React shop demo, inventory page | React 19 | `select.options`, then its `length` and each option's `selected` (react-dom's option updater) |
| TodoMVC, Svelte build | Svelte 5 | the global `Text`, whose prototype Svelte's start-up patches |
| TodoMVC, Vue build | Vue 3 | the global `SVGElement` (`app.mount` asks `container instanceof SVGElement`) |

They belong to two families, inventoried here as Window and Document were in
0e:

- **(a) the DOM's interface objects**, the global constructors pages use for
  `instanceof` and for patching prototypes. webref exposes **1,072 names** on
  Window (interfaces, namespaces, aliases and legacy factory functions such as
  `Image`); **934 ship** in at least one engine (BCD 8.1.7). The binding has
  **61** of them: `Event` and `Image` classic, the rest modern (the switch on).
- **(b) the form controls' members**: every member of `HTMLSelectElement`,
  `HTMLOptionElement`, `HTMLOptionsCollection`, `HTMLOptGroupElement`,
  `HTMLInputElement`, `HTMLTextAreaElement`, `HTMLButtonElement`,
  `HTMLFormElement`, `HTMLLabelElement`, `HTMLFieldSetElement`,
  `HTMLOutputElement`, `HTMLDataListElement` and `ValidityState`, and of what
  they hand out that is a form interface of its own
  (`HTMLFormControlsCollection` from `form.elements`, `RadioNodeList` from its
  `namedItem`, `HTMLLegendElement` inside a fieldset): **215 shipped members**,
  of which the binding has **21**.

| family | shipped | present | unweighted | corpus-weighted | Chrome-weighted |
|---|---:|---:|---:|---:|---:|
| (a) interface objects on Window | 934 | **61** | 6.5% | **54.0%** of interface-object uses in the 2026-10-25 pages (54.6% in 0e's 839) | none: Chrome counts no interface-object read (§ Usage, 1) |
| (a), the 224 that are prototype questions (classes a and p, and the present) | 224 | 61 | 27.2% | **80.1%** of `instanceof`/`.prototype` uses (80.8%) | none |
| (b) form members | 215 | **21** | 9.8% | **4.2%** of the 110 distinctive members' page mentions (5.6%); 23.9% counting every name (22.2%) | 6 members have a figure, all under 1.2%: not used as a weight |

The weightings are § Usage's; "distinctive" leaves out the 105 members whose
names (`value`, `name`, `type`, `form`, `length`, `options`, `labels`...) any
object may have, so a mention of them says nothing about a form control. The
present members are mostly generic names (`value`, `type`, `name`), which is
why (b)'s distinctive figure is so low and its all-names figure is not.

**What blocks pages most.** Three findings, in the order they bite:

- **The interface objects pages trip on are mostly APIs, not prototypes.**
  Of the 3,813 file-name pairs in which the 2026-10-25 records name an absent
  shipped interface as MISSING, the top four are `MutationObserver` (364
  files, 66 sites; slice 5a), `PerformanceObserver` (216), `MessageChannel`
  (110) and `CSS` (101): each comes with its API, and its interface object
  with it. Every phase below together resolves 13.2% of those pairs. Of the
  224 interfaces that ARE prototype questions, the plan resolves 50.1% of
  their pairs, 89.0% of those outside the polyfill list (next point), and
  takes them to 99.4% of their `instanceof`/`.prototype` uses.
- **65 records name the same 27 collection names together**: `NodeList`,
  `HTMLCollection`, `DOMTokenList`, `FileList`, `TouchList`, `SVGLengthList`
  and the rest, with `ClientRectList`, `CSSValueList`, `PaintRequestList` and
  `SVGPathSegList`, which no current IDL defines. They are the absent names of
  a list of 31 (the other four are present), core-js's iterable-DOM-collections list
  (`internals/dom-iterables.js`, read at 3.51.0), which reads each name off
  the global and patches the prototype only when it exists, so the absence
  costs nothing. It is why `NodeList`, `HTMLCollection` and `DOMTokenList`
  head 0e's Appendix D ("16/21") and this pull's tally (76, 68 and 71 files);
  outside those 65 records they are named by 11, 3 and 6. Appendix A gives
  every row both counts.
- **What throws, by the records' FAILED lines** (`'X' is not defined`, over
  the 873 judged records): `EventTarget` 14 files (5 sites), `HTMLMetaElement`
  9 (2), `HTMLCanvasElement` 6 (1), `MouseEvent` 5, `NodeList` 4,
  `KeyboardEvent` 4, `Window`, `SVGElement` and `Text` 2 each, and `Location`
  34 files (one site). For (b), the FAILED lines do not name the member
  (`cannot read property 'length' of undefined` is in 217 lines and cannot be
  attributed, § What could not be determined); the three known pages and the
  framework sources are the evidence there.

**The plan, in one line each** (§ The phase plan has each phase in full):

| phase | what | adds | (a) inst. / (b) distinctive after |
|---|---|---|---|
| P1a | the three asks: `Text`, `Comment`, `SVGElement`; `select.options` and the option's own members; `getRootNode` | 8 interfaces, 24 form members, 1 Node member | 83.5% / 10.0% |
| P1b | `EventTarget`, `Window`, `NodeList`, `HTMLCollection`, `DOMTokenList`, `DOMStringMap` | 6 interfaces | 91.5% / 10.0% |
| P2 | the text controls' selection | 12 members | 91.5% / 32.3% |
| P3 | the controls' reflections, form owner and labels; label, fieldset, legend, output; the form's named access | 86 members, 6 interfaces | 91.5% / 87.6% |
| P4 | constraint validation, whole, with `invalid` | 49 members, `ValidityState` | 91.5% / 99.6% |
| P5 | numbers and dates on input | 4 members | 91.5% / 99.9% |
| P6 | the element kinds whose members are reflections | 36 interfaces | 94.9% / — |
| P7 | `Location`, `History`, `Navigator` | 3 interfaces | 96.5% / — |
| P8 | the event interfaces yonder's events lack | 7 interfaces | 99.4% / — |

P1a is first because three named pages die there; **the numbers rank P1b's
names above P1a's** (EventTarget alone throws on 14 files, and 159 pages ask
`instanceof HTMLCollection` or read its prototype), and P1a stays first only
because it is the smaller PR and its pages are known (§ Rulings, 1).

**The interface-object hazard, which binds every phase.** 0e's handler
hazard has an analogue: an interface object's existence is how some pages
detect its element's feature. The corpus has a component that calls
`showModal` when `window.HTMLDialogElement` exists, and a shadow-DOM polyfill
that picks its path by `window.HTMLSlotElement`. So **an element interface
whose element's behaviour yonder lacks joins with that behaviour**, and an
interface whose members are reflections and plain state (div, meta,
template's `content`, the table rows) may join as soon as it has a prototype
of its own. § Classification has the line, per interface in Appendix A.

## How the binding was measured

**The probe.** As 0e's: one case added to `tools/test_dom_host.c` in a scratch
copy of this tree (`git archive` of 0246cb7e), built by
`tools/test_dom_host.sh` in both its profiles (they agreed line for line),
with a binding made as `os64_dom_default_options()` makes one at an `https:`
address, run with `modern` on and off. Its script (Appendix C) is 0e's
Appendix E's `dump`, extended: it adds a form with every control in it, a
label, fieldset, legend, output, datalist, an optgroup and options, a text
node, a comment and an SVG element, walks each object's prototype chain, and
walks the objects they hand out (`options`, `selectedOptions`, `elements`,
`elements.namedItem`, `validity`, `labels`, `files`, `list`, `htmlFor`,
`childNodes`, `getElementsByTagName`, `querySelectorAll`, `classList`,
`document.forms`), then tries the three known asks.

What it found, beyond the member lists in Appendix B:

- **The chains.** A form control is `HTMLxxxElement.prototype` →
  `HTMLElement.prototype` → `Element.prototype` → `Node.prototype` →
  `Object.prototype`. HTMLElement's level holds only `constructor`: `click`,
  `focus`, `blur`, `hidden`, `dataset`, `style`, `tabIndex` and the classic
  `on<type>` handlers live on Element's, so an SVG element has them too.
  `addEventListener` and its two siblings are Node's own, and the global's,
  and each plain event target's (AbortSignal, XMLHttpRequest, MediaQueryList,
  Performance, ScreenOrientation, Attr, through `d_event_target`): there is no
  EventTarget level.
- **option, optgroup, label, fieldset, legend, output, datalist** are plain
  HTMLElements (`[object HTMLElement]`). A text node and a comment are
  `[object CharacterData]`. An SVG element is `[object Element]`.
- **`document.createElementNS` is absent** (WINDOW_DOCUMENT P1-Db, not built),
  so the probe made its SVG elements with the parser (`innerHTML`); the brief
  asked for `createElementNS`.
- **One collection prototype serves every live list**: `childNodes`,
  `getElementsByTagName`, `document.forms` and `form.elements` all inherit a
  level with `length`, `item` and `namedItem` and no `constructor` or
  `Symbol.toStringTag`. So a NodeList has `namedItem`, which no NodeList has.
  `querySelectorAll` answers a plain object (LIBDOM.md § Objects and
  collections). `classList`'s level is a prototype of its own (DOMTokenList's
  members, `supports` included), and so is `dataset`'s.
- **`form.elements`** lists the twelve inputs, the select, the textarea and
  the button, but not the fieldset or the output, which HTML lists too; and
  its `namedItem('r')` over two radios named `r` answers the first input, not
  a RadioNodeList.
- **The asks:** `select.options` is undefined, so React's
  `options.length` throws; `typeof Text` and `typeof SVGElement` are
  "undefined". `option.selected = true` makes an expando: the select's
  `selectedIndex` did not move.

**What yonder adds beyond libdom** is unchanged from 0e: providers, which
change what members answer and not which exist, and `console`. The probe's
lists are yonder's lists.

### The prototype rule, applied

DEBTS.md ("Interface objects only for the binding's own prototypes") records
the rule this inventory is bound by: **naming a broader prototype for an
interface would make `instanceof` lie**, so an interface whose objects share
a prototype with another needs a prototype of its own first. The binding's
kinds (`internal.h` `D_PROTO_*`, chosen in `node.c`'s `node_prototype_kind`,
parented by `d_proto_parent`) are Node, Document, Element, CharacterData,
DocumentFragment, DocumentType, Attr, HTMLElement, and the HTML kinds input,
textarea, select, button, form, image, script, anchor, link and iframe. Every
kind has its interface object today (`classic.c` `d_interfaces_install`, the
frames row for iframe). Per interface:

| interface | its objects today | first step |
|---|---|---|
| `Text`, `Comment` | share CharacterData's | a TEXT and a COMMENT kind under CharacterData (libhtml already tells them apart) |
| `SVGElement` | share Element's with every non-HTML element | an SVG kind under Element for `OS64_HTML_NS_SVG` |
| `HTMLOptionElement`, `HTMLOptGroupElement`, the other HTML elements | share HTMLElement's | a kind per tag, as the form controls have (every tag named here is in libhtml's `tags.h`) |
| `NodeList`, `HTMLCollection` | one collection prototype for both | two prototypes, and `querySelectorAll`'s list made a static NodeList |
| `HTMLOptionsCollection`, `HTMLFormControlsCollection`, `RadioNodeList` | none (no `options`; `elements` is the shared collection; no radio list) | each its own prototype, inheriting HTMLCollection's or NodeList's |
| `DOMTokenList`, `DOMStringMap` | their own prototypes (`mixin.c`'s token and map classes) | none: they can be named as they are |
| `EventTarget` | no level | one EventTarget.prototype that Node.prototype, the global's chain and each plain target's prototype inherit |
| `Window` | the global has no interface prototype | WINDOW_DOCUMENT P3c's chain (§ Rulings, 3) |
| `Location`, `History`, `Navigator` | plain objects | a prototype each |
| `MouseEvent`, `KeyboardEvent`, `FocusEvent`, `UIEvent`, `InputEvent`, `SubmitEvent` | yonder's events are Events with own data properties (`event.c`) | a prototype each, the data as accessors |
| `ValidityState` | none | with constraint validation (P4) |

## Usage, and what it can and cannot say

1. **Chrome's use counters** (chromestatus.com `/data/featurepopularity`,
   the day 2026-10-24, fetched 2026-10-25).
   - **No counter reads an interface object.** The only
     `V8Window_*_ConstructorGetter` counters are prefixed names
     (`webkitSpeechRecognition` 9.41% and its family, `webkitRTCPeerConnection`
     6.47%, `WebKitAnimationEvent` 1.06%). What Chrome counts is
     *construction* for some APIs: `IntersectionObserver` 57.27%,
     `MessageChannel` 54.38%, `ResizeObserver` 45.46%, `CompressionStream`
     23.87%, `ReadableStream` 16.63%, `BroadcastChannel` 15.39%, `FontFace`
     8.46% (Appendix A's column). Construction is a lower bound for the
     interface object's read, and none of these is a prototype question.
   - **Five form members have a counter**: `capture` 1.15%, the fieldset's
     `elements` 0.080%, `valueAsDate` 0.079% (`InputElementValueAsDateGetter`),
     `webkitEntries` 0.042% and input's `showPicker` 0.031%; webstatus.dev
     gives select's `showPicker` 0.0004% (below). Six of 215, all under 1.2%,
     cannot rank a family, so (b) has no Chrome-weighted figure.
   - Feature-level counters say how common forms are, not which members
     scripts touch: `FormElement` 78.86% (a page has a form element),
     `FormsSubmitted` 16.34%, `FormValidationStarted` 4.29% (interactive
     validation on a submission), `DataListElement` 1.58%. And one is a
     member's lower bound: `HTMLInputElementTypeChangedWhileDisconnected`,
     66.83%, is an input's type changing while it is in no document (by its
     attribute or its `type`, which the binding has).
2. **webstatus.dev** (with web-features 3.44.0) adds per-feature figures only
   where a member is its feature: `show-picker-input` 0.031% (the counter's
   figure again), `show-picker-select` 0.0004%, `datalist` 1.57%. The rest
   map our interfaces to whole features (`select`, `input`, `form`,
   `constraint-validation`, `dom`, `svg`) for which the API serves no usage
   or a feature-level one; they were not used.
3. **HTTP Archive** is behind BigQuery, as 0e found. No number is used.
4. **Our corpus**, in four instruments:
   - **The new pull, 2026-10-25**, is the P5's whole record directory: 879
     records dated 2026-10-23 18:05 to 2026-10-25 19:44 UTC by their
     `clock:` lines (a line the older pulls' records do not carry), 137 sites,
     825 of them kept pages (128 sites). It is counted on its own, beside
     0e's 839 kept pages of 2026-10-21, -22 and -22b (163 sites), never
     summed with them.
   - **MISSING files/sites/asks** (Window only, as in 0e: the global-miss hook
     hears lookups on `window`): `python3 -I tools/yonder_tally.py --kind global
     --top 0` over the new pull's records. The tally refuses a directory with
     any record it cannot judge, and six records hold a `MISSING global`
     line with an empty name, so it was run over the other **873** files
     (137 sites; 61 of them marked unjudged, their kept lines counted). The
     P5 changed builds during the pull, so a name present at 0246cb7e can be
     MISSING in an earlier record (`HTMLIFrameElement` is in 9 files); for a
     name absent at 0246cb7e that does not matter. **Outside the list** is
     the same count over the records that do not name `PaintRequestList`,
     the marker of the 65 polyfill records (§ Summary).
   - **FAILED files/sites**: the records whose FAILED lines say `'X' is not
     defined`, which is a lookup that threw rather than one a page guarded.
   - **The module subset.** The records name no build. The last `MISSING
     script module` is at 15:25 UTC on 2026-10-25, and module failures of a
     build that runs modules (a bare import refused, a specifier not
     resolved) first appear at 18:46. The **31 records after 15:25** (12
     sites, 17:05 to 19:44) are the candidate slice-3 subset; Appendix A's
     "module subset" column is its MISSING (M) and FAILED (F) files. In it,
     `Text` fails 2 files and `SVGElement` 1, and `MutationObserver` 11. Its
     MISSING interface names (30) are led by the same APIs as the whole
     pull's: `MutationObserver` 16 files, `PerformanceObserver` 8,
     `MessageChannel`, `IntersectionObserver` and `BroadcastChannel` 4 each,
     then `ShadowRoot`, `HTMLDialogElement`, `CSSStyleSheet` and `CSS` 3 each
     and `Text` 2. Module code asks for what classic code asks for, plus
     `Text` and `SVGElement`.
   - **Scan pages**: a static scan of each kept page's `<script>` bodies,
     `on<type>` attribute values and every `.js` kept beside it (not the
     sheets). An interface counts on `instanceof X`, `X.prototype`,
     `typeof X`, `window.X`/`self.X`/`globalThis.X`, `new X(` or
     `'X' in window`; the **inst.** figure is the first two alone, the uses
     the prototype rule is about. A member counts on `.x`, `['x']` or
     `'x' in`. A member's receiver cannot be seen, so a name any object may
     carry is marked **g** (generic) in Appendix B and left out of the
     distinctive weighting; even the distinctive names collect a framework's
     own tables (react-dom spells `htmlFor`, `formAction` and `defaultValue`
     as properties of its own objects). It is a rank, not a census, as 0e
     said of its scan.
5. **The frameworks' own sources**, read from npm on 2026-10-25 to say what
   each known page meets after its first ask (the demos' builds were not
   fetched, so their versions are not known):
   - **react-dom 19.3.1**: `updateOptions` reads `options`, its `length`,
     each option's `value` and `disabled`, and writes `selected` and
     `defaultSelected`; `updateTextarea` writes `defaultValue`. Its select
     event plugin asks `"selectionStart" in el` of the focused field and,
     when that is false, calls `window.getSelection()` unguarded: that is
     the `getSelection` slice 3 saw when typing into the shop's login field.
     A text input with `selectionStart` never takes that branch.
   - **svelte 5.57.2**: `init_operations` reads `Element.prototype`,
     `Node.prototype` (the `firstChild` and `nextSibling` getters, present
     as accessors) and `Text.prototype`; the mount path's `first_child`
     asks `first instanceof Comment`, and `deep_read_state` asks
     `value instanceof EventTarget`.
   - **@vue/runtime-dom 3.5.44**: `resolveRootNamespace` asks
     `instanceof SVGElement`, then `MathMLElement` behind a `typeof` guard;
     `v-model`'s `beforeUpdate` on a text field calls `el.getRootNode()` and
     asks `instanceof Document || instanceof ShadowRoot` when the model
     changes the field's value. `getRootNode` is absent (0e § Node).

Neither the scan nor the tally alone ranks everything, so the plan is ordered
by both, with the FAILED lines and the three pages first.

## Classification

0e's five classes are recast for this inventory as the brief asks:

| class | meaning | (a) rows in Appendix A | (b) members |
|---|---|---:|---:|
| present | there at 0246cb7e | — | 21 |
| a, cheap | reads state the binding, libhtml or libpage already keeps, or a reflection; for (a), an interface whose prototype already exists | 4 | 104 |
| p, a prototype of its own | the objects exist on a broader prototype (or not as their own object); a kind or a level first, then members as cheap as their class | 69 | 8 |
| m, new machinery | a new piece of state, a host hook or an algorithm, or a whole API | 21 | 62 |
| r, layout or rendering | needs the layout's answer or a new drawing | 6 | 1 |
| d, defer | a subsystem nothing plans, a feature the element announces (the hazard), or skip (obsolete, a phone's, meaningless on one machine) | 106 | 19 |

The (a) column counts Appendix A's 206 rows, the absent shipped interfaces
any source names; the other 667 absent shipped names, which no source
names, are counted by spec after Appendix A's table; 0e's Appendix D names
most of them.

**The interface-object hazard, per kind of interface.**

- **An element interface whose members are reflections and plain state**
  announces nothing: `HTMLDivElement`, the lists, headings, `HTMLMetaElement`
  (`name`, `content`, `httpEquiv`, `media`, `scheme`), `HTMLTemplateElement`
  (`content` is the template contents libhtml already keeps; its
  `shadowRoot*` members are declarative shadow DOM, absent, which is how
  pages detect that), the table elements. They are (p): P6.
- **`HTMLCanvasElement`** is (p) in P6 although canvas is absent: pages
  detect canvas by `getContext`, not by the interface, and the corpus's
  failures on it are a `toBlob` polyfill reading `HTMLCanvasElement.prototype`
  unguarded (17 of the kept scripts hold it). It gets `width` and `height`; `getContext`
  and the rest stay canvas's.
- **An element interface whose element's behaviour yonder lacks** is (d) and
  joins with that behaviour: dialog, details, slot, picture, the media
  elements, object, embed, frames, marquee, meter and progress.
- **API interfaces** come with their API (m, r or d), and are not this
  inventory's to plan; § What the plan leaves names them.

**Rule 1 for an interface named before all its members.** Slice 0a named
`HTMLSelectElement` over a prototype with a few of its 26 members (5 at
0246cb7e, one partial) and `HTMLElement` with none of its own; this
inventory continues that, and it needs saying as a rule
(§ Rulings, 2): an interface object may join with fewer than all its members
when each missing member is detected on its own and the interface announces
nothing (the hazard above); a member joins only whole (constraint validation
is one API, P4).

## The phase plan

Each phase is one reviewable PR unless it says otherwise, and adds a row to
modern.c's table, as 0e's phases did: interface objects and members both go
behind the switch, and the DOM host's name check holds the row
(`tools/test_dom_modern.inc`; prototype levels themselves are the same with
the switch on or off, Slice 0a's rule). Corrections to members that exist
(`form.elements`'s missing fieldset and output, `select.remove(index)`) are
classic fixes and say so. Each phase names its WPT oracle at the runner's pin
`a699fb16`; a directory not in `tools/wpt_fetch.sh`'s `WPT_DIRS` is marked
"add". "After" figures are § Coverage by phase's.

### P1a — the three asks (8 interfaces, 24 form members, `getRootNode`)

(a) to 83.5% inst. of the prototype interfaces; (b) to 10.0% distinctive,
42.8% all names.

- **`Text` and `Comment`**: TEXT and COMMENT kinds under CharacterData, their
  interface objects, and their constructors (`new Text(data)`,
  `new Comment(data)`, DOM's), which `d_wrap_constructed`
  already supports (DocumentFragment's). Text's own members: `splitText`
  (a libhtml text split through the state-aware text entrance) and
  `wholeText`; `assignedSlot` is null, which is right while there is no
  shadow DOM. Svelte's start-up and its first `instanceof Comment`.
- **`SVGElement`**: an SVG kind under Element for elements in the SVG
  namespace, with `ownerSVGElement`, `viewportElement` and `className` as an
  **SVGAnimatedString** (`baseVal` reflects `class`, `animVal` reads it), and
  `SVGAnimatedString`'s interface object. Vue's mount. The SVG subclasses
  (`SVGSVGElement`, `SVGGraphicsElement`...) are slice 6's, whose members are
  the renderer's (SVG.md); until then every SVG element is an SVGElement,
  which is true, and no `instanceof SVGSVGElement` can lie because the name
  does not exist. `className` becoming an object is § Rulings, 4.
- **`Node.getRootNode`**: pulled forward from 0e's P4a, since Vue's `v-model`
  calls it as soon as an app changes a field's value.
- **The select's options**: OPTION and OPTGROUP kinds;
  `HTMLOptionsCollection` as a live collection over the select's list of
  options (libpage's option list, `os64_page_option_t`), with `length`
  (settable), `add`, `remove`, `selectedIndex`, the indexed setter and
  HTMLCollection's `item`/`namedItem`/indices; the select's `options`,
  `length`, `item`, `namedItem`, `add`, `remove(index)` (a classic fix beside
  ChildNode's `remove()`), the indexed setter, `selectedOptions`, `multiple`
  and `size`; the option's `value`, `text`, `label`, `selected`,
  `defaultSelected`, `disabled`, `index` and `form`; the optgroup's
  `disabled` and `label`; and the `Option` factory. `selected` needs one
  libpage state verb (an option's selectedness by node, which a multiple
  select needs where `selectedIndex` is one index).
- Size: M.

Oracle:

- `dom/nodes` (fetched): `Text-constructor.html`, `Comment-constructor.html`,
  `Text-splitText.html`, `rootNode.html`.
- Add `html/semantics/forms/the-select-element`: `common-HTMLOptionsCollection*.html`,
  `select-add*.html`, `select-remove.html`, `select-selectedOptions*.html`,
  `selected-index.html`, `select-value.html`, `select-multiple.html`,
  `select-named-getter.html`, `select-setter-dirtiness.html` and
  `option-selectedness-script-mutation.html`; `customizable-select/` and
  `filterable-select/` (`appearance: base-select`, tentative), the reftests
  and the validity pages (P4) excluded by name.
- Add `html/semantics/forms/the-option-element` and `the-optgroup-element`
  (their `-manual` and rendering pages excluded by name), and
  `html/infrastructure/common-dom-interfaces/collections`
  (`htmloptionscollection.html` here; the rest in P1b and P3).
- Add `svg/types`: `scripted/SVGAnimatedString.html` and
  `SVGElement.ownerSVGElement-01.html` (the `.svg` pages are XML documents,
  which the runner does not run).

Acceptance, over the wire in QEMU as slice 3's was: the React shop's
inventory page sorts with its `<select>`; the Svelte TodoMVC passes its
start-up and mounts; the Vue TodoMVC mounts and survives the app clearing
its new-todo field.

**As built** (MODERN_WEB.md § Slice 0f, as built, is the record), three
departures from the list above. SVGElement is the name and its prototype
only: `className` as an SVGAnimatedString, `ownerSVGElement` and
`viewportElement` wait for slice 6, which Quinn holds, so § Rulings, 4 is
hers to take up. HTMLOptionsCollection was a prototype without an interface
object, waiting for HTMLCollection's, until the page fixes after P1a named
both (MODERN_WEB.md § 0f's page fixes, as built). A select's own indices
(`select[0]`) are a DEBTS.md row. The oracle added the three
`html/semantics/forms` element directories to `tools/wpt_fetch.sh`; `svg/types`
and `html/infrastructure/common-dom-interfaces/collections` were not added.

### P1b — event targets and collections (6 interfaces)

(a) to 91.5% inst. These names outrank P1a's in every corpus measure.

- **`EventTarget`** (79 files, 19 sites; 57 outside the polyfill list; 14
  FAILED files; 47 pages' `instanceof`/`.prototype`): one EventTarget.prototype holding
  `addEventListener`, `removeEventListener` and `dispatchEvent` (`when`,
  Observable's and Chromium-only, stays out as 0e's (e)), inherited by Node.prototype, by each plain
  target's prototype (`d_event_target`'s classes), and by the global's chain;
  and `new EventTarget()`, a plain target of its own class. Svelte's
  `deep_read_state`.
- **`Window`** with it, as the global's interface prototype between the
  global and EventTarget.prototype, which moves 0e's P3c chain item here
  (§ Rulings, 3); `WindowProperties` and named access stay in P3c.
- **`NodeList`** and **`HTMLCollection`** (159 pages'
  `instanceof`/`.prototype` of HTMLCollection): the collection prototype
  split in two, NodeList's without `namedItem` and with
  `forEach`/`entries`/`keys`/`values`; `childNodes` a NodeList, and
  `querySelectorAll` a static one; `children`, `getElementsBy*`, `forms`,
  `images` and `elements` HTMLCollections (`elements` becomes the
  HTMLFormControlsCollection under it in P3).
- **`DOMTokenList`** and **`DOMStringMap`**: named on the prototypes they
  have. core-js's list then finds them and patches what it finds missing,
  which is the point of naming them.
- Size: M.

**Built ahead, in the page fixes** (MODERN_WEB.md § 0f's page fixes, as
built): `NodeList`, `HTMLCollection`, `DOMTokenList` and `DOMStringMap`
named on the prototypes they have, `childNodes` and `getElementsByName`
NodeLists over NodeList's prototype, the rest HTMLCollections. `EventTarget`
and `Window` are what P1b has left.

Oracle: `dom/events` (fetched) `EventTarget-constructible.any.js` and
`EventTarget-window-constructor.html`; add `dom/collections` and `dom/lists`;
`html/infrastructure/common-dom-interfaces/collections` (`domstringlist.html`
excluded by name: DOMStringList is not built); `html/browsers/the-window-object`
(fetched) `window-prototype-chain.html`, whose WindowProperties tests wait for
P3c. Acceptance: the records' EventTarget and NodeList FAILED lines go.

**As built** (MODERN_WEB.md § 0f's P1b, as built, is the record), as
listed, with the chain's levels present whatever the switch says and only
the names behind it. The interface objects follow their prototypes, so
`Node`, `AbortSignal` and every other interface whose prototype inherits
EventTarget.prototype inherit `EventTarget`. The oracle's two fetched
directories were enough; `dom/collections`, `dom/lists` and the
collections directory were not added.

### P2 — the text controls' selection (12 members)

(b) to 32.3% distinctive.

- `selectionStart` (193 pages), `selectionEnd` (166), `setSelectionRange`
  (153), `select()`, `setRangeText` and `selectionDirection`, on input and
  textarea. Each applies to the input types HTML says (text, search, url,
  tel, password) and answers null or throws InvalidStateError on the rest.
- Machinery: a selection kept per text control in libpage's control state
  (start, end, direction, in UTF-16 code units, moved by value changes as
  HTML's steps say), which yonder's field widget reads and writes through a
  host hook (its widgets keep a caret and a selection already, in bytes).
- React's select event plugin takes its field branch, so typing in the
  shop's login field no longer reaches `window.getSelection` (acceptance).
- Size: M.

**Its core built ahead, in the page fixes** (MODERN_WEB.md § 0f's page
fixes, as built), because the shop's login fields typed nothing without it:
`selectionStart`, `selectionEnd`, `selectionDirection`,
`setSelectionRange` and `select()` over a selection kept in libpage's
control state, and `getSelection()`'s Selection, which holds no range. Not
built: `setRangeText`, the host hook (yonder's field caret neither moves
with the record nor is read into it, a DEBTS.md row), and the oracle below.

Oracle: add `html/semantics/forms/textfieldselection`, and
`html/semantics/forms/the-input-element` `selection.html` (the directory is
added in full by P3; its other pages wait there).

**As built** (MODERN_WEB.md § 0f's P2, as built, is the record), as listed,
and with it HTML's select event, which three of the oracle's pages wait on,
and the rules for where a selection begins (0) and how a value moves it.
`the-input-element` was added whole now, its expectations P3's to grow, and
`onselect` waits on the handler mask (DEBTS.md).

### P3 — the controls' reflections, form owner and labels (86 members, 6 interfaces)

(b) to 87.6% distinctive, 95.5% all names. Likely two PRs: the controls'
members, then the form, fieldset, label, legend and output.

- The reflections and state of input, textarea, select and button
  (`defaultValue` 300 pages, `defaultChecked`, `placeholder`, `readOnly`,
  `maxLength`, `minLength`, `min`, `max`, `step`, `pattern`, `accept`, `alt`,
  `size`, `src`, `width`, `height`, `disabled`, `required`, `autocomplete`,
  `dirName`, the `form*` overrides, textarea's `cols`, `rows`, `wrap` and
  `textLength`), `indeterminate` (an IDL flag with no attribute, which
  yonder's checkbox must then draw: class r), and each control's `form` (its
  form owner, libpage's) and `labels` (a NodeList).
- The form's `action`, `method`, `enctype`, `encoding`, `target`,
  `acceptCharset`, `noValidate`, `autocomplete`, `rel`, `relList`, `length`,
  and its indexed and named getters (`form[0]`, `form.q`): a legacy platform
  object over its controls, storage.c's pattern.
- `HTMLFormControlsCollection` (`namedItem` answering a `RadioNodeList` for
  a name several controls share) and `RadioNodeList` (`value`); the classic
  fix that `elements` lists fieldsets and outputs.
- `HTMLLabelElement` (`htmlFor`, `control`, `form`), `HTMLFieldSetElement`
  (`disabled`, `name`, `type`, `form`, `elements`), `HTMLLegendElement`
  (`form`), `HTMLOutputElement` (`value`, `defaultValue`, `htmlFor` as a
  DOMTokenList, `name`, `type`, `form`, `labels`), each a kind of its own.
- Size: L.

Oracle: `html/dom/reflection-forms.html` (fetched, excluded by name in
`tools/wpt/expected/html/dom.txt` "the phase whose oracle names this page
runs it": this phase); add `html/semantics/forms/form-control-infrastructure`,
`the-input-element` (its picker, file, color and rendering pages excluded by
name), `the-textarea-element`, `the-button-element` (its `commandfor` pages
excluded), `the-form-element`, `the-label-element` (its click pages
excluded by name where they need `test_driver`), `the-fieldset-element`,
`the-legend-element` and `the-output-element`; and
`htmlformcontrolscollection.html` and `radionodelist.html` from P1a's
collections directory.

**As built, P3a** (MODERN_WEB.md § 0f's P3a, as built, is the record): the
first bullet whole, the controls' reflections as one table of HTML's
rules, with `indeterminate` libpage's and drawn by yonder, and each
control's `form` and `labels`; and two form-owner rules WPT's
`form_attribute.html` found wrong (an empty `form` attribute, a control in
no document). `reflection-forms.html` and four of the oracle's directories
run now, nothing excluded. P3b is the rest: the form's members and its
collections, and the label, fieldset, legend and output, whose `htmlFor`
and `control` most of `the-label-element`'s failures wait on.

### P4 — constraint validation, whole (49 members and `ValidityState`)

(b) to 99.6% distinctive.

- `ValidityState` (its 11 flags), and `willValidate`, `validity`,
  `validationMessage`, `checkValidity`, `reportValidity` and
  `setCustomValidity` on input, select, textarea, button, fieldset and
  output; the form's `checkValidity` and `reportValidity`; the `invalid`
  event with its `oninvalid` handler, which moves here from 0e's P5
  (§ Rulings, 7).
- Machinery: libpage judges `valueMissing` and `tooLong`/`tooShort` today
  (`submit.c` `p_validate`); this adds `typeMismatch` (email, url),
  `patternMismatch` (HTML's pattern is an ECMAScript regular expression in
  `v` mode, so the binding compiles it with the engine's), `rangeUnderflow`,
  `rangeOverflow`, `stepMismatch` and `badInput` (over libpage's number and
  date grammars), and `customError` with a custom validity message kept per
  control, which submission then obeys too. `reportValidity` reports through
  what yonder does for a refused submission now: focus the control and say
  the reason on the status line (§ Rulings, 5).
- Size: L.

Oracle: add `html/semantics/forms/constraints` (its `-manual` pages
excluded by name), and the `*-validity*`, `*checkvalidity*`,
`*setcustomvalidity*`, `*validationmessage*` and `*willvalidate*` pages of
the element directories P1a and P3 add.

### P5 — numbers and dates (4 members)

- `valueAsNumber`, `valueAsDate`, `stepUp`, `stepDown`, over libpage's
  `number.c` and `range.c`. Size: S.

Oracle: `the-input-element`'s `input-valueasnumber*.html`,
`input-valueasdate*.html`, `input-stepup.html` and `input-stepdown*.html`.

### P6 — the element kinds whose members are reflections (36 interfaces)

(a) to 94.9% inst.

- `HTMLMetaElement` first (9 FAILED files; its `name` is also what
  testharness.js reads for a long timeout, which slice 0a found the binding
  lacks), then
  `HTMLTemplateElement` (37 pages' `instanceof`/`.prototype`, with `content`),
  `HTMLDivElement`, `HTMLUnknownElement`, `HTMLStyleElement` (its
  reflections; `sheet` stays CSSOM's, as link's does), `HTMLCanvasElement`
  (`width` and `height`), `HTMLBodyElement` (its window-reflecting handlers
  exactly as window has them, the handler hazard's rule), the text-level and
  grouping elements, `HTMLBaseElement`, `HTMLTitleElement`, the table
  elements (whose `rows`, `insertRow` and the rest are tree verbs, the
  largest part) and the obsolete `HTMLFontElement`, `HTMLDirectoryElement`
  and `HTMLMenuElement`. Appendix A lists all 36.
- Machinery: a kind per tag in `node_prototype_kind`, which wants a table
  rather than the `if` chain it is today.
- Size: M, the tables most of it.

Oracle: `html/dom` (fetched) `reflection-grouping.html`, `reflection-text.html`,
`reflection-sections.html`, `reflection-metadata.html`, `reflection-tabular.html`,
`reflection-misc.html` and `reflection-obsolete.html`; add
`html/semantics/tabular-data`, `html/semantics/scripting-1/the-template-element`
and `html/semantics/document-metadata/the-meta-element`.

### P7 — the window's own objects (3 interfaces)

- `Location` (34 FAILED files, one site), `History`, `Navigator`: a
  prototype each for the objects window hands out, with the members they
  have where HTML puts them (Location's are `[LegacyUnforgeable]`, on the
  instance). Size: S. Oracle: `html/browsers/history/the-location-interface`
  (add) and the `?exclude=(Document|Window|HTML.+)` variant of
  `html/dom/idlharness.https.html`, which holds their subtests and is
  excluded by name today.

### P8 — the event interfaces (7 interfaces)

- `UIEvent`, `MouseEvent` (5 FAILED files; 58 pages' `instanceof`/`.prototype`),
  `KeyboardEvent` (4), `FocusEvent`, `InputEvent`, `SubmitEvent` and
  `CompositionEvent`: prototypes for the events yonder already dispatches,
  their data moved to accessors, with constructors; `createEvent`'s table
  then makes each as itself (`event.c`'s C_UNBUILT rows). `PointerEvent` is
  not here: its existence makes pointer libraries stop listening for mouse
  events (0e's handler hazard). Size: M.
- Oracle: add `uievents/constructors`.

### What the plan leaves, and why

- **The APIs whose interface objects pages ask for most**, none a prototype
  question: `MutationObserver` (364 files; slice 5a), `PerformanceObserver`
  (216; a DEBTS row), `MessageChannel` and `MessagePort` (110; 54.38% of
  Chrome page loads construct one; P2b left ports out), `CSS` (101 files, 87
  FAILED; a DEBTS row), `IntersectionObserver` (77; 57.27%) and
  `ResizeObserver` (30; 45.46%), the streams (`ReadableStream` 48 files, 28
  FAILED), `WebSocket` (52), `Worker` (31), `BroadcastChannel`, `ShadowRoot`
  (212 pages' `instanceof`/`.prototype`, and only 18 MISSING files: pages
  guard it with `typeof`; shadow DOM),
  `DOMParser` (23 files, 167 asks: cheap, over `implementation.c` and the
  fragment parser, and worth a phase of its own).
- **The SVG subclasses and SVG's types**, 95 interfaces: slice 6.
- **The feature-announcing element interfaces** (§ Classification).
- **`HTMLAllCollection`** (0e's P3c), **`XMLDocument`** (documents of their
  own share Document's prototype today) and **`MathMLElement`**.
- **(b)'s deferred members** (Appendix B, class d): `files` (a FileList needs
  the file picker), `list` and `HTMLDataListElement` (§ Rulings, 6),
  `showPicker`, `webkitdirectory` and `webkitEntries`, `alpha` and
  `colorSpace` (the color picker), `popoverTargetElement`/`Action`,
  `command`/`commandForElement`, and skips: `capture` (a phone's camera hint)
  and the obsolete `align`/`useMap`.

## Coverage by phase

Cumulative. **(a)**: n of the 934 shipped names; **scan** is interface-object
uses in the 2026-10-25 pages (0e's 839 pages in brackets); **inst.** is the
prototype interfaces' `instanceof`/`.prototype` pages (224 interfaces: the
61 present and classes a and p); **MISSING** is the share of file-name pairs
resolved, over all absent shipped names (3,813 pairs) and over the prototype
interfaces outside the polyfill list (246); **FAILED** the share of the 315
FAILED file-name pairs. P2 and P5 add no interface.

| after | (a) n | unw | scan | inst. | MISSING all | MISSING, prototype interfaces outside the list | FAILED |
|---|---:|---:|---:|---:|---:|---:|---:|
| today | 61 | 6.5% | 54.0% (54.6%) | 80.1% (80.8%) | 0.0% | 0.0% | 0.0% |
| P1a | 69 | 7.4% | 55.3% (55.5%) | 83.5% (83.2%) | 0.6% | 8.9% | 1.3% |
| P1b | 75 | 8.0% | 58.6% (58.6%) | 91.5% (91.7%) | 8.7% | 43.5% | 7.6% |
| P3 | 81 | 8.7% | 58.6% (58.8%) | 91.5% (91.7%) | 8.9% | 43.5% | 7.6% |
| P4 | 82 | 8.8% | 58.6% (58.8%) | 91.5% (91.7%) | 8.9% | 43.5% | 7.6% |
| P6 | 118 | 12.6% | 59.8% (59.8%) | 94.9% (94.4%) | 11.0% | 60.6% | 12.4% |
| P7 | 121 | 13.0% | 60.4% (60.4%) | 96.5% (96.3%) | 12.5% | 79.3% | 23.2% |
| P8 | 128 | 13.7% | 62.9% (62.8%) | 99.4% (98.1%) | 13.2% | 89.0% | 26.0% |

**(b)**: n of 215; **distinctive** weights the 110 distinctive members by
their pages; **all names** weights every member, generic ones included.

| after | (b) n | unw | distinctive, 10-25 (0e) | all names, 10-25 (0e) |
|---|---:|---:|---:|---:|
| today | 21 | 9.8% | 4.2% (5.6%) | 23.9% (22.2%) |
| P1a | 45 | 20.9% | 10.0% (13.7%) | 42.8% (40.9%) |
| P2 | 57 | 26.5% | 32.3% (25.7%) | 46.7% (43.7%) |
| P3 | 143 | 66.5% | 87.6% (81.8%) | 95.5% (94.2%) |
| P4 | 192 | 89.3% | 99.6% (99.0%) | 97.0% (96.6%) |
| P5 | 196 | 91.2% | 99.9% (99.4%) | 97.1% (96.6%) |

Reading them:

- (a)'s ceiling is low by construction: 806 of the 934 shipped names are
  APIs, SVG's or features this plan does not own, and most MISSING pairs are
  theirs. The prototype interfaces' columns are the plan's own measure.
- (b)'s P3 jump is many cheap members at once, and P4's is constraint
  validation's distinctive names (`validity`, `checkValidity`), which few
  pages use but which ship as one API.
- The 19 deferred (b) members are the rest of the 215.

## Rulings this needs

The lean for each is stated. All of them are Chris's.

1. **P1a before P1b.** The corpus ranks P1b's names higher (EventTarget's 14
   FAILED files, HTMLCollection's 159 pages' `instanceof`/`.prototype`). The lean is P1a first: three
   named pages die on its names today, and it is the smaller PR; P1b follows
   at once.
2. **An interface object before all its members.** The lean is yes when each
   missing member is detected on its own and the interface announces nothing
   (Slice 0a's practice, made a rule), and no for an element whose behaviour
   yonder lacks (the hazard, § Classification). DIVERGENCES.md needs no line:
   absence of a member is what a page tests.
3. **EventTarget and Window move from 0e's P3c into P1b.** `window
   instanceof EventTarget` has to hold the day `EventTarget` exists, or the
   name lies about window. The lean is to build the global's chain (Window,
   EventTarget) in P1b and leave `WindowProperties` and named access in P3c.
4. **`SVGElement.className` becomes an SVGAnimatedString in P1a.** Today an
   SVG element's `className` is a string, which no browser answers; the
   standard's object breaks code that calls string methods on it, as it does
   in every browser. The lean is the standard, with the corpus replay
   watching for new FAILED lines.
5. **`reportValidity`'s report.** The lean is what yonder does for a refused
   submission now (`form_send_as`: the control focused, the reason on the
   status line), with the control's validation message as the reason. A
   bubble is a later toolkit question.
6. **`HTMLDataListElement` and `input.list` without suggestions.** The lean
   is to wait for yonder's field to offer them: a page that finds datalist
   expects its suggestions shown.
7. **`invalid` and `oninvalid` move from 0e's P5 to P4.** The event is
   constraint validation's own; the lean is to fire it where it is decided.
8. **`getRootNode` moves from 0e's P4a to P1a,** for Vue's `v-model`. The
   lean is yes; Node's other eight missing members stay in P4a.

## What could not be determined

- **Chrome numbers for interface objects**: no counter reads one (§ Usage,
  1). The interface objects are ranked by the corpus alone.
- **Chrome numbers for 209 of the 215 form members**, among them every one
  the three pages ask.
- **Which member a `cannot read property 'length' of undefined` was**: 217
  FAILED lines in the new pull say it without naming the receiver, so how
  many pages die on `select.options` (or `form.length`, or any other absent
  list) cannot be told from the records.
- **Which build wrote each record.** The records name none; the module
  subset is inferred from the last `MISSING script module` and the first
  module failures (§ Usage, 4), so it may hold a record of the older build.
- **Six records the tally cannot judge** (a `MISSING global` line with an
  empty name); they are left out of the MISSING and FAILED counts.
- **The demos' framework versions.** The sources read are npm's current
  releases (react-dom 19.3.1, svelte 5.57.2, @vue/runtime-dom 3.5.44); what
  each page meets after its first ask is read from them, not from the pages'
  own builds.
- **Two of webref's IDL files did not parse** with webidl2 24.5.0
  (`DOM-Style.idl` and `svg-paths.idl`); their interfaces are counted from
  the specs that also define them (`StyleSheetList` from cssom) or not at all
  (`SVGPathSegment`).
- **HTTP Archive per-member usage**, behind BigQuery as in 0e.

## Sources

| source | version / date | used for |
|---|---|---|
| webref `ed/idl` (w3c/webref, main) | commit `5db07db`, 2026-10-25T18:42Z; 354 IDL files, parsed with webidl2 24.5.0 | every member, its spec and kind; what is exposed on Window |
| @mdn/browser-compat-data | 8.1.7, 2026-10-22 | shipped in Chrome, Firefox or Safari, desktop or mobile, without a flag or a prefix and not removed |
| Chrome use counters (chromestatus.com `/data/featurepopularity`) | the day 2026-10-24, fetched 2026-10-25 | § Usage, 1; Appendix A's constructed column |
| webstatus.dev API, with web-features 3.44.0 | as served 2026-10-25 | § Usage, 2 |
| HTTP Archive / Web Almanac | not usable per member | nothing |
| yonder_tally over the 2026-10-25 pull | 873 of 879 records, 137 sites | MISSING; outside the list; FAILED |
| the module subset of that pull | 31 records, 12 sites | Appendix A's module column |
| static scan of the 2026-10-25 kept pages | 825 pages, 128 sites | scan pages, first figure |
| static scan of 0e's three pulls (2026-10-21, -22, -22b) | 839 pages, 163 sites | scan pages, second figure |
| the DOM host probe at 0246cb7e | 2026-10-25 | present, classic/modern, the chains |
| react-dom, svelte, @vue/runtime-dom, core-js (npm) | 19.3.1, 5.57.2, 3.5.44, 3.51.0 | § Usage, 5; the polyfill list |
| WPT | the runner's pin `a699fb16` (directory and page names checked against it) | each phase's oracle |

No site name or address from the corpus appears in this document.

## Appendix A — Interface objects

Every absent shipped interface object that a source names: MISSING in at
least 5 files of 2 sites, 5 pages' `instanceof`/`.prototype` across the two
scans, a FAILED file, a Chrome construction counter, or a phase. Columns:
MISSING files/sites/asks over the 873 records; **outside the list**, the files
of those not among the 65 polyfill records; FAILED files/sites; the module
subset's MISSING (M) and FAILED (F) files; scan pages (`instanceof` and
`.prototype` alone in brackets), the new pull's and 0e's; Chrome's
construction counter; the prototype the binding has; class; phase. Ordered by
FAILED, then MISSING, then the scans.

| interface | spec | MISSING files/sites/asks | outside the list | FAILED files/sites | module subset | scan pages (inst.) 10-25 / 0e | Chrome % (constructed) | prototype today | class | phase | note |
|---|---|---|---|---|---|---|---|---|---|---|---|
| `CSS` | cssom | 101/10/218 | 94 | 87/1 | M 3, F 3 | 79 (0) / 109 (0) |  | no such object | m |  | DEBTS row: no CSS.supports |
| `MutationObserver` | dom | 364/66/995 | 301 | 78/21 | M 16, F 11 | 422 (4) / 414 (19) |  | no such object | m |  | slice 5a (RELAYOUT.md) |
| `Location` | html | 35/2/35 | 35 | 34/1 |  | 35 (35) / 33 (33) |  | a plain object | p | P7 |  |
| `ReadableStream` | streams | 48/16/49 | 35 | 28/8 | M 1, F 1 | 63 (26) / 91 (18) | 16.63 `ReadableStreamConstructor` | no such object | m |  | streams |
| `EventTarget` | dom | 79/19/133 | 57 | 14/5 | M 1 | 95 (47) / 67 (38) |  | no level: each target carries its own methods | p | P1b |  |
| `IntersectionObserver` | intersection-observer | 77/25/137 | 50 | 11/8 | M 4 | 246 (32) / 229 (1) | 57.27 `IntersectionObserver_Constructor` | no such object | r |  | layout observation |
| `TransformStream` | streams | 17/5/17 | 6 | 9/2 |  | 5 (0) / 5 (0) | 0.83 `TransformStreamConstructor` | no such object | m |  | streams |
| `HTMLMetaElement` | html | 9/2/9 |  | 9/2 |  | 2 (2) / 2 (2) |  | shares HTMLElement's | p | P6 | its members are reflections, or text the tree has |
| `ResizeObserver` | resize-observer | 30/12/33 | 21 | 8/3 |  | 195 (0) / 153 (0) | 45.46 `ResizeObserver_Constructor` | no such object | r |  | layout observation |
| `HTMLCanvasElement` | html | 16/5/35 | 14 | 6/1 |  | 16 (16) / 22 (20) |  | shares HTMLElement's | p | P6 | width and height only: getContext and the rest are canvas's, which pages detect by getContext itself; a toBlob polyfill reads its prototype unguarded |
| `ShadowRoot` | dom | 18/5/25 | 16 | 5/1 | M 3 | 212 (212) / 279 (279) |  | no such object | m |  | shadow DOM (attachShadow) |
| `MouseEvent` | pointerevents | 13/7/23 | 10 | 5/2 |  | 132 (58) / 187 (32) |  | events are Event's, with own data properties | p | P8 |  |
| `NodeList` | dom | 76/31/106 | 11 | 4/2 |  | 36 (36) / 62 (61) |  | one collection prototype for both; querySelectorAll a plain object | p | P1b |  |
| `KeyboardEvent` | uievents | 9/3/114 | 7 | 4/2 |  | 77 (25) / 50 (19) |  | events are Event's, with own data properties | p | P8 |  |
| `Worker` | html | 31/19/41 | 19 | 3/3 | M 1 | 99 (0) / 131 (0) |  | no such object | d |  | workers |
| `PerformanceObserver` | performance-timeline | 216/23/335 | 198 | 2/1 | M 8 | 341 (5) / 333 (4) |  | no such object | m |  | DEBTS row: no PerformanceObserver |
| `Window` | html | 15/7/18 | 8 | 2/2 | M 1 | 19 (19) / 34 (31) |  | the global has none (P3c) | p | P1b |  |
| `SVGElement` | SVG | 13/4/13 | 11 | 2/2 | M 1, F 1 | 96 (87) / 62 (61) |  | an SVG element has Element's | p | P1a |  |
| `Text` | dom | 5/3/5 | 5 | 2/1 | M 2, F 2 | 9 (9) / 11 (11) |  | shares CharacterData's | p | P1a |  |
| `MessageChannel` | html | 110/39/161 | 47 | 1/1 | M 4 | 158 (0) / 144 (0) | 54.38 `V8MessageChannel_Constructor` | no such object | m |  | messaging: P2b left ports out |
| `Audio` | html | 23/4/24 | 21 | 1/1 |  | 37 (6) / 12 (1) |  | shares HTMLElement's | d |  | announces media: joins with that feature |
| `DOMTokenList` | dom | 71/27/98 | 6 |  |  | 4 (4) / 6 (6) |  | its own (classList, relList): name it | a | P1b |  |
| `HTMLCollection` | dom | 68/27/86 | 3 |  |  | 159 (159) / 173 (173) |  | one collection prototype for both | p | P1b |  |
| `TextTrackList` | html | 68/29/92 | 3 |  | M 1 | 0 (0) / 0 (0) |  | no such object | d |  | with its API |
| `PluginArray` | html | 67/28/84 | 2 |  |  | 11 (11) / 8 (8) |  | no such object | d |  | with its API |
| `MimeTypeArray` | html | 67/28/84 | 2 |  |  | 10 (10) / 8 (8) |  | no such object | d |  | with its API |
| `FileList` | FileAPI | 65/27/82 |  |  |  | 10 (10) / 10 (6) |  | no such object | d |  | the file picker |
| `Plugin` | html | 65/27/82 |  |  |  | 4 (1) / 0 (0) |  | no such object | d |  | with its API |
| `SVGLengthList` | SVG | 65/27/82 |  |  |  | 0 (0) / 1 (1) |  | no such object | p | slice 6 | slice 6 (SVG.md) |
| `SVGNumberList` | SVG | 65/27/82 |  |  |  | 0 (0) / 1 (1) |  | no such object | p | slice 6 | slice 6 (SVG.md) |
| `SVGStringList` | SVG | 65/27/82 |  |  |  | 0 (0) / 1 (1) |  | no such object | p | slice 6 | slice 6 (SVG.md) |
| `SVGTransformList` | SVG | 65/27/82 |  |  |  | 0 (0) / 1 (1) |  | no such object | p | slice 6 | slice 6 (SVG.md) |
| `CSSRuleList` | cssom | 65/27/82 |  |  |  | 0 (0) / 0 (0) |  | no such object | m |  | CSSOM sheets |
| `DOMRectList` | geometry | 65/27/82 |  |  |  | 0 (0) / 0 (0) |  | no such object | a |  | getClientRects answers a plain list |
| `DOMStringList` | html | 65/27/82 |  |  |  | 0 (0) / 0 (0) |  | no such object | d |  | with its API |
| `DataTransferItemList` | html | 65/27/82 |  |  |  | 0 (0) / 0 (0) |  | no such object | d |  | with its API |
| `HTMLAllCollection` | html | 65/27/82 |  |  |  | 0 (0) / 0 (0) |  | no such object | p |  | P3c of WINDOW_DOCUMENT (document.all) |
| `MediaList` | cssom | 65/27/82 |  |  |  | 0 (0) / 0 (0) |  | no such object | m |  | CSSOM sheets |
| `SVGPointList` | SVG | 65/27/82 |  |  |  | 0 (0) / 0 (0) |  | no such object | p | slice 6 | slice 6 (SVG.md) |
| `SourceBufferList` | media-source | 65/27/82 |  |  |  | 0 (0) / 0 (0) |  | no such object | d |  | with its API |
| `StyleSheetList` | cssom | 65/27/82 |  |  |  | 0 (0) / 0 (0) |  | no such object | m |  | CSSOM sheets |
| `TextTrackCueList` | html | 65/27/82 |  |  |  | 0 (0) / 0 (0) |  | no such object | d |  | with its API |
| `TouchList` | touch-events | 65/27/82 |  |  |  | 0 (0) / 0 (0) |  | no such object | d |  | with its API |
| `WebSocket` | websockets | 52/13/558 | 43 |  | M 1 | 45 (1) / 33 (1) |  | no such object | m |  | a WebSocket client |
| `PointerEvent` | pointerevents | 38/17/112 | 23 |  |  | 41 (3) / 121 (12) |  | no such object | d |  | the pointer-events hazard (WINDOW_DOCUMENT § The handler hazard) |
| `CompressionStream` | compression | 34/4/35 | 34 |  |  | 124 (0) / 145 (0) | 23.87 `CompressionStreamConstructor` | no such object | m |  | streams over libgzip |
| `BroadcastChannel` | html | 26/8/29 | 15 |  | M 4 | 57 (0) / 50 (0) | 15.39 `V8BroadcastChannel_Constructor` | no such object | m |  | messaging between windows |
| `Notification` | notifications | 24/14/49 | 8 |  | M 1 | 16 (0) / 32 (0) |  | no such object | d |  | skip: no notifications |
| `DOMParser` | html | 23/10/167 | 15 |  |  | 86 (7) / 84 (7) |  | no such object | a |  | implementation.c and the fragment parser |
| `FileReader` | FileAPI | 21/10/21 | 9 |  | M 1 | 58 (4) / 35 (0) |  | no such object | m |  | over Blob (slice 2) |
| `Navigator` | html | 16/9/48 | 8 |  |  | 3 (3) / 7 (4) |  | a plain object | p | P7 |  |
| `HTMLUnknownElement` | html | 16/7/28 | 7 |  | M 1 | 6 (6) / 0 (0) |  | shares HTMLElement's | p | P6 | its members are reflections, or text the tree has |
| `RTCPeerConnection` | webrtc | 16/9/42 | 11 |  |  | 5 (0) / 42 (1) |  | no such object | d |  | with its API |
| `EventSource` | html | 15/6/15 | 6 |  | M 1 | 7 (0) / 23 (0) |  | no such object | d |  | with its API |
| `DeviceOrientationEvent` | orientation-event | 14/7/47 | 9 |  |  | 4 (0) / 10 (0) |  | no such object | d |  | with its API |
| `SharedWorker` | html | 14/5/14 | 6 |  | M 1 | 6 (0) / 29 (0) |  | no such object | d |  | workers |
| `TouchEvent` | touch-events | 12/7/47 | 5 |  |  | 4 (4) / 7 (7) |  | no such object | d |  | skip: touch reads as a phone |
| `HTMLTemplateElement` | html | 11/6/11 | 4 |  |  | 47 (37) / 18 (8) |  | shares HTMLElement's | p | P6 | its members are reflections, or text the tree has |
| `AudioContext` | webaudio | 11/6/24 | 5 |  |  | 45 (0) / 16 (0) | 5.64 `V8AudioContext_Constructor` | no such object | d |  | with its API |
| `TextTrackCue` | html | 11/6/11 | 3 |  | M 1 | 1 (0) / 0 (0) |  | no such object | d |  | with its API |
| `IDBTransaction` | IndexedDB | 10/5/10 | 3 |  | M 1 | 8 (8) / 16 (16) |  | no such object | d |  | with its API |
| `IDBDatabase` | IndexedDB | 10/5/10 | 3 |  | M 1 | 7 (7) / 14 (14) |  | no such object | d |  | with its API |
| `IDBRequest` | IndexedDB | 10/5/10 | 3 |  | M 1 | 7 (7) / 14 (14) |  | no such object | d |  | with its API |
| `HTMLDialogElement` | html | 10/4/13 | 8 |  | M 3 | 22 (8) / 4 (4) |  | shares HTMLElement's | d |  | announces dialog (the corpus has code that calls showModal when the interface exists): joins with that feature |
| `OffscreenCanvas` | html | 10/6/17 | 6 |  |  | 10 (1) / 23 (7) |  | no such object | r |  | canvas |
| `CanvasRenderingContext2D` | html | 10/4/22 | 8 |  |  | 4 (4) / 2 (1) |  | no such object | r |  | canvas |
| `MessagePort` | html | 10/5/10 | 3 |  | M 1 | 1 (0) / 1 (1) |  | no such object | m |  | messaging |
| `AudioTrackList` | html | 10/5/10 | 3 |  | M 1 | 0 (0) / 0 (0) |  | no such object | d |  | with its API |
| `ChannelMergerNode` | webaudio | 10/5/10 | 3 |  | M 1 | 0 (0) / 0 (0) |  | no such object | d |  | with its API |
| `PublicKeyCredential` | webauthn | 10/8/13 | 6 |  |  | 7 (0) / 2 (0) |  | no such object | d |  | with its API |
| `PushManager` | push-api | 10/5/10 | 5 |  |  | 13 (0) / 4 (0) |  | no such object | d |  | with its API |
| `TextTrack` | html | 10/5/10 | 3 |  | M 1 | 0 (0) / 0 (0) |  | no such object | d |  | with its API |
| `HTMLPictureElement` | html | 9/4/9 | 5 |  |  | 1 (0) / 10 (10) |  | shares HTMLElement's | d |  | announces picture source selection: joins with that feature |
| `WebGLRenderingContext` | webgl1 | 9/4/12 | 7 |  |  | 24 (8) / 25 (1) |  | no such object | r |  | canvas |
| `AuthenticatorAssertionResponse` | webauthn | 9/7/10 | 5 |  |  | 0 (0) / 0 (0) |  | no such object | d |  | with its API |
| `AuthenticatorAttestationResponse` | webauthn | 9/7/10 | 5 |  |  | 0 (0) / 0 (0) |  | no such object | d |  | with its API |
| `AuthenticatorResponse` | webauthn | 9/7/10 | 5 |  |  | 0 (0) / 0 (0) |  | no such object | d |  | with its API |
| `DeviceMotionEvent` | orientation-event | 9/6/42 | 6 |  |  | 4 (0) / 4 (0) |  | no such object | d |  | with its API |
| `FontFace` | css-font-loading | 9/5/10 | 5 |  |  | 7 (0) / 14 (0) | 8.46 `FontFaceConstructor` | no such object | d |  | with its API |
| `XPathResult` | dom | 9/8/41 | 4 |  |  | 0 (0) / 0 (0) |  | no such object | d |  | with its API |
| `Animation` | web-animations | 8/3/8 | 6 |  |  | 11 (1) / 26 (8) | 0.27 `V8Animation_Constructor` | no such object | d |  | with its API |
| `HTMLMediaElement` | html | 8/3/8 | 6 |  |  | 0 (0) / 3 (3) |  | shares HTMLElement's | d |  | announces media: joins with that feature |
| `DOMMatrixReadOnly` | geometry | 8/3/8 | 6 |  |  | 4 (0) / 9 (0) |  | no such object | m |  | geometry |
| `MediaStream` | mediacapture-streams | 8/3/8 | 6 |  |  | 1 (0) / 0 (0) |  | no such object | d |  | with its API |
| `OfflineAudioContext` | webaudio | 8/7/10 | 3 |  |  | 5 (0) / 41 (0) | 6.95 `V8OfflineAudioContext_Constructor` | no such object | d |  | with its API |
| `PaymentRequest` | payment-request | 8/4/11 | 5 |  |  | 6 (0) / 8 (0) |  | no such object | d |  | with its API |
| `Permissions` | permissions | 8/3/8 | 6 |  |  | 0 (0) / 0 (0) |  | no such object | d |  | with its API |
| `PerformanceEventTiming` | event-timing | 7/3/7 | 3 |  |  | 100 (48) / 174 (98) |  | no such object | d |  | event timing |
| `CSSStyleSheet` | cssom | 7/4/8 | 4 |  | M 3 | 37 (34) / 16 (16) |  | no such object | m |  | CSSOM sheets |
| `MediaSource` | media-source | 7/3/13 | 4 |  |  | 10 (7) / 22 (5) |  | no such object | d |  | with its API |
| `WebKitCSSMatrix` | geometry | 7/3/7 | 4 |  |  | 0 (0) / 1 (0) |  | no such object | d |  | with its API |
| `HTMLFieldSetElement` | html | 6/2/6 | 4 |  |  | 2 (2) / 9 (9) |  | no such object | d | P3 | with its API |
| `HTMLMeterElement` | html | 6/2/6 | 4 |  |  | 1 (1) / 1 (1) |  | shares HTMLElement's | d |  | announces meter rendering: joins with that feature |
| `HTMLProgressElement` | html | 6/2/6 | 4 |  |  | 1 (1) / 1 (1) |  | shares HTMLElement's | d |  | announces progress rendering: joins with that feature |
| `HTMLDataListElement` | html | 6/2/6 | 4 |  |  | 1 (1) / 0 (0) |  | shares HTMLElement's | d |  | announces datalist suggestions (§ Rulings): joins with that feature |
| `HTMLTimeElement` | html | 6/2/11 | 4 |  |  | 1 (1) / 0 (0) |  | shares HTMLElement's | p | P6 | its members are reflections, or text the tree has |
| `ContentIndex` | content-index | 6/5/7 | 2 |  |  | 0 (0) / 0 (0) |  | no such object | d |  | with its API |
| `MediaMetadata` | mediasession | 6/5/7 | 2 |  |  | 2 (0) / 1 (0) |  | no such object | d |  | with its API |
| `MediaStreamTrack` | mediacapture-streams | 6/2/6 | 4 |  |  | 0 (0) / 0 (0) |  | no such object | d |  | with its API |
| `WritableStream` | streams | 6/2/6 | 4 |  |  | 16 (0) / 8 (0) | 0.56 `WritableStreamConstructor` | no such object | m |  | streams |
| `History` | html | 5/1/5 | 3 |  |  | 17 (17) / 31 (31) |  | a plain object | p | P7 |  |
| `HTMLDivElement` | html | 5/1/15 | 3 |  |  | 30 (30) / 15 (15) |  | shares HTMLElement's | p | P6 | its members are reflections, or text the tree has |
| `PerformanceResourceTiming` | resource-timing | 5/1/10 | 3 |  |  | 20 (20) / 42 (18) |  | no such object | m |  | DEBTS row: no resource entries |
| `HTMLVideoElement` | html | 5/1/30 | 3 |  |  | 9 (8) / 14 (13) |  | shares HTMLElement's | d |  | announces media: joins with that feature |
| `MathMLElement` | mathml-core | 5/1/5 | 3 |  |  | 6 (6) / 13 (13) |  | no such object | p |  | a MathML element has Element's |
| `HTMLBodyElement` | html | 5/1/20 | 3 |  |  | 3 (3) / 11 (11) |  | shares HTMLElement's | p | P6 | its members are reflections, or text the tree has |
| `DragEvent` | html | 5/1/5 | 3 |  |  | 3 (3) / 6 (6) |  | no such object | d |  | with its API |
| `Range` | dom | 5/1/5 | 3 |  |  | 1 (0) / 7 (7) |  | no such object | m |  | Selection and Range |
| `PageTransitionEvent` | html | 5/1/5 | 3 |  |  | 0 (0) / 6 (6) |  | no such object | d |  | with its API |
| `HTMLOListElement` | html | 5/1/5 | 3 |  |  | 0 (0) / 3 (3) |  | shares HTMLElement's | p | P6 | its members are reflections, or text the tree has |
| `HTMLHeadElement` | html | 5/1/5 | 3 |  |  | 0 (0) / 2 (2) |  | shares HTMLElement's | p | P6 | its members are reflections, or text the tree has |
| `Accelerometer` | accelerometer | 5/1/5 | 3 |  |  | 2 (0) / 0 (0) | 0.079 `AccelerometerConstructor` | no such object | d |  | with its API |
| `Gyroscope` | gyroscope | 5/1/5 | 3 |  |  | 2 (0) / 0 (0) | 0.30 `GyroscopeConstructor` | no such object | d |  | with its API |
| `Path2D` | html | 5/4/5 | 5 |  |  | 28 (0) / 10 (0) |  | no such object | r |  | canvas |
| `ScrollTimeline` | scroll-animations | 5/1/5 | 3 |  |  | 3 (0) / 1 (0) | 0.081 `ScrollTimelineConstructor` | no such object | d |  | with its API |
| `SpeechRecognition` | speech-api | 5/1/5 | 3 |  |  | 1 (0) / 0 (0) | 0.90 `V8SpeechRecognition_Constructor` | no such object | d |  | with its API |
| `UIEvent` | uievents | 5/4/5 | 4 |  |  | 2 (0) / 0 (0) |  | events are Event's, with own data properties | p | P8 |  |
| `URLPattern` | urlpattern | 5/1/10 | 3 |  |  | 0 (0) / 0 (0) | 2.06 `V8URLPattern_Constructor` | no such object | d |  | with its API |
| `HTMLSlotElement` | html | 4/1/4 | 4 |  |  | 12 (4) / 17 (12) |  | shares HTMLElement's | d |  | announces shadow DOM (the corpus has a polyfill that picks its path by the interface): joins with that feature |
| `PerformanceNavigationTiming` | navigation-timing | 4/3/12 | 4 |  |  | 10 (5) / 16 (0) |  | no such object | d |  | with its API |
| `Comment` | dom | 3/2/3 | 3 |  |  | 16 (15) / 10 (10) |  | shares CharacterData's | p | P1a |  |
| `SubmitEvent` | html | 3/2/3 | 3 |  |  | 8 (7) / 6 (4) |  | events are Event's | p | P8 |  |
| `AudioWorkletNode` | webaudio | 3/3/3 | 1 |  |  | 4 (0) / 7 (0) | 0.11 `AudioWorkletNodeConstructor` | no such object | d |  | with its API |
| `Option` | html | 3/2/3 | 3 |  |  | 2 (0) / 7 (0) |  | with HTMLOptionElement's | p | P1a |  |
| `TextDecoderStream` | encoding | 3/2/3 | 3 |  |  | 0 (0) / 0 (0) | 8.79 `TextDecoderStreamConstructor` | no such object | d |  | with its API |
| `TextEncoderStream` | encoding | 3/2/3 | 3 |  |  | 0 (0) / 0 (0) | 5.67 `TextEncoderStreamConstructor` | no such object | d |  | with its API |
| `CSSMediaRule` | css-conditional | 1/1/1 | 1 |  |  | 25 (25) / 0 (0) |  | no such object | d |  | with its API |
| `SVGGraphicsElement` | SVG | 1/1/1 |  |  |  | 7 (5) / 9 (2) |  | an SVG element has Element's | p | slice 6 | slice 6 (SVG.md): its members are the renderer's |
| `HTMLOutputElement` | html | 1/1/1 | 1 |  |  | 1 (1) / 3 (3) |  | no such object | d | P3 | with its API |
| `HTMLDataElement` | html | 1/1/1 | 1 |  |  | 1 (1) / 0 (0) |  | shares HTMLElement's | p | P6 | its members are reflections, or text the tree has |
| `PresentationRequest` | presentation-api | 1/1/1 | 1 |  |  | 1 (0) / 0 (0) | 5.67 `PresentationRequestConstructor` | no such object | d |  | with its API |
| `RTCIceTransport` | webrtc | 1/1/1 | 1 |  |  | 1 (0) / 0 (0) | 0.016 `V8RTCIceTransport_Constructor` | no such object | d |  | with its API |
| `CSSStyleRule` | cssom |  |  |  |  | 54 (54) / 16 (16) |  | no such object | d |  | with its API |
| `ServiceWorkerRegistration` | service-workers |  |  |  |  | 6 (1) / 44 (39) |  | no such object | d |  | with its API |
| `HTMLStyleElement` | html |  |  |  |  | 6 (6) / 19 (19) |  | shares HTMLElement's | p | P6 | its members are reflections, or text the tree has |
| `IDBCursor` | IndexedDB |  |  |  |  | 7 (7) / 14 (14) |  | no such object | d |  | with its API |
| `MimeType` | html |  |  |  |  | 10 (10) / 8 (8) |  | no such object | d |  | with its API |
| `HTMLLIElement` | html |  |  |  |  | 7 (7) / 5 (5) |  | shares HTMLElement's | p | P6 | its members are reflections, or text the tree has |
| `BaseAudioContext` | webaudio |  |  |  |  | 5 (5) / 4 (4) |  | no such object | d |  | with its API |
| `FileSystemDirectoryEntry` | entries-api |  |  |  |  | 5 (5) / 4 (4) |  | no such object | d |  | with its API |
| `FileSystemEntry` | entries-api |  |  |  |  | 5 (5) / 4 (4) |  | no such object | d |  | with its API |
| `FileSystemFileEntry` | entries-api |  |  |  |  | 5 (5) / 4 (4) |  | no such object | d |  | with its API |
| `InputEvent` | uievents |  |  |  |  | 4 (0) / 11 (9) |  | events are Event's | p | P8 |  |
| `MediaSession` | mediasession |  |  |  |  | 5 (5) / 4 (4) |  | no such object | d |  | with its API |
| `HTMLLabelElement` | html |  |  |  |  | 0 (0) / 7 (7) |  | no such object | d | P3 | with its API |
| `ImageData` | html |  |  |  |  | 26 (3) / 49 (4) |  | no such object | d |  | with its API |
| `CompositionEvent` | uievents |  |  |  |  | 5 (5) / 0 (0) |  | no such event | p | P8 |  |
| `HTMLOptionElement` | html |  |  |  |  | 2 (2) / 3 (3) |  | shares HTMLElement's | p | P1a |  |
| `Selection` | selection-api |  |  |  |  | 0 (0) / 5 (5) |  | no such object | m |  | Selection and Range |
| `FocusEvent` | uievents |  |  |  |  | 9 (0) / 5 (3) |  | events are Event's | p | P8 |  |
| `HTMLLegendElement` | html |  |  |  |  | 0 (0) / 2 (2) |  | no such object | d | P3 | with its API |
| `SVGAnimatedString` | SVG |  |  |  |  | 1 (1) / 1 (1) |  | none (className reads a string) | p | P1a |  |
| `HTMLAreaElement` | html |  |  |  |  | 0 (0) / 1 (1) |  | shares HTMLElement's | p | P6 | its members are reflections, or text the tree has |
| `HTMLBaseElement` | html |  |  |  |  | 0 (0) / 1 (1) |  | shares HTMLElement's | p | P6 | its members are reflections, or text the tree has |
| `HTMLMapElement` | html |  |  |  |  | 0 (0) / 1 (1) |  | shares HTMLElement's | p | P6 | its members are reflections, or text the tree has |
| `HTMLModElement` | html |  |  |  |  | 0 (0) / 1 (1) |  | shares HTMLElement's | p | P6 | its members are reflections, or text the tree has |
| `HTMLOptGroupElement` | html |  |  |  |  | 0 (0) / 1 (1) |  | shares HTMLElement's | p | P1a |  |
| `HTMLQuoteElement` | html |  |  |  |  | 0 (0) / 1 (1) |  | shares HTMLElement's | p | P6 | its members are reflections, or text the tree has |
| `HTMLTableCellElement` | html |  |  |  |  | 0 (0) / 1 (1) |  | shares HTMLElement's | p | P6 | its members are reflections, or text the tree has |
| `HTMLTableColElement` | html |  |  |  |  | 0 (0) / 1 (1) |  | shares HTMLElement's | p | P6 | its members are reflections, or text the tree has |
| `HTMLTableElement` | html |  |  |  |  | 0 (0) / 1 (1) |  | shares HTMLElement's | p | P6 | its members are reflections, or text the tree has |
| `HTMLTableRowElement` | html |  |  |  |  | 0 (0) / 1 (1) |  | shares HTMLElement's | p | P6 | its members are reflections, or text the tree has |
| `HTMLTableSectionElement` | html |  |  |  |  | 0 (0) / 1 (1) |  | shares HTMLElement's | p | P6 | its members are reflections, or text the tree has |
| `HTMLUListElement` | html |  |  |  |  | 1 (1) / 0 (0) |  | shares HTMLElement's | p | P6 | its members are reflections, or text the tree has |
| `OscillatorNode` | webaudio |  |  |  |  | 0 (0) / 1 (1) | 0.01 `V8OscillatorNode_Constructor` | no such object | d |  | with its API |
| `AbsoluteOrientationSensor` | orientation-sensor |  |  |  |  | 2 (0) / 0 (0) | 0.024 `AbsoluteOrientationSensorConstructor` | no such object | d |  | with its API |
| `AnalyserNode` | webaudio |  |  |  |  | 0 (0) / 0 (0) | 0.10 `V8AnalyserNode_Constructor` | no such object | d |  | with its API |
| `AudioBuffer` | webaudio |  |  |  |  | 0 (0) / 0 (0) | 0.23 `V8AudioBuffer_Constructor` | no such object | d |  | with its API |
| `AudioBufferSourceNode` | webaudio |  |  |  |  | 0 (0) / 0 (0) | 0.0039 `V8AudioBufferSourceNode_Constructor` | no such object | d |  | with its API |
| `BiquadFilterNode` | webaudio |  |  |  |  | 0 (0) / 0 (0) | 0.0022 `V8BiquadFilterNode_Constructor` | no such object | d |  | with its API |
| `ByteLengthQueuingStrategy` | streams |  |  |  |  | 0 (0) / 0 (0) | 0.078 `ByteLengthQueuingStrategyConstructor` | no such object | d |  | with its API |
| `CountQueuingStrategy` | streams |  |  |  |  | 0 (0) / 0 (0) | 0.0064 `CountQueuingStrategyConstructor` | no such object | d |  | with its API |
| `DOMStringMap` | html |  |  |  |  | 0 (0) / 0 (0) |  | its own (dataset): name it | a | P1b |  |
| `DecompressionStream` | compression |  |  |  |  | 0 (0) / 9 (0) | 0.61 `DecompressionStreamConstructor` | no such object | d |  | with its API |
| `DelayNode` | webaudio |  |  |  |  | 0 (0) / 0 (0) | 0.0018 `V8DelayNode_Constructor` | no such object | d |  | with its API |
| `DynamicsCompressorNode` | webaudio |  |  |  |  | 0 (0) / 0 (0) | 0.0043 `V8DynamicsCompressorNode_Constructor` | no such object | d |  | with its API |
| `GainNode` | webaudio |  |  |  |  | 0 (0) / 0 (0) | 0.011 `V8GainNode_Constructor` | no such object | d |  | with its API |
| `GravitySensor` | accelerometer |  |  |  |  | 1 (0) / 0 (0) | 0.21 `GravitySensorConstructor` | no such object | d |  | with its API |
| `HTMLBRElement` | html |  |  |  |  | 0 (0) / 0 (0) |  | shares HTMLElement's | p | P6 | its members are reflections, or text the tree has |
| `HTMLDListElement` | html |  |  |  |  | 0 (0) / 0 (0) |  | shares HTMLElement's | p | P6 | its members are reflections, or text the tree has |
| `HTMLDirectoryElement` | html |  |  |  |  | 0 (0) / 0 (0) |  | shares HTMLElement's | p | P6 | its members are reflections, or text the tree has |
| `HTMLFontElement` | html |  |  |  |  | 0 (0) / 0 (0) |  | shares HTMLElement's | p | P6 | its members are reflections, or text the tree has |
| `HTMLFormControlsCollection` | html |  |  |  |  | 0 (0) / 0 (0) |  | the shared collection's | p | P3 |  |
| `HTMLHRElement` | html |  |  |  |  | 0 (0) / 0 (0) |  | shares HTMLElement's | p | P6 | its members are reflections, or text the tree has |
| `HTMLHeadingElement` | html |  |  |  |  | 0 (0) / 0 (0) |  | shares HTMLElement's | p | P6 | its members are reflections, or text the tree has |
| `HTMLHtmlElement` | html |  |  |  |  | 0 (0) / 0 (0) |  | shares HTMLElement's | p | P6 | its members are reflections, or text the tree has |
| `HTMLMenuElement` | html |  |  |  |  | 0 (0) / 0 (0) |  | shares HTMLElement's | p | P6 | its members are reflections, or text the tree has |
| `HTMLOptionsCollection` | html |  |  |  |  | 0 (0) / 0 (0) |  | no such object (select.options absent) | p | P1a |  |
| `HTMLParagraphElement` | html |  |  |  |  | 0 (0) / 0 (0) |  | shares HTMLElement's | p | P6 | its members are reflections, or text the tree has |
| `HTMLPreElement` | html |  |  |  |  | 0 (0) / 0 (0) |  | shares HTMLElement's | p | P6 | its members are reflections, or text the tree has |
| `HTMLSpanElement` | html |  |  |  |  | 0 (0) / 0 (0) |  | shares HTMLElement's | p | P6 | its members are reflections, or text the tree has |
| `HTMLTableCaptionElement` | html |  |  |  |  | 0 (0) / 0 (0) |  | shares HTMLElement's | p | P6 | its members are reflections, or text the tree has |
| `HTMLTitleElement` | html |  |  |  |  | 0 (0) / 0 (0) |  | shares HTMLElement's | p | P6 | its members are reflections, or text the tree has |
| `ImageCapture` | image-capture |  |  |  |  | 0 (0) / 0 (0) | 0.0036 `ImageCaptureConstructor` | no such object | d |  | with its API |
| `KeyframeEffect` | web-animations |  |  |  |  | 3 (0) / 7 (0) | 0.22 `V8KeyframeEffect_Constructor` | no such object | d |  | with its API |
| `LinearAccelerationSensor` | accelerometer |  |  |  |  | 2 (0) / 0 (0) | 0.069 `LinearAccelerationSensorConstructor` | no such object | d |  | with its API |
| `MediaStreamAudioDestinationNode` | webaudio |  |  |  |  | 0 (0) / 0 (0) | 0.0047 `V8MediaStreamAudioDestinationNode_Constructor` | no such object | d |  | with its API |
| `PressureObserver` | compute-pressure |  |  |  |  | 0 (0) / 1 (0) | 0.22 `PressureObserver_Constructor` | no such object | d |  | with its API |
| `RadioNodeList` | html |  |  |  |  | 0 (0) / 0 (0) |  | no such object (namedItem answers an element) | p | P3 |  |
| `RelativeOrientationSensor` | orientation-sensor |  |  |  |  | 2 (0) / 0 (0) | 0.021 `RelativeOrientationSensorConstructor` | no such object | d |  | with its API |
| `SpeechGrammarList` | speech-api |  |  |  |  | 0 (0) / 0 (0) | 0.051 `V8SpeechGrammarList_Constructor` | no such object | d |  | with its API |
| `StereoPannerNode` | webaudio |  |  |  |  | 0 (0) / 0 (0) | 0.0014 `V8StereoPannerNode_Constructor` | no such object | d |  | with its API |
| `TaskController` | scheduling-apis |  |  |  |  | 0 (0) / 0 (0) | 0.031 `TaskControllerConstructor` | no such object | d |  | with its API |
| `ValidityState` | html |  |  |  |  | 0 (0) / 0 (0) |  | no such object | p | P4 |  |
| `ViewTimeline` | scroll-animations |  |  |  |  | 0 (0) / 0 (0) | 0.0093 `ViewTimelineConstructor` | no such object | d |  | with its API |
| `XPathEvaluator` | dom |  |  |  |  | 0 (0) / 2 (0) | 0.93 `V8XPathEvaluator_Constructor` | no such object | d |  | with its API |

The other 667 absent shipped names, which no source names here, by spec: html 52, SVG 51, webgpu 36, css-typed-om 28, filter-effects 27, webaudio 22, webxr 21, webrtc 19, usb 13, webcodecs 12, speech-api 11, webgl1 11, dom 10, webxrlayers 10, and 153 more specs with 344 names between them. WINDOW_DOCUMENT.md's Appendix D names most of them by spec, as webref stood on 2026-10-23.

Exposed on Window in the IDL but shipped by no engine BCD records, or not in BCD: 138 names, left out of every count.

## Appendix B — The form members

Per interface, every member webref gives it (mixins included; constructors left out; an overloaded name once), present or not. Columns: **yonder** as the probe found it; **Chrome %** where a counter exists (§ Usage, 1; **W** is webstatus.dev); **scan pages**, the new pull's and 0e's, **g** marking a generic name (§ Usage, 4); **class** (§ Classification); **phase**. Ordered by distinctive names first, by pages. `hspace` and `vspace` on input ship nowhere (BCD) and are left out of every count.

### HTMLSelectElement (26 members, 5 present)

| member | kind | yonder | Chrome % | scan pages 10-25 / 0e | class | phase | note |
|---|---|---|---|---|---|---|---|
| `selectedIndex` | attr | classic |  | 146 / 288 |  |  |  |
| `validity` | ro attr | absent |  | 38 / 69 | m | P4 | constraint validation, whole, with the invalid event |
| `checkValidity` | method | absent |  | 20 / 17 | m | P4 | constraint validation, whole, with the invalid event |
| `namedItem` | method | absent |  | 20 / 17 | a | P1a | the select's list of options (HTMLOptionsCollection) |
| `reportValidity` | method | absent |  | 13 / 23 | m | P4 | constraint validation, whole, with the invalid event |
| `selectedOptions` | ro attr | absent |  | 12 / 54 | p | P1a | the select's list of options (HTMLOptionsCollection) |
| `setCustomValidity` | method | absent |  | 6 / 18 | m | P4 | constraint validation, whole, with the invalid event |
| `validationMessage` | ro attr | absent |  | 3 / 7 | m | P4 | constraint validation, whole, with the invalid event |
| `showPicker` | method | absent | 0.0004 W | 1 / 0 | d |  | a script-opened picker: yonder's list opens only for the person |
| `willValidate` | ro attr | absent |  | 0 / 4 | m | P4 | constraint validation, whole, with the invalid event |
| `type` | ro attr | classic |  | 696 / 691 g |  |  |  |
| `length` | attr | absent |  | 630 / 674 g | a | P1a | the select's list of options (HTMLOptionsCollection) |
| `name` | attr | classic |  | 582 / 636 g |  |  |  |
| `value` | attr | classic |  | 578 / 621 g |  |  |  |
| `remove` | method | classic, partial |  | 564 / 541 g |  | P1a | partial: Element's remove() (ChildNode), which ignores an index; P1a adds remove(index) |
| `add` | method | absent |  | 548 / 560 g | a | P1a | the select's list of options (HTMLOptionsCollection) |
| `size` | attr | absent |  | 403 / 524 g | a | P1a | the select's list of options (HTMLOptionsCollection) |
| `item` | method, getter | absent |  | 367 / 397 g | a | P1a | the select's list of options (HTMLOptionsCollection) |
| `disabled` | attr | absent |  | 347 / 444 g | a | P3 | a reflection, or the control's form owner and labels |
| `options` | ro attr | absent |  | 344 / 457 g | p | P1a | the select's list of options (HTMLOptionsCollection) |
| `form` | ro attr | absent |  | 343 / 391 g | a | P3 | a reflection, or the control's form owner and labels |
| `labels` | ro attr | absent |  | 246 / 335 g | a | P3 | a reflection, or the control's form owner and labels |
| `autocomplete` | attr | absent |  | 172 / 248 g | a | P3 | a reflection, or the control's form owner and labels |
| `multiple` | attr | absent |  | 142 / 172 g | a | P1a | the select's list of options (HTMLOptionsCollection) |
| `required` | attr | absent |  | 114 / 165 g | a | P3 | a reflection, or the control's form owner and labels |
| (indexed setter) | setter | absent |  |  | a | P1a | the select's list of options (HTMLOptionsCollection) |

### HTMLOptionsCollection (5 members, 0 present)

| member | kind | yonder | Chrome % | scan pages 10-25 / 0e | class | phase | note |
|---|---|---|---|---|---|---|---|
| `selectedIndex` | attr | absent |  | 146 / 288 | p | P1a | a live list over the select's options; React reads its length and each index |
| `length` | attr | absent |  | 630 / 674 g | p | P1a | a live list over the select's options; React reads its length and each index |
| `remove` | method | absent |  | 564 / 541 g | p | P1a | a live list over the select's options; React reads its length and each index |
| `add` | method | absent |  | 548 / 560 g | p | P1a | a live list over the select's options; React reads its length and each index |
| (indexed setter) | setter | absent |  |  | p | P1a | a live list over the select's options; React reads its length and each index |

### HTMLOptionElement (8 members, 0 present)

| member | kind | yonder | Chrome % | scan pages 10-25 / 0e | class | phase | note |
|---|---|---|---|---|---|---|---|
| `defaultSelected` | attr | absent |  | 98 / 130 | a | P1a | React's option updater reads or writes it; libpage keeps the option's value and selectedness |
| `value` | attr | absent |  | 578 / 621 g | a | P1a | React's option updater reads or writes it; libpage keeps the option's value and selectedness |
| `text` | attr | absent |  | 561 / 600 g | a | P1a | the option's own state, which libpage's option list already keeps, or a reflection |
| `label` | attr | absent |  | 376 / 410 g | a | P1a | the option's own state, which libpage's option list already keeps, or a reflection |
| `index` | ro attr | absent |  | 356 / 447 g | a | P1a | the option's own state, which libpage's option list already keeps, or a reflection |
| `disabled` | attr | absent |  | 347 / 444 g | a | P1a | React's option updater reads or writes it; libpage keeps the option's value and selectedness |
| `form` | ro attr | absent |  | 343 / 391 g | a | P1a | the option's own state, which libpage's option list already keeps, or a reflection |
| `selected` | attr | absent |  | 286 / 382 g | a | P1a | React's option updater reads or writes it; libpage keeps the option's value and selectedness |

### HTMLOptGroupElement (2 members, 0 present)

| member | kind | yonder | Chrome % | scan pages 10-25 / 0e | class | phase | note |
|---|---|---|---|---|---|---|---|
| `label` | attr | absent |  | 376 / 410 g | a | P1a | the option's own state, which libpage's option list already keeps, or a reflection |
| `disabled` | attr | absent |  | 347 / 444 g | a | P1a | the option's own state, which libpage's option list already keeps, or a reflection |

### HTMLInputElement (61 members, 4 present)

| member | kind | yonder | Chrome % | scan pages 10-25 / 0e | class | phase | note |
|---|---|---|---|---|---|---|---|
| `defaultValue` | attr | absent |  | 300 / 395 | a | P3 | a reflection, or the control's form owner and labels |
| `selectionStart` | attr | absent |  | 193 / 134 | m | P2 | the text control's selection, kept per control and shared with yonder's field |
| `selectionEnd` | attr | absent |  | 166 / 132 | m | P2 | the text control's selection, kept per control and shared with yonder's field |
| `formAction` | attr | absent |  | 153 / 181 | a | P3 | a reflection, or the control's form owner and labels |
| `setSelectionRange` | method | absent |  | 153 / 75 | m | P2 | the text control's selection, kept per control and shared with yonder's field |
| `formTarget` | attr | absent |  | 106 / 166 | a | P3 | a reflection, or the control's form owner and labels |
| `defaultChecked` | attr | absent |  | 102 / 130 | a | P3 | a reflection, or the control's form owner and labels |
| `maxLength` | attr | absent |  | 80 / 68 | a | P3 | a reflection, or the control's form owner and labels |
| `readOnly` | attr | absent |  | 67 / 91 | a | P3 | a reflection, or the control's form owner and labels |
| `minLength` | attr | absent |  | 64 / 83 | a | P3 | a reflection, or the control's form owner and labels |
| `validity` | ro attr | absent |  | 38 / 69 | m | P4 | constraint validation, whole, with the invalid event |
| `formMethod` | attr | absent |  | 36 / 21 | a | P3 | a reflection, or the control's form owner and labels |
| `indeterminate` | attr | absent |  | 24 / 79 | r | P3 | a checkbox's third state: an IDL flag with no attribute, which yonder's checkbox must then draw |
| `checkValidity` | method | absent |  | 20 / 17 | m | P4 | constraint validation, whole, with the invalid event |
| `setRangeText` | method | absent |  | 15 / 5 | m | P2 | the text control's selection, kept per control and shared with yonder's field |
| `reportValidity` | method | absent |  | 13 / 23 | m | P4 | constraint validation, whole, with the invalid event |
| `hspace` | attr | absent |  | 11 / 4 | not shipped |  | in no shipping engine (BCD): left out of every count |
| `vspace` | attr | absent |  | 11 / 4 | not shipped |  | in no shipping engine (BCD): left out of every count |
| `valueAsDate` | attr | absent | 0.079 (`InputElementValueAsDateGetter`) | 7 / 10 | a | P5 | libpage's number and date grammars (number.c, range.c) |
| `valueAsNumber` | attr | absent |  | 7 / 13 | a | P5 | libpage's number and date grammars (number.c, range.c) |
| `setCustomValidity` | method | absent |  | 6 / 18 | m | P4 | constraint validation, whole, with the invalid event |
| `formNoValidate` | attr | absent |  | 5 / 2 | a | P3 | a reflection, or the control's form owner and labels |
| `selectionDirection` | attr | absent |  | 5 / 12 | m | P2 | the text control's selection, kept per control and shared with yonder's field |
| `validationMessage` | ro attr | absent |  | 3 / 7 | m | P4 | constraint validation, whole, with the invalid event |
| `colorSpace` | attr | absent |  | 2 / 29 | d |  | the color input's picker, which yonder lacks |
| `showPicker` | method | absent | 0.031 | 1 / 0 | d |  | a script-opened picker |
| `stepDown` | method | absent |  | 1 / 0 | a | P5 | libpage's number and date grammars (number.c, range.c) |
| `stepUp` | method | absent |  | 1 / 0 | a | P5 | libpage's number and date grammars (number.c, range.c) |
| `dirName` | attr | absent |  | 0 / 0 | a | P3 | a reflection, or the control's form owner and labels |
| `formEnctype` | attr | absent |  | 0 / 0 | a | P3 | a reflection, or the control's form owner and labels |
| `popoverTargetAction` (PopoverTargetAttributes) | attr | absent |  | 0 / 0 | d |  | popover (an element feature yonder lacks) |
| `popoverTargetElement` (PopoverTargetAttributes) | attr | absent |  | 0 / 0 | d |  | popover (an element feature yonder lacks) |
| `useMap` | attr | absent |  | 0 / 6 | d |  | skip: obsolete reflections; trivial if ever asked |
| `webkitEntries` (entries-api) | ro attr | absent | 0.042 | 0 / 0 | d |  | a directory picker and the Entries API |
| `webkitdirectory` (entries-api) | attr | absent |  | 0 / 0 | d |  | a directory picker and the Entries API |
| `willValidate` | ro attr | absent |  | 0 / 4 | m | P4 | constraint validation, whole, with the invalid event |
| `type` | attr | classic |  | 696 / 691 g |  |  |  |
| `src` | attr | absent |  | 612 / 667 g | a | P3 | a reflection, or the control's form owner and labels |
| `height` | attr | absent |  | 601 / 604 g | a | P3 | a reflection, or the control's form owner and labels |
| `width` | attr | absent |  | 593 / 612 g | a | P3 | a reflection, or the control's form owner and labels |
| `name` | attr | classic |  | 582 / 636 g |  |  |  |
| `min` | attr | absent |  | 578 / 586 g | a | P3 | a reflection, or the control's form owner and labels |
| `value` | attr | classic |  | 578 / 621 g |  |  |  |
| `max` | attr | absent |  | 440 / 575 g | a | P3 | a reflection, or the control's form owner and labels |
| `size` | attr | absent |  | 403 / 524 g | a | P3 | a reflection, or the control's form owner and labels |
| `disabled` | attr | absent |  | 347 / 444 g | a | P3 | a reflection, or the control's form owner and labels |
| `form` | ro attr | absent |  | 343 / 391 g | a | P3 | a reflection, or the control's form owner and labels |
| `select` | method | absent |  | 309 / 355 g | m | P2 | the text control's selection, kept per control and shared with yonder's field |
| `placeholder` | attr | absent |  | 283 / 356 g | a | P3 | a reflection, or the control's form owner and labels |
| `alt` | attr | absent |  | 279 / 231 g | a | P3 | a reflection, or the control's form owner and labels |
| `checked` | attr | classic |  | 273 / 389 g |  |  |  |
| `accept` | attr | absent |  | 251 / 296 g | a | P3 | a reflection, or the control's form owner and labels |
| `labels` | ro attr | absent |  | 246 / 335 g | a | P3 | a reflection, or the control's form owner and labels |
| `list` | ro attr | absent |  | 240 / 268 g | d |  | with HTMLDataListElement |
| `step` | attr | absent |  | 184 / 284 g | a | P3 | a reflection, or the control's form owner and labels |
| `capture` (html-media-capture) | attr | absent | 1.15 | 178 / 191 g | d |  | skip: a phone camera hint; presence reads as a phone |
| `autocomplete` | attr | absent |  | 172 / 248 g | a | P3 | a reflection, or the control's form owner and labels |
| `multiple` | attr | absent |  | 142 / 172 g | a | P3 | a reflection, or the control's form owner and labels |
| `pattern` | attr | absent |  | 137 / 150 g | a | P3 | a reflection, or the control's form owner and labels |
| `required` | attr | absent |  | 114 / 165 g | a | P3 | a reflection, or the control's form owner and labels |
| `files` | attr | absent |  | 102 / 116 g | d |  | a FileList: needs the file picker |
| `align` | attr | absent |  | 50 / 91 g | d |  | skip: obsolete reflections; trivial if ever asked |
| `alpha` | attr | absent |  | 31 / 91 g | d |  | the color input's picker, which yonder lacks |

### HTMLTextAreaElement (30 members, 3 present)

| member | kind | yonder | Chrome % | scan pages 10-25 / 0e | class | phase | note |
|---|---|---|---|---|---|---|---|
| `defaultValue` | attr | absent |  | 300 / 395 | a | P3 | a reflection, or the control's form owner and labels |
| `selectionStart` | attr | absent |  | 193 / 134 | m | P2 | the text control's selection, kept per control and shared with yonder's field |
| `selectionEnd` | attr | absent |  | 166 / 132 | m | P2 | the text control's selection, kept per control and shared with yonder's field |
| `setSelectionRange` | method | absent |  | 153 / 75 | m | P2 | the text control's selection, kept per control and shared with yonder's field |
| `maxLength` | attr | absent |  | 80 / 68 | a | P3 | a reflection, or the control's form owner and labels |
| `readOnly` | attr | absent |  | 67 / 91 | a | P3 | a reflection, or the control's form owner and labels |
| `minLength` | attr | absent |  | 64 / 83 | a | P3 | a reflection, or the control's form owner and labels |
| `validity` | ro attr | absent |  | 38 / 69 | m | P4 | constraint validation, whole, with the invalid event |
| `checkValidity` | method | absent |  | 20 / 17 | m | P4 | constraint validation, whole, with the invalid event |
| `setRangeText` | method | absent |  | 15 / 5 | m | P2 | the text control's selection, kept per control and shared with yonder's field |
| `reportValidity` | method | absent |  | 13 / 23 | m | P4 | constraint validation, whole, with the invalid event |
| `setCustomValidity` | method | absent |  | 6 / 18 | m | P4 | constraint validation, whole, with the invalid event |
| `selectionDirection` | attr | absent |  | 5 / 12 | m | P2 | the text control's selection, kept per control and shared with yonder's field |
| `validationMessage` | ro attr | absent |  | 3 / 7 | m | P4 | constraint validation, whole, with the invalid event |
| `textLength` | ro attr | absent |  | 1 / 20 | a | P3 | the value's length in UTF-16 code units |
| `dirName` | attr | absent |  | 0 / 0 | a | P3 | a reflection, or the control's form owner and labels |
| `willValidate` | ro attr | absent |  | 0 / 4 | m | P4 | constraint validation, whole, with the invalid event |
| `type` | ro attr | classic |  | 696 / 691 g |  |  |  |
| `name` | attr | classic |  | 582 / 636 g |  |  |  |
| `value` | attr | classic |  | 578 / 621 g |  |  |  |
| `disabled` | attr | absent |  | 347 / 444 g | a | P3 | a reflection, or the control's form owner and labels |
| `form` | ro attr | absent |  | 343 / 391 g | a | P3 | a reflection, or the control's form owner and labels |
| `select` | method | absent |  | 309 / 355 g | m | P2 | the text control's selection, kept per control and shared with yonder's field |
| `placeholder` | attr | absent |  | 283 / 356 g | a | P3 | a reflection, or the control's form owner and labels |
| `labels` | ro attr | absent |  | 246 / 335 g | a | P3 | a reflection, or the control's form owner and labels |
| `wrap` | attr | absent |  | 238 / 382 g | a | P3 | a reflection, or the control's form owner and labels |
| `autocomplete` | attr | absent |  | 172 / 248 g | a | P3 | a reflection, or the control's form owner and labels |
| `required` | attr | absent |  | 114 / 165 g | a | P3 | a reflection, or the control's form owner and labels |
| `rows` | attr | absent |  | 83 / 152 g | a | P3 | a reflection, or the control's form owner and labels |
| `cols` | attr | absent |  | 26 / 57 g | a | P3 | a reflection, or the control's form owner and labels |

### HTMLButtonElement (21 members, 3 present)

| member | kind | yonder | Chrome % | scan pages 10-25 / 0e | class | phase | note |
|---|---|---|---|---|---|---|---|
| `formAction` | attr | absent |  | 153 / 181 | a | P3 | a reflection, or the control's form owner and labels |
| `formTarget` | attr | absent |  | 106 / 166 | a | P3 | a reflection, or the control's form owner and labels |
| `validity` | ro attr | absent |  | 38 / 69 | m | P4 | constraint validation, whole, with the invalid event |
| `formMethod` | attr | absent |  | 36 / 21 | a | P3 | a reflection, or the control's form owner and labels |
| `checkValidity` | method | absent |  | 20 / 17 | m | P4 | constraint validation, whole, with the invalid event |
| `reportValidity` | method | absent |  | 13 / 23 | m | P4 | constraint validation, whole, with the invalid event |
| `setCustomValidity` | method | absent |  | 6 / 18 | m | P4 | constraint validation, whole, with the invalid event |
| `formNoValidate` | attr | absent |  | 5 / 2 | a | P3 | a reflection, or the control's form owner and labels |
| `validationMessage` | ro attr | absent |  | 3 / 7 | m | P4 | constraint validation, whole, with the invalid event |
| `commandForElement` | attr | absent |  | 0 / 0 | d |  | invoker commands (an element feature yonder lacks) |
| `formEnctype` | attr | absent |  | 0 / 0 | a | P3 | a reflection, or the control's form owner and labels |
| `popoverTargetAction` (PopoverTargetAttributes) | attr | absent |  | 0 / 0 | d |  | popover (an element feature yonder lacks) |
| `popoverTargetElement` (PopoverTargetAttributes) | attr | absent |  | 0 / 0 | d |  | popover (an element feature yonder lacks) |
| `willValidate` | ro attr | absent |  | 0 / 4 | m | P4 | constraint validation, whole, with the invalid event |
| `type` | attr | classic |  | 696 / 691 g |  |  |  |
| `name` | attr | classic |  | 582 / 636 g |  |  |  |
| `value` | attr | classic |  | 578 / 621 g |  |  |  |
| `disabled` | attr | absent |  | 347 / 444 g | a | P3 | a reflection, or the control's form owner and labels |
| `form` | ro attr | absent |  | 343 / 391 g | a | P3 | a reflection, or the control's form owner and labels |
| `command` | attr | absent |  | 251 / 322 g | d |  | invoker commands (an element feature yonder lacks) |
| `labels` | ro attr | absent |  | 246 / 335 g | a | P3 | a reflection, or the control's form owner and labels |

### HTMLFormElement (19 members, 5 present)

| member | kind | yonder | Chrome % | scan pages 10-25 / 0e | class | phase | note |
|---|---|---|---|---|---|---|---|
| `relList` | ro attr | absent |  | 115 / 139 | a | P3 | a reflection, or the control's form owner and labels |
| `acceptCharset` | attr | absent |  | 43 / 9 | a | P3 | a reflection, or the control's form owner and labels |
| `requestSubmit` | method | classic |  | 37 / 30 |  |  |  |
| `enctype` | attr | absent |  | 32 / 103 | a | P3 | a reflection, or the control's form owner and labels |
| `checkValidity` | method | absent |  | 20 / 17 | m | P4 | constraint validation, whole, with the invalid event |
| `reportValidity` | method | absent |  | 13 / 23 | m | P4 | constraint validation, whole, with the invalid event |
| `noValidate` | attr | absent |  | 1 / 2 | a | P3 | a reflection, or the control's form owner and labels |
| `length` | ro attr | absent |  | 630 / 674 g | a | P3 | a reflection, or the control's form owner and labels |
| `target` | attr | absent |  | 605 / 626 g | a | P3 | a reflection, or the control's form owner and labels |
| `name` | attr | classic |  | 582 / 636 g |  |  |  |
| `reset` | method | classic |  | 561 / 501 g |  |  |  |
| `submit` | method | classic |  | 349 / 384 g |  |  |  |
| `rel` | attr | absent |  | 344 / 375 g | a | P3 | a reflection, or the control's form owner and labels |
| `action` | attr | absent |  | 330 / 430 g | a | P3 | a reflection, or the control's form owner and labels |
| `method` | attr | absent |  | 298 / 448 g | a | P3 | a reflection, or the control's form owner and labels |
| `elements` | ro attr | classic |  | 255 / 343 g |  |  |  |
| `autocomplete` | attr | absent |  | 172 / 248 g | a | P3 | a reflection, or the control's form owner and labels |
| `encoding` | attr | absent |  | 50 / 60 g | a | P3 | a reflection, or the control's form owner and labels |
| (indexed and named getters) | getter | absent |  |  | m | P3 | form[0] and form.name: the controls by index and by name (legacy platform object) |

### HTMLFormControlsCollection (1 member, 1 present)

| member | kind | yonder | Chrome % | scan pages 10-25 / 0e | class | phase | note |
|---|---|---|---|---|---|---|---|
| `namedItem` | method, getter | classic, partial |  | 20 / 17 |  | P3 | partial: the shared collection's namedItem, which answers the first control and never a RadioNodeList |

### RadioNodeList (1 member, 0 present)

| member | kind | yonder | Chrome % | scan pages 10-25 / 0e | class | phase | note |
|---|---|---|---|---|---|---|---|
| `value` | attr | absent |  | 578 / 621 g | p | P3 | the value of the checked radio of a name group; form.elements.namedItem hands it out |

### HTMLLabelElement (3 members, 0 present)

| member | kind | yonder | Chrome % | scan pages 10-25 / 0e | class | phase | note |
|---|---|---|---|---|---|---|---|
| `htmlFor` | attr | absent |  | 203 / 238 | a | P3 | a reflection, or the control's form owner and labels |
| `form` | ro attr | absent |  | 343 / 391 g | a | P3 | a reflection, or the control's form owner and labels |
| `control` | ro attr | absent |  | 69 / 111 g | a | P3 | a reflection, or the control's form owner and labels |

### HTMLFieldSetElement (11 members, 0 present)

| member | kind | yonder | Chrome % | scan pages 10-25 / 0e | class | phase | note |
|---|---|---|---|---|---|---|---|
| `validity` | ro attr | absent |  | 38 / 69 | m | P4 | constraint validation, whole, with the invalid event |
| `checkValidity` | method | absent |  | 20 / 17 | m | P4 | constraint validation, whole, with the invalid event |
| `reportValidity` | method | absent |  | 13 / 23 | m | P4 | constraint validation, whole, with the invalid event |
| `setCustomValidity` | method | absent |  | 6 / 18 | m | P4 | constraint validation, whole, with the invalid event |
| `validationMessage` | ro attr | absent |  | 3 / 7 | m | P4 | constraint validation, whole, with the invalid event |
| `willValidate` | ro attr | absent |  | 0 / 4 | m | P4 | constraint validation, whole, with the invalid event |
| `type` | ro attr | absent |  | 696 / 691 g | a | P3 | a reflection, or the control's form owner and labels |
| `name` | attr | absent |  | 582 / 636 g | a | P3 | a reflection, or the control's form owner and labels |
| `disabled` | attr | absent |  | 347 / 444 g | a | P3 | a reflection, or the control's form owner and labels |
| `form` | ro attr | absent |  | 343 / 391 g | a | P3 | a reflection, or the control's form owner and labels |
| `elements` | ro attr | absent | 0.080 | 255 / 343 g | a | P3 | a reflection, or the control's form owner and labels |

### HTMLLegendElement (2 members, 0 present)

| member | kind | yonder | Chrome % | scan pages 10-25 / 0e | class | phase | note |
|---|---|---|---|---|---|---|---|
| `form` | ro attr | absent |  | 343 / 391 g | a | P3 | a reflection, or the control's form owner and labels |
| `align` | attr | absent |  | 50 / 91 g | d |  | skip: obsolete reflection; trivial if ever asked |

### HTMLOutputElement (13 members, 0 present)

| member | kind | yonder | Chrome % | scan pages 10-25 / 0e | class | phase | note |
|---|---|---|---|---|---|---|---|
| `defaultValue` | attr | absent |  | 300 / 395 | a | P3 | a reflection, or the control's form owner and labels |
| `htmlFor` | ro attr | absent |  | 203 / 238 | a | P3 | a reflection, or the control's form owner and labels |
| `validity` | ro attr | absent |  | 38 / 69 | m | P4 | constraint validation, whole, with the invalid event |
| `checkValidity` | method | absent |  | 20 / 17 | m | P4 | constraint validation, whole, with the invalid event |
| `reportValidity` | method | absent |  | 13 / 23 | m | P4 | constraint validation, whole, with the invalid event |
| `setCustomValidity` | method | absent |  | 6 / 18 | m | P4 | constraint validation, whole, with the invalid event |
| `validationMessage` | ro attr | absent |  | 3 / 7 | m | P4 | constraint validation, whole, with the invalid event |
| `willValidate` | ro attr | absent |  | 0 / 4 | m | P4 | constraint validation, whole, with the invalid event |
| `type` | ro attr | absent |  | 696 / 691 g | a | P3 | a reflection, or the control's form owner and labels |
| `name` | attr | absent |  | 582 / 636 g | a | P3 | a reflection, or the control's form owner and labels |
| `value` | attr | absent |  | 578 / 621 g | a | P3 | a reflection, or the control's form owner and labels |
| `form` | ro attr | absent |  | 343 / 391 g | a | P3 | a reflection, or the control's form owner and labels |
| `labels` | ro attr | absent |  | 246 / 335 g | a | P3 | a reflection, or the control's form owner and labels |

### HTMLDataListElement (1 member, 0 present)

| member | kind | yonder | Chrome % | scan pages 10-25 / 0e | class | phase | note |
|---|---|---|---|---|---|---|---|
| `options` | ro attr | absent |  | 344 / 457 g | d |  | announces suggestions yonder's field does not offer (§ Rulings) |

### ValidityState (11 members, 0 present)

| member | kind | yonder | Chrome % | scan pages 10-25 / 0e | class | phase | note |
|---|---|---|---|---|---|---|---|
| `tooLong` | ro attr | absent |  | 26 / 23 | m | P4 | one flag of constraint validation, which ships whole or not at all |
| `customError` | ro attr | absent |  | 16 / 10 | m | P4 | one flag of constraint validation, which ships whole or not at all |
| `badInput` | ro attr | absent |  | 6 / 24 | m | P4 | one flag of constraint validation, which ships whole or not at all |
| `tooShort` | ro attr | absent |  | 6 / 15 | m | P4 | one flag of constraint validation, which ships whole or not at all |
| `patternMismatch` | ro attr | absent |  | 1 / 10 | m | P4 | one flag of constraint validation, which ships whole or not at all |
| `rangeOverflow` | ro attr | absent |  | 1 / 10 | m | P4 | one flag of constraint validation, which ships whole or not at all |
| `rangeUnderflow` | ro attr | absent |  | 1 / 10 | m | P4 | one flag of constraint validation, which ships whole or not at all |
| `stepMismatch` | ro attr | absent |  | 1 / 10 | m | P4 | one flag of constraint validation, which ships whole or not at all |
| `typeMismatch` | ro attr | absent |  | 1 / 19 | m | P4 | one flag of constraint validation, which ships whole or not at all |
| `valueMissing` | ro attr | absent |  | 1 / 32 | m | P4 | one flag of constraint validation, which ships whole or not at all |
| `valid` | ro attr | absent |  | 75 / 141 g | m | P4 | one flag of constraint validation, which ships whole or not at all |

## Appendix C — The probe

Run by the DOM host, with `modern` on and then off, from one case added in a scratch copy (§ How the binding was measured). The case, before `main`, and its line at the top of `main`:

```c
static void dom_probe_run(const char *path, bool modern)
{
    FILE *file=fopen(path,"rb"); if(!file){fprintf(stderr,"no probe\n");return;}
    static char source[65536]; size_t n=fread(source,1,sizeof(source)-1,file); fclose(file); source[n]=0;
    os64_dom_options_t options=os64_dom_default_options();
    options.modern=modern;
    options.url="https://example.test/probe.html";
    DomFixture f=fixture_new(&options);
    if(!fixture_ready(&f)) return;
    if(dom_script(&f,source,"probe")){
        os64_js_outcome_t outcome;
        JSContext *ctx=os64_js_context(f.runtime,OS64_JS_ABI_ID,&outcome);
        JSValue global=JS_GetGlobalObject(ctx);
        JSValue text=JS_GetPropertyStr(ctx,global,"__probe");
        size_t length; const char *bytes=JS_ToCStringLen(ctx,&length,text);
        if(bytes){printf("=== modern %s\n",modern?"on":"off");fwrite(bytes,1,length,stdout);printf("\n");JS_FreeCString(ctx,bytes);}
        JS_FreeValue(ctx,text);JS_FreeValue(ctx,global);
    }
    fixture_free(&f);
}
    /* first line of main */
    if(argc==3&&!strcmp(argv[1],"--probe")){dom_probe_run(argv[2],true);dom_probe_run(argv[2],false);return failures?1:0;}
```

Built and run as `tools/test_dom_host.sh --probe probe.js`. The script, whose `dump` is WINDOW_DOCUMENT.md Appendix E's with a line for each level's `constructor` and each object's `Object.prototype.toString`:

```js
(function () {
  function repr(val) {
    var t = typeof val;
    if (val === null) return ['object', 'null'];
    if (t === 'function') return [t, 'len=' + val.length + ' name=' + val.name];
    if (t === 'object') {
      var s;
      try { s = Object.prototype.toString.call(val); } catch (e) { s = '?'; }
      return [t, s];
    }
    if (t === 'string') return [t, JSON.stringify(val.slice(0, 50))];
    return [t, String(val)];
  }
  // Appendix E's dump: one line per own property at every level of obj's
  // chain above Object.prototype.
  function dump(obj, label) {
    var out = [], level = 0;
    for (var p = obj; p && p !== Object.prototype; p = Object.getPrototypeOf(p), level++) {
      var tag = '';
      try { tag = p[Symbol.toStringTag] || ''; } catch (e) {}
      Object.getOwnPropertyNames(p).forEach(function (n) {
        var d = Object.getOwnPropertyDescriptor(p, n);
        var kind = (d.get || d.set) ? 'acc' + (d.get ? 'G' : '') + (d.set ? 'S' : '')
                                    : 'data' + (d.writable ? 'W' : '');
        var r;
        try { r = repr(obj[n]); } catch (e) { r = ['throws', String(e).slice(0, 60)]; }
        out.push([label, level, tag, n, kind, r[0], String(r[1]).replace(/[\t\n]/g, ' ')].join('\t'));
      });
      if (level > 0 || label !== 'window') {
        // the level's own constructor name, when it has one
        var c = Object.getOwnPropertyDescriptor(p, 'constructor');
        out.push([label, level, tag, '#level', c ? 'ctor' : 'noctor',
                  c && typeof c.value === 'function' ? c.value.name : '', ''].join('\t'));
      }
    }
    out.push([label, -1, '', '#toString', '', '', Object.prototype.toString.call(obj)].join('\t'));
    return out.join('\n');
  }
  var out = [dump(globalThis, 'window')];
  var host = document.createElement('div');
  document.body.appendChild(host);
  host.innerHTML =
    '<form id=pf action=/go method=post><fieldset id=pfs><legend id=pl>L</legend>' +
    '<label id=plab for=pin>Name</label><input id=pin name=n required>' +
    '<input id=pcb type=checkbox name=c><input id=pr1 type=radio name=r value=a><input id=pr2 type=radio name=r value=b>' +
    '<input id=pnum type=number min=1 max=9><input id=pfile type=file><input id=pmail type=email>' +
    '<input id=prange type=range><input id=pdate type=date><input id=phid type=hidden><input id=psub type=submit>' +
    '<input id=plist list=pdl><datalist id=pdl><option value=x></option></datalist>' +
    '<select id=psel name=s><optgroup id=pog label=G><option id=po1>A</option></optgroup><option id=po2 selected>B</option></select>' +
    '<textarea id=pta>t</textarea><button id=pbtn>b</button><output id=pout for=pin></output></fieldset></form>' +
    '<svg id=psvg><rect id=prect></rect></svg><!--c-->';
  function $(id) { return document.getElementById(id); }
  var targets = [
    ['select', $('psel')], ['option', $('po1')], ['optgroup', $('pog')],
    ['input-text', $('pin')], ['input-checkbox', $('pcb')], ['input-radio', $('pr1')],
    ['input-number', $('pnum')], ['input-file', $('pfile')], ['input-email', $('pmail')],
    ['input-range', $('prange')], ['input-date', $('pdate')], ['input-hidden', $('phid')],
    ['input-submit', $('psub')], ['textarea', $('pta')], ['button', $('pbtn')], ['form', $('pf')],
    ['label', $('plab')], ['fieldset', $('pfs')], ['legend', $('pl')], ['output', $('pout')],
    ['datalist', $('pdl')], ['div', host]
  ];
  var text = document.createTextNode('t');
  targets.push(['text', text]);
  targets.push(['comment', document.createComment('c')]);
  var svg;
  try { svg = document.createElementNS('http://www.w3.org/2000/svg', 'svg'); targets.push(['svg-createElementNS', svg]); }
  catch (e) { out.push(['svg-createElementNS', -1, '', '#throws', '', '', String(e).slice(0, 80)].join('\t')); }
  if (typeof document.createElementNS !== 'function')
    out.push(['svg-createElementNS', -1, '', '#absent', '', '', typeof document.createElementNS].join('\t'));
  targets.push(['svg-parsed', $('psvg')]);
  targets.push(['svg-rect-parsed', $('prect')]);
  function view(label, f) {
    try { var v = f(); if (v !== null && typeof v === 'object') targets.push([label, v]);
          else out.push([label, -1, '', '#value', '', typeof v, String(v)].join('\t')); }
    catch (e) { out.push([label, -1, '', '#throws', '', '', String(e).slice(0, 80)].join('\t')); }
  }
  view('select.options', function () { return $('psel').options; });
  view('select.selectedOptions', function () { return $('psel').selectedOptions; });
  view('form.elements', function () { return $('pf').elements; });
  view('form.elements.r', function () { return $('pf').elements.namedItem ? $('pf').elements.namedItem('r') : $('pf').elements.r; });
  view('fieldset.elements', function () { return $('pfs').elements; });
  view('input.validity', function () { return $('pin').validity; });
  view('input.labels', function () { return $('pin').labels; });
  view('input.files', function () { return $('pfile').files; });
  view('input.list', function () { return $('plist').list; });
  view('datalist.options', function () { return $('pdl').options; });
  view('output.htmlFor', function () { return $('pout').htmlFor; });
  view('childNodes', function () { return document.body.childNodes; });
  view('getElementsByTagName', function () { return document.getElementsByTagName('p'); });
  view('querySelectorAll', function () { return document.querySelectorAll('p'); });
  view('classList', function () { return document.body.classList; });
  view('document.forms', function () { return document.forms; });
  for (var i = 0; i < targets.length; i++) out.push(dump(targets[i][1], targets[i][0]));
  // How the three known asks fare.
  var asks = [
    ['react: select.options.length', function () { var e = $('psel').options; return e.length; }],
    ['react: options[0].selected', function () { var e = $('psel').options; return e[0].selected; }],
    ['svelte: Text.prototype', function () { return typeof Text; }],
    ['vue: SVGElement', function () { return typeof SVGElement; }],
    ['option.selected set', function () { $('po1').selected = true; return $('psel').selectedIndex; }],
    ['new Option', function () { return typeof Option; }]
  ];
  asks.forEach(function (a) {
    var r; try { r = repr(a[1]()); } catch (e) { r = ['throws', String(e).slice(0, 80)]; }
    out.push([a[0], -1, '', '#ask', '', r[0], r[1]].join('\t'));
  });
  globalThis.__probe = out.join('\n');
})();
```
