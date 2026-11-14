#ifndef OS64_MEDIA_PCM_H
#define OS64_MEDIA_PCM_H
// media_pcm.c — one decoded frame of any sample layout into the stream's
// own: signed 16-bit stereo, interleaved, at the media's rate (the rate is
// the resampler's business, after this). PURE: no FFmpeg or os64 header,
// so tools/test_media_pcm_host.sh drives it against a model.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    MEDIA_SAMPLE_U8,     // unsigned, 128 is silence
    MEDIA_SAMPLE_S16,
    MEDIA_SAMPLE_S32,
    MEDIA_SAMPLE_F32,    // full scale is -1.0 to 1.0
    MEDIA_SAMPLE_F64,
} media_sample_t;

// What a channel is for, as far as a stereo downmix cares.
typedef enum {
    MEDIA_ROLE_LEFT,         // front left
    MEDIA_ROLE_RIGHT,        // front right
    MEDIA_ROLE_CENTRE,       // front centre
    MEDIA_ROLE_LEFT_REAR,    // side or back, left
    MEDIA_ROLE_RIGHT_REAR,   // side or back, right
    MEDIA_ROLE_LFE,          // low-frequency effects: left out of a stereo mix
    MEDIA_ROLE_OTHER,        // anything else (top, wide, a centre at the back)
} media_role_t;

#define MEDIA_PCM_MAX_CHANNELS 64

typedef struct {
    media_sample_t format;
    bool planar;                 // one plane per channel, else interleaved in planes[0]
    uint32_t channels;           // 1 to MEDIA_PCM_MAX_CHANNELS
    const uint8_t *const *planes;
    uint32_t frames;
    const media_role_t *roles;   // one per channel; consulted only above two channels
} media_pcm_in_t;

// Writes in->frames stereo frames (2 * frames samples) to `out`. False for
// a channel count out of range, which writes nothing.
bool media_pcm_to_stereo(const media_pcm_in_t *in, int16_t *out);

#endif
