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
