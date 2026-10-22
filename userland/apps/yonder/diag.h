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
// and the plain lines, `key: value`, and last the line `end`
// (YONDER_DIAG_END). A token appears on the verdict line
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
// The line a record carries when any of its lines was lost (no room, no
// memory, a value with no memory to keep): a replay and the corpus diff
// take such a record as one they cannot judge; the census tally counts the
// lines it kept, since what a page asked for is asked for however much of
// it was written down, and says how many records lost lines.
#define YONDER_DIAG_UNJUDGED "REPLAY unjudged: "
#define YONDER_DIAG_LINES_LOST YONDER_DIAG_UNJUDGED "lines lost"
// A claimed name's file before its record is written: its second line,
// followed by ` ` and the claim's nonce in sixteen hex digits, which every
// record the claim writes after carries too (`claim: <nonce>`), so a write
// replaces only a file that is still its own. A replay and the diff take
// it as a record they cannot judge; the tally takes it as damage (a yonder
// that stopped between claim and write) and names it.
#define YONDER_DIAG_UNWRITTEN "verdict: unwritten"
// The last line of every record yonder_diag_render writes. A reader
// requires it on any record that says `kept:` or `kept lines:` (a kept
// page's, from its claim on) or `REPLAY state:` (a replay's), so a record
// cut short is never read as a page; records written before it (the first
// census) have none of those, and no `end`.
#define YONDER_DIAG_END "end"

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
// window it was read in, whether it was kept whole, how many lines its
// index holds): it has room of its own, YONDER_DIAG_HEAD_FACTS, counted
// apart from the ordinary facts' YONDER_DIAG_FACTS_MAX, so a page with
// more plain lines than that never loses one of these, and neither room
// takes a place in the other.
#define YONDER_DIAG_HEAD_FACTS 24
void yonder_diag_head_fact(yonder_diag_t *d, const char *key, const char *value);
// The plain lines whose keys begin `prefix` let go, the rest kept in
// order — and so are those of them that met no room, so a set of lines
// rewritten whole, when there may be fewer now, is counted once.
void yonder_diag_forget(yonder_diag_t *d, const char *prefix);

// A line the caller could not even name for want of memory: the file
// says some lines are not on it, as it counts those that met no room.
void yonder_diag_short(yonder_diag_t *d);
// The record cannot be judged, for `why` (a static string: a setting
// changed during the load): it says so on a line YONDER_DIAG_UNJUDGED
// begins, which no replay or diff judges. The first reason stands.
void yonder_diag_unjudged(yonder_diag_t *d, const char *why);
// A plain line's value as it stands; NULL when there is none, or its
// value had no memory to keep.
const char *yonder_diag_fact_value(const yonder_diag_t *d, const char *key);

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
// (os64_open's "x", holding YONDER_DIAG_UNWRITTEN and the claim's nonce
// until the record is written) and no `<name>.html` or `<name>.d` is there, kept or not; with
// `keep`, `<name>.d/` is then made. The number it ends on is
// yonder_diag_seq's. A fresh name whose folder cannot be made is claimed
// and nothing of its page is kept. A record that finds no fresh name in
// YONDER_DIAG_CLAIM_TRIES tries has NO CLAIM, keeps the number it began
// with, and nothing of it is written. A record claims once: a second call
// answers 0 and changes nothing.
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
// never meets half a file. The claimed file must still be this claim's
// own, carrying its nonce. 0, or negative with no claim, with the claimed
// file gone or another's, or when any step failed (the temporary is then
// removed).
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
// over one there already, which is a failed step, and the record gains
// `kept: whole`, the fact a replay requires; a later step that fails
// replaces it with `kept: not whole`.
int64_t yonder_diag_keep_bytes_end(const yonder_diag_claim_t *c);

// What a kept sub-resource is: its file's extension, and the word the
// index names it by.
typedef enum { YONDER_DIAG_KEPT_SCRIPT, YONDER_DIAG_KEPT_SHEET } yonder_diag_kept_t;

// One sub-resource as the window received it: when it came, written whole
// as the next `<name>.d/<n>.js` or `.css`; when its fetch failed (`bytes`
// NULL), no file, since a replay must fail it as the page saw it fail. The
// index is written again whole with its line, and the record's `kept lines`
// fact says how many it holds. `job` is the job's own identity (a
// script's token, a sheet's place in its page's table), so a replay gives
// two jobs of one kind and address each the outcome its own fetch had;
// `url` is the address its job carried, `final` the one it came from after
// redirects, `label` the charset its response named (a sheet's: a replay
// parses the bytes again under it; "" for none). Lines are numbered from 1
// in the order they are kept, each once.
//
// THE INDEX GROWS with what pages fetch (REPLAY.md § 4):
// its first line is `# yonder kept index <version>`, more lines beginning
// `#` say what the rest hold, and every other line is
//     <n> TAB <kind> TAB what that kind carries
// so a kind a later slice keeps (a `fetch`, a `storage` image) is one more
// line shape and leaves these as they are; the version moves only when a
// shape that exists changes. A replay reads every line as a kind it
// knows, and takes a line it does not as damage, so the slice that keeps
// a new kind teaches the replay it in the same change. A script's and a
// sheet's line carries
//     <url> TAB came|failed TAB <final> TAB <label> TAB <job>
// with a byte below 0x20, 0x7F and a backslash in a field written `\xHH`
// and `\\`, as a page file writes them. Version 2 added `<job>`; a replay
// reads a kept page of version 1 by REPLAY.md § 3, matching its jobs by
// kind and address in the order kept.
#define YONDER_DIAG_INDEX_VERSION 2
int64_t yonder_diag_keep_resource(const yonder_diag_claim_t *c, yonder_diag_kept_t kind, uint32_t job,
                                  const char *url, const char *final, const char *label,
                                  const void *bytes, size_t n);

#endif
