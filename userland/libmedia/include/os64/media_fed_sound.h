#ifndef OS64_MEDIA_FED_SOUND_H
#define OS64_MEDIA_FED_SOUND_H
// os64/media_fed_sound.h — where a fed media's decoded sound frames go on
// its timeline (libmedia.so). Pure: no locks, no allocation.
//
// SOUND RUNS ON BY COUNT. Sound is counted in the media's own samples (its
// rate, before the resampler), as Firefox counts it (AudioSink.cpp's
// mFramesParsed). A frame whose presentation time is within the tolerance
// of where the count says it goes is played whole, at the count: the times
// a fragment states are rounded to its timescale (to the millisecond in
// some), and following each one to the sample would put silence or a cut
// at nearly every frame. Counting at the media's rate keeps a 44.1 kHz AAC
// stream exact (1024 samples a frame), where 48 kHz output frames would
// not divide (1114.56 a frame).
//
// BEYOND THE TOLERANCE, the frame's time decides:
// - a GAP (the frame starts later than the count) up to the bound is filled
//   with silence before it. A wider one cannot be met by reading on (the
//   track buffer's cursor joins no gap wider than the join), so the count
//   starts again at the frame and nothing is filled: `restarted`;
// - an OVERLAP (the frame starts earlier than the count) is cut from the
//   frame's start, the whole frame if it ends before the count. After a
//   seek, which starts the count at its target, this is what drops the
//   decoder's run-in.
//
// Firefox fills a gap past one sample and never cuts; Chromium plays by
// count whatever the times say. The cut is os64's: an overlap left whole
// moves the sound late for the rest of the media.

#include <stdbool.h>
#include <stdint.h>

#pragma GCC visibility push(default)

// The tolerance: a millisecond of the media's samples, at least one.
#define OS64_MEDIA_FED_SOUND_TOLERANCE_US 1000

typedef struct {
    uint32_t rate;          // the media's samples a second
    int64_t tolerance;      // samples
    int64_t bound;          // samples: the widest gap filled with silence
    int64_t count;          // where the next sample goes, in the media's samples from 0
    bool placed;            // a frame or a target has set the count
} os64_media_fed_sound_t;

typedef struct {
    int64_t at;             // where the frame's first kept sample goes (the media's samples)
    int64_t silence;        // samples of silence before it, ending at `at`
    uint32_t skip;          // samples cut from the frame's start
    uint32_t keep;          // samples kept after them
    bool restarted;         // a gap past the bound: the count starts again at the frame
} os64_media_fed_place_t;

// The placement for a media at `rate` samples a second whose widest
// readable gap is `join_ns`. With `target_ns` (a seek's target, the
// presentation time the sound is to start at) the count starts there;
// with OS64_MEDIA_NO_TIME (os64/media_track.h's) it starts at the first
// frame placed.
void os64_media_fed_sound_start(os64_media_fed_sound_t *s, uint32_t rate, int64_t join_ns, int64_t target_ns);

// Where a decoded frame of `samples` samples presented at `pts_ns` goes;
// the count moves past what is kept.
void os64_media_fed_sound_place(os64_media_fed_sound_t *s, int64_t pts_ns, uint32_t samples,
                                os64_media_fed_place_t *out);

// Nanoseconds as the media's samples, nearest.
int64_t os64_media_fed_sound_samples(int64_t ns, uint32_t rate);

#pragma GCC visibility pop
#endif
