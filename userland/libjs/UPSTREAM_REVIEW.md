# QuickJS source review

The manifest pins the official 2026-06-04 archive and hashes each retained file.
The five core translation units and their required tables/headers are imported;
quickjs-libc.c, qjs/qjsc, host modules, examples, and upstream test runners are
excluded. Original LICENSE and VERSION are retained. Patch 0001 omits Atomics
without disabling stack checks. Patch 0002 selects os64 allocation/date hooks
and engine API visibility for target builds; patch 0003 adds a private ties-even
diagnostic conversion request without changing JavaScript dtoa rounding. Patch
0004 fixes four allocation-failure paths: unlink a raw context before freeing
it after prototype-table refusal; validate the global uninitialized-variable
object; release a lazy global value if its variable-reference allocation fails;
and avoid freeing Proxy's constructor again after its consuming installation
fails. The first two fail through the actual cross-built engine. The sanitized
constructor sweep also reaches the individual lazy-global and Proxy failures.
Upstream originals remain unchanged; the manifest records the patch and digest.

Patch 0005 retains the error object while assembling its backtrace. Allocating
the backtrace can replace `current_exception`; without an owned reference,
that replacement frees the object still used by the backtrace builder. The
patch also frees the temporary buffer on the builder's early failure paths.
V1's persistent host-allocation refusal during thrown-object conversion
reproduces the crash before this patch and returns HOST_FAILURE after it.
The acceptance suite runs that case against the actual target core and an
ASan-instrumented engine. Source hashes cover the additional patch.

Patch 0010 refuses to make a function from a definition whose bytecode
emit failed. Upstream checks the buffer's failure mark only on the output
of `resolve_variables` and `resolve_labels`, never on the parser's own
bytecode that they read, so a parse whose emit ran out of memory was
transformed as if whole and read one instruction as another (a garbage
atom; an ASan SEGV in yonder's allocation sweeps). The check sits at
`js_create_function`'s entry, which every definition passes through.

Patch 0011a throws out of memory where upstream ignored a failed
allocation and went on: a child definition with no constant-pool slot
(upstream asserted), a closure-variable add in `resolve_scope_var` (upstream
used the -1 as an index, emitted nothing, or let `resolve_variables` read
it as a position), an `arguments` or function-name add there (upstream
captured an enclosing function's -1 as a wild argument slot, and looked
the function's own up as a global), and `get_lvalue` reading a failed bytecode buffer as a
SyntaxError. Upstream still lacks checks on the parser's other allocation
failures (DEBTS.md lists them).

Patch 0012 lets a module compile without resolving its imports
(`JS_EVAL_FLAG_NO_RESOLVE`, with `JS_EVAL_FLAG_COMPILE_ONLY` only) and
names what it imports (`JS_GetModuleRequestCount`, `JS_GetModuleRequest`),
because upstream resolves through a loader that must answer at once, as it
compiles, and a browser has not fetched the imports yet; upstream's own
comment at the spot reads "Could add a flag to avoid resolution if
necessary". It also makes handled, telling the tracker, the promise of a
module body that `js_execute_sync_module` takes as the module's error:
upstream left it rejected with no handler, so a host heard one module
error twice. And `js_resolve_module`, which marks a module resolved before
it resolves its imports, leaves none of them marked when one fails: the
outermost attempt (a loader's compile nests one inside it) stamps each
module it marks and unmarks every stamped one on failure. Upstream
discards a failed module whole, but a browser's module map keeps its
modules across a failed run, and a later graph sharing one linked it
through the NULL of the import never found (a SEGV in
`js_create_module_function`, which WPT's module directory reached), or,
in a cycle whose members resolved back to the failing module, through
that module after the failure freed it (a use-after-free; both are
runtime-suite cases).
CONTRACT.md § Modules is the wrapper built on it.

Patch 0013 gives back, in `JS_SetPrototypeInternal`, the reference it
takes on the new prototype before making the object's shape its own:
when `js_shape_prepare_update` is refused for memory, upstream returned
-1 still holding it, so the prototype was never freed and the runtime's
teardown found it on its object list. libdom's installer allocation
sweep reached it the first time libdom reparented a prototype it had
made (0f's P1b, EventTarget.prototype beneath Node.prototype).

Patch 0014 confines the `v` flag's ClassSetCharacter rules to where
ES2024 puts them: a character class's atoms and a class string's
(`\q{...}`). `get_class_atom` parses the atoms outside a class too, and
upstream applied both rules there, so a `v` RegExp of `a/b` or `a-b`
threw "invalid character in class", and one of `a::b` or `a@@b` "invalid
class set operation" (a `.` outside a class is the term parser's, and
never reached it). HTML compiles every input's
`pattern` with `v` (§ 4.10.5.3.6), and libdom's constraint validation
(0f's P4) found it: `http://www.example.com` would not compile, and a
pattern that does not compile is no constraint. A class string's atoms
are now asked as class atoms, which is what keeps the rules there, and
lets `\-` be escaped in one, as a class allows.

Patch 0015 adds `JS_ReadObjectNamed`, `JS_ReadObject` with a filename:
every function the read makes takes it as its debug name in place of the
one it was written with. A browser that keeps a classic script's
bytecode (BYTECODE_CACHE.md) runs it under a name of the page's, the
script's place there (`os64-script:<n>`), which libdom reads back off a
thrown error to know whose error it is and whether it is muted, and one
bundle is the third script of one page and the seventh of another. The
name is in every function's debug information, so the read has to give
it; nothing else in a classic script's bytecode depends on its name. A
module's own name, its identity for resolution, is not touched.
`JS_ReadObject` reads with none, as it always has.

Not patched, and worked round instead: `JS_WriteObject2` returns its
buffer without asking whether a growth of it failed (`dbuf.error`), and
the writer's callers of `bc_put_atom` ignore a failed atom index, so under
memory pressure it answers with a payload that reads back as a broken
function (yonder's bytecode round trip ran one into a NULL closure
reference). libjs judges a write by its allocator instead: any refusal
during it, and the payload is thrown away (CONTRACT.md § Source and job
turns, `run_bytecode`).

Selected upstream test sources are retained separately in
`tools/js_acceptance/upstream`, with archive/file hashes and the original
licence. Their assertions remain unchanged; separate drivers report individual
pass/fail results and explicit unsupported host-capability skips. This selection
does not retain or run upstream's command-line test runners.

The engine baseline applies patch 0001 to a disposable copy, checks hashes,
builds the five units, and exercises language execution,
Promise draining, absence of Atomics/std/os, and recursion failure. Review
follow-up probes cover continued execution and queued work after script/job
exceptions, unhandled rejection followed by a handler, and repeated class-ID
requests through one engine slot. The profile guard uses the Python dependency
already required by source verification; it has no ripgrep dependency.

R1 adds private target headers, allocation accounting, clock/date conversion,
diagnostic formatting, hidden compiler memory veneers, the freestanding core
build, and target symbol/header/link audits. The adapter fixtures execute both
a sanitized host build and the cross-built core on the host, using host maths
and controlled heap/syscall inputs. The original engine-only host baseline
remains separate.

R2 adds wrapper capability, lifetime, failure and job-budget tests using the
actual target core and M1 maths, plus a sanitized engine build. The wrapper
removes shared-buffer exposure before context publication. Exact guest and host
evidence is recorded in VALIDATION.md. This is not an audit of current upstream
fixes, numerical conformance or complete interrupt coverage. The
initial research's dependency list is in docs/design/pending/JAVASCRIPT.md.
Repeat symbol and required-behaviour audits on R1's actual target objects.

QuickJS inline helpers in the public header have unused context parameters.
The binding syntax check treats that upstream include directory as a system
include, keeping strict warnings on the os64 example without patching those
helpers. Target publication must preserve this scoped upstream-header policy.

Publishing quickjs.h exposes its inline value layout. R0's reviewed ABI check
is enforced before publishing a wrapper-owned engine context. Native bindings remain
trusted code and retain reference-count, GC marking, finalizer, class-ID, and
re-entry obligations. The host probe is not a test of these wrapper contracts.

### UTF-8 export refusal (patch 0017)

`JS_ToCStringLen2` duplicates or produces a source string before allocating
its encoded copy. Its refusal label returned without releasing that reference.
The combined DOM quota fixture exposed a retained 5 MiB wide string; a smaller
C API regression reproduces the same ownership error for Latin-1 and wide
strings. Patch 0017 releases the reference on that label, preserving the
pending exception and NULL/zero-length result. The adapter suite checks both
target and sanitized engines with leak detection enabled.


Patch 0038 checks the length-property insertion in `JS_NewObjectFromShape`.
Shape cloning/growth can refuse an allocation; the upstream "cannot fail"
comment was false. On refusal the object has not joined the GC list, so the
patch frees its current shape, property storage and object directly. The DOM
synchronous callback allocation sweep reproduces the null dereference with
that opt-in disabled as well as enabled, and retains both paths as regression
coverage. The engine identity changes without a public ABI or bytecode layout
change.


Patch 0039 adds explicit native preparation of a trusted implementation script
and its nested bytecode before closure publication. A bounded validation pass
checks exclusive references and realm identity before a nonallocating commit
pass marks the tree. Marked closures carry no author ScriptOrModule; execution
realm, budgets and lifecycle checks retain their existing behavior. The flag is
not serialized and dynamically compiled code does not inherit it. The shared
host/guest `host_implementation.h` fixture covers reentry, resumptions, cache
readmission and atomic refusal. Browser consumers require their own opt-in.

Patch 0040 adds explicit neutral settings admission for trusted native queue
entries. It reuses queue holds, execution/discard paths and the existing neutral
scope save/restore; raw jobs retain their prior behavior. Streams uses it behind
its callback-settings opt-in. The proof and ordinary Promise retirement boundary
are recorded in `docs/design/pending/IFRAME_F4_STREAM_CONTINUATIONS.md`.

Patch 0041 admits explicitly private Promise capabilities and intrinsic-then
adoption without changing ordinary Promise ownership. Native resolving functions
keep actual realm/trace holds while their jobs use neutral settings. Single
observable then lookup, late species construction, custom capabilities, groups,
budgets and failed-code entry guards remain. Bit-packed shared resolver state
preserves its size; the additional queue argument is charged only to private
adoption. See `docs/design/pending/IFRAME_F4_HOST_PROMISE_LIFETIME.md` for the
embedding policy and evidence; this is not blanket native-function neutrality.

Patch 0045 lets a profiler's ledger observe the engine (the JS speed
measurement; `os64_js_ledger_*` in `js_engine.h`). `JS_SetLedgerHooks` installs
an optional owner-thread hook pair for each of: a C-function-with-data
invocation (given the function and magic it was made with), a call into
script from C through `JS_Call`, `JS_CallConstructor(2)` or `JS_EvalFunction`,
and a cycle collection. Pairs nest and always close in order, also when the
bracketed work throws; a hook must not enter the engine. Unobserved, each site
costs one pointer test. `JS_CallConstructor` now calls `JS_CallConstructor2`
with the function as `new_target`, which its body always was, so the
constructor has one observed door. The runtime removes the hooks before it
frees the engine, whose last collection would otherwise call them. Covered by
the runtime host suite's ledger case.

Patch 0046 reports the bytecode function at a depth of the stack (identity,
name, defining file, line and column) without allocation, exception or
refcount change, so the profile can sample it from the interrupt handler; a
native frame reports `[native]`. `JS_AtomGetStrRT` answers an ASCII atom with
its own bytes and writes the caller's buffer only otherwise, so the query
copies whatever it returns. Covered by the runtime host suite's ledger case
(the sampled function's name, file and line).

Patch 0047 counts every cycle collection: runs, those the allocation
threshold asked for (`js_trigger_gc`; the rest come through `JS_RunGC`), the GC
objects the runs walked (one addition per object in `gc_decref`'s pass), the
objects, function bytecodes, async functions and modules they freed from
cycles, the heap's malloc bytes before and after each run, summed,
the most one run walked, and the threshold the next allocation-asked run waits
for. With a clock set (`JS_SetCollectorClock`) each run is timed by two reads
of it, summed with the longest. `JS_GetCollectorCounts` reads them at any
moment, so what a page's collector costs is known without the ledger's
per-call brackets. The runtime sets its ledger clock (the TSC) when it makes
the engine. Covered by the runtime host suite's collector case.

Patch 0048 holds the collector's next threshold below the memory limit.
Upstream sets it to half again the heap a run left, so with the live part
past two thirds of the limit the next run waits for a heap the limit
refuses, and a page making garbage there fails with the collector never
asked. The threshold is now set after every run (`gc_schedule`, also after
a `JS_RunGC` from the host, unless the host turned automatic runs off with
a threshold of -1, which stays): half again what the run left, then
lowered to the limit less a sixteenth (the margin); where that lowered it,
raised to a sixty-fourth of the limit above what the run left, so a heap
whose live part is inside the margin is walked again only after that much
growth; then lowered to the limit less a 256th, so the last stretch still
has a run, which can leave it below the heap. A run the limit scheduled
also waits until the heap has grown by that 256th from its least since the
last run (`js_trigger_gc`): a heap that is all live reaches the limit in at
most eight runs once inside the margin, not a run at every object made,
and a heap that shrank and filled again with garbage is still collected.
The engine collects only where it always has, at an object's creation: a
collection inside any other allocation could mark an object halfway
through a change. So an allocation of another kind the limit refuses still
throws, and a later object made collects once the heap is past the
threshold. The counts gain the runs whose threshold the limit lowered and
the limit itself; libjs's allocator counts each refusal. The public
collector struct grew, so the ABI identifier is os64-js/6. Covered by the
runtime host suite's limit case and `tools/test_collector_limit_mutants.py`.
