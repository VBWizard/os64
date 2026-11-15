// test_media_queue_host.c — libmedia's PCM queue (userland/libmedia/media_queue.c)
// against a model kept a frame at a time, under ASan and UBSan: random
// pushes that continue the last or jump (a loop), takes of random sizes,
// flushes with stale-generation pushes after them, the end, a full queue,
// and a queue out of segments.

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "os64/media_queue.h"

static int s_failures, s_checks;
#define CHECK(cond, ...) do { s_checks++; if (!(cond)) { s_failures++; \
    fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); fprintf(stderr, __VA_ARGS__); \
    fputc('\n', stderr); if (s_failures > 20) exit(1); } } while (0)

static uint64_t s_rng = 0x9E3779B97F4A7C15ull;
static uint64_t rnd(void) { s_rng ^= s_rng << 13; s_rng ^= s_rng >> 7; s_rng ^= s_rng << 17; return s_rng; }

// The model: every queued frame with its two samples and its output frame.
typedef struct { int16_t l, r; int64_t at; } Frame;
static Frame *s_model;
static uint32_t s_held, s_cap;
static uint32_t s_generation;
static int s_ended;

// The segments the model's frames make: a frame not one past the frame
// before it starts another.
static uint32_t model_segments(void)
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < s_held; i++)
        if (i == 0 || s_model[i].at != s_model[i - 1].at + 1) n++;
    return n;
}

static void run(uint32_t capacity, int rounds)
{
    int16_t *storage = malloc(sizeof(int16_t) * 2 * capacity);
    os64_media_queue_t q;
    os64_media_queue_init(&q, storage, capacity);
    s_model = malloc(sizeof(Frame) * capacity);
    s_held = 0;
    s_cap = capacity;
    s_generation = 0;
    s_ended = 0;
    int64_t next_at = 0;          // where a continuing push starts
    int16_t serial = 1;
    int16_t *pcm = malloc(sizeof(int16_t) * 2 * (capacity + 8));
    int16_t *out = malloc(sizeof(int16_t) * 2 * (capacity + 8));
    for (int round = 0; round < rounds; round++) {
        uint64_t op = rnd() % 100;
        if (op < 50) {
            uint32_t frames = 1 + (uint32_t)(rnd() % (capacity / 3 + 2));
            int jump = rnd() % 8 == 0;
            int64_t at = jump ? (int64_t)(rnd() % 1000) : next_at;
            int stale = rnd() % 16 == 0;
            uint32_t generation = stale ? s_generation - 1 : s_generation;
            for (uint32_t i = 0; i < frames; i++) {
                pcm[2 * i] = serial;
                pcm[2 * i + 1] = (int16_t)~serial;
                serial++;
            }
            // What the model says a push takes.
            uint32_t want = frames;
            if (stale || s_ended) want = 0;
            if (want > s_cap - s_held) want = s_cap - s_held;
            int continues = s_held > 0 && s_model[s_held - 1].at + 1 == at;
            if (want > 0 && !continues && model_segments() == OS64_MEDIA_QUEUE_SEGMENTS) want = 0;
            uint32_t got = os64_media_queue_push(&q, generation, pcm, frames, at);
            CHECK(got == want, "round %d: push of %u at %lld took %u, want %u", round, frames, (long long)at, got,
                  want);
            for (uint32_t i = 0; i < got; i++)
                s_model[s_held++] = (Frame){pcm[2 * i], pcm[2 * i + 1], at + i};
            if (got > 0) next_at = at + got;
        } else if (op < 85) {
            uint32_t frames = 1 + (uint32_t)(rnd() % (capacity / 2 + 2));
            int drop = rnd() % 10 == 0;
            uint32_t want = 0;
            while (want < frames && want < s_held && (want == 0 || s_model[want].at == s_model[want - 1].at + 1))
                want++;
            int64_t at = -7;
            uint32_t got = os64_media_queue_take(&q, drop ? NULL : out, frames, &at);
            CHECK(got == want, "round %d: take of %u gave %u, want %u", round, frames, got, want);
            if (got > 0) CHECK(at == s_model[0].at, "round %d: take at %lld, want %lld", round, (long long)at,
                               (long long)s_model[0].at);
            for (uint32_t i = 0; !drop && i < got && i < want; i++)
                CHECK(out[2 * i] == s_model[i].l && out[2 * i + 1] == s_model[i].r,
                      "round %d: frame %u is %d,%d, want %d,%d", round, i, out[2 * i], out[2 * i + 1],
                      s_model[i].l, s_model[i].r);
            uint32_t gone = got < s_held ? got : s_held;
            memmove(s_model, s_model + gone, sizeof(Frame) * (s_held - gone));
            s_held -= gone;
        } else if (op < 92) {
            uint32_t generation = os64_media_queue_flush(&q);
            CHECK(generation == s_generation + 1, "flush gave generation %u, want %u", generation, s_generation + 1);
            s_generation = generation;
            s_held = 0;
            s_ended = 0;
            next_at = (int64_t)(rnd() % 5000);
        } else {
            os64_media_queue_end(&q);
            s_ended = 1;
        }
        CHECK(os64_media_queue_count(&q) == s_held, "round %d: count %u, want %u", round,
              os64_media_queue_count(&q), s_held);
        CHECK(os64_media_queue_room(&q) == s_cap - s_held, "round %d: room", round);
        CHECK(os64_media_queue_front(&q) == (s_held > 0 ? s_model[0].at : -1), "round %d: front", round);
        CHECK(os64_media_queue_ended(&q) == (s_ended != 0), "round %d: ended", round);
        CHECK(os64_media_queue_finished(&q) == (s_ended && s_held == 0), "round %d: finished", round);
    }
    free(storage);
    free(s_model);
    free(pcm);
    free(out);
}

static void edges(void)
{
    int16_t storage[8], pcm[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
    os64_media_queue_t q;
    os64_media_queue_init(&q, storage, 4);
    CHECK(os64_media_queue_take(&q, pcm, 4, NULL) == 0, "an empty queue gives nothing");
    CHECK(os64_media_queue_push(&q, 0, pcm, 0, 0) == 0, "a push of nothing takes nothing");
    CHECK(os64_media_queue_push(&q, 0, pcm, 2, -1) == 0, "a frame before the start is refused");
    CHECK(os64_media_queue_push(&q, 0, pcm, 5, 10) == 4, "a push past the room takes what fits");
    CHECK(os64_media_queue_push(&q, 0, pcm, 1, 14) == 0, "a full queue takes nothing");
    int64_t at;
    int16_t out[8];
    CHECK(os64_media_queue_take(&q, out, 3, &at) == 3 && at == 10 && out[0] == 1 && out[5] == 6, "take");
    // Wrapping round the end of the storage.
    CHECK(os64_media_queue_push(&q, 0, pcm + 8, 2, 14) == 2, "a push that wraps");
    CHECK(os64_media_queue_take(&q, out, 8, &at) == 3 && at == 13 && out[0] == 7 && out[2] == 9 && out[3] == 10,
          "a take across the wrap");
    // A queue with no storage is a queue of nothing.
    os64_media_queue_init(&q, NULL, 100);
    CHECK(os64_media_queue_room(&q) == 0 && os64_media_queue_push(&q, 0, pcm, 1, 0) == 0, "no storage");
}

int main(void)
{
    edges();
    uint32_t capacities[] = {1, 2, 7, 64, 1000};
    for (size_t i = 0; i < sizeof(capacities) / sizeof(capacities[0]); i++)
        run(capacities[i], 20000);
    if (s_failures) {
        fprintf(stderr, "FAIL media_queue: %d of %d checks\n", s_failures, s_checks);
        return 1;
    }
    printf("PASS media_queue: %d checks against the model\n", s_checks);
    return 0;
}
