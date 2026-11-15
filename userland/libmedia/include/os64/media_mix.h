#ifndef OS64_MEDIA_MIX_H
#define OS64_MEDIA_MIX_H

// os64/media_mix.h — several sounds in one stream (libmedia.so).
//
// A player that sounds more than one media at once writes ONE stream to
// /dev/audio, the MIX: each sound is a VOICE, whose frames, times its gain,
// are summed into a 32-bit accumulator (os64_media_mix_add), and the sum is
// held to 16 bits once every voice has added its part (os64_media_mix_out).
// A voice that has fewer frames than the block adds what it has, and is
// silent for the rest.
//
// THE CLOCK. One stream position (/dev/audiopos's `heard`) has to say where
// each voice is. So each voice keeps ANCHORS: for every RUN of its frames
// (frames that continue one another in the media AND in the stream), the
// stream frame the run began at, the media frame there, and its length. A
// write that does not continue the last run in both starts a new one: a
// seek, a loop, a voice that was silent while others played (paused, or
// with less than a block another voice filled). What the ear hears of a
// voice is `heard` read through its anchors: inside a run, the media frame
// that far into it; between runs (the voice silent while others played),
// the end of the run before; before its first, the start of that one.
//
// PURE, AND NOT LOCKED: it calls nothing and allocates nothing, so
// tools/test_media_mix_host.sh drives it against a model. Its caller (one
// thread, the one that writes the stream) owns every voice.

#include <stdbool.h>
#include <stdint.h>

#pragma GCC visibility push(default)

// The runs one voice keeps while the ear has not reached them. A voice
// with every anchor in use takes nothing until the ear passes one (a clip
// a few milliseconds long on loop starts a run per block).
#define OS64_MEDIA_MIX_ANCHORS 64

typedef struct {
    uint64_t stream;        // the stream frame the run began at
    int64_t media;          // the media frame there
    uint32_t frames;        // its length
} os64_media_mix_anchor_t;

typedef struct {
    os64_media_mix_anchor_t anchor[OS64_MEDIA_MIX_ANCHORS];
    uint32_t anchors;       // in use, the oldest first
} os64_media_voice_t;

// Nothing written: the voice's next frames begin its first run.
void os64_media_voice_reset(os64_media_voice_t *v);
// Whether its next write may begin a new run (an anchor is free).
bool os64_media_voice_room(const os64_media_voice_t *v);
// `frames` of the voice, media frame `media` first, written at stream frame
// `stream`. A write that continues the last run grows it; any other begins
// a run, letting the oldest go when none is free (a caller that cannot read
// the stream's position, and so never prunes).
void os64_media_voice_wrote(os64_media_voice_t *v, uint64_t stream, int64_t media, uint32_t frames);
// The anchors whose run the ear has finished (it is at or past the next
// one's start) are let go: `passed` is the stream frame the ear has
// certainly passed. The last one is always kept.
void os64_media_voice_prune(os64_media_voice_t *v, uint64_t passed);
// The media frame the ear is at when it hears stream frame `heard`, or -1
// for a voice with nothing written.
int64_t os64_media_voice_heard(const os64_media_voice_t *v, uint64_t heard);
// The stream frame after the voice's last written one (0 for none): once
// the ear is there, every frame of it has been heard.
uint64_t os64_media_voice_stream_end(const os64_media_voice_t *v);
// The media frame after its last written one, or -1 for none.
int64_t os64_media_voice_media_end(const os64_media_voice_t *v);

// `frames` of `pcm` (two channels), times `gain` (0 to 1, as Q16: a gain of
// 1 adds the samples as they are), added into `acc`.
void os64_media_mix_add(int32_t *acc, const int16_t *pcm, uint32_t frames, double gain);
// The sum held to 16 bits: what is written.
void os64_media_mix_out(const int32_t *acc, int16_t *out, uint32_t frames);

#pragma GCC visibility pop

#endif
