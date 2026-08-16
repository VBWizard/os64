// heap.c — os64's userland malloc.
//
// THE DESIGN, in one paragraph, so the code below reads as prose:
// memory comes from the kernel in REGIONS (os64_map — page-rounded,
// demand-paged, zeroed, guard page after, addresses never reused). Small
// requests are carved out of POOL regions with BOUNDARY TAGS (Knuth, 1968:
// the block's size recorded at both ends, so a free can merge with its
// neighbour on either side in O(1), at free time, with no scheduled pass
// ever). Requests at or above HEAP_BIG_BYTES get a DEDICATED region of their
// own, and freeing one is a single os64_unmap. A pool that empties out is
// handed back to the kernel too — which is the thing brk-based mallocs
// cannot do, since brk can only shrink from the top. The full design
// conversation, and which decisions were Chris's rulings on which date,
// is MALLOC.md.
//
// LINEAGE. This is the third generation of the same instinct. os32's
// allocator (2016) put a header on every block with a `marker` field it
// verified on free — a heap canary, independently invented, before the
// industry had settled the name. What os32 never got was coalescing: two
// adjacent free chunks stayed split forever. Round two gets Knuth's tags,
// gets the canary back with teeth (a bad one is now LOUD — os32 silently
// skipped the block, which Chris's verdict on his own code was "not even a
// debug print... for shame"), and gets a thing Knuth's machine could not
// have imagined: the ability to hand a whole region back to the operating
// system from the middle of the heap.
//
// THREADS. Threads in a task share one address space and therefore share
// this heap, so every entry point takes gHeapLock. That makes malloc the
// first genuine consumer of shared mutable state in os64 userland (DEBTS'
// thread rows predicted it would be), which is why the lock lives here,
// sized to its one consumer, rather than in a general mutex API designed
// before anything needed one.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "os64/heap.h"     // the ABI report struct — shared with procfs
#include "os64/io.h"       // os64_write, os64_serial_log
#include "os64/fmt.h"      // os64_snprintf
#include "os64/mem.h"      // os64_map / os64_unmap / os64_heap_publish
#include "os64/proc.h"     // os64_exit, os64_yield, os64_getenv
#include "os64/str.h"      // os64_memset / os64_memcpy / os64_streq

// ── The numbers, named out loud (MALLOC.md house rule: no magic literals) ───

#define HEAP_ALIGN         16u          // every payload, always (SSE-ready ABI)
#define HEAP_PAGE          4096u        // what the kernel rounds regions to

// A block: 16-byte header, payload, and — only while FREE — two list links at
// the front of the payload and a size footer at its end. So the smallest
// block that can ever be freed is header + links + footer, rounded to the
// alignment: 16 + 16 + 8 -> 48. An in-use block pays only the 16-byte header;
// the footer is Doug Lea's refinement of Knuth, and it is why the in-use bit
// exists (see HEAP_PREV_FREE).
#define HEAP_MIN_BLOCK     48u

// The first pool. Demand paging makes this nearly free: a mapped-but-untouched
// megabyte costs ZERO physical memory in os64, so the pool is generous and a
// program that mallocs forty bytes touches exactly one page.
#define HEAP_POOL_BYTES    (1024u * 1024u)

// At or above this, an allocation gets its own region and its free is one
// unmap. 128KB is the classic threshold (dlmalloc's, glibc's starting value)
// and it is chosen here for the reason it was chosen there: above it, the
// fragmentation a first-fit list suffers is worse than the cost of a syscall.
#define HEAP_BIG_BYTES     (128u * 1024u)

// Block flags, in the low 4 bits of the size word (sizes are 16-aligned, so
// those bits are structurally free — the trick every boundary-tag allocator
// since 1968 has used).
#define HEAP_IN_USE        0x1u   // this block is the program's
#define HEAP_PREV_FREE     0x2u   // the block BELOW us is free: read its footer
#define HEAP_DIRTY         0x4u   // has held data/bookkeeping: calloc must zero
#define HEAP_DEDICATED     0x8u   // sole occupant of its own region
#define HEAP_FLAG_MASK     0xFu

// The canary. Address-tied and size-tied on purpose: a header copied from
// somewhere else in the heap does NOT validate at its new address, so this
// catches a stomp that a constant magic number would sail straight past.
// Two seeds, so that a live block and a freed block are distinguishable —
// which is exactly how a double free is caught by name instead of by
// symptom. (os32 used one constant and a silent skip. We know better now.)
#define HEAP_CANARY_LIVE   0x05CA9A12ABCDEF01ULL
#define HEAP_CANARY_FREE   0x0DEADCA9A12F1EEDULL
#define HEAP_CANARY_MIX    0x9E3779B97F4A7C15ULL   // golden ratio, 64-bit

// Poison. A freed body is filled with this so a use-after-free reads
// obviously-wrong data LOUDLY instead of working by luck — the ring-3 sibling
// of the kernel's unmap-the-HHDM-on-free tripwire, one privilege level up.
#define HEAP_POISON        0xA5

// The region ledger's own magic, checked on every containment walk.
#define HEAP_REGION_MAGIC  0x5245474E30303634ULL   // "REGN0064"

// Exit codes for heap crimes. If we are going to die, we may as well die
// spelling (the fixture badges next door set the precedent).
#define HEAP_EXIT_FREE_BAD 0xF12EEBAD   // "FREE BAD" — wild free, or a second one
#define HEAP_EXIT_CANARIED 0xCA9A12ED   // "CANARIED" — somebody stomped a header

// ── Structures ──────────────────────────────────────────────────────────────

// A block header. Sixteen bytes, and every one of them earns its place.
typedef struct
{
	uint64_t size_flags;   // block size (multiple of 16) | flags
	uint64_t canary;
} heap_block_t;

// A free block's payload starts with these two links. (An in-use block's
// payload starts with the program's data — that is the whole point of the
// in-band design Chris ratified: the metadata is glued to the block, and the
// links live in space the program is not using anyway.)
typedef struct heap_free
{
	heap_block_t hdr;
	struct heap_free *next;
	struct heap_free *prev;
} heap_free_t;

// A region: one os64_map, one ledger entry. The ledger is THREADED THROUGH
// THE REGIONS THEMSELVES — each region's first 32 bytes are its own entry —
// so the ledger needs no allocation of its own and cannot run out of slots.
// (A static array of entries was the obvious alternative and would have had a
// bootstrap problem: the ledger would need a size chosen before anyone knew
// the program.)
typedef struct heap_region
{
	uint64_t             magic;      // HEAP_REGION_MAGIC
	struct heap_region  *next;
	uint64_t             bytes;      // exactly what we asked the kernel for
	uint64_t             flags;      // HEAP_REGION_* below
} heap_region_t;

#define HEAP_REGION_POOL       0x1u   // carved into blocks
#define HEAP_REGION_DEDICATED  0x2u   // one big allocation, alone
#define HEAP_REGION_KEEP       0x4u   // the primordial pool: never given back

// A pool region's runtime state lives right behind its ledger entry. Kept in
// a second struct purely for readability: `heap_pool_t` is what a POOL region
// actually looks like in memory.
typedef struct
{
	heap_region_t region;
	uint64_t      frontier;   // offset of the first VIRGIN byte in this region
	uint64_t      live;       // blocks currently handed to the program
	uint64_t      carve_end;  // offset one past the last carvable byte
	uint64_t      pad;        // keeps the first block 16-aligned
} heap_pool_t;

// A dedicated region: ledger entry, one block header, then the payload.
typedef struct
{
	heap_region_t region;
	uint64_t      pad0, pad1;   // 16-align the block header that follows
	heap_block_t  block;
} heap_dedicated_t;

// ── State (per PROCESS: libos64 links into each program privately) ──────────

static heap_region_t *gRegions;      // the ledger, newest first
static heap_free_t   *gFreeList;     // one list, first-fit (Chris's ruling)
static bool           gInited;
static bool           gCheckAlways;  // HEAPCHECK=1 in the environment
static volatile uint32_t gLock;

// The report the kernel renders as /proc/<pid>/heap. Its ADDRESS is what gets
// registered, so it must be a plain global: .bss, fixed for the life of the
// program, and (being written before any thread exists) safe to publish once.
static os64_heap_report_t gReport;

// ── The lock ────────────────────────────────────────────────────────────────
//
// Test-and-set, spin a little, then yield. Deliberately NOT a general mutex:
// os64's mutex will be designed when a program (not a library) needs one, and
// designing it here — for a critical section measured in dozens of
// instructions — would be designing it for the wrong customer. The spin count
// is small because the holder never blocks: no syscall is made with this lock
// held except the map/unmap that grows or shrinks the heap.

#define HEAP_SPINS_BEFORE_YIELD 64

static void heap_lock(void)
{
	for (;;)
	{
		if (__atomic_exchange_n(&gLock, 1u, __ATOMIC_ACQUIRE) == 0u)
			return;

		for (int i = 0; i < HEAP_SPINS_BEFORE_YIELD; i++)
		{
			if (__atomic_load_n(&gLock, __ATOMIC_RELAXED) == 0u)
				break;
			__asm__ volatile("pause");
		}
		if (__atomic_load_n(&gLock, __ATOMIC_RELAXED) != 0u)
			os64_yield();
	}
}

static void heap_unlock(void)
{
	__atomic_store_n(&gLock, 0u, __ATOMIC_RELEASE);
}

// ── Complaint and death ─────────────────────────────────────────────────────

// Both doors: stderr for the human at the glass, the serial wire for the log
// that survives the program. panic() does exactly this in the kernel, and for
// the same reason — the last byte before a death is the one that matters.
static void heap_complain(const char *what, const void *ptr, uint64_t detail)
{
	char line[192];
	os64_snprintf(line, sizeof(line), "heap: %s (ptr=%p detail=0x%lx)\n",
	              what, ptr, detail);
	os64_write(2, line, os64_strlen(line));
	os64_serial_log(line);
}

// A heap crime kills the program. Chris's ruling, 2026-08-15: "killing tasks
// is absolutely the right way to go on free(garbage)." A corrupted heap has
// already lost; continuing only moves the crash somewhere less informative.
// The lock is dropped first — we are dying, and a held lock would hang any
// sibling thread that tried to report anything on the way out.
static void heap_die(const char *what, const void *ptr, uint64_t detail,
                     int32_t code) __attribute__((noreturn));
static void heap_die(const char *what, const void *ptr, uint64_t detail,
                     int32_t code)
{
	heap_unlock();
	heap_complain(what, ptr, detail);
	os64_exit(code);
}

// ── Block arithmetic ────────────────────────────────────────────────────────

static inline uint64_t block_size(const heap_block_t *b)
{
	return b->size_flags & ~(uint64_t)HEAP_FLAG_MASK;
}

static inline uint64_t block_flags(const heap_block_t *b)
{
	return b->size_flags & HEAP_FLAG_MASK;
}

static inline bool block_in_use(const heap_block_t *b)
{
	return (b->size_flags & HEAP_IN_USE) != 0;
}

static inline uint64_t heap_canary_for(const heap_block_t *b, uint64_t size,
                                       bool in_use)
{
	return (in_use ? HEAP_CANARY_LIVE : HEAP_CANARY_FREE)
	     ^ (uint64_t)(uintptr_t)b
	     ^ (size * HEAP_CANARY_MIX);
}

static inline void block_set(heap_block_t *b, uint64_t size, uint64_t flags)
{
	b->size_flags = size | (flags & HEAP_FLAG_MASK);
	b->canary = heap_canary_for(b, size, (flags & HEAP_IN_USE) != 0);
}

static inline bool block_canary_ok(const heap_block_t *b)
{
	return b->canary == heap_canary_for(b, block_size(b), block_in_use(b));
}

static inline void *block_payload(heap_block_t *b)
{
	return (void *)((char *)b + sizeof(heap_block_t));
}

static inline heap_block_t *payload_block(void *p)
{
	return (heap_block_t *)((char *)p - sizeof(heap_block_t));
}

static inline heap_block_t *block_next(heap_block_t *b)
{
	return (heap_block_t *)((char *)b + block_size(b));
}

// The footer: the block's size, repeated at its last 8 bytes. Only free
// blocks carry one — an in-use block's neighbours never need to walk
// backwards into it, because HEAP_PREV_FREE tells them not to bother.
static inline void block_write_footer(heap_block_t *b)
{
	uint64_t size = block_size(b);
	*(uint64_t *)((char *)b + size - sizeof(uint64_t)) = size;
}

static inline uint64_t block_prev_footer(const heap_block_t *b)
{
	return *(const uint64_t *)((const char *)b - sizeof(uint64_t));
}

static inline uint64_t align_up(uint64_t v, uint64_t a)
{
	return (v + a - 1) & ~(a - 1);
}

// ── The report ──────────────────────────────────────────────────────────────

// Bucket a block by size: 16..31, 32..63, 64..127, ... The histogram counts
// PAYLOAD CAPACITY, not what the caller asked for. That is deliberate and it
// is the honest number: capacity is what the heap actually committed, and it
// is recoverable from the header at free time, so the count that goes up and
// the count that comes down are computed the same way and can never drift.
static uint32_t heap_class_of(uint64_t payload_bytes)
{
	uint32_t cls = 0;
	uint64_t limit = 1ULL << OS64_HEAP_CLASS_MIN_SHIFT;   // 16

	while (cls + 1 < OS64_HEAP_CLASSES && payload_bytes >= limit * 2)
	{
		limit *= 2;
		cls++;
	}
	return cls;
}

// generation: odd while the numbers are in motion, even when they agree with
// each other. The kernel prints "torn yes" if it catches an odd one, rather
// than publishing a set of figures that never coexisted.
static inline void report_begin(void) { gReport.generation++; }

// Overhead is DERIVED rather than accumulated, because it is exactly
// derivable: one header per block that exists, plus each region's fixed
// furniture (a pool's own struct and its end epilogue; a dedicated region's
// struct minus the block header already counted above). Deriving it means
// there is no third counter to drift out of step with the other two — and
// what is left over, bytes_virgin, is tracked by the three places that
// actually move the frontier, so the audit identity in os64/heap.h is a real
// check rather than an arithmetic tautology.
static inline void report_end(void)
{
	gReport.bytes_overhead =
	      (gReport.blocks_live + gReport.blocks_free) * sizeof(heap_block_t)
	    + gReport.region_pools * (sizeof(heap_pool_t) + sizeof(heap_block_t))
	    + gReport.region_dedicated * (sizeof(heap_dedicated_t) - sizeof(heap_block_t));
	gReport.generation++;
}

// ── Free list ───────────────────────────────────────────────────────────────
//
// One doubly-linked list, first fit. Size-class bins were presented in the
// design conversation and ruled OVERHEAD for this OS ("verdict delivered from
// the floor, laughing" — MALLOC.md). Knuth's own baseline ran the world's
// timesharing systems on exactly this, and the day regret arrives the bins
// can be added without any caller noticing.

static void free_list_insert(heap_free_t *f)
{
	// A BLOCK ON THIS LIST IS DIRTY BY DEFINITION, and marking it here is the
	// whole enforcement of that rule.
	//
	// HEAP_DIRTY answers exactly one question: "may calloc skip the memset?"
	// It may only when every byte of the payload is still the kernel's zero,
	// which is true of precisely two things — memory carved off a pool's
	// virgin frontier, and a fresh dedicated region. The instant a block
	// joins this list, the allocator writes its two list links into the front
	// of that payload and a size footer into the back, so the answer becomes
	// no, forever.
	//
	// It used to be marked by INHERITANCE instead (each split handed its
	// dirtiness to its leftover), and inheritance had a hole: realloc growing
	// a CLEAN live block over a freed neighbour, then splitting, gave the
	// leftover the parent's clean flag while the memory itself came from the
	// poisoned neighbour. The next calloc trusted the flag and handed a
	// caller 0xA5 (found by Chris's own malloc test, 2026-08-15, hours after
	// the heap shipped). One choke point ends the whole class: whoever splits,
	// merges, or shrinks anything cannot forget what they never had to
	// remember.
	f->hdr.size_flags |= HEAP_DIRTY;   // flags are outside the canary's input

	f->prev = NULL;
	f->next = gFreeList;
	if (gFreeList != NULL)
		gFreeList->prev = f;
	gFreeList = f;

	// Free bytes are counted as PAYLOAD, exactly like live bytes: the header
	// is overhead in both states, so a block changing hands never moves bytes
	// between the two columns. That is what keeps the audit identity honest.
	uint64_t payload = block_size(&f->hdr) - sizeof(heap_block_t);
	gReport.blocks_free++;
	gReport.bytes_free += payload;
	if (payload > gReport.largest_free)
		gReport.largest_free = payload;
}

// Recompute the largest free block. Called ONLY when the reigning largest is
// removed — every other path maintains it incrementally, so a /proc reader
// never sees a stale fragmentation figure.
static void free_list_rescan_largest(void)
{
	uint64_t largest = 0;

	for (heap_free_t *f = gFreeList; f != NULL; f = f->next)
	{
		uint64_t s = block_size(&f->hdr) - sizeof(heap_block_t);
		if (s > largest)
			largest = s;
	}
	gReport.largest_free = largest;
}

static void free_list_remove(heap_free_t *f)
{
	if (f->prev != NULL)
		f->prev->next = f->next;
	else
		gFreeList = f->next;
	if (f->next != NULL)
		f->next->prev = f->prev;
	f->next = NULL;
	f->prev = NULL;

	uint64_t payload = block_size(&f->hdr) - sizeof(heap_block_t);
	gReport.blocks_free--;
	gReport.bytes_free -= payload;
	if (payload >= gReport.largest_free)
		free_list_rescan_largest();
}

// ── Regions ─────────────────────────────────────────────────────────────────

static inline heap_pool_t *pool_of(heap_region_t *r) { return (heap_pool_t *)r; }

static inline heap_block_t *pool_first_block(heap_pool_t *p)
{
	return (heap_block_t *)((char *)p + sizeof(heap_pool_t));
}

static void region_link(heap_region_t *r)
{
	r->next = gRegions;
	gRegions = r;
	gReport.regions++;
	gReport.bytes_mapped += r->bytes;
}

static void region_unlink(heap_region_t *r)
{
	heap_region_t **pp = &gRegions;

	while (*pp != NULL && *pp != r)
		pp = &(*pp)->next;
	if (*pp == r)
		*pp = r->next;

	gReport.regions--;
	gReport.bytes_mapped -= r->bytes;
}

// Which region owns this address? Walked before ANY header is dereferenced on
// a caller-supplied pointer — so free(garbage) is diagnosed by reading only
// our own structures, and dies with a badge instead of taking a page fault at
// a wild address. Regions are few (a program with a dozen is unusual), so the
// walk is cheap; the day one has hundreds, this is the line to make a tree.
static heap_region_t *region_of(const void *p)
{
	uintptr_t a = (uintptr_t)p;

	for (heap_region_t *r = gRegions; r != NULL; r = r->next)
	{
		uintptr_t base = (uintptr_t)r;

		if (r->magic != HEAP_REGION_MAGIC)
		{
			heap_complain("region ledger corrupted", r, r->magic);
			return NULL;
		}
		if (a > base && a < base + r->bytes)
			return r;
	}
	return NULL;
}

// Take a fresh region from the kernel. `bytes` is rounded to whole pages HERE
// rather than letting the kernel round silently, so the ledger's number is
// exactly what exists — the difference matters when the last block's end is
// compared against the region's end.
static heap_region_t *region_map(uint64_t bytes, uint64_t flags)
{
	bytes = align_up(bytes, HEAP_PAGE);

	void *base = os64_map((size_t)bytes);
	if (base == NULL)
		return NULL;

	heap_region_t *r = (heap_region_t *)base;
	r->magic = HEAP_REGION_MAGIC;
	r->next  = NULL;
	r->bytes = bytes;
	r->flags = flags;

	gReport.calls_map++;
	region_link(r);
	return r;
}

static void region_give_back(heap_region_t *r)
{
	uint64_t flags = r->flags;

	region_unlink(r);
	if (flags & HEAP_REGION_POOL)
	{
		heap_pool_t *p = pool_of(r);
		gReport.bytes_virgin -= p->carve_end - p->frontier;   // uncarved tail goes home too
		gReport.region_pools--;
	}
	else
	{
		gReport.region_dedicated--;
	}

	// The ledger entry lives INSIDE the region being released, so nothing may
	// touch `r` after this call — the addresses stop existing (and by design
	// are never handed out again, so a stale pointer faults forever).
	os64_unmap(r);
	gReport.calls_unmap++;
}

// Grow the heap by one pool region big enough for `need` bytes of block.
static heap_pool_t *pool_new(uint64_t need)
{
	uint64_t want = sizeof(heap_pool_t) + need + sizeof(heap_block_t);
	uint64_t bytes = (want > HEAP_POOL_BYTES) ? want : HEAP_POOL_BYTES;

	heap_region_t *r = region_map(bytes, HEAP_REGION_POOL);
	if (r == NULL)
		return NULL;

	heap_pool_t *p = pool_of(r);
	p->frontier  = sizeof(heap_pool_t);
	p->live      = 0;
	// The last 16 bytes are reserved for the EPILOGUE — a header with size 0,
	// which every walk treats as "stop". Without it, coalescing forward from
	// the final block would read the guard page and take the fault the guard
	// page exists to give. (Virgin memory reads as a size-0 header too, since
	// the kernel zeroes every page — so the frontier fenceposts itself. The
	// epilogue is written anyway: a fencepost that depends on nobody ever
	// writing there is a fencepost you find out about the hard way.)
	p->carve_end = r->bytes - sizeof(heap_block_t);
	p->pad       = 0;

	heap_block_t *epilogue = (heap_block_t *)((char *)p + p->carve_end);
	block_set(epilogue, 0, HEAP_IN_USE);

	// Everything between the pool header and the epilogue is VIRGIN: mapped,
	// never carved, and — this being os64 — costing not one byte of physical
	// memory until something touches it.
	gReport.bytes_virgin += p->carve_end - p->frontier;

	gReport.region_pools++;
	if (gReport.region_pools == 1)
		r->flags |= HEAP_REGION_KEEP;   // the primordial pool stays forever

	return p;
}

// ── Carving ─────────────────────────────────────────────────────────────────

// Split `b` (already removed from the free list, or freshly carved) so that it
// holds exactly `need` bytes, returning any remainder to the free list.
static void block_split(heap_block_t *b, uint64_t need, uint64_t keep_flags)
{
	uint64_t size = block_size(b);
	uint64_t rest = size - need;
	heap_block_t *successor = block_next(b);   // BEFORE b's size changes

	if (rest < HEAP_MIN_BLOCK)
	{
		// Not worth splitting: the caller gets the slack. (A remainder too
		// small to carry links and a footer could never be freed, which is
		// the whole reason for the minimum.)
		block_set(b, size, keep_flags | HEAP_IN_USE);

		// AND the successor must be told its neighbour is no longer free.
		// Forgetting this is a whole class of boundary-tag bug: the stale
		// PREV_FREE sends a later free BACKWARDS into a block that is now in
		// use, merging live memory into a free block. Caught here on the
		// first host run — the tag that lies costs more than the tag that
		// isn't there (heap.c's version of the house doctrine on names).
		//
		// Unconditional, epilogues included: a size-0 header is a fencepost
		// for WALKING, not an excuse to leave its neighbour bit stale — the
		// frontier's header is exactly such a fencepost and pool_carve reads
		// this bit out of it.
		successor->size_flags &= ~(uint64_t)HEAP_PREV_FREE;
		return;
	}

	block_set(b, need, keep_flags | HEAP_IN_USE);

	heap_block_t *tail = block_next(b);
	// The remainder inherits DIRTY from its parent: if the parent had ever
	// held data or list links, so has this memory.
	block_set(tail, rest, keep_flags & HEAP_DIRTY);
	block_write_footer(tail);

	heap_block_t *after = block_next(tail);
	after->size_flags |= HEAP_PREV_FREE;

	free_list_insert((heap_free_t *)tail);
}

// First fit, exactly as ratified: walk the one list, take the first block big
// enough. (Best-fit was the alternative Knuth analysed; first-fit wins on
// modern workloads for the same reason it won on his — it keeps the small
// leftovers together at the front instead of manufacturing new ones.)
static heap_block_t *free_list_first_fit(uint64_t need)
{
	for (heap_free_t *f = gFreeList; f != NULL; f = f->next)
	{
		if (!block_canary_ok(&f->hdr))
			heap_die("free-list block has a bad canary", f, f->hdr.canary,
			         HEAP_EXIT_CANARIED);
		if (block_size(&f->hdr) >= need)
			return &f->hdr;
	}
	return NULL;
}

// Carve `need` bytes off a pool's virgin frontier. Virgin memory has never
// held bookkeeping OR user data, so the block comes back CLEAN — which is
// what makes calloc free for the common case: the kernel already zeroed
// these pages and nobody has written to them since.
static heap_block_t *pool_carve(heap_pool_t *p, uint64_t need)
{
	if (p->frontier + need > p->carve_end)
		return NULL;

	heap_block_t *b = (heap_block_t *)((char *)p + p->frontier);

	// THE FRONTIER CARRIES A HEADER, AND THAT HEADER IS WHERE PREV_FREE LIVES.
	//
	// This block's predecessor may be free (somebody freed the last carved
	// block) or in use, and we must inherit the right answer — but there is
	// exactly one wrong way to find out, and this code did it first: peek at
	// the 8 bytes below and treat them as the predecessor's footer. An IN-USE
	// block has NO footer; those 8 bytes are the PROGRAM'S DATA. Guess with
	// them and a program whose last word happens to look like a small size
	// gets a PREV_FREE bit it never earned, and the next free() merges
	// backwards into the middle of somebody's live buffer.
	//
	// (Found within a minute of the churn fixture existing, on the real OS,
	// after a 20,000-round host soak sailed past it — the soak's stamp bytes
	// never happened to spell a plausible footer. The tripwire caught it as
	// designed: "the block below this one is not the free block its tag
	// claims", and the program died instead of corrupting itself quietly.)
	//
	// So the bit is READ from the header already sitting at the frontier,
	// which the free path maintains exactly like any other block's — and
	// which starts life correct for free, because a virgin region is zeroed
	// (size 0, no flags) and a zero header means "the block below is in use".
	uint64_t prev_free = block_flags(b) & HEAP_PREV_FREE;

	block_set(b, need, prev_free | HEAP_IN_USE);
	p->frontier += need;
	gReport.bytes_virgin -= need;

	// And the new frontier header inherits the truth about US: in use.
	// Unconditionally — the header at the frontier is either virgin zeroes or
	// the pool's end epilogue, and both are ours to keep honest.
	heap_block_t *after = block_next(b);
	after->size_flags &= ~(uint64_t)HEAP_PREV_FREE;

	return b;
}

// ── init ────────────────────────────────────────────────────────────────────

void os64_heap_init(void)
{
	if (gInited)
		return;
	gInited = true;

	os64_memset(&gReport, 0, sizeof(gReport));
	gReport.magic   = OS64_HEAP_REPORT_MAGIC;
	gReport.version = OS64_HEAP_REPORT_VERSION;

	// HEAPCHECK=1: verify every canary in every region on every malloc and
	// free. Slow on purpose — MALLOC_CHECK done honestly, as a bug-hunt gear
	// you shift into, not a mode that changes what the allocator DOES.
	const char *check = os64_getenv("HEAPCHECK");
	gCheckAlways = (check != NULL && check[0] == '1' && check[1] == '\0');

	// Hand the kernel the report's address. From this instant
	// /proc/<pid>/heap answers for this program — even if it never allocates
	// a byte, which is exactly why this is eager and not lazy-on-first-malloc.
	os64_heap_publish(&gReport);
}

// ── verify ──────────────────────────────────────────────────────────────────

static uint64_t heap_verify_locked(void)
{
	uint64_t problems = 0;

	for (heap_region_t *r = gRegions; r != NULL; r = r->next)
	{
		if (r->magic != HEAP_REGION_MAGIC)
		{
			heap_complain("region magic wrong", r, r->magic);
			problems++;
			break;   // the chain itself is suspect; walking further is guessing
		}

		if (r->flags & HEAP_REGION_DEDICATED)
		{
			heap_dedicated_t *d = (heap_dedicated_t *)r;
			if (!block_canary_ok(&d->block))
			{
				heap_complain("dedicated block canary", &d->block, d->block.canary);
				problems++;
			}
			continue;
		}

		heap_pool_t *p = pool_of(r);
		heap_block_t *b = pool_first_block(p);
		uint64_t walked = sizeof(heap_pool_t);
		bool prev_was_free = false;

		while (walked < p->frontier)
		{
			uint64_t size = block_size(b);

			// THE VERIFIER MUST SURVIVE THE DISEASE IT DIAGNOSES. Every
			// number below comes from memory that may already be stomped, so
			// the size is checked for containment BEFORE it is used to step
			// anywhere: a clobbered header holding 0xCCCC... passes an
			// alignment test happily and then walks the verifier off the map.
			// (It did exactly that the first time the HEAPCHECK knob was
			// turned on over a deliberately corrupted heap — the same shape
			// as panic() being forbidden to touch the filesystem, which it
			// learned from a panic caused by an unreadable filesystem.)
			if (size == 0 || (size & (HEAP_ALIGN - 1)) != 0
			    || size > p->frontier - walked)
			{
				heap_complain("block size implausible", b, size);
				problems++;
				break;
			}
			if (!block_canary_ok(b))
			{
				// Stop walking THIS region: past a corrupt header every
				// further step is a guess, and a guess that reports twenty
				// invented problems buries the one real one.
				heap_complain("block canary", b, b->canary);
				problems++;
				break;
			}
			if (prev_was_free != ((block_flags(b) & HEAP_PREV_FREE) != 0))
			{
				heap_complain("prev-free bit disagrees with the block below",
				              b, block_flags(b));
				problems++;
			}
			// Two free neighbours is the invariant coalescing EXISTS to keep,
			// and it is invisible to every tag check above: both blocks carry
			// honest sizes, honest canaries, and consistent bits. It needs its
			// own tripwire — the realloc-shrink bug lived exactly here for a
			// day, detectable only as a downstream give-back complaint.
			if (prev_was_free && !block_in_use(b))
			{
				heap_complain("two adjacent free blocks (a merge was missed)",
				              b, block_size(b));
				problems++;
			}
			if (!block_in_use(b))
			{
				uint64_t footer = *(uint64_t *)((char *)b + size - sizeof(uint64_t));
				if (footer != size)
				{
					heap_complain("footer disagrees with header", b, footer);
					problems++;
				}
			}

			prev_was_free = !block_in_use(b);
			walked += size;
			b = block_next(b);
		}

		// A walk that stopped early already reported WHY (and problems > 0
		// says so); only a clean walk that still doesn't land on the frontier
		// is news.
		if (walked != p->frontier && problems == 0)
		{
			heap_complain("block walk overshot the frontier", p, walked);
			problems++;
		}
	}

	// The audit identity (os64/heap.h): every mapped byte is live, free,
	// overhead, or virgin — nothing else, and nothing twice.
	uint64_t accounted = gReport.bytes_live + gReport.bytes_free
	                   + gReport.bytes_overhead + gReport.bytes_virgin;
	if (accounted != gReport.bytes_mapped)
	{
		heap_complain("the books do not balance (live+free+overhead+virgin != mapped)",
		              &gReport, accounted);
		problems++;
	}

	return problems;
}

uint64_t os64_heap_verify(void)
{
	heap_lock();
	uint64_t problems = heap_verify_locked();
	heap_unlock();
	return problems;
}

// ── malloc ──────────────────────────────────────────────────────────────────

static void *malloc_dedicated(uint64_t size)
{
	uint64_t want = sizeof(heap_dedicated_t) + size;

	heap_region_t *r = region_map(want, HEAP_REGION_DEDICATED);
	if (r == NULL)
		return NULL;

	heap_dedicated_t *d = (heap_dedicated_t *)r;
	d->pad0 = 0;
	d->pad1 = 0;

	// The block covers everything from its header to the end of the region:
	// page rounding means the caller silently gets the slack, which is the
	// one place in this allocator where "more than you asked for" is free.
	uint64_t block_bytes = r->bytes - (uint64_t)((char *)&d->block - (char *)r);
	block_set(&d->block, block_bytes, HEAP_IN_USE | HEAP_DEDICATED);

	gReport.region_dedicated++;
	return block_payload(&d->block);
}

void *os64_malloc(size_t size)
{
	if (!gInited)
		os64_heap_init();

	// A block must hold header + payload, be 16-aligned, and be at least big
	// enough to be freeable later. malloc(0) therefore returns a real,
	// freeable minimum block rather than NULL — `free(malloc(0))` is honest.
	uint64_t need = align_up((uint64_t)size + sizeof(heap_block_t), HEAP_ALIGN);
	if (need < HEAP_MIN_BLOCK)
		need = HEAP_MIN_BLOCK;
	if (need < size)            // overflow of the addition above
		return NULL;

	heap_lock();
	report_begin();
	gReport.calls_malloc++;

	if (gCheckAlways)
		heap_verify_locked();

	void *out = NULL;

	if (size >= HEAP_BIG_BYTES)
	{
		out = malloc_dedicated((uint64_t)size);
		if (out != NULL)
		{
			heap_block_t *b = payload_block(out);
			uint64_t payload = block_size(b) - sizeof(heap_block_t);
			gReport.blocks_live++;
			gReport.bytes_live += payload;
			gReport.live_by_class[heap_class_of(payload)]++;
			if (gReport.bytes_live > gReport.high_water)
				gReport.high_water = gReport.bytes_live;
		}
		report_end();
		heap_unlock();
		return out;
	}

	heap_block_t *b = free_list_first_fit(need);
	heap_pool_t  *home = NULL;

	if (b != NULL)
	{
		free_list_remove((heap_free_t *)b);
		block_split(b, need, block_flags(b) & (HEAP_PREV_FREE | HEAP_DIRTY));
		home = pool_of(region_of(b));
	}
	else
	{
		// Nothing on the list fits. Try each pool's virgin frontier, newest
		// first (the newest pool is the one most likely to have room).
		for (heap_region_t *r = gRegions; r != NULL && b == NULL; r = r->next)
			if (r->flags & HEAP_REGION_POOL)
			{
				b = pool_carve(pool_of(r), need);
				if (b != NULL)
					home = pool_of(r);
			}

		if (b == NULL)
		{
			heap_pool_t *p = pool_new(need);
			if (p == NULL)
			{
				report_end();
				heap_unlock();
				return NULL;    // out of memory: NULL, not a death
			}
			b = pool_carve(p, need);
			home = p;
		}
	}

	if (home != NULL)
		home->live++;

	uint64_t payload = block_size(b) - sizeof(heap_block_t);
	gReport.blocks_live++;
	gReport.bytes_live += payload;
	gReport.live_by_class[heap_class_of(payload)]++;
	if (gReport.bytes_live > gReport.high_water)
		gReport.high_water = gReport.bytes_live;

	out = block_payload(b);
	report_end();
	heap_unlock();
	return out;
}

// ── free ────────────────────────────────────────────────────────────────────

// Validate a caller's pointer down to the bone, then hand back its block.
// Order matters: containment FIRST (reads only our own ledger), then the
// canary (reads the header, now known to be inside a region we own).
static heap_block_t *block_from_user_pointer(void *ptr, heap_region_t **region_out)
{
	heap_region_t *r = region_of(ptr);

	if (r == NULL)
		heap_die("free of a pointer this heap never handed out", ptr, 0,
		         HEAP_EXIT_FREE_BAD);

	if (((uintptr_t)ptr & (HEAP_ALIGN - 1)) != 0)
		heap_die("free of a misaligned pointer", ptr, (uintptr_t)ptr & (HEAP_ALIGN - 1),
		         HEAP_EXIT_FREE_BAD);

	heap_block_t *b = payload_block(ptr);

	if (block_canary_ok(b))
	{
		if (!block_in_use(b))
			heap_die("free of a block that is already free (double free)",
			         ptr, b->canary, HEAP_EXIT_FREE_BAD);
	}
	else
	{
		// A freed block's canary uses the FREE seed. If THAT is what we are
		// looking at, this is a double free and deserves to be named as one
		// rather than reported as generic corruption.
		if (b->canary == heap_canary_for(b, block_size(b), false))
			heap_die("free of a block that is already free (double free)",
			         ptr, b->canary, HEAP_EXIT_FREE_BAD);

		heap_die("block header canary is wrong — the heap has been stomped",
		         ptr, b->canary, HEAP_EXIT_CANARIED);
	}

	*region_out = r;
	return b;
}

// Fill the body with poison, skipping the bytes the allocator itself is about
// to use (the two list links at the front, the footer at the back).
static void block_poison(heap_block_t *b)
{
	uint64_t size = block_size(b);
	char *start = (char *)b + sizeof(heap_free_t);
	char *end   = (char *)b + size - sizeof(uint64_t);

	if (end > start)
		os64_memset(start, HEAP_POISON, (size_t)(end - start));
}

void os64_free(void *ptr)
{
	if (ptr == NULL)
		return;      // as it has been since V7 — free(NULL) is a no-op

	if (!gInited)
		os64_heap_init();

	heap_lock();
	report_begin();
	gReport.calls_free++;

	if (gCheckAlways)
		heap_verify_locked();

	heap_region_t *r = NULL;
	heap_block_t *b = block_from_user_pointer(ptr, &r);

	uint64_t payload = block_size(b) - sizeof(heap_block_t);
	gReport.blocks_live--;
	gReport.bytes_live -= payload;
	gReport.live_by_class[heap_class_of(payload)]--;

	// The big case: one allocation, one region, one syscall. No coalescing
	// question exists, no fragmentation is possible, and the memory is back
	// with the kernel before free() returns.
	if (block_flags(b) & HEAP_DEDICATED)
	{
		region_give_back(r);
		report_end();
		heap_unlock();
		return;
	}

	heap_pool_t *p = pool_of(r);
	uint64_t flags = block_flags(b) & HEAP_PREV_FREE;

	block_set(b, block_size(b), flags | HEAP_DIRTY);
	block_poison(b);

	// ── Coalesce forward. The successor's header sits immediately above us,
	// so this costs one load: Knuth's tags at work.
	heap_block_t *next = block_next(b);
	if (block_size(next) != 0 && !block_in_use(next))
	{
		if (!block_canary_ok(next))
			heap_die("the block above this one has a bad canary", ptr,
			         next->canary, HEAP_EXIT_CANARIED);
		free_list_remove((heap_free_t *)next);
		block_set(b, block_size(b) + block_size(next), block_flags(b));
	}

	// ── Coalesce backward, through the predecessor's FOOTER — the other half
	// of the boundary tag, and the reason this merge is O(1) instead of a
	// search. os32 never had this; adjacent free chunks stayed split forever.
	if (block_flags(b) & HEAP_PREV_FREE)
	{
		uint64_t footer = block_prev_footer(b);
		heap_block_t *prev = (heap_block_t *)((char *)b - footer);

		if (footer == 0 || !block_canary_ok(prev) || block_in_use(prev))
			heap_die("the block below this one is not the free block its tag claims",
			         ptr, footer, HEAP_EXIT_CANARIED);

		free_list_remove((heap_free_t *)prev);
		block_set(prev, footer + block_size(b),
		          (block_flags(prev) & (HEAP_PREV_FREE | HEAP_DIRTY)) | HEAP_DIRTY);
		b = prev;
	}

	block_write_footer(b);

	// Tell whatever sits above us that its neighbour is now free — epilogue
	// and frontier header included, because that is precisely where the next
	// pool_carve reads the answer from.
	heap_block_t *above = block_next(b);
	above->size_flags |= HEAP_PREV_FREE;

	free_list_insert((heap_free_t *)b);
	p->live--;

	// ── The give-back. THIS is the payoff of the region ledger, and the thing
	// no brk-based malloc in history could do: when the last live block in a
	// region is freed, the whole region goes home to the kernel — even if it
	// sits in the MIDDLE of the heap, with other regions above and below it.
	// The primordial pool is exempt (HEAP_REGION_KEEP): a malloc/free loop
	// would otherwise map and unmap a megabyte per iteration, and since region
	// addresses are never reused, that churn spends address space for nothing.
	if (p->live == 0 && !(r->flags & HEAP_REGION_KEEP))
	{
		heap_block_t *first = pool_first_block(p);
		uint64_t span = p->frontier - sizeof(heap_pool_t);

		if (b == first && block_size(b) == span)
		{
			free_list_remove((heap_free_t *)b);
			region_give_back(r);
		}
		else
		{
			// Every block accounted for, yet the survivors did not merge into
			// one: that is an allocator bug, not a caller's crime. Say so, and
			// keep the region rather than unmapping something still in use.
			heap_complain("region empty but its free space did not merge", p,
			              block_size(b));
		}
	}

	report_end();
	heap_unlock();
}

// ── calloc ──────────────────────────────────────────────────────────────────

void *os64_calloc(size_t count, size_t size)
{
	if (count != 0 && size > (size_t)-1 / count)
		return NULL;    // the multiply would wrap: refuse, don't truncate

	size_t total = count * size;
	void *p = os64_malloc(total);
	if (p == NULL)
		return NULL;

	// The os64 dividend: memory carved from a region's virgin frontier has
	// never been written by anybody — and the kernel guarantees every page of
	// a fresh region arrives zeroed. So calloc's memset is skipped entirely
	// for first-touch allocations, which is most of what a starting program
	// does. Recycled memory carries its predecessor's bytes (and this heap's
	// 0xA5 poison), so it gets the memset it deserves.
	//
	// Read WITHOUT the lock, deliberately: a LIVE block's DIRTY bit never
	// changes (only its PREV_FREE bit moves, under the lock, when a neighbour
	// changes state), and an aligned 64-bit load is atomic on x86-64 — so the
	// one bit this decision rests on is stable even while the word around it
	// isn't.
	heap_block_t *b = payload_block(p);
	bool dirty = (block_flags(b) & HEAP_DIRTY) != 0;

	// THE CLAIM, AUDITED. Skipping the memset rests entirely on HEAP_DIRTY
	// telling the truth, and a flag that silently lies hands a caller its
	// predecessor's bytes — the worst failure this allocator can have, because
	// it looks like the CALLER's bug. So under HEAPCHECK the claim is checked
	// against the memory itself: if a block calling itself virgin holds so
	// much as one non-zero byte, say so and zero it anyway. The caller
	// committed no crime here (this would be the allocator's own bug), so
	// this complains rather than kills.
	if (gCheckAlways && !dirty)
	{
		for (size_t i = 0; i < total; i++)
			if (((const uint8_t *)p)[i] != 0)
			{
				heap_complain("a block claiming to be untouched was not zero", p, i);
				dirty = true;
				break;
			}
	}

	if (dirty)
		os64_memset(p, 0, total);

	heap_lock();
	report_begin();            // even a counter shuffle honours the shutter
	gReport.calls_calloc++;
	gReport.calls_malloc--;    // it was counted by the malloc above
	report_end();
	heap_unlock();
	return p;
}

// ── realloc ─────────────────────────────────────────────────────────────────

void *os64_realloc(void *ptr, size_t size)
{
	if (ptr == NULL)
		return os64_malloc(size);

	if (size == 0)
	{
		os64_free(ptr);
		return NULL;
	}

	if (!gInited)
		os64_heap_init();

	heap_lock();
	report_begin();
	gReport.calls_realloc++;

	heap_region_t *r = NULL;
	heap_block_t *b = block_from_user_pointer(ptr, &r);
	uint64_t old_size = block_size(b);
	uint64_t old_payload = old_size - sizeof(heap_block_t);
	uint64_t need = align_up((uint64_t)size + sizeof(heap_block_t), HEAP_ALIGN);

	if (need < HEAP_MIN_BLOCK)
		need = HEAP_MIN_BLOCK;

	// A dedicated block already owns a whole region; if the request still
	// fits inside it, there is nothing to do at all.
	if ((block_flags(b) & HEAP_DEDICATED) && size <= old_payload)
	{
		report_end();
		heap_unlock();
		return ptr;
	}

	if (!(block_flags(b) & HEAP_DEDICATED))
	{
		if (need <= old_size)
		{
			// Shrink in place, returning the tail to the heap if it is big
			// enough to be a block of its own.
			uint64_t rest = old_size - need;

			if (rest >= HEAP_MIN_BLOCK)
			{
				block_set(b, need, block_flags(b));
				heap_block_t *tail = block_next(b);
				block_set(tail, rest, HEAP_DIRTY);

				// THE TAIL'S NEIGHBOUR MAY ALREADY BE FREE — because b was
				// LIVE, so nothing ever guaranteed the block above it wasn't.
				// (Every OTHER maker of free blocks gets that guarantee
				// structurally: a split parent came off the free list, so its
				// successor was in use by the coalescing invariant; free()
				// merges forward itself.) Skip this merge and the heap holds
				// two adjacent free blocks — legal to every tag check, but
				// first-fit refuses requests their sum could serve, free()'s
				// single forward merge only ever repairs half of it, and when
				// such a pool empties, the give-back check finds the survivors
				// unmerged and complains "region empty but its free space did
				// not merge" while keeping a region that should have gone
				// home. Found 2026-08-16 by exactly that complaint, ~30 times
				// per mallochavoc run.
				heap_block_t *succ = block_next(tail);
				if (block_size(succ) != 0 && !block_in_use(succ))
				{
					if (!block_canary_ok(succ))
						heap_die("the block above this one has a bad canary",
						         ptr, succ->canary, HEAP_EXIT_CANARIED);
					free_list_remove((heap_free_t *)succ);
					block_set(tail, rest + block_size(succ), HEAP_DIRTY);
				}
				// The caller's abandoned bytes get the same paint free() would
				// have given them — a use-after-shrink reads 0xA5, not stale
				// data that works by luck.
				block_poison(tail);
				block_write_footer(tail);

				heap_block_t *after = block_next(tail);
				after->size_flags |= HEAP_PREV_FREE;

				free_list_insert((heap_free_t *)tail);

				gReport.bytes_live -= (old_payload - (need - sizeof(heap_block_t)));
				gReport.live_by_class[heap_class_of(old_payload)]--;
				gReport.live_by_class[heap_class_of(need - sizeof(heap_block_t))]++;
			}
			report_end();
			heap_unlock();
			return ptr;
		}

		// Grow in place if the block above is free and big enough — one load
		// to find out, because the tags put its header exactly there. This is
		// the case a brk heap can only serve at the very top of the heap.
		heap_block_t *next = block_next(b);
		if (block_size(next) != 0 && !block_in_use(next)
		    && old_size + block_size(next) >= need)
		{
			free_list_remove((heap_free_t *)next);
			// Absorbing a free neighbour means absorbing its poison and its
			// list links: whatever this block was, it is dirty now. (The free
			// list marks its own members, so this is belt to that braces —
			// but a block that swallows dirty memory and still calls itself
			// clean is a lie whether or not anything currently reads it.)
			block_set(b, old_size + block_size(next), block_flags(b) | HEAP_DIRTY);
			block_split(b, need, block_flags(b) & (HEAP_PREV_FREE | HEAP_DIRTY));

			uint64_t new_payload = block_size(b) - sizeof(heap_block_t);
			gReport.bytes_live += (new_payload - old_payload);
			gReport.live_by_class[heap_class_of(old_payload)]--;
			gReport.live_by_class[heap_class_of(new_payload)]++;
			if (gReport.bytes_live > gReport.high_water)
				gReport.high_water = gReport.bytes_live;

			report_end();
			heap_unlock();
			return ptr;
		}
	}

	report_end();
	heap_unlock();

	// The honest fallback: a new block, a copy, and the old one released.
	void *fresh = os64_malloc(size);
	if (fresh == NULL)
		return NULL;    // the original is untouched — the caller still owns it

	uint64_t copy = (old_payload < size) ? old_payload : (uint64_t)size;
	os64_memcpy(fresh, ptr, (size_t)copy);
	os64_free(ptr);

	heap_lock();
	report_begin();           // same shutter honesty as calloc's shuffle
	gReport.calls_malloc--;   // the malloc above belongs to this realloc
	gReport.calls_free--;
	report_end();
	heap_unlock();
	return fresh;
}
