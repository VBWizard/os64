# DOM_BRIEFS.md — handing out the DOM slices

Status: briefs for the slices of [DOM.md](DOM.md) that other builders take
on, written 2026-10-02 by Fable at Chris's request. DOM.md stays the
design and the acceptance; this file says what a builder who did not write
that design needs in order to build one slice right, and it names what was
left undecided on purpose. When a slice is built, its "as built" section
goes in DOM.md and its brief here is struck.

| Slice | Builder | Reviewer | After |
|---|---|---|---|
| D2b fragments and serialisation | Opus | Fable, then an outside round (Codex) | nothing: D2a is merged |
| D3 — **Built; review pending** | Quinn and two scoped subagents | Fable | [D3 as built](DOM.md#d3-as-built) |
| D6 reclaiming unheld detached subtrees | Opus | Fable | D2b (its churn driver) |
| D5 the binding library and J3's fixture | Quinn | Fable | D2b and D3; D4 is not needed |
| D4 the parser on the window's thread; D7 the loop | Fable | — | D4 any time; D7 after D5 |

D4 and D7 stay with Fable because their sections of DOM.md are findings and
leans, not specifications, and a wrong call there is a hunt rather than a
review round. The remaining briefs below are for slices whose rules are already
written down.

## What every DOM slice does the same way

**Read first, in this order.** DOM.md whole, then the "as built"
sections at its foot (D1, D2a and D3), which record the implemented
contracts and proof. The contract blocks in
`userland/libhtml/include/html/html.h` (THE PARSE THAT STOPS, CHANGING A
DOCUMENT): the header is the contract and the test harness holds the code
to it a sentence at a time. LIBHTML.md (the parser's own design and its
skip-list rules), LIBPAGE.md for D3 and D5, JAVASCRIPT.md § Browser
integration requirements and the D rows of JAVASCRIPT_TASKS.md. CLAUDE.md's
opening rules about comments and review rounds, and AGENTS.md.

**Branch and PR.** Branch from `userland` (D2a merged there as #199,
`8016dd43`). Open the PR against `userland`; Chris merges. Nothing is
committed on `userland` directly. A slice that must build on an unmerged
slice branches from that slice's branch and says so in the PR body; at
most three such PRs are open at once.

**The design doc ships with the code.** The same PR adds a `### Dn, as
built` section to DOM.md in the shape of the two there, moves the slice's
row in DOM.md § Slices to **Built**, and updates its row in
JAVASCRIPT_TASKS.md. A debt the slice creates or pays is a DEBTS.md row in
the same PR. A deferral the builder plans is discussed with Chris before
the code is written, not discovered in review.

**Comments say WHY and HOW, now.** No counts of other places, no "the only
caller", no history (that goes in the commit message). Every comment the
diff touches or makes false elsewhere is re-read before the PR opens.
`tools/stale_refs.sh` runs before any commit that renames or removes a
name. The numbers a slice measured go in its as-built section, not in
code comments, where they would go stale.

**The proof, in the house's shape.** Each of the two built slices did all
of these, and the harness is where a reviewer looks first:

- **A host harness under ASan and UBSan**, driven by a `tools/test_*_host.sh`
  that builds with plain `cc` and runs in under a few minutes. libhtml's is
  `tools/test_html_host.sh` (the reference cases through
  `tools/test_html_driver.c`, the corpus, the fuzz pass) and
  `tools/test_html_dom_host.sh` (the verbs); libpage's is
  `tools/test_libpage_host.sh` with its `.inc` case files.
- **Cases by hand, one per sentence of the contract.** When the header says
  "a verb either happens or changes nothing a reader can see", there is a
  case that makes the verb fail and checks that nothing changed.
- **Every allocation failed in turn.** The driver counts the allocations a
  call makes, then runs it again failing the first, the second, and so on,
  checking after each that the refusal is named, nothing a reader can see
  moved, and every byte taken was given back. The D1 and D2a sections say
  what that found.
- **Random walks against an independent oracle.** The D1 harness keeps its
  own model of the tree, written from the standard with arrays, and after
  every random verb compares every node it knows with the real one. The
  D2a harness predicts nothing and instead checks invariants after every
  step (no cycle, links agree both ways, the landmarks by their
  definitions, nothing past the depth limit, a pinned string still reading
  as it did, an empty heap after the free). Either shape works; pick the
  one whose oracle is cheap to write and expensive to fool.
- **Mutants.** A script applies one deliberate break to the finished code,
  rebuilds, runs the harness, and records whether the break was caught.
  One for each rule the slice adds. A break the harness misses means a
  test is missing, and the test gets written; the counts (built, caught,
  what the one that was not caught turned out to be) go in the as-built
  section. The D2a run was sixty mutants, fifty-nine caught, and the misses
  of its first pass are where that harness grew.
- **Consumers unchanged.** The host harnesses of everything that links the
  library run before the PR opens and their results do not move:
  `tools/test_libpage_host.sh`, `tools/test_wend_host.sh`,
  `tools/test_garb_host.sh`, `tools/test_libflow_host.sh`,
  `tools/test_way_host.sh`, `tools/test_yonder_host.sh`, plus the fuzz and
  corpus passes. A libhtml change is also measured: the Wikipedia corpus
  page's parse time and the parser's peak arena are in the D2a section,
  and a slice that moves them says by how much and why.
- **In the guest.** `/tests/htmltest` (`userland/tests/htmltest`) is the
  libhtml fixture `testrun` spawns; a slice adds a function there that
  drives its new calls through the real library on the real heap, and the
  PASS line names it. libpage's and yonder's guest proof is a page served
  by a local server and the server's access log, or a screenshot through
  the vm tools, as LIBPAGE.md § Proof before integration describes. Boot a
  scratch copy of the image, never the one on Chris's port.

**Review.** Fable reads every DOM PR before it merges, with the harness
output in the PR body. D2b then gets an outside round because it is
lifetime work in a library fed by whatever a server sends (DOM.md, ruling
4); Chris requests it. D3, D5 and D6 are reviewed in house, with D6's
reviewer free to ask for an outside round, since reclamation is the
use-after-free class. When handing a PR to a reviewer, name the DOM.md
sections and the header contract it implements; a reviewer reading code
without the contract files findings the contract already answers.

**Decisions.** Where a brief says "the builder decides", the builder
decides, writes the reason in the header's contract block, and the
as-built section records it. A question about web-platform semantics is
answered from the standard and Chrome's behaviour and told to Chris; a
question about what os64 wants is asked of Chris with a lean.

## D2b — fragments and serialisation

**What it is.** The two verbs DOM.md § The verbs lists and D1 left out:

```c
os64_html_node_t *os64_html_parse_fragment(os64_html_document_t *doc,
                                           const os64_html_node_t *context,
                                           const char *utf8, size_t len,
                                           bool scripting, int64_t *status);
size_t os64_html_serialize(const os64_html_node_t *node, bool children_only,
                           bool scripting, char *out, size_t cap);
```

(The shapes are a starting point; the builder owns the final spelling.)
The first is the HTML Standard's "parsing HTML fragments" algorithm with a
context element, which is what `innerHTML` assignment needs: the result is
a detached FRAGMENT node owned by `doc`, its children the parsed nodes,
ready for `os64_html_insert` to move into the tree. The second is the
standard's "serialising HTML fragments": `children_only` set is
`innerHTML`'s getter, clear is `outerHTML`'s. It writes at most `cap`
bytes, always NUL-terminates when `cap` is nonzero, and answers the length
the whole output needs, the `snprintf` shape, so a caller sizes a buffer
by calling it twice.

**Read.** DOM.md § The verbs, § The parser with scripting on (the last two
paragraphs), and § D2a, as built. LIBHTML.md § Out, by name (the fragment
bullet is what this slice removes). The standard: HTML § 13.4 "Parsing HTML
fragments" and § 13.3 "Serialising HTML fragments", read against the
WHATWG living standard, not a snapshot. `tree.c`'s `reset_mode` (the
standard's "reset the insertion mode appropriately" has a fragment branch
that reads the context element) and `tokenizer.c`'s initial state.

**Rules already settled, so they are not reopened:**

- **A fragment parse never stops.** A script inserted through `innerHTML`
  never runs, so the parser has no one to wait for: a `script` element is
  built like any other and `OS64_HTML_SCRIPT` is never answered. The
  `scripting` argument exists for `noscript`, whose parse differs by it,
  and the binding passes the page's mode.
- **One string, parsed whole.** `innerHTML` is a string, so there is no
  streaming form, no encoding sniff, no byte-order mark and no `<meta
  charset>` (it parses as an ordinary element). The input is UTF-8 and is
  decoded by the parser's own UTF-8 decoder, so an invalid sequence and a
  NUL become U+FFFD as they do in a document.
- **The nodes belong to `doc`.** Every verb refuses another document's
  node, so the fragment's nodes are made in `doc`'s arena and charged to
  its budget, and `context` must be `doc`'s (`OS64_HTML_BAD_ARGUMENT`
  otherwise). The standard's algorithm builds under a fresh `html` root in
  a fresh document; here the root is a detached element of `doc` that the
  parse uses as its root and never links under the document (the document
  keeps one element under it, and that promise is the verbs'). The nodes
  of that scaffolding stay charged until the document is freed, as any
  node does; D6 is where unheld detached nodes come back.
- **A detached result does not move the version.** Creating detached
  nodes is invisible to a reader of the tree, and the verbs that create
  (`create_element`, `clone`) leave the version alone today. The insert
  that follows moves it.
- **A refusal changes nothing a reader can see** and answers by name
  (`OS64_HTML_NO_MEMORY`, `ARENA_EXHAUSTED`, `TOO_DEEP`; the parser's own
  refusals for a fragment that spends its work); `doc->refusal` stays what
  the document's parse said. Depth is the document's limit (twice
  `max_depth`, counted from the fragment's own root; the insert that
  follows counts it again from the real tree).
- **Serialisation follows the standard's escaping table**, which differs
  by where the text sits: text inside `style`, `script`, `xmp`, `iframe`,
  `noembed`, `noframes` and `plaintext` is written raw; inside `noscript`
  it is raw when `scripting` is set and escaped otherwise, which is why
  the serialiser takes the flag too; everywhere else `&`, U+00A0, `<` and
  `>` are escaped. Attribute values escape `&`, U+00A0 and `"`. Void
  elements have no end tag; a template's contents are serialised, not its
  (empty) children; foreign attributes keep their qualified spelling,
  which the attribute records already carry.

**The builder decides, and records why:** the work budget a fragment
parse runs under (the document keeps a depth limit and an arena budget
but no work budget of its own; a fragment's input is bounded by its
caller, and the budget should be proportional to it rather than a second
hundred-million constant); whether the parser struct is reused with a
different root or a smaller fragment parser is extracted; and whether
`serialize` answers a status at all (a cycle cannot reach it, since the
verbs refuse one, so a plain length may be enough).

**The oracle for parsing is already in the tree.** The 192 reference cases
with a `#document-fragment` line (across `adoption01`, `foreign-fragment`,
`math`, `svg`, `template`, `tests4`, `tests6`, `tests7`,
`tests_innerHTML_1`, `webkit02`) name a context element and expect a
tree. The context is spelled `div`, `svg path`, `math ms`, `template` and
so on: a bare name is an HTML element, a prefixed one is a foreign
element in that namespace, created with no attributes. Un-skipping them:
remove `fragment-parsing` from `tools/html_reference.py`'s skip reasons,
regenerate `tools/html5lib-tests/INVENTORY.json` and `SKIPS.tsv` by
running that script (the harness refuses a stale manifest), teach
`tools/test_html_driver.c`'s batch protocol a fragment kind that carries
the context, and print the fragment's children at depth zero in the
reference dump. Every case runs with scripting off and on where the
fixture allows (`scripting_modes`), whole; then every allocation failed in
turn and the work running out at every step, as `safety_case` does for
documents. The fuzz pass gains a fragment arm: a mutated reference input
parsed as a fragment under a random context from the list above, with
the invariant checks of the D2a walks.

**Serialisation has no upstream oracle in the fixtures**, so its proof is
by hand, one case per rule above, plus the `snprintf` contract (`cap`
zero, one short, exact, ample), a serialise of every reference-case
document and fragment under ASan (the output is not compared, only that
it is produced within the answered length and is valid UTF-8), and a
round trip where the standard promises one: a fragment serialised and
parsed again under the same context gives the same tree, over the
fragment cases where it holds (the standard notes it does not hold
everywhere, and the as-built section names the cases excluded and why).
Headless Chrome's `--dump-dom` is its serialiser's output and makes a
fine spot check for a handful of documents; the memory of how to run it
from WSL is Fable's, so ask.

**Docs and surface.** `html.h` gains a contract block for the two verbs
under CHANGING A DOCUMENT; LIBHTML.md's "Out, by name" loses the fragment
bullet and gains a dated section, as D2a's did; `userland/libhtml/README.md`
lists the verbs; DOM.md's D2b row and as-built section; `/tests/htmltest`
gains a fragment-and-serialise function (an `innerHTML`-shaped
replacement of a node's children, then the children read back as text).
Mutants: at least one per escaping rule, one per context kind that
changes the insertion mode, the stop that must not happen, and the
version that must not move.

## D3 — built; review pending

The brief is retired in favour of [DOM.md § D3, as built](DOM.md#d3-as-built)
and the public contract in `userland/libpage/include/page/page.h`.
Implementation is in [PR #211](https://github.com/VBWizard/os64/pull/211),
`codex/dom-d3`; D5 consumes the state revision and the
attribute-transition handoff, and D6 adds holds for state and model keys.

## D6 — reclaiming detached subtrees nothing holds

**Storage contract to settle before building.** Reusing node bodies while
retaining each fragment's parsed names and attributes cannot meet the flat
arena requirement below. D2b and D6 must account for reclaimable fragment
payloads and the scaffold/result containers, as well as node reuse, without
weakening the hour-long churn proof. D3's persistent state also retains node
keys, including option keys and default-value caches: those records must
hold their nodes until released when D6 introduces reclamation.

**What it is.** DOM.md § Wrappers, the paragraph that begins "So the
reclamation JAVASCRIPT.md booked is not a someday item", and the DEBTS.md
row "libhtml keeps a detached node until its document is freed". A page
that replaces a hundred nodes sixty times a second meets the 64 MiB
budget in about a minute; this slice makes that page run for an hour.
The first cut reclaims a removed subtree in which nothing is held, which
is the `innerHTML` case. The full answer (a weak wrapper table, the
collector deciding) is booked and is not this slice.

**The mechanism DOM.md sketches, and the brief adopts:**

- **A `held` mark on a node** says something outside the tree points at
  it: a wrapper (D5's table), the parser's own references while it is
  building (its open elements, active formatting list, form and head
  pointers, the script it stopped at), or a face's own node pointer. Two
  verbs expose it: `os64_html_hold(doc, node)` and
  `os64_html_release(doc, node)`, counted so two holders do not cancel
  each other. Where the mark lives is the builder's: the private word
  after a node carries a text buffer's capacity or an element's flags, and
  a node's public struct was given its document mark in spare bytes by D1;
  the as-built section records whether a node grew.
- **Detachment is the moment.** When `remove`, `replace`, or an `insert`
  that moves a fragment's children out leaves a subtree with no parent,
  the subtree is walked once: if no node in it is held, it is retired as a
  unit, stamped with the version, like a replaced string (DOM.md § The
  mutation core, the retired list). The pins then say when it can be
  reclaimed: a snapshot built before the detach may still point at those
  nodes, so they are freed when no live pin is below the stamp, the same
  rule the ledger already applies to strings. With no pins out, at once.
- **What reclaiming frees.** Nodes live in the permanent chunks and are
  one size (the public struct plus the private word), so a reclaimed node
  goes on a free list that `node_new` and `h_node` draw from before the
  chunk; a verb-made text buffer and an element's private attribute
  records are ledger blocks and are freed or retired as `set_text` and
  `set_attr` do today; a parser-era string in a permanent chunk stays
  where it is and stays charged, as DOM.md says. `node_count` and the
  arena figures move accordingly, which is what the proof measures.
- **Form-owner records.** A control inside the reclaimed subtree with a
  record, or a form inside it that a record elsewhere names, must leave
  the record count right. The verbs' own rule says a move that parts a
  control from its form clears the record, so by the time a subtree is
  detached no record crosses its edge; the walk asserts that rather than
  assuming it, and the record count comes down for each record it frees.
- **Template contents** are a tree of their own under their template; a
  reclaimed template takes its contents with it.

**Read.** DOM.md § The mutation core, § Wrappers (the whole section: the
teardown order is what the hold calls must respect), § D1, as built and
§ D2a, as built (the grow-in-place rule and the retirement stamps are the
machinery this reuses). `core.c`: `d_alloc`, `h_free`, `d_retire` and the
reclaim pass; `dom.c`: `node_new`, `moved`, the form-owner walks;
`internal.h`: `HDoc`, `h_word`.

**Rules already settled:** a node with a wrapper is never reclaimed in this
slice (the table holds it strongly until teardown); nodes created and
never inserted (`createElement` then dropped, a `clone` dropped) are the
full answer's and stay charged; the parser holds what it references and
releases at `finish` and `abandon`, so a tree changed under a stopped
parser (D2a's disturbed mode) cannot have an open element reclaimed from
under it; `os64_html_document_free` frees everything regardless and the
pin check stays.

**The builder decides, and records why:** whether the subtree walk at
detach time is charged as work (the D2a checks charge their walks); the
free list's shape; whether a held count or a held bit (a count is the
lean: the binding and a face can both hold one node); and what
`os64_html_release` of an unheld node does (ending the program, as a
stale unpin does, is the lean: it is the same class of fault).

**The proof.** The DOM.md row is "the churn page that met the budget in a
minute runs for an hour", and the libhtml half of it runs on the host
without a page: in `tools/test_html_dom_host.c`, a loop that replaces a
hundred-node subtree through `parse_fragment` and `insert`/`remove`
216,000 times (an hour at sixty a second) under a 64 MiB budget, with
`arena_bytes` and `node_count` read every thousand steps and required to
stay flat after the first. Then the same loop with one node of the
subtree held: nothing reclaimed, the budget met, the refusal named. The
same with a pin taken before the detach: nothing reclaimed until the
unpin, everything after. A reclaimed node's memory reused by the next
`create_element` (the free list is live). The D1 random walk extended:
the model forgets a detached unheld subtree the way the library does and
asserts the node count, and holds are taken and dropped at random so
that reclaim and no-reclaim both happen under every verb. The allocation
sweep over the walk at detach time. Mutants over the hold check, the
stamp, the pin rule, the free list and the record count. In the guest,
`/tests/htmltest` runs a short churn loop under a small budget and reads
the arena figures back.

**Docs.** `html.h` gains the hold verbs and a paragraph under A NODE
LIVES AS LONG AS ITS DOCUMENT that is no longer the whole truth and must
be rewritten (it becomes "as long as its document, or until nothing holds
a subtree it was removed in"); the DEBTS.md row is paid and the full
answer's row stays; DOM.md's D6 row and as-built section; the consumer
harnesses and the corpus numbers unchanged with scripting off.

## D5 — the binding library (libdom) and J3's fixture

**What it is.** A new library, `userland/libdom` (name ruled, DOM.md
ruling 3), that stands between libjs and libhtml/libpage: it makes the
objects a script sees, converts arguments, calls one verb, and turns a
status into an exception. Plus yonder running a page's inline scripts
against the finished document and redrawing, which is J3's fixture. Two
PRs are the lean: D5a the library with its host harness, D5b yonder's
wiring, the switch and the guest fixture.

**J3's bar** (JAVASCRIPT.md § Delivery and validation): a script changes
visible text and the page redraws; a held reference and a typed-in field
survive an unrelated change; a navigation with a script queued tears down
clean; the leak count is zero. JAVASCRIPT.md § Parsing and script
execution allows exactly this shape: "an early fixture may deliberately
execute a script against a finished document to prove mutation and
redraw. Label it as an integration milestone; it does not establish
browser-compatible script execution order." So D5 does not wait for D4:
the page arrives as it does today (fetched, parsed and modelled on the
worker with scripting on, `finish` running straight through), and the
window then runs the `script` elements in tree order, one per turn of
its loop, against the finished document. Script order, `src` scripts,
`defer`, `async`, timers and events are D7's.

**The surface, as much as J3 needs and no more.** `window` and `document`;
`document.documentElement`, `body`, `head`; `getElementById`,
`getElementsByTagName` (a live collection is a query plus the version it
was last answered at); `createElement`, `createTextNode`, `createComment`,
`createDocumentFragment`; `appendChild`, `insertBefore`, `removeChild`,
`replaceChild`, `cloneNode`; the parent, child and sibling accessors,
`nodeType`, `nodeName`, `tagName`, `textContent`, `nodeValue` and
`data` for text; `getAttribute`, `setAttribute`, `removeAttribute`,
`hasAttribute`, `id`, `className`; `innerHTML` both ways and `outerHTML`
read (D2b's verbs); `value`, `checked`, `selectedIndex` through D3's
script-facing setters; `console.log` through the runtime's output
install; `alert`, `confirm`, `prompt` as DOM.md ruling 2 says (the
question bar, no, nothing). Events, timers, `document.write`, geometry
and `document.cookie` are not in this slice.

**Rules already settled**, all in DOM.md § Wrappers and § What this asks
of libjs: a node's wrapper is found through a table the binding owns,
keyed by node pointer, so `document.body === document.body`; the table
holds every wrapper strongly until teardown; every engine value C holds
is in one of the registries that have a drain (the wrapper table, the
listener lists, the task and timer queues; the last two are D7's); a
value stored anywhere else is a finding; no finalizer owns a native
resource; three budgets reach the script as exceptions (the engine's
heap, the document's arena, the page's own allocations on the script's
behalf); teardown runs in the seven listed steps and never from inside a
script. Class IDs come through `os64_js_class_id`, the context through
`os64_js_context` with the unit's compiled-in ABI string, both in
`userland/libjs/include/os64/js_engine.h` and governed by
`userland/libjs/CONTRACT.md`. Strings cross as UTF-8 with no NUL: a lone
surrogate and a NUL each become U+FFFD before a verb sees them (DOM.md
§ The verbs, the deviation recorded there). Pre-insertion validity is
libhtml's; the binding maps each refusal status to the DOM exception's
name and does no checking of its own.

**Input attributes and state.** D3's node property APIs interpret the
current tree. An attribute binding must also apply the input transition
rules when each mutation happens: a text → hidden → text sequence cannot
be recovered from the final tree/version. Add a state-aware entrance for
`type` and other attributes with control-state effects, including transfer
of dirty text to the value attribute when required. Stage the tree and
state effects together; an allocation refusal changes neither. Test both
intermediate observations and multiple transitions between observations.
Calling `os64_html_set_attr` alone is not that transaction.

**Select mutation history.** Clearing `selectedIndex` or assigning an
unmatched value may leave a size-one select empty. Option insertion or
removal and changes to `size` or `multiple` run selectedness normalization
again, including the first enabled option fallback. The state-aware tree
and attribute entrances must release the explicit-empty selection marker
at those transitions, with refusal preserving both tree and state.

**The leak count.** DOM.md § A leak at teardown asks libjs for a destroy
that reports instead of aborting (CONTRACT.md § Browser extensions, the
third), with a reviewed patch and a ledger allocator. For J3 the fixture
may run on the fatal destroy: "the leak count is zero" is then the
engine's own assertion not firing after the registries are drained, and
the host harness's leak check passing. Whether the reporting destroy is
built in D5 or in the slice that turns scripting on for people (with D6)
is Quinn's call as the runtime's owner, told to Chris.

**Where it runs.** On the window's thread (DOM.md § Where script runs;
ruled). yonder's page arrives through `trip.c` and is seated in
`yonder.c` where `os64_page_build` is called on the fresh page; the
scripts run after that, each as a step of the loop. After a script, if
the document version moved, the rendering step rebuilds: `os64_page_rebuild` (D3),
`garb_cascade`, `flow_layout`, paint. libgarb's cascade and libflow's
layout take a pin at build and let go at free; a layout's `link` and
`control` indices belong to the model it was built from, so a click is
resolved by node (`os64_page_link_for`, `os64_page_control_for`), and a
control's widget becomes the node's, which is what keeps the caret in a
field a script did not touch. The switch is DOM.md ruling 1: a box in
yonder's Settings window, "Run page scripts", off by default, and a word
on the status line while on. A dirty control property may leave the HTML
version unchanged; compare `os64_page_state_version` as well and refresh
control presentation when it moves.

**Read.** DOM.md § Wrappers, § Where script runs, § What this asks of
libjs, § A leak at teardown, § Ruled by Chris; JAVASCRIPT.md § Browser
integration requirements; CONTRACT.md whole; `js_engine.h`; the D3
as-built section; YONDER.md § What runs where;
`userland/libway/session.c` (`way_page_clear` is today's teardown order
and becomes step 6 and 7 of DOM.md's).

**The builder decides, and records why:** the binding's own allocation
budget and its cap; how a live collection is spelled (an array snapshot
re-answered when the version moves is the lean); the exception names'
table; how a script's source name is spelled in diagnostics (the page URL
and the script's ordinal); and whether `textContent` assignment on an
element is one `set_text` on a sole text child or a remove-and-insert.

**The proof.** On the host, `tools/test_dom_host.sh` builds the engine the
way `tools/test_js_runtime_host.sh` does, plus libhtml, libpage and
libdom under ASan, and runs scripts whose outcome is checked against the
tree the verbs built: identity (`body === body` across a rebuild),
every surface entry once, each refusal reaching script as the right
exception with the tree unchanged, the three budgets each met and
reported, teardown in order with the registries drained and no leak, and
an allocation sweep over the binding's own allocations. In the guest, a
page served by a local server with an inline script that changes a
heading's text and a second that leaves a typed field alone, driven
through the vm tools: the screenshot shows the new text, the typed value
is still in the field after the second script, a navigation away with a
script still queued tears down with the heap check in the log, and
`htmltest`'s pinned-document rule still ends a program that gets the
order wrong.

**Docs.** A `userland/libdom/LIBDOM.md` in the shape of the other library
designs, saying what a wrapper is and the registry rule; DOM.md's D5 row
and as-built section; JAVASCRIPT_TASKS.md's D5 and J3 rows; DEBTS.md rows
for what D7 owes (events, timers, script order) if they are not there.
