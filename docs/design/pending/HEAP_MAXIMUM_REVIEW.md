# Heap maximum bookkeeping review

Prepared by Quinn for Fable, 2026-11-01.

The proposed change removes repeated free-list scans used to maintain
`largest_free` in the **userland allocator**,
[`userland/libos64/heap.c`](../../../userland/libos64/heap.c). Those scans caused
visible animation stalls in Yonder. Hardware acceptance now reports smooth
animation. Fable approved the architecture on 2026-11-01, requesting an
in-process cache audit and alignment with the existing size-bin plan; those
follow-ups are included below. No kernel allocator or syscall changes are involved.

The implementation and agreed review follow-ups are committed as `b33e2be7`
and included in `canvas-integration`. The production patch is confined to
`heap.c`; the associated tests are linked below. Earlier Phoria image and
rasterization changes are separate from this review.

## Why this change exists

Phoria's animated point-light demo would run smoothly, become jumpy for several
seconds, then recover. Hardware measurements separated scene calculations,
drawing, and display. Model-view work stayed near 1.6 ms while drawing rose
from about 5 ms to 65–72 ms. Displaying the finished bitmap remained inexpensive.
A finer trace counted the same 397 strokes per frame in both cases, but their
combined duration rose from about 3 ms to 50 ms. The delay was spread across
many small operations.

The first host replay did not reproduce these clusters. That replay used the
host allocator. Substituting the actual Os64 heap implementation reproduced
them, and counters inside its list walks identified the cause: a slow frame
visited as many as **13,775,541 free-list entries just to recompute the largest
free block**. This was accounting work added to allocation and release, rather
than the search needed to choose an allocation.

The existing allocator keeps `largest_free` exact for `/proc/<pid>/heap`.
Removing a free block whose payload equalled the reported maximum immediately
rescanned the remaining list. A small allocation could remove the largest
block, scan the list, split that block, and insert a remainder that was still
the largest. Freeing that allocation could unlink the remainder, scan again,
then coalesce and reinsert the original large block. A fragmented heap made
each scan expensive; repeated Canvas scratch allocations multiplied the cost.

The reported statistic was correct. The problem was paying to discover a new
maximum at an intermediate point when the same allocator transaction was
about to supply a provably larger replacement. Variation in which free block
served scratch allocations explains why a fixed drawing workload could move
between cheap and expensive stretches.

## Why retain the allocator and its exact report

This replay identifies maximum-statistic maintenance as the expensive path
behind the intermittent animation stalls. The separate 2026-10-28 layout
measurements already met the gate for size bins in
[HEAP_BINS.md Part B](HEAP_BINS.md#3-part-b--size-class-bins-in-the-heap-libos64-heapc-size-m).
This targeted fix complements that plan. It preserves the current first-fit
policy recorded in [MALLOC.md](../completed/MALLOC.md), leaving the broader
changes to address selection and fragmentation for the bins implementation.
The original draft overlooked that existing evidence when discussing bins;
Fable's review corrected the rationale.

Dropping or approximating `largest_free` would weaken the heap report. Deferring
the scan to the end of each allocator transaction, by itself, would still scan
on every small allocation from the largest block. Retaining only a pointer to
the maximum would also leave us searching whenever that block was removed.

The chosen approach keeps enough information to prove that a replacement is
still a maximum. It adds one pointer and one 64-bit bound to the allocator's
static state, with no per-block storage or extra allocation. First-fit order,
block layout, canaries, poisoning, region handling, locking, and the public
heap-report ABI remain unchanged.

## Maximum tracking and publication

`gLargestFree` identifies a free block with the exact maximum payload size.
`gOtherFreeUpper` is an upper bound on the payload sizes of the other linked
free blocks. It need not be their actual current maximum: removing a smaller
block can leave the bound conservatively high.

The invariants are:

1. With a non-NULL maximum pointer, it names a linked free block and
   `gReport.largest_free` equals its payload size. Other linked blocks are no
   larger than that maximum and no larger than `gOtherFreeUpper`.
2. After removing the tracked maximum, the pointer becomes NULL. The retained
   bound covers the remaining free blocks. During this unresolved state,
   insertions below the bound preserve it; an insertion meeting the bound can
   establish an exact maximum immediately.
3. Before publishing an even report generation, `report_end()` resolves any
   remaining uncertainty with a full list scan. An empty list publishes zero.

The existing heap lock protects this state. The existing odd/even generation
protocol permits the report to be incomplete while a transaction is in
progress; the change completes the maximum before the existing release fence
and even-generation publication. It does not make a stale maximum acceptable
in a stable report.

The helper transitions implement those invariants:

| Operation | Maximum tracking |
| --- | --- |
| Remove a block other than the tracked maximum | Keep the maximum and the conservative bound. |
| Remove the tracked maximum | Clear the pointer; retain the bound on survivors. |
| Insert while unresolved, with payload at least the bound | Select the inserted block as the exact maximum. |
| Insert while unresolved, below the bound | Leave resolution to a later insertion or `report_end()`. |
| Insert a block larger than a known maximum | Select it; the former exact maximum bounds the other blocks. |
| Insert a block no larger than a known maximum | Raise the other-block bound if needed. |
| Scan to resolve | Find a maximum and the second-largest payload size, counting ties. |

For example, suppose the largest free payload is 64 KiB and the other blocks
are bounded by 32 KiB. A small request removes the 64 KiB block and leaves a
remainder still larger than 32 KiB. That remainder is a maximum without a
search. Freeing the small allocation and coalescing it with the remainder
similarly supplies a maximum without a search. The list's first-fit behavior
does not change.

If two blocks tie for the maximum, the bound includes the other tied block.
Splitting the tracked one below that bound cannot falsely certify its smaller
remainder. The fallback scan resolves the surviving maximum. Likewise,
removing a block that supplied the bound may make the bound unnecessarily
high, but cannot make it unsafe: the consequence is an extra scan, not an
incorrect statistic.

This is not a constant-time allocator claim. First-fit searches remain linear,
and transactions that cannot establish a replacement maximum still scan.
The improvement targets repeated rescans whose answer can be proved locally.

A concrete remaining case is a 64 KiB maximum at the list head followed by a
60 KiB runner-up. An 8 KiB allocation splits the maximum below the bound,
forcing a scan; freeing it coalesces back to 64 KiB, so the next pair repeats
the scan. Fable reproduced one scan per pair before and after this patch.
That shape could account for part of the residual replay work, but the replay
has not been classified by free-list shape. The
[heap debt](../../../DEBTS.md) records it: planned size bins will restrict
fallback scans to the highest occupied bin. Retaining a runner-up pointer is
another possible optimization, deferred because it adds a lifetime invariant.

`heap_verify_locked()` now uses its physical pool walk to verify the free-block
count, the cached maximum's identity and payload, the published maximum, and
the bound on other free blocks. It compares the cached pointer with validated
free-block addresses without dereferencing it. NULL is valid only with no free
blocks; conservative overestimates of the other-block bound remain valid.
Derived checks are skipped after a damaged physical walk. Explicit verification
runs outside mutation; `HEAPCHECK=1` runs before mutation, after `report_begin`
has made the generation odd but while the cache is still resolved.

An empty free list retains a NULL maximum, so subsequent transactions may run
an empty rescan. This visits zero entries. Future counters must distinguish
rescan invocations from nodes visited; there is no optimization for this case
in the present patch.

## Evidence and limits

A 360-frame replay used the same captured point-light scene, advancing clock,
production geometry, and production Os64 heap before and after the change.
The host supplies the allocator's mapped memory. List-walk counters isolate
the bookkeeping work; the elapsed times below are sanitizer-host measurements.

| Measurement over 360 frames | Before | After |
| --- | ---: | ---: |
| Maximum-statistic list entries visited | 591,838,404 | 21,915,998 |
| Most such visits in one frame | 13,775,541 | 1,117,313 |
| Median script work per replay frame | 23.320 ms | 22.254 ms |
| Maximum script work per replay frame | 101.778 ms | 32.119 ms |
| Frames with more than 40 ms of script work | 50 | 0 |
| Complete bitmap hashes matching the original run | — | 360 of 360 |

The modest median improvement and much smaller maximum are consistent with
removing the intermittent expensive path. These figures are not hardware
frame-rate predictions or a bound on future allocation latency.

The existing heap suite passes **215 checks**. The new
[`test_heap_largest_host.c`](../../../tools/test_heap_largest_host.c) passes
**43,855 checks** under ASan, UBSan, and LeakSanitizer. It walks physical pool
blocks independently of the free-list cache and compares the maximum, free
byte total, free-block count, and completed report generation after operations.
Cases include tied maxima, splitting below a runner-up, coalescing, realloc
growth and shrinkage, dedicated allocations, a pool return at teardown, and
10,000 deterministic mixed operations. The follow-up adds deliberate corruption of
cached pointers (NULL, invalid, live, and smaller free block), maximum values,
free counts and bounds. It checks that conservative bounds remain accepted
and runs the mixed workload with `HEAPCHECK` enabled. It is run by
[`test_js_support_host.sh`](../../../tools/test_js_support_host.sh).

The replay also passes its leak-enabled sanitizer checks. An isolated native
QEMU run advances the point-light scene with the candidate library. The
accepted hardware delivery changed only `libos64.so` from the prior runtime
cohort. It remains frozen and predates the review follow-up verifier checks.
The user confirmed smooth point-light animation and also
reported that the basic cube now rotates as fast as Chrome when timed again.
The latter is user-reported acceptance, without recorded numerical rotation
times or matched comparison conditions.

The later software-triangle demo has a separate JavaScript-throughput issue.
Its diagnosis and possible engine work are deferred to a separate slice; they
are not part of this allocator change. Additional Canvas context is in the
[Phoria evidence](../../canvas-evidence/phoria-uv.md#point-light-follow-up-allocator-bookkeeping).

## Review scope

Fable approved the maximum-tracking invariants and their integration with the
existing allocator, including:

- Whether insertion, removal, split, coalesce, realloc, and region-return
  paths preserve the bound and invalidate the maximum pointer before its
  storage can change ownership or be unmapped.
- Whether ties and a conservatively stale bound can produce a false maximum,
  rather than merely an unnecessary fallback scan.
- Whether every completed report still publishes an exact maximum under the
  existing locking and generation protocol, including failure paths.
- Whether this amount of retained state is an appropriate targeted fix for
  the measured cost, and whether the independent audit misses an important
  state transition.

Hardware acceptance establishes the observed improvement. The architectural
review covers the shared allocator's correctness; the verifier additions are
covered by the follow-up tests described above.
