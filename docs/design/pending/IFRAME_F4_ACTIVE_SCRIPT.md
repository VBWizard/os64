# F.4b — Exact active ScriptOrModule

A native Promise callback can run after the script that registered it has
returned. For `Promise.resolve("import('./child')").then(otherRealm.eval)`,
the callback must retain the original source identity even though eval and its
result Promise belong to another realm. A filename or presence bit cannot
represent that identity. Patch 0034 retains exact records and restores them
around Promise callbacks. F.4 remains in progress; Yonder does not enable the
Promise-settings constructor.

## Representation and ownership

Classic scripts use their compiled root bytecode as record identity. Modules
use their existing module object. Equal filenames do not merge records.
Bytecode closures retain the record they inherit at creation, independently
of their executable realm. Script roots create a record; direct eval inherits
its immediate caller's record; indirect eval's root has none. Closures created
inside indirect eval can still inherit the active outer record. Generator and
async resumes establish the closure's record again.

Each activation owns a hold until it unwinds, so clearing a function's stored
provenance during reentry cannot invalidate the active record. Null-record
code does not hide an older activation. Incumbent suppression affects settings
selection, not the active-record query. Inline handlers keep their null-record
contract: the source-verified clearing operation releases the closure's record
without changing executable realm or an active invocation.

Pending Promise reactions trace retained records with the existing incumbent
context edges. Enqueue retains separate queue roots; execution, discard and
teardown release them. Host callback invocation pushes a synthetic activation
with the captured record's realm and pops it before capability resolution.
The actual callable's execution realm remains independent. A script-bearing
handler can establish its own record above the restored activation.

The closure stores its owned record pointer in a trailing slot of its variable
reference array, with the type tag in spare object-header bits. The array is
allocated when closure variables or a record require it; clearing provenance
releases the record, and function destruction releases the storage. The heap
census includes the trailing slot. General JS objects remain 72 bytes.
Pending reactions grow from 72 to 88 bytes and bytecode headers from 128 to
136 bytes on x86-64. The existing 5,000-Promise crypto stress fixture passes
its unchanged 16 MiB cap in both shipping and instrumented engine profiles.
Putting the record in the general object union exceeded that cap; the
function-local storage avoids charging nonfunctions for this edge.

This adds retained storage. A classic record keeps the root's compiled constant
pool and nested bytecode alive. F15 diagnostics do not enumerate these graph
edges and must keep them explicitly unknown. No physical realm reclamation,
slot reuse, pin API or recovery permission is supplied here.

## Imports and cached code

`import.meta` follows the exact active module, eliminating filename lookup.
Dynamic import snapshots its referrer before specifier/options coercion and
retains it through queued loading. The referrer's realm selects the module
provider; the Promise remains in the executing realm. Under the Promise opt-in,
the internal module continuation prepares module entry and backup settings so
module evaluation, top-level await and native Promise chaining can complete.
This continuation does not install the referrer as a callback activation.
Queue-allocation failure follows the import rejection path and releases its
referrer and converted arguments.

The settlement regression caught a missing continuation entry: the loader ran,
but native chaining could not capture settings, leaving the import pending.
The test now requires both module loading and an observed fulfilled value.

Semantic source names are stored independently of debug data. Stripped code
therefore keeps its source name, and `JS_ReadObjectNamed` rebinds it when cached
classic code is read into another realm. Module records keep their module name
as resolution identity. Bytecode format version 6 rejects previous
layouts. Partial reads release decoded name atoms before and after ownership
moves into the allocated bytecode object; this also fixes the reader's existing
function-name atom leak on prefix/allocation failure.

`JS_GetActiveScriptOrModule` returns an owned opaque engine value;
`JS_GetScriptOrModuleContext` borrows its realm. Neither confers authority to
execute in or expose that realm. `JS_GetScriptOrModuleName(ctx, 0)` uses this
active record. Positive depths retain their diagnostic physical-stack walk.

## Validation

The shared host/guest fixture `userland/libjs/tests/active_script.h` and host
selector `--active-script` cover distinct same-name scripts/modules, foreign
Function construction, direct and indirect eval, generator/await suspension,
bound native callbacks, pending reactions across GC, thenables, rejection,
activation lifetime through provenance clearing and GC, stripped cached code,
all proper bytecode prefixes and old-version refusal. Both teardown policies
must leave no active record or retained allocation roots.

The DOM Promise fixture checks exact restored source identity during native
custom-element registration and exercises listener, observer, Streams and
chained Promise integration. Existing inline-handler tests check null-record
compatibility. Full runtime tests include allocation-failure sweeps for pending
reactions, queued jobs and bytecode reading, parser failures and terminal paths.
Sanitizer profiles retain ASan/UBSan and LeakSanitizer. Host tests do not prove
P5 execution or visible iframe acceptance.

## Integration boundary

The prepared engine identity is
`50e0bb8409b1d0d01fd0dad73adaa305e4a449e49b3eacac33d58c322cc632ba`.
Internal engine layouts change; rebuild consumers with matching prepared
headers and objects. Public wrapper layouts and ABI 5 remain unchanged.
The branch composes merged F15 diagnostics (#610), preserving both focused
test families and the complete Promise/retention export inventory.

HTML's [HostMakeJobCallback](https://html.spec.whatwg.org/multipage/webappapis.html#hostmakejobcallback)
and [HostCallJobCallback](https://html.spec.whatwg.org/multipage/webappapis.html#hostcalljobcallback)
specify capture and restoration separately from job entry. The implementation
uses existing GC objects to represent their source identity; it does not claim
complete browser host integration. Imports with no active record still lack
the browser current-settings API-base-URL fallback and refuse. Realm-less
custom-capability jobs, finalization policy, remaining WebIDL conversions and
pre-member security checks remain F.4b gates. F.4c contexts/origins and F.4d
proxy/location behavior retain their own acceptance requirements.
