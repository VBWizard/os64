#ifndef OS64_MEDIA_FED_H
#define OS64_MEDIA_FED_H
// os64/media_fed.h — a media fed by appends, as a page's MediaSource feeds
// its element (Media Source Extensions' half in libmedia.so).
// docs/design/pending/MEDIA_SOURCE.md § 5 is the design.
//
// A FED MEDIA holds BUFFERS, one per SourceBuffer: each a fragmented-MP4
// parser (os64/media_fragment.h) and its track buffers (os64/media_track.h).
// A player made with the fed media in its config (os64/media_player.h,
// `fed`) decodes from it: its decoding thread pulls coded frames from the
// track buffers, so everything after the decoders (the queues, the clock,
// the pace, the picture and show threads, the voice) is the player's own.
//
// THE CALLS here mirror libdom's fed provider (dom/media.h § THE FED SOURCE)
// one for one, its times in seconds, so a host's provider is a thin hand-off.
// They are made from ONE thread (yonder's window thread, gplay's main). Each
// returns at once: it takes the fed media's lock for what it does and no
// longer, and never waits on the player's threads. A parse holds the lock
// for one 64 KiB piece of input at a time.
//
// READINESS (HTML's readyState) is the player's: os64_media_player_facts'
// `current`, `future` and `enough` (os64/media_player.h), which for a fed
// media are judged from what both kinds hold, intersected, at the
// position, never from how far the decoder has got: `current` while the
// position is in a range, `future` with a quarter second buffered past it,
// `enough` with three seconds, each also once the range reaches the end
// after endOfStream, or the position is past the last range then (all
// there will be is there). A playing player whose position leaves what is
// buffered (before the end) BUFFERS, its clock held there, and plays on
// once `future` holds again, as other browsers fire `waiting` only when a
// buffered range runs out. The player's length is the duration last given.
// A host copies all of it and derives nothing of its own.
//
// LIFETIME: the fed media has two holders, its creator and the player made
// over it. The creator lets go with os64_media_fed_close; the player lets
// go after its decoders have let go of every coded frame. Whichever is last
// frees it, so a page may close the fed media while an append is in hand
// and the player is still decoding, in either order.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#pragma GCC visibility push(default)

typedef struct os64_media_fed os64_media_fed_t;
typedef struct os64_media_fed_buffer os64_media_fed_buffer_t;

typedef enum {
    OS64_MEDIA_FED_DONE,        // every byte taken was parsed
    OS64_MEDIA_FED_PENDING,     // the parse runs elsewhere: poll again
    OS64_MEDIA_FED_ERROR,       // the bytes broke a rule: the append error (the parser was reset)
} os64_media_fed_status_t;

typedef enum { OS64_MEDIA_FED_SEGMENTS, OS64_MEDIA_FED_SEQUENCE } os64_media_fed_mode_t;

// Where sequence mode's next coded frame group starts, as the assignment
// that made an os64_media_fed_set says (MSE § 3.1); segments mode ignores it.
typedef enum {
    OS64_MEDIA_FED_GROUP_KEEP,      // no assignment says: as the frames already decided
    OS64_MEDIA_FED_GROUP_AT_OFFSET, // timestampOffset was assigned (the same value too): at it
    OS64_MEDIA_FED_GROUP_AT_END,    // mode was assigned "sequence" (the same mode too): at the group's end
} os64_media_fed_group_t;

// What a parse, a reset or a removal did to a buffer (dom/media.h's
// os64_dom_fed_result_t, field for field). Times in seconds.
typedef struct {
    os64_media_fed_status_t status;
    uint32_t inits;             // initialization segments completed
    double init_duration;       // the FIRST one's duration (the spec takes one only while the duration is
                                // unknown); NaN when it gives none (0 or all ones in the file)
    bool audio, video;          // the tracks the buffer's first initialization segment holds
    bool frames;                // coded frames were added
    bool parsing_media;         // the parser stopped inside a media segment
    double group_end;           // the coded frame group's end; NaN when unset
    double highest_pts;         // the latest presentation start of any frame the buffer holds; NaN none
    double timestamp_offset;    // the buffer's timestampOffset now (sequence mode moves it)
    char message[96];           // why, with OS64_MEDIA_FED_ERROR
} os64_media_fed_result_t;

typedef struct { double start, end; } os64_media_fed_range_t;

// What the fed media holds and has done, for a player's report and tests.
typedef struct {
    uint64_t slab_bytes;        // media segment storage held (slabs, their padding included)
    uint64_t frame_bytes;       // the coded frames' own bytes, which the quotas count
    uint64_t resyncs;           // a cursor's place lost (its frames removed or evicted)
    uint64_t silence_frames;    // output frames of silence put into gaps in the sound
    uint64_t cut_frames;        // output frames cut where sound overlapped
    uint64_t reopens;           // decoders reopened for a new configuration
} os64_media_fed_counts_t;

// A fed media with no buffers: 0, or OS64_MEDIA_NO_MEMORY (os64/media.h).
int os64_media_fed_create(os64_media_fed_t **out);
// The creator's hold let go (the detach): every buffer goes with it, and
// the player made over it, if any, reads nothing more from it.
void os64_media_fed_close(os64_media_fed_t *fed);

// Whether a MediaSource buffer of this MIME type, with its codecs, is one
// this library takes (isTypeSupported): fragmented MP4 ("video/mp4",
// "audio/mp4") naming codecs libav.so decodes. A type naming no codecs is
// refused, as other browsers refuse it.
bool os64_media_fed_type_supported(const char *type);

// A buffer for `type` (os64_media_fed_type_supported's), or NULL: a type
// not taken, no memory, or once the player has picked the tracks it
// decodes (the first time every buffer held an initialization segment).
os64_media_fed_buffer_t *os64_media_fed_add(os64_media_fed_t *fed, const char *type);
// The buffer and everything it holds go. A cursor the player had in it is
// detached at once; frames the decoders still hold keep their own bytes.
void os64_media_fed_remove_buffer(os64_media_fed_t *fed, os64_media_fed_buffer_t *buf);

// Coded frame eviction before an append of `incoming` bytes, protecting
// what plays from `play` (seconds; NaN for none): false when the buffer
// still could not hold them (QuotaExceededError).
bool os64_media_fed_evict(os64_media_fed_t *fed, os64_media_fed_buffer_t *buf, size_t incoming, double play);
// The append's bytes copied into the buffer's input: false for no memory.
// An append, like a removal, reopens an ended fed media.
bool os64_media_fed_take(os64_media_fed_t *fed, os64_media_fed_buffer_t *buf, const uint8_t *bytes, size_t length);
// The segment parser loop over what was taken; and the last parse's answer
// again (poll) while it is PENDING.
void os64_media_fed_parse(os64_media_fed_t *fed, os64_media_fed_buffer_t *buf, os64_media_fed_result_t *out);
void os64_media_fed_poll(os64_media_fed_t *fed, os64_media_fed_buffer_t *buf, os64_media_fed_result_t *out);
// The reset parser state algorithm (MSE § 3.5.2).
void os64_media_fed_reset(os64_media_fed_t *fed, os64_media_fed_buffer_t *buf, os64_media_fed_result_t *out);
// Coded frame removal of [start, end), seconds.
void os64_media_fed_remove(os64_media_fed_t *fed, os64_media_fed_buffer_t *buf, double start, double end,
                           os64_media_fed_result_t *out);
// The buffer's mode, timestampOffset and append window for the frames that
// follow, and where sequence mode's next group starts (`group`).
void os64_media_fed_set(os64_media_fed_t *fed, os64_media_fed_buffer_t *buf, os64_media_fed_mode_t mode,
                        double timestamp_offset, os64_media_fed_group_t group, double window_start,
                        double window_end);
// The buffer's ranges, up to `cap`, and their count: its tracks' ranges
// intersected, and with `ended` each track's last range first reaching the
// highest end of any; *highest_end (NULL for none) that highest end, NaN
// when the buffer holds nothing.
uint32_t os64_media_fed_ranges(os64_media_fed_t *fed, os64_media_fed_buffer_t *buf, bool ended,
                               os64_media_fed_range_t *out, uint32_t cap, double *highest_end);
// The MediaSource's duration (NaN unknown, INFINITY for no end), after
// each change: the player's length.
void os64_media_fed_duration(os64_media_fed_t *fed, double seconds);
// endOfStream() without an error: nothing more is coming, so each kind ends
// at the end of what its track holds. The next take or removal reopens it.
void os64_media_fed_end(os64_media_fed_t *fed);

void os64_media_fed_counts(os64_media_fed_t *fed, os64_media_fed_counts_t *out);

#pragma GCC visibility pop
#endif
