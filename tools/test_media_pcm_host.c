// test_media_pcm_host.c — libmedia's media_pcm.c (any decoded sound to
// 16-bit stereo) against a model written the obvious way, under ASan and
// UBSan: every sample format, planar and interleaved, one to eight
// channels with and without named roles, and the edges (full scale, past
// it, NaN, the 32-bit rounding carry).

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "media_pcm.h"

static int s_failures, s_checks;
#define CHECK(cond, ...) do { s_checks++; if (!(cond)) { s_failures++; \
    fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); fprintf(stderr, __VA_ARGS__); \
    fputc('\n', stderr); } } while (0)

static uint64_t s_rng = 0x2545F4914F6CDD1Dull;
static uint64_t rnd(void) { s_rng ^= s_rng << 13; s_rng ^= s_rng >> 7; s_rng ^= s_rng << 17; return s_rng; }

static size_t width(media_sample_t f)
{
    switch (f) {
    case MEDIA_SAMPLE_U8: return 1;
    case MEDIA_SAMPLE_S16: return 2;
    case MEDIA_SAMPLE_S32: case MEDIA_SAMPLE_F32: return 4;
    case MEDIA_SAMPLE_F64: return 8;
    }
    return 0;
}

// --- the model: each sample as a double at full scale, the same way the
// standard formats are read; the integer formats convert exactly as their
// rule says; a fold is the weighted average.

static double model_value(media_sample_t f, const void *p)
{
    switch (f) {
    case MEDIA_SAMPLE_U8:  return (*(const uint8_t *)p - 128.0) / 128.0;
    case MEDIA_SAMPLE_S16: return *(const int16_t *)p / 32768.0;
    case MEDIA_SAMPLE_S32: return *(const int32_t *)p / 2147483648.0;
    case MEDIA_SAMPLE_F32: return *(const float *)p;
    case MEDIA_SAMPLE_F64: return *(const double *)p;
    }
    return 0;
}

static int16_t model_round(double v)
{
    if (isnan(v)) return 0;
    double s = v * 32768.0;
    if (s >= 32767.0) return 32767;
    if (s <= -32768.0) return -32768;
    return (int16_t)lround(s);
}

static int16_t model_exact(media_sample_t f, const void *p)
{
    switch (f) {
    case MEDIA_SAMPLE_S16: return *(const int16_t *)p;
    case MEDIA_SAMPLE_U8:  return (int16_t)((*(const uint8_t *)p - 128) * 256);
    case MEDIA_SAMPLE_S32: {
        int64_t v = ((int64_t)*(const int32_t *)p + 0x8000) >> 16;
        return (int16_t)(v > 32767 ? 32767 : v);
    }
    default: return model_round(model_value(f, p));
    }
}

static void fill_random(media_sample_t f, uint8_t *p, size_t count, int edges)
{
    for (size_t i = 0; i < count; i++) {
        uint64_t r = rnd();
        switch (f) {
        case MEDIA_SAMPLE_U8:  p[i] = (uint8_t)r; break;
        case MEDIA_SAMPLE_S16: ((int16_t *)p)[i] = (int16_t)r; break;
        case MEDIA_SAMPLE_S32: ((int32_t *)p)[i] = (int32_t)r; break;
        case MEDIA_SAMPLE_F32: ((float *)p)[i] = edges && r % 7 == 0 ? (r % 2 ? 1.5f : -1.5f)
                                                 : (float)((double)(int32_t)r / 2147483648.0); break;
        case MEDIA_SAMPLE_F64: ((double *)p)[i] = edges && r % 11 == 0 ? NAN
                                                  : (double)(int32_t)r / 2147483648.0; break;
        }
    }
}

static void test_one_layout(media_sample_t f, int planar, uint32_t channels, int named)
{
    enum { FRAMES = 257 };
    size_t w = width(f);
    uint8_t *storage = malloc(w * FRAMES * channels);
    fill_random(f, storage, (size_t)FRAMES * channels, 1);
    const uint8_t *planes[8];
    for (uint32_t c = 0; c < channels; c++) planes[c] = planar ? storage + w * FRAMES * c : storage;
    media_role_t roles[8] = { MEDIA_ROLE_LEFT, MEDIA_ROLE_RIGHT, MEDIA_ROLE_CENTRE, MEDIA_ROLE_LFE,
                              MEDIA_ROLE_LEFT_REAR, MEDIA_ROLE_RIGHT_REAR, MEDIA_ROLE_OTHER, MEDIA_ROLE_OTHER };
    if (named == 0) for (int c = 0; c < 8; c++) roles[c] = MEDIA_ROLE_OTHER;
    if (named == 2) {      // no front left or right: a centre, rears and the LFE
        media_role_t no_fronts[8] = { MEDIA_ROLE_CENTRE, MEDIA_ROLE_LFE, MEDIA_ROLE_LEFT_REAR,
                                      MEDIA_ROLE_RIGHT_REAR, MEDIA_ROLE_OTHER, MEDIA_ROLE_LFE,
                                      MEDIA_ROLE_CENTRE, MEDIA_ROLE_OTHER };
        memcpy(roles, no_fronts, sizeof(roles));
    }
    media_pcm_in_t in = { f, planar, channels, planes, FRAMES, roles };
    int16_t out[FRAMES * 2 + 2];
    out[FRAMES * 2] = 0x5A5A;
    CHECK(media_pcm_to_stereo(&in, out), "accepted %u channels", channels);
    CHECK(out[FRAMES * 2] == 0x5A5A, "wrote past the frames");

    for (uint32_t i = 0; i < FRAMES; i++) {
        #define AT(c) (planar ? storage + w * ((size_t)FRAMES * (c) + i) : storage + w * ((size_t)i * channels + (c)))
        int16_t want_l, want_r;
        if (channels <= 2) {
            want_l = model_exact(f, AT(0));
            want_r = model_exact(f, AT(channels == 2 ? 1 : 0));
        } else {
            double wl[8] = { 0 }, wr[8] = { 0 }, h = sqrt(0.5);
            int fronts = 0;
            for (uint32_t c = 0; c < channels; c++) {
                if (roles[c] == MEDIA_ROLE_LEFT) { wl[c] = 1; fronts = 1; }
                if (roles[c] == MEDIA_ROLE_RIGHT) { wr[c] = 1; fronts = 1; }
                if (roles[c] == MEDIA_ROLE_CENTRE) wl[c] = wr[c] = h;
                if (roles[c] == MEDIA_ROLE_LEFT_REAR) wl[c] = h;
                if (roles[c] == MEDIA_ROLE_RIGHT_REAR) wr[c] = h;
            }
            if (!fronts) for (uint32_t c = 0; c < channels; c++) wl[c] = wr[c] = roles[c] == MEDIA_ROLE_LFE ? 0 : 1;
            double l = 0, r = 0, sl = 0, sr = 0;
            for (uint32_t c = 0; c < channels; c++) {
                double v = model_value(f, AT(c));
                if (wl[c] != 0) l += v * wl[c];
                if (wr[c] != 0) r += v * wr[c];
                sl += wl[c]; sr += wr[c];
            }
            want_l = model_round(l / sl);
            want_r = model_round(r / sr);
        }
        // The fold sums in a different order than the model may, so it is
        // allowed one count; a straight copy must be exact.
        int slack = channels > 2 ? 1 : 0;
        CHECK(abs(out[2 * i] - want_l) <= slack && abs(out[2 * i + 1] - want_r) <= slack,
              "format %d planar %d channels %u named %d frame %u: %d,%d want %d,%d",
              f, planar, channels, named, i, out[2 * i], out[2 * i + 1], want_l, want_r);
        #undef AT
    }
    free(storage);
}

static void test_edges(void)
{
    // 32-bit: the rounding carry at the top clamps rather than wraps.
    int32_t s32[4] = { INT32_MAX, INT32_MIN, 0x7FFF8000, -0x8000 };
    const uint8_t *p32[1] = { (const uint8_t *)s32 };
    media_pcm_in_t in = { MEDIA_SAMPLE_S32, 0, 1, p32, 4, NULL };
    int16_t out[8];
    media_pcm_to_stereo(&in, out);
    CHECK(out[0] == 32767 && out[2] == -32768 && out[4] == 32767 && out[6] == 0,
          "s32 edges: %d %d %d %d", out[0], out[2], out[4], out[6]);
    // Floating point: full scale, past it, NaN, and the value just under
    // full scale that rounds UP to 32768, which must clamp, not wrap to
    // -32768 (a click at the loudest moment of a song).
    double nearly[1] = { 32767.7 / 32768.0 };
    const uint8_t *pn[1] = { (const uint8_t *)nearly };
    media_pcm_in_t nin = { MEDIA_SAMPLE_F64, 0, 1, pn, 1, NULL };
    media_pcm_to_stereo(&nin, out);
    CHECK(out[0] == 32767, "just under full scale: %d", out[0]);
    float f[4] = { 1.0f, -1.0f, 2.0f, NAN };
    const uint8_t *pf[1] = { (const uint8_t *)f };
    media_pcm_in_t fin = { MEDIA_SAMPLE_F32, 0, 1, pf, 4, NULL };
    media_pcm_to_stereo(&fin, out);
    CHECK(out[0] == 32767 && out[2] == -32768 && out[4] == 32767 && out[6] == 0,
          "f32 edges: %d %d %d %d", out[0], out[2], out[4], out[6]);
    // A fold of channels all at full scale stays at full scale and never wraps.
    float full[6] = { 1, 1, 1, 1, 1, 1 };
    const uint8_t *pfull[1] = { (const uint8_t *)full };
    media_role_t roles[6] = { MEDIA_ROLE_LEFT, MEDIA_ROLE_RIGHT, MEDIA_ROLE_CENTRE, MEDIA_ROLE_LFE,
                              MEDIA_ROLE_LEFT_REAR, MEDIA_ROLE_RIGHT_REAR };
    media_pcm_in_t fold = { MEDIA_SAMPLE_F32, 0, 6, pfull, 1, roles };
    media_pcm_to_stereo(&fold, out);
    CHECK(out[0] == 32767 && out[1] == 32767, "a full-scale fold: %d %d", out[0], out[1]);
    // Refusals write nothing.
    out[0] = 77;
    media_pcm_in_t none = { MEDIA_SAMPLE_S16, 0, 0, pf, 1, NULL };
    CHECK(!media_pcm_to_stereo(&none, out) && out[0] == 77, "zero channels refused");
    none.channels = MEDIA_PCM_MAX_CHANNELS + 1;
    CHECK(!media_pcm_to_stereo(&none, out) && out[0] == 77, "too many channels refused");
}

int main(void)
{
    for (int f = MEDIA_SAMPLE_U8; f <= MEDIA_SAMPLE_F64; f++)
        for (int planar = 0; planar < 2; planar++)
            for (uint32_t ch = 1; ch <= 8; ch++)
                for (int named = 0; named < 3; named++)
                    test_one_layout((media_sample_t)f, planar, ch, named);
    test_edges();
    if (s_failures) {
        fprintf(stderr, "FAIL media_pcm: %d of %d checks\n", s_failures, s_checks);
        return 1;
    }
    printf("PASS media_pcm: %d checks against the model\n", s_checks);
    return 0;
}
