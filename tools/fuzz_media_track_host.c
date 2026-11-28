// A coverage-guided fuzzer for libmedia's track buffer
// (userland/libmedia/media_track.c), in fuzz_media_fragment_host.c's shape:
// gcc's -fsanitize-coverage=trace-pc marks the code reached, an input that
// reaches new code joins the corpus, and every run is under ASan and UBSan.
//
// An input is an OPERATION STREAM: each operation a byte saying which, and
// the bytes after it its arguments, read as hostile as they come (times up
// to ±2^62 and past, durations of zero and of hours, timescales up to and
// past a nanosecond's, decode times that step back, thousands of frames).
// After every operation the invariants are checked: ranges in order, each
// starting at a random access point and in decode order inside, the bytes
// counted equal to the frames' sizes, every kept frame's generation held;
// at the end every block (slabs included) is given back. A broken
// invariant aborts, and the input is written to the crash file.
//
//   fuzz_media_track <seconds> <crash file> [seed files...]

#define _POSIX_C_SOURCE 200809L   // clock_gettime
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "os64/media_track.h"

#define MAP_BITS 16
static uint8_t s_map[1 << MAP_BITS], s_seen[1 << MAP_BITS];

void __sanitizer_cov_trace_pc(void)
{
    uintptr_t pc = (uintptr_t)__builtin_return_address(0);
    s_map[(pc ^ (pc >> MAP_BITS)) & ((1u << MAP_BITS) - 1)] = 1;
}

static const char *s_crash_path;
static const uint8_t *s_current;
static size_t s_current_size;

static void dump_current(void)
{
    if (s_crash_path == 0 || s_current == 0) return;
    FILE *f = fopen(s_crash_path, "wb");
    if (f) {
        fwrite(s_current, 1, s_current_size, f);
        fclose(f);
    }
}
void __asan_on_error(void) { dump_current(); }
static void on_abort(int sig)
{
    (void)sig;
    dump_current();
    signal(SIGABRT, SIG_DFL);
    abort();
}
static void fail(const char *why)
{
    fprintf(stderr, "fuzz: invariant broken: %s\n", why);
    dump_current();
    abort();
}

static long s_live;
static void *f_alloc(void *c, size_t n) { (void)c; s_live++; return malloc(n); }
static void f_release(void *c, void *b) { (void)c; s_live--; free(b); }
static const os64_media_track_memory_t kMemory = { f_alloc, f_release, 0 };

// The operation stream's reader: past the end, zeros.
typedef struct { const uint8_t *p; size_t n, at; } in_t;
static uint64_t take(in_t *in, int bytes)
{
    uint64_t v = 0;
    for (int i = 0; i < bytes; i++) v = v << 8 | (in->at < in->n ? in->p[in->at++] : 0);
    return v;
}
static int64_t take_time(in_t *in)
{
    // Mostly small, sometimes anything: a byte chooses the scale.
    uint8_t scale = (uint8_t)take(in, 1);
    int64_t v = (int64_t)take(in, 4);
    if (scale & 0x80) v = (int64_t)take(in, 8);          // any 64-bit pattern
    else if (scale & 0x40) v *= 1000;
    if (scale & 1) v = -v;
    return v;
}

static void check(os64_media_buffer_t *b)
{
    for (uint32_t k = 0; k < b->track_count; k++) {
        const os64_media_track_t *t = &b->track[k];
        uint64_t bytes = 0;
        for (uint32_t ri = 0; ri < t->range_count; ri++) {
            const os64_media_track_range_t *r = &t->range[ri];
            if (r->count == 0) fail("an empty range");
            if (!(r->frame[0].flags & OS64_MEDIA_FRAME_RAP)) fail("a range starts at no random access point");
            if (ri > 0 && r->frame[0].dts < t->range[ri - 1].frame[0].dts) fail("ranges out of order");
            for (uint32_t i = 0; i < r->count; i++) {
                const os64_media_track_frame_t *f = &r->frame[i];
                if (i > 0 && f->dts < r->frame[i - 1].dts) fail("decode order broken in a range");
                bytes += f->size;
                bool have = false;
                for (uint32_t g = 0; g < t->gen_count; g++) have |= t->gen[g].generation == f->generation;
                if (!have) fail("a kept frame's generation is gone");
                if (f->slab->holds == 0) fail("a kept frame's slab has no hold");
            }
        }
        if (bytes != t->bytes) fail("the bytes counted are not the frames' sizes");
    }
}

static unsigned s_ops;

static void run(const uint8_t *data, size_t size)
{
    s_current = data;
    s_current_size = size;
    memset(s_map, 0, sizeof(s_map));
    long live0 = s_live;
    in_t in = { data, size, 0 };
    os64_media_buffer_t b;
    os64_media_buffer_init(&b, &kMemory);
    os64_media_track_cursor_t cursor = { 0 };
    os64_media_track_frame_t held[16];
    int nheld = 0;
    uint32_t gen = 0;
    uint32_t timescale[2] = { 90000, 48000 };
    int tracks = 0;
    while (in.at < in.n) {
        uint8_t op = (uint8_t)take(&in, 1);
        s_ops++;
        switch (op % 12) {
        case 0: {                                   // an init segment
            os64_media_fragment_init_t init = { .generation = ++gen, .track_count = tracks ? (uint32_t)tracks : 1u + (uint32_t)(op >> 7) };
            uint8_t config[16];
            for (uint32_t k = 0; k < init.track_count; k++) {
                uint8_t sel = (uint8_t)take(&in, 1);
                static const uint32_t scales[] = { 90000, 48000, 15360, 1000, 1, 10000000, 1000000000, 2000000000u };
                timescale[k] = scales[sel % 8];
                init.track[k] = (os64_media_fragment_track_t){ .id = k + 1, .kind = k == 0 ? OS64_MEDIA_FRAGMENT_VIDEO : OS64_MEDIA_FRAGMENT_AUDIO,
                                                               .timescale = timescale[k], .entry_count = 1 };
                init.track[k].entry[0].config = config;
                init.track[k].entry[0].config_size = (uint32_t)(sel % 17);
            }
            for (int i = 0; i < 16; i++) config[i] = (uint8_t)take(&in, 1);
            if (os64_media_buffer_init_segment(&b, &init) == OS64_MEDIA_TRACK_OK && tracks == 0)
                tracks = (int)init.track_count;
            memset(config, 0xEE, sizeof(config));
            break;
        }
        case 1: case 2: case 3: case 4: {           // a run of frames from one slab
            if (gen == 0) break;
            int track = (int)(take(&in, 1) % 2);
            uint32_t count = 1 + (uint32_t)(take(&in, 1) % 64);
            if (op & 0x80) count = 1 + (uint32_t)(take(&in, 2) % 4096);
            int64_t start = take_time(&in), step = (int64_t)(take(&in, 2) % 6000), dur = (int64_t)(take(&in, 2) % 6000);
            uint8_t pattern = (uint8_t)take(&in, 1);
            os64_media_fragment_slab_t *slab = f_alloc(0, sizeof(*slab) + count * 4 + 1);
            *slab = (os64_media_fragment_slab_t){ .data = (uint8_t *)(slab + 1), .size = count * 4, .filled = count * 4,
                                                  .holds = 1, .release = f_release };
            for (uint32_t i = 0; i < count; i++) {
                // The fuzzer's own arithmetic wraps (unsigned): any int64 is a fair input.
                uint64_t ud = (uint64_t)start + (uint64_t)i * (uint64_t)step;
                uint64_t up = ud + ((pattern & 1) ? (uint64_t)((i * 7) % 3) * (uint64_t)step : 0);
                if (pattern & 0x20) up = ud - (uint64_t)((i * 5) % 3) * (uint64_t)step;   // decoded after presented
                if (pattern & 0x10) ud += (uint64_t)INT64_MAX / 2;                        // decode times near the top
                if (pattern & 2) ud -= (take(&in, 1) % 4) * (uint64_t)step;              // steps back
                int64_t dts = (int64_t)ud, pts = (int64_t)up;
                os64_media_fragment_frame_t f = {
                    .track_id = (uint32_t)track + 1 + ((pattern & 0x40) ? 5 : 0), .kind = track + 1, .generation = gen,
                    .timescale = timescale[track], .dts = dts, .pts = pts, .duration = (uint32_t)dur,
                    .rap = (i % (1 + pattern % 16)) == 0 || (pattern & 4), .slab = slab, .offset = i * 4, .size = 4,
                };
                os64_media_buffer_frame(&b, &f);
            }
            os64_media_fragment_slab_drop(slab);
            break;
        }
        case 5: { int64_t a = take_time(&in), z = take_time(&in); os64_media_buffer_remove(&b, a, z); break; }
        case 6: b.window_start = take_time(&in); b.window_end = (op & 0x80) ? OS64_MEDIA_TIME_MAX : take_time(&in); break;
        case 7: b.offset = take_time(&in); break;
        case 8: os64_media_buffer_set_mode(&b, (int)(op >> 7)); break;
        case 9: os64_media_buffer_reset_parser(&b); break;
        case 10: {                                  // room for an append
            for (uint32_t k = 0; k < b.track_count; k++) b.track[k].quota = 1 + take(&in, 3);
            uint64_t incoming = take(&in, 3);
            int64_t play = (op & 0x80) ? OS64_MEDIA_NO_TIME : take_time(&in);
            os64_media_buffer_make_room(&b, incoming, play);
            os64_media_time_range_t r[8];
            os64_media_buffer_buffered(&b, r, 8);
            break;
        }
        default: {                                  // the player
            if (b.track_count == 0) break;
            os64_media_track_t *t = &b.track[take(&in, 1) % b.track_count];
            int64_t at = take_time(&in);
            os64_media_track_frame_t f;
            if (op & 0x80) os64_media_track_seek(t, at, &cursor);
            for (int i = 0; i < 6; i++) {
                int r = os64_media_track_next(t, &cursor, &f);
                if (r != OS64_MEDIA_TRACK_FRAME) break;
                if (f.offset + f.size > f.slab->size) fail("a frame outside its slab");
                if (nheld < 16) held[nheld++] = f;      // held a while: the player's own hold
                else os64_media_track_frame_done(&f);
            }
            if (op & 0x40) while (nheld > 0) os64_media_track_frame_done(&held[--nheld]);
            os64_media_track_rap_at_or_before(t, at, &f);
            os64_media_track_frame_at_or_after(t, at, &f);
            int64_t end;
            os64_media_track_ahead(t, at, &end);
            os64_media_track_generation_t g;
            if (os64_media_track_generation_hold(t, gen, &g)) os64_media_track_generation_drop(t, gen);
            break;
        }
        }
        check(&b);
    }
    while (nheld > 0) os64_media_track_frame_done(&held[--nheld]);
    os64_media_buffer_destroy(&b);
    if (s_live != live0) fail("blocks not given back");
}

typedef struct { uint8_t *d; size_t n; } input_t;
static input_t *s_corpus;
static size_t s_count, s_capacity;
static uint64_t s_rng = 0x9E3779B97F4A7C15ull;
static uint64_t rnd(void) { s_rng ^= s_rng << 13; s_rng ^= s_rng >> 7; s_rng ^= s_rng << 17; return s_rng; }

static void corpus_add(const uint8_t *d, size_t n)
{
    if (s_count == s_capacity) {
        s_capacity = s_capacity ? s_capacity * 2 : 64;
        s_corpus = realloc(s_corpus, s_capacity * sizeof(input_t));
    }
    s_corpus[s_count].d = malloc(n ? n : 1);
    memcpy(s_corpus[s_count].d, d, n);
    s_corpus[s_count].n = n;
    s_count++;
}

static unsigned merge_coverage(void)
{
    unsigned fresh = 0;
    for (size_t i = 0; i < sizeof(s_map); i++)
        if (s_map[i] && !s_seen[i]) { s_seen[i] = 1; fresh++; }
    return fresh;
}

static size_t mutate(const input_t *in, uint8_t *out, size_t cap)
{
    size_t n = in->n < cap ? in->n : cap;
    memcpy(out, in->d, n);
    for (int r = 0, rounds = 1 + (int)(rnd() % 8); r < rounds; r++) {
        size_t at = n ? rnd() % n : 0;
        switch (rnd() % 6) {
        case 0: if (n) out[at] ^= (uint8_t)(1u << (rnd() % 8)); break;
        case 1: if (n) out[at] = (uint8_t)rnd(); break;
        case 2: if (n + 8 <= cap) { memmove(out + at + 8, out + at, n - at); for (int i = 0; i < 8; i++) out[at + i] = (uint8_t)rnd(); n += 8; } break;
        case 3: { size_t len = 1 + rnd() % 16; if (at + len > n) len = n - at; memmove(out + at, out + at + len, n - at - len); n -= len; break; }
        case 4: { const input_t *o = &s_corpus[rnd() % s_count]; size_t len = o->n < cap - at ? o->n : cap - at; memcpy(out + at, o->d, len); if (at + len > n) n = at + len; break; }
        default: if (n + 1 <= cap) out[n++] = (uint8_t)rnd(); break;
        }
    }
    return n;
}

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: fuzz_media_track <seconds> <crash file> [seeds...]\n"); return 2; }
    double seconds = atof(argv[1]);
    s_crash_path = argv[2];
    signal(SIGABRT, on_abort);
    for (int i = 3; i < argc; i++) {
        FILE *f = fopen(argv[i], "rb");
        if (!f) continue;
        uint8_t buf[1 << 16];
        size_t n = fread(buf, 1, sizeof(buf), f);
        fclose(f);
        run(buf, n);
        merge_coverage();
        corpus_add(buf, n);
    }
    // Seeds of our own: an init and runs of frames, several shapes.
    for (int s = 0; s < 16; s++) {
        uint8_t seed[512];
        for (size_t i = 0; i < sizeof(seed); i++) seed[i] = (uint8_t)rnd();
        seed[0] = 0;                                  // an init segment first
        run(seed, sizeof(seed));
        merge_coverage();
        corpus_add(seed, sizeof(seed));
    }
    static uint8_t buf[1 << 16];
    unsigned long runs = 0, kept = 0;
    struct timespec t0, t;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    double elapsed = 0, last = 0;
    while (elapsed < seconds) {
        size_t n = mutate(&s_corpus[rnd() % s_count], buf, sizeof(buf));
        run(buf, n);
        runs++;
        if (merge_coverage() > 0 && n < 8192) { corpus_add(buf, n); kept++; }
        clock_gettime(CLOCK_MONOTONIC, &t);
        elapsed = (double)(t.tv_sec - t0.tv_sec) + (double)(t.tv_nsec - t0.tv_nsec) / 1e9;
        if (elapsed - last >= 60) {
            unsigned loc = 0;
            for (size_t i = 0; i < sizeof(s_seen); i++) loc += s_seen[i];
            printf("fuzz: %.0f s, %lu runs, %u operations, corpus %zu, %u code locations\n", elapsed, runs, s_ops, s_count, loc);
            fflush(stdout);
            last = elapsed;
        }
    }
    unsigned loc = 0;
    for (size_t i = 0; i < sizeof(s_seen); i++) loc += s_seen[i];
    printf("fuzz: done: %.0f s, %lu runs, %u operations, corpus %zu (%lu kept for new coverage), %u code locations, no report\n",
           elapsed, runs, s_ops, s_count, kept, loc);
    return 0;
}
