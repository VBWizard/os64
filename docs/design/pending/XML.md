# XML event parser design

Status: slice 6a design, 2026-10-25. Quinn, following Chris's standalone
in-house parser ruling and Fable's events-only boundary in
[SVG_REVIEW.md](SVG_REVIEW.md). The standalone foundation and native shared-tree adapter are implemented; the
as-built sections below record their contracts and verification. The first
browser-owned consumer is the [standalone SVG adapter](SVG_DOCUMENTS.md);
picture-job integration is tracked separately.

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

The builder represents arbitrary/no-namespace elements and processing
instructions in shared nodes. Its rendering projection coalesces CDATA into
text, as specified below. These are shared-tree features; libxml remains
an event parser.
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

## Standalone foundation, as built — 2026-10-25

`userland/libxml/include/xml/xml.h` exposes the opaque push parser, borrowed
UTF-8 event slices, allocator/context hooks, limits, statistics and terminal
statuses. `userland/libxml/xml.c` implements it without libc calls or a document
tree. `make -C userland xml-library` builds the prelinked `libxml.so`; the
cross-built object has no undefined symbols or `DT_NEEDED` dependencies.
Root/userland build membership and the GDB symbol list include the library.
The event-parser increment exposes no script-visible API. The later
[standalone SVG adapter](SVG_DOCUMENTS.md) consumes its shared-tree output.

Start/end element events carry expanded names; namespace declarations are
separate from attributes. The element stack retains scope, not child nodes.
Comments and PIs in the internal subset carry `in_doctype`, so a consumer can
distinguish them from document children. Locations identify the start of the
containing lexical token in original bytes, plus normalized line/column;
internal-subset events identify their DOCTYPE token. Finish emits the end
location. All events are provisional until `XML_DONE`.

Tokens are accumulated geometrically and interpreted on closure. This makes
one-byte feeds linear in unfinished-token length. Large uninterrupted text,
comments, CDATA and DOCTYPE subsets share the explicit token cap: they are not
silently streamed past it. Default limits are 16 MiB encoded, 32 MiB decoded,
32 MiB reference-normalized text/attribute output, 8 MiB live parser storage,
128 Mi work units, depth 256, token 1 MiB, name 4096 bytes, 256 attributes per
element, 1024 simultaneously live namespace bindings and 256 entity
declarations (including repeats). Capacity and allocation headers are charged;
replacement output is charged before appending. Work units are deterministic
parser operations, not a wall-clock deadline. Cancellation is polled during
input, scans, allocation and expansion.

Supported encodings are UTF-8 and both UTF-16 byte orders. BOM/signature,
declaration agreement, split sequences, surrogate pairs and line endings are
checked. A BOM-less UTF-16 signature requires an explicit endian encoding
declaration. Unknown declared encodings retain a label for a host's deliberate
transcode/restart. Internal entities support literal replacement text and
predefined/numeric references; first declaration wins, with predefined-entity
constraints enforced. A reference requiring an unread external subset returns
`UNSUPPORTED`; an undeclared reference in a standalone document is malformed.
DTD element/attribute/notation declarations, parameter/external entities,
nested general references, markup replacements and validation are omitted
with a distinct refusal. This is not a complete nonvalidating XML processor.

**Verification.** `tools/test_xml_host.sh` uses ASan/UBSan with leak detection.
The local suite checks event values, normalization, namespaces, UTF encodings,
feed consumption, terminal states, callback abort/reentry, independent
interleaved parsers, budget edges and allocation failures across legal and
malformed fixtures. It includes 10,000 deterministic byte/chunk fuzz cases.
A separate 2,505-case structural differential campaign against host Expat
reports 392 acceptances and 2,113 rejections, agreeing in whole, one-byte and
varied feeds. Expat accepts some malformed version labels, so declaration
grammar is verified by explicit specification fixtures instead.

The pinned official xmlconf archive has 2,585 catalog cases. The independent
Expat catalog reader selects version/edition/namespace/entity requirements
before invoking this library. Results: 73 accepted well-formed but DTD-invalid
cases, 416 rejected not-well-formed cases, 1,229 unsupported cases and 867
catalog exclusions; no unexpected verdicts. `tools/xmlconf/required.json`
requires the 489 supported cases to retain their catalog verdicts: changing
one to unsupported is a regression. Upstream's DTD-valid positives all reach
omitted DTD features; local legal positives verify this parser's intended
profile. Unsupported and excluded tests remain visible with IDs and reasons.
`--require-complete` deliberately fails on the remaining unsupported cases.
This result does not mean the whole xmlconf suite is green.

The sandbox denied LeakSanitizer attachment with `EPERM` and `TracerPid: 0`.
The host suites passed outside it with `ASAN_OPTIONS=detect_leaks=1`, retaining
ASan/UBSan. A single independent Codex cold read found missing BOM-less UTF-16
declaration enforcement and comment terminators overlapping their opener;
both are fixed, with regressions including the DOCTYPE-comment sibling.
Opus/Fable implementation review and target execution remain outstanding;
no QEMU, broad browser suite or mutant campaign was run for this unconsumed
library. The next section records the shared-tree increment; the geometry
proof remains slice 6a work, not an implicit completion of the SVG stage.

## Shared-tree increment contract

The native XML builder lives in libhtml and consumes libxml events into an
independently owned shared document. Its opaque handle exposes feed, finish
and destroy; the document is unavailable before successful finish. Finish
consumes the handle and either transfers one complete document or frees the
partial tree. No JavaScript context, HTML parser recovery, resource fetching
or script execution participates. Parser allocations, builder scratch and
tree allocations use the same document storage ledger. Parser work and tree
copy work have separately reported, explicit caps.

An XML document has `is_xml` set and null `html`, `head` and `body` landmarks.
A generic document-element accessor reads its element child. The shared nodes
retain local name, original prefix and namespace URI, distinguishing no
namespace from an arbitrary URI and from the known HTML/SVG/MathML enums.
Processing instructions retain their target/data as a distinct node kind;
DOCTYPE-internal comments/PIs are excluded from document children. This
rendering-oriented projection coalesces adjacent text and CDATA into text.
It does not expose DOMParser, XMLSerializer or a lossless XML source model.

Existing mutation/hold/pin/clone contracts also apply to these documents.
XML roots may be replaced or removed under document child constraints;
HTML's required-root rule stays in force for HTML documents. Namespace and
prefix strings copied from another document must outlive their source.
The existing serializer remains an HTML serialization API, not an XML
round-trip API.

The cascade proof uses an SVG document root, SVG style text (including CDATA),
`:root`, custom-property inheritance, case-sensitive type/attribute names,
namespace-aware sibling types and selectors for no namespace. It supplies
only the document's own sheets and makes no resource requests. Rendering,
SVG property computation and the image-job route remain later increments.


## Native shared tree, as built — 2026-10-25

`userland/libhtml/include/html/xml.h` exposes the native builder. NULL options
select a 64 MiB combined storage cap, 64 Mi tree-copy work units and libxml's
default parser limits. Supplied tree limits are literal, including zero;
zero parser-limit fields select libxml defaults. Tree allocation refusal is
reported as `XML_LIMIT` with an HTML arena/work status; physical allocation
failure is `XML_NO_MEMORY`. Parser errors, unsupported input and cancellation
retain their XML status. Result storage statistics describe the verdict
before finish frees parser/scratch; the returned document reports its own
remaining arena. The document's work counter sums parser and tree work.

All retained names, prefixes, namespace URIs, attribute values and PI data
belong to the document. Input buffers may be reused after feed. Text/CDATA
are coalesced in a bounded geometric buffer; node mutation and reclamation
own the transferred buffer. Cross-document clones copy arbitrary namespace
strings, including attribute records changed under snapshot pins. The former
attribute-copy implementation borrowed namespace pointers, which was valid
for its static known URIs but would dangle for an XML source document.

`is_xml` marks the document and each node records its owning document mode.
HTML landmarks stay NULL; `os64_html_document_element` supplies the generic
root. `os64_html_namespace_uri` covers known, arbitrary and absent namespaces.
The selector engine uses that identity for sibling types, local attribute
names for wildcard namespace selectors, and the document mode for HTML-name
case folding. A clone takes its destination document's mode.

The existing HTML serializer retains qualified element spelling and emits
PI nodes according to its HTML contract. It does not perform XML namespace
fixup or XML serialization. HTML fragment parsing refuses XML documents and
unknown/no-namespace contexts; there is no XML fragment parser or general
namespace-aware creation API. DOMParser, responseXML, script bindings and
SVG image jobs do not consume this native builder. These boundaries are
recorded in DEBTS.md rather than treated as completed browser features.

**Verification.** `tools/test_xml_tree_host.sh` checks whole-buffer, one-byte
and varied feeds with poisoned input buffers; independent interleaved
builders; cleanup after malformed input, abandonment, cancellation and each
physical allocation failure; storage/work/depth limits; namespace and PI
ownership after source teardown; held/pinned root replacement and attribute
mutation; CDATA coalescing; and HTML serializer output. The SVG-root cascade
proof checks `:root`, case, namespace sibling identity, and the actual red
color inherited through a root custom property. A maximum-depth tree keeps
its final text level when cloned and inserted into an equally limited XML
document.

The new suite and the existing HTML, DOM, libgarb, libpage, libflow, libway,
libwend, Yonder painter and scripted-page suites pass with ASan/UBSan and
LeakSanitizer enabled outside the sandbox. Cross-built `libhtml.so` links
`libxml.so` and `libos64.so`. This tree increment had no P5/QEMU feature
checkpoint. The later [standalone SVG adapter](SVG_DOCUMENTS.md) records its
browser-consumer evidence separately. WPT/browser conformance and mutant
campaigns were not run for the tree increment; this is native tree evidence,
not a script API claim.


A single independent Codex cold reader (Opus was unavailable) found XHTML
attribute values incorrectly receiving HTML's implicit ASCII case folding.
The rule now requires an HTML document, with regressions for default and
explicit `i`/`s` matching and cloning into an HTML document. The sibling
namespace-attribute name path also keeps namespaced spelling case-sensitive.
This follows the [HTML selector rules](https://html.spec.whatwg.org/multipage/semantics-other.html#case-sensitivity-of-selectors).
Fable's implementation read and SVG geometry proof remain outstanding.
