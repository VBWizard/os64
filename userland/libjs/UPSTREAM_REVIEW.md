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
it resolves its imports, marks it unresolved again when one of them fails:
upstream discards a failed module whole, but a browser's module map keeps
its modules across a failed run, and a later graph sharing one linked it
through the NULL of the import never found (a SEGV in
`js_create_module_function`, which WPT's module directory reached).
CONTRACT.md § Modules is the wrapper built on it.

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
