# P5 image-loading diagnostic

This diagnostic branch is based on D11 and responds to Chris's P5 tests of
the 007 Museum redirect to `www.nybrobildelar.se`. Six simultaneous image
jobs caused external-command and htop stalls exceeding ten seconds. Three
jobs load images faster and reduce command delays to about two seconds;
htop still pauses six to seven seconds. Around 200 loaded images, the
physical allocator reaches its 100,000-entry ledger limit and panics.
Nine CPU-only hogs and a 100,000-operation mallochavoc run do not reproduce
the stalls. Disabling the image/style cache does not reliably improve them.

The diagnostic retains three simultaneous image jobs, a twelve-worker pool
and the existing work budget. `ALLOCATOR_P5_REPORT=1` enables periodic
allocator reports through the buffered kernel log consumed by logd. The
report uses kworker's elapsed ticks, at least ten seconds apart; contention
or delayed scheduling can delay a report. Missed intervals are not replayed.
The table size, allocation policy, locks and page lifetimes are unchanged.

The existing ledger walk reports `entries`, `inuse`, `free`, `dead`,
`exactfit`, `splits`, `merges`, `compactions` and common free-hole sizes.
The additional `allocator P5:` line reports:

- `live_page_entries`: live, aligned, exactly 4 KiB extents. This is an
  allocation shape, not ownership attribution to Yonder.
- `live_bytes`: bytes in live ledger entries, including reserved entries
  seeded from the firmware memory map. Compare changes, not this total
  directly with a process's heap size.
- `searches` and `examined`: cumulative ordinary allocation searches and
  ledger entries they examine. The ratio of the changes between consecutive
  reports is the mean number of entries examined per search in that interval.
  Counters are captured with the ledger under its lock and printed after
  unlocking. Searches by explicitly requested physical address are excluded.
- `paging_used`: pages consumed from the separate paging-table pool and
  its capacity. This pool has a different exhaustion panic.

The report adds a periodic full ledger walk under its existing lock;
diagnostic overhead is not zero. Search counters update at search exits,
rather than on each examined entry. Setting `ALLOCATOR_P5_REPORT=0` omits
the search counters and restores DEBUG_ALLOCATOR-gated reporting.

Boot the diagnostic kernel with KWORKER and logd enabled. Keep the current
three-image Yonder executable and cache settings. Capture an idle baseline,
start the Museum load, then stop loading before the ledger reaches its limit.
Continue collecting reports after closing Yonder to see whether live entries
fall. The normal physical-root boot sends logd output to `/home/os64.log`;
ramdisk entries can select `/fat/os64.log` instead. Use the active boot's
LOGD destination. Search the saved log for `allocator:` and `allocator P5:`.

The optional `/tests/loadprobe` executable times file access, child creation,
allocation and monitoring independently. It is not required for the first
ledger-growth comparison. These diagnostics do not establish that growth is
a leak or that scanning alone accounts for the observed delays.

Verification: the full diagnostic image builds. An eight-core headless QEMU
boot passes 34 initial and three deferred built-in tests. Logd's saved file
contains four reports spaced about ten seconds apart, with 760–806 ledger
entries and paging usage stable at 280/5191. This verifies the report's timer
and file delivery; the P5 image-loading workload remains the required check.

## P5 ledger-growth results

Chris's run of this diagnostic starts around 2,800 entries and 1,000 live
page-shaped entries. At roughly 80 loaded images it reaches 70,767 entries:
70,250 in use, 498 free, 19 dead, with 66,220 live page-shaped entries.
The paging pool uses 523/12,616 pages. After Yonder is killed and cleanup
settles, the ledger has 4,808 entries, 4,094 in use and 1,158 live
page-shaped entries; paging usage is 529/12,616. The remaining difference
from the initial baseline is not attributed to a particular owner.

Differences between successive cumulative counters show the search cost:

| Interval | Searches | Entries examined | Mean entries per search |
| --- | ---: | ---: | ---: |
| 15:58:10–15:58:20 | 8,423 | 19,157,248 | 2,274 |
| 15:59:20–15:59:30 | 15,255 | 732,677,672 | 48,029 |
| 15:59:30–15:59:40 | 17,043 | 921,377,371 | 54,062 |
| 16:00:12–16:00:22 | 7,426 | 468,114,988 | 63,037 |
| 16:03:05–16:03:15 | 416 | 1,340,322 | 3,222 |

The growth is predominantly live allocations rather than dead entries
awaiting compaction. The increase from 1,034 to 66,220 live page-shaped
entries represents approximately 254.6 MiB of 4 KiB allocations, consistent
in scale with Yonder's 256 MiB retained-image budget. This shape is not proof
of the owning subsystem, but most page-shaped entries disappear on exit.
Repeated allocation searches scan tens of thousands of entries while
holding the allocator's global irqsave lock; this strongly supports scan
growth as a contributor to the system stalls. These counters measure visits,
not elapsed lock hold or wait time, so they do not apportion all observed
latency. The paging pool remains well below its separate limit.

Two issues need separate treatment: lookup cost grows with the live ledger,
and the fixed ledger can exhaust on legitimate demand-paged image memory.
Increasing capacity leaves the scan cost; indexing searches alone leaves
the capacity limit. Reverting exact-fit preference also reintroduces the
free-hole fragmentation problem that motivated the allocator's current
policy. A fix must preserve recycling while accounting for live-page scale.
