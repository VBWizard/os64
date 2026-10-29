# CUSTOM_ELEMENTS.md: custom elements, and the mutation callback they share

*The end of Tier 0 in [MODERN_WEB.md](MODERN_WEB.md): `customElements`,
the element reactions, and `MutationObserver` moved here from slice 5a,
because both stand on the same libhtml callback. Design by Opus,
2026-10-29, from a read of libhtml, libgarb, libdom and yonder at
`userland` ec337800 and of Fable's [RELAYOUT.md](RELAYOUT.md), whose
mutation callback this builds. Opus builds it in three cuts. Chris rules
and tests.*

## What this settles

1. **One mutation callback in libhtml, as RELAYOUT.md designs it, built
   now.** `os64_html_observe` tells its one listener of every change the
   standard's "queue a mutation record" step sees, inside the verb, after
   the change. Custom elements read it for their reactions and
   `MutationObserver` reads it for its records. The changes to RELAYOUT's
   record are below. 5a keeps the change log and the
   instrument. The log is built from the same record later, so the two
   cannot disagree.
2. **The callback never enters the engine.** It appends to queues that
   libdom keeps in C, holding the nodes, and it returns. JavaScript runs
   only at the three points the standard names: when a `[CEReactions]`
   member returns, at the microtask checkpoint, and in synchronous
   construction inside `createElement`. Nothing a page does runs inside a
   libhtml verb or inside one of libdom's multi-step methods.
3. **`[CEReactions]` is a table generated from the standard's IDL, not
   code repeated in each member.** At install, each listed method and
   attribute setter that libdom has is wrapped by one trampoline: it
   pushes an element queue, calls the member, then pops the queue and runs
   its reactions. A member the table misses still works. Its reactions go
   to the backup element queue and run at the next microtask checkpoint,
   which is the standard's own behaviour for a change made outside any
   `[CEReactions]` member.
4. **An element's custom state lives on its libhtml node**, set by a verb
   that moves the version, because `:defined` is a selector and libgarb
   matches against libhtml's tree. The rule for a valid custom element
   name moves from libdom into libhtml so that libdom, libgarb and
   yonder's census use one copy.
5. **Autonomous custom elements only.** `define`'s `extends` is a
   NotSupportedError, as in Safari, and there is no shadow DOM, no
   `ElementInternals`, and no scoped registries. What is out, and the
   census line that would bring each in, is listed at the end.
6. **The parser does not construct synchronously, but its elements are
   upgraded before any script can see them.** An element the document
   parser makes after its definition exists goes on the backup element
   queue. yonder drains that queue (`os64_dom_reactions_run`) after every
   parser feed and before every parser-blocking script, so the classic
   pattern of `define` in `<head>` and use in `<body>` works. What remains
   is a divergence with a DEBTS row: such an element is UPGRADED (its
   constructor runs with its attributes present and the element
   connected, and `attributeChangedCallback` fires once per attribute)
   rather than constructed bare. A constructor that reads
   `this.getAttribute` can tell the difference. GitHub's pages, whose
   module scripts call `define` after the parse, take the upgrade-on-define
   path, which is exact.

## What GitHub asks for

From Chris's P5 records of a GitHub pull-request page (2026-10-28, two
loads), with modules and import maps in:

- **Globals the bundles reach for and die on:** `customElements` 4
  times, `MutationObserver` 2, `IntersectionObserver` 1, `ResizeObserver`
  1, and `ShadowRoot` 2.
- **How they die, in order:**
  - Two bundles stop at `'customElements' is not defined`, and a third
    reads `get` of something undefined.
  - Two stop at `'MutationObserver' is not defined`. One of them is the
    element registry, which stops inside catalyst, GitHub's
    custom-element framework.
  - The behaviours and diff bundles stop at `IntersectionObserver` and
    `ResizeObserver`.
- **The undefined elements:** 269 elements of 34 custom names are
  counted as MISSING, from `g-emoji` (76) and `tool-tip` (47) to
  `include-fragment` (32), `details-menu` (16), `relative-time` (12) and
  `react-partial` (3).

Custom elements alone would move the first failure one line down, to
catalyst's observer. That is why `MutationObserver` comes with them as
this slice's third cut and not as 5a's. Shadow DOM is likely the next
wall after these two: the record already holds two `ShadowRoot`
lookups.

## The callback (libhtml), and what changes from RELAYOUT.md

The declaration is RELAYOUT.md's `os64_html_observe` and
`os64_html_change_t`, with these changes:

- **Old values always arrive, borrowed.** A verb has the old attribute
  value and the old text in hand when it calls the listener, so
  `attr_old` and `text_old` are always set, valid for the call. A NULL
  `attr_old` means the attribute was absent. `keep_old` then only decides
  whether the LOG copies them. `attributeChangedCallback` needs the old
  value for every observed attribute, and paying for a copy that the
  callback reads and lets go would make no sense.
- **The attribute's namespace and local name.** `attr_ns` (NULL for
  none) and `attr_local` join `attr_name`.
  `attributeChangedCallback(name, old, new, namespace)` and
  `MutationRecord.attributeNamespace` both need them, and
  `observedAttributes` matches the local name.
- **The record shapes follow the standard's, three of them by name:**
  - **A move is two records.** `insert` or `replace` of a node that has
    a parent reports its removal from that parent first, then the
    insertion.
  - **A fragment drained is two records.** The FRAGMENT's record has its
    children as `removed`, and the parent's record has them as `added`.
    An observer on a fragment sees it emptied.
  - **A replace is one record** on the parent, with both lists filled,
    after the separate removal record if the new node had a parent.
  Each record's `previous_sibling` and `next_sibling` are read as the
  tree stood at that step. A node replaced by itself is one record too,
  as the standard has it, though nothing moves.
- **The node lists are runs, not arrays.** `added` and `removed` are the
  first node and a count, read along `next`. RELAYOUT's record has
  arrays, and a drained fragment would need one allocated inside a verb
  that cannot fail once it has begun. A run needs none: the nodes a
  record names lie together as siblings when the listener is told, a
  drained fragment's in their new parent. The log, which keeps entries
  past the verb, copies a run into an array of its own.
- **The document parser reports every node it links in or takes out,
  every text it extends, and every attribute it adds.** `tree.c` links by
  two paths, `h_attach` on an undisturbed tree and `d_place` once a verb
  has touched it, and both report. So do its removals (the adoption
  agency moves nodes) and the `html` element a parse is given at its end.
  `insert_character` appending to an existing text node is a TEXT
  record, and a second `html` or `body` tag giving the element an
  attribute it lacks is an ATTR record. A fragment parse
  (`os64_html_parse_fragment`) reports nothing: it builds in a scratch
  tree and copies the result. Its elements reach the page through
  `insert`, which the listener sees. Nor does a clone, which nothing can
  be observing while it is built.
- **A fourth kind, `OS64_HTML_CHANGE_STATE`,** names an element whose
  custom state changed (below). It is never delivered to the listener,
  because the standard records no mutation for it. 5a's log writes an
  entry for it, because the version moves and 5b's restyle set is
  computed from the entries. Cut a defines the kind, and 5a writes the
  entry.

A verb that sets an attribute or a text to the value it already has calls
the listener and moves no version, as RELAYOUT says. The standard fires
`attributeChangedCallback` and an observer's record for it. Every verb
tells the listener after its change and before it retires the old bytes
or reclaims a removed subtree, so old values are still the node's own
and a removed node is still alive. The listener must not mutate the
document, hold a pin, or run a page's code. A listener that holds nodes
is allowed (`os64_html_hold` allocates nothing), and a hold taken there
keeps a removed node from being reclaimed. One listener per document. libdom installs it when the
custom-elements row runs, so with the Modern Web switch off a verb pays
one NULL test.

## The element's state (libhtml and libgarb)

```c
typedef enum {
    OS64_HTML_CE_PLAIN = 0,   /* by its name: undefined when it is a valid custom element name */
    OS64_HTML_CE_CUSTOM,      /* constructed by its definition */
    OS64_HTML_CE_FAILED       /* its constructor threw, or returned something else */
} os64_html_ce_t;
/* os64_html_node_t gains `uint8_t ce;` (an os64_html_ce_t), in the padding after xml_document. */
int64_t os64_html_set_ce(os64_html_document_t *doc, os64_html_node_t *element, os64_html_ce_t ce);
bool os64_html_custom_name(const char *name);
```

- **`set_ce` moves the version** (`:defined` is a cascade input) and is
  a STATE change: logged once 5a's log exists, and never delivered to
  the listener.
- **A clone starts PLAIN**, because the standard creates a clone and
  then upgrades it. This also lets a FAILED element's clone be tried
  again.
- **FAILED and the wrapper's interface.** `d_node_kind` reads FAILED as
  `HTMLUnknownElement`'s kind, and it is consulted only when a wrapper is
  first made. So `createElement`'s failure answers an
  `HTMLUnknownElement`, while an upgrade's failure leaves the wrapper it
  already had, with `HTMLElement`'s prototype. Both are as the standard
  has them.
- **`os64_html_custom_name`** is the valid-custom-element-name rule, in
  libhtml so that libdom (`d_node_kind`), libgarb (`:defined`) and yonder's
  census read one copy of it.

The standard's five states fold into these three.

| Standard | Here | Why it folds |
|---|---|---|
| "uncustomized" | PLAIN, name not valid | Only a valid name ever gets a definition. |
| "undefined" | PLAIN, valid name | Same thing, read from the name. |
| "precustomized" | the construction stack | Only the upgrade's own stack frame can observe it. |

**`:defined`, with the switch on, matches** CUSTOM, and PLAIN when the
element is not an HTML element with a valid custom name. It matches
nothing FAILED. Scripting off does not change this, and Chrome with
scripts off is the same. A page with
`x-foo:not(:defined) { visibility: hidden }` hides `x-foo` until a
definition constructs it, as Chrome does.

**With the switch off it stays `true`, today's answer.** The switch's
contract is that OFF shows a page as yonder showed it before the arc. A
browser with no custom elements does not know the pseudo-class, drops
the rule, and shows the element. An honest `:defined` with no
`customElements` would hide that element forever, which no browser does.
libgarb already reads `modern_off`. This is a web semantic and mine to
decide, and I am telling it here.

**yonder's census** reads the same field. A CUSTOM element is not
MISSING. A FAILED one is counted on a `FAILED element <name>` line that
carries the constructor's message, so the next wall (shadow DOM) shows up
as a reason, not as a count.

## The registry (libdom, `custom.c`)

`window.customElements` is a `[SameObject]` getter for the one
`CustomElementRegistry`, whose interface is an Illegal constructor.

**`define(name, constructor, options)`** follows HTML § 4.13.4 in order:
- `IsConstructor` is checked, else TypeError.
- The name must be valid, else SyntaxError.
- A name or constructor already defined, or a define already running, is
  a NotSupportedError, and so is `options.extends` (decision 5).
- `constructor.prototype` must be an object.
- The lifecycle callbacks are read off the prototype, each converted to a
  Function, in the standard's order. WPT's `CustomElementRegistry` tests
  check the getter order.
- `observedAttributes` is read only when `attributeChangedCallback` is
  defined, and converted as `sequence<DOMString>`.
- `disabledFeatures` and `formAssociated` are read as the standard reads
  them, and their values are kept and unused. A true `formAssociated` is
  booked in DEBTS, not refused, because refusing it would take the whole
  element away.
- Then the definition is recorded and the window document's upgrade
  candidates are found: its elements in tree order with that local name,
  in HTML's namespace, in state PLAIN. Each is queued for an upgrade.
  Because `define` is `[CEReactions]`, they are constructed before it
  returns.
- Last, a pending `whenDefined` promise for the name resolves with the
  constructor.
- The candidate walk covers the whole document for each name. A hundred
  defines over 10,000 nodes is a million visits, which is milliseconds.
  The page record carries the walk's total, so this is measured and not
  assumed. No index is wanted until the number says so.

**`get(name)`, `getName(constructor)`, `whenDefined(name)`, and
`upgrade(root)`** follow the standard's steps.
- `whenDefined` answers a rejected promise (SyntaxError) for an invalid
  name and a resolved one for a defined name.
- `upgrade` tries each inclusive descendant of `root`, in tree order.

**A definition** is C: the name, and the constructor, prototype and four
callbacks as `DValue`s the registry owns. It also holds the observed
local names (copied), and the construction stack, which is a small array
of held nodes plus the "already constructed" marker. Definitions are
looked up by name through a hash and by constructor through a pointer
compare over the list. GitHub defines about a hundred.

## Constructing

- **The `HTMLElement` constructor runs the standard's "HTML element
  constructors" steps.** QuickJS hands a C constructor `new.target` as
  `this_val`, which is what `super()` from a derived class passes.
  - If `new.target` is `HTMLElement` itself, or no definition has it as
    its constructor, the answer is TypeError, Illegal constructor. That
    is today's answer, so with no definitions nothing changes.
  - With an empty construction stack (`new MyElement()`), the answer is
    a new element of the definition's name. Its state is CUSTOM and its
    wrapper's prototype is `new.target.prototype` (`d_wrap_constructed`).
  - With a stack (an upgrade), the wrapper is the stack's top element.
    Its prototype is set to `new.target.prototype`, and the stack's top
    becomes the marker. A second `super()` hits the marker and gets an
    InvalidStateError.
  - The other HTML interfaces stay Illegal constructors (decision 5).
- **`document.createElement`, `createElementNS` in HTML's namespace, and
  a definition for the name:** synchronous construction.
  - The constructor is called. The result must be an `HTMLElement` with
    no attributes, no children, no parent, the same node document, and
    the right local name, else NotSupportedError.
  - When the call or that check throws, the exception is reported (not
    rethrown, per the standard) and the answer is an
    `HTMLUnknownElement` in state FAILED.
- **A clone** (`cloneNode`, `importNode`): each cloned element whose name
  has a definition is queued for an upgrade before the method returns.
  Both methods are `[CEReactions]`, so the clone comes back upgraded.
- **The document parser:** decision 6. The listener sees the insertion
  into the window document, and a PLAIN element with a definition is
  queued for an upgrade on the backup element queue. yonder drains that
  queue after the feed, or before the script that stopped the parse. A
  fragment parse (`innerHTML`, `DOMParser`) reports nothing and makes
  PLAIN elements, which are upgraded when an insertion connects them.
- **Second documents** (`createHTMLDocument`, `DOMParser`) have no
  registry, so nothing in them is upgraded. An element that is already
  custom stays custom, and gets its connected and disconnected callbacks
  there as the standard says. "Connected" means the root is a DOCUMENT
  node, of either kind. "Upgradable" means the root is the window's
  document.

**An upgrade** is HTML's "upgrade an element":
- Its state is set to FAILED first.
- Its attributes queue `attributeChangedCallback` with a null old value,
  for observed names.
- If it is connected, `connectedCallback` is queued.
- The element is pushed onto the construction stack, and the constructor
  is called with `new.target` the constructor.
- Unless that call threw, the result must be the element itself, else
  TypeError.
- The stack is popped. On success the state becomes CUSTOM. On failure
  the element's reaction queue is emptied, its definition dropped, and
  the exception reported.

## Reactions (libdom)

- **The records.** An element's reaction queue holds upgrades and
  callbacks. A callback's arguments are C copies: an attribute's local
  name, old value, new value and namespace, or the two documents of an
  adoption. The queue hangs off a `DCustom` record that exists only while
  a reaction is pending, and the record holds its element. An idle
  CUSTOM element has no record. Its state is on the node, and its
  definition is the registry's entry for its local name, since there are
  no scoped registries and no undefine. A record that outlived its node
  could be found again by a new node reclaimed at the same address and
  read as its state. A record that lives only while it holds its element
  cannot.
- **The element queues.** The reaction stack is an array of element
  queues, one per `[CEReactions]` frame, plus the backup element queue.
  - Enqueueing an element puts it on the top queue, or on the backup
    queue when the stack is empty.
  - The first element put on the backup queue asks for its processing.
  - Inside an open task, that request is a microtask (`JS_EnqueueJob`,
    as `queueMicrotask` does).
  - Outside one, which is the parser's case, yonder calls
    `os64_dom_reactions_run(dom, outcome)` after every parser feed and
    before every parser-blocking script. That call runs the queue as a
    task named "custom element reactions".
  - A due-now entry that `os64_dom_timer_next` answers is the backstop,
    for an insertion that came from no task and no feed.
- **Invoking.** Elements are taken in queue order, and each element's
  reactions run in order:
  - an upgrade runs as above;
  - a callback is called with the element as `this`, inside
    `d_script_enter` and `d_script_leave` for the callback's script.
  - An exception is reported as that script's (`d_report_as`) and the
    next reaction runs.
  Reactions run inside the open task's deadline and job cap like every
  other call.
- **The trampoline and its table.** `tools/ce_reactions.py` reads
  `dom.idl`, `html.idl` and `cssom.idl` from the pinned WPT checkout and
  writes `userland/libdom/ce_reactions.inc`: `{interface, member, kind}`
  for every `[CEReactions]` operation and every attribute whose setter
  is `[CEReactions]`.
  - The custom-elements row runs last and wraps what exists. A method's
    function is replaced. An accessor keeps its getter and gets a wrapped
    setter, so a read pays nothing.
  - Each wrapper keeps its original's `name` and `length`, and its data
    holds the original function. It is redefined with the original
    property's attributes (writable, enumerable, configurable), or
    idlharness fails every wrapped member.
  - When the original throws, the wrapper still pops the queue and runs
    its reactions, then rethrows. The standard's `[CEReactions]` steps
    cover a member's exceptions too.
  - A row naming a member libdom lacks is skipped. The DOM host lists the
    members it expects to be wrapped and fails when one is not, so a
    member gained later that is not in the table is caught by the
    reactions tests, not by a reader.
  - **Exotic paths cannot be wrapped**, and the host's list names each
    one with that reason: `DOMStringMap`'s setter and deleter
    (`dataset.x = y`, `delete dataset.x`, `attr.c`'s exotic class), and
    the indexed and named exotic paths of the same shape. Their reactions
    reach the backup queue by design and run at the checkpoint. The style
    object's `cssText`, `setProperty` and camel-cased properties are
    prototype accessors, so the table reaches them.
- **Where reactions come from.** The listener (decision 2) handles three
  kinds of change.
  - **CHILDREN, insertion:** if the target is connected, each inclusive
    descendant element of each added node, in tree order, either queues
    `connectedCallback` (CUSTOM) or is tried for an upgrade (PLAIN, an
    upgradable root, and a definition for its name).
  - **CHILDREN, removal:** if the target was connected, each CUSTOM
    element in the removed subtrees queues `disconnectedCallback`.
  - **ATTR:** a CUSTOM element whose definition observes the local name
    queues `attributeChangedCallback`. That includes a set to the same
    value.
  - **Adoption:** an element whose node document changes (`adoptNode`,
    or a move between the window's document and a second document,
    which `d_leaving` already records) queues `adoptedCallback`, before
    its connected callbacks.
  - **No definitions, no work.** Until the first `define`, the listener's
    custom-element half returns at once, so a page that defines nothing
    walks nothing. After that, an insertion walks the inserted subtree,
    as a browser does. A parser insertion is usually a single node.
- **No memory inside the listener.** The listener can neither throw nor
  allocate a JavaScript value, so when memory runs out the reaction is
  dropped and counted. At the next checkpoint the binding reports one
  error, a QuotaExceededError naming the count. A lost reaction is never
  silent.
  - The listener's records and copies come from libdom's `d_alloc`
    quota, not the document's arena. So a page cannot turn a verb's
    arena refusal into a lost reaction, or a lost reaction into an arena
    refusal.
- **Teardown.** Queues, definitions and their holds go in libdom's
  teardown, before the engine, in D8's order.

## MutationObserver (the third cut)

RELAYOUT.md § "MutationObserver reads the log" is the design, unchanged
except for three points:
- it is built on this slice's listener, which libdom already installed;
- the old-value options need no `keep_old`, because the listener always
  has the values;
- the listener feeds both halves. Which half it feeds first cannot be
  observed, because reactions run when the `[CEReactions]` member returns
  and records are delivered at the checkpoint, and both are queued
  before either point.

What 5a then holds is the change log, its readers, and the instrument.

## The cut, in order

| Cut | Builds | Oracle | Size |
|---|---|---|---|
| a | libhtml: `os64_html_observe` with the record above, and the `STATE` kind; the document parser's reports on both attach paths and the text append; `ce` and `os64_html_set_ce`; `os64_html_custom_name`. libgarb: `:defined`, honest with the switch on. yonder's census. | libhtml host: one fixture per verb, asserting the records by shape (a move is two, a fragment drained is two, a replace is one, an attribute set to its own value is one), the parser's attachments on both paths and its text appends, and a fragment parse reporting nothing; `set_ce` moves the version and reaches no listener; a clone is PLAIN. libgarb host: `:defined` against each state, switch on and off. Mutants. | S |
| b | libdom `custom.c`: the registry, the `HTMLElement` constructor, `createElement`'s and clone's construction, the reactions, the trampoline and `ce_reactions.inc`, `os64_dom_reactions_run` and yonder's calls to it, the timer backstop, the row. | WPT `custom-elements/` (added to the sparse checkout): its expected file, and the required lines the cut earns. DOM host: the cases above, and the wrapped-member list with the exotic paths named. Scripted-page host: a classic page that defines in `<head>` and calls a method in `<body>`, and a streamed page. Mutants. P5: GitHub's record. | L |
| c | libdom `MutationObserver`, `MutationRecord`. | WPT `dom/nodes/MutationObserver-*` (17 files). DOM host. P5: GitHub's record past catalyst. | M |

Each cut is a PR, stacked on the one before. Codex reads each. Fable
reads cut a's diff before merge, because it is his callback in libhtml's
mutation core. He reads the verbs' report sites, the parser's two
attach paths, the text append, and the `STATE` kind, against the
records the standard names.

## Acceptance

- **WPT:** `custom-elements/` and `dom/nodes/MutationObserver-*`, counts
  before and after, every directory's other lines unchanged, and all of
  them failing with the switch off.
- **The P5, GitHub's pull-request page:**
  - the record has no `MISSING global customElements` and no
    `MISSING global MutationObserver`;
  - the MISSING element lines drop to the names nothing defines;
  - the FAILED element lines name what stopped the rest (expected:
    `attachShadow`);
  - the first failing bundle moves on to `IntersectionObserver` and
    `ResizeObserver`.
- **The P5, any page that defines nothing:** load time unchanged. The
  listener's custom-element half is one test there.

## What this does not do, and the risks named

- **Customized built-ins** (`extends`, `is=`): NotSupportedError. Safari
  never shipped them, so pages feature-test for them.
- **Shadow DOM:** none, so `attachShadow` is undefined, and a
  constructor that calls it fails into FAILED with that message on the
  census line. It is its own slice and needs a Fable design, because it
  is a second tree under an element, with slots, that libflow has to lay
  out.
- **`ElementInternals` and form-associated elements:** a DEBTS row. The
  census names neither yet.
- **Scoped registries, `connectedMoveCallback` and `moveBefore`:**
  `define` reads the callback in the standard's order but never calls
  it, and `moveBefore` does not exist.
- **The parser's synchronous construction:** decision 6, a DEBTS row. A
  parser-made element is upgraded, not constructed bare. A constructor
  that reads its own attributes in its body sees them present, and
  `attributeChangedCallback` fires once for each.
- **The trampoline adds one call** to every wrapped method and setter. A
  `[CEReactions]` member already does tree work, so one more call there
  is noise. Cut b's numbers have to show that on the scripted-page
  host's mutation fixtures, and if it is not noise, the measurement says
  so.
- **`:defined` turning honest with the switch on** hides the hyphenated
  elements a page hides while they are undefined (above). If a P5 test
  page loses something it showed, that page is behaving the way Chrome
  does, and the census line says which element it is waiting on. With
  the switch off it shows them, as before.

## Review record

**Fable, 2026-10-29, at `ec337800`.** Build it as cut. Fable said yes to
all four foundation questions:
- RELAYOUT's record changes;
- the state on the node;
- the generated `[CEReactions]` table;
- `MutationObserver` moving out of 5a.

He had seven findings, all taken:
1. `set_ce` needs a log kind now, `STATE`.
2. A fragment parse must not report. The document parser reports both of
   its attach paths and its text appends.
3. The classic-script case made the parser divergence larger than this
   document first said. Draining the backup queue after every feed and
   before every parser-blocking script reduces it to "upgraded, not
   constructed bare".
4. `:defined` stays `true` with the switch off.
5. The exotic members the table cannot wrap are named. The wrapper keeps
   the original's property attributes and runs reactions when the member
   throws.
6. `DCustom` exists only while reactions are pending, and the definition
   is found by name.
7. A fragment drained is two records, and a replace is one.

Text corrections taken: the three points where JavaScript runs, the
marker's InvalidStateError, FAILED and the wrapper's interface, the
candidate walk's number on the record, and the listener's `d_alloc`
quota.
