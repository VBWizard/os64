# Userland memory arenas

Frame Studio needs temporary drawing storage whose allocations share a
lifetime. `libos64` supplies this through `<os64/arena.h>` (also included by
`<os64/os64.h>`), backed by `os64_malloc` and `os64_free`. The implementation
is `userland/libos64/arena.c`. Kernel arenas are separate facilities.

## Lifetime and ownership

An arena owns a collection of heap chunks. Allocation advances a cursor inside
a chunk; adding chunks preserves existing pointers. There is no individual
free, realloc, mark, or rewind. Finishing with one allocation does not reclaim
its space. Never pass an arena allocation to `os64_free` or `os64_realloc`.

`os64_arena_reset` invalidates the arena's allocations together and makes its
chunks reusable, retaining the backing memory. It does not zero the storage.
`os64_arena_destroy` invalidates allocations and releases both chunks and
handle to the heap. Both accept NULL. Reset is O(number of chunks), and
destroy performs one free per chunk plus one for the handle.

Each arena requires caller synchronization. Separate arenas can be used by
separate threads. Reset/destroy must also wait for readers of arena data;
locking allocation alone does not protect readers from lifetime changes.

For Frame Studio, persistent decoration/settings objects must outlive frame
scratch storage. Reset scratch only after drawing has finished consuming its
allocations. If submission queues work holding these pointers, its completion
determines when reset is safe. Integration with Frame Studio is a separate
consumer change; this library does not assume its rendering lifetime.

## API and failure contracts

- `os64_arena_create(initial_capacity, max_bytes)` creates an opaque handle.
  Initial capacity 0 selects 4096 bytes; backing chunks are allocated lazily.
  A nonzero initial capacity is a preferred payload size, not a reservation.
- `os64_arena_alloc(arena, size)` returns uninitialized, 16-byte-aligned
  storage. `os64_arena_alloc_aligned` accepts a nonzero power-of-two alignment,
  including alignments larger than the heap's 16-byte guarantee.
- `os64_arena_calloc(arena, count, size)` checks multiplication and zeroes the
  requested payload, including after reset. Default alignment is 16 bytes.
- `os64_arena_strdup(arena, string)` copies a NUL-terminated string, including
  the terminator. Empty strings are supported; NULL input fails.
- Zero-sized allocation, NULL arena, invalid alignment, arithmetic overflow,
  budget exhaustion, and backing allocation failure return NULL. Failure
  leaves existing contents, allocation cursors, statistics, and growth policy
  unchanged. A failed create returns NULL without retaining heap storage.
- `os64_arena_stats(arena)` returns `used_bytes`, `reserved_bytes`, and
  `chunk_count`; NULL returns zeroes. Used bytes include payload and alignment
  padding. Reserved bytes include the handle, chunk headers, and chunk
  capacities, but exclude malloc metadata and allocator/page rounding.

`max_bytes == 0` means no arena-imposed budget. A nonzero value bounds
`reserved_bytes`, including before the first payload allocation. It is a
bound on the bytes requested from malloc, not physical memory consumption.
After reset, used bytes become zero and reserved bytes/chunk count remain
unchanged. A large one-frame peak remains reserved until destroy; callers
that want to release it can destroy and recreate the arena.

## Chunk policy

The current chunk supplies the fast path: alignment and a cursor increment,
with no heap call. If it cannot fit, search the other retained chunks before
growing; this path is O(number of chunks). Chunks remain in creation order,
and reset restarts at the first chunk so repeated frame workloads reuse their
storage. Some tails can be too small for a particular request even when the
total unused space is sufficient; each allocation must fit in one chunk.

Regular chunk capacities double to 256 KiB. A caller-specified initial size
above that threshold is retained as the regular size. An oversized request
gets a sufficiently large chunk without raising the regular growth target.
Growth clips its preferred capacity to the remaining budget when possible.
If that heap allocation fails and exceeds the request's required capacity,
retry once at the required capacity, including alignment slack. A successful
retry retains the regular growth target; failure of both attempts leaves the
arena unchanged.
Larger alignments reserve a conservative padding bound for new chunks; the
budget can refuse such a request even if a fortunate heap address could have
needed less slack. Existing chunks use their actual alignment padding.

There is no per-allocation heap metadata or arena canary. The backing heap's
checks protect its own chunks; overwriting a neighboring arena allocation
may remain inside a valid heap block. Reset invalidates pointers logically,
not by unmapping or poisoning them. A stale pointer may alias a new object.

## Example

```c
os64_arena_t *scratch = os64_arena_create(4096, 1024 * 1024);
if (!scratch)
    return -1;

/* Repeat while building frames; consume vertices before resetting. */
float *vertices = os64_arena_calloc(scratch, 128, sizeof(*vertices));
if (!vertices) {
    os64_arena_destroy(scratch);
    return -1;
}
/* Build and synchronously consume vertices here. */
os64_arena_reset(scratch);

os64_arena_destroy(scratch);
```

## Verification

`tools/test_arena_host.sh` runs sanitizer-backed lifetime, alignment, growth,
budget, failure-injection, reuse, and payload-integrity checks. Shared cases
also run through the real heap in the guest as `/tests/arenatest`, including
heap verification before and after destruction. The fixture returns success
badge `0xA2E7A000` and is registered in `testrun`. Build with the repository's
default strict userland flags and run `/tests/testrun arenatest` from a freshly
populated image. The host suite also checks preferred-chunk fallback, failure
of both allocation attempts, and continued allocation with a heap that refuses
large blocks. These checks do not establish Frame Studio integration or rendering
performance.
