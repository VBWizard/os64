#ifndef OS64_MEDIA_TRACK_H
#define OS64_MEDIA_TRACK_H

// os64/media_track.h — a SourceBuffer's coded frames: its track buffers,
// the spec's coded frame processing over them, and what a player asks of
// them (libmedia.so; MEDIA_SOURCE.md § 4 is the contract).
//
// os64_media_buffer_t is the SourceBuffer's half: its tracks (one picture,
// one sound at most, as the parser gives them), its mode, timestampOffset,
// append window, group start and end, and each track's quota. The parser
// (os64/media_fragment.h) hands it init segments and coded frames; a page
// sets its offset, window and mode, removes ranges and asks for room
// before an append; a player reads each track through a cursor.
//
// PURE, AND NOT LOCKED: no os64 headers, no clock; it allocates only
// through the caller's memory functions and calls nothing but those and
// the slabs' own release. The caller's ONE LOCK covers every call,
// including the player's. tools/test_media_track_host.sh drives it beside
// a model (a flat list of frames written from the spec's text, no ranges),
// tools/test_media_track_mutants.py breaks each rule, and
// tools/fuzz_media_track_host.sh drives it with operation streams made of
// fuzz bytes.
//
// TIMES ARE NANOSECONDS, int64, everywhere in the buffer: the SourceBuffer's
// offset, window, group start and end, and every frame's times. A frame
// enters in its track's ticks and is converted ONCE, to the nearest
// nanosecond, from its own exact tick values (os64_media_ticks_to_ns); no
// time in the buffer is ever a sum of rounded values, so nothing drifts:
// in sequence mode a group's first frame lands exactly on the group start,
// and a frame is never more than half a nanosecond from its ticks. Not
// ticks inside a track: a quality switch may change a track's timescale
// (a 15360 rendition beside a 90000 one), so a track has no single tick;
// and a nanosecond is finer than any timescale the buffer takes (up to
// 1,000,000,000; a finer one is refused, TIMESCALE), so the round trip
// from a frame's ticks to nanoseconds and back is exact. Seconds, a page's
// doubles, meet nanoseconds only in os64_media_seconds_to_ns and
// os64_media_ns_to_seconds, nearest both ways; the round trip from
// nanoseconds to seconds and back is exact below 2^51 ns (26 days).
//
// MEMORY. Frame records live in arrays a range grows by doubling; a
// generation's configurations are copied once, at its init segment;
// nothing is allocated a frame. The buffer keeps one hold on a frame's
// slab (os64/media_fragment.h § A SLAB'S LIFE) for every frame it keeps,
// and a player keeps one more for every frame it has taken and not yet
// given back (os64_media_track_frame_done).
//
// NOTHING IS FREED BY THE BUFFER ITSELF, IN ITS OWN TIME. Every release it
// makes happens inside the call that dropped the last reference, and so
// under the caller's lock: a slab's last drop calls the slab's own release
// (the parser's memory functions), the buffer's arrays and configuration
// blocks go to this buffer's release. Both may therefore be "queue it":
// MS.2's glue hands them to a queue emptied after the lock is let go,
// because on os64 freeing a slab is an unmap under the heap's lock and an
// eviction drops many. The buffer never touches a block after releasing it
// (the host test frees at once under ASan to prove it), and since those
// functions see every slab made and freed, the caller can count the slab
// bytes really held beside the frame bytes the buffer counts.
//
// THE PLAYER'S CURSOR reads a track in decode order. Overlaps, removes and
// evictions move frames under it; the cursor carries the serial of the
// frame it returned last and of the frame that followed it, and after any
// such change it is re-found by them. RESYNC ONLY WHEN IT MUST: if either
// of those two frames is gone, the answer is OS64_MEDIA_TRACK_RESYNC and
// the player starts again from the random access point at or before its
// position (frames before the position decoded, their output discarded).
// Anything else, a quality switch appended ahead of the decoder above all,
// reads on: through the old frames up to the new random access point, then
// into the new ones. Other browsers keep the tail of the group being read
// until the new data's next random access point, so even the cursor's own
// next frame going does not stall them; v1 does not (DEBTS).

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "os64/media_fragment.h"

#pragma GCC visibility push(default)

#define OS64_MEDIA_NO_TIME INT64_MIN                    // unset, or no play position
#define OS64_MEDIA_TIME_MAX INT64_MAX                   // appendWindowEnd's +Infinity
#define OS64_MEDIA_MAX_TIMESCALE 1000000000u            // a tick no finer than a nanosecond
#define OS64_MEDIA_QUOTA_PICTURE (150ull << 20)         // bytes of frames a picture track keeps
#define OS64_MEDIA_QUOTA_SOUND (12ull << 20)            // and a sound track
#define OS64_MEDIA_PROTECT_AHEAD_NS 10000000000ll       // eviction keeps 10 s past the play position

typedef enum {
    OS64_MEDIA_TRACK_OK = 0,
    OS64_MEDIA_TRACK_NO_MEMORY,
    OS64_MEDIA_TRACK_TIMESCALE,         // a timescale finer than a nanosecond
    OS64_MEDIA_TRACK_TIME_OVERFLOW,     // a time that does not fit in int64 nanoseconds
    OS64_MEDIA_TRACK_UNKNOWN_TRACK,     // a frame or init for a track the buffer has not got
    OS64_MEDIA_TRACK_QUOTA,             // QuotaExceededError
    OS64_MEDIA_TRACK_RULE_COUNT
} os64_media_track_rule_t;

enum { OS64_MEDIA_SEGMENTS = 0, OS64_MEDIA_SEQUENCE = 1 };

enum {
    OS64_MEDIA_FRAME_RAP = 1,           // a random access point (every sound frame is one)
    OS64_MEDIA_FRAME_PRELOAD = 2,       // decode it, discard its output (§ Preroll)
};

typedef struct {
    void *(*alloc)(void *context, size_t bytes);
    void (*release)(void *context, void *block);    // may be called under the caller's lock
    void *context;
} os64_media_track_memory_t;

// A coded frame as the buffer keeps it, and as a player is given it.
typedef struct {
    int64_t dts, pts, duration;         // ns, the offset applied
    os64_media_fragment_slab_t *slab;   // its bytes: slab->data + offset, `size` of them
    uint64_t offset;
    uint32_t size;
    uint32_t generation, entry;         // the init segment, and which of its track's sample entries
    uint64_t serial;                    // unique in its track, rising in the order frames were kept
    uint32_t flags;                     // OS64_MEDIA_FRAME_*
} os64_media_track_frame_t;

// A run of frames in decode order that starts at a random access point,
// made of whole coded frame groups (media_track.c says why ranges never
// merge and where the join is applied instead).
typedef struct {
    os64_media_track_frame_t *frame;
    uint32_t count, capacity;
    int64_t start, end;                 // presentation [start, end), preload frames not counted
    uint64_t id;                        // unique in its track
} os64_media_track_range_t;

// An init segment's configurations for one track, copied.
typedef struct {
    uint32_t generation;
    uint32_t entry_count;
    os64_media_fragment_entry_t entry[OS64_MEDIA_FRAGMENT_MAX_ENTRIES];   // configs point into `block`
    uint8_t *block;
    uint32_t frames;                    // frames of it the buffer keeps
    uint32_t holds;                     // decoders opened from it (os64_media_track_generation_hold)
} os64_media_track_generation_t;

typedef struct {
    os64_media_track_memory_t memory;   // the buffer's, copied
    uint32_t id;
    int kind;                           // OS64_MEDIA_FRAGMENT_VIDEO or _AUDIO
    os64_media_track_range_t *range;    // sorted by their first frame's decode time
    uint32_t range_count, range_capacity;
    os64_media_track_generation_t *gen;
    uint32_t gen_count, gen_capacity;
    // Coded frame processing's per-track state.
    int64_t last_dts, last_duration, highest_end;   // OS64_MEDIA_NO_TIME when unset
    bool need_rap;
    int64_t interbuffer;                // the longest frame duration or decode step seen
    uint64_t append_range;              // the range this coded frame group is appended to (0: none)
    uint64_t last_kept;                 // the serial of the group's last kept frame
    os64_media_track_frame_t preload;   // the last sound frame dropped before appendWindowStart
    bool have_preload;
    uint64_t bytes, quota;              // sizes of the frames kept, and the most it may keep
    uint64_t edits;                     // rises with every change that moves a kept frame
    uint64_t next_serial, next_range_id;
} os64_media_track_t;

typedef struct {
    os64_media_track_memory_t memory;
    os64_media_track_t track[OS64_MEDIA_FRAGMENT_MAX_TRACKS];
    uint32_t track_count;
    int mode;                           // OS64_MEDIA_SEGMENTS or _SEQUENCE
    int64_t offset;                     // timestampOffset, ns
    int64_t window_start, window_end;   // appendWindowStart / End, ns (end may be OS64_MEDIA_TIME_MAX)
    int64_t group_start, group_end;     // ns; group_start is OS64_MEDIA_NO_TIME when unset
} os64_media_buffer_t;

typedef struct {
    int64_t start, end;                 // ns, [start, end)
} os64_media_time_range_t;

// A player's place in a track. Zero it, then os64_media_track_seek.
typedef struct {
    uint64_t range_id;
    uint32_t index;                     // of the frame next() returns
    uint64_t edits;                     // the track's, when index was last right
    uint64_t last_serial, next_serial;  // returned last; followed it then (0: none)
    int64_t last_dts;
} os64_media_track_cursor_t;

enum { OS64_MEDIA_TRACK_FRAME = 1, OS64_MEDIA_TRACK_WAIT = 0, OS64_MEDIA_TRACK_RESYNC = -1 };

// Times.
bool os64_media_ticks_to_ns(int64_t ticks, uint32_t timescale, int64_t *ns);
int64_t os64_media_ns_to_ticks(int64_t ns, uint32_t timescale);        // nearest; ns from ticks only
bool os64_media_seconds_to_ns(double seconds, int64_t *ns);             // false: not finite or past int64
double os64_media_ns_to_seconds(int64_t ns);

// The SourceBuffer's half.
void os64_media_buffer_init(os64_media_buffer_t *b, const os64_media_track_memory_t *memory);
void os64_media_buffer_destroy(os64_media_buffer_t *b);
// MSE § 5.5.7 for the buffer: the first makes the tracks, each later one
// adds a generation; every one sets need-random-access-point on all tracks.
os64_media_track_rule_t os64_media_buffer_init_segment(os64_media_buffer_t *b, const os64_media_fragment_init_t *init);
// Coded frame processing for one frame (MSE § 5.5.8, step 1).
os64_media_track_rule_t os64_media_buffer_frame(os64_media_buffer_t *b, const os64_media_fragment_frame_t *frame);
// The buffer's half of reset parser state (MSE § 5.5.2): in sequence mode
// the group start becomes the group end; every track's last decode time,
// last duration and highest end are unset, and it needs a random access point.
void os64_media_buffer_reset_parser(os64_media_buffer_t *b);
void os64_media_buffer_set_mode(os64_media_buffer_t *b, int mode);
// Range removal (MSE § 3.5.9): every track's frames presented in [start, end), and what depended on them.
void os64_media_buffer_remove(os64_media_buffer_t *b, int64_t start, int64_t end);
// The coded frame eviction and prepare-append's check (MSE § 3.5.13, § 3.5.12 steps 6-7) before an
// append of `incoming` bytes, with the play position (OS64_MEDIA_NO_TIME for none): QUOTA when it
// still does not fit, and then the append is refused whole.
os64_media_track_rule_t os64_media_buffer_make_room(os64_media_buffer_t *b, uint64_t incoming, int64_t play);
// `buffered`: the intersection of the tracks' ranges. Answers how many there are; fills at most `cap`.
uint32_t os64_media_buffer_buffered(const os64_media_buffer_t *b, os64_media_time_range_t *out, uint32_t cap);
os64_media_track_t *os64_media_buffer_track(os64_media_buffer_t *b, int kind);

// A track, for a player. Each copies a value out and allocates nothing.
uint32_t os64_media_track_ranges(const os64_media_track_t *t, os64_media_time_range_t *out, uint32_t cap);
// Whether `time` is buffered, and where its range ends.
bool os64_media_track_ahead(const os64_media_track_t *t, int64_t time, int64_t *range_end);
// The random access point to start a decode for `time`: FRAME (and *rap) or WAIT (not buffered).
int os64_media_track_rap_at_or_before(const os64_media_track_t *t, int64_t time, os64_media_track_frame_t *rap);
// The first frame presented at or after `time` in the range holding it (a
// scan of that range: for a paused seek's preview, not for every frame).
int os64_media_track_frame_at_or_after(const os64_media_track_t *t, int64_t time, os64_media_track_frame_t *frame);
// Place the cursor at the random access point for `time` (a preload before it included): FRAME or WAIT.
int os64_media_track_seek(const os64_media_track_t *t, int64_t time, os64_media_track_cursor_t *cursor);
// The next frame in decode order: FRAME (copied, its slab held for the player), WAIT, or RESYNC.
int os64_media_track_next(const os64_media_track_t *t, os64_media_track_cursor_t *cursor,
                          os64_media_track_frame_t *frame);
// A frame taken by next() is done with: the player's hold on its slab is dropped.
void os64_media_track_frame_done(os64_media_track_frame_t *frame);
// A generation's configurations, copied out and held while a decoder uses
// them (the configuration bytes stay put until the drop); false if it is gone.
bool os64_media_track_generation_hold(os64_media_track_t *t, uint32_t generation,
                                      os64_media_track_generation_t *out);
void os64_media_track_generation_drop(os64_media_track_t *t, uint32_t generation);

const char *os64_media_track_rule_name(os64_media_track_rule_t rule);

#pragma GCC visibility pop

#endif
