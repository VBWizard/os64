#ifndef OS64_ABI_HEAP_H
#define OS64_ABI_HEAP_H

#include <stdint.h>

// The heap report — one struct, published once by libos64's malloc and
// rendered by the kernel as /proc/<pid>/heap.
//
// THE PROBLEM THIS SOLVES. A heap's interesting facts (how many blocks are
// live, how badly it is fragmented, whether it has ever given a region back)
// are known ONLY to the allocator, which lives at ring 3 — while every other
// virtual file in os64 is written by the kernel, which cannot see inside a
// program's malloc. Three ways out were weighed (2026-08-15, with Chris):
//
//   (a) the program publishes a POINTER to this struct once, and the kernel
//       renders the file — chosen;
//   (b) the kernel sniffs a magic word at the head of the task's first
//       anonymous region — no new syscall, but numbers with no provenance
//       ("jenky", ruled from the floor);
//   (c) every program reports its own heap — rejected: procfs owns every
//       other file's format and should own this one too.
//
// (a) keeps the pen in the kernel's hand. The APPLICATION writes nothing:
// libos64's own init calls os64_heap_init(), which fills this struct and
// hands its address to SYSCALL_HEAP_REPORT. A program that never allocates
// a byte still has an honest, catable /proc/<pid>/heap from its first
// instruction.
//
// WHAT THIS IS NOT: a frozen ABI in the syscall sense. Same doctrine as the
// rest of /proc (PROC.md): the file is a REPORT, not a struct — fields may
// be appended, and `version` tells the kernel which ones it may believe.
// The struct itself IS shared memory between the two rings, so it changes
// only with a version bump, and the kernel validates before it believes.

#define OS64_HEAP_REPORT_MAGIC    0x4845415052505421ULL   // "HEAPRPT!"
#define OS64_HEAP_REPORT_VERSION  1

// Live-block histogram: bucket i counts blocks whose REQUESTED size falls in
// [2^(i+4), 2^(i+5)) — 16..31, 32..63, ... The last bucket is everything
// bigger and says so. A first-fit heap has no size classes (Chris's 7/19
// ruling — bins were "OVERHEAD for this OS"), but a histogram costs one
// shift per call and answers the question bins would have answered:
// "what shape is this program's appetite?"
#define OS64_HEAP_CLASSES         16
#define OS64_HEAP_CLASS_MIN_SHIFT 4    // bucket 0 starts at 16 bytes

typedef struct
{
	uint64_t magic;              // OS64_HEAP_REPORT_MAGIC, or the kernel says nothing
	uint32_t version;            // OS64_HEAP_REPORT_VERSION
	uint32_t reserved;           // zero — keeps the 64-bit fields 8-aligned

	// Bumped to ODD before each mutation and back to EVEN after, so a reader
	// that catches the struct mid-update can SAY SO instead of publishing
	// numbers that never coexisted. The report is a photograph of a moving
	// thing; this is the shutter speed, admitted out loud.
	uint64_t generation;

	uint64_t regions;            // live regions (pools + dedicated)
	uint64_t region_pools;       // regions malloc carves blocks out of
	uint64_t region_dedicated;   // regions holding exactly one big allocation
	// THE AUDIT IDENTITY, and it is not a courtesy:
	//
	//     bytes_mapped == bytes_live + bytes_free + bytes_overhead + bytes_virgin
	//
	// Every byte the kernel gave this heap is in exactly one of four states —
	// handed to the program, sitting in a free block, spent on the
	// allocator's own headers, or never carved at all. The identity is
	// checked by whoever renders this (procfs prints `audit ok` or `audit
	// BROKEN`), so a bookkeeping slip in malloc announces itself in a file
	// instead of hiding until the numbers look strange. Same doctrine as
	// os64/memory.h's `free + used == usable`.
	uint64_t bytes_mapped;       // total VA the heap holds from the kernel
	uint64_t bytes_live;         // payload bytes currently handed to the program
	uint64_t bytes_free;         // payload bytes sitting in free blocks
	uint64_t bytes_overhead;     // headers, region structs, epilogues
	uint64_t bytes_virgin;       // mapped but never carved — costs no RAM at all
	                             // (demand paging: untouched pages don't exist)
	uint64_t blocks_live;
	uint64_t blocks_free;
	uint64_t largest_free;       // biggest single free block (the fragmentation tell)
	uint64_t high_water;         // largest bytes_live ever reached

	uint64_t calls_malloc;
	uint64_t calls_free;
	uint64_t calls_calloc;
	uint64_t calls_realloc;
	uint64_t calls_map;          // regions taken from the kernel, ever
	uint64_t calls_unmap;        // regions GIVEN BACK, ever — brk could not

	uint64_t live_by_class[OS64_HEAP_CLASSES];
} os64_heap_report_t;

#endif // OS64_ABI_HEAP_H
