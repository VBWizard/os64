# libhtml

Optional streaming HTML-to-tree library for os64 userland. The settled scope,
reference revisions, deliberate deviations, and evidence are in
[LIBHTML.md](../../LIBHTML.md). Public declarations live in
[html.h](include/html/html.h); only consumers link against `libhtml.so`.

```c
os64_html_options_t opt = os64_html_options_default();
opt.charset = "utf-8"; /* HTTP transport label, when present. */
os64_html_parser_t *p = os64_html_parser_new(&opt);
if (!p) {
    /* Initial heap or arena allocation failed. */
    return;
}
int64_t status = os64_html_parser_feed(p, bytes, length);
/* Further feed calls can be made while status == OS64_HTML_OK. */
os64_html_document_t *doc = os64_html_parser_finish(p);
/* finish consumes p, including on refusal. Walk doc->document or doc->html;
 * head/body are nullable. Inspect doc->refusal after finish as EOF may fail. */
render(doc);
os64_html_document_free(doc);
```

For cancellation call `os64_html_parser_destroy(p)` instead of `finish`.
A supplied options object uses its values literally, including zero limits.

A host that runs scripts sets `opt.scripting`. The parse then stops at each
script, and the host does what it does with one before it resumes:

```c
static int64_t run_scripts(os64_html_parser_t *p, int64_t status)
{
    while (status == OS64_HTML_SCRIPT) {
        run(os64_html_parser_document(p), os64_html_parser_script(p));
        status = os64_html_parser_resume(p);
    }
    return status;
}

status = run_scripts(p, os64_html_parser_feed(p, bytes, length));   /* each chunk */
status = run_scripts(p, os64_html_parser_end(p));                   /* no more input */
os64_html_document_t *doc = os64_html_parser_finish(p);
```

Between two calls the tree is a document: it can be read, pinned and changed
with the verbs. A page left mid-load keeps what was built with
`os64_html_parser_abandon(p)`. The contract is in `html.h` under THE PARSE
THAT STOPS.

Nodes and attributes are read-only views. A document is changed
through the verbs at the foot of `html.h` (create, insert, replace, remove,
attributes, text, clone, fragment parsing), which keep every rule a reader relies on; the design
is [DOM.md](../../docs/design/pending/DOM.md). Elements made by formatting
reconstruction share one list of attribute records, so a verb copies an
element's list before it first changes it. A document owns the
allocation ledger; scratch buffers are charged to the same budget and released
on finish. (Input held while a parse is stopped is the heap's: `max_bytes`
bounds it.) Document-parser node/string storage uses geometric arena chunks;
reclaimed node bodies are reused, while those chunks remain charged.
Fragment results use individually reclaimable ledger blocks, described below. A parser
needs serialized calls, and so do a document's verbs; distinct documents
share two counters (document marks and pin numbers), advanced atomically, and
nothing else. A verb refuses a node of another document; `os64_html_clone`
copies one across.

`os64_html_owns_node(doc, node)` answers ownership for a live connected or
detached node without walking the tree or changing it. Consumers use this
query rather than interpreting the library's opaque document mark.
`os64_html_hold/release` count external node holders. A removed subtree is
reclaimed as a unit when none of its nodes is held and no live snapshot pin
predates its retirement. This includes template contents and consumed
fragment containers. A node needed after a mutation must be held beforehand;
newly created nodes never inserted remain document-owned until teardown.
Document teardown rejects outstanding node holds and snapshot pins;
consumers must release their references before freeing the document.

`core.c` owns allocations, pins and retirement, topology primitives, limits,
and the parsing API, with the stop at a script and the input held meanwhile.
`dom.c` holds the verbs that change a document, and the rules the parser
keeps once a verb has moved a node under it.
`encoding.c` selects/decodes the byte stream and preprocesses newlines.
`tokenizer.c` implements token states and character references. `tree.c`
implements insertion modes, formatting reconstruction/adoption, templates,
foreign integration, and foster parenting. `fragment.c` stages contextual parses
and copies their reachable result into document-owned ledger blocks.
`serialize.c` writes HTML without allocating or using recursion.
Reprocessing is iterative; bounded
mode delegation follows the standard's calls to another insertion mode.

Fragment parsing and serialization:

```c
int64_t status;
os64_html_node_t *fragment = os64_html_parse_fragment(doc, context,
    markup, markup_length, scripting, &status);
if (fragment)
    status = os64_html_insert(doc, context, fragment, NULL);
size_t length = os64_html_serialize(context, true, scripting, NULL, 0);
/* Allocate length + 1, then serialize into that buffer. */
```

`context` must be an element owned by `doc`; it may be detached. Parsing uses
its namespace, attributes, insertion mode and nearest ancestor form, and the
source document's quirks mode. Those context nodes are read-only during the
parse. Scripts never stop or run; `scripting` selects `noscript` behavior.
The input is UTF-8 without encoding sniffing, BOM stripping or meta charset
interpretation. Invalid UTF-8 becomes U+FFFD. NUL follows tokenizer/tree
rules: ordinary HTML text drops it, while raw text, RCDATA, attributes,
comments and foreign text replace it.

Select contexts retain D2a's insertion rules and the pinned reference corpus:
`<div>x<option>a<option>b` in a select context drops the div wrapper. Current
customizable-select parsing preserves that wrapper; adopting those rules also
changes document parsing and is separate compatibility work. The boundary is
recorded in [DEBTS.md](../../DEBTS.md) and
[DOM.md's D2b account](../../docs/design/pending/DOM.md#d2b-as-built).

The work limit is saturating `4096 + 256 * input_length`, covering parser
steps, context ancestry, copying and form-owner remapping. The fixed allowance
covers empty-input setup; the per-byte allowance gives ordinary markup room
for parser repair and copying while bounding adversarial walks. The private
`h_parse_fragment` entry point accepts an exact work limit and reports work
for failure-sweep tests. Depth follows the document's existing tree limit
from the detached fragment root; inserting the result checks the destination
depth again.

A private parser ledger and a separate result ledger share the document's
remaining arena budget. Peak storage includes both, including parser metadata
and copy scratch; a near-cap parse can refuse even when its final tree alone
would fit. Failure frees both ledgers and preserves document accounting,
version, parse diagnostics, existing form associations, landmarks and pins.
Success publishes detached owned storage and its allocation statistics;
insertion is the operation that changes the tree version.

Returned nodes pack their names and original attributes into tagged ledger
blocks. Text buffers and later private attribute records have separate blocks.
Clone and attribute COW copy inline attributes and names rather than borrow
another node's packed payload. Temporary scaffolding and unreachable parser
payloads are freed before return. Reclamation frees packed node blocks and
owned text/private attributes after holds and document pins allow it;
inline attributes are not separate allocations. Inserting a fragment consumes
its unheld container, even when empty, without changing the version for an
empty insertion. Hold the fragment if its identity must remain usable.

Serialization takes `children_only` for innerHTML and clears it for outerHTML.
Document and fragment nodes have no wrapper. It follows current WHATWG HTML
serialization: HTML void elements omit children and end tags, templates use
their contents, and only HTML raw-text parents suppress text escaping.
Attribute values escape `&`, NBSP, `<`, `>` and double quotes; ordinary text
escapes the first four. Names preserve namespace-qualified attribute spelling
and source order remains stable. The current standard escapes angle brackets
in attribute values as well as text.

The serializer allocates nothing and traverses iteratively, including template
hosts. It returns the full byte length excluding NUL, saturating at `SIZE_MAX`,
and writes at most `cap - 1` bytes plus NUL when a non-NULL buffer has nonzero
capacity. A zero capacity writes nothing. Truncation is a byte prefix and may
split a UTF-8 character; the complete output is UTF-8. Calls on a document
must be serialized with mutations of that document.

The parser implementation is project code. Entity, SVG-adjustment, and quirks
tables are generated from the imported WHATWG data under
`tools/html5lib-tests/standard/`; its license is included there. Tag enum input
is `tags.txt`. Regenerate with `tools/gen_html_tables.py`; `--check` verifies
without writing. Upstream tokenizer/tree fixtures retain their licenses and
exact bytes. Saved corpus pages retain their publishers' content; their URLs,
fetch dates, transport charset, and hashes are in `tools/html_corpus/SOURCES.json`.

Validation:

```sh
tools/test_html_host.sh
tools/test_html_dom_host.sh        # the verbs
make -j4
# Inside os64:
/tests/testrun htmltest
```

The host script checks reference provenance/inventory, exact token/tree results
with scripting off and on, chunking, every byte prefix, heap failure points,
topology/UTF-8, encoding, resource refusals, saved trees, a 30-second mutation
pass, and the parse that stops: each call's contract by hand, a parser
abandoned at every stop, and random verbs run at every stop of random
documents (`html_driver --checks <walks> [first]` runs more of those, or one). It does not fetch
network resources or update expected results. `HTML_FUZZ_SECONDS` selects a
longer fuzz budget. `tools/update_html_fixtures.py` and
`tools/update_html_corpus.py` are explicit refresh commands; review their inputs
and regenerated expectations together. `tools/test_html_dom_host.sh` proves the
verbs: cases by hand, every allocation failed in turn, pins and retirement, and
random walks checked step by step against a second tree kept by the test. Corpus snapshots can be regenerated
with `tools/test_html_corpus.py --driver <host-driver> --refresh-snapshots`.
