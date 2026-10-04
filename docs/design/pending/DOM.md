# DOM.md — one tree, and a script that may change it

*Written 2026-10-01 by Fable. This is packet D0 of
[JAVASCRIPT_TASKS.md](JAVASCRIPT_TASKS.md): the design J3 waits on. D1, D2a, D2b, D3 and D5a are merged (§ Slices); D5b is implemented
and awaiting review. The remaining slices are proposals. Read
against the tree at `b55c3770`, the vendored QuickJS 2026-06-04 source
(`userland/libjs/upstream/`) and the R0 runtime contract
(`userland/libjs/CONTRACT.md`). Names of functions that do not exist yet
are working names; the slice that builds each one settles its spelling.*

## What this settles

[JAVASCRIPT.md](JAVASCRIPT.md) ruled that the DOM is libhtml's tree made
mutable, and left six things to this packet: the mutation verbs and how a
change is noticed, how long a document and a script's handle on it live,
how replaced text is reclaimed while something still points at it, what
happens when a rebuild fails, which thread runs script and how the parser
hands over, and whether yonder can survive a leak at teardown.

Eight rulings answer them. Each says what would reverse it.

1. **One tree, one set of mutation primitives, two callers.** The parser is
   the first caller and the verbs are the second.
   *Reversed by:* a mutation the parser needs that a script must never be
   able to make.
2. **A string in the tree is never changed in place once something may
   have borrowed it.** A replacement swaps the pointer and *retires* the
   old bytes. Retired bytes are freed when no snapshot built before the
   swap is left, which the document knows because every snapshot *pins*
   the version it was built at.
   *Reversed by:* a borrower that cannot say when it was built.
3. **The model, the cascade and the layout stay derived, and are rebuilt
   whole.** Presentation may lag the tree. Meaning may not: nothing leaves
   the machine from a model older than the tree.
   *Reversed by:* a real page whose rebuilds are visibly slow, which is the
   trigger LAYOUT.md already booked for incremental relayout.
4. **One runtime per document**, made when the document's first script is
   about to run and destroyed before the document is freed. A wrapper
   cannot outlive its document because its runtime does not.
   *Reversed by:* frames, or anything else that lets one script reach two
   documents.
5. **The document never crosses a thread.** With this design yonder parses
   on its window's thread and a worker carries only bytes. This reverses
   the lean I wrote in JAVASCRIPT_REVIEW.md (the parser parks on a worker
   and asks the window to run the script); § Where script runs says what
   reading the code turned up.
   *Reversed by:* parsing in slices measurably stalling the window on real
   pages.
6. **The event loop is yonder's own loop with three additions**: a task
   queue per page, a microtask checkpoint after every callback, and one
   rendering step per turn. There is no nested loop anywhere.
7. **A geometry read is answered from a fresh layout, and the cost is
   charged to the script that asked.**
   *Reversed by:* nothing; a stale answer is a wrong answer. What changes
   is the cost, when incremental relayout arrives.
8. **A leak at teardown can be survived, by reclaiming the dead runtime's
   memory wholesale and never entering it again.** It is a libjs slice with
   its own evidence, held to Quinn's four conditions. Until that evidence
   exists the contract stays as R0 has it: fatal.

## What borrows from the tree today

The design follows from who holds what, so this is the inventory it was
drawn from.

| Holder | What it keeps | Where that is said |
|---|---|---|
| libpage's model | node pointers as keys; attribute values and text it "never copies" when it can point | LIBPAGE.md § Where it sits |
| libflow's tree | `box->node`; text items that point straight into `node->text` when collapsing changes nothing; family names as slices of an attribute value | `flow.h` header comment, `boxes.c` `text_item` |
| libgarb's cascade | node pointers as keys (`garb_style_for`); a sheet's strings are its own arena's copies | `cascade.h` |
| yonder | the reader's `flipped` details, the scroll anchor, a picture's address (the model's) | `way.h`, `yonder.c` |

Three of the four are **snapshots**: built whole from the tree at one
moment, read many times, freed whole. That is the property rulings 2 and 3
stand on.

How the tree's memory is laid out matters as much. libhtml has two
allocators over one budget (`core.c`): `h_permanent` carves nodes,
attribute records, names and attribute values out of chunks that are freed
only with the document, and `h_alloc` makes individually freeable blocks on
a ledger. A text node's bytes are already a ledger block, because the
parser grows them a character at a time. So "retire, then reclaim" needs no
new allocator. It needs the verbs to put what they write on the ledger.

## The mutation core

Three private facts are added to a document. The public structs do not
change, and a consumer still reads them as read-only views.

**The version.** A counter that every primitive bumps when it changes
something a reader could see, and leaves alone when it does not: a moved
version rebuilds a model, a cascade and a layout, so a verb that ends
where it began must not move it. An empty fragment inserted, a node put
where it already sits, a text or an attribute set to the value it has, an
absent attribute removed and a node replaced by itself all answer OK with
the version where it was. The one same-place insert that does move it is
the one that changed something: a node is still taken out and put back,
which parts a control from a form outside what moved. (A script that sets `className` to what it is
already, every frame, is common.) It is the whole invalidation contract:
libhtml calls nobody. A consumer that wants to know whether the tree moved
compares the version it was built at with `os64_html_version(doc)`. A live
collection (`childNodes`, `getElementsByTagName`) is a query and the
version it was last answered at.

**Pins.** A snapshot's constructor pins the document's current version and
its destructor lets go. `os64_page_build`, `garb_cascade` and `flow_layout`
do it themselves, so no face ever pins and none can forget to. The pin
table is a small fixed array inside the document, so taking a pin allocates
nothing and cannot fail for memory; a full table is a refusal by name, and
the snapshot is not built. Freeing a document with a pin still out
ends the program, by name: something is about to read freed memory, and
that is the class of fault `os64_free` already ends a program for. So does
letting go of a pin that is not held. A pin's number is its slot and a
serial no other pin in the program has had, so a number let go already, or
one taken on another document, matches nothing; a bare slot number would
come round again, and its stale twin would release the pin of whichever
snapshot took the slot next.

**The retired list.** When a primitive replaces a string or unlinks an
attribute record, the old one is stamped with the new version and moved to
the retired list. The move reuses the ledger block's own links, so retiring
cannot fail. Two rules read the stamps:

- *Reclaim.* A block retired at version V is freed when no live pin is
  below V. A snapshot built at V or later was built after the swap and
  never saw the old bytes. With no pins at all, retiring frees at once,
  which is what the parser's buffers do today.
- *Grow in place* (with D2, when a tree can be read mid-parse). The
  parser may append to a text buffer in place only when every live pin is
  older than the buffer. Otherwise a layout built mid-parse could be
  pointing at bytes the next character moves or overwrites, so the append
  copies to a fresh block and retires the old one. That is one copy per
  text node per snapshot taken, not one per character.

A parser-era string that is replaced is simply left where it lies in its
permanent chunk. It was charged when it was parsed and goes with the
document. What tells the two kinds apart costs the parser nothing. A text
node's private word already holds its buffer's capacity, and zero means
"not a block of its own". An element's attribute records are known by the
element: the parser's clones share one list of records (a `<b>` reopened in
every paragraph is many elements and one list), so the first verb to change
an element's attributes copies its list, each record into a block of its
own, and marks the element. From then on every record of that element is a
block that can be retired, and no other element sees the change.

This is quiescent-state reclamation with one thread, the idea RCU made
famous in the Linux kernel: a reader announces when it started, and a
writer frees nothing a reader that old could still be holding. Here the
readers are three snapshots and the announcement is a version number.

**Budgets and failure.** Everything a verb allocates is charged to the
document's `max_arena_bytes`, and retired bytes stay charged until they are
reclaimed. A verb that cannot have its memory changes nothing a reader can
see, and answers `OS64_HTML_ARENA_EXHAUSTED` or `OS64_HTML_NO_MEMORY`. It
does *not* set `doc->refusal`: that field records how the parse ended, and
a script that runs the document out of budget has not unparsed it. The
binding turns the status into an exception.

Nodes a failed verb had already made stay charged until the document is
freed, like any detached node.

## The verbs

All of them live in libhtml and are tested there, on the host, with no
engine in the process. The binding's job is to convert arguments, call one
verb and turn a status into an exception.

| Verb | What it does |
|---|---|
| `create_element(doc, ns, name)`, `create_text`, `create_comment`, `create_fragment` | a detached node, owned by the document |
| `clone(doc, node, deep)` | a detached copy; form-owner records are not copied. `node` may be another document's, and the copy then shares no memory with it |
| `insert(doc, parent, node, before)` | moves `node` (or a fragment's children) under `parent`; adjacent text is not merged, as the DOM does not |
| `remove(doc, node)` | unlinks it; the node lives on |
| `replace(doc, parent, node, old)` | one validity check for the pair, then both moves |
| `set_attr(doc, element, name, value)`, `remove_attr` | first-wins order is kept: a set on an existing name replaces its value in place in the list |
| `set_text(doc, node, utf8, len)` | replaces a text or comment node's data whole |
| `parse_fragment(doc, context, utf8, len, scripting)` | the standard's fragment algorithm, into a detached fragment, with the page's `noscript` policy |
| `serialize(node, children_only, scripting, out, cap)` | the standard's serialisation, in `snprintf`'s shape, with the same `noscript` policy |

The DOM's finer text operations (`appendData`, `insertData`, `splitText`)
are built by the binding as "make the new string, call `set_text`". A
string is immutable anyway, and their offsets are UTF-16 code units, which
is the engine's unit and not the tree's.

**The rules the verbs enforce**, because a second caller must not be able
to break what every consumer assumes:

- **Pre-insertion validity is libhtml's**, by the DOM Standard's own
  steps: no cycle, the right kinds of child under each kind of parent, the
  reference child really a child. Each refusal has its own status and the
  binding maps it to the exception's name. A cycle in the tree is an
  endless loop in every walker, so this cannot be left to callers.
  The rule that a document has one element and one doctype counts the
  node being inserted, so the document's own `html` element cannot be
  inserted under it again, even to where it already is. That reads like an
  oversight and is what the standard's steps say and what Chrome does
  (`document.appendChild(document.documentElement)` throws "Only one
  element on document allowed"); replacing it with itself is quiet.
- **A node belongs to one document, and a verb refuses another's.** A
  node lies in its document's arena and its replaced strings are retired
  on its document's ledger, so one document's node in another's tree would
  dangle when the first was freed, and a change to it would unlink a block
  from the wrong list. Each node carries its document's mark and every
  verb checks it (`OS64_HTML_BAD_ARGUMENT`). A mark is a count of the
  documents the program has begun and is never given out twice, so the
  count ends: after 4,294,967,295 documents a program is refused another
  (a page a second for 136 years). A count that came round could put one
  mark on two live documents, and the check would then pass the very node
  it exists to refuse. `clone` is the way across:
  given another document's node it copies everything, names and attribute
  records included, into the document it was asked to make the copy in.
  That is `importNode`, and `adoptNode` is a clone and a remove when a
  second document a script can reach arrives.
- **The tree stays UTF-8 with no NUL**, so every string in it is still a C
  string (LIBHTML.md). A verb refuses bytes that are not. The binding
  converts first: a lone surrogate and a NUL each become U+FFFD. The NUL
  rule is a deviation from the DOM, which lets script store one; it is the
  parser's own rule for the same character, and it is recorded here so
  nobody rediscovers it as a bug.
- **Depth is a property of the real tree.** The parser bounds its
  open-element stack at `max_depth`; a script could build deeper with a
  loop of `appendChild`, and libpage's walks recurse down the tree with no
  cap of their own (`gather`, `collect_forms` in its `core.c`), so that
  bound is what stands between a page and their stack. But `max_depth`
  bounds the parser's *stack*, and a tree can be deeper than its stack:
  `</form>` takes the form off the stack while what it holds stays open,
  so `<form><div></form>` repeated nests two levels for each entry the
  stack keeps. A parse can therefore build a tree up to twice `max_depth`
  deep, and that is the number the verbs hold a document to: an insert
  whose deepest node would pass it is refused with `OS64_HTML_TOO_DEEP`.
  The depth is counted when asked, by walking up and across what moves; no
  node stores it.
- **The document keeps its `html` element.** `doc->html` is promised never
  NULL, and libflow's root is that element. A verb that would leave the
  document node without an `html` element child is refused. That is a
  deviation (the DOM allows an empty document), booked below.
  `doc->head` and `doc->body` are recomputed by the verbs when a child of
  `html` changes, by the standard's definitions of them.
- **A form-owner record is cleared by the mutation that invalidates it,
  and no reader ever re-validates one.** libpage trusts a record it finds
  (`form_owner` in its `core.c` prefers it to ancestry), so a record left
  standing after it stopped being true comes back to life the moment the
  thing that masked it goes away. The standard's "reset the form owner"
  says when a record dies, and the verbs clear it at each of those points:
  - *a move that parts a control from its form*: every control in the
    moved subtree whose form is outside it, and every control outside it
    whose form is inside. A control and its form that move together stay
    tied, as the standard's "no longer in the same tree" has it. A tree
    here is the DOM's: a template's contents are a tree of their own, so a
    control and its form inside them are parted by nothing that happens to
    the template. The document counts its live records, so the second half
    costs a walk only when there is one to find;
  - *a listed control given a `form` attribute*: that control. In
    `<table><form id=f><input id=q></table>` the parser ties `q` to `f`,
    which is not its ancestor. Set `q`'s `form` to a name that matches
    nothing and then remove the attribute, and `q` must be left with no
    owner, not handed back to `f`. Removing the attribute needs no step of
    its own: a control that has the attribute has no record.
  After a clear, libpage's existing rule (the `form` attribute, else the
  nearest ancestor) is the standard's own answer.

## The parser with scripting on

`os64_html_options_t` gains `scripting`. Off is today's parser, bit for
bit, and stays wend's mode. On:

- **`noscript` follows the standard's scripting-enabled branches**: raw
  text in the head and in the body, and the "in head noscript" mode is not
  entered.
- **The parser stops at a script's end tag.** `feed` returns
  `OS64_HTML_SCRIPT`, and `os64_html_parser_script(p)` names the element.
  Bytes of that chunk the parser had not reached are held inside it, and a
  `feed` while a script is pending adds to the hold.
  `os64_html_parser_resume(p)` carries on from the hold, and may stop at
  the next script. The push interface keeps its shape: chunk boundaries
  are still invisible. That is why the hold is the heap's and not the
  arena's: it is input, `max_bytes` already bounds it, and charged to the
  arena it would make a page near its budget refuse in one cutting of its
  bytes and parse in another. An SVG `script` stops the parse too, at its
  end tag or its self-closing start tag. A script inside a template's
  contents does not: it is in a document that runs nothing.
- **A parser can be abandoned without losing its document.**
  `os64_html_parser_destroy` frees the document with the parser, which is
  right for a download nobody will read. A page being left mid-load still
  has a runtime, wrappers and snapshots that point into the tree, so it
  needs `os64_html_parser_abandon(p)`: release the parser's scratch, run
  nothing, and hand back the document as far as it was built. The page
  then frees that document last, like any other (§ Wrappers, teardown).
- **`finish` keeps meaning "the input has ended"**, and never stops: it
  runs what is held and the end-of-file steps straight through, so a
  script still in the hold is not run. A host that wants them run says the
  input ended with `os64_html_parser_end(p)`, which may stop at a script
  as `feed` does, and calls `finish` once that answers OK.
- **The tree so far is readable between calls**
  (`os64_html_parser_document`). That is the debt LIBHTML.md booked as
  "reading the tree during `feed`", paid by the mutation core: a snapshot
  taken mid-parse pins, and the parser's own writes go through the same
  primitives as a script's. Each call that parses anything moves the
  version once, before the first thing it parses, so whatever was pinned
  between two calls is older than every block the call makes.
- **A verb between two calls can leave the tree unlike anything a parser
  built**, and the parser's own soundness came from its stack of open
  elements: nothing put under its own descendant, nothing deeper than
  twice the stack. A script that takes an open `p` out and puts the `div`
  around it inside it has voided both. So the document remembers that a
  verb moved a node while it was being parsed, and from then on the parser
  holds each link it makes to the verbs' own rules, charging the walks as
  work. A link the rules refuse is not made and the parse goes on, as the
  standard's parser goes on whatever a script did; a node that would lie
  past the depth limit refuses the parse by name. Until a verb moves
  something none of this runs, and the parser is the one that was there
  before.
- **`document.write` is in**, as its own slice. The old web yonder exists
  to read is full of it: counters, dates, banners. It is
  `os64_html_parser_write(p, utf8, len)`, which tokenizes the text at the
  insertion point ahead of the held input, and may itself stop at a script
  the text contained. It is legal only while the parser is stopped at a
  script. After the parse has ended it would mean "throw the document away
  and start a new one", and that is refused and booked.
- **Fragment parsing** is the standard's algorithm with a context element,
  which is what `innerHTML` needs.

**The proof was already written, and skipped.** LIBHTML.md's skip list
named two families out of scope: the cases marked `#script-on` and the
cases with a `#document-fragment` line. D2a takes the first off the list
and D2b the second. The acceptance bar is the one the parser already met:
every newly in-scope case passes. D2a's document input runs whole, byte at
a time and in random chunks; D2b's fragment input is one UTF-8 string,
parsed whole, as `innerHTML` receives it.

Which scripts run, and when, is not the parser's business. The standard's
"prepare the script element" steps (type, `nomodule`, `defer`, `async`, a
script inserted by `innerHTML` never running, one inserted by
`appendChild` running) live in the binding library. Modules are not run;
a `nomodule` script is, since it was written for a browser like this one.

## Snapshots when the tree moves

**The model.** `os64_page_rebuild(old)` is the verb LIBPAGE.md booked. It
was specified there as "re-key the edits that survive"; the next paragraph
is why it no longer carries edits at all. It builds a *new* model over the old one's document at its
current version, beside the old, and returns it; NULL on no memory, the
old one untouched. The old model is freed by its owner once nothing uses
it.

**A control's state belongs to its node, not to a model.** The standard
puts the value, the checkedness and their dirty flags on the element, and
a script can tell: set an input's value, detach it, let the page be
rebuilt, put it back, and the value must still be there. A control that
was never in the document (`createElement('input')`, then `value = ...`,
then `appendChild`) has a value too. So the edit table leaves the model.
It becomes its own object in libpage, the page's *control state*: keyed by
node, at both levels (a `select`'s choices are kept option by option),
living as long as the document, owned by whoever owns the document.
Models borrow it. A rebuild copies nothing and so cannot lose anything,
and the old and the new model read one truth. The build's passes that
normalise radio groups write to it only after the new model's own memory
is in hand, so a failed rebuild still changes nothing. A face that keeps
no document after its model (wend) passes no state, and the model makes
and frees its own, which is today's behaviour.

The state has its own version: a script can change a field's live value,
checkedness or selection without changing any HTML node. The rendering
step observes that version as well as the document's. Publication updates
the views in live models before replaced state bytes are freed.

**A script's setter is not a person's edit, and gets its own door.**
`os64_page_set_text` refuses a disabled or readonly control and does not
take a hidden one at all; that is right for a keyboard and wrong for
`input.value = ...`, which the standard allows on all of them. What a
script's assignment does depends on the input's *value mode*: for a text
field it sets the dirty value; for a hidden input or a button it sets the
`value` attribute, which is a tree mutation; for a checkbox it sets the
attribute too; for a file input only the empty string is accepted. So
libpage gains script-facing verbs (value, checkedness, a `select`'s
choice) that take a node, connected or not, apply the value mode, and
share the sanitizer underneath. The checks that belong to a person's
interaction stay in the door a person's edits use. Form semantics still
live in one library; they now have two entrances because there are two
kinds of caller.

D3 interprets the current tree and stages dirty-value sanitization when a
getter or rebuild observes changed types or constraints. A final tree
version cannot reconstruct input value-mode transitions between observations.
D5's attribute bindings must therefore use a state-aware mutation entrance
that observes each type transition, including the standard's transfer of a
dirty text value to the value attribute. That entrance must stage its tree
and state changes before publication; the existing single-attribute HTML
verb does not provide a transaction spanning two attribute assignments.

**A model older than its document sends nothing.** `os64_page_activate`
and the edit verbs answer a new reason, `STALE`, when the document's
version is past the model's. A script may have changed a form's action; a
submission from the old model would send what a person typed to an address
the page no longer names. This is the class of fault LIBPAGE.md was
written to end, so the gate is in the door and not in each face.

**The layout keeps the model it was built from alive.** A flow tree's
`link` and `control` are indices into *that* model. So a face that holds a
newer model asks by node (`os64_page_link_for`, `os64_page_control_for`)
and never uses a box's index with a model the box was not built from. In
yonder a control's widget becomes the *node's*, not the index's: a rebuild
keeps the widget of a control that survived, and with it the caret.

**What stays valid when a rebuild fails:**

| What failed | What the person keeps | What is refused |
|---|---|---|
| the model | the old page, drawn and scrolling | following a link, sending a form (`STALE`), with a sentence |
| the cascade or the layout | the last layout that fit, as today's "this is the last layout that fit" | nothing: the model is current, and a click is resolved by node |
| a verb, for budget | everything | that one change; the script gets an exception |

Old layouts point at nodes that may since have been detached. That is safe
(a node lives as long as its document) and it is correct: a click on a box
whose node is no longer a link in the current model does nothing.

**When rebuilds happen.** Once per turn of the loop, in the rendering step,
if the version moved; sooner only when something asks a question that needs
the answer now (a form property, a geometry read, an activation). Every
mutation dirties all three snapshots. Telling a text change from a
structural one is an optimisation, booked with its trigger.

## Wrappers, and how long things live

**A node's wrapper is found through a table the binding owns**, keyed by
node pointer, the shape of libpage's edit table. libhtml learns nothing
about script. `document.body === document.body` holds because the table
answers the same object both times.

**The table holds each wrapper strongly until the page is torn down.**
That is what makes `element.myState = 1` and a listener still be there the
next time the element is reached, with no tracing between two heaps. Its
cost is the cost of the review's finding 2 (nodes live as long as the
document), and the two are honest about the same thing:

- For the old web and for J3's fixture it is right. A page's nodes are
  bounded by what it was served plus what its scripts add, and they add
  little.
- For a page that rebuilds itself it is wrong, and soon. A page that
  replaces a hundred nodes sixty times a second writes about a megabyte a
  second, and meets the 64 MiB budget in about a minute. That is not a
  rare page; it is any page built on a rendering framework.

So the reclamation JAVASCRIPT.md booked is not a someday item. It is the
first slice after J3's fixture and before scripting is something a person
is told to turn on. The design leaves the door open on purpose: nodes are
one size, so a free list serves; the pins already say when no snapshot can
be holding a detached node; and a private `held` bit on a node says a
wrapper, the parser's stack or a face holds it. The first cut reclaims a
removed subtree in which nothing is held, which is the `innerHTML` case.
The full answer makes the table weak and lets the collector's verdict on a
wrapper decide its node, and is its own design.

**Every engine value C holds is in a registry that has a drain.** Three
exist: the wrapper table, the listener lists, and the task and timer
queues. A value stored anywhere else is a finding. This is what makes
teardown checkable, and what makes ruling 8 possible.

**No finalizer owns a native resource.** A wrapper's opaque pointer is a
node, which the document owns. Finalizers at most clear a slot.

**Three budgets, and nothing a script causes is outside them:** the
engine's heap (libjs's limit), the document's arena (nodes and strings),
and the page's own allocations on a script's behalf (the binding's tables
and queues, and the control state), each with a cap and each reaching the
script as an exception.

**Teardown, in order**, and never from inside a script (a navigation a
script asks for is recorded and performed when its task has ended):

1. The page's serial is retired: late results are dropped unread, as a
   picture's are today. Its jobs in the pool are cancelled.
2. The parser, if the page was still loading, is abandoned
   (`os64_html_parser_abandon`): its scratch goes and the document passes
   to the page. Destroying it here would free the tree under everything
   below.
3. Timers and queued tasks are dropped.
4. The three registries are drained.
5. The runtime is destroyed. Finalizers run; the document is still there.
6. Widgets, then the layout, the cascade and the model, each letting go
   of its pin, then the control state.
7. The document, which checks that no pin is left.

No `unload` handler runs. A page being left must not be able to keep a
person there, and teardown must be bounded.

## Where script runs, and how a page arrives

**Script runs on the window's thread.** It reads geometry and changes what
layout reads, and layout is on that thread because every page shares one
text context (YONDER.md § What runs where).

**The parser moves to the window's thread too, and the worker carries
bytes.** Today the worker runs `way_load`: fetch, parse, model. My lean in
the review was to keep that and have the parser, at a script's end tag,
ask the window to run it and park, the way a downgrade question is asked.
Reading libway, yonder and the pool against that idea found three things:

- **A question shares a sentence; a script would share the document.**
  The confirm mechanism passes text one way and a yes or no back. Here the
  window would be reading and writing a tree that a pool job owns.
  `os64_work_cancel` "surrenders input AND product to the pool", `release`
  "may run on the owner or a worker", and the pool cancels on its own
  failures as well as on Stop. So a release on a worker could free the
  document under a script running in the window. It can be closed, with a
  three-state baton and a rule that a taken document must be handed back
  before the wait may end, but it is a lock protocol around a whole
  document where today there is a refcount around a mailbox.
- **`document.write` would re-enter a parser that is parked in the middle
  of its own call stack on another thread.** The standard's algorithm is
  written for one thread with a script nesting level. On one thread it is
  a re-entrant call. Across two it is not something I can make sound.
- **Everything a script touches mid-parse would have to commute between
  threads with it**: the lazily built model a form property asks for, the
  layout a geometry read forces, the runtime whose timers start during the
  load.

With the parser on the window's thread none of the three exists. Nothing
waits on a script, so nothing needs a cancellable wait; the only waits
left are the network's, which already are.

**The stream.** A navigation's job becomes: open the fetch, judge the head
exactly as `way_load` does (a page, text, or "that is not a page"), post
the head, then post the body in chunks through a bounded queue in the
mailbox, waiting in the 100 ms cancellable steps `yonder_mail_wait`
already uses when the window has not caught up, then post the fetch's
verdict. The window, on the bell, makes the page's parser from the head
and feeds it a slice at a time, returning to its event loop between
slices. A question asked mid-fetch works as it does now.

libway keeps one implementation of every judgement. `way_load` stays, for
wend, as the same pieces driven to completion on one thread. yonder takes
the pieces. There is one way a page arrives in yonder whether scripting is
on or off, so the path a script-free page takes is the path that is
tested every day.

**What the old page does meanwhile.** It stays on screen and stays live,
as it does today while sheets are awaited. Two pages can exist at once
(the one shown, and the one loading or waiting for its sheets), each with
its own runtime, both on the window's thread. A third cannot:
`start_trip` already drops the one that was coming.

**Scripts and sheets.** A script may read computed style, so the standard
makes it wait for the sheets named before it. Here it waits for them for
as long as the first paint already does (`SHEETS_WAIT_MS`), and runs
without the stragglers after that. That needs sheets discovered as the
parse goes, which the model rebuilt at a script's stop gives.

**Generations.** Nothing new is needed. The stream's mailbox carries the
navigation's generation, as mail does now. Everything else a page asks
for (a script's fetch, later a script's own requests) is a pool job that
carries the page's serial, as pictures and sheets do. A timer lives in its
page's own table. So every door into a runtime either belongs to the page
or is checked against it.

## The event loop

yonder's loop today: wait for an event, dispatch the batch, lay out if
owed, paint. The standard's loop maps onto it without a second loop.

- **A task** is one of: a slice of parsing, a script run to completion, a
  timer's callback, the dispatch of one input event, a result arriving
  from the pool. Each page has a queue of them. The loop runs queued tasks
  for a bounded slice of time, then goes back to the window's events, and
  rings its own doorbell when tasks remain. A page's tasks run only while
  the page exists.
- **A microtask checkpoint** follows every callback that returns with no
  script on the stack: after a script, after each listener when the event
  was dispatched by the window, after each timer. It drains the engine's
  job queue under the task's budget. This is the rule that makes a
  `Promise` callback run before the next listener and not after the next
  click.
- **The rendering step** is the one the loop has: if the tree's version
  moved, rebuild, lay out, paint; if live control state moved, refresh its
  presentation. Once per turn, however many tasks ran.
- **Timers** ride the ticker, which already holds the earliest of the
  next animation frame and a stale layout; the earliest timer joins them.
  The tick is 10 ms, so that is a timer's resolution, and it is said here
  so nobody measures 4 ms and files a bug.
- **Input.** A click finds its node through `flow_hit` as now, becomes a
  `click` event dispatched down and up the tree, and the default action
  (follow the link, send the form) runs afterwards through the door it
  uses today, unless a listener prevented it. Event handler attributes
  (`onclick`, `onload`, `onmouseover`) are in the first events slice and
  not a later one: the old web is written in them.
- **A script that asks to navigate** (`location`, `form.submit()`) goes
  through `os64_page_activate` and `way_judge` like any request a page
  makes, and is judged as a declared refresh is (`WAY_ASK_GO`): the page
  chose where and nobody pressed anything. A chain of them is capped as a
  refresh chain is. A script cannot take a person from https to http
  without the question bar.
- **Execution-time policy is a D7 decision.** D5b's one-second wall-clock
  deadline is a fixture choice, not a JavaScript requirement or a settled
  ordinary-browsing default. It covers one script invocation and its Promise
  checkpoint, not the page's lifetime; later turns receive fresh budgets.
  D7 should consider Chris's proposed script-timeout control in Settings.
  Choose its default and allowed range using real P5 workloads, define when
  an applied change takes effect, use Apply for this window and Save as default
  for persistence. The expiry behavior below remains the design unless revised
  in that review.
- **A script that runs too long is stopped, and the page goes on without
  script.** Its task ends at the deadline, the runtime is finished with
  (R0's rule for an exceeded limit), timers and listeners go quiet, and
  the status line says so. Links and forms keep working, because their
  default actions never needed the script. A page whose script died is a
  page without script, which is what yonder shows today.

Out of this packet, each its own design when its slice comes: `fetch` and
`XMLHttpRequest` (origins, CORS, credentials), `document.cookie` (the jar
keeps `HttpOnly`; nothing reads it for a script yet), storage,
`requestAnimationFrame`.

## Geometry

`offsetWidth`, `getBoundingClientRect` and their kind need a layout that
matches the tree. When the version has moved, the read rebuilds what is
stale and lays the page out, then answers. The page being loaded can be
asked too; it is laid out at the view's size as far as it has been parsed.

Each forced layout is a whole layout: 600 ms for Wikipedia on the P5
(YONDER.md § Booked). A script that writes a style and reads a height in a
loop pays that each time round. The cost is charged to the task's budget
and shown: the status line reports how many layouts a script forced and
how long they took. That number is the trigger for incremental relayout,
and the first page that trips the budget this way is the page that slice
is measured against.

A forced layout runs on the script's stack. J2 measures the engine's
frames; the slice that adds geometry measures libflow's deepest layout on
top of them, and that pair is the measurement DEBTS.md's configurable-
stack row is waiting for.

## What this asks of libjs

Four things. The first is in the R0 contract as approved. The other three
are reserved there by name (CONTRACT.md § Browser extensions reserved for
later work), and each is built with the slice that needs it.

1. **A thrown exception does not finish the runtime.** In a page, the
   first script error, which most pages have, must not silence every
   script after it. R0 reports EXCEPTION and UNHANDLED_REJECTION as
   outcomes and keeps the runtime usable; LIMIT, CANCELLED and
   HOST_FAILURE retire it, which is the policy above for a script that
   ran too long.
2. **A turn that begins with a call.** A listener or a timer is a function
   value the host calls. Calling it with the raw engine call runs it
   outside any turn, so with no deadline armed. The browser needs an entry
   that calls a function value under a turn's budget and reports through
   the same outcome.
3. **A checkpoint inside a task.** Draining jobs between two listeners of
   one event, with the task's deadline continuing and unhandled rejections
   judged at each checkpoint.
4. **A destroy that can report** (ruling 8), chosen by the host at
   creation. The runner keeps fatal.

Two more of R0's rules shape the binding library. A binding's class IDs
come through `os64_js_class_id(&slot)`, once per process however many
pages are opened. And each of its translation units takes the context
through `os64_js_context` with its own compiled-in ABI string, so a stale
binding object is refused by name.

## A leak at teardown

`JS_FreeRuntime` frees the pending exception and the job list, runs the
collector, and then asserts that no object and no weak reference is left
(`quickjs.c:2464`). Quinn's objection to my first lean stands: by then
finalizers have run, so continuing *in the engine* proves nothing, and
abandoning a runtime per navigation bounds nothing across a session. Her
four conditions, and what meets each:

| Condition | What meets it |
|---|---|
| A safe detection point | The two assertions become a verdict that `JS_FreeRuntime` returns from, before it frees anything further. A reviewed patch in `patches/`, the same standing as the Atomics one. Nothing is "continued past". |
| The failed runtime and its handles are dead | After the verdict the engine is never entered for that runtime again. Every value C held was in a registry drained before destroy, and every registry is page state that teardown frees, so no code path is left that can reach a value. |
| Native resources and finalizers | No finalizer owns a native resource (§ Wrappers). The finalizers that did not run, for the leaked objects, therefore held nothing but engine memory. |
| A bound across the process's life | The adapter keeps every block it hands the runtime on a ledger, as libhtml's `h_alloc` does. After a leak verdict it frees the ledger, the runtime's own struct included. Nothing is abandoned, so the cumulative bound is zero. The engine pools small objects in 4 KiB arenas (`JS_MALLOC_ARENA_SIZE`), so the ledger's two links ride on few, large blocks. |

What has to be shown before it is believed, and is the slice's acceptance:

- **Every allocation for a runtime goes through its allocator.** The
  runtime struct does (`JS_NewRuntime2`). But `cutils.c` and
  `libunicode.c` carry fallbacks to libc's `realloc` for a caller that
  passes none (`dbuf_default_realloc`, `cr_default_realloc`). The audit
  proves no retained call site reaches them, on the linked target objects.
- **No process-global state points into a runtime.** With Atomics out,
  the one mutable global I found is the class ID counter, which holds no
  pointer.
- **The J2 leak fixture, run both ways**: fatal with its badge when the
  host chose fatal; and when it chose reclaim, a report, a ledger back at
  zero, and ten thousand leaked runtimes in one process with no growth.

It is also loud. A reclaimed leak is a binding bug, so it is logged with
the page's address and counted where a fixture can read it, and every
fixture that drives the binding fails on a nonzero count. A person keeps
their window; a developer cannot miss it.

Any other engine abort stays fatal. This covers the one failure that a
single unbalanced reference in a binding produces, at the one point where
the engine is already finished.

Considered and not chosen: a process per page. It is how the large
browsers contain an engine, and os64 can spawn. It is also a different
browser: the page's pixels, its widgets and its jar would all cross a
process boundary. Booked with tabs, where it would be designed anyway.

## Slices, and what proves each

In the house's order: each finished, tested by its builder, the next
stacked on it. D1 to D4 need no engine and can start before J2 ends.

The slices handed to other builders each have a brief in
[DOM_BRIEFS.md](DOM_BRIEFS.md): what to read, what is settled, what the
builder decides, and the proof in this house's shape.

| Slice | What | Proof |
|---|---|---|
| D1 | **Built.** libhtml's mutation core and the verbs but `parse_fragment` | `tools/test_html_dom_host.sh`, under ASan and UBSan: § D1, as built |
| D2a | **Built.** Scripting-enabled parsing: stop and resume, the end of the input, abandon, the tree read and changed between calls | A parser abandoned at every stop leaves a document that walks and frees; the `#script-on` cases un-skipped and passing, whole and chunked; a stop at every script of each corpus page: § D2a, as built |
| D2b | **Merged (#212, `dca2fe46`).** Fragment parsing and serialisation | All 192 fragment fixtures, contextual serialisation, transactional allocation/work cuts and bounded-buffer proof; § D2b, as built |
| D3 | **Merged (#211, `3f5fa28a`).** `os64_page_rebuild`, pinned models, the `STALE` gate, shared node state and script property APIs | `tools/test_libpage_rebuild.inc`: detach/reinsert, never-inserted state, value modes, option identity, current-type sanitization, person/script origin, pins, transactional allocation sweeps and independent random walks; § D3, as built |
| D4 | The stream: yonder parses on its own thread. No script | yonder's and libway's harnesses unchanged in result; the guest walk that proved Y3, its server log identical; the window live through a stalled body |
| D5 | **D5a merged (#214, `72b2e710`); D5b implemented, awaiting review.** The binding library and J3's fixture | § D5a, as built records library host/guest proof. § D5b, as built records J3 fixture proof: a script changes text and the page redraws; a held reference and a typed-in field survive an unrelated change; a navigation with a script queued tears down clean; the leak count is zero |
| D6 | Reclaiming unheld detached subtrees | The churn page that met the budget in a minute runs for an hour |
| D7 | The loop: tasks, checkpoints, timers, events and their attributes, script order | J4's evidence, per contract |
| later | `document.write`; geometry; the libjs reclaim slice | each with its own |

### D1, as built

`userland/libhtml/dom.c` and the pin and retirement half of `core.c`;
`html.h` carries the contract. The parser is unchanged in what it builds:
all 8,596 reference cases, the 63,708 allocation-failure points and the
corpus give the same trees and the same work counts as before, and each
arena figure is 320 bytes higher, the growth of the document's header. A
node is no larger: its document's mark sits in four bytes the struct had
spare.

What the harness holds the verbs to:

- **By hand**, each rule with its own case: the DOM Standard's validity
  table, the `html` element, templates across their boundary, attribute
  order and the shared list, text, each form-owner reset (Quinn's table
  case among them, and a pair inside a template's contents), the depth
  limit, pins and the ways to die (a pinned document freed, a pin let go
  twice, a stale number whose slot was taken again, another document's
  pin), and two documents: each verb refusing the other's node in each
  seat, and a clone read after its source is freed.
- **Out of memory**: every allocation a verb makes failed in turn. The
  verb answers `OS64_HTML_NO_MEMORY`, the tree spells as it did, the
  version has not moved, the parse's `refusal` is untouched, and a verb
  that only replaces has given back every byte it took.
- **A deep tree on a small stack**: a 12,000-deep document cloned and
  moved on a thread whose stack could not hold a frame per level.
- **Random walks against a second tree.** The test keeps its own model of
  the document, written from the standard with arrays of children and
  none of the library's code. Each step picks a verb and its arguments at
  random, including wrong ones; the model predicts the answer; then every
  node the model knows is compared with the real one: kind, name, text,
  attributes in order, parent, children in order, template contents and
  form owner. Twenty-two walks of 4,000 steps, some under a depth limit
  low enough to keep meeting it, with up to three snapshots pinned at
  random whose borrowed strings are re-read after every step.
- **Mutants.** Fifty-eight deliberate breaks of the library, one at a
  time, against the finished code; thirty of them undo what Codex's rounds
  on #190 changed. Fifty-seven are caught. The one that
  is not cannot be told apart while the document keeps its `html`
  element: the DOM's check for a doctype after an inserted element, which
  the element the document already has always trips first. It stays, as
  the standard's rule. An earlier run also found a break nothing could
  catch because the code could never run (a reset on removing the `form`
  attribute); that code is gone.
- **In the guest**, `/tests/htmltest` drives the verbs through the real
  library on the real heap, and `htmltest pinned` passes by dying: a
  pinned document freed ends the program with its badge.

Two things the building changed, both recorded above where they belong:
attribute records carry no private word (the element owns the knowledge),
and the depth limit is twice `max_depth` (the fuzz pass found a parser
tree deeper than its stack within a minute of the harness checking for
one).

What D1 left for the slices after it: the parser's own writes did not
retire, which D2a has paid, and snapshot builders needed pins. D3 supplies
libpage's model pins; libgarb's cascade and libflow's layout pins belong
to D5 before a binding changes a tree beneath those snapshots.

### D2a, as built

`core.c` holds the calls and the hold, `tree.c` the stop and the checked
link, `dom.c` what the parser asks of the verbs' rules, and `html.h` the
contract, under THE PARSE THAT STOPS. With scripting off the parser builds
the same trees for the same work: the 8,596 reference cases, the corpus
snapshots and the fuzz pass did not move. Its peak arena is 64 bytes
higher, the growth of the parser's own scratch, and the saved Wikipedia
page parses in the time it did (22 ms at `-O2` on the host).

What `tools/test_html_host.sh` holds it to:

- **The reference cases, both ways.** The eight `#script-on` cases are off
  the skip list, and every tree case not marked `#script-off` now runs with
  scripting on as well: 1,545 more runs. Each is driven four ways and must
  stop at the same scripts and build the same document for the same work:
  whole, a byte at a time, in random pieces, and as a pile, where
  everything is fed before anything is resumed, which is the driving that
  fills the hold. Then every allocation failed in turn, byte at a time and
  as a pile, and a byte limit at every offset.
- **By hand**, the header's contract a case at a time: what each call
  answers and when, where the parse stops and where it does not, the sniff
  window, the byte limit met while stopped, `finish` straight through for
  the same work, and a snapshot pinned at a stop whose bytes stay while the
  text they belonged to grows elsewhere, once.
- **Abandoned at every stop**, and before the first byte and part way
  through the input: each document walks and frees, and the heap is empty
  after. Every corpus page is put through the same, and each stops once for
  each script it closes.
- **A tree changed under the parser.** A case for each thing a verb was
  found able to void: the open elements nested the other way round, so that the adoption
  agency's own move would make a cycle; an open element sunk to the depth
  limit, under text and under a control already tied to its form; the
  document element replaced; a `head` and a `body` put in before the
  parser's; a formatting element given attributes of its own before the
  parser clones it; the body cloned before a second `<body>` tag adds to
  it; a control carried out of its form's tree by the adoption agency, a
  form carried away from its control, and the same by a `frameset` taking
  the body out. And the ones that say what the checks do not do: a checked
  parse of a page that moves controls about inside one tree builds the
  same tree and parts nobody, and the walks are charged as work. Each case
  then runs again with every allocation failed in turn and with the work
  running out at every step.
- **Random scripts.** Documents built from pieces that set the machinery
  going (formatting to adopt, tables to foster out of, forms, templates,
  foreign content, a second `<body>`), a script at every turn, and at each
  script a run of random verbs on random nodes, the parser's open elements
  among them, with snapshots pinned and let go at random. Half the walks
  run under limits tight enough to keep meeting them, and some are
  abandoned part way. Nothing is predicted. After every stop and at the
  end, every node the document ever had is found by following every link
  from every node already known, and asked what a reader relies on: no
  cycle, nothing past the depth limit, links that agree in both directions,
  one element under the document, the landmarks by their definitions, each
  control in its form's tree and counted, what a pin held still reading as
  it did, and an empty heap after the free. The harness runs 20,000; a
  million ran clean (2.4 million stops, 197,000 refusals, 80,000 abandons)
  once the three findings below were fixed. The fuzz pass does the same
  to every mutated reference case it parses with scripting on.
- **In the guest**, `/tests/htmltest` drives a stop, a verb at the stop, a
  pinned text that grows, the end of the input and an abandon through the
  real library on the real heap.

What the building found, none of it in the reference cases:

1. **The parser on `userland` could be sent round a loop by 24 bytes.**
   `</p>` or `</br>` in foreign content is handed to the HTML rules after
   the foreign elements are popped. At an integration point there are none
   to pop, and the token was handed back to the dispatcher, which sent it
   to the same place: `<svg><foreignObject></p>` went round until the work
   budget of a hundred million ran out, and the page was refused. The
   budget is what made it a slow refusal and not a hang, and it is also
   what hid it: the fuzz pass runs under small budgets, where running out
   is ordinary. A random walk that took three and a half seconds gave it
   away. The token now goes to the HTML rules where the standard says, and
   the four trees are checked against Chrome's.
2. **The adoption agency builds aside.** It moves the furthest block under
   fresh clones before the clones have a place, so judging each link alone
   parted controls from forms they were about to rejoin. Form owners are
   now settled once, by where the block ends up. The same building aside
   meant a parse refused part way (out of work, out of memory) could leave
   a control in one tree tied to a form in another, with scripting off as
   well: the clone of the formatting element now goes under the block
   before the block's children go under the clone, and a refusal settles
   the owners like any other ending.
3. **A control that never reached the tree kept its record**, when the push
   that follows the association was refused. The count was then one high
   for good, and a count that never returns to zero never lets a move skip
   its walk.

**Mutants.** Sixty deliberate breaks of the new code, one at a time,
against the finished harness: fifty-nine are caught. The sixtieth takes
out a shortcut and nothing else: a node the parser moved that has stayed in
its tree is then walked, and the walk finds nothing. The first pass caught thirty-four of
forty-six, and what it missed is where the harness grew: the cases for the
landmarks between two feeds, the form carried away from its control, the
frameset, a control refused for depth, the byte order marks, and the
allocation and work sweeps over the cases with a changed tree. It also
found code that could not matter and is gone: a second flag beside the one
that says a verb moved a node, and two settlings of form owners at moments
when no control can have one.

**Review tier.** D1 and D2 are lifetime work in a library fed by whatever
a server sends, which is what CLAUDE.md says an outside round is for.
They get one, requested by Chris's hand as always. The rest is reviewed
here.

### D2b, as built

`userland/libhtml/fragment.c` and `serialize.c` add the two verbs in
`html.h`: `os64_html_parse_fragment(doc, context, utf8, len, scripting,
status)` and `os64_html_serialize(node, children_only, scripting, out,
cap)`. D2b needs D2a's parser machinery; D3's state API is not a
prerequisite of fragment parsing. Implemented in
[PR #212](https://github.com/VBWizard/os64/pull/212), initially stacked on
D3 at Chris's request.

**Parsing is contextual and inert.** An owned element supplies the
namespace, tokenizer state, insertion mode, integration-point attributes
and nearest ancestor form. The input is one UTF-8 string, without sniffing,
BOM removal or a charset restart. Invalid UTF-8 becomes U+FFFD. NUL follows
the tokenizer and tree rules: ordinary HTML text drops it, while raw-text,
RCDATA, attributes, comments and foreign text replace it with U+FFFD.
Scripts become nodes and never stop the parse; `scripting` selects the
`noscript` branch. The result is a detached fragment with no change to the
visible tree, document version, landmarks, existing form-owner records, pin table
or original parse outcome. Associations between nodes in the detached
result add their own records. Inserting its children is a separate verb and
checks their depth in the destination tree.

**Publication is transactional.** The existing parser builds under an
isolated document ledger, with the destination's owner mark, quirks and
depth limit. A second staged ledger receives the reachable result; only
after parsing, copying and depth checks succeed are its blocks published
to the destination. Refusals discard both ledgers and preserve the
destination's live accounting. Temporary parse storage and the staged copy
both count against the destination's remaining arena while they coexist;
a near-budget parse can therefore refuse even when its final tree alone
would fit. Parser work is bounded by saturating `4096 + 256 * len`,
including context setup and the final walks. The private `h_parse_fragment`
entry point accepts a budget and reports consumption for the work-cut
proof; it is not a public override.

**D6 has an ownership seam.** Returned node bodies, names and original
attributes are packed into individually owned ledger blocks; text buffers
have their own blocks. Inline attributes and names are copied when a clone
would otherwise share reclaimable storage. Parser scaffolding, ignored
tokens and temporary payload chunks are discarded, rather than charged
permanently for each replacement. Collection remains D6's task: detached
result nodes stay alive until document teardown under the current contract.

**Serialisation allocates nothing.** `children_only` selects inner or outer
markup, templates use their contents, HTML void elements omit their end
tags and foreign names retain their spelling. Text under HTML raw-text
elements is literal; `noscript` uses the supplied scripting mode. Other
text escapes ampersand, NBSP and angle brackets; attributes also escape
quotes. The current [HTML serialization algorithm](https://html.spec.whatwg.org/multipage/parsing.html#serialising-html-fragments)
escapes angle brackets in attributes too: the brief's earlier escaping
table was outdated. Output capacity includes the NUL, a zero capacity
writes nothing, and the return value is the full byte length. A truncated
output is a byte prefix and can end inside a UTF-8 sequence; a complete
output is UTF-8. Length overflow saturates at `SIZE_MAX`.

The standard does not promise arbitrary parse/serialize round trips.
Initial linefeeds, raw-text delimiters and tree repair can change the
result; the maintained tests name their exclusions rather than treating
all fragments as a reversible encoding.

**Compatibility boundary.** This slice retains D2a's document parsing and
the approved historical corpus. Current customizable-select parsing is
different: in a `select` context, `<div>x<option>a<option>b` loses its
`div` wrapper here, while Chrome retains it. Adopting the current
[in-body select rules](https://html.spec.whatwg.org/multipage/parsing.html#parsing-main-inbody)
also changes document parsing and its oracle; that migration remains
compatibility work in DEBTS.md. Passing the pinned fixtures is not a claim
of full current-browser equivalence.

**Proof, 2026-10-03.** The final source passes ASan, UBSan and LSan with
leak inspection enabled:

- `tools/test_html_fragment_host.sh`: 326 focused checks; the full
  `tools/test_html_host.sh` run passes 1,218 checks and 10,525 reference
  runs. The 8,796 selected fixtures include all 192 fragment fixtures in
  both scripting modes; only four XML-output-coercion fixtures remain
  skipped. Fixture bytes and expected trees were not changed.
- Fragment failure injection visits 5,270 allocation failures and 21,504
  work cuts. Document safety retains its 91,621 allocation-failure and
  662,498 prefix/chunk checks. The 30-second mutation run completes
  134,912 mutations with document and fragment arms and varied limits.
- `tools/test_html_fragment_mutants.py`: 29 selected, 29 compiled, 29
  caught. These cover escaping, raw text, inner/outer output, void and
  template traversal, context states and insertion modes, foreign-root
  preservation, ancestor forms, inert scripts and unchanged version.
- Round trips pass for 188 of the 192 fragment fixtures in both modes
  (376 checks). The eight excluded mode runs are four named fixtures:
  `foreign-fragment.dat:50` introduces `plaintext`, which absorbs closing
  markup on reparse; `tests4.dat:3`, `:4` and `:8` have style, plaintext
  and script contexts whose returned fragment text has a non-raw parent.
  [The coverage record](../../../tools/test_html_fragment_roundtrips.json)
  names each fixture and reason. An independent unfiltered pass reproduced
  precisely those eight non-round-tripping runs.
- The DOM suite passes 363,628 checks, page 196,571, wend 177,854, way
  226, way-fetch 21, flow 18,585 and Yonder 115; garb also passes. Flow's
  twelve dumps and Yonder's six paint snapshots match. These consumer
  checks are not a new full flow fuzz or visual P5 acceptance run.

Independent Chrome spot checks match all six document serialization
examples. Context `innerHTML` after inserting a parsed fragment matches
seven of eight SVG, MathML, table, textarea, script and select examples;
the select difference is the boundary above. Compare context children
after insertion: detached text under a fragment escapes markup even when
its parse context was `script`, whereas script-parent text serializes raw.
The document CLI comparison normalizes Windows CRLF and Chrome's added
newline after the doctype, without normalizing DOM text.

The proof caught an address-dependent work count in an early staging map;
temporary node ordinals make mapping deterministic. A final audit also
caught foreign `</html>` popping the fragment scaffold and losing following
nodes; the standard's sentinel guard and focused SVG/MathML cases cover it.
The detached-form fuzz failure was a test oracle counting only visible
owners, corrected while retaining strict detached-tree invariants. The
fuzz controls now use high PRNG bits so document scripting is actually
varied. These are separate receipts, not suppressed failed runs.

**Cost.** Eleven alternating unsanitized `-O2` Wikipedia pairs against D3
have medians 22.899 ms before and 23.991 ms after (+4.77% on this host),
with identical tree, diagnostics, work (1,210,567) and node count (15,470).
Peak arena rises from 4,255,504 to 4,377,480 bytes (+121,976, 2.87%): ledger
headers gain a node tag/alignment padding and the parser gains its context
pointer. These timings establish no P5 performance claim. The adjusted
current-node helper is inline so ordinary document dispatch avoids an
additional wrapper call.

**Native acceptance.** The strict root build produces the library,
consumers, disk image and ISO. `readelf` shows only `libos64.so` as a
dependency and both new public exports; private staging helpers stay
hidden. The private QEMU run on port 55579 passes `/tests/htmltest`,
including child replacement, escaping, table/foreign contexts, capped
output and heap verification. It returns `0x48640000`; the guest library
matches the built library byte for byte (SHA-256
`5616e62aa672f409c69f0e4fc9cf46628d535f889f1fa71c01fb6713736de9d2`).
The VM is stopped. There is no independent P5 or browser scripting run.

**Review handoff.** Read the public contract in `html.h`, this section and
the allocation/attribute changes alongside the parser and serializer.
D6 must reclaim packed node payloads, text and later private attributes;
clones must not retain pointers into a reclaimed source. D5 consumes the
verbs; D3's state API remains independent.

### D3, as built

Implemented on `codex/dom-d3`, initially based on `userland` at `ae0a23d5`
and rebased onto `a90eba97` (the merged GIF improvements) for publication
in [PR #211](https://github.com/VBWizard/os64/pull/211).
`userland/libpage/state.c` owns control state;
`page.h` is the public contract. The implementation, test harness and
integration were split between Quinn and two scoped subagents, followed
by Quinn's review of the combined change.

**The ownership and API choices.** `os64_page_build(doc, url, options,
state)` takes a fourth argument. A supplied state belongs to the same
document and outlives its models; NULL creates private shared state that
ends with its last model, including rebuilt models in either free order.
Each model pins its document version. Free models, then explicit state,
then the document. Freeing state while models borrow it ends with the
PAGE badge (`0x50414745`). `os64_html_owns_node` supplies a constant-time
ownership query for live nodes, connected or detached, without exposing
the opaque document mark to libpage.

The state ceiling defaults to 16 MiB and includes the state header,
allocation headers, lookup tables, records, values, getter caches and
operation scratch. This bounds property work separately from the HTML
arena and engine heap; the owner may set a different ceiling. A build
retains default records where group normalization differs from markup,
and keeps previously established records. Property assignments may retain
records for a whole radio group or select's options. Single-node
reservations reuse spare lookup capacity; larger batches index their new
records privately, and the live table is copied when it must grow. State has
its own nonzero revision, because a property edit can leave the HTML
version unchanged. Failed operations and getter-only cache fills do not
advance it; effective changes, including person/script edit origin, do.

**What rebuilding commits.** A fresh model is built beside the old one
using the same document, URL, options and state. It grows its own arenas
rather than using the old allocation sizes as a hint. Any incomplete
candidate returns NULL. Dirty-value reconciliation, new records and
connected radio/select normalization are staged until no allocation
remains. On success, live model views publish before replaced state bytes
are freed. On refusal, the old model, state, revisions, pins and allocation
accounting remain as before the call. Detached records are retained;
options are keyed by node, so a moved option keeps its selectedness.
Radio groups are normalized over the candidate's connected controls;
members in a separate detached tree do not untick that group.

**Two property entrances.** A person's verbs retain control indices and
access restrictions. Script getters/setters take nodes, including ones
never inserted, and follow the current input value mode. Text uses dirty
state; hidden/button/tick values use the HTML value attribute; files accept
empty only; select value/selectedIndex assignments retain option identity
and may clear a size-one list. Both entrances share sanitization. Dirty
text retains whether the last assignment came from a person, because
minlength/maxlength validation requires that origin; assigning identical
bytes by script can therefore change validity without replacing the bytes.
Activation, person edits and reset refuse a changed document with STALE.
Reset restores the fresh model's normalized defaults without allocating.

**Proof, at the frozen implementation.**

- The full libpage host suite passes **247,179 checks, zero failures**
  under ASan/UBSan/LSan. The general build sweep fails each of **397**
  allocations independently; the new cases separately sweep rebuilds,
  node setters, default getters and dirty-value reconciliation. Refusals
  preserve publication, revisions, state bytes and pin counts, and release
  their allocations. The live-state-free probe stops with the PAGE badge.
- The initial independent D3 suite passed **17,926 checks**. Four 320-step walks
  keep separate value arrays, connection flags and option permutations;
  they compare state and rebuilt models after each step. The walk asserts
  that each operation and each input is exercised. Coordinator review
  caught low-bit generator bias in its first draft; the final walk uses
  high bits and covers all operations and inputs.
- **Twenty-six mutants proposed, twenty-six compiled, twenty-six caught**, using
  temporary source copies. They cover STALE, pins and release, incomplete
  rebuild rejection, normalization order, value modes, script access,
  revisions, shared publication, option identity, current-type sanitization,
  observed file clearing and assignment origin. Review regressions add
  tight-budget first touch, sparse defaults, equal-value current getters,
  the option traversal's excluded subtrees and publication to older models.
- Existing consumers retain their results: Wend **177,854/0**, libway
  **226/0**, its real-fetch integration **21/0**, Yonder **115/0** with six
  matching paints, libflow **18,585/0**
  with twelve matching corpus dumps and 597 sampled fuzz trees; libgarb's
  corpus and allocation sweeps pass. The HTML mutation suite remains
  **363,628/0**. The parser reference suite passes **10,141 runs**; its
  allocation/prefix checks, unchanged corpus snapshots and 30-second fuzz
  pass are clean. Leak checking stayed enabled; runs needing process
  inspection used approved execution outside the sandbox.
- The ownership helper leaves parser work and peak allocation unchanged.
  Nine alternating plain `-O2` Wikipedia parses on unchanged userland and
  D3 measured medians **23.374 ms** and **23.707 ms**, respectively
  (ranges 22.995–24.144 and 23.515–24.567 ms). Both report **1,210,567 work**,
  **4,255,504 peak arena bytes**, **15,470 nodes**, and identical stable
  output. These overlapping host timings do not establish a speed change.
- A scratch userland build and root image build pass. On a private QEMU
  guest, `/tests/pagetest` exercises detach/rebuild/reinsert across model
  frees, a stale changed destination, the fresh request's retained value,
  never-inserted script state, disabled assignment and the state ceiling.
  It prints `PASS D3 rebuild and script state`, passes heap verification
  and returns `0x50670000`. `/tests/htmltest` also passes (`0x48640000`).
  The guest's `libpage.so` matches the built library byte for byte. This
  proves the native seam; Yonder's scripted redraw remains D5.

**Review corrections.** First touch reuses spare lookup slots at commit;
large pending batches use a private index. An 8,000-node reservation probe
copies the live table eleven times rather than 8,000. A 140,000-option
host fixture builds and rebuilds completely with 368 bytes of state, using
a larger HTML arena for the document itself. The state ceiling still
bounds property assignments, caches and scratch. Equal assignments stamp
the current HTML version, so the next dirty getter allocates nothing.
Models and native property verbs share the HTML list-of-options walk:
ordinary containers can contain options, while option, select, datalist,
hr and nested-optgroup subtrees are excluded. The review regressions sweep
each allocation in both a small and a larger batch, retaining state,
publication and revisions on refusal. Native pagetest covers sparse
defaults, cap refusal, equal assignment and shared option numbering; its
final heap check and success badge pass with the built library installed.

Before publication, the branch was rebased over the non-overlapping GIF
merge. The root image build and full libpage/Yonder host suites were rerun
there and retain the results above.

**The handoff.** Current dirty getters allocate nothing while their HTML
version is current. After a mutation, a getter or rebuild stages
re-sanitization for the current type and constraints; observing file mode
clears prior dirty text. A final tree cannot reveal historical input-mode
transitions between observations. D5 must apply those transitions at each
attribute mutation through a state-aware, failure-atomic entrance (supplied
by D5a below), and
watch both document and state revisions when refreshing presentation.
Option insertion/removal and select `size`/`multiple` changes must also
release explicit-empty selectedness and run the select's normalization.
D6 must add holds for persistent state keys, including option/default-cache
records, and model node references, with paired releases. Pins protect
snapshot bytes; they do not replace those holds. Both requirements are in
DOM_BRIEFS.md and DEBTS.md. D3 adds neither DOM bindings nor reclamation.

### D5a, as built

Implemented in [PR #214](https://github.com/VBWizard/os64/pull/214), initially
stacked on D2b #212 at Chris's request.

`userland/libdom` binds the D5 surface to the mutable HTML document and
persistent page state. Its native contract is `include/dom/dom.h`; the
ownership, budget and error contracts are in `LIBDOM.md`. The shared library
and `/tests/domtest` are built and installed by the normal image build. This
is the library half of D5; D5b owns J3's visible Yonder acceptance.

**Objects and lifetime.** A node identity returns the same strongly held
wrapper, preserving expandos across garbage collection, detach/reinsert and model
rebuilds. Native collections expose live length/item/index access; child
collections retain their identity and tag queries refresh on HTML's revision.
Numeric property definitions, including future indices, cannot mask live
answers. PreventExtensions is refused; ordinary nonnumeric/Symbol expandos
remain available. Query refresh stages its complete replacement vector before
publication. Template innerHTML uses the contents fragment, while ordinary
child/text access follows actual children.

The binding's private function-data anchor leaves libjs's runtime/context
opaque slots untouched. Every C-retained engine value is in the drain ledger,
including the private nonextensible object used to ask QuickJS to apply its
strict-assignment refusal rules. Finalizers detach opaque slots; native records
survive engine destruction. Teardown is drain, runtime destroy, native ledger
free, models/state, document. Drain is idempotent and works after a sticky
runtime failure. Partial construction clears native slots and retained values;
the host retires that runtime without further evaluation.

**Native transactions.** `os64_html_set_attrs` stages a final qualified
attribute list, preserves order/namespaces and pinned old records, and commits
one HTML revision. Failure and final no-ops preserve accounting, including peak.
`os64_page_node_set_attr` reserves state before that commit and then publishes
control effects without allocating. Input mode changes transfer dirty text,
clear file-mode text and sanitize current constraints at each transition;
radio/select regrouping does not depend on an intervening getter.

State-aware tree entrances reserve affected records before insertion,
replacement or removal, then publish group/selection changes. Content setters
prepare detached text/fragments and use the shared replace-children entrance.
Refusal leaves visible children intact, although successful detached staging
can remain document-owned until D6. Tree methods prepare return wrappers before
making a visible change. Model presentation must observe both HTML and state
revisions; libdom does not own a rendering loop.

Mutation plans walk the moving subtrees and prepare affected old/new option
lists. Moving radios or IDs that can retarget explicit form owners require
a peer scan; the ID path is skipped when libhtml's owned explicit-input count
is zero. That count includes detached nodes and template contents; nonzero
does not establish an affected radio in the document tree. Unrelated controls
are not staged. Clean
textarea state is reset for direct child changes. Reserved records carry
temporary stage links, cleared on commit/refusal, so lookup does not scan the
stage list. Unrelated tree edits allocate no control state and skip model
publication. Template contents stay independent when their host moves.

Cloning delegates to `os64_page_node_clone`, preserving INPUT current value,
checkedness and their dirty flags, and TEXTAREA current value/dirty flag.
Other control state initializes from cloned markup. Copied state bytes belong
to the clone; source getters/caches are not changed while preparing it. A
shallow clean textarea keeps its current value without copied text children.
Its direct child changes update that clean value, while unrelated
mutations leave it alone. The binding routes CharacterData through
`os64_page_node_set_text` and tree changes through the shared entrances so
change-and-restore histories are observed before a later getter. Direct native
libhtml calls reconcile observed text but cannot reconstruct such histories.
Clone preparation includes the return wrapper; state refusal leaves source
state, published values and both revisions unchanged, with detached HTML
preparation charged to the document as above. Success advances neither revision.
Textarea defaults use direct Text children, including the model/reset path.
The cloning rules follow the HTML Standard's [input cloning steps](https://html.spec.whatwg.org/multipage/input.html#the-input-element)
and [textarea cloning and children-changed steps](https://html.spec.whatwg.org/multipage/form-elements.html#the-textarea-element).

**Strings and refusals.** UTF-8 conversion replaces NUL/lone surrogates with
U+FFFD and preserves valid surrogate pairs. Element and attribute names use
the DOM Standard's separate validation predicates. Nullable nodeValue and
textContent map null/undefined to empty; innerHTML's null maps to empty while
undefined is a DOMString. Native errors have own nonenumerable name/message
properties, so inherited setters do not run while reporting a refusal. A
DOMException constructor is outside this surface. Confirm returns false,
prompt null; alert borrows its message during a synchronous host callback.

The binding-owned default is 8 MiB; explicit zero is a literal ceiling.
Document, page-state and engine budgets are separately enforced. Native
allocation/depth/work ceilings become catchable QuotaExceededError; engine
exhaustion during evaluation follows libjs's sticky LIMIT/MEMORY result.
The idle installer cannot distinguish engine-cap refusal from host OOM and
reports construction failure after consuming pending exceptions. Closed
bindings and nonempty file-input assignments report InvalidStateError.

**Independent proof at the initial source freeze.**

- `tools/test_dom_host.sh`: actual target engine 1,463 checks / 0 failures,
  88 reached allocation cuts; separately ASan-instrumented target-profile
  engine 3,419 / 0, 414 reached cuts. Binding/tree/state/runtime are sanitized
  in both profiles; upstream engine UBSan is excluded in the second profile.
  Normal leak detection is enabled. Native installer, full constructor and
  seven binding mutation/read/refresh operations are swept, plus 42 native
  clone and two CharacterData refusal points. Focused clone mode is 877 / 0
  in each profile, including clean/dirty current values, template controls,
  direct-child defaults, Comment histories, cap refusals and borrowed bytes.
  One sanitized constructor cut succeeds coherently after a nonessential allocation is refused; its
  pending exception is empty and teardown remains clean.
- `tools/test_dom_mutants.py`: all 26 proposed mutants compile and are caught
  by relevant assertions. No build failure or timeout is counted as a catch.
- New attribute histories: 154 / 0; tree histories: 2,133 / 0. Full libpage:
  198,858 / 0, including allocation sweeps. The existing HTML DOM suite is
  363,628 / 0; focused fragment checks are 326 / 0.
- Full HTML gate: 1,218 native checks / 0; 10,525 reference runs pass, with
  four existing XML-coercion skips. Safety covers 91,621 allocation refusals
  and 662,498 prefix/chunk checks. The 30-second fuzz run makes 132,352
  mutations with four chunkings. Corpus trees/work/peak remain unchanged.
- Downstream gates: wend 177,854 / 0 with all seven saved dumps matching;
  libway 226 / 0, libflow 18,585 / 0 and all 12 dumps matching, Yonder
  115 / 0 and six paints matching. The libgarb suite and sweeps pass.

Before the control-cloning correction, a full paired libflow fuzz comparison
with untouched D2b completed 55,077 layout trees and 128,510 checks per revision.
Both produced the same three existing textfiles-computers geometry failures;
neither produced an ASan/UBSan/LSan error. Eight bounded shares per revision
covered the original all-page input and RNG sequence, with a temporary driver
dispatch for `--fuzz all K 8`; the normal per-page share dispatch would skip
other pages' RNG advances and compare a different sequence. Final page and
normal consumer gates cover the later clone/textarea changes separately.

**Initial target proof.** Strict root build/link/image creation passes. In a private
copied-disk QEMU guest, `/tests/domtest` runs four document/runtime lifetimes,
changes text/markup, retains wrapper expandos and a live child collection,
preserves a person's field value across model rebuilding and type changes,
checks clean/dirty control clones and child mutation histories, drains twice
and passes heap verification (`0x446f0000`). `/tests/htmltest`
adds atomic attribute publication/refusal/no-op/pinned-byte checks and passes
(`0x48640000`); its separate pinned-document misuse still terminates with
the HTML badge (`0x48544d4c`). Deployed libdom/libhtml/libpage/libjs all match
the built libraries byte for byte. libdom's SHA256 is
`6e90a2b50d4f86f5d1edbebc4b031afbe2055376e3544198691af5b34fbe8e2b`.
The owned VM is stopped. This establishes the library seam, not a P5 or
visible Yonder scripting result.

**Parent review integration.** The D3 review corrections reuse spare lookup
capacity and retain normalized defaults sparsely. Attribute and clone plans
use the reservation lookup for records that are still private; the former
full-table assumption was wrong once reservations could reuse live capacity.
Option attributes use the shared list-of-options exclusions, so changing an
excluded nested option does not normalize the outer select. Regression cases
cover explicit-empty selection after insertion, removal, `size` and `multiple`
transitions, including an excluded nested option.

At parent review merge `d7acdf10`, the full libpage suite passed 249,491 checks
/ 0 failures. The binding suite passed 1,447 / 0 with 87 reached allocation cuts
on the actual target engine, and 3,403 / 0 with 413 cuts on the instrumented
engine; clone refusal has 41 allocation points. The smaller sweep counts
reflect fewer reservation allocations. Normal leak detection remains enabled.
Both changed D3 mutation anchors compile and are caught. The strict userland
build passes; private QEMU `pagetest` and `domtest` return their success badges
and pass heap verification. The installed libpage/libhtml/libdom and both test
executables match the build byte for byte. This guest is stopped.

**D5a review corrections.** The mutation planner retains sparse state for
unrelated edits and uses indexed stage lookup. It prepares affected select
lists, named radio peers and clean textarea parents. ID moves are included
because they can retarget an explicit form owner even without a moving control.
The tests cover a control-free form move, separate template contents, no state
allocation for unrelated edits on 20,000 options under a 1 KiB state cap, and
refusal followed by retry with existing records. Temporary stage links are
cleared before their storage is released.

An independent plain-O2 host probe, with a live model and an explicitly enlarged
HTML arena, measures one text insert/remove pair at the normal 16 MiB state cap:

| Options | Before | After | Allocations after |
| --- | --- | --- | --- |
| 1,000 | 2.540 ms | 3 microseconds | 0 |
| 10,000 | 428.712 ms | 7 microseconds | 0 |
| 60,000 | NO_MEMORY after 12.157 seconds | 24 microseconds | 0 |

These are individual host measurements. The current 60,000-option pair retains
384 state bytes; the larger record includes its temporary stage link. A real
10,000-option-list insert/remove pair drops from 1,289.068 ms to 21.198 ms in
the same probe. A touched list still spends state budget on its records/stages;
this does not remove the cap or establish P5/Yonder scripting timings.

CharacterData `data = null` now clears the string, while undefined still
stringifies. Kind prototypes inherit Node, and HTML control prototypes inherit
Element, so unsupported properties are absent rather than throwing during a
normal lookup; `in` reports the supported surface. The registry drains retained
prototypes alongside wrappers, and clone preparation chooses its source kind's
prototype before native publication. LIBDOM.md lists the property scopes and
the constructor/full-DOM boundary.

Round-1 correction proof at `839e6801`: full libpage 254,057 / 0 with 397
independently failed build allocations; binding target profile 1,527 / 0
with 101 reached allocation cuts,
instrumented profile 4,227 / 0 with 551 cuts, including 40 clone refusal points.
ASan/UBSan/LSan remain enabled. All 29 DOM, six semantic tree and 26 D3 state
mutants compile and are caught; the tree checks do not count removal of a
performance-only shortcut as a semantic catch. Normal consumers pass: Yonder
115 / 0 with six matching paints, wend 177,854 / 0 and libflow 18,585 / 0 with
twelve matching dumps. The strict userland build passes. Copied-disk
QEMU `pagetest` and `domtest` return their success badges and pass final heap
checks, including native null/property-detection cases. Installed libraries and
tests match their built bytes, and the owned guest is stopped.

**ID planning without explicit owners.** Libhtml counts owned HTML inputs
with an unnamespaced `form` attribute. Creation by the parser, clones and
packed fragment copies adds contributions; attribute commits update them;
fragment publication transfers its staged count. Attribute/fragment refusal
leaves the owner's count unchanged. Detached nodes, template contents and
retained clone preparation still count. Zero lets the tree planner omit its
ID set and explicit-owner scan; nonzero remains a conservative trigger rather
than an index of connected radios. D6's brief records the physical-reclamation
decrement and the nonexclusive inline/private attribute flags.

An independent O2 host probe uses a live model and repeatedly inserts/removes
a fresh `div id=moved` into body. These are medians of seven batches of 100
pairs. Both variants use the new HTML metadata implementation; the before
variant uses `839e6801`'s planner to isolate this guard's effect.

| Fixture | Owned nodes | Before pair, microseconds | After pair, microseconds | Allocations after |
| --- | --- | --- | --- | --- |
| Wikipedia | 15,471 | 163.01 | 0.13 | 0 |
| 1,000 options | 2,006 | 11.71 | 0.13 | 0 |
| 10,000 options | 20,006 | 120.82 | 0.13 | 0 |
| 60,000 options | 120,006 | 2,333.64 | 0.14 | 0 |

State bytes remain 80 for Wikipedia and 384 for the option fixtures. These
are warm host measurements, not P5/Yonder scripting timings. The count adds
one size word per document: the Wikipedia corpus keeps its tree, 15,470
parsed nodes and 1,210,567 work units, with peak arena 4,377,488 bytes.

Round-2 proof: native HTML DOM 451,748 / 0, including an independent count
oracle across the random walk; full libpage 254,057 / 0 with 397 failed build
allocations; DOM target/instrumented profiles 1,527 / 0 and 4,227 / 0 with
101/551 reached allocation cuts. Seven count-update mutants and eight tree
mutants compile and are caught, including the zero-count ID allocation gate
and the retained explicit-owner scan. Full HTML passes 1,218 / 0, 10,525
reference runs, allocation/corpus gates and 135,680 bounded fuzz mutations.
Normal consumers pass: Yonder 115 / 0 with six matching paints, wend
177,854 / 0 and libflow 18,585 / 0 with twelve matching dumps and 597 sampled
fuzz trees. The strict userland build passes. Copied-disk QEMU pagetest/domtest pass,
including native attribute/clone/detached-fragment counts, four runtime
lifetimes and final heap verification. Installed libraries/tests match their
build bytes; the owned guest is stopped.

**Parser cost at the initial D5a freeze.** Seven alternating O2 host parses
per revision, after warmup, compared with untouched D2b: Wikipedia medians
24.365 ms before and 24.377 ms here (+0.049%). Observed ranges were
24.214–39.585 ms and 24.164–24.645 ms;
the difference was below sample variation. Both produced the same tree,
15,470 nodes, 1,210,567 work units and 4,377,480 peak arena bytes. This is
a host parser measurement, not Yonder/P5 scripting performance.

**Next seam.** D5b below seats the library in Yonder and adds cascade/layout
pins before changing a tree beneath those snapshots. D6 adds paired holds for wrapper keys, query roots, state/model keys
and either cached query answers or a proven version-before-dereference rule.
New control-state reservations need their holds before calling a tree verb,
with releases on abort, because normalization reads those keys after mutation.
Strings copied into JavaScript hold no native snapshot bytes. Libjs retains
R0's fatal destroy invariant; reporting/reclaiming destroy is a separate
reviewed slice before scripting is offered for ordinary browsing. D4 and D7
are Fable's slices.


### D5b, as built

**Implemented 2026-10-03; awaiting Fable review.** Yonder links libdom/libjs
and runs a bounded finished-document fixture. The loader finishes HTML on a
worker with the chosen scripting policy, resuming parser script stops without
executing them. The window thread queues connected HTML inline classic scripts
in tree order and runs at most one per turn, followed by its budgeted Promise
checkpoint and one native rebuild. This does not implement D4/D7 execution
order: external `src`, module, async/defer scheduling, inserted-script
execution, DOM events, timers, document.write, geometry and location APIs are
outside this slice. A type absent or empty, or exactly `text/javascript`,
`application/javascript`, `text/ecmascript` or `application/ecmascript`, qualifies;
other types and template contents are excluded.

**The switch.** Settings adds **Run page scripts**, default off. Apply changes
this window; Save as default writes `scripts = on/off` to `yonder.conf`.
Missing or unrecognized values leave scripts off. A changed setting cancels
in-flight navigation and queued scripts, destroys the runtime, then requests
reload with the new parser mode. POST reload retains the resend question;
refusing it leaves the already drawn page with no remaining script runtime.
An empty window waits for its next page. The status line begins **SCRIPTS ON**
while enabled. The document's captured mode also reaches `flow_env_t.scripting`,
so raw `noscript` fallback text is hidden when enabled and ordinary fallback
markup is parsed and drawn when disabled. The agent list scrolls within one
fewer nominal row so the added switch fits the existing settings window.

**Owners and snapshots.** The runtime is created lazily before the first
runnable script. Its native queue holds nodes, not JavaScript values. Page
state and document outlive the runtime. Departure, mode change and window
close drop the queue, drain binding registries, destroy the engine, free binding
records, then release widgets, layout, cascade, models/state and document.
Libjs's fatal destroy invariant and libhtml's pinned-document refusal remain.
`os64_page_retain/free` reference models on their owner thread; a flow tree
retains the model whose indices its boxes carry and exposes it through
`flow_model`. Flow and cascade independently pin their borrowed document bytes.
The caller still keeps the document, cascade inputs, fonts and text context
alive until their borrowers are released.

**Publication.** HTML version and page-state revision are observed separately.
A failed model rebuild preserves the old drawing and its native STALE action
gate. Once a new model exists, a failed cascade/layout leaves older geometry
and its retained model alive. Links and controls resolve through native nodes
before consulting current meaning; detached old links do nothing. Resize and
sheet completion cannot publish layout while a DOM rebuild is owed. Failures
retry on another external event before running the next script, without a
self-generated retry loop. Successful HTML changes stage fresh sheet metadata,
cascade and flow beside the old snapshot, then publish them. Ready linked CSS
parses, including imports and duplicate links, transfer ownership after their
old cascade is released; text mutations therefore preserve loaded CSS without
refetching it. A parse has one staged borrower; abort leaves its old owner intact.
Stylesheet metadata retains the loader's omission-on-allocation-refusal policy.
Page scroll offsets are retained and clamped; box scroll positions remain node
keyed. Node-based page scroll anchoring across DOM changes is not added.

**Controls and pictures.** Widgets are individually allocated and node keyed.
Surviving controls preserve widget identity, focus, caret, selection and typed
buffers across index changes and even a partial layout omitting their boxes.
Before a script or activation, changed editor buffers flush to shared state.
Native-value changes refresh editors; unchanged projected values leave partial
numeric edits alone. State-only property changes refresh controls without
rebuilding HTML geometry. Widget allocation refusal preserves the old list;
missing new widgets can be retried by a subsequent HTML rebuild. Picture maps
are rebuilt against current model indices, with bounds checks while older
geometry remains. Catalog URLs are owned rather than borrowed from a replaced
model; decoded pictures and pending work survive unrelated changes.

**Bounds and outcomes.** The schedule refuses more than 4096 qualifying
scripts. Each script source is copied before evaluation, capped at 4 MiB, so
self-modification cannot invalidate it. Runtime heap/stack/source use the
measured 64 MiB/256 KiB/4 MiB profile; this fixture chooses a one-second deadline
and 4096 jobs per turn, with the binding's default 8 MiB cap. The one-second
wall-clock limit covers evaluation and its Promise checkpoint; it is neither
a browser-standard threshold nor a page-lifetime allowance. The ordinary
browsing default and a possible Settings control belong to D7's execution-time
policy decision above. Exceptions and
unhandled rejections report inline source diagnostics and allow later scripts.
Limit, cancellation and host failure retire the remaining schedule and runtime.
The per-page picture catalog admits at most 4096 URLs and 4 MiB of owned URL
bytes; existing catalog entries remain usable at the cap. Console output uses
stdout; alert reports on the status line.

**Evidence.** `tools/test_yonder_scripts_host.sh` exercises the actual Yonder
queue, publication and widget code with real libui editing and native libraries,
a target-profile engine, ASan/UBSan and normal LSan. It passes **1473/0** checks:
one task per turn, wrapper identity, a real key edit and caret/selection/focus,
control reorder/kind/hidden transitions, state-only refresh, invalid numeric
input, current-link routing through older geometry, navigation cancellation,
job/source/schedule caps, URL ownership and setting persistence. A 160-cut
native sweep reaches 18 model refusals and 21 layout refusals, with 121 completed
rebuilds; a separate 64-cut stylesheet sweep covers duplicate parses/imports,
rollback and retry. The ledger is empty after teardown. Worker services and
fixture transport are hosted; this suite does not simulate a compositor or
claim guest timing performance. The real fetch suite adds a multi-chunk HTML
case with scripts before/after the read boundary: **27/0**. Both libdom profiles
pass **1527/0** and **4227/0** with sanitizers; libpage rebuild **68328/0**, libflow
**18585/0** plus matching corpus dumps, libgarb's parser/cascade/allocation suites,
and the painter **115/0** plus six matching paints pass. Strict userland build
passes with bounded `-j4` concurrency.

A private QEMU image/server loads `tools/yonder_script_fixture/index.html`.
The first and final inline scripts change the visible heading to **Two JavaScript donuts!**;
a real typed **Q** remains and the held wrapper matches after redraw. The manual
fixture includes twenty 300 ms turns with a countdown, giving about six seconds
for input without increasing the runtime deadline. Navigation
from `cancel.html` discards its queued sentinel and shows `quiet.html`.
`yonder --script-audit URL` reports **heap problems=0** on scripted-page retirement:
this is heap integrity, while the host ledger/LSan and libjs destroy invariant
supply the leak checks. Settings Apply/Save, saved-off startup, enable/reload,
off-mode fallback, and on-mode fallback suppression were checked on the guest.
The 21 installed library/app/test/fixture files were byte-compared with their
build inputs. Guest `domtest` passes four lifetimes; `pagetest` and `htmltest`
pass, and `htmltest pinned` still exits with the HTML badge (`0x48544d4c`).
No P5 scripting or ordinary-site compatibility evidence is claimed.

**Remaining gates.** D6 detached reclamation, D7's event loop/execution order,
and reporting/reclaiming runtime teardown remain separate reviewed work before
recommending this switch for ordinary browsing. D4 and D7 remain Fable's slices.

## Booked, with their triggers

| Debt | Why it waits | Trigger |
|---|---|---|
| Node reclamation beyond D6 (a weak wrapper table, the collector deciding) | its own design; D6 covers the common case | a page whose own created-and-dropped elements meet the budget |
| Invalidation finer than "the tree moved" | whole rebuilds are correct; this is speed | a page whose scripted updates are visibly slow |
| Incremental relayout | LAYOUT.md's own row | the forced-layout count above |
| An id index kept by the verbs | `getElementById` as a walk is right and linear | a profile that shows it |
| An empty document; a root that is not `html` | libflow's root and `doc->html`'s promise | a page that needs one |
| `document.write` after the parse has ended | it replaces the document | a page that needs it |
| `unload` and `beforeunload` | teardown must be bounded | a ruling that a page may delay leaving |
| Namespaced attribute verbs | no caller until SVG is scripted | that |
| Script execution-time default/range and a possible Settings control | the one-second D5b deadline bounds the fixture; ordinary browsing needs workload evidence and a reviewed policy | D7, including P5 measurements and Apply/Save behavior |
| A text field's change callback in libui | `input` and `change` events need it | D7 |
| A modal `alert`, `confirm` and `prompt` | each needs a loop nested inside a script; the first cut answers without waiting | a page that cannot be used without a real `confirm` |
| A process per page | a different browser | tabs |

## Ruled by Chris, 2026-10-01

These were a person's to answer, so they were asked of Chris. He took the
lean on each.

1. **The switch.** A box in Settings, "Run page scripts", and a word on
   the status line while it is on, as POSITIONING OFF is said today. Off
   by default until D7's evidence is in. The reason it needs a switch at
   all: with scripting on, `noscript` fallbacks vanish, so a site that
   serves a working plain page to a browser without script serves this
   one a script it may only half run. DuckDuckGo's results are exactly
   that case.
2. **`alert`, `confirm`, `prompt`.** Each is a modal wait in the middle of
   a script, and yonder has no nested loop by design. So `alert` shows its
   text in the question bar and the script carries on; `confirm` answers
   no; `prompt` answers nothing. A modal bar that pumps only its own
   buttons while the script waits is closer to what the old web expects,
   and is the nested loop Y3 was careful not to have. It is booked, with
   the first page that cannot be used without a real `confirm` as its
   trigger.
3. **The binding library's name** stays `libdom`. (`libsub` was offered,
   with a smirk.)
4. **The review tier.** D1 and D2 get an outside round; the rest is
   reviewed here.

## Review record

**Quinn, 2026-10-01: four findings, all correct, all taken.**

| Finding | What it was | Where it landed |
|---|---|---|
| P1 | Teardown destroyed the parser at step 2, and `os64_html_parser_destroy` frees the document with it (`core.c`), under the wrappers, the runtime and the snapshots that steps 4 to 6 still use | `os64_html_parser_abandon`, in § The parser with scripting on and in the teardown order |
| P2 | Edits kept only for connected controls: a value set, the control detached, a rebuild, the control put back, and the value was gone; a detached control had no working value at all | § Snapshots: control state belongs to the node and is its own object |
| P2 | `input.value` routed through the door for a person's edits, which refuses disabled, readonly and hidden controls a script may assign to | § Snapshots: script-facing setters, by value mode |
| P2 | Form-owner records cleared only on a move; changing a control's `form` attribute and removing it again resurrected the parser's association | § The verbs: a record is cleared by the mutation that invalidates it |

The fourth was one case of a class. My first text also had libpage *ignore*
a record whose form had left the control's tree, which resurrects the
same way when the form comes back. That is replaced by the same rule:
cleared at the mutation, never re-validated by a reader.

**Codex, 2026-10-01, on D1 (PR #190): seven findings over three rounds, six taken.**

| Finding | What it was | What was done |
|---|---|---|
| P1 | `clone` given another document's node shared that document's names, identifiers and attribute records, which dangle when it is freed | A node carries its document's mark. `clone` copies everything when the source is another document's; every other verb refuses a node that is not its own document's, since the same hazard sat behind each of them |
| P1 | A pin's number was its slot, so a stale release could let go of the pin that took the slot next | A number is slot and serial, unique in the program; a stale one, or another document's, ends the program |
| P2 | The document's own element or doctype cannot be re-inserted under it | Not changed: it is the standard's rule and Chrome's behaviour, now stated in `html.h` and held by a case |
| P2 | Moving a template cleared the record of a control and form that sat together in its contents | The form-owner walks stay out of template contents, which are a tree of their own |
| P2 (second round) | Inserting an empty fragment changed nothing and still moved the version, which is every snapshot's signal to rebuild | It answers OK and the version stays |
| P2 (third round) | A node inserted where it already sits moved the version too | The rule is now whole: the version moves when a reader could see a difference and at no other time (§ The version), and the random walk's model predicts which for every step |
| P2 (third round) | The document mark was a 32-bit counter that came round, so two live documents could share one | The count does not come round; when it is spent no further document is made |

## What was checked, and what was not

Read for this: `userland/libhtml` (`core.c`, `internal.h`, the tree
builder's insertion and text paths), LIBHTML.md, LIBPAGE.md and libpage's
`internal.h`, `flow.h` and libflow's text items, libgarb's `cascade.h` and
`select.h`, libway's `way.h` and `load.c`, yonder's page, arrival, trip,
mailbox, ticker and doorbell code, `os64/work.h`, YONDER.md, and in the
engine `JS_FreeRuntime`, `JS_NewRuntime2` and the raw allocation sites of
the five files.

Not checked, and owed by the slice that depends on each: whether libgarb's
and libflow's tree walks are bounded by their own caps or by libhtml's;
how much of a model libflow borrows beyond the indices; the cost of a parse in slices
in the guest; libflow's stack depth under a script frame; and every claim
in § A leak at teardown beyond the lines cited, which is why that section
ends in an acceptance list and not a verdict.
