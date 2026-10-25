# XML event parser design

Status: slice 6a design, 2026-10-25. Quinn, following Chris's standalone
in-house parser ruling and Fable's events-only boundary in
[SVG_REVIEW.md](SVG_REVIEW.md). Implementation and conformance results are
pending. [SVG.md](SVG.md) defines the first consuming feature.

## Purpose and boundary

`libxml` converts XML bytes into events. It owns parsing state, nesting and
namespace bindings, not an application document tree. Its public header has
no node type and no dependency on libhtml, libdom, libgarb or window APIs.
`libhtml` consumes events to build its shared nodes. A feed reader can consume
the same events without constructing a browser document.

The parser performs no file access, network access, stylesheet loading,
script execution or DTD retrieval. Namespace URIs are identity strings, not
fetch targets. The host supplies bytes, allocator hooks with a context,
resource budgets, cancellation and an event callback.

## Streaming interface

Provide create, feed, finish, destroy and status/statistics operations over
an opaque parser handle. Feed takes a counted byte slice; finish marks final
input. `NEED_INPUT` is distinct from malformed or truncated input. A feed
reports consumed bytes and leaves no ambiguity about whether they must be
resubmitted. Input need only remain alive for the feed call. Same-parser
calls are serialized; independent parsers share no mutable state.

Events carry counted UTF-8 slices and source locations. Element names expose
namespace URI, local name and original prefix; namespaced attributes do the
same. An absent namespace is distinct from the HTML namespace. Expose namespace
declarations separately from ordinary attributes so a tree builder can preserve
them without treating them as application attributes. Event data is borrowed
for the callback duration. A consumer copies what it retains under its own
budget. Reentering the same parser from a callback is prohibited.

Emit start/end document, declaration/encoding information, start/end element,
text, CDATA, comment, processing instruction and doctype information. Empty
elements produce a start/end pair. Text and CDATA chunks may be split across
callbacks; the logical sequence must be independent of feed boundaries.
Consumers must not infer text-node identity from callback count.

A callback may continue or abort with a consumer error. After an abort or
fatal parse error, the parser emits no more events. Destroy is still valid
and releases parser-owned allocations. A tree builder treats construction
as provisional until finish succeeds and destroys partial output on failure.
Already delivered events cannot be retracted by the parser.

## Supported XML profile

Target XML 1.0 Fifth Edition and Namespaces in XML 1.0 Third Edition for the
declared subset. Reject unsupported XML versions distinctly. Implement XML
character/name rules, QName checks, declaration placement, exactly one root,
matching tags, attribute quoting/normalization, end-of-line normalization,
comments, CDATA and processing-instruction syntax. HTML tokenizer recovery
and DOM name-validation rules are not substitutes for XML parsing rules.

Namespace processing includes scoped bindings, prefix resolution, default
namespace changes, reserved `xml`/`xmlns` rules and duplicate attributes by
expanded name. The default namespace applies to unprefixed elements, not
unprefixed attributes. Resolve a start tag after reading its declarations,
regardless of attribute order. Preserve arbitrary URIs, including an absent
namespace; feed consumers require this even though SVG uses a known URI.
[XML namespaces](https://www.w3.org/TR/xml-names/)

Decode UTF-8, UTF-16LE and UTF-16BE, including BOM/signature detection,
declaration agreement, split multibyte sequences and surrogate pairs. Reject
overlong UTF-8, invalid scalar values and malformed/truncated UTF-16.
Normalize XML line endings after decoding while preserving locations that
can identify the original byte range.

For another declared encoding, report `UNSUPPORTED_ENCODING` with the label
before delivering element content. A host may transcode and restart a new
parser. That restart must deliberately reconcile the declaration with the
new bytes: for example, replace the parsed encoding pseudo-attribute with
UTF-8 and retain the original label as host metadata. Do not pass UTF-8
bytes to a parser still instructed to decode the original encoding. An
inconsistent original BOM/declaration is an error, not a transcoding request.

Initial entity/DTD policy:

| Construct | Behavior |
|---|---|
| Five predefined entities and numeric character references | Supported with XML character checks and context-correct normalization |
| External DOCTYPE identifier | Parse declaration metadata; do not retrieve the external subset |
| Internal general entity with literal text value | Supported, including namespace URI declarations used by older SVG exporters |
| Numeric/predefined references inside that literal value | Supported, preserving XML replacement-text semantics and escaping |
| General-entity references inside entity declarations | `UNSUPPORTED` in the initial profile; do not recursively expand |
| Entity replacement containing markup | `UNSUPPORTED` in the initial profile; no silent conversion of markup to text |
| Parameter entities, external entity declarations/references, DTD validation/default attributes | `UNSUPPORTED`; no resolver callback |

Tokenize and validate the admitted declaration grammar; do not skip a DTD by
searching for the next `>`. Quoted delimiters, comments and internal-subset
boundaries need their XML handling. A defined entity's decoded value cannot
be reparsed blindly as fresh source: escaped ampersands and attribute quotes
must retain their meaning. Use the specification's first-declaration behavior
for repeated general-entity declarations and test predefined-entity constraints.
Future nested-entity support requires cycle detection and expansion-depth limits
in addition to the byte/work limits already required for repeated references.
[XML entities and normalization](https://www.w3.org/TR/xml/)

The literal entity feature is deliberately narrow. It includes
`<!ENTITY ns_svg "http://www.w3.org/2000/svg">` followed by
`xmlns="&ns_svg;"`. It does not establish general DTD conformance, and its
implementation cost is not estimated as a few lines.

## Ownership and limits

Allocator hooks carry the caller's context. Charge parser state, retained
tokens, bindings and entity values, including allocation headers/capacity.
A shared SVG admission ledger can enforce one combined budget across parser,
tree and renderer; parser statistics remain a subdivision of that ledger.
Do not charge the same allocation twice or hide internal heap allocations.

Bound encoded bytes, decoded bytes, expanded bytes, live storage, nesting,
name/token length, attributes per element, namespace bindings, declarations
and parser work. Charge scans and hash comparisons as well as emitted events.
A document containing one huge unfinished token must not require quadratic
rescanning on single-byte feeds. Repeated uses of a large literal entity are
bounded even without recursive expansion. Check cancellation during long
tokens and expansion, not only at element boundaries.

Separate statuses: malformed XML, unsupported profile/version/encoding,
limit exceeded, allocator failure, cancellation and consumer abort. Empty
input and premature end-of-input are malformed. Failed finish owns no
published document. A stopped parser can be destroyed without a special
cleanup protocol from the caller.

## Shared tree integration

The existing libhtml node representation already supports an SVG element
directly under a document node. Build on that instead of constructing a
second DOM in libxml. Prove independent document ownership without creating
a JavaScript context; libdom's page-arena `createDocument` is not that API.

The builder still needs a representation for arbitrary/no-namespace elements,
processing instructions and CDATA behavior appropriate to XML documents.
Those are shared-tree changes, not a reason to add nodes to the parser.
Preserve HTML parsing and existing known-namespace behavior. Keep namespace
prefix and local name distinct where selector/DOM APIs require them.

For SVG, prove cascade traversal from a document element other than `html`,
`:root`, inheritance, case-sensitive names and SVG `<style>` collection.
Inline SVG can proceed while the byte-to-tree route is built. Generic
`DOMParser`, `responseXML`, XHTML navigation and feed applications are later
consumers, not features delivered merely by linking libxml.

## Verification

Pin an official xmlconf archive with a cryptographic digest and preserve its
license and test IDs. Use the catalog's XML version, namespace and entity
requirements to classify applicability before running the parser. A separate
host tool may use an established XML parser to read the catalog; the parser
under test must not grade its own input or decide the expected answer.
[W3C XML conformance suites](https://www.w3.org/XML/Test/)

Report in-profile accepted well-formed cases, rejected not-well-formed cases,
unexpected accepts, unexpected rejects and unsupported-profile cases separately.
DTD validity failures are not necessarily XML well-formedness failures.
Out-of-profile cases stay visible with a reason; refusing a valid file cannot
be counted as a pass. Do not describe a profile pass as the entire xmlconf
suite being green.

Compare normalized logical event streams, allowing adjacent text chunks to
coalesce. Exercise whole-buffer, single-byte and varied chunk feeds, especially
inside BOMs, UTF-8/UTF-16 sequences, entity references, quoted attributes and
terminators. Add namespace collisions, split CDATA/comment ends, misplaced
declarations, literal entity escaping, long unfinished tokens and budget edges.
Test feed-consumption/status contracts and consumer aborts explicitly.

Sweep parser and builder allocations, interleave independent parser instances,
and run ASan/UBSan with leak detection. Fuzz bytes and chunk boundaries under
work limits. The first shipping gate is the declared profile's conformance,
streaming invariance and leak verdict, not a successful logo alone.
