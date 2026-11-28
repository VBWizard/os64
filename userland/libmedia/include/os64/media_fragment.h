#ifndef OS64_MEDIA_FRAGMENT_H
#define OS64_MEDIA_FRAGMENT_H

// os64/media_fragment.h — fragmented MP4 turned into coded frames
// (libmedia.so; MEDIA_SOURCE.md § 3 is the contract).
//
// What a page hands SourceBuffer.appendBuffer: an init segment (what each
// track is), then media segments (a moof saying where each coded frame is
// and when, then the mdat holding the frames), cut wherever the network cut
// them. The caller appends the bytes in whatever pieces it has; the parser
// calls its sink, in arrival order, with each init segment and with each
// coded frame the moment the frame's last byte is in. An append answers
// OS64_MEDIA_FRAGMENT_MORE (every byte taken; a box not yet whole waits) or
// -1, with os64_media_fragment_error naming the rule broken
// (os64_media_fragment_rule_t); the error stays until os64_media_fragment_reset.
//
// THE SINK may hold and drop slabs, and may reset or destroy the parser:
// that ends the append it was called from, the rest of the append's bytes
// discarded as the spec's reset parser state discards the input buffer
// (MSE § 5.5.2). A frame in hand outlives that only if its slab was held
// first, and a destroy takes the init's configurations with it. It may
// not append.
//
// PURE, AND NOT LOCKED: no os64 headers, no clock, no I/O; it allocates only
// through the caller's memory functions, and calls nothing but those and its
// sink. tools/test_media_fragment_host.sh drives it beside a model (a box
// writer that builds streams from a description of their frames), under
// every cut down to one byte at a time; tools/test_media_fragment_mutants.py
// breaks each rule; tools/fuzz_media_fragment_host.sh feeds it hostile bytes
// under ASan and UBSan. These bytes come off the network: every length and
// count is checked against the box it sits in before it is used, and a box
// that lies is an error, never a read past what was given.
//
// MEMORY. Appended bytes are copied once:
//  - a moov or moof into the parser's box buffer, reused (grown only when
//    a box is larger than it, up to `max_box`); an accepted moov's buffer
//    BECOMES the init segment, by exchanging the two buffers, so the
//    configurations an init hands out point into it until the next init;
//  - an mdat that holds the moof's frames into a SLAB, one allocation an
//    mdat (at most `max_mdat`), which its frames point into;
//  - a box the parser does not read, and an mdat holding none of the
//    moof's frames, is counted past and never held.
// Nothing is moved as boxes are consumed, and nothing is allocated a frame
// (the moof's sample table is reused, at most OS64_MEDIA_FRAGMENT_MAX_SAMPLES).
//
// A SLAB'S LIFE is counted in holds. The parser holds a slab while its mdat
// is filling and lets go when the mdat completes, at reset and at destroy.
// A caller that keeps a frame takes a hold on its slab
// (os64_media_fragment_slab_hold) and lets go when the frame is gone
// (os64_media_fragment_slab_drop); the last drop frees the slab through the
// memory functions it was made with. So a frame handed out stays good after
// a reset cuts its mdat short, and a slab no frame was kept from is freed
// as soon as its mdat ends (or, when the mdat ends in an error, at the reset
// or destroy that follows). Holds are not atomic: one lock of the caller's
// covers every hold and drop on a slab.
//
// TIMES are in the track's timescale, decode time and presentation time
// both, with the init segment's EDIT LIST APPLIED (§ The edit list), so
// either may be negative: an encoder's B-frame delay and AAC's priming are
// written as an edit, and without it picture and sound sit apart.
//
// THE RULES, each an os64_media_fragment_rule_t (W3C Media Source
// Extensions and its ISO BMFF byte stream format; the host test has a case
// for each and the mutants break each):
//  - a partial box waits for the rest (MSE § 5.5.1);
//  - a top-level box whose size is smaller than its header, or zero (to
//    the end of a file, which a byte stream has not got), is BOX_SIZE;
//    inside a box, a size of zero runs to the end of its container (ISO
//    14496-12 § 4.2) and one smaller than its header is MALFORMED;
//  - a held box over `max_box` is BOX_TOO_LARGE, an mdat holding the
//    moof's frames over `max_mdat` MDAT_TOO_LARGE; a moof naming more than
//    OS64_MEDIA_FRAGMENT_MAX_SAMPLES frames is TOO_MANY_SAMPLES;
//  - a box that runs past its parent, a field that runs past its box, a
//    field with a value the format forbids (an stsd of no entries, an avcC
//    not of version 1 or shorter than its fixed fields, an edit's media
//    time below -1), or a box the format requires that is missing (a trak's
//    mdia, stbl or stsd; a sample entry's avcC or esds; a moof's traf,
//    format § 4) is MALFORMED;
//  - a media segment (a moof) before any init segment is
//    MEDIA_BEFORE_INIT (MSE § 5.5.1 step 6.1);
//  - an init segment without mvex is NO_MVEX, one with samples of its own
//    (entries in stts, stsc, stco, co64 or stsz) INIT_HAS_SAMPLES (format § 3);
//  - an init segment with no picture or sound track is NO_TRACKS; one with
//    more than one track of a kind (not in v1), or more than
//    OS64_MEDIA_FRAGMENT_MAX_IGNORED of other kinds, is TOO_MANY_TRACKS; two
//    tracks with one id is DUPLICATE_TRACK (format § 2); a later init
//    segment with a different number of picture or sound tracks is
//    TRACK_COUNT_CHANGED (MSE § 5.5.7, which counts text tracks too; v1
//    hands none out, so their number changes nothing downstream);
//  - a sample entry v1 does not decode (anything but avc1 or avc3 for a
//    picture; for a sound anything but mp4a carrying AAC: Main, LC, LTP,
//    HE-AAC or HE-AAC v2, under MPEG-4 Audio or MPEG-2 AAC; encrypted
//    entries among them), or an mp4a without its AudioSpecificConfig, is
//    UNSUPPORTED_CODEC; more than
//    OS64_MEDIA_FRAGMENT_MAX_ENTRIES in one stsd is TOO_MANY_ENTRIES; a
//    timescale of zero is BAD_TIMESCALE;
//  - a traf for a track the init segment has not got is UNKNOWN_TRACK;
//    one naming a base data offset is BASE_DATA_OFFSET, and a traf whose
//    first trun has no data offset NO_DATA_OFFSET (format § 4, movie
//    fragment relative addressing); a traf without tfdt is NO_TFDT (§ 4);
//    a sample description index naming no entry is BAD_ENTRY_INDEX; a
//    fragment marked duration-is-empty that names frames is
//    EMPTY_FRAGMENT_HAS_SAMPLES;
//  - a frame whose bytes are not all inside one mdat after its moof (a
//    zero-size frame at an mdat's very end is inside it) is
//    SAMPLE_OUTSIDE_MDAT (format § 4: the mdat holds every byte a trun
//    names), found as soon as it can be: a frame placed before its moof at
//    once, one before the mdat it should be in when that mdat begins, one
//    running past its mdat when the mdat ends, one in no mdat when the
//    next moof or moov begins;
//  - a time that does not fit in 63 bits is TIME_OVERFLOW.
// Boxes it does not read are skipped by length, inside a box it reads and
// at the top level (ftyp, styp, sidx, free, emsg and the rest).
//
// WHAT IT ACCEPTS that the narrowest reading of the format would not: a
// moof may carry several mdats after it; a stsd may carry several entries
// (a frame says which decodes it); avc3 (parameter sets in the frames, not
// the init: the parser does not look inside frames, so their presence is
// the decoder's to find); a traf without default-base-is-moof following
// another, whose data begins where the previous traf's ended (ISO
// 14496-12 § 8.8.7; the byte stream format calls only a single such traf
// relative addressing, MEDIA_SOURCE.md § 3 says why this is accepted); a
// track of another kind (a text or metadata track), whose trafs are read
// for their length and whose frames are not handed out.
//
// THE EDIT LIST. The first edit with a media time places the media: its
// media time is subtracted from every time, and the durations of any empty
// edits before it (in the movie's timescale, mvhd) are added back, so the
// stream starts that much later. Edits after it, and a media rate other
// than one, are ignored (the byte stream format asks only for a single
// edit at rate one, § 3). os64_media_fragment_track_t carries the offset.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#pragma GCC visibility push(default)

#define OS64_MEDIA_FRAGMENT_MAX_TRACKS 2          // one picture and one sound (v1)
#define OS64_MEDIA_FRAGMENT_MAX_IGNORED 4         // tracks of another kind, read past
#define OS64_MEDIA_FRAGMENT_MAX_ENTRIES 4         // sample entries in one track's stsd
#define OS64_MEDIA_FRAGMENT_MAX_SAMPLES 65536     // frames one moof may name
#define OS64_MEDIA_FRAGMENT_DEFAULT_MAX_BOX (4u << 20)
#define OS64_MEDIA_FRAGMENT_DEFAULT_MAX_MDAT (128u << 20)

#define OS64_MEDIA_FRAGMENT_MORE 0                // every byte taken: append more

typedef enum {
    OS64_MEDIA_FRAGMENT_OK = 0,
    OS64_MEDIA_FRAGMENT_NO_MEMORY,
    OS64_MEDIA_FRAGMENT_BOX_SIZE,
    OS64_MEDIA_FRAGMENT_BOX_TOO_LARGE,
    OS64_MEDIA_FRAGMENT_MDAT_TOO_LARGE,
    OS64_MEDIA_FRAGMENT_TOO_MANY_SAMPLES,
    OS64_MEDIA_FRAGMENT_MALFORMED,
    OS64_MEDIA_FRAGMENT_MEDIA_BEFORE_INIT,
    OS64_MEDIA_FRAGMENT_NO_MVEX,
    OS64_MEDIA_FRAGMENT_INIT_HAS_SAMPLES,
    OS64_MEDIA_FRAGMENT_NO_TRACKS,
    OS64_MEDIA_FRAGMENT_TOO_MANY_TRACKS,
    OS64_MEDIA_FRAGMENT_DUPLICATE_TRACK,
    OS64_MEDIA_FRAGMENT_TRACK_COUNT_CHANGED,
    OS64_MEDIA_FRAGMENT_UNSUPPORTED_CODEC,
    OS64_MEDIA_FRAGMENT_TOO_MANY_ENTRIES,
    OS64_MEDIA_FRAGMENT_BAD_TIMESCALE,
    OS64_MEDIA_FRAGMENT_UNKNOWN_TRACK,
    OS64_MEDIA_FRAGMENT_BASE_DATA_OFFSET,
    OS64_MEDIA_FRAGMENT_NO_DATA_OFFSET,
    OS64_MEDIA_FRAGMENT_NO_TFDT,
    OS64_MEDIA_FRAGMENT_BAD_ENTRY_INDEX,
    OS64_MEDIA_FRAGMENT_EMPTY_FRAGMENT_HAS_SAMPLES,
    OS64_MEDIA_FRAGMENT_SAMPLE_OUTSIDE_MDAT,
    OS64_MEDIA_FRAGMENT_TIME_OVERFLOW,
    OS64_MEDIA_FRAGMENT_RULE_COUNT
} os64_media_fragment_rule_t;

enum { OS64_MEDIA_FRAGMENT_VIDEO = 1, OS64_MEDIA_FRAGMENT_AUDIO = 2 };

// The caller's memory: `alloc` answers NULL when it has none, `release`
// frees what `alloc` gave. A cap of zero takes the default.
typedef struct {
    void *(*alloc)(void *context, size_t bytes);
    void (*release)(void *context, void *block);
    void *context;
    uint64_t max_box;
    uint64_t max_mdat;
} os64_media_fragment_memory_t;

// An mdat's payload. `data` holds `filled` of `size` bytes so far.
typedef struct {
    uint8_t *data;
    uint64_t size;
    uint64_t filled;
    uint32_t holds;
    void (*release)(void *context, void *block);
    void *context;
} os64_media_fragment_slab_t;

// One sample entry: how its frames are decoded.
typedef struct {
    uint32_t fourcc;            // 'avc1', 'avc3' or 'mp4a', as the box spells it
    const uint8_t *config;      // the avcC payload, or the AudioSpecificConfig
    uint32_t config_size;
    uint16_t width, height;     // a picture's
    uint16_t channels;          // a sound's
    uint32_t sample_rate;
    uint8_t profile;            // avcC's AVCProfileIndication; esds's objectTypeIndication
    uint8_t level;              // avcC's AVCLevelIndication; the AAC audio object type (1, 2, 4, 5 or 29)
} os64_media_fragment_entry_t;

typedef struct {
    uint32_t id;
    int kind;                   // OS64_MEDIA_FRAGMENT_VIDEO or _AUDIO
    uint32_t timescale;
    int64_t edit_offset;        // subtracted from every time (§ The edit list)
    uint32_t entry_count;
    os64_media_fragment_entry_t entry[OS64_MEDIA_FRAGMENT_MAX_ENTRIES];
    uint32_t trex_entry, trex_duration, trex_size, trex_flags;   // mvex's defaults
} os64_media_fragment_track_t;

// What an init segment says. Its configurations point into the parser's
// copy of the moov and are good until the next init segment or destroy.
typedef struct {
    uint32_t generation;        // 1 for the first init segment, rising with each
    uint32_t track_count;
    os64_media_fragment_track_t track[OS64_MEDIA_FRAGMENT_MAX_TRACKS];
} os64_media_fragment_init_t;

typedef struct {
    uint32_t track_id;
    int kind;
    uint32_t generation;        // the init segment that decodes it
    uint32_t entry;             // which of its track's sample entries (from 0)
    uint32_t timescale;
    int64_t dts, pts;           // edit list applied
    uint32_t duration;
    bool rap;                   // a random access point
    os64_media_fragment_slab_t *slab;   // its bytes: slab->data + offset, `size` of them
    uint64_t offset;
    uint32_t size;
} os64_media_fragment_frame_t;

typedef struct {
    void (*init)(void *context, const os64_media_fragment_init_t *init);
    void (*frame)(void *context, const os64_media_fragment_frame_t *frame);
    void *context;
} os64_media_fragment_sink_t;

// One sample of the current moof, by its stream position (the parser's).
typedef struct {
    uint64_t start;             // stream position of its first byte
    uint32_t size, duration, track, entry;
    int64_t dts, pts;
    bool rap;
    bool ignored;               // a track of another kind: never handed out
} os64_media_fragment_sample_t;

typedef struct {
    os64_media_fragment_memory_t memory;
    os64_media_fragment_sink_t sink;
    os64_media_fragment_rule_t error;
    uint32_t resets;            // counts resets, so an append sees a sink's
    int state;
    uint64_t position;          // stream bytes taken so far
    // The box being read.
    uint8_t header[16];
    uint32_t header_have, header_size;
    uint64_t box_size;
    uint8_t *box;               // the box buffer (moov, moof)
    uint64_t box_capacity, box_have;
    uint64_t box_start;         // its stream position (a moof's data offsets count from it)
    uint64_t skip_left;
    // The init segment.
    uint8_t *init_box;          // the accepted moov's bytes; configurations point here
    uint64_t init_capacity;
    os64_media_fragment_init_t init;
    uint32_t ignored[OS64_MEDIA_FRAGMENT_MAX_IGNORED];
    uint32_t ignored_count;
    bool have_init;
    // The current moof's samples, ordered by stream position.
    os64_media_fragment_sample_t *sample;
    uint32_t sample_capacity, sample_count, sample_next;
    // The mdat being filled.
    os64_media_fragment_slab_t *slab;
    uint64_t mdat_start, mdat_end;  // its payload's stream positions
} os64_media_fragment_t;

void os64_media_fragment_init(os64_media_fragment_t *p, const os64_media_fragment_memory_t *memory,
                              const os64_media_fragment_sink_t *sink);
// OS64_MEDIA_FRAGMENT_MORE, or -1 with os64_media_fragment_error saying which rule.
int os64_media_fragment_append(os64_media_fragment_t *p, const uint8_t *bytes, size_t length);
os64_media_fragment_rule_t os64_media_fragment_error(const os64_media_fragment_t *p);
const char *os64_media_fragment_rule_name(os64_media_fragment_rule_t rule);
// The spec's reset parser state (MSE § 5.5.2): a partial box and an
// unfinished mdat are dropped, the error is cleared, and the init segment
// is kept, so the media segments after an abort() still parse.
void os64_media_fragment_reset(os64_media_fragment_t *p);
void os64_media_fragment_destroy(os64_media_fragment_t *p);

void os64_media_fragment_slab_hold(os64_media_fragment_slab_t *slab);
void os64_media_fragment_slab_drop(os64_media_fragment_slab_t *slab);

#pragma GCC visibility pop

#endif
