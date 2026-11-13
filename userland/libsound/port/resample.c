// resample.c — os64/resample.h over speexdsp's resampler. The upstream file
// is built as it came; this is the whole of os64's own code in libsound.

#include "os64/resample.h"
#include "os64/mem.h"
#include "speex_resampler.h"

struct os64_resampler
{
    SpeexResamplerState *speex;
};

// speexdsp's 0 to 10. 5 is what it calls "desktop": a filter long enough
// that 44.1 to 48 kHz leaves no audible image, at a cost of a few percent
// of one core for a stereo stream.
#define OS64_RESAMPLER_QUALITY 5

#define OS64_RESAMPLER_MAX_CHANNELS 8

os64_resampler_t *os64_resampler_create(uint32_t channels, uint32_t in_rate, uint32_t out_rate)
{
    if (channels == 0 || channels > OS64_RESAMPLER_MAX_CHANNELS || in_rate == 0 || out_rate == 0)
        return NULL;
    os64_resampler_t *r = os64_malloc(sizeof(*r));
    if (r == NULL)
        return NULL;
    int err = RESAMPLER_ERR_SUCCESS;
    r->speex = speex_resampler_init(channels, in_rate, out_rate, OS64_RESAMPLER_QUALITY, &err);
    if (r->speex == NULL || err != RESAMPLER_ERR_SUCCESS)
    {
        if (r->speex != NULL)
            speex_resampler_destroy(r->speex);
        os64_free(r);
        return NULL;
    }
    return r;
}

int os64_resampler_run(os64_resampler_t *r, const int16_t *in, uint32_t *in_frames,
                       int16_t *out, uint32_t *out_frames)
{
    if (r == NULL || in == NULL || out == NULL || in_frames == NULL || out_frames == NULL)
        return -1;
    // Its lengths are per channel, which is what a frame is.
    return speex_resampler_process_interleaved_int(r->speex, in, in_frames, out, out_frames)
               == RESAMPLER_ERR_SUCCESS ? 0 : -1;
}

uint32_t os64_resampler_latency(const os64_resampler_t *r)
{
    return r != NULL ? (uint32_t)speex_resampler_get_input_latency(r->speex) : 0;
}

void os64_resampler_destroy(os64_resampler_t *r)
{
    if (r == NULL)
        return;
    speex_resampler_destroy(r->speex);
    os64_free(r);
}
