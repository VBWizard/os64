#ifndef OS64_SLURP_H
#define OS64_SLURP_H

// os64/slurp.h — READ A WHOLE FILE, AND SAY HONESTLY WHAT HAPPENED.
//
// WHY THIS EXISTS. "Read a file up to a cap and tell me whether it was
// bigger" was hand-written FOUR times in this tree before anyone counted:
// the kernel's conf reader, libos64's os64_conf_read, os64_conf_write twenty
// lines away from it, and gui/desktop.c's read_whole_file. Codex #29 found
// the SAME bug in three of them across three separate rounds (rd1, rd3,
// rd12), and the fourth was never reviewed at all because it was named
// differently and nobody was looking there. Three rounds spent on one bug in
// n places. The diagnosis was not "delicate code" — it was DUPLICATED code.
//
// The in-tree cure for exactly this disease is klog_format.h, which is the
// ONLY copy of the log layout because that layout was once spelled four
// times across the ring boundary. This is the same move for whole-file
// reads.
//
// THE FOUR DISTINCTIONS, each of which was a shipped bug:
//
//   1. COMPLETE vs TRUNCATED. You cannot tell without reading ONE BYTE PAST
//      the cap. Without that probe a reader silently hands back a prefix and
//      calls it the file (rd1: os64_conf_write ate the tail; the kernel
//      returned a truncated prefix as success).
//
//   2. EXACTLY AT THE CAP IS NOT TRUNCATION. The probe must distinguish "the
//      file ends here" from "the file continues" (rd3: an 8191-byte file
//      reported truncated when it was whole).
//
//   3. END OF FILE IS NOT AN ERROR, AND AN ERROR IS NOT AN ENDING. Both
//      readers looped on `n <= 0`, folding "the file ended" together with
//      "the storage failed". rd12 was the worst finding of the whole review
//      because it DESTROYS DATA: the conf merge proceeded on a prefix and
//      the atomic rename published it over the user's real file — the
//      atomicity that exists to prevent a half-written file is what commits
//      the loss, because the corruption happened UPSTREAM of the part being
//      made safe.
//
//   4. A READ MAY BE SHORT WITHOUT BEING DONE. Loop until the file says
//      otherwise; never assume one read fills the buffer.
//
// ALL FOUR ARE INVISIBLE ON A SMALL, HEALTHY FILE. That is precisely why
// four copies survived: every one of them worked perfectly until the edge.
// If you are about to write a fifth read loop, use this instead — and if
// this one is wrong, it is wrong in one place.

#include <stddef.h>
#include <stdint.h>

typedef enum {
    OS64_SLURP_OK = 0,        // read whole, buffer is yours to free
    OS64_SLURP_NO_FILE,       // could not open it
    OS64_SLURP_TOO_BIG,       // larger than cap — NOTHING is returned
    OS64_SLURP_IO_ERROR,      // a read failed partway; contents unknown
    OS64_SLURP_NO_MEMORY      // could not allocate the buffer
} os64_slurp_status_t;

// Read the whole of `path`, up to `cap` bytes of CONTENT.
//
// On OS64_SLURP_OK: *out holds an os64_malloc'd buffer the caller frees, and
// *out_len its length. The buffer is always NUL-terminated one byte past the
// content, so text callers can treat it as a string without copying — the
// terminator is not counted in *out_len.
//
// On every other status: *out is NULL and *out_len is 0. Nothing partial is
// ever handed back, deliberately. A caller that receives a prefix cannot
// tell it from the whole file, and the conf arc proved where that ends.
//
// TOO_BIG AND IO_ERROR ARE DIFFERENT ANSWERS AND MUST STAY THAT WAY: TOO_BIG
// means "there is more here than you allowed" (the file is intact, your cap
// is the problem); IO_ERROR means "there WAS something and I could not read
// it" (saving over it would destroy it). NO_FILE means "nothing is there, a
// fresh write is safe". Collapsing any two of these is how rd12 happened.
os64_slurp_status_t os64_slurp(const char *path, size_t cap,
                               uint8_t **out, size_t *out_len);

// The status as a word, for messages. Never NULL.
const char *os64_slurp_status_name(os64_slurp_status_t status);

#endif // OS64_SLURP_H
