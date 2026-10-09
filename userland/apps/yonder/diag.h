#ifndef YONDER_DIAG_H
#define YONDER_DIAG_H

// ONE PAGE LOAD'S RECORD (YONDER_DIAGNOSTICS.md): what the page asked for
// that yonder does not have, what went wrong, and the plain facts of its
// load. The page owns it from the start of its navigation; the counts are
// kept whether or not yonder.conf asks for files, and the badge reads them.
//
// THE FILE IT RENDERS answers one grep. Line one is the address, line two
// the verdict: `verdict: clean`, or the tokens that have records, with
// their counts (`verdict: 7 MISSING, 2 FAILED`, `verdict: 2 FAILED`).
// Then the RECORD LINES, each beginning with its token:
//     MISSING <kind> <name> (<count>)
//     FAILED <what>: <message> (<count>)
// and the plain lines, `key: value`. A token appears on the verdict line
// only when it has records, and otherwise only at the start of its own
// record lines: DATA that spells one, in any line, has its second letter
// percent-encoded (`M%49SSING`, `F%41ILED`), which an address reads as the
// same address. So `grep -l MISSING` is exactly the files with a MISSING
// record. A byte that would break a line, and a backslash, are written
// `\xHH` and `\\`.
//
// Pure apart from yonder_diag_write: no clock, no window.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define YONDER_DIAG_MISSING "MISSING"
#define YONDER_DIAG_FAILED "FAILED"

// Distinct record lines kept, and plain lines: a page that asks for more
// has the rest counted on a plain line, not kept.
#define YONDER_DIAG_RECORDS_MAX 512
#define YONDER_DIAG_FACTS_MAX 512

typedef struct yonder_diag yonder_diag_t;

// A record for the load of `url`, numbered `seq`, begun at `began_us` on
// the caller's clock (kept for the caller: this file reads no clock).
// NULL on no memory.
yonder_diag_t *yonder_diag_new(const char *url, uint32_t seq, int64_t began_us);
void yonder_diag_free(yonder_diag_t *d);
uint32_t yonder_diag_seq(const yonder_diag_t *d);
int64_t yonder_diag_began(const yonder_diag_t *d);

// A MISSING line per kind and name: `times` more asks for it.
void yonder_diag_missing(yonder_diag_t *d, const char *kind, const char *name, uint32_t times);
// The same line, for a tally taken whole each time it is looked at (the
// elements in a tree, what one cascade passed over): it holds the most
// any look has seen, so looking twice does not count twice.
void yonder_diag_missing_seen(yonder_diag_t *d, const char *kind, const char *name, uint32_t count);
// A FAILED line per `what` and `message`, counted.
void yonder_diag_failed(yonder_diag_t *d, const char *what, const char *message);
// A plain line, `key: value`, kept where its key was first set and
// replaced by a later setting of the same key.
void yonder_diag_fact(yonder_diag_t *d, const char *key, const char *value);

// Distinct record lines of each token: the verdict's numbers, the badge's.
uint32_t yonder_diag_missing_lines(const yonder_diag_t *d);
uint32_t yonder_diag_failed_lines(const yonder_diag_t *d);

// The file's text, as snprintf: the length it needs, writing what fits.
size_t yonder_diag_render(const yonder_diag_t *d, char *out, size_t cap);
// The file's name, `<host>-<seq>.txt`: the host as an address names it,
// in the characters a file name keeps (`file` for a file's, `page` for
// none), the sequence zero-padded to four digits so `ls` lists in browse
// order. False when it does not fit `cap`.
bool yonder_diag_file_name(const yonder_diag_t *d, char *out, size_t cap);

// A picture's format libimage does not decode, named by its first bytes
// for a `MISSING image` line: "svg", "webp", "avif", "ico", "tiff", or
// "unknown".
const char *yonder_diag_image_format(const uint8_t *bytes, size_t len);

// Written into `dir` whole: rendered, put in a temporary file beside the
// real one and renamed over it, so a reader never meets half a file. 0, or
// negative when any step failed (the temporary is then removed).
int64_t yonder_diag_write(const yonder_diag_t *d, const char *dir);

// The sequence a run starts from in `dir`: one past the highest a file
// there already carries, so a run never writes over the census an earlier
// one kept. 1 for an empty or unreadable directory.
uint32_t yonder_diag_next_seq(const char *dir);

#endif
