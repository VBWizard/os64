#ifndef MEMORY_FRAMES_H
#define MEMORY_FRAMES_H

// The frame table: one 8-byte descriptor per 4 KiB physical frame, and the
// free-run lists that make allocating, freeing and "is this frame live?"
// cost the same however many frames the machine has or hands out.
// docs/design/pending/FRAMES.md is the design.
//
// No kernel headers and no locks, on purpose: the host harness
// (tools/test_frames_host.sh) compiles this with plain cc under ASan and
// UBSan, and the kernel's caller serialises every call under its frame lock.
// Nothing here allocates; the table is the caller's, sized once.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// A frame number, a length in frames, and the null list link share one
// 30-bit field width: 2^30 frames is 4 TiB of RAM.
#define FRAMES_NONE ((uint64_t)0x3FFFFFFF)

// Free runs of 1..FRAMES_EXACT_MAX frames each have a list of their own, so
// a repeating size reuses a run of its own length before anything longer
// is split. Longer runs are kept
// two-level segregated (TLSF: a power-of-two bin, then one of
// FRAMES_SL_COUNT sub-bins), so finding one that fits is two bit scans.
#define FRAMES_EXACT_MAX 64
#define FRAMES_SL_LOG2 3
#define FRAMES_SL_COUNT (1u << FRAMES_SL_LOG2)
#define FRAMES_FL_COUNT 30

typedef uint64_t frame_t;

typedef enum {
	FRAMES_RESERVED = 0,  // not usable memory, or not yet described; never handed out
	FRAMES_FREE = 1,
	FRAMES_RUN = 2,       // allocated by the frame table's own callers
	FRAMES_LEDGER = 3,    // lent to the small-object ledger, which owns its HHDM state
} frames_kind_t;

typedef enum {
	FRAMES_OK = 0,
	FRAMES_OUT_OF_RANGE,     // past the table, or a range that runs off it
	FRAMES_NOT_ALLOCATED,    // a free or reserved frame: a double or stray free
	FRAMES_NOT_A_START,      // inside an allocated run, not its first frame
	FRAMES_LEDGER_PINNED,    // a ledger run freed while live extents still overlap it
	FRAMES_NOT_LEDGER,       // a pin on a frame the ledger does not hold
	FRAMES_PIN_RANGE,        // a pin count would pass zero or its field width
	FRAMES_NOT_RESERVED,     // describing memory that is already described
	FRAMES_BAD_ARGUMENT,
} frames_status_t;

typedef struct {
	frame_t *table;
	uint64_t nframes;

	uint64_t exact_head[FRAMES_EXACT_MAX + 1];       // indexed by run length
	uint64_t exact_mask;                             // bit (length - 1): list non-empty
	uint64_t tlsf_head[FRAMES_FL_COUNT][FRAMES_SL_COUNT];
	uint32_t fl_mask;
	uint8_t sl_mask[FRAMES_FL_COUNT];

	// usable == free + run + ledger, always (frames_audit checks it).
	uint64_t usable_frames, free_frames, run_frames, ledger_frames;
	uint64_t free_runs;

	// Cost evidence. A refusal is an allocation that found nothing; a
	// fallback walk is the one list the TLSF round-up skipped, walked only
	// on the way to refusing, and `fallback_examined` counts its runs.
	uint64_t allocs, refusals, fallback_walks, fallback_examined;
} frames_t;

// Bytes of table needed to describe frames 0..nframes-1.
size_t frames_table_bytes(uint64_t nframes);

// Every frame starts RESERVED. The table memory is the caller's and is
// zeroed here.
frames_status_t frames_init(frames_t *f, frame_t *table, uint64_t nframes);

// Describe usable memory: [first, first+count) becomes free and joins any
// free neighbour. Frame 0 is never described as usable, so it can never be
// handed out. A range overlapping memory already described is refused.
frames_status_t frames_add_region(frames_t *f, uint64_t first, uint64_t count);

// Describe usable memory that is already in use (the table's own frames at
// boot): [first, first+count) becomes one allocated run of `kind`.
frames_status_t frames_add_allocated(frames_t *f, uint64_t first, uint64_t count,
                                     frames_kind_t kind);

// Allocate `count` contiguous frames as a run of `kind` (RUN or LEDGER).
// Returns the first frame, or FRAMES_NONE when no free run is long enough.
uint64_t frames_alloc(frames_t *f, uint64_t count, frames_kind_t kind);

// Free the run that STARTS at `first`. Anything else is refused and changes
// nothing: the caller panics, naming the address (frames_run_start helps).
// On success *count_out, if given, is the run's length.
frames_status_t frames_free(frames_t *f, uint64_t first, uint64_t *count_out);

// A LEDGER frame counts the live ledger extents overlapping it. The ledger
// adds after its carve has mapped the extent and subtracts before its free
// unmaps it, so a count above zero means mapped. All frames in the range
// change or none do.
frames_status_t frames_ledger_pin(frames_t *f, uint64_t first, uint64_t count, int delta);

// Is this frame live: allocated, and safe to read through its HHDM alias?
// A RUN frame is; a LEDGER frame is while its pin count is above zero.
bool frames_live(const frames_t *f, uint64_t frame);

frames_kind_t frames_kind(const frames_t *f, uint64_t frame);

// The first frame of the run holding `frame`, or FRAMES_NONE for a reserved
// or out-of-range frame. Walks back through the run, so it is for reports
// and panics, not hot paths.
uint64_t frames_run_start(const frames_t *f, uint64_t frame);

// The longest free run, exactly. Walks only the highest non-empty list.
uint64_t frames_largest_free(const frames_t *f);

// Check every descriptor, every list and every counter against each other.
// NULL when consistent; otherwise what is wrong, and *where the frame it was
// found at. A full walk of the table: for tests and debug boots.
const char *frames_audit(const frames_t *f, uint64_t *where);

#endif
