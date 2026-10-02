# QuickJS target adaptation

R1 builds the five retained engine files, the private adapter and the required
compiler-runtime objects into `userland/obj/js/core.o`. The production engine
link records `libmath.so` and `libos64.so`; it uses `--no-undefined`, SysV hashes,
separate code/data segments and the shared-library placement map. The default
userland build produces it for the `js` runner; the image installs it with I1.

## Allocation

`jsport_malloc_functions` uses `os64_malloc_size` for actual usable payload.
The callback counters charge that capacity, including allocator rounding, and
exclude libos64's private heap metadata. There is no second allocation header.
QuickJS's arena and large-block headers are payload and therefore charged.
Both a requested-size preflight and a post-allocation capacity check enforce
limits. Failed allocation or budget refusal leaves counters unchanged.
An allocation request of zero bytes is an engine invariant failure and reaches
the fatal diagnostic. Resizing a NULL pointer to zero returns NULL without
allocating; resizing an existing pointer to zero releases it.

Resize allocates, checks the rounded capacity, copies, then frees the original.
This keeps the original pointer, bytes and counters valid when rounding exceeds
the replacement budget. During copying both blocks exist: the budget measures
retained payload, with a transient extra old block. When the callbacks' opaque
pointer names a caller-owned `JSPortAllocator`, its payload ceiling applies
from the first constructor allocation and combines with the engine's own limit.
Failure flags distinguish budget refusal from OS allocation failure and persist
until the owner clears them. This state must outlive the runtime. A NULL opaque
pointer leaves the raw constructor's initial budget unlimited, as upstream does.
R2 supplies that state and accounts separately for its own storage.

The target patch makes `JS_NewRuntime` use these callbacks too, so retained
upstream default-allocator code does not import a libc size query. The generic
buffer/Unicode fallback realloc paths map to `os64_realloc`; they are included
in the symbol inventory. Proving every runtime allocation participates in an
allocator ledger remains the later teardown-reclamation audit. Fatal teardown
is preserved.

## Headers and strings

`compat/` is a private freestanding include environment for the pinned core,
not a libc implementation. Its memory/string aliases call libos64. Conventional
memory entry points that GCC may emit forward to those same shared primitives
and remain hidden. `stdlib.h` maps allocation and compiler stack allocation;
assertions and aborts reach a named fatal diagnostic and `OS64_JS_FATAL_EXIT`.
Target assertions remain active even with `NDEBUG`.

The engine includes time, fenv, ctype and setjmp headers but this retained
profile uses no declarations from them. Its private placeholders deliberately
provide none. `<math.h>` comes directly from `userland/libmath/include`; the
engine and its bindings use the maths library's published declarations and
classification macros.

A target binding needs these include paths: `-I userland/libmath/include`,
`-I userland/libjs/port/compat`,
`-I userland/libjs/port`, `-I userland/libjs/include`, libos64 and ABI includes,
and `-isystem userland/obj/js/upstream` for the prepared pinned engine headers.
The system-header scope suppresses upstream inline-helper warnings while
keeping the binding strict. The runner needs only `os64/js.h`. The compatibility
paths do not grant a general stdio ABI; their `FILE` is a private handle carrier
for engine diagnostics. Publish matching prepared headers with the runtime.

## Dates and diagnostics

The wall-clock adapter uses one `os64_time` snapshot and converts its subsecond
phase to microseconds using 64-bit arithmetic. A failed or malformed snapshot
is a named fatal platform failure rather than uninitialized Date/random data.
The timezone hook floors negative milliseconds to seconds and calls
`os64_localtime`, retaining the library's TZ/DST policy and reversing its
minutes-east sign for JavaScript's minutes-west offset. Script dates within
TimeClip's range pass through unchanged; native out-of-range hook inputs are
clamped before calendar conversion.

The diagnostic formatter implements the pinned profile's `%s`, `%c`, `%d`,
`%i`, `%u`, `%x`, `%X`, `%o`, `%p`, `%f`, `%%`, flags, widths, precisions and
integer length modifiers. `%f` uses an explicitly requested private dtoa
rounding flag for ties-even under the supported round-to-nearest environment.
JavaScript's own fixed-point conversion keeps its ties-away behavior. It is
not general C stdio: unsupported conversions fail by name. Memory diagnostics
use bounded stack buffers and short-write-aware output, without allocation.
Bounded string formatting retains snprintf's full-length and termination rules.

## Build and audit

`tools/js_prepare.py` verifies every retained source and patch digest, copies
originals to the build directory and applies the patch series. Source hashes
are not merely checked by a test: preparation refuses mismatches before build.
The target flags keep PIC, SSE2, no red zone, stack checks, no Atomics, and
strict warnings. Upstream-only exceptions cover unused callback parameters,
signed comparisons, partial aggregate initializers and the pinned JSON switch's
intentional fallthrough; adapter files retain the full warning policy.

The partial link pulls `__udivti3` and `__udivmodti4` from the cross toolchain's
libgcc archive. Their current disassembly uses registers/saved stack slots,
with no red-zone scratch or other external dependencies. Target tests pin the
remaining imports to the 32 maths names and 18 libos64 services and check that
engine header helpers are exported while port/compiler helpers stay hidden.
I1 must also carry the toolchain runtime notices for the linked libgcc helpers.
A trap-dependency link proves ELF shape and symbol coverage; it has no maths
or system behavior and is not a guest execution test. The target audit also
builds the production engine shared object against the real dependency
libraries, checks its imports, and verifies relinking when the shared recipe
or placement assigner changes.

Remaining: guest engine execution, R2 runtime wrappers and capability policy,
source/file/output helpers, runtime budget/cancellation outcomes, shared-buffer
suppression, guest lifecycle/failure tests, and the runner. Nothing here changes
the kernel or implements browser APIs.
