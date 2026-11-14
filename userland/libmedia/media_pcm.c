// media_pcm.c — any decoded sound into signed 16-bit stereo (media_pcm.h).
//
// The rules, each the conventional one:
//
// - One channel is played on both sides, unchanged.
// - Two channels are left and right, whatever the media calls them.
// - More are folded down by ITU-R BS.775's weights: each side is its front
//   channel, plus the centre and that side's rear at 1/sqrt(2); the
//   low-frequency channel is left out, as a stereo downmix does. Each side
//   is then divided by the sum of the weights it used, so the fold can never
//   clip, whatever the channels hold. A layout with no front left or right
//   at all (only unnamed channels) becomes the average of every channel, on
//   both sides.
// - Integer samples are taken as they are: 16-bit unchanged, 8-bit moved to
//   signed and widened, 32-bit narrowed by rounding off the low half.
//   Floating point is clamped to full scale and rounded to the nearest
//   16-bit value. When a fold or a conversion needs arithmetic it is done
//   in double precision.

#include "media_pcm.h"

#define HALF_SQRT2 0.70710678118654752440

static double sample(const media_pcm_in_t *in, uint32_t channel, uint32_t frame)
{
    const uint8_t *base;
    size_t index;
    if (in->planar) {
        base = in->planes[channel];
        index = frame;
    } else {
        base = in->planes[0];
        index = (size_t)frame * in->channels + channel;
    }
    switch (in->format) {
    case MEDIA_SAMPLE_U8:  return ((double)base[index] - 128.0) / 128.0;
    case MEDIA_SAMPLE_S16: return (double)((const int16_t *)base)[index] / 32768.0;
    case MEDIA_SAMPLE_S32: return (double)((const int32_t *)base)[index] / 2147483648.0;
    case MEDIA_SAMPLE_F32: return (double)((const float *)base)[index];
    case MEDIA_SAMPLE_F64: return ((const double *)base)[index];
    }
    return 0;
}

// A full-scale value (-1.0 to 1.0 nominal) as the nearest 16-bit sample,
// clamped. A NaN is silence.
static int16_t to_s16(double v)
{
    double scaled = v * 32768.0;
    if (!(scaled > -32768.0)) return scaled != scaled ? 0 : -32768;
    if (scaled >= 32767.0) return 32767;
    double r = scaled + (scaled >= 0 ? 0.5 : -0.5);
    return (int16_t)(int32_t)r;
}

// The same, for the formats whose integer value carries over exactly: no
// floating point, so 16-bit is bit for bit and 8-bit and 32-bit lose only
// what 16 bits cannot hold.
static bool exact(const media_pcm_in_t *in, uint32_t channel, uint32_t frame, int16_t *out)
{
    size_t index = in->planar ? frame : (size_t)frame * in->channels + channel;
    const uint8_t *base = in->planar ? in->planes[channel] : in->planes[0];
    switch (in->format) {
    case MEDIA_SAMPLE_S16:
        *out = ((const int16_t *)base)[index];
        return true;
    case MEDIA_SAMPLE_U8:
        *out = (int16_t)(((int32_t)base[index] - 128) * 256);
        return true;
    case MEDIA_SAMPLE_S32: {
        int64_t v = ((const int32_t *)base)[index];
        v = (v + 0x8000) >> 16;              // round half up, then clamp the one value that overflows
        *out = (int16_t)(v > 32767 ? 32767 : v);
        return true;
    }
    default:
        return false;
    }
}

static int16_t one(const media_pcm_in_t *in, uint32_t channel, uint32_t frame)
{
    int16_t v;
    if (exact(in, channel, frame, &v)) return v;
    return to_s16(sample(in, channel, frame));
}

bool media_pcm_to_stereo(const media_pcm_in_t *in, int16_t *out)
{
    if (in->channels < 1 || in->channels > MEDIA_PCM_MAX_CHANNELS) return false;
    if (in->channels <= 2) {
        uint32_t right = in->channels == 2 ? 1 : 0;
        for (uint32_t f = 0; f < in->frames; f++) {
            out[2 * f] = one(in, 0, f);
            out[2 * f + 1] = one(in, right, f);
        }
        return true;
    }

    // The fold's weights, decided once from the roles.
    double wl[MEDIA_PCM_MAX_CHANNELS] = { 0 }, wr[MEDIA_PCM_MAX_CHANNELS] = { 0 };
    bool fronts = false;
    for (uint32_t c = 0; c < in->channels; c++) {
        switch (in->roles[c]) {
        case MEDIA_ROLE_LEFT:       wl[c] = 1.0; fronts = true; break;
        case MEDIA_ROLE_RIGHT:      wr[c] = 1.0; fronts = true; break;
        case MEDIA_ROLE_CENTRE:     wl[c] = wr[c] = HALF_SQRT2; break;
        case MEDIA_ROLE_LEFT_REAR:  wl[c] = HALF_SQRT2; break;
        case MEDIA_ROLE_RIGHT_REAR: wr[c] = HALF_SQRT2; break;
        default:                    break;
        }
    }
    if (!fronts)
        for (uint32_t c = 0; c < in->channels; c++) wl[c] = wr[c] = 1.0;
    double sum_l = 0, sum_r = 0;
    for (uint32_t c = 0; c < in->channels; c++) { sum_l += wl[c]; sum_r += wr[c]; }
    if (sum_l == 0) sum_l = 1;
    if (sum_r == 0) sum_r = 1;

    for (uint32_t f = 0; f < in->frames; f++) {
        double l = 0, r = 0;
        for (uint32_t c = 0; c < in->channels; c++) {
            if (wl[c] == 0 && wr[c] == 0) continue;
            double v = sample(in, c, f);
            // Added only where it weighs: 0 times a NaN is a NaN, and one
            // bad sample on the right must not silence the left.
            if (wl[c] != 0) l += v * wl[c];
            if (wr[c] != 0) r += v * wr[c];
        }
        out[2 * f] = to_s16(l / sum_l);
        out[2 * f + 1] = to_s16(r / sum_r);
    }
    return true;
}
