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
// THE PAGE KEPT beside its file, when yonder.conf says `keep_pages = on`
// (MODERN_WEB.md rule 3: a kept page can be fed to the headless binding
// again, tools/test_yonder_replay_host.sh): `<name>.html`, the page's bytes
// as its parser was fed them, and a directory `<name>.d/` holding each
// script and sheet the page fetched as the window received it, `<n>.js` and
// `<n>.css` in the order they came, and an `index` naming them. `<name>` is
// the page file's name without its `.txt`.
//
// Pure apart from the writers (yonder_diag_write and the keepers): no
// clock, no window.

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
// Lets the record go. A page's bytes still being kept (keep_bytes without
// keep_bytes_end) go with it: their temporary file is removed, since a page
// that never had all its bytes cannot be fed again as it was.
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
// The same line for a condition that holds rather than an event that
// happened: once, however often it is looked at.
void yonder_diag_failed_seen(yonder_diag_t *d, const char *what, const char *message);
// A plain line, `key: value`, kept where its key was first set and
// replaced by a later setting of the same key.
void yonder_diag_fact(yonder_diag_t *d, const char *key, const char *value);
// The plain lines whose keys begin `prefix` let go, the rest kept in
// order — and so are those of them that met no room, so a set of lines
// rewritten whole, when there may be fewer now, is counted once.
void yonder_diag_forget(yonder_diag_t *d, const char *prefix);

// A line the caller could not even name for want of memory: the file
// says some lines are not on it, as it counts those that met no room.
void yonder_diag_short(yonder_diag_t *d);

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

// ── Keeping the page (keep_pages) ──
//
// Each answers 0, or negative when nothing more of this page will be kept:
// the step that failed, and every call after it, since a kept page missing
// a piece would replay as a different page. The record then carries the
// plain line `kept: not whole: ...`, which a replay reads. A NULL record
// keeps nothing and answers 0.

// The next `n` bytes the page's parser was fed, appended to a temporary
// file beside `<name>.html`, which is begun at the first call.
int64_t yonder_diag_keep_bytes(yonder_diag_t *d, const char *dir, const void *bytes, size_t n);
// The parser has had all the bytes it will be fed: the temporary file is
// renamed over `<name>.html` (an empty one for a page of no bytes).
int64_t yonder_diag_keep_bytes_end(yonder_diag_t *d, const char *dir);

// What a kept sub-resource is: its file's extension, and the word the
// index names it by.
typedef enum { YONDER_DIAG_KEPT_SCRIPT, YONDER_DIAG_KEPT_SHEET } yonder_diag_kept_t;

// One sub-resource as the window received it: when it came, written whole
// as the next `<name>.d/<n>.js` or `.css`; when its fetch failed (`bytes`
// NULL), no file, since a replay must fail it as the page saw it fail. The
// index is written again whole with its line. `url` is the address its job
// carried, `final` the one it came from after redirects, `label` the
// charset its response named (a sheet's: a replay parses the bytes again
// under it; "" for none).
//
// THE INDEX GROWS with what pages fetch (MODERN_WEB_BRIEFS.md § Slice 0b):
// its first line is `# yonder kept index <version>`, more lines beginning
// `#` say what the rest hold, and every other line is
//     <n> TAB <kind> TAB what that kind carries
// so a kind a later slice keeps (a `fetch`, a `storage` image) is one more
// line shape and leaves these as they are; the version moves only when a
// shape that exists changes, and a reader passes over a kind it does not
// know. A script's and a sheet's line carries
//     <url> TAB came|failed TAB <final> TAB <label>
// with a byte below 0x20, 0x7F and a backslash in a field written `\xHH`
// and `\\`, as a page file writes them.
#define YONDER_DIAG_INDEX_VERSION 1
int64_t yonder_diag_keep_resource(yonder_diag_t *d, const char *dir, yonder_diag_kept_t kind,
                                  const char *url, const char *final, const char *label,
                                  const void *bytes, size_t n);

#endif
