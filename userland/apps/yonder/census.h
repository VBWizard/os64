#ifndef YONDER_CENSUS_H
#define YONDER_CENSUS_H
// THE MEMORY CENSUS (MEMORY_CENSUS.md): who holds a page's memory, written
// into its record. Pure: owners hand it numbers and it writes lines, so the
// host suite proves its formats and byte budgets without a page.
//
// A SHEET is one moment's answer: each owner's total, the process heap
// around them, and — in the census tier — libdom's kinds, the layout's and
// the cascade's parts, and the script heap's census. A TIMELINE keeps a
// bounded run of sheets' totals across a page's life.
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "os64/js_engine.h"

#define YONDER_CENSUS_OWNERS 16
#define YONDER_CENSUS_PARTS 24
// An owner whose parts the census tier names (`memory layout`, `memory
// cascade`), and the most parts one may name.
typedef enum { YONDER_CENSUS_OF_LAYOUT, YONDER_CENSUS_OF_CASCADE, YONDER_CENSUS_OF_N } yonder_census_of_t;
#define YONDER_CENSUS_FEW 4
#define YONDER_CENSUS_SAMPLES 32
// The record's byte budgets (MEMORY_CENSUS.md § 2): the summary line every
// record carries, and everything a censused page adds to it.
#define YONDER_CENSUS_SUMMARY_MAX 320
#define YONDER_CENSUS_DETAIL_MAX 6144
// The first interval between timeline samples; it doubles each time the
// timeline fills and is thinned.
#define YONDER_CENSUS_FIRST_INTERVAL_MS 10000u

// A part with a name: an owner, or one of an owner's kinds. `unit` names
// what `count` counts ("boxes", "nodes"); NULL for a count not printed.
typedef struct {
    const char *name;
    uint64_t bytes, count;
    const char *unit;
} yonder_census_entry_t;

typedef struct {
    yonder_census_entry_t owners[YONDER_CENSUS_OWNERS];
    int32_t nowners;
    // The whole process (os64_heap_report_copy): what it has mapped, and
    // what is live in it. `uncounted` is live less the owners' sum.
    bool heap_known;
    uint64_t heap_mapped, heap_live;
    // Census tier only.
    yonder_census_entry_t dom[YONDER_CENSUS_PARTS];
    int32_t ndom;
    yonder_census_entry_t parts[YONDER_CENSUS_OF_N][YONDER_CENSUS_FEW];
    int32_t nparts[YONDER_CENSUS_OF_N];
    bool script_known;
    os64_js_heap_census_t script;
    uint64_t cost_us;
} yonder_census_sheet_t;

// One sample: the time it was taken (ms since the page began) and the
// totals of the owners the timeline follows (kTimelineOwners).
#define YONDER_CENSUS_TIMELINE_OWNERS 6
typedef struct {
    uint64_t ms;
    uint64_t heap, owners[YONDER_CENSUS_TIMELINE_OWNERS];
} yonder_census_sample_t;

typedef struct {
    yonder_census_sample_t samples[YONDER_CENSUS_SAMPLES];
    int32_t n;
    uint32_t interval_ms;
    uint64_t cost_us_max;
} yonder_census_timeline_t;

// An owner's total, added to the sheet; past YONDER_CENSUS_OWNERS it is
// dropped, which a sheet built by yonder never reaches.
void yonder_census_owner(yonder_census_sheet_t *sheet, const char *name, uint64_t bytes, uint64_t count,
                         const char *unit);
// One of libdom's kinds, census tier.
void yonder_census_dom_kind(yonder_census_sheet_t *sheet, const char *name, uint64_t bytes, uint64_t blocks);
// One part of an owner the census tier breaks down; past
// YONDER_CENSUS_FEW it is dropped.
void yonder_census_part(yonder_census_sheet_t *sheet, yonder_census_of_t of, const char *name, uint64_t bytes);

// The summary (the record's `memory` line, its value only): the process
// heap first, as mapped, with how much of what is live in it no owner
// counts; then each owner, biggest first, as `name value [count unit]`
// separated by "; ". Always NUL-terminated and within both `cap` and
// YONDER_CENSUS_SUMMARY_MAX; owners that would not fit are dropped,
// smallest first, and the line ends ";..." to say so; a `cap` under 5 gets
// an empty line. Answers the length.
size_t yonder_census_summary(const yonder_census_sheet_t *sheet, char *out, size_t cap);
// The census tier's lines, each the value of its own record key: libdom by
// kind (`memory dom`), the script heap by what it holds (`memory script`).
// Zero length when the sheet has none.
size_t yonder_census_dom_line(const yonder_census_sheet_t *sheet, char *out, size_t cap);
size_t yonder_census_script_line(const yonder_census_sheet_t *sheet, char *out, size_t cap);
// An owner's parts (`memory layout`, `memory cascade`), biggest first, as
// `name value` separated by "; ", empty ones left out. Zero length when
// the sheet names none.
size_t yonder_census_parts_line(const yonder_census_sheet_t *sheet, yonder_census_of_t of, char *out, size_t cap);

// THE TIMELINE. Begins empty with the first interval.
void yonder_census_timeline_init(yonder_census_timeline_t *tl);
// Whether a sample is due at `ms`: at the first, and one interval after the
// latest.
bool yonder_census_timeline_due(const yonder_census_timeline_t *tl, uint64_t ms);
// Records the sheet's totals at `ms`. When the timeline is full, every
// other sample is dropped, the first kept, and the interval doubles, so it
// always spans the page's whole life evenly; the sample being added is
// always the newest there.
void yonder_census_timeline_add(yonder_census_timeline_t *tl, uint64_t ms, const yonder_census_sheet_t *sheet);
// `memory timeline`: the samples as `+<time> heap <v> <owner> <v> ...`
// separated by " | ", whole samples only, as many as `cap` holds; and
// `census`: on, how many samples at what spacing, and the most a census
// walk cost.
size_t yonder_census_timeline_line(const yonder_census_timeline_t *tl, char *out, size_t cap);
size_t yonder_census_state_line(const yonder_census_timeline_t *tl, char *out, size_t cap);

// A size as the census prints it: bytes below 1 KiB whole, and above, the
// largest unit it reaches, truncated to three significant figures — four
// from 1000 to 1023 of a unit (512, 12.0K, 1023K, 3.10M, 1.31G). Answers
// the length.
size_t yonder_census_size(uint64_t bytes, char *out, size_t cap);

// Whether `host` is censused by a `census` entry: the same host, a
// subdomain of it, or `*`. Case does not matter; a trailing dot neither.
bool yonder_census_matches(const char *entry, const char *host);

#endif
