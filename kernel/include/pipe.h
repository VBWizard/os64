#ifndef PIPE_H
#define PIPE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "spinlock.h"
#include "thread.h"

// pipe.h — the os64 pipe: a bounded, kernel-owned byte stream between tasks.
//
// WHERE THE BYTES LIVE
// A kmalloc'd ring buffer owned by the pipe_t, with head/tail/count/refcounts
// in this kernel-owned struct. The writer copies IN, the reader copies OUT —
// two copies, and deliberately so. The tempting alternative (map one page into
// both tasks and let them share the ring) puts the head/tail pointers in memory
// USERLAND CAN WRITE, and a buggy program could then corrupt or hang the
// process on the other end. The kernel must be the arbiter of the buffer, so
// the buffer lives where only the kernel can reach it. (Same call GRAPHICS.md
// makes for the GUI: syscalls carry handles, never pixels.)
//
// The copies are free of any page-table dance because kmalloc'd memory is
// HHDM-mapped in the UPPER HALF, which is shared into every task's PML4 — so a
// read/write syscall running on the caller's CR3 can see the user's buffer
// (lower half, that task's mapping) and the pipe ring (upper half, shared) at
// the same instant. This is the standard arrangement (Linux's direct map does
// exactly this, and it is what makes copy_from_user possible at all).
// NOTE the exception that bit spawn: LOWER-half identity-mapped regions (the
// NVMe/AHCI DMA queues) are NOT in a user CR3. Kernel heap: fine. Device DMA:
// needs call_in_kernel_context. That one sentence explains both.
//
// WHY BOUNDED — THE LIMIT IS THE FEATURE
// An unbounded pipe is a memory leak with a friendly API: `cat 4gb | grep foo`
// would let the producer run flat out and allocate until the kernel dies.
// Bound it, and a writer that fills the buffer BLOCKS — which throttles the
// producer to exactly the consumer's speed, asleep and costing zero CPU until
// the consumer drains. That is BACKPRESSURE, and it is the entire reason a
// pipeline runs in constant memory no matter how big the data is. Nobody wrote
// a line of flow control; the bound IS the flow control.
#define PIPE_CAPACITY (64 * 1024)
// 64KB (not in CONFIG.h on purpose — this is a pipe-internal constant, not a
// system dial). Too small (one page) and the writer blocks constantly: you
// ping-pong through the scheduler every 4KB and the pipeline crawls. Too big
// and you waste memory per pipe and add latency (bytes sitting instead of
// moving). 64KB is the well-tested sweet spot.

typedef struct pipe
{
	spinlock_t lock;

	uint8_t *buffer;          // kmalloc'd ring, PIPE_CAPACITY bytes
	size_t head;              // next write offset
	size_t tail;              // next read offset
	size_t count;             // bytes currently in the ring

	// TWO refcounts, not one — this is the crux of the whole design:
	//   reader sees EOF   when writers == 0   (EOF is the ABSENCE OF WRITERS,
	//                                          not a byte in the stream)
	//   writer gets EPIPE when readers == 0   (writing into the void)
	//   the pipe is freed when both hit 0.
	// The classic hang — `cat foo | grep x` never finishing — is a shell that
	// forgot to close ITS copy of the write end: writers never reaches 0, so
	// the reader waits forever for an EOF that can never come.
	uint32_t readers;
	uint32_t writers;

	// Parked threads (NULL = nobody waiting). One slot each: a second waiter
	// simply falls back on its SIGSLEEP backstop and retries — correct, just
	// slower. A proper wait list is an easy upgrade if it ever matters.
	thread_t * volatile readWaiter;    // blocked in pipe_read, wants data (or EOF)
	thread_t * volatile writeWaiter;   // blocked in pipe_write, wants space (or EPIPE)

	struct pipe *next;        // kPipeList, walked by pipe_wake_if_ready()
} pipe_t;

// Result codes (negative = error; >= 0 is a byte count).
#define PIPE_ERR_CLOSED   (-1)    // all readers gone: writing into the void (EPIPE)
#define PIPE_ERR_NOMEM    (-2)

// Lifecycle. pipe_create() returns a pipe with readers == writers == 1 (the
// creator holds both ends); handing an end to a child is a pipe_ref_*, and
// giving one up is a pipe_close_*.
pipe_t *pipe_create(void);
void pipe_ref_read_end(pipe_t *p);
void pipe_ref_write_end(pipe_t *p);
void pipe_close_read_end(pipe_t *p);
void pipe_close_write_end(pipe_t *p);

// Reads return SHORT: whatever is available, immediately — a reader asking for
// 4096 when 10 bytes are there gets 10. (Waiting to fill the caller's buffer
// deadlocks every interactive pipeline.) Blocks only when the pipe is EMPTY and
// a writer still exists. Returns 0 = EOF once the last writer is gone.
long pipe_read(pipe_t *p, char *buf, size_t len);

// Writes land WHOLE: a write of <= PIPE_CAPACITY is atomic — nothing is copied
// until there is room for all of it, so two writers can never interleave. (Our
// rule, not Unix's PIPE_BUF hair-splitting: "your write lands whole, or it
// waits.") Writes LARGER than the capacity must chunk — they physically cannot
// fit — and that is the only case where interleaving is possible.
// Blocks while there is not enough room. Returns PIPE_ERR_CLOSED if all readers
// are gone (the caller raises SIGPIPE, whose default action terminates — that
// is what makes `yes | head` terminate instead of spinning forever).
long pipe_write(pipe_t *p, const char *buf, size_t len);

// Level-triggered wake sweep, called once per scheduler pass from
// processSignals — the same discipline as console_wake_if_ready(). The fast
// path (a reader/writer waking its counterpart directly) covers the common
// case in microseconds; this sweep catches the one race it cannot: a waiter
// that had registered but had not yet parked when the wake fired. Level-
// triggered means no wake is ever lost — worst case it lands on the next pass
// (~10ms), and the SIGSLEEP backstop (~1s) is the final net beneath that.
void pipe_wake_if_ready(void);

// Dump every live pipe (buffered bytes, both refcounts, who is parked). Call it
// from GDB when a pipeline wedges: (gdb) call pipe_dump_all()
void pipe_dump_all(void);

#endif // PIPE_H
