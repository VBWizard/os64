# DOM_BRIEFS.md — handing out the DOM slices

Status: briefs for the slices of [DOM.md](DOM.md) that other builders take
on, written 2026-10-02 by Fable at Chris's request. DOM.md stays the
design and the acceptance; this file says what a builder who did not write
that design needs in order to build one slice right, and it names what was
left undecided on purpose. When a slice is built, its "as built" section
goes in DOM.md and its brief here is struck.

| Slice | Builder | Reviewer | After |
|---|---|---|---|
| D2b fragments and serialisation — **Merged #212** | Quinn and two scoped subagents | Fable, then an outside round (Codex); Chris schedules reviews | [D2b as built](DOM.md#d2b-as-built); merged after D3 |
| D3 — **Merged #211** | Quinn and two scoped subagents | Fable | [D3 as built](DOM.md#d3-as-built) |
| D6 reclaiming unheld detached subtrees | Available | Fable | D2b (its churn driver) |
| D5 — **D5a merged #214; D5b implemented; awaiting review** | Quinn with scoped subagents | Fable | D2b and D3; D4 is not needed |
| D4 the parser on the window's thread; D7 the loop | Fable | — | D4 any time; D7 after D5 |

D4 and D7 stay with Fable because their sections of DOM.md are findings and
leans, not specifications, and a wrong call there is a hunt rather than a
review round. The remaining briefs below are for slices whose rules are already
written down.

**D7 policy handoff.** Its execution-time default is not settled by D5b's
one-second fixture deadline. Consider a script-timeout control in Settings,
as Chris proposed on 2026-10-03; choose the default/range from P5 workloads and
define Apply/Save behavior. DOM.md's execution-time policy decision records
the per-turn scope and the existing expiry behavior. This remains Fable's D7
work.

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

## D2b — completed brief

The brief is retired in favour of [DOM.md § D2b, as built](DOM.md#d2b-as-built)
and the public contract in `userland/libhtml/include/html/html.h`.
Implementation is in [PR #212](https://github.com/VBWizard/os64/pull/212),
initially stacked on D3 at Chris's request; D3 is not an
architectural dependency. D6 consumes the packed-payload
ownership seam. The as-built section records the corrected escaping/NUL
rules, the historical select compatibility boundary and the full proof.

## D3 — completed brief

The brief is retired in favour of [DOM.md § D3, as built](DOM.md#d3-as-built)
and the public contract in `userland/libpage/include/page/page.h`.
Implementation is in [PR #211](https://github.com/VBWizard/os64/pull/211),
`codex/dom-d3`; D5 consumes the state revision and the
attribute-transition handoff, and D6 adds holds for state and model keys.

## D6 — reclaiming detached subtrees nothing holds

**Storage contract.** D2b packs returned node bodies, names and original
attributes into individually owned ledger blocks and gives text its own
blocks; parser scaffolding is discarded. D6 must reclaim those payloads,
later private attribute records and empty result containers as well as
reuse permanent-chunk nodes, without weakening the hour-long churn proof.
D3's persistent state retains node keys, including option keys and
default-value caches: those records need holds and paired releases when
D6 introduces reclamation. D5a stages new state records before tree mutation;
those reservations must acquire their holds before the native verb and release
them on abort, so post-mutation normalization can still read their keys.

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
- **What reclaiming frees.** Document-parser and creation-verb nodes live
  in permanent chunks and have one size (the public struct plus the
  private word), so a reclaimed node goes on a free list that `node_new`
  and `h_node` draw from before the chunk. D2b's returned nodes are packed
  ledger blocks: the node, its name and its original attributes share one
  individually freeable allocation. Their text buffers and any later
  private attribute records have their own ledger blocks. The reclaim
  walk distinguishes these storage kinds and frees or retires each owned
  allocation once; inline attributes are not freed separately. A string
  from the document parser's permanent chunks stays charged. D2b's
  temporary scaffolding is discarded during parsing, so repeated fragment
  replacement retains no permanent payload chunks. `node_count` and the
  arena figures move accordingly, which is what the proof measures.
- **Form-owner records.** A control inside the reclaimed subtree with a
  record, or a form inside it that a record elsewhere names, must leave
  the record count right. The verbs' own rule says a move that parts a
  control from its form clears the record, so by the time a subtree is
  detached no record crosses its edge; the walk asserts that rather than
  assuming it, and the record count comes down for each record it frees.
- **Explicit-input count.** `os64_html_form_input_count` counts owned HTML
  inputs with an unnamespaced `form` attribute, including detached/template
  nodes and retained clone preparation. Its zero gate skips D5a's ID peer scan.
  Physical reclamation subtracts an input's contribution before freeing its
  attributes; detachment/retirement alone does not. `H_ATTRS_INLINE` and
  `H_ATTRS_PRIVATE` can coexist, so reclamation must not assume exclusivity.
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

**D5a was delivered in [PR #214](https://github.com/VBWizard/os64/pull/214).**
The library contract and measured proof are
in [DOM.md § D5a, as built](DOM.md#d5a-as-built),
`userland/libdom/include/dom/dom.h` and `userland/libdom/LIBDOM.md`.
D5b implements the Yonder integration, default-off settings switch and guest
acceptance; it awaits review. [DOM.md § D5b, as built](DOM.md#d5b-as-built)
records the implementation, evidence and remaining ordinary-browsing gates.

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
resource; native document and page-owned allocation refusals reach script
as named exceptions. Engine exhaustion follows libjs's sticky
`LIMIT/MEMORY` contract and retires the runtime; it is not a resumable DOM
exception. Teardown runs in the seven listed steps and never from inside a
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
reported through their owning contracts, teardown in order with the registries drained and no leak, and
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
