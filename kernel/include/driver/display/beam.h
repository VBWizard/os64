// beam.h — where the display's sweep is (FRAME_CLOCK.md § 1).
//
// THE SEAM answers what the frame clock asks: is there a beam and what is it
// called; the frame (its lines, the blank's first and last line, and the
// period); and the line being read now; and, for /sys/gui, why there is none
// and how many reads failed. A driver finds a display core it can read and
// says how to read its line counter; the seam measures the period, keeps the
// facts, and withdraws the beam for the rest of the boot after
// BEAM_FAILS_IN_ROW reads in a row that are not lines of the frame. A
// counter that stops at a line of the frame no read can show: while the
// frame clock runs, its aim, which waits on the counter, sees it stop (in
// the shown rows or in the blank) and withdraws the beam (beam_withdraw);
// with the clock off nothing watches for it. NO DRIVER WRITES
// TO THE GRAPHICS CHIP: each maps the registers it reads read-only, so a
// stray store is a page fault, not a write. A driver's mapping stays when
// the measuring fails.
//
// No beam is an ordinary answer (QEMU, another chip, a counter that reads
// all ones), and `beam_why` says which.

#ifndef BEAM_H
#define BEAM_H

#include <stdbool.h>
#include <stdint.h>

#include "gui/frame_clock.h"

// Looks once, at boot, before the compositor's task exists: the drivers in
// turn, then the period measured over a few frames with interrupts on
// (bounded by FC_MEASURE_BOUND_US an attempt). An attempt spoiled by a gap
// between readings, the busy machine's doing, is made again, up to
// BEAM_MEASURE_ATTEMPTS. Called only on a boot with the desktop.
#define BEAM_MEASURE_ATTEMPTS 4
void beam_init(void);

// There is a beam now: found, measured, and not withdrawn.
bool beam_present(void);

// The driver's name and where it found the core ("64:00.0 1002:1681
// OTG0"), or NULL when no driver ever found one.
const char *beam_driver(void);
const char *beam_where(void);

// Why there is no beam, or why it was withdrawn; NULL while there is one.
const char *beam_why(void);

// The frame's facts as found and measured, once they have been measured and
// make sense (they stay readable after a withdrawal, for /sys/gui).
bool beam_frame(fc_frame_t *out);

// The line being read now. False when the read failed: all ones (nothing
// answered) or a line beyond the frame. BEAM_FAILS_IN_ROW failures in a row
// withdraw the beam for the boot. ONE READER: the compositor thread, which
// owns the failure count; others read the facts above.
#define BEAM_FAILS_IN_ROW 8
bool beam_line(uint32_t *line);

// Reads that failed, since boot.
uint64_t beam_read_failures(void);

// Withdraw the beam for the rest of the boot, for a reason the reader saw
// that no single read shows: the frame clock's aim finding the counter
// stopped at a line of the frame. The one reader's call (beam_line's).
void beam_withdraw(const char *why);

// ── For drivers ─────────────────────────────────────────────────────────────
// A driver's probe either finds a core it can read, filling the frame's
// lines and blank and a reader for its line counter, or returns why not.
// A reader returns the counter masked to its line field, or 0xFFFFFFFF when
// the register read all ones.
typedef struct {
	uint32_t lines, blank_start, blank_end;
	uint32_t (*read_line)(void);
	char where[48];
} beam_found_t;

typedef struct {
	const char *name;
	// NULL when found; otherwise why not, or BEAM_NOT_MINE when the machine
	// has nothing this driver knows, so the next driver is asked.
	const char *(*probe)(beam_found_t *out);
} beam_driver_t;

#define BEAM_NOT_MINE ((const char *)1)

extern const beam_driver_t kBeamAmdDcn;

#endif
