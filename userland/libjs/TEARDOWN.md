# D8 reporting runtime destruction

Status: implemented on `codex/dom-d8`, based on `userland` `29641e20`;
independent review pending. Browser default-off policy is unchanged.

## Host contract and APIs

`os64_js_create` selects fatal teardown. The additive
`os64_js_create_with_teardown(config, OS64_JS_TEARDOWN_RECLAIM, abi, out, outcome)`
selects the audited browser profile at creation. Existing public layouts and
ABI identity are unchanged. `os64_js_destroy_report(runtime, report)` consumes
the runtime and fills inline `leaked`, `reclaimed_blocks`, `reclaimed_bytes`.
NULL runtime clears a supplied report; NULL report discards it. Void destroy
uses the selected policy and discards the report. Existing callers keep fatal.

A reclaimed leak is a binding ownership bug. The host must have drained its
registries and stopped all users of runtime/context/value/buffer handles before
destroy. No native resource may require an engine finalizer to release it:
external buffers, native handles and host allocations need independent host
cleanup. No leaked handle may be touched afterward. Other engine aborts and
active destruction remain fatal. See CONTRACT.md for the complete contract.

## Detection and reclamation

Manifest patch `0006-reporting-runtime-teardown.patch` preserves the original
upstream files. It factors `JS_FreeRuntime` into shared cleanup with the fatal
entry retaining both assertions. The private reporting entry returns at that
same gate if the object or weak-reference list remains, after queued jobs,
pending exceptions and GC have been processed. It does not continue engine
cleanup past that verdict. The wrapper closes context access and holds its
entry guard through finalizers, then nulls its engine/context and frees each
ledger block directly, including the runtime structure. Nothing invokes more
engine code or finalizers after the verdict.

Ordinary strings and raw buffers can be retained without leaving an object in
the two engine lists. After a clean engine return, leftover ledger blocks are
also reported and reclaimed. Reports count reclaimed allocator blocks/charge,
not JavaScript values. This catches that leak class rather than silently
abandoning it.

The tracked adapter prepends a `max_align_t`-aligned two-link/owner header to
each allocator block. It charges libos64's actual usable capacity, header
included, from the first constructor allocation. Payload usable-size queries
exclude the header. Resize stages the replacement, checks its rounded charge,
copies the payload, repairs ledger links and frees the old block. Refusal
preserves pointer, bytes, links and counters. As in the original adapter, the
budget measures retained charge; copying temporarily retains the old block.
The fatal profile keeps its header-free allocation format. Constructor failure
uses ordinary engine cleanup and requires the ledger to be empty.

## Allocation and process-global audit

The source audit follows the pinned/generated target engine:

- `JS_NewRuntime2` allocates the runtime through the supplied allocator. The
  engine's arena and large-block operations call that allocator; wrapper source
  buffers and rejection records use `js_malloc_rt` too.
- `quickjs.c` forbids direct malloc/free/realloc below the default allocator
  section. The target patch removes that default allocator and supplies the
  port callbacks. The linked `quickjs.o` has no direct heap-service import.
- Engine dynamic-buffer construction uses `js_dbuf_init` with `js_realloc_rt`,
  or the bytecode wrapper that delegates to it. Unicode normalization passes
  `ctx->rt` and `js_realloc_rt`; subsequent buffers/ranges propagate that callback.
- `libregexp.c` supplies `lre_realloc` (or its bytecode wrapper) to its buffers
  and ranges. `lre_realloc` obtains the context's runtime and calls
  `js_realloc_rt`; nested Unicode ranges preserve the callback.
- The generic `cutils.c` and `libunicode.c` fallback callbacks still exist for
  callers outside this runtime path. Their linked objects each contain one
  direct `os64_realloc` relocation, within the respective default callback.
  The runtime paths above supply non-NULL callbacks and cannot select them.
- `dtoa.c` uses caller-provided stack scratch. Its heap-allocation debug branch
  is disabled by the pinned source; the linked object has no heap import.
- With Atomics disabled, the linked engine's non-relocation writable storage is
  the four-byte `js_class_id_alloc` integer. Other engine tables are constants
  in `.rodata`/`.data.rel.ro*`, containing static function/string addresses.
  No such table retains a runtime pointer. The wrapper's class lock is an
  integer; the runtime/context opaque references are runtime-local.

`python3 tools/test_js_teardown_audit.py` checks the target objects' direct heap
imports, fallback relocation locations, writable globals and private-symbol
visibility. It checks premises of the source audit; it is not a general
proof for future bindings or unreviewed engine profiles. The existing manifest
and target-symbol audits also pass. Custom bindings must satisfy the host
contract before selecting reclamation.

## Browser ownership and D7's join

Yonder selects RECLAIM when lazily constructing its page runtime. Retirement
stops its native script queue, drains libdom's C-held values, reports/destroys
the engine, frees DOM native records, then releases queued node holds. Native
models/state/document survive this sequence. libdom's finalizers only clear
opaque slots; its query/node records and node holds are freed independently by
`os64_dom_free`. A leaked wrapper therefore cannot suppress their cleanup.
Yonder logs the page URL and reclaimed block/byte counts through
`os64_debug_log`, so logd preserves the verdict from a desktop launch, and keeps a
saturating owner-thread session count exposed by
`yonder_scripts_teardown_leaks()`. Ordinary fixtures require zero; the deliberate
leak case expects one and proves that a subsequent page still executes.

D7's registries join that order (DOM.md § D7c, as built): `os64_dom_drain`
frees the C-held listener callbacks, timer callbacks and arguments and event
values before destroy, and `os64_dom_free` frees their native records
afterward without entering the engine. The event finalizer owns engine-allocator
memory only, never an external or native resource that needs finalizing, and
must stay that way: a leaked event's finalizer does not run. The browser leak
fixture runs again through D7's stream with a listener installed and a timer
pending (`test_yonder_scripts_host.c`, the join cases).

## Validation

Maintained suites and evidence are recorded in VALIDATION.md and DOM_D8.md.
The shared runtime cases exercise clean cycles/jobs, finalizer disposition,
weak references, raw buffers, ordinary strings, cancellation with pending work,
exceptions and 10,000 leaked runtimes. Host profiles check zero live allocations
on each repeated cycle; the guest also compares `/proc/self/heap` live bytes and
block counts across the full repeated run and verifies heap integrity.
Constructor-failure sweeps, tracked resize/refusal/alignment tests and a live
peer-runtime probe add host coverage. Separate fatal processes retain the full
JSFA badge for default leaks, raw-engine leaks under reclaiming creation,
active destruction in both policies and unrelated invariants.
