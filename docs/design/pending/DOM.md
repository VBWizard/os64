# DOM.md — one tree, and a script that may change it

*Written 2026-10-01 by Fable. This is packet D0 of
[JAVASCRIPT_TASKS.md](JAVASCRIPT_TASKS.md): the design J3 waits on. D1, D2a
and D3 are built (§ Slices); the remaining slices are proposals. Read
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
| `parse_fragment(doc, context, utf8, len, limits)` | the standard's fragment algorithm, into a detached fragment |
| `serialize(node, children_only, out, cap)` | the standard's serialisation, in `snprintf`'s shape |

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
every newly in-scope case passes, whole, byte at a time and in random
chunks.

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
| D2b | Fragment parsing and serialisation | The `#document-fragment` cases un-skipped and passing, whole and chunked |
| D3 | **Built; review pending.** `os64_page_rebuild`, pinned models, the `STALE` gate, shared node state and script property APIs | `tools/test_libpage_rebuild.inc`: detach/reinsert, never-inserted state, value modes, option identity, current-type sanitization, person/script origin, pins, transactional allocation sweeps and independent random walks; § D3, as built |
| D4 | The stream: yonder parses on its own thread. No script | yonder's and libway's harnesses unchanged in result; the guest walk that proved Y3, its server log identical; the window live through a stalled body |
| D5 | The binding library and J3's fixture | A script changes text and the page redraws; a held reference and a typed-in field survive an unrelated change; a navigation with a script queued tears down clean; the leak count is zero |
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

### D3, as built

Implemented on `codex/dom-d3`, initially based on `userland` at `ae0a23d5`
and rebased onto `a90eba97` (the merged GIF improvements) for publication;
Review and merge are pending in [PR #211](https://github.com/VBWizard/os64/pull/211).
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
arena and engine heap; the owner may set a different ceiling. State has
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

- The full libpage host suite passes **196,571 checks, zero failures**
  under ASan/UBSan/LSan. The general build sweep fails each of **402**
  allocations independently; the new cases separately sweep rebuilds,
  node setters, default getters and dirty-value reconciliation. Refusals
  preserve publication, revisions, state bytes and pin counts, and release
  their allocations. The live-state-free probe stops with the PAGE badge.
- The independent D3 suite passes **17,926 checks**. Four 320-step walks
  keep separate value arrays, connection flags and option permutations;
  they compare state and rebuilt models after each step. The walk asserts
  that each operation and each input is exercised. Coordinator review
  caught low-bit generator bias in its first draft; the final walk uses
  high bits and covers all operations and inputs.
- **Sixteen mutants proposed, sixteen compiled, sixteen caught**, using
  temporary source copies. They cover STALE, pins and release, incomplete
  rebuild rejection, normalization order, value modes, script access,
  revisions, shared publication, option identity, current-type sanitization,
  observed file clearing and assignment origin.
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

Before publication, the branch was rebased over the non-overlapping GIF
merge. The root image build and full libpage/Yonder host suites were rerun
there and retain the results above.

**The handoff.** Current dirty getters allocate nothing while their HTML
version is current. After a mutation, a getter or rebuild stages
re-sanitization for the current type and constraints; observing file mode
clears prior dirty text. A final tree cannot reveal historical input-mode
transitions between observations. D5 must apply those transitions at each
attribute mutation through a state-aware, failure-atomic entrance, and
watch both document and state revisions when refreshing presentation.
D6 must add holds for persistent state keys, including option/default-cache
records, and model node references, with paired releases. Pins protect
snapshot bytes; they do not replace those holds. Both requirements are in
DOM_BRIEFS.md and DEBTS.md. D3 adds neither DOM bindings nor reclamation.

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
