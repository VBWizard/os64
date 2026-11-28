// gplay --fed: fed.h says what it does.
#include "fed.h"
#include "os64/lock.h"
#include "os64/os64.h"
#include "os64/thread.h"

#define RATE 48000
#define AHEAD_US 4000000                // fragments are appended this far past the position
#define FRAGMENTS 16

typedef struct {
    uint8_t *data;
    int64_t size;
    int64_t init[2];
    int count;
    int64_t frag[FRAGMENTS][2];         // offset, length
    double start[FRAGMENTS], duration[FRAGMENTS];
} rendition_t;

struct gplay_fed {
    rendition_t pic[2], sound;          // pic[0] the largest found, pic[1] the smallest
    os64_media_fed_t *fed;
    os64_media_fed_buffer_t *v, *a;
    int current;                        // the picture rendition appended now
    bool appended_v[FRAGMENTS], appended_a[FRAGMENTS];
    bool ended;
    int64_t thread;
    os64_lock_t lock;                   // what main and the feeder share, below
    int64_t position;                   // output frames
    bool quit, want_switch, want_evict;
    os64_media_fed_counts_t counts;
    uint32_t appends, switches, removes;
    bool end_said;                      // the feeder's `ended`, for the readout
};

static uint8_t *slurp(const char *path, int64_t *size)
{
    int64_t h = os64_open(path, "r");
    if (h < 0) return 0;
    int64_t n = os64_seek((int32_t)h, 0, OS64_SEEK_END);
    os64_seek((int32_t)h, 0, OS64_SEEK_SET);
    uint8_t *d = n > 0 ? os64_malloc((size_t)n + 1) : 0;
    int64_t got = d ? os64_read((int32_t)h, d, (size_t)n) : -1;
    os64_close((int32_t)h);
    if (got != n) {
        os64_free(d);
        return 0;
    }
    d[n] = 0;
    *size = n;
    return d;
}

// The next number in `p` (digits, a fraction kept), and where it ended.
static const char *number(const char *p, double *out)
{
    while (*p && !(*p >= '0' && *p <= '9')) p++;
    if (!*p) return 0;
    double v = 0, scale = 1;
    while (*p >= '0' && *p <= '9') v = v * 10 + (*p++ - '0');
    if (*p == '.')
        for (p++; *p >= '0' && *p <= '9'; p++) v += (*p - '0') * (scale /= 10);
    *out = v;
    return p;
}

static const char *find(const char *text, const char *key)
{
    size_t n = os64_strlen(key);
    for (; *text; text++)
        if (os64_memcmp(text, key, n) == 0) return text;
    return 0;
}

static bool load(const char *prefix, const char *suffix, rendition_t *r)
{
    char path[160];
    os64_snprintf(path, sizeof(path), "%s-%s.mp4", prefix, suffix);
    r->data = slurp(path, &r->size);
    os64_snprintf(path, sizeof(path), "%s-%s.json", prefix, suffix);
    int64_t n;
    char *j = (char *)slurp(path, &n);
    const char *p = j ? find(j, "\"init\"") : 0;
    double v[4];
    bool ok = r->data && p && (p = number(p, &v[0])) && (p = number(p, &v[1]));
    if (ok) {
        r->init[0] = (int64_t)v[0];
        r->init[1] = (int64_t)v[1];
        p = find(j, "\"fragments\"");
        r->count = 0;
        while (p && r->count < FRAGMENTS && (p = number(p, &v[0])) && (p = number(p, &v[1])) &&
               (p = number(p, &v[2])) && (p = number(p, &v[3]))) {
            r->frag[r->count][0] = (int64_t)v[0];
            r->frag[r->count][1] = (int64_t)v[1];
            r->start[r->count] = v[2];
            r->duration[r->count] = v[3];
            r->count++;
        }
        ok = r->count > 0;
    }
    os64_free(j);
    return ok;
}

static bool append(gplay_fed_t *g, os64_media_fed_buffer_t *b, const rendition_t *r, int64_t off, int64_t len)
{
    if (!os64_media_fed_take(g->fed, b, r->data + off, (size_t)len)) return false;
    os64_media_fed_result_t res;
    os64_media_fed_parse(g->fed, b, &res);
    if (res.status != OS64_MEDIA_FED_DONE) os64_printf("gplay: fed: %s\n", res.message);
    g->appends++;
    return res.status == OS64_MEDIA_FED_DONE;
}

gplay_fed_t *gplay_fed_open(const char *prefix, char *why, size_t cap)
{
    gplay_fed_t *g = os64_calloc(1, sizeof(*g));
    static const char *const sizes[] = { "1080p", "720p", "480p" };
    int found = 0;
    for (int i = 0; i < 3 && g; i++) {
        rendition_t r = { 0 };
        if (!load(prefix, sizes[i], &r)) continue;
        if (found == 0) g->pic[0] = r;
        else {
            os64_free(g->pic[1].data);
            g->pic[1] = r;
        }
        found++;
    }
    if (found == 1) g->pic[1] = g->pic[0];      // one size: a switch appends it again
    if (!g || found == 0 || !load(prefix, "a48", &g->sound)) {
        os64_snprintf(why, cap, "%s: no renditions (%s-<size>.mp4, %s-a48.mp4 and their .json)", prefix, prefix,
                      prefix);
        return 0;                       // a failed open's memory goes with the program
    }
    if (os64_media_fed_create(&g->fed) != 0 ||
        !(g->v = os64_media_fed_add(g->fed, "video/mp4; codecs=\"avc1.640028\"")) ||
        !(g->a = os64_media_fed_add(g->fed, "audio/mp4; codecs=\"mp4a.40.2\""))) {
        os64_snprintf(why, cap, "the fed media would not open");
        return 0;
    }
    const rendition_t *p = &g->pic[0];
    os64_media_fed_duration(g->fed, p->start[p->count - 1] + p->duration[p->count - 1]);
    if (!append(g, g->v, p, p->init[0], p->init[1]) || !append(g, g->a, &g->sound, g->sound.init[0], g->sound.init[1]) ||
        !append(g, g->v, p, p->frag[0][0], p->frag[0][1]) ||
        !append(g, g->a, &g->sound, g->sound.frag[0][0], g->sound.frag[0][1])) {
        os64_snprintf(why, cap, "the first fragments would not append");
        return 0;
    }
    g->appended_v[0] = g->appended_a[0] = true;
    return g;
}

os64_media_fed_t *gplay_fed_media(gplay_fed_t *g)
{
    return g->fed;
}

// The fragment of `r` holding `seconds` (the last for a time past them).
static int fragment_at(const rendition_t *r, double seconds)
{
    for (int i = 0; i < r->count; i++)
        if (seconds < r->start[i] + r->duration[i]) return i;
    return r->count - 1;
}

static int64_t feeder(void *arg)
{
    gplay_fed_t *g = arg;
    for (;;) {
        os64_lock_acquire(&g->lock);
        bool quit = g->quit, sw = g->want_switch, ev = g->want_evict;
        g->want_switch = g->want_evict = false;
        double at = (double)g->position / RATE;
        os64_lock_release(&g->lock);
        if (quit) break;
        if (sw) {
            // The other picture rendition from the next fragment on: its
            // initialization segment first (a new configuration), and the
            // fragments not yet appended in it.
            g->current ^= 1;
            const rendition_t *p = &g->pic[g->current];
            (void)append(g, g->v, p, p->init[0], p->init[1]);
            for (int i = fragment_at(p, at) + 1; i < p->count; i++) g->appended_v[i] = false;
            g->switches++;
            g->ended = false;           // the append reopened an ended source: the end is said again
        }
        if (ev && at > 1.0) {
            os64_media_fed_result_t res;
            os64_media_fed_remove(g->fed, g->v, 0, at - 1.0, &res);
            os64_media_fed_remove(g->fed, g->a, 0, at - 1.0, &res);
            for (int i = 0; i < FRAGMENTS; i++) {
                if (i < g->pic[0].count && g->pic[0].start[i] + g->pic[0].duration[i] <= at - 1.0) g->appended_v[i] = false;
                if (i < g->sound.count && g->sound.start[i] + g->sound.duration[i] <= at - 1.0) g->appended_a[i] = false;
            }
            g->removes++;
            g->ended = false;           // a removal reopens an ended source: the end is said again
        }
        // What the next four seconds need, from where the position is.
        const rendition_t *p = &g->pic[g->current];
        double until = at + AHEAD_US / 1e6;
        for (int i = fragment_at(p, at); i < p->count && p->start[i] < until; i++)
            if (!g->appended_v[i]) g->appended_v[i] = append(g, g->v, p, p->frag[i][0], p->frag[i][1]);
        for (int i = fragment_at(&g->sound, at); i < g->sound.count && g->sound.start[i] < until; i++)
            if (!g->appended_a[i])
                g->appended_a[i] = append(g, g->a, &g->sound, g->sound.frag[i][0], g->sound.frag[i][1]);
        // Everything from the position on appended (what was removed behind
        // it is never appended again): then the end, as a page says it.
        bool all = true;
        for (int i = fragment_at(p, at); i < p->count; i++) all &= g->appended_v[i];
        for (int i = fragment_at(&g->sound, at); i < g->sound.count; i++) all &= g->appended_a[i];
        if (all && !g->ended) os64_media_fed_end(g->fed);
        g->ended = all;
        os64_media_fed_counts_t c;
        os64_media_fed_counts(g->fed, &c);
        os64_lock_acquire(&g->lock);
        g->counts = c;
        g->end_said = g->ended;
        os64_lock_release(&g->lock);
        os64_sleep(50);
    }
    return 0;
}

bool gplay_fed_start(gplay_fed_t *g)
{
    g->thread = os64_thread_named(feeder, g, "feed");
    return g->thread > 0;
}

void gplay_fed_position(gplay_fed_t *g, int64_t position)
{
    os64_lock_acquire(&g->lock);
    g->position = position < 0 ? 0 : position;
    os64_lock_release(&g->lock);
}

void gplay_fed_switch(gplay_fed_t *g)
{
    os64_lock_acquire(&g->lock);
    g->want_switch = true;
    os64_lock_release(&g->lock);
}

void gplay_fed_evict(gplay_fed_t *g)
{
    os64_lock_acquire(&g->lock);
    g->want_evict = true;
    os64_lock_release(&g->lock);
}

void gplay_fed_line(gplay_fed_t *g, char *line, size_t cap)
{
    os64_lock_acquire(&g->lock);
    os64_media_fed_counts_t c = g->counts;
    uint32_t appends = g->appends, switches = g->switches, removes = g->removes;
    int current = g->current;
    bool end = g->end_said;
    int64_t at = g->position;
    os64_lock_release(&g->lock);
    os64_snprintf(line, cap, "fed %s: appends %u sw %u rm %u%s; reopens %lu resyncs %lu; %lu KB; at %ld ms",
                  current ? "small" : "large", appends, switches, removes, end ? ", end" : "", (unsigned long)c.reopens,
                  (unsigned long)c.resyncs, (unsigned long)(c.slab_bytes / 1024), (long)(at * 1000 / RATE));
}

void gplay_fed_close(gplay_fed_t *g)
{
    if (g == 0) return;
    os64_lock_acquire(&g->lock);
    g->quit = true;
    os64_lock_release(&g->lock);
    if (g->thread > 0) os64_thread_join((int32_t)g->thread, 0);
    os64_media_fed_close(g->fed);
    if (g->pic[1].data != g->pic[0].data) os64_free(g->pic[1].data);
    os64_free(g->pic[0].data);
    os64_free(g->sound.data);
    os64_free(g);
}
