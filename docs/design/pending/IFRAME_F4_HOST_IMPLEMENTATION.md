# F.4b: trusted implementation-code provenance

Self-hosted browser algorithms execute in a JavaScript realm, but their source
is not a page's ScriptOrModule. Treating Streams' implementation frames as page
frames would capture the library realm instead of the caller's incumbent and
could attribute imports or delayed callbacks to the implementation filename.

Patch 0039 adds the trusted native operation
`JS_PrepareHostImplementation(ctx, compiled)`. It accepts a fresh compile-only
script and its nested constant-pool bytecode, before closure publication. An
allocation-free validation pass checks exclusive ownership, context identity
and nesting below 128; a second pass marks the tree. Refusal changes neither
the tree nor pending exceptions. The caller supplies trusted implementation
code; this is not a page-callable privilege or validation of arbitrary pointers.

Marked closures do not inherit an author record, including closures made while
an author frame remains on the stack. Their actual executable/current realm,
native call facts, budget and failure checks remain unchanged. Existing
null-record frame behavior leaves the surrounding author activation visible.
An invoked author callback supplies its own record. Promise jobs can restore
captured author records around implementation resumptions; the mark does not
suppress those legitimate records.

Dynamically compiled author code does not inherit the mark. The mark is omitted
from serialized bytecode: native code must explicitly prepare a decoded trusted
implementation again. This avoids giving cached page code the privilege by
accident. Bytecode format 6 and public wrapper ABI 5 are unchanged; the generated
engine identity changes. The bytecode header uses an available bit, with no
new pointer or retained graph edge.

This cut supplies the engine facility and proof. Streams admission and callback
conversion/invocation are separate opt-ins in the following cut; ordinary
constructors, Yonder and existing code compilation do not call this operation.
It neither completes F.4b nor supplies F.15 ownership/reclamation authority.

## Validation

`userland/libjs/tests/host_implementation.h` is shared by host and guest runtime
consumers. `--host-implementation` selects the focused host proof. Both teardown
policies cover caller/implementation/author-callback realms, nested closure
creation, bound/proxy wrappers, generator and async resumptions, dynamic
functions, exceptions, native backup settings, cached readmission and refusal
of shared, foreign, non-bytecode and over-deep input. The cache control executes
a decoded tree without preparation and requires ordinary page provenance.

Engine identity: `344c59f46b90baac450e194d627a0757e39e618371d84e63fc449fcb0d6a93e0`.
Focused proof: 391 checks on each engine profile. Full runtime: 29,867 shipping
and 96,091 instrumented checks, zero failures/live allocations; parser refusal
and fatal invariant probes pass. Existing DOM Promise, neutral Promise,
synchronous callback and Streams-realm selectors pass on both engines, using
unchanged binding objects relinked against the new engine. Manifest/header
checks, six preparer tests and target ELF/link audits pass, including 227 engine
and 84 runtime exports. The guest runtime consumer compiles.

Host sanitizer runs retain normal-exit LeakSanitizer, ASan and wrapper/support
UBSan; the engine follows the harness's ASan-only instrumentation. Guest
compilation is not guest acceptance.

The semantic distinction follows HTML's
[incumbent settings](https://html.spec.whatwg.org/multipage/webappapis.html#incumbent-settings-object)
and active ScriptOrModule model. The native trust boundary and bounded
preparation algorithm are embedding choices.
