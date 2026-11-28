// A coverage-guided fuzzer for libmedia's fragment parser
// (userland/libmedia/media_fragment.c), built with gcc alone: the parser is
// compiled with -fsanitize-coverage=trace-pc, whose callback below marks
// each code location reached in a bitmap, and an input that reaches a new
// one joins the corpus. Every run is under ASan and UBSan; on a report the
// input in hand is written to the crash file first (ASan's error hook, and
// SIGABRT for UBSan's abort). Each input is fed in random pieces, and the
// sink checks what the header promises: a frame lies inside its slab, wholly
// filled, and every block is given back once its frames are dropped. Now and
// then the sink resets the parser from inside a callback, as the header lets
// it.
//
//   fuzz_media_fragment <seconds> <crash file> <seed files...>
//   (tools/fuzz_media_fragment_host.sh builds it, makes the seeds, runs it)

#define _POSIX_C_SOURCE 200809L   // clock_gettime
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "os64/media_fragment.h"

// ── Coverage ───────────────────────────────────────────────────────────────

#define MAP_BITS 16
static uint8_t s_map[1 << MAP_BITS];       // locations seen by this run
static uint8_t s_seen[1 << MAP_BITS];      // by any run

void __sanitizer_cov_trace_pc(void)
{
    uintptr_t pc = (uintptr_t)__builtin_return_address(0);
    s_map[(pc ^ (pc >> MAP_BITS)) & ((1u << MAP_BITS) - 1)] = 1;
}

// ── The input in hand, for the crash file ──────────────────────────────────

static const char *s_crash_path;
static const uint8_t *s_current;
static size_t s_current_size;

static void dump_current(void)
{
    if (s_crash_path == 0 || s_current == 0)
        return;
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

// ── The parser's world ─────────────────────────────────────────────────────

static long s_live;
static void *f_alloc(void *c, size_t n) { (void)c; s_live++; return malloc(n); }
static void f_release(void *c, void *b) { (void)c; s_live--; free(b); }

typedef struct {
    os64_media_fragment_slab_t *held[4096];
    int n;
    unsigned frames, inits;
    os64_media_fragment_t *p;
} sink_t;

static uint64_t rnd(void);

static void fail(const char *why)
{
    fprintf(stderr, "fuzz: invariant broken: %s\n", why);
    dump_current();
    abort();
}

static void on_init(void *c, const os64_media_fragment_init_t *init)
{
    sink_t *s = c;
    s->inits++;
    if (init->track_count == 0 || init->track_count > OS64_MEDIA_FRAGMENT_MAX_TRACKS)
        fail("an init with no tracks or too many");
    for (uint32_t i = 0; i < init->track_count; i++) {
        const os64_media_fragment_track_t *t = &init->track[i];
        if (t->entry_count == 0 || t->entry_count > OS64_MEDIA_FRAGMENT_MAX_ENTRIES)
            fail("a track with no entries or too many");
        for (uint32_t e = 0; e < t->entry_count; e++) {
            volatile uint8_t sum = 0;   // every configuration byte is readable
            for (uint32_t k = 0; k < t->entry[e].config_size; k++)
                sum ^= t->entry[e].config[k];
            (void)sum;
        }
    }
}

static void on_frame(void *c, const os64_media_fragment_frame_t *f)
{
    sink_t *s = c;
    s->frames++;
    if (f->offset > f->slab->size || f->size > f->slab->size - f->offset)
        fail("a frame outside its slab");
    if (f->offset + f->size > f->slab->filled)
        fail("a frame handed out before its bytes");
    volatile uint8_t sum = 0;
    for (uint32_t k = 0; k < f->size; k++)
        sum ^= f->slab->data[f->offset + k];
    (void)sum;
    if (s->n < 4096 && (s->frames & 1)) {    // keep every other frame for a while
        os64_media_fragment_slab_hold(f->slab);
        s->held[s->n++] = f->slab;
    }
    if (rnd() % 512 == 0)
        os64_media_fragment_reset(s->p);
}

static uint64_t s_rng = 0x2545F4914F6CDD1Dull;
static uint64_t rnd(void) { s_rng ^= s_rng << 13; s_rng ^= s_rng >> 7; s_rng ^= s_rng << 17; return s_rng; }

// One run: the input fed in random pieces; the rule it ended with.
static int run(const uint8_t *data, size_t size, bool small_caps)
{
    s_current = data;
    s_current_size = size;
    memset(s_map, 0, sizeof(s_map));
    sink_t sink = { .n = 0 };
    os64_media_fragment_memory_t mem = { f_alloc, f_release, 0, small_caps ? 4096 : 0, small_caps ? 65536 : 0 };
    os64_media_fragment_sink_t cb = { on_init, on_frame, &sink };
    os64_media_fragment_t p;
    long live0 = s_live;
    os64_media_fragment_init(&p, &mem, &cb);
    sink.p = &p;
    int rule = 0;
    for (size_t at = 0; at < size;) {
        size_t n = 1 + rnd() % (rnd() & 1 ? 16 : 4096);
        if (n > size - at) n = size - at;
        if (os64_media_fragment_append(&p, data + at, n) < 0) {
            rule = os64_media_fragment_error(&p);
            if (rnd() % 4 == 0) {   // sometimes carry on after a reset, as abort() lets a page
                os64_media_fragment_reset(&p);
                rule = 0;
            } else {
                break;
            }
        }
        at += n;
    }
    os64_media_fragment_destroy(&p);
    for (int i = 0; i < sink.n; i++)
        os64_media_fragment_slab_drop(sink.held[i]);
    if (s_live != live0)
        fail("blocks not given back");
    return rule;
}

// ── The corpus ─────────────────────────────────────────────────────────────

typedef struct { uint8_t *d; size_t n; } input_t;
static input_t *s_corpus;
static size_t s_count, s_capacity;

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
        if (s_map[i] && !s_seen[i]) {
            s_seen[i] = 1;
            fresh++;
        }
    return fresh;
}

// A mutation of `in` into `out` (capacity `cap`): flips, interesting values
// over 32-bit fields (box sizes, counts, offsets), inserts, deletions, splices.
static size_t mutate(const input_t *in, uint8_t *out, size_t cap)
{
    size_t n = in->n < cap ? in->n : cap;
    memcpy(out, in->d, n);
    int rounds = 1 + (int)(rnd() % 6);
    static const uint32_t interesting[] = { 0, 1, 7, 8, 9, 15, 16, 17, 0x7F, 0x80, 0xFF, 0x100, 0xFFFF, 0x10000,
                                            0x7FFFFFFF, 0x80000000u, 0xFFFFFFFFu, 0xFFFFFFFEu, 65536, 65537 };
    for (int r = 0; r < rounds && n > 0; r++) {
        size_t at = rnd() % n;
        switch (rnd() % 8) {
        case 0: out[at] ^= (uint8_t)(1u << (rnd() % 8)); break;
        case 1: out[at] = (uint8_t)rnd(); break;
        case 2:
            if (at + 4 <= n) {
                uint32_t v = interesting[rnd() % (sizeof(interesting) / sizeof(interesting[0]))];
                out[at] = (uint8_t)(v >> 24); out[at + 1] = (uint8_t)(v >> 16);
                out[at + 2] = (uint8_t)(v >> 8); out[at + 3] = (uint8_t)v;
            }
            break;
        case 3:
            if (at + 4 <= n) {      // a 32-bit field nudged by a little
                uint32_t v = (uint32_t)out[at] << 24 | out[at + 1] << 16 | out[at + 2] << 8 | out[at + 3];
                v += (uint32_t)(int32_t)((int)(rnd() % 33) - 16);
                out[at] = (uint8_t)(v >> 24); out[at + 1] = (uint8_t)(v >> 16);
                out[at + 2] = (uint8_t)(v >> 8); out[at + 3] = (uint8_t)v;
            }
            break;
        case 4: {                   // delete a span
            size_t len = 1 + rnd() % 64;
            if (at + len > n) len = n - at;
            memmove(out + at, out + at + len, n - at - len);
            n -= len;
            break;
        }
        case 5: {                   // duplicate a span
            size_t len = 1 + rnd() % 64;
            if (at + len > n) len = n - at;
            if (n + len <= cap) {
                memmove(out + at + len, out + at, n - at);
                n += len;
            }
            break;
        }
        case 6: {                   // splice the tail of another input
            const input_t *other = &s_corpus[rnd() % s_count];
            if (other->n > 0) {
                size_t from = rnd() % other->n, len = other->n - from;
                if (at + len > cap) len = cap - at;
                memcpy(out + at, other->d + from, len);
                n = at + len;
            }
            break;
        }
        default:                    // truncate
            n = at + 1;
            break;
        }
    }
    return n;
}

static uint8_t *read_file(const char *path, size_t *n)
{
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *d = malloc(len > 0 ? (size_t)len : 1);
    *n = fread(d, 1, (size_t)len, f);
    fclose(f);
    return d;
}

int main(int argc, char **argv)
{
    if (argc < 4) {
        fprintf(stderr, "usage: fuzz_media_fragment <seconds> <crash file> <seeds...>\n");
        return 2;
    }
    double seconds = atof(argv[1]);
    s_crash_path = argv[2];
    signal(SIGABRT, on_abort);
    for (int i = 3; i < argc; i++) {
        size_t n;
        uint8_t *d = read_file(argv[i], &n);
        if (!d) continue;
        run(d, n, false);
        merge_coverage();
        corpus_add(d, n);
        free(d);
    }
    if (s_count == 0) {
        fprintf(stderr, "fuzz: no seeds\n");
        return 2;
    }
    size_t cap = 1 << 20;
    uint8_t *buf = malloc(cap);
    unsigned long execs = 0, kept = 0;
    unsigned rules[OS64_MEDIA_FRAGMENT_RULE_COUNT] = { 0 };
    struct timespec t0, t;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    double last_report = 0, elapsed = 0;
    while (elapsed < seconds) {
        const input_t *parent = &s_corpus[rnd() % s_count];
        size_t n = mutate(parent, buf, cap);
        int rule = run(buf, n, rnd() % 8 == 0);
        if ((unsigned)rule < OS64_MEDIA_FRAGMENT_RULE_COUNT)
            rules[rule]++;
        execs++;
        if (merge_coverage() > 0 && n < (256u << 10)) {
            corpus_add(buf, n);
            kept++;
        }
        clock_gettime(CLOCK_MONOTONIC, &t);
        elapsed = (double)(t.tv_sec - t0.tv_sec) + (double)(t.tv_nsec - t0.tv_nsec) / 1e9;
        if (elapsed - last_report >= 60) {
            unsigned locations = 0;
            for (size_t i = 0; i < sizeof(s_seen); i++) locations += s_seen[i];
            printf("fuzz: %.0f s, %lu runs, corpus %zu (+%lu), %u code locations\n", elapsed, execs, s_count, kept, locations);
            fflush(stdout);
            last_report = elapsed;
        }
    }
    unsigned locations = 0;
    for (size_t i = 0; i < sizeof(s_seen); i++) locations += s_seen[i];
    printf("fuzz: done: %.0f s, %lu runs, corpus %zu (%lu kept for new coverage), %u code locations, no report\n",
           elapsed, execs, s_count, kept, locations);
    printf("fuzz: how the runs ended:");
    for (int i = 0; i < OS64_MEDIA_FRAGMENT_RULE_COUNT; i++)
        if (rules[i]) printf(" %s=%u", i == 0 ? "ok" : os64_media_fragment_rule_name((os64_media_fragment_rule_t)i), rules[i]);
    printf("\n");
    return 0;
}
