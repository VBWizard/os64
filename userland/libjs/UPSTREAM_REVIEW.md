# QuickJS source review

The manifest pins the official 2026-06-04 archive and hashes each retained file.
The five core translation units and their required tables/headers are imported;
quickjs-libc.c, qjs/qjsc, host modules, examples, and upstream test runners are
excluded. Original LICENSE and VERSION are retained. Patch 0001 omits Atomics
without disabling stack checks. The host smoke test applies it to a disposable
copy, checks hashes, builds the five units, and exercises language execution,
Promise draining, absence of Atomics/std/os, and recursion failure. Review
follow-up probes cover continued execution and queued work after script/job
exceptions, unhandled rejection followed by a handler, and repeated class-ID
requests through one engine slot. The profile guard uses the Python dependency
already required by source verification; it has no ripgrep dependency.

This is source preparation and an engine-only host check. It does not audit
current upstream fixes, numerical conformance, complete interrupt coverage,
shared-buffer suppression, target header adaptation, or an os64 binary. The
initial research's dependency list is in docs/design/pending/JAVASCRIPT.md.
Repeat symbol and required-behaviour audits on R1's actual target objects.

QuickJS inline helpers in the public header have unused context parameters.
The binding syntax check treats that upstream include directory as a system
include, keeping strict warnings on the os64 example without patching those
helpers. Target publication must preserve this scoped upstream-header policy.

Publishing quickjs.h exposes its inline value layout. R0's proposed ABI check
is a prerequisite for publishing an engine context. Native bindings remain
trusted code and retain reference-count, GC marking, finalizer, class-ID, and
re-entry obligations. The host probe is not a test of these wrapper contracts.
