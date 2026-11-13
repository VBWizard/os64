// checks.h — what a resampler must do to a sine, for os64/resample.h. One
// body, run in ring 3 by /tests/resampletest and on the host under the
// sanitizers by tools/test_resample_host.sh, so the port and its host proof
// cannot come to check different things.
//
// A 1 kHz sine at 44.1 kHz, stereo with the right channel inverted, goes in
// through writes of awkward sizes and comes out at 48 kHz. The output must
// be the right LENGTH (the second, and ahead of it the silence the
// resampler's latency puts there, once what it holds is drawn out with as
// much silence behind), and over its steady middle it must be the SAME SINE:
// fitted against 1 kHz at 48 kHz by least squares, what is left over is
// under two counts in 12,000 (rounding to 16 bits alone leaves about a
// third of one), and the amplitude is within a thousandth of what went in. The same holds going down, 48 kHz to 44.1, and a
// mono stream; and the refusals are refusals.
//
// Returns 0, or the number of the check that failed; `say` is told each
// measurement.

#ifndef RESAMPLE_CHECKS_H
#define RESAMPLE_CHECKS_H

#include <math.h>
#include <stddef.h>
#include <stdint.h>

#include "os64/resample.h"

#define RC_PI        3.14159265358979323846
#define RC_AMPLITUDE 12000.0
#define RC_TONE_HZ   1000.0
#define RC_MAX       (2 * 48000 + 4096)

static int16_t rc_in[2 * 48000];
static int16_t rc_out[2 * RC_MAX];

// One second of the tone at `rate`, `channels` wide, resampled to `to`.
// Returns the frames produced, or a negative check number; *held is the
// resampler's latency, in input frames.
__attribute__((unused)) static long rc_convert(uint32_t channels, uint32_t rate, uint32_t to, uint32_t *held)
{
    for (uint32_t i = 0; i < rate; i++)
    {
        double v = RC_AMPLITUDE * sin(2.0 * RC_PI * RC_TONE_HZ * (double)i / (double)rate);
        rc_in[i * channels] = (int16_t)lrint(v);
        if (channels == 2)
            rc_in[i * 2 + 1] = (int16_t)-lrint(v);
    }
    os64_resampler_t *r = os64_resampler_create(channels, rate, to);
    if (r == NULL)
        return -1;
    uint32_t latency = os64_resampler_latency(r);
    *held = latency;
    uint32_t fed = 0, made = 0, piece = 1;
    // The second, in pieces of 1, 2, 3 ... frames, into room of odd sizes.
    while (fed < rate)
    {
        uint32_t in = rate - fed < piece ? rate - fed : piece;
        uint32_t out = (piece * 7) % 301 + 1;
        if (made + out > RC_MAX)
            return -2;
        if (os64_resampler_run(r, rc_in + fed * channels, &in, rc_out + made * channels, &out) != 0)
            return -3;
        fed += in;
        made += out;
        piece = piece % 997 + 1;
    }
    // What it still holds, drawn out with silence.
    static const int16_t silence[2 * 64];
    uint32_t tail = latency;
    while (tail > 0)
    {
        uint32_t in = tail < 64 ? tail : 64, out = RC_MAX - made;
        if (os64_resampler_run(r, silence, &in, rc_out + made * channels, &out) != 0)
            return -3;
        if (in == 0 && out == 0)
            return -4;   // neither consuming nor producing: it would never end
        tail -= in;
        made += out;
    }
    os64_resampler_destroy(r);
    return (long)made;
}

// Fit channel `c` of rc_out, frames [from, to), against the tone at `rate`.
__attribute__((unused)) static void rc_fit(uint32_t channels, uint32_t c, uint32_t rate, uint32_t from, uint32_t to,
                   double *amplitude, double *residual)
{
    double ss = 0, cc = 0, sc = 0, ys = 0, yc = 0;
    for (uint32_t i = from; i < to; i++)
    {
        double w = 2.0 * RC_PI * RC_TONE_HZ * (double)i / (double)rate;
        double s = sin(w), k = cos(w), y = rc_out[i * channels + c];
        ss += s * s; cc += k * k; sc += s * k; ys += y * s; yc += y * k;
    }
    double det = ss * cc - sc * sc;
    double a = (ys * cc - yc * sc) / det, b = (yc * ss - ys * sc) / det;
    double err = 0;
    for (uint32_t i = from; i < to; i++)
    {
        double w = 2.0 * RC_PI * RC_TONE_HZ * (double)i / (double)rate;
        double d = rc_out[i * channels + c] - (a * sin(w) + b * cos(w));
        err += d * d;
    }
    *amplitude = sqrt(a * a + b * b);
    *residual = sqrt(err / (double)(to - from));
}

__attribute__((unused)) static int rc_one(int base, uint32_t channels, uint32_t rate, uint32_t to,
                  void (*say)(const char *what, long value))
{
    uint32_t held = 0;
    long made = rc_convert(channels, rate, to, &held);
    say("frames out for one second in", made);
    say("latency, input frames", held);
    if (made < 0)
        return base;
    // Everything fed, at the new rate: the second and the latency's worth
    // of silence, give or take the rounding between rates.
    long want = (long)(((uint64_t)rate + held) * to / rate);
    if (made < want - 4 || made > want + 4)
        return base + 1;
    for (uint32_t c = 0; c < channels; c++)
    {
        double amplitude, residual;
        rc_fit(channels, c, to, to / 4, to - to / 4, &amplitude, &residual);
        say("fitted amplitude", lrint(amplitude));
        say("residual, hundredths of a count", lrint(residual * 100.0));
        if (fabs(amplitude - RC_AMPLITUDE) > RC_AMPLITUDE / 1000.0)
            return base + 2;
        if (residual > 2.0)
            return base + 3;
    }
    // The channels were each other's negative going in, and are coming out.
    if (channels == 2)
        for (uint32_t i = to / 4; i < to - to / 4; i++)
        {
            int sum = rc_out[i * 2] + rc_out[i * 2 + 1];
            if (sum < -2 || sum > 2)
                return base + 4;
        }
    return 0;
}

__attribute__((unused)) static int resample_checks(void (*say)(const char *what, long value))
{
    int failed;
    if ((failed = rc_one(10, 2, 44100, 48000, say)) != 0) return failed;
    if ((failed = rc_one(20, 2, 48000, 44100, say)) != 0) return failed;
    if ((failed = rc_one(30, 1, 22050, 48000, say)) != 0) return failed;
    if ((failed = rc_one(40, 2, 48000, 48000, say)) != 0) return failed;

    if (os64_resampler_create(0, 44100, 48000) != NULL) return 50;
    if (os64_resampler_create(9, 44100, 48000) != NULL) return 51;
    if (os64_resampler_create(2, 0, 48000) != NULL) return 52;
    if (os64_resampler_create(2, 44100, 0) != NULL) return 53;
    uint32_t n = 1;
    int16_t frame[2] = { 0, 0 };
    if (os64_resampler_run(NULL, frame, &n, frame, &n) == 0) return 54;
    if (os64_resampler_latency(NULL) != 0) return 55;
    os64_resampler_destroy(NULL);
    return 0;
}

#endif // RESAMPLE_CHECKS_H
