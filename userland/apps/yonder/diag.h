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
// record. A `%` of the data's own that a reader would take for part of a
// spelling (one beginning `%25`, or standing where `M%49SSING`'s or
// `F%41ILED`'s does) is written `%25`, so the spelling is injective and a
// reader gets back the data's bytes exactly by decoding `M%49SSING`,
// `F%41ILED` and `%25` in one pass from the left; every other `%` is
// written as it is. A byte that would break a line, and a backslash, are
// written `\xHH` and `\\`.
//
// THE PAGE KEPT beside its file, when yonder.conf says `keep_pages = on`
// (MODERN_WEB.md rule 3: a kept page can be fed to the headless binding
// again, tools/test_yonder_replay_host.sh): `<name>.html`, the page's bytes
// as its parser was fed them, and a directory `<name>.d/` holding each
// script and sheet the page fetched as the window received it, `<n>.js` and
// `<n>.css` in the order they came, and an `index` naming them. `<name>` is
// the page file's name without its `.txt`.
//
// Pure apart from the writers (the claim, yonder_diag_write and the
// keepers) and yonder_diag_next_seq's reading of a directory: no
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
// The same line, for a fact a replay rebuilds the page from (the head, the
// width it was laid out at, whether it was kept whole): it has room of its
// own past YONDER_DIAG_FACTS_MAX, so a page with more plain lines than that
// never loses one of these, and a replay never rebuilds a head from
// defaults it did not know it was taking.
#define YONDER_DIAG_HEAD_FACTS 16
void yonder_diag_head_fact(yonder_diag_t *d, const char *key, const char *value);
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

// The sequence a run starts from in `dir`: one past the highest a file
// there already carries, so a run never writes over the census an earlier
// one kept. 1 for an empty or unreadable directory.
uint32_t yonder_diag_next_seq(const char *dir);

// THE CLAIM: the proof that a name in a directory is this record's own.
// Only a successful exclusive claim makes one, and the record's file, its
// page's bytes and its folder are written only through it, so nothing is
// ever written under a name the record did not claim.
typedef struct yonder_diag_claim yonder_diag_claim_t;

// THE RECORD'S NAME, CLAIMED in `dir` before anything is written under it,
// so two yonders writing one directory, or one whose sequence was counted
// before a file went, never write over each other's records or pair one
// record with another's kept files. The record's number moves up from
// where it began until `<name>.txt` can be made where nothing is
// (os64_open's "x"; the record as it stands goes in it), and, with `keep`,
// until `<name>.d/` can be made fresh and no `<name>.html` is there; the
// number it ends on is yonder_diag_seq's. A record that finds no such name
// in YONDER_DIAG_CLAIM_TRIES tries takes the first number whose file it
// can make, and nothing of its page is kept; one that can make none in as
// many tries has NO CLAIM, keeps the number it began with, and nothing of
// it is written. A record claims once: a second call answers 0 and changes
// nothing.
// Answers 0; -1 when `keep` asked for kept files and the name holds none
// (the keepers answer as for a failed step); YONDER_DIAG_UNCLAIMED with
// no claim.
#define YONDER_DIAG_CLAIM_TRIES 64
#define YONDER_DIAG_UNCLAIMED (-2)
int64_t yonder_diag_claim(yonder_diag_t *d, const char *dir, bool keep);
// The record's claim; NULL when it has none, which every writer below
// takes as nothing to write.
const yonder_diag_claim_t *yonder_diag_claimed(const yonder_diag_t *d);

// The claim's record written under its name whole: rendered, put in a
// temporary file beside the real one and renamed over it, so a reader
// never meets half a file. 0, or negative with no claim or when any step
// failed (the temporary is then removed).
int64_t yonder_diag_write(const yonder_diag_claim_t *c);

// ── Keeping the page (keep_pages) ──
//
// Each answers 0, or negative when nothing more of this page will be kept:
// the step that failed, and every call after it, since a kept page missing
// a piece would replay as a different page. The record then carries the
// plain line `kept: not whole: ...`, which a replay reads. No claim keeps
// nothing and answers 0, and a claim made without `keep` has no folder,
// so its keepers answer as for a failed step.

// The next `n` bytes the page's parser was fed, appended to a temporary
// file beside `<name>.html`, which is begun at the first call.
int64_t yonder_diag_keep_bytes(const yonder_diag_claim_t *c, const void *bytes, size_t n);
// The parser has had all the bytes it will be fed: the temporary file is
// renamed to `<name>.html` (an empty one for a page of no bytes), never
// over one there already, which is a failed step.
int64_t yonder_diag_keep_bytes_end(const yonder_diag_claim_t *c);

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
int64_t yonder_diag_keep_resource(const yonder_diag_claim_t *c, yonder_diag_kept_t kind,
                                  const char *url, const char *final, const char *label,
                                  const void *bytes, size_t n);

#endif
