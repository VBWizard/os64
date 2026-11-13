#ifndef OS64_RESAMPLE_H
#define OS64_RESAMPLE_H

// os64/resample.h — change the sample rate of 16-bit PCM (libsound.so).
//
// /dev/audio takes one rate (os64/audio.h). Sound at another goes through
// this first. The work is speexdsp's resampler, ported whole and unmodified
// (userland/libsound/README.md); this header is the only part of it a
// program sees.
//
//     os64_resampler_t *r = os64_resampler_create(2, 44100, 48000);
//     uint32_t in = have, out = room;
//     os64_resampler_run(r, pcm_in, &in, pcm_out, &out);
//     // `in` frames were consumed, `out` frames produced
//     os64_resampler_destroy(r);
//
// A FRAME is one sample for each channel, interleaved. A resampler holds a
// few milliseconds of sound inside it: the first output is that much
// silence, and the end of a stream is drawn out of it by running it with
// os64_resampler_latency() frames of silence as input.

#include <stdint.h>

// libsound.so is built with everything hidden; these are what it exports.
#pragma GCC visibility push(default)

typedef struct os64_resampler os64_resampler_t;

// A resampler for `channels` interleaved channels (1 to 8) from `in_rate`
// to `out_rate` frames a second. NULL for a rate of zero, a channel count
// out of range, or no memory.
os64_resampler_t *os64_resampler_create(uint32_t channels, uint32_t in_rate, uint32_t out_rate);

// Convert. On entry *in_frames is how many frames `in` holds and
// *out_frames how many `out` has room for; on return they are how many
// were consumed and produced. It stops at whichever runs out first, so a
// caller loops until its input is consumed. Returns 0, or -1 for a NULL
// argument.
int os64_resampler_run(os64_resampler_t *r, const int16_t *in, uint32_t *in_frames,
                       int16_t *out, uint32_t *out_frames);

// How many INPUT frames of sound the resampler is holding back.
uint32_t os64_resampler_latency(const os64_resampler_t *r);

void os64_resampler_destroy(os64_resampler_t *r);

#pragma GCC visibility pop

#endif // OS64_RESAMPLE_H
