# Large-image cleanup profile

Measured 2026-10-10 in the WebP worktree. Releasing populated memory is a
shared bottleneck in the QEMU stress environment: a codec-free 64 MiB
mapping takes about 5.86 seconds to unmap on eight emulated cores. The
same operation takes 0.157 seconds with one core. No production kernel,
allocator, or decoder behavior was changed for this investigation.

## Method and measurements

[`memphase.c`](../../userland/tests/memphase/memphase.c) separates mapping,
first-touch faults, and release. Each worker repeats three times with a
64 MiB region. Touch mode writes one byte per 4 KiB page, committing the
whole region without a codec or a full-buffer fill. Untouched mode leaves
the payload alone; the heap still touches its own metadata. Results are
stored per worker and printed after joining, avoiding output in the timed
sections. Raw map/unmap uses one worker; the six-worker case uses the
heap's normal serialization of region metadata.

Both VMs used q35, qemu64 with RDRAND/RDSEED, 8 GiB RAM, FAT root, ext2
`/home`, and the GUI running. QEMU used software emulation; `/dev/kvm` was
unavailable. Port 55557 kept this VM separate from the other sessions.
The core-count control changed `-smp 8` to `-smp 1`. Timings include
scheduling and contention, and are not CPU-time measurements. They are
three-round diagnostic samples, not a statistically controlled benchmark
or a prediction of P5 latency.

| Operation | Cores / workers | Median map or allocation | Median touch | Median release | Whole workload |
| --- | --- | --- | --- | --- | --- |
| Raw mapping, untouched | 8 / 1 | 0.028 ms | 0.003 ms | 2.446 ms | 0.021 s |
| Heap allocation, untouched | 8 / 1 | 0.152 ms | 0.004 ms | 3.435 ms | 0.021 s |
| Raw mapping, touched | 8 / 1 | 0.058 ms | 175.243 ms | 5,861.448 ms | 18.145 s |
| Heap allocation, touched | 8 / 6 | 0.434 ms | 10,453.655 ms | 12,871.244 ms | 88.476 s |
| Raw mapping, touched | 1 / 1 | 0.074 ms | 330.509 ms | 157.176 ms | 1.483 s |
| Heap allocation, touched | 1 / 6 | 0.122 ms | 1,825.525 ms | 209.889 ms | 6.427 s |

The median allocation in the contended eight-core run conceals long
outliers: the slowest allocation took **19.164 s**, and the slowest free
took **27.776 s**. The raw eight-core test spent about 97% of its summed
measured phases releasing pages. With one core, median release became
37.3 times faster even though first-touch time increased. The six-worker
workload completed 13.8 times faster on one core. Every recorded run
returned zero and passed `os64_heap_verify()`.

## Where the time spreads

The source path is:

1. [`heap_release`](../../userland/libos64/heap.c) holds the process heap
   lock, marks the heap report in progress, and returns a dedicated region
   through `region_give_back` and `os64_unmap` before unlocking.
2. [`syscall_unmap`](../../kernel/src/syscall.c) walks each resident page,
   removes its user mapping, and calls `free_memory`. The per-page
   `signalLock` protects signal-frame writes through the HHDM alias.
3. [`frames_free_bytes`](../../kernel/src/memory/allocator.c) holds the
   frame allocator lock while removing that allocation's HHDM mapping
   and releasing its frame-table entry.
4. [`paging_hhdm_unmap_range`](../../kernel/src/memory/paging.c) invalidates
   the local mapping and calls `mpSendInvTLB`. For these demand-paged
   allocations, this happens once per 4 KiB page.
5. [`mpSendInvTLB`](../../kernel/src/smp_core.c) sends an individual IPI to
   each other core. The receiving handler reloads CR3.

One populated 64 MiB raw region therefore requests **16,384 broadcasts**,
or **114,688 destination IPI sends** on eight cores, from this path alone.
This is a source-derived count, not a measured number of delivered
interrupts; hardware can coalesce pending interrupts. A heap region also
has metadata pages.

Sparse CPU snapshots during the raw test caught frame-table bookkeeping
on the freeing core and TLB-vector/EOI handling on other cores. A snapshot
during the six-worker case caught kernel spinlock acquisition and an APIC
register read. These snapshots support the call-path analysis but are not
a statistical CPU profile. The much faster single-core release, with no
remote destinations, strongly implicates cross-core invalidation overhead
in this environment; it does not assign an exact fraction to each function.

This explains how cleanup can delay unrelated browser work: the heap lock
blocks sibling allocations, and the frame allocator lock also competes
with demand faults. Keeping the heap report generation odd throughout
release is consistent with the long runs of torn snapshots in the browser
tests. WebP additionally holds its decode gate through scratch cleanup;
waiting decoders inherit that delay. PNG reaches the same memory-release
path without that gate, matching the earlier PNG control.

The raw reproduction rules out codecs, networking, JavaScript, and the
observer as requirements for this particular bottleneck. It does not
establish that every browser delay has the same cause, or quantify WebP
gate waiting and reconstruction time independently. The recent WebP
integration did not edit these kernel/heap paths; this is an existing cost
exposed by the large-image workload, not evidence of a new decoder-loop
regression.

## Repair direction and remaining work

Tracked in [DEBTS.md, Performance / fairness / cleanup](../../DEBTS.md#performance--fairness--cleanup)
as **Batch TLB shootdowns during populated-region release**. The measured
latency has met the scheduling gate; implementation is backlog work.

The first repair candidate is to coalesce redundant remote invalidations
across bounded batches of pages during region release. Preserve immediate
local HHDM maintenance, allocator liveness, the signal-frame lifetime
barrier, and the required remote user-mapping invalidation behavior.
Moving an entire large unmap inside one interrupts-disabled lock would
extend a different latency problem. Dropping the heap lock around unmap
also needs a separate review of shared VMA metadata; it is not a safe
one-line change.

No invalidations or checks were disabled to obtain these results. The
repair needs its own kernel validation, followed by the same benchmark
and the twelve-round browser navigation/close stress. P5 measurements
remain necessary: software-emulated APIC operations exaggerate costs
relative to hardware, and the ratios above do not transfer automatically.
Browser stress acceptance remains open.

## Reproduce and evidence

Build with root `make`, boot scratch images, and run in Os64:

```sh
/tests/memphase map untouched 1 > /home/mem-map-none.txt
/tests/memphase heap untouched 1 > /home/mem-heap-none.txt
/tests/memphase map touch 1 > /home/mem-map-touch.txt
/tests/memphase heap touch 6 > /home/mem-heap-six.txt
```

Repeat the last two commands after booting the same configuration with
one CPU. The test synchronizes the filesystem after reporting its result;
retrieve files with `tools/vmget`. Its result line reports the return code
and heap verification count. It rejects raw map mode with six workers.

The [raw logs and CPU snapshots](stress/memory-profile/) and
[machine-readable summary](stress/memory-profile/summary.json) retain the
measurements. CPU snapshots were taken only in the eight-core runs and
may perturb those timings slightly. The full build passed, including
the benchmark on both images. This investigation adds no sanitizer claim
for the kernel; earlier decoder sanitizer evidence remains separate.
