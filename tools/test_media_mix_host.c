// libmedia's mixer (userland/libmedia/media_mix.c, os64/media_mix.h) on the
// host under ASan and UBSan. The voice clock is held to a model that keeps
// every frame written, as (stream frame, media frame) pairs, and answers
// what the ear hears from them directly: the frame at the ear's stream
// frame if there is one, else one past the last frame before it, else the
// first frame. Thousands of random histories of runs, gaps, seeks, pauses
// and prunes are played through both. The mix is held to its arithmetic.

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "os64/media_mix.h"

static int s_checks, s_failed;

static void check(int ok, const char *what)
{
    s_checks++;
    if (!ok) {
        s_failed++;
        printf("FAIL: %s\n", what);
    }
}

#define MODEL_MAX 200000

typedef struct {
    uint64_t stream[MODEL_MAX];
    int64_t media[MODEL_MAX];
    uint32_t n;
} Model;

static void model_wrote(Model *mo, uint64_t stream, int64_t media, uint32_t frames)
{
    for (uint32_t i = 0; i < frames && mo->n < MODEL_MAX; i++, mo->n++) {
        mo->stream[mo->n] = stream + i;
        mo->media[mo->n] = media + i;
    }
}

// Frames are written at rising stream frames, so the list is sorted.
static int64_t model_heard(const Model *mo, uint64_t heard)
{
    if (mo->n == 0)
        return -1;
    int64_t before = -1;
    for (uint32_t i = 0; i < mo->n; i++) {
        if (mo->stream[i] == heard)
            return mo->media[i];
        if (mo->stream[i] < heard)
            before = mo->media[i] + 1;
    }
    return before >= 0 ? before : mo->media[0];
}

static uint64_t rnd(uint64_t n)
{
    return n == 0 ? 0 : (uint64_t)rand() % n;
}

static void clock_cases(void)
{
    static Model mo;
    os64_media_voice_t v;
    int bad = 0;
    for (int history = 0; history < 3000; history++) {
        os64_media_voice_reset(&v);
        mo.n = 0;
        uint64_t stream = rnd(1000);     // where the stream's writes have got to
        int64_t media = (int64_t)rnd(100000);
        uint64_t passed = 0;
        for (int step = 0; step < 40 && os64_media_voice_room(&v); step++) {
            uint32_t frames = (uint32_t)(1 + rnd(2048));
            switch (rnd(6)) {
            case 0: stream += rnd(3000); break;                       // silent while others played
            case 1: media = (int64_t)rnd(1000000); break;             // a seek
            case 2: media = 0; break;                                 // a loop
            case 3: stream += rnd(3000); media += (int64_t)rnd(500); break;
            default: break;                                           // goes on
            }
            os64_media_voice_wrote(&v, stream, media, frames);
            model_wrote(&mo, stream, media, frames);
            stream += frames;
            media += frames;
            if (rnd(4) == 0) {
                passed += rnd(stream - passed + 1);
                os64_media_voice_prune(&v, passed);
            }
            for (int q = 0; q < 6; q++) {
                uint64_t h = passed + rnd(stream - passed + 4000);
                int64_t got = os64_media_voice_heard(&v, h), want = model_heard(&mo, h);
                if (got != want && bad++ < 5)
                    printf("  history %d step %d: heard(%llu) = %lld, the model says %lld\n", history, step,
                           (unsigned long long)h, (long long)got, (long long)want);
            }
        }
        if (os64_media_voice_stream_end(&v) != mo.stream[mo.n - 1] + 1 ||
            os64_media_voice_media_end(&v) != mo.media[mo.n - 1] + 1)
            bad++;
    }
    check(bad == 0, "clock: every history read through the anchors answers as the frames written say");

    // Long histories with no prune: past OS64_MEDIA_MIX_ANCHORS runs the
    // oldest go, and from the first kept run on the ear still answers as
    // the frames say; before it, at that run's start (the rule a caller
    // that prunes by `taken` while the ear lags relies on).
    bad = 0;
    for (int history = 0; history < 300; history++) {
        os64_media_voice_reset(&v);
        mo.n = 0;
        uint64_t stream = 0;
        int64_t media = 0;
        for (int step = 0; step < 150; step++) {
            uint32_t frames = (uint32_t)(1 + rnd(64));
            stream += rnd(2) ? rnd(100) : 0;
            media = rnd(3) == 0 ? (int64_t)rnd(100000) : media;
            os64_media_voice_wrote(&v, stream, media, frames);
            model_wrote(&mo, stream, media, frames);
            stream += frames;
            media += frames;
        }
        uint64_t first = v.anchor[0].stream;
        for (int q = 0; q < 20; q++) {
            uint64_t h = first + rnd(stream - first + 100);
            if (os64_media_voice_heard(&v, h) != model_heard(&mo, h))
                bad++;
        }
        if (first > 0 && os64_media_voice_heard(&v, first - 1) != v.anchor[0].media)
            bad++;
    }
    check(bad == 0 && v.anchors == OS64_MEDIA_MIX_ANCHORS,
          "clock: with the oldest runs let go, the kept ones answer as the frames say, and before them at the first's start");

    os64_media_voice_reset(&v);
    check(os64_media_voice_heard(&v, 5) == -1 && os64_media_voice_media_end(&v) == -1 &&
              os64_media_voice_stream_end(&v) == 0,
          "clock: nothing written says nothing");
    os64_media_voice_wrote(&v, 0, 0, 100);
    os64_media_voice_wrote(&v, 100, 100, 100);
    check(v.anchors == 1 && v.anchor[0].frames == 200, "clock: a write that goes on grows its run");
    os64_media_voice_wrote(&v, 300, 200, 50);
    check(v.anchors == 2 && os64_media_voice_heard(&v, 250) == 200 && os64_media_voice_heard(&v, 310) == 210,
          "clock: paused while others played, the ear stays at the end of the run until the next");
    for (uint32_t i = 0; i < OS64_MEDIA_MIX_ANCHORS; i++)
        os64_media_voice_wrote(&v, 1000 + 10 * i, 0, 1);
    check(v.anchors == OS64_MEDIA_MIX_ANCHORS && !os64_media_voice_room(&v),
          "clock: with every anchor in use there is no room for a run");
    os64_media_voice_prune(&v, 1000 + 10 * 63);
    check(v.anchors == 1 && v.anchor[0].stream == 1000 + 10 * 63 && os64_media_voice_room(&v),
          "clock: the ear passing them frees all but the last");
}

static void mix_cases(void)
{
    int16_t a[8] = {1000, -1000, 32767, -32768, 20000, -20000, 7, -7};
    int16_t b[8] = {1000, -1000, 1, -1, 20000, -20000, 0, 0};
    int32_t acc[8] = {0};
    int16_t out[8];
    os64_media_mix_add(acc, a, 4, 1.0);
    os64_media_mix_add(acc, b, 4, 1.0);
    os64_media_mix_out(acc, out, 4);
    check(out[0] == 2000 && out[1] == -2000 && out[2] == 32767 && out[3] == -32768 && out[4] == 32767 &&
              out[5] == -32768 && out[6] == 7 && out[7] == -7,
          "mix: summed, held to 16 bits at both ends");
    // A half and a quarter: the sample divided, rounded toward minus
    // infinity (an arithmetic shift), worked out here by division.
    static const double gains[] = {0.5, 0.25};
    static const int divisors[] = {2, 4};
    int ok = 1;
    for (int g = 0; g < 2; g++) {
        memset(acc, 0, sizeof(acc));
        os64_media_mix_add(acc, a, 4, gains[g]);
        os64_media_mix_add(acc, b, 4, 0.0);
        os64_media_mix_out(acc, out, 4);
        for (int i = 0; i < 8; i++) {
            int q = a[i] / divisors[g];
            if (a[i] < 0 && a[i] % divisors[g] != 0)
                q--;
            ok &= out[i] == q;
        }
    }
    check(ok, "mix: a gain of a half halves, of a quarter quarters, and of nothing adds nothing");
    memset(acc, 0, sizeof(acc));
    os64_media_mix_add(acc, a, 2, 1.0);
    os64_media_mix_out(acc, out, 4);
    check(out[4] == 0 && out[7] == 0 && out[0] == 1000, "mix: a voice shorter than the block is silent after it");
}

int main(void)
{
    srand(4242);
    clock_cases();
    mix_cases();
    printf("media mix host: %d checks, %d failed\n", s_checks, s_failed);
    return s_failed != 0;
}
