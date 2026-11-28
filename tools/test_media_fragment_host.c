// libmedia's fragment parser (userland/libmedia/media_fragment.c,
// os64/media_fragment.h) beside a model.
//
// THE MODEL is a box writer: a stream is built from a description of its
// tracks and frames (the init segment's tracks, edit lists, sample entries
// and mvex defaults; each media segment's trafs, truns, flags, times, sizes,
// and how its data is laid out in mdats), and the description says what the
// parser must hand back: each init segment, and each frame with its track,
// entry, times (edit list applied), duration, random access, and bytes, in
// stream order. Every stream is fed whole, in random pieces, cut inside
// every box header, and one byte at a time, and every feeding must give the
// same events. Then one case per rule (each must fail with its own rule and
// nothing else), the slab's holds, reset, and a sweep of corrupted bytes
// that must never read past what was given (ASan says so).
//
//   tools/test_media_fragment_host.sh

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "os64/media_fragment.h"

static unsigned long s_checks, s_failed;

static void check(bool ok, const char *what, ...)
{
    s_checks++;
    if (ok)
        return;
    s_failed++;
    if (s_failed <= 40) {
        va_list ap;
        va_start(ap, what);
        printf("FAIL: ");
        vprintf(what, ap);
        printf("\n");
        va_end(ap);
    }
}

// ── The caller's memory, counted ───────────────────────────────────────────

static long s_live;             // blocks the parser holds (slabs included)
static long s_fail_countdown;   // the Nth allocation from now fails (0 = never)

static void *t_alloc(void *context, size_t bytes)
{
    (void)context;
    if (s_fail_countdown > 0 && --s_fail_countdown == 0)
        return 0;
    s_live++;
    return malloc(bytes);
}

static void t_release(void *context, void *block)
{
    (void)context;
    s_live--;
    free(block);
}

// ── The box writer ─────────────────────────────────────────────────────────

typedef struct { uint8_t *d; size_t n, cap; } buf_t;

static void put(buf_t *b, const void *p, size_t n)
{
    if (b->n + n > b->cap) {
        b->cap = (b->n + n) * 2 + 64;
        b->d = realloc(b->d, b->cap);
    }
    memcpy(b->d + b->n, p, n);
    b->n += n;
}
static void u8(buf_t *b, uint64_t v) { uint8_t x = (uint8_t)v; put(b, &x, 1); }
static void u16(buf_t *b, uint64_t v) { u8(b, v >> 8); u8(b, v); }
static void u24(buf_t *b, uint64_t v) { u8(b, v >> 16); u16(b, v); }
static void u32(buf_t *b, uint64_t v) { u16(b, v >> 16); u16(b, v); }
static void u64(buf_t *b, uint64_t v) { u32(b, v >> 32); u32(b, v); }
static void cc4(buf_t *b, const char *s) { put(b, s, 4); }
static void zeros(buf_t *b, size_t n) { for (size_t i = 0; i < n; i++) u8(b, 0); }
static void patch32(buf_t *b, size_t at, uint64_t v)
{
    b->d[at] = (uint8_t)(v >> 24); b->d[at + 1] = (uint8_t)(v >> 16);
    b->d[at + 2] = (uint8_t)(v >> 8); b->d[at + 3] = (uint8_t)v;
}
static size_t box(buf_t *b, const char *type) { size_t at = b->n; u32(b, 0); cc4(b, type); return at; }
static size_t fullbox(buf_t *b, const char *type, unsigned version, unsigned flags)
{
    size_t at = box(b, type);
    u8(b, version);
    u24(b, flags);
    return at;
}
static void close_box(buf_t *b, size_t at) { patch32(b, at, b->n - at); }

// ── Descriptions ───────────────────────────────────────────────────────────

enum { K_VIDEO = 1, K_AUDIO = 2, K_TEXT = 3 };

typedef struct {
    uint32_t id;
    int kind;
    uint32_t timescale;
    int entries;                    // sample entries (default 1)
    const char *fourcc;             // default: avc1 / mp4a / tx3g
    uint8_t oti;                    // mp4a's object type (default 0x40)
    uint16_t asc;                   // the AudioSpecificConfig's two bytes (default 0x1190: AAC LC)
    int elst;                       // 0 none, 1 one edit, 2 an empty edit then one, 3 one edit in a v1 elst,
                                    // 4 one edit then another (only the first counts)
    int64_t media_time;
    uint32_t empty_duration;        // movie timescale
    uint32_t trex_entry, trex_dur, trex_size, trex_flags;
    const char *table_with_entries; // stts, stsc, stco, co64 or stsz given one entry
    int no_avcc, no_dsi;
    int short_avcc;                 // an avcC of six bytes, one short of its fixed fields
} dtrack_t;

typedef struct {
    dtrack_t t[6];
    int n;
    uint32_t movie_timescale;
    int no_mvex;
    int largesize;                  // the moov's header with a 64-bit size
    int mvhd_v1;                    // a version 1 mvhd (64-bit times)
    uint64_t mvhd_duration;         // 0 (unknown), or a duration in the movie timescale
    int mehd;                       // 0 none, 1 a v0 mehd, 2 a v1 mehd
    uint64_t mehd_duration;
} dinit_t;

typedef struct { uint32_t size, dur, flags; int32_t cto; } dsample_t;

typedef struct {
    uint32_t track_id;
    uint32_t tfhd_flags;            // beyond the defaults the writer sets itself
    uint32_t sdi, def_dur, def_size, def_flags;
    int tfdt_version, no_tfdt;
    uint64_t base_time;
    int truns;                      // the samples split into this many truns (default 1)
    uint32_t trun_fields;           // which of 0x100 0x200 0x400 0x800 each sample carries
    int trun_version;
    int first_flags_set;
    uint32_t first_flags;
    int omit_data_offset;           // the first trun has no data offset
    int64_t data_offset_skew;       // added to every trun's data offset
    dsample_t s[64];
    int n;
} dtraf_t;

typedef struct {
    dtraf_t f[4];
    int n;
    int mdats;                      // the data split into this many mdats (default 1)
    int spare_mdat;                 // one more mdat holding no frame
    int free_between;               // a free box between the moof and the first mdat
    int largesize;                  // 64-bit sizes on the moof and the mdats
    int interleave;                 // trafs' truns interleaved in the data (default-base-is-moof)
    int chained;                    // no default-base-is-moof: trafs' data one after another
    int truncate_last_mdat;         // the last mdat one byte short of its last frame
    int gap;                        // filler bytes between truns inside an mdat
    int no_traf;
} dseg_t;

// What the parser must hand back.
typedef struct {
    int kind;                       // 1 init, 2 frame
    uint32_t generation;
    // init
    uint32_t track_count;
    uint32_t ids[2], kinds[2], timescales[2], entry_counts[2];
    int64_t edits[2];
    uint32_t fourcc[2];
    uint64_t duration64;            // the presentation's (0: unknown)
    uint32_t movie_timescale;
    // frame
    uint32_t track, entry, size, duration;
    int64_t dts, pts;
    bool rap;
    uint64_t start;                 // stream position (the model's)
    uint32_t pattern;               // the bytes are pattern(seed, i)
    uint32_t sum;                   // the parser's: a checksum of the bytes it handed out
} event_t;

typedef struct { event_t *e; int n, cap; } events_t;

static void ev_add(events_t *v, const event_t *e)
{
    if (v->n == v->cap) {
        v->cap = v->cap ? v->cap * 2 : 64;
        v->e = realloc(v->e, v->cap * sizeof(event_t));
    }
    v->e[v->n++] = *e;
}

static uint8_t pattern_byte(uint32_t seed, uint32_t i) { return (uint8_t)(seed * 131u + i * 7u + (i >> 8)); }
static uint32_t pattern_sum(uint32_t seed, uint32_t size)
{
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < size; i++)
        h = (h ^ pattern_byte(seed, i)) * 16777619u;
    return h;
}

// The model's state across a stream: the tracks of the latest init.
typedef struct {
    buf_t out;
    events_t want;
    uint32_t generation;
    dinit_t init;
    uint32_t next_seed;
} model_t;

static const dtrack_t *model_track(const model_t *m, uint32_t id)
{
    for (int i = 0; i < m->init.n; i++)
        if (m->init.t[i].id == id)
            return &m->init.t[i];
    return 0;
}

static int64_t model_edit(const model_t *m, const dtrack_t *t)
{
    int64_t shift = 0;
    if (t->elst == 2 && m->init.movie_timescale != 0)   // zero only in a stream built to fail
        shift = (int64_t)((uint64_t)t->empty_duration * t->timescale / m->init.movie_timescale);
    return t->elst ? t->media_time - shift : 0;
}

static void write_avc1(buf_t *b, const char *fourcc, int no_avcc, int short_avcc)
{
    size_t e = box(b, fourcc);
    zeros(b, 6); u16(b, 1);                     // reserved, data reference index
    zeros(b, 16);
    u16(b, 640); u16(b, 360);
    u32(b, 0x00480000); u32(b, 0x00480000); u32(b, 0); u16(b, 1);
    zeros(b, 32); u16(b, 0x18); u16(b, 0xFFFF);
    if (short_avcc) {
        size_t c = box(b, "avcC");
        u8(b, 1); u8(b, 0x4D); u8(b, 0); u8(b, 0x1F); u8(b, 0xFF); u8(b, 0xE1);
        close_box(b, c);
    } else if (!no_avcc) {
        size_t c = box(b, "avcC");
        u8(b, 1); u8(b, 0x4D); u8(b, 0); u8(b, 0x1F); u8(b, 0xFF);
        u8(b, 0xE1); u16(b, 4); u32(b, 0x674D001F);
        u8(b, 1); u16(b, 2); u16(b, 0x68EE);
        close_box(b, c);
    }
    close_box(b, e);
}

static void write_mp4a(buf_t *b, uint8_t oti, uint16_t asc, int no_dsi)
{
    size_t e = box(b, "mp4a");
    zeros(b, 6); u16(b, 1);
    zeros(b, 8);
    u16(b, 2); u16(b, 16); u16(b, 0); u16(b, 0); u32(b, 48000u << 16);
    size_t s = fullbox(b, "esds", 0, 0);
    unsigned dsi = no_dsi ? 0 : 4;              // tag, length, two bytes
    u8(b, 3); u8(b, 3 + 2 + 13 + dsi + 3);      // ES_Descriptor
    u16(b, 1); u8(b, 0);
    u8(b, 4); u8(b, 13 + dsi);                  // DecoderConfigDescriptor
    u8(b, oti); u8(b, 0x15); u24(b, 0); u32(b, 128000); u32(b, 128000);
    if (!no_dsi) { u8(b, 5); u8(b, 2); u16(b, asc); }
    u8(b, 6); u8(b, 1); u8(b, 2);               // SLConfigDescriptor
    close_box(b, s);
    close_box(b, e);
}

static void write_init(model_t *m, const dinit_t *d)
{
    buf_t *b = &m->out;
    size_t f = box(b, "ftyp");
    cc4(b, "iso6"); u32(b, 0); cc4(b, "iso6"); cc4(b, "mp41");
    close_box(b, f);
    size_t moov_at = b->n;
    if (d->largesize) { u32(b, 1); cc4(b, "moov"); u64(b, 0); } else box(b, "moov");
    size_t mv = fullbox(b, "mvhd", d->mvhd_v1 ? 1 : 0, 0);
    if (d->mvhd_v1) { u64(b, 0); u64(b, 0); u32(b, d->movie_timescale); u64(b, d->mvhd_duration); }
    else { u32(b, 0); u32(b, 0); u32(b, d->movie_timescale); u32(b, d->mvhd_duration); }
    zeros(b, 80);
    close_box(b, mv);
    for (int i = 0; i < d->n; i++) {
        const dtrack_t *t = &d->t[i];
        size_t trak = box(b, "trak");
        size_t tk = fullbox(b, "tkhd", 0, 3);
        u32(b, 0); u32(b, 0); u32(b, t->id); u32(b, 0); u32(b, 0);
        zeros(b, 60);
        close_box(b, tk);
        if (t->elst) {
            size_t ed = box(b, "edts");
            int v1 = t->elst == 3;
            size_t el = fullbox(b, "elst", v1, 0);
            u32(b, t->elst == 2 || t->elst == 4 ? 2 : 1);
            if (t->elst == 2) { u32(b, t->empty_duration); u32(b, 0xFFFFFFFF); u32(b, 0x00010000); }
            if (v1) { u64(b, 0); u64(b, (uint64_t)t->media_time); } else { u32(b, 0); u32(b, (uint32_t)t->media_time); }
            u32(b, 0x00010000);
            if (t->elst == 4) { u32(b, 1000); u32(b, (uint32_t)t->media_time + 777); u32(b, 0x00010000); }
            close_box(b, el);
            close_box(b, ed);
        }
        size_t md = box(b, "mdia");
        size_t mh = fullbox(b, "mdhd", 0, 0);
        u32(b, 0); u32(b, 0); u32(b, t->timescale); u32(b, 0); u16(b, 0x55C4); u16(b, 0);
        close_box(b, mh);
        size_t h = fullbox(b, "hdlr", 0, 0);
        u32(b, 0);
        cc4(b, t->kind == K_VIDEO ? "vide" : t->kind == K_AUDIO ? "soun" : "text");
        zeros(b, 12); u8(b, 0);
        close_box(b, h);
        size_t mi = box(b, "minf");
        size_t junk = box(b, "dinf");               // a box the parser does not read
        close_box(b, junk);
        size_t st = box(b, "stbl");
        size_t sd = fullbox(b, "stsd", 0, 0);
        int entries = t->entries ? t->entries : 1;
        u32(b, entries);
        for (int e = 0; e < entries; e++) {
            if (t->fourcc && strcmp(t->fourcc, "avc1") && strcmp(t->fourcc, "avc3") && strcmp(t->fourcc, "mp4a")) {
                size_t x = box(b, t->fourcc);
                zeros(b, 8);
                close_box(b, x);
            } else if (t->kind == K_VIDEO) {
                write_avc1(b, t->fourcc ? t->fourcc : "avc1", t->no_avcc, t->short_avcc);
            } else if (t->kind == K_AUDIO) {
                write_mp4a(b, t->oti ? t->oti : 0x40, t->asc ? t->asc : 0x1190, t->no_dsi);
            } else {
                size_t x = box(b, "tx3g");
                zeros(b, 8);
                close_box(b, x);
            }
        }
        close_box(b, sd);
        static const char *const tables[] = { "stts", "stsc", "stco", "stsz" };
        for (int k = 0; k < 4; k++) {
            size_t tb = fullbox(b, tables[k], 0, 0);
            if (!strcmp(tables[k], "stsz"))
                u32(b, 0);
            bool full = t->table_with_entries && !strcmp(t->table_with_entries, tables[k]);
            u32(b, full ? 1 : 0);
            if (full) u64(b, 0);
            close_box(b, tb);
        }
        if (t->table_with_entries && !strcmp(t->table_with_entries, "co64")) {
            size_t tb = fullbox(b, "co64", 0, 0);
            u32(b, 1); u64(b, 0);
            close_box(b, tb);
        }
        close_box(b, st);
        close_box(b, mi);
        close_box(b, md);
        close_box(b, trak);
    }
    if (!d->no_mvex) {
        size_t mx = box(b, "mvex");
        if (d->mehd) {
            size_t mh = fullbox(b, "mehd", d->mehd == 2 ? 1 : 0, 0);
            if (d->mehd == 2) u64(b, d->mehd_duration); else u32(b, d->mehd_duration);
            close_box(b, mh);
        }
        for (int i = 0; i < d->n; i++) {
            const dtrack_t *t = &d->t[i];
            size_t tx = fullbox(b, "trex", 0, 0);
            u32(b, t->id); u32(b, t->trex_entry ? t->trex_entry : 1);
            u32(b, t->trex_dur); u32(b, t->trex_size); u32(b, t->trex_flags);
            close_box(b, tx);
        }
        close_box(b, mx);
    }
    if (d->largesize) {
        uint64_t size = b->n - moov_at;
        for (int i = 0; i < 8; i++) b->d[moov_at + 8 + i] = (uint8_t)(size >> (56 - 8 * i));
    } else {
        close_box(b, moov_at);
    }
    // What the parser must say about it.
    m->init = *d;
    uint64_t want_duration = d->mvhd_duration;
    if (!d->mvhd_v1 && want_duration == UINT32_MAX) want_duration = 0;
    if (d->mvhd_v1 && want_duration == UINT64_MAX) want_duration = 0;
    if (d->mehd && d->mehd_duration != 0) want_duration = d->mehd_duration;
    event_t e = { .kind = 1, .generation = ++m->generation, .duration64 = want_duration,
                  .movie_timescale = d->movie_timescale };
    for (int i = 0; i < d->n; i++) {
        const dtrack_t *t = &d->t[i];
        if (t->kind == K_TEXT)
            continue;
        uint32_t k = e.track_count++;
        e.ids[k] = t->id;
        e.kinds[k] = t->kind;
        e.timescales[k] = t->timescale;
        e.entry_counts[k] = t->entries ? t->entries : 1;
        e.edits[k] = model_edit(m, t);
        const char *fc = t->fourcc ? t->fourcc : t->kind == K_VIDEO ? "avc1" : "mp4a";
        e.fourcc[k] = (uint32_t)fc[0] << 24 | (uint32_t)fc[1] << 16 | (uint32_t)fc[2] << 8 | (uint32_t)fc[3];
    }
    ev_add(&m->want, &e);
}

// A sample's effective fields: per sample if its trun carries them, else the
// trun's first flags, else tfhd's defaults, else trex's.
static uint32_t eff_dur(const model_t *m, const dtraf_t *f, const dsample_t *s)
{
    if (f->trun_fields & 0x100) return s->dur;
    if (f->tfhd_flags & 0x8) return f->def_dur;
    const dtrack_t *t = model_track(m, f->track_id);
    return t ? t->trex_dur : 0;
}
static uint32_t eff_size(const model_t *m, const dtraf_t *f, const dsample_t *s)
{
    if (f->trun_fields & 0x200) return s->size;
    if (f->tfhd_flags & 0x10) return f->def_size;
    const dtrack_t *t = model_track(m, f->track_id);
    return t ? t->trex_size : 0;
}
static uint32_t eff_flags(const model_t *m, const dtraf_t *f, const dsample_t *s, int index_in_trun)
{
    if (f->trun_fields & 0x400) return s->flags;
    if (index_in_trun == 0 && f->first_flags_set) return f->first_flags;
    if (f->tfhd_flags & 0x20) return f->def_flags;
    const dtrack_t *t = model_track(m, f->track_id);
    return t ? t->trex_flags : 0;
}

typedef struct { int traf, first, count; } run_t;   // a trun: samples [first, first+count) of a traf

static void write_segment(model_t *m, const dseg_t *d)
{
    buf_t *b = &m->out;
    size_t seg_start = b->n;
    // The truns, and the order their data lies in.
    run_t runs[64];
    int nruns = 0;
    int per_traf_runs[4] = { 0 };
    for (int t = 0; t < d->n; t++) {
        const dtraf_t *f = &d->f[t];
        int k = f->truns ? f->truns : 1;
        per_traf_runs[t] = k;
        (void)k;
    }
    if (d->interleave) {
        for (int r = 0;; r++) {
            int added = 0;
            for (int t = 0; t < d->n; t++) {
                const dtraf_t *f = &d->f[t];
                int k = per_traf_runs[t];
                if (r >= k) continue;
                int per = (f->n + k - 1) / k;
                int first = r * per, count = f->n - first < per ? f->n - first : per;
                if (count <= 0) continue;
                runs[nruns++] = (run_t){ t, first, count };
                added++;
            }
            if (!added) break;
        }
    } else {
        for (int t = 0; t < d->n; t++) {
            const dtraf_t *f = &d->f[t];
            int k = per_traf_runs[t];
            int per = f->n ? (f->n + k - 1) / k : 0;
            for (int r = 0; r < k; r++) {
                int first = r * per, count = f->n - first < per ? f->n - first : per;
                if (count < 0) count = 0;
                runs[nruns++] = (run_t){ t, first, count };
            }
        }
    }
    // The moof, its data offsets written as 0 and patched once the layout is known.
    size_t moof_at = b->n;
    if (d->largesize) { u32(b, 1); cc4(b, "moof"); u64(b, 0); } else box(b, "moof");
    size_t mf = fullbox(b, "mfhd", 0, 0);
    u32(b, 1);
    close_box(b, mf);
    size_t offset_at[64];
    for (int i = 0; i < 64; i++) offset_at[i] = 0;
    for (int t = 0; t < (d->no_traf ? 0 : d->n); t++) {
        const dtraf_t *f = &d->f[t];
        size_t tf = box(b, "traf");
        uint32_t fl = f->tfhd_flags | (d->chained ? 0 : 0x020000u);
        size_t th = fullbox(b, "tfhd", 0, fl);
        u32(b, f->track_id);
        if (fl & 0x1) u64(b, 0);
        if (fl & 0x2) u32(b, f->sdi);
        if (fl & 0x8) u32(b, f->def_dur);
        if (fl & 0x10) u32(b, f->def_size);
        if (fl & 0x20) u32(b, f->def_flags);
        close_box(b, th);
        size_t unknown = box(b, "sdtp");            // inside a box it reads, one it does not
        close_box(b, unknown);
        if (!f->no_tfdt) {
            size_t td = fullbox(b, "tfdt", f->tfdt_version, 0);
            if (f->tfdt_version == 1) u64(b, f->base_time); else u32(b, f->base_time);
            close_box(b, td);
        }
        int trun_index = 0;
        for (int r = 0; r < nruns; r++) {
            if (runs[r].traf != t) continue;
            uint32_t tfl = f->trun_fields | (f->first_flags_set ? 0x4u : 0);
            bool with_offset = !(trun_index == 0 && f->omit_data_offset);
            if (with_offset) tfl |= 0x1;
            size_t tr = fullbox(b, "trun", f->trun_version, tfl);
            u32(b, runs[r].count);
            if (with_offset) { offset_at[r] = b->n; u32(b, 0); }
            if (f->first_flags_set) u32(b, f->first_flags);
            for (int i = 0; i < runs[r].count; i++) {
                const dsample_t *s = &f->s[runs[r].first + i];
                if (f->trun_fields & 0x100) u32(b, s->dur);
                if (f->trun_fields & 0x200) u32(b, s->size);
                if (f->trun_fields & 0x400) u32(b, s->flags);
                if (f->trun_fields & 0x800) u32(b, (uint32_t)s->cto);
            }
            close_box(b, tr);
            trun_index++;
        }
        close_box(b, tf);
    }
    if (d->largesize) {
        uint64_t size = b->n - moof_at;
        for (int i = 0; i < 8; i++) b->d[moof_at + 8 + i] = (uint8_t)(size >> (56 - 8 * i));
    } else {
        close_box(b, moof_at);
    }
    size_t moof_size = b->n - moof_at;
    // The data: each run's samples in run order, split over mdats.
    int total = 0;
    for (int r = 0; r < nruns; r++) total += runs[r].count;
    int mdats = d->mdats ? d->mdats : 1;
    int per_mdat = total ? (total + mdats - 1) / mdats : 0;
    uint64_t pos = moof_size + (d->free_between ? 8 : 0);   // from the moof's first byte
    uint64_t run_pos[64], sample_pos[4][64];
    int placed = 0;
    unsigned head = d->largesize ? 16 : 8;
    // Where a new mdat begins: only where a trun does (a trun's samples are
    // contiguous), at the first trun once per_mdat samples have been placed.
    bool new_mdat[64];
    {
        int seen = 0, next_break = per_mdat;
        for (int r = 0; r < nruns; r++) {
            new_mdat[r] = r == 0 || (per_mdat > 0 && seen >= next_break && runs[r].count > 0);
            if (r > 0 && new_mdat[r])
                next_break += per_mdat;
            seen += runs[r].count;
        }
    }
    // First pass: positions.
    {
        uint64_t p = pos;
        for (int r = 0; r < nruns; r++) {
            const dtraf_t *f = &d->f[runs[r].traf];
            if (new_mdat[r])
                p += head;
            else if (r > 0)
                p += d->gap;
            for (int i = 0; i < runs[r].count; i++) {
                if (i == 0) run_pos[r] = p;
                sample_pos[runs[r].traf][runs[r].first + i] = p;
                p += eff_size(m, f, &f->s[runs[r].first + i]);
                placed++;
            }
            if (runs[r].count == 0) run_pos[r] = p;
        }
    }
    // Patch the data offsets: from the moof (default-base-is-moof, or the
    // first traf), or from where the previous traf's data ended (chained).
    {
        int64_t chain_end = 0;
        for (int t = 0; t < d->n; t++) {
            int64_t base = (!d->chained || t == 0) ? 0 : chain_end;
            for (int r = 0; r < nruns; r++) {
                if (runs[r].traf != t || offset_at[r] == 0) continue;
                patch32(b, offset_at[r], (uint64_t)((int64_t)run_pos[r] - base + d->f[t].data_offset_skew));
            }
            const dtraf_t *f = &d->f[t];
            if (f->n > 0)
                chain_end = (int64_t)sample_pos[t][f->n - 1] + eff_size(m, f, &f->s[f->n - 1]);
        }
    }
    if (d->free_between) { size_t fr = box(b, "free"); close_box(b, fr); }
    // Second pass: write the mdats, and record what the parser must hand out.
    uint32_t seeds[4][64];
    for (int t = 0; t < d->n; t++)
        for (int i = 0; i < d->f[t].n; i++)
            seeds[t][i] = m->next_seed++;
    {
        size_t mdat_at = 0;
        bool open = false;
        for (int r = 0; r < nruns; r++) {
            const dtraf_t *f = &d->f[runs[r].traf];
            if (new_mdat[r]) {
                if (open) close_box(b, mdat_at);
                mdat_at = b->n;
                if (d->largesize) { u32(b, 1); cc4(b, "mdat"); u64(b, 0); } else box(b, "mdat");
                open = true;
            } else if (r > 0) {
                for (int g = 0; g < d->gap; g++) u8(b, 0xEE);
            }
            for (int i = 0; i < runs[r].count; i++) {
                int si = runs[r].first + i;
                uint32_t size = eff_size(m, f, &f->s[si]);
                uint32_t seed = seeds[runs[r].traf][si];
                for (uint32_t k = 0; k < size; k++) {
                    uint8_t byte = pattern_byte(seed, k);
                    put(b, &byte, 1);
                }
            }
        }
        if (open) {
            if (d->truncate_last_mdat) b->n--;   // its size field then says one byte less than the trun
            if (d->largesize) {
                uint64_t size = b->n - mdat_at;
                for (int i = 0; i < 8; i++) b->d[mdat_at + 8 + i] = (uint8_t)(size >> (56 - 8 * i));
            } else {
                close_box(b, mdat_at);
            }
        }
        if (d->spare_mdat) { size_t sp = box(b, "mdat"); u32(b, 0xDEADBEEF); close_box(b, sp); }
    }
    // The frames, in stream order.
    event_t frames[256];
    int nf = 0;
    for (int t = 0; t < d->n; t++) {
        const dtraf_t *f = &d->f[t];
        const dtrack_t *tr = model_track(m, f->track_id);
        if (!tr || tr->kind == K_TEXT) continue;
        int64_t edit = model_edit(m, tr);
        int64_t time = (int64_t)f->base_time;
        int r_first = 0;
        int k = per_traf_runs[t], per = f->n ? (f->n + k - 1) / k : 0;
        for (int i = 0; i < f->n; i++) {
            int index_in_trun = per ? (i - r_first) % per : 0;
            uint32_t dur = eff_dur(m, f, &f->s[i]);
            uint32_t flags = eff_flags(m, f, &f->s[i], index_in_trun);
            int64_t cto = (f->trun_fields & 0x800) ? (int64_t)f->s[i].cto : 0;    // signed in v0 too
            uint32_t sdi = (f->tfhd_flags & 0x2) ? f->sdi : (tr->trex_entry ? tr->trex_entry : 1);
            frames[nf++] = (event_t){
                .kind = 2, .generation = m->generation, .track = f->track_id, .entry = sdi - 1,
                .size = eff_size(m, f, &f->s[i]), .duration = dur,
                // wrapping: only a stream built to overflow wraps, and it fails before its frames matter
                .dts = (int64_t)((uint64_t)time - (uint64_t)edit),
                .pts = (int64_t)((uint64_t)time + (uint64_t)cto - (uint64_t)edit),
                .rap = !(flags & 0x10000) && ((flags >> 24) & 3) != 1,
                .start = seg_start + sample_pos[t][i],
                .pattern = seeds[t][i],
            };
            time = (int64_t)((uint64_t)time + dur);
        }
    }
    // stream order; equal starts keep the order they were written in
    for (int i = 1; i < nf; i++)
        for (int j = i; j > 0 && frames[j - 1].start > frames[j].start; j--) {
            event_t x = frames[j]; frames[j] = frames[j - 1]; frames[j - 1] = x;
        }
    for (int i = 0; i < nf; i++)
        ev_add(&m->want, &frames[i]);
}

// ── Feeding ────────────────────────────────────────────────────────────────

enum { CUT_WHOLE, CUT_RANDOM, CUT_HEADERS, CUT_BYTE, CUT_COUNT };
static const char *const kCut[] = { "whole", "random pieces", "inside every header", "one byte at a time" };

typedef struct {
    events_t got;
    const uint8_t *config;          // the latest init's first configuration, as handed out
    uint8_t config_copy[64];
    uint32_t config_size;
    unsigned config_changed;        // frames that found those bytes changed under them
    unsigned early;                 // frames handed out before their last byte was in
    bool hold;                      // keep every frame's slab until the end
    os64_media_fragment_slab_t **held;
    int nheld, capheld;
    os64_media_fragment_t *p;
    int reenter_at;                 // after this many events the sink re-enters the parser (0 = never)
    bool reenter_destroys;          // ... by destroying it, not resetting it
} sink_ctx_t;

static void maybe_reenter(sink_ctx_t *c)
{
    if (c->reenter_at == 0 || c->got.n != c->reenter_at)
        return;
    if (c->reenter_destroys)
        os64_media_fragment_destroy(c->p);
    else
        os64_media_fragment_reset(c->p);
}

static void on_init(void *context, const os64_media_fragment_init_t *init)
{
    sink_ctx_t *c = context;
    event_t e = { .kind = 1, .generation = init->generation, .track_count = init->track_count,
                  .duration64 = init->duration, .movie_timescale = init->movie_timescale };
    for (uint32_t i = 0; i < init->track_count && i < 2; i++) {
        e.ids[i] = init->track[i].id;
        e.kinds[i] = (uint32_t)init->track[i].kind;
        e.timescales[i] = init->track[i].timescale;
        e.entry_counts[i] = init->track[i].entry_count;
        e.edits[i] = init->track[i].edit_offset;
        e.fourcc[i] = init->track[i].entry[0].fourcc;
    }
    // An init's configurations must stay good until the next init.
    c->config = init->track[0].entry[0].config;
    c->config_size = init->track[0].entry[0].config_size < 64 ? init->track[0].entry[0].config_size : 64;
    memcpy(c->config_copy, c->config, c->config_size);
    ev_add(&c->got, &e);
    maybe_reenter(c);
}

static void on_frame(void *context, const os64_media_fragment_frame_t *f)
{
    sink_ctx_t *c = context;
    if (c->config && memcmp(c->config, c->config_copy, c->config_size) != 0)
        c->config_changed++;
    if (f->offset + f->size > f->slab->filled)
        c->early++;
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < f->size; i++)
        h = (h ^ f->slab->data[f->offset + i]) * 16777619u;
    event_t e = { .kind = 2, .generation = f->generation, .track = f->track_id, .entry = f->entry,
                  .size = f->size, .duration = f->duration, .dts = f->dts, .pts = f->pts,
                  .rap = f->rap, .sum = h };
    ev_add(&c->got, &e);
    if (c->hold) {
        if (c->nheld == c->capheld) {
            c->capheld = c->capheld ? c->capheld * 2 : 64;
            c->held = realloc(c->held, c->capheld * sizeof(*c->held));
        }
        os64_media_fragment_slab_hold(f->slab);
        c->held[c->nheld++] = f->slab;
    }
    maybe_reenter(c);
}

static const os64_media_fragment_memory_t kMemory = { t_alloc, t_release, 0, 0, 0 };

// Every top-level box boundary in `b`, for the cut inside every header.
static size_t next_cut(const buf_t *b, size_t at, int cut, uint64_t *rng)
{
    size_t left = b->n - at;
    if (cut == CUT_WHOLE) return left;
    if (cut == CUT_BYTE) return 1;
    if (cut == CUT_RANDOM) {
        *rng ^= *rng << 13; *rng ^= *rng >> 7; *rng ^= *rng << 17;
        size_t n = 1 + (size_t)(*rng % 3000);
        return n < left ? n : left;
    }
    // inside every header: stop 3 and 11 bytes past each top-level box's start
    size_t pos = 0;
    while (pos + 8 <= b->n) {
        uint64_t size = (uint64_t)b->d[pos] << 24 | b->d[pos + 1] << 16 | b->d[pos + 2] << 8 | b->d[pos + 3];
        if (size == 1 && pos + 16 <= b->n) {
            size = 0;
            for (int i = 0; i < 8; i++) size = size << 8 | b->d[pos + 8 + i];
        }
        for (size_t c = pos + 3; c <= pos + 11; c += 8)
            if (c > at && c < b->n) return c - at;
        if (size < 8) break;
        pos += size;
    }
    return left;
}

// Feed `b` under a cut; answer the rule it ended with (OK if every append
// took its bytes) and leave the events in `c`.
static os64_media_fragment_rule_t feed(const buf_t *b, int cut, sink_ctx_t *c, os64_media_fragment_memory_t mem)
{
    os64_media_fragment_t p;
    os64_media_fragment_sink_t sink = { on_init, on_frame, c };
    os64_media_fragment_init(&p, &mem, &sink);
    c->p = &p;
    uint64_t rng = 0x9E3779B97F4A7C15ull;
    os64_media_fragment_rule_t rule = OS64_MEDIA_FRAGMENT_OK;
    for (size_t at = 0; at < b->n;) {
        size_t n = next_cut(b, at, cut, &rng);
        if (os64_media_fragment_append(&p, b->d + at, n) < 0) {
            rule = os64_media_fragment_error(&p);
            break;
        }
        at += n;
    }
    os64_media_fragment_destroy(&p);
    c->p = 0;
    return rule;
}

static void release_held(sink_ctx_t *c)
{
    for (int i = 0; i < c->nheld; i++)
        os64_media_fragment_slab_drop(c->held[i]);
    c->nheld = 0;
}

static bool same_event(const event_t *a, const event_t *b)
{
    if (a->kind != b->kind || a->generation != b->generation)
        return false;
    if (a->kind == 1) {
        if (a->track_count != b->track_count || a->duration64 != b->duration64 ||
            a->movie_timescale != b->movie_timescale) return false;
        for (uint32_t i = 0; i < a->track_count; i++)
            if (a->ids[i] != b->ids[i] || a->kinds[i] != b->kinds[i] || a->timescales[i] != b->timescales[i] ||
                a->entry_counts[i] != b->entry_counts[i] || a->edits[i] != b->edits[i] || a->fourcc[i] != b->fourcc[i])
                return false;
        return true;
    }
    return a->track == b->track && a->entry == b->entry && a->size == b->size && a->duration == b->duration &&
           a->dts == b->dts && a->pts == b->pts && a->rap == b->rap;
}

// The stream in `m` must give exactly `m->want`, under every cut, with the
// memory all given back at the end whether or not frames were kept.
static void expect_stream_with(const char *name, model_t *m, os64_media_fragment_memory_t mem)
{
    for (int cut = 0; cut < CUT_COUNT; cut++)
        for (int hold = 0; hold < 2; hold++) {
            sink_ctx_t c = { .hold = hold };
            long live0 = s_live;
            os64_media_fragment_rule_t rule = feed(&m->out, cut, &c, mem);
            check(rule == OS64_MEDIA_FRAGMENT_OK, "%s, %s: failed with \"%s\"", name, kCut[cut],
                  os64_media_fragment_rule_name(rule));
            check(c.got.n == m->want.n, "%s, %s: %d events, want %d", name, kCut[cut], c.got.n, m->want.n);
            check(c.config_changed == 0, "%s, %s: an init's configuration changed under %u frames", name,
                  kCut[cut], c.config_changed);
            check(c.early == 0, "%s, %s: %u frames handed out before their last byte was in", name, kCut[cut], c.early);
            for (int i = 0; i < c.got.n && i < m->want.n; i++) {
                const event_t *g = &c.got.e[i], *w = &m->want.e[i];
                bool ok = same_event(g, w) && (w->kind != 2 || g->sum == pattern_sum(w->pattern, w->size));
                check(ok, "%s, %s: event %d differs (kind %d/%d gen %u/%u track %u/%u entry %u/%u size %u/%u dur %u/%u dts %lld/%lld pts %lld/%lld rap %d/%d)",
                      name, kCut[cut], i, g->kind, w->kind, g->generation, w->generation, g->track, w->track,
                      g->entry, w->entry, g->size, w->size, g->duration, w->duration, (long long)g->dts,
                      (long long)w->dts, (long long)g->pts, (long long)w->pts, g->rap, w->rap);
                if (!ok) break;
            }
            // Kept frames keep their slabs; each drop frees one at its last hold.
            if (hold)
                check(s_live - live0 >= 0, "%s: slabs released while held", name);
            release_held(&c);
            check(s_live == live0, "%s, %s, %s frames: %ld blocks not given back", name, kCut[cut],
                  hold ? "kept" : "unkept", s_live - live0);
            free(c.got.e);
            free(c.held);
        }
}

static void expect_stream(const char *name, model_t *m) { expect_stream_with(name, m, kMemory); }

static os64_media_fragment_rule_t expect_rule(const char *name, model_t *m, os64_media_fragment_rule_t want,
                                              os64_media_fragment_memory_t mem)
{
    os64_media_fragment_rule_t got = OS64_MEDIA_FRAGMENT_OK;
    for (int cut = 0; cut < CUT_COUNT; cut++) {
        sink_ctx_t c = { .hold = cut & 1 };
        long live0 = s_live;
        got = feed(&m->out, cut, &c, mem);
        check(got == want, "%s, %s: \"%s\", want \"%s\"", name, kCut[cut], os64_media_fragment_rule_name(got),
              os64_media_fragment_rule_name(want));
        release_held(&c);
        check(s_live == live0, "%s, %s: %ld blocks not given back", name, kCut[cut], s_live - live0);
        free(c.got.e);
        free(c.held);
    }
    return got;
}

// ── Streams ────────────────────────────────────────────────────────────────

static dtrack_t video(uint32_t id) { return (dtrack_t){ .id = id, .kind = K_VIDEO, .timescale = 15360 }; }
static dtrack_t audio(uint32_t id) { return (dtrack_t){ .id = id, .kind = K_AUDIO, .timescale = 48000 }; }

// A traf of n samples of every field per sample, B-frame-like offsets, sync every 4th.
static dtraf_t full_traf(uint32_t track, uint64_t base, int n, uint32_t dur)
{
    dtraf_t f = { .track_id = track, .base_time = base, .trun_fields = 0xF00, .trun_version = 1, .n = n };
    for (int i = 0; i < n; i++)
        f.s[i] = (dsample_t){ .size = 50 + (uint32_t)(i * 37 % 211), .dur = dur,
                              // a sync frame; depends-on and non-sync; depends-on alone; non-sync alone
                              .flags = i % 4 == 0 ? 0x02000000u : i % 4 == 1 ? 0x01010000u : i % 4 == 2 ? 0x01000000u : 0x00010000u,
                              .cto = (int32_t)((i % 3) * (int32_t)dur) - (int32_t)dur };
    return f;
}

static void model_free(model_t *m) { free(m->out.d); free(m->want.e); *m = (model_t){ 0 }; }

static dinit_t av_init(void)
{
    dinit_t d = { .n = 2, .movie_timescale = 1000 };
    d.t[0] = video(1);
    d.t[0].elst = 1; d.t[0].media_time = 1024;
    d.t[1] = audio(2);
    d.t[1].elst = 1; d.t[1].media_time = 1024;
    return d;
}

static void streams(void)
{
    model_t m = { 0 };

    // Picture and sound, a segment each way the data can lie.
    {
        dinit_t d = av_init();
        write_init(&m, &d);
        dseg_t s = { .n = 2 };
        s.f[0] = full_traf(1, 0, 12, 512);
        s.f[1] = full_traf(2, 0, 9, 1024);
        write_segment(&m, &s);
        s.f[0].base_time = 12 * 512; s.f[1].base_time = 9 * 1024;
        s.interleave = 1; s.f[0].truns = 3; s.f[1].truns = 3;   // data interleaved: frames out of traf order
        s.gap = 5;                                              // and filler between the truns
        write_segment(&m, &s);
        s.f[0].base_time = 24 * 512; s.f[1].base_time = 18 * 1024;
        s.interleave = 0; s.gap = 0; s.mdats = 3; s.spare_mdat = 1; s.free_between = 1;   // several mdats, a spare, a free
        write_segment(&m, &s);
        s.f[0].base_time = 36 * 512; s.f[1].base_time = 27 * 1024;
        s.mdats = 1; s.spare_mdat = 0; s.free_between = 0; s.chained = 1;      // trafs chained, no dbim
        write_segment(&m, &s);
        s.f[0].base_time = 48 * 512; s.f[1].base_time = 36 * 1024;
        s.chained = 0; s.largesize = 1;                                         // 64-bit box sizes
        write_segment(&m, &s);
        expect_stream("picture and sound", &m);
        model_free(&m);
    }

    // mvex's defaults (the trun carries nothing per sample), then tfhd's over them.
    {
        dinit_t d = { .n = 1, .movie_timescale = 1000 };
        d.t[0] = video(7);
        d.t[0].trex_dur = 1001; d.t[0].trex_size = 333; d.t[0].trex_flags = 0x01010000;
        write_init(&m, &d);
        dseg_t s = { .n = 1 };
        s.f[0] = (dtraf_t){ .track_id = 7, .n = 10, .first_flags_set = 1, .first_flags = 0x02000000, .tfdt_version = 0 };
        write_segment(&m, &s);
        s.f[0] = (dtraf_t){ .track_id = 7, .n = 6, .base_time = 10010, .tfhd_flags = 0x38,
                            .def_dur = 2002, .def_size = 99, .def_flags = 0x02000000 };
        write_segment(&m, &s);
        expect_stream("defaults", &m);
        model_free(&m);
    }

    // Several sample entries, a frame naming the second; avc3.
    {
        dinit_t d = { .n = 1, .movie_timescale = 1000 };
        d.t[0] = video(3);
        d.t[0].entries = 2; d.t[0].fourcc = "avc3";
        write_init(&m, &d);
        dseg_t s = { .n = 1 };
        s.f[0] = full_traf(3, 0, 5, 512);
        s.f[0].tfhd_flags = 0x2; s.f[0].sdi = 2;
        write_segment(&m, &s);
        expect_stream("several entries", &m);
        model_free(&m);
    }

    // The edit list's shapes: an empty edit before one, a v1 elst, none.
    {
        dinit_t d = { .n = 2, .movie_timescale = 600 };
        d.t[0] = video(1);
        d.t[0].elst = 2; d.t[0].empty_duration = 300; d.t[0].media_time = 1024;   // starts half a second later
        d.t[1] = audio(2);
        d.t[1].elst = 4; d.t[1].media_time = 2112;
        write_init(&m, &d);
        dseg_t s = { .n = 2 };
        s.f[0] = full_traf(1, 0, 4, 512);
        s.f[1] = full_traf(2, 0, 4, 1024);
        write_segment(&m, &s);
        expect_stream("edit lists", &m);
        model_free(&m);
        d.t[1].elst = 3;                    // the same edit in a v1 elst
        write_init(&m, &d);
        write_segment(&m, &s);
        expect_stream("a v1 edit list", &m);
        model_free(&m);
    }

    // A text track beside them: its trafs read past (chained, so its data
    // still moves the next traf's base), its frames never handed out.
    {
        dinit_t d = av_init();
        d.t[2] = (dtrack_t){ .id = 9, .kind = K_TEXT, .timescale = 1000 };
        d.n = 3;
        write_init(&m, &d);
        dseg_t s = { .n = 3, .chained = 1 };
        s.f[0] = full_traf(1, 0, 4, 512);
        s.f[1] = full_traf(9, 0, 2, 1000);
        s.f[2] = full_traf(2, 0, 4, 1024);
        write_segment(&m, &s);
        // Its sizes from trex alone, then from tfhd: either way its bytes
        // still move the chained traf after it.
        d.t[2].trex_size = 40;
        write_init(&m, &d);
        s.f[1].trun_fields = 0xD00;
        write_segment(&m, &s);
        s.f[1].tfhd_flags = 0x10; s.f[1].def_size = 25;
        write_segment(&m, &s);
        expect_stream("a text track", &m);
        model_free(&m);
    }

    // An empty fragment (duration-is-empty, no frames), a quality switch (a
    // second init, generation 2), and a later segment out of time order.
    {
        dinit_t d = av_init();
        write_init(&m, &d);
        dseg_t s = { .n = 1 };
        s.f[0] = (dtraf_t){ .track_id = 1, .tfhd_flags = 0x10000, .n = 0, .omit_data_offset = 0 };
        write_segment(&m, &s);
        s.f[0] = full_traf(1, 10 * 512, 6, 512);
        write_segment(&m, &s);
        d.t[0].timescale = 90000;           // the new quality's
        write_init(&m, &d);
        s.f[0] = full_traf(1, 0, 6, 3000);  // an earlier time after a later one: the parser's to hand on, not judge
        write_segment(&m, &s);
        expect_stream("switch and seek", &m);
        model_free(&m);
    }

    // Zero-size frames: in the middle of an mdat, at its very end, at its
    // very start, and an mdat holding nothing but them (four truns of two:
    // samples 1, 3, 4, 6 and 7 have no bytes), with a segment after so a
    // frame left pending would fail it.
    // The stream also ends at that segment's last mdat, so its frames must
    // go out when the mdat ends, not when a later byte arrives.
    for (int mdats = 2; mdats <= 4; mdats += 2)
        for (int follow = 0; follow < 2; follow++) {
            dinit_t d = av_init();
            write_init(&m, &d);
            dseg_t s = { .n = 1, .mdats = mdats };
            s.f[0] = full_traf(1, 0, 8, 512);
            s.f[0].truns = 4;
            s.f[0].s[1].size = s.f[0].s[3].size = s.f[0].s[4].size = s.f[0].s[6].size = s.f[0].s[7].size = 0;
            write_segment(&m, &s);
            if (follow) {
                s = (dseg_t){ .n = 1 };
                s.f[0] = full_traf(1, 8 * 512, 4, 512);
                write_segment(&m, &s);
            }
            char name[64];
            snprintf(name, sizeof(name), "%s%s", mdats == 2 ? "zero-size frames" : "an mdat of zero-size frames",
                     follow ? ", a segment after" : "");
            expect_stream(name, &m);
            model_free(&m);
        }

    // The presentation's duration: mvhd's (v0, v1), unknown (0, all ones),
    // and mehd's over mvhd's (v0, v1, and a mehd of 0 that defers to mvhd).
    {
        static const struct { int v1; uint64_t mvhd; int mehd; uint64_t mehd_d; } cases[] = {
            { 0, 90000, 0, 0 }, { 1, 1ull << 40, 0, 0 }, { 0, 0, 0, 0 }, { 0, UINT32_MAX, 0, 0 },
            { 1, UINT64_MAX, 0, 0 }, { 0, 0, 1, 123456 }, { 0, 5000, 2, 1ull << 36 }, { 0, 7000, 1, 0 },
        };
        for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
            dinit_t d = av_init();
            d.mvhd_v1 = cases[i].v1; d.mvhd_duration = cases[i].mvhd;
            d.mehd = cases[i].mehd; d.mehd_duration = cases[i].mehd_d;
            write_init(&m, &d);
            char name[48];
            snprintf(name, sizeof(name), "the duration, case %zu", i);
            expect_stream(name, &m);
            model_free(&m);
        }
    }

    // trex's sample entry, with tfhd naming none.
    {
        dinit_t d = { .n = 1, .movie_timescale = 1000 };
        d.t[0] = video(3);
        d.t[0].entries = 2; d.t[0].trex_entry = 2;
        write_init(&m, &d);
        dseg_t s = { .n = 1 };
        s.f[0] = full_traf(3, 0, 4, 512);
        write_segment(&m, &s);
        expect_stream("trex's entry", &m);
        model_free(&m);
    }

    // AAC under MPEG-2's object type indication, and HE-AAC (object type 5).
    {
        dinit_t d = av_init();
        d.t[1].oti = 0x67;
        write_init(&m, &d);
        d.t[1].oti = 0; d.t[1].asc = 0x2B90;
        write_init(&m, &d);
        dseg_t s = { .n = 1 };
        s.f[0] = full_traf(2, 0, 4, 1024);
        write_segment(&m, &s);
        expect_stream("MPEG-2 AAC, then HE-AAC", &m);
        model_free(&m);
    }

    // A v0 trun's negative composition offsets read as negative, as FFmpeg
    // and other browsers read them (ISO 14496-12 § 8.8.8 says unsigned).
    {
        dinit_t d = av_init();
        write_init(&m, &d);
        dseg_t s = { .n = 1 };
        s.f[0] = full_traf(1, 0, 6, 512);
        s.f[0].trun_version = 0;
        write_segment(&m, &s);
        expect_stream("a v0 trun's negative offsets", &m);
        model_free(&m);
    }

    // First-sample flags beside per-sample flags: the sample's own win, as
    // FFmpeg reads them (the format says a trun should carry one or the other).
    {
        dinit_t d = av_init();
        write_init(&m, &d);
        dseg_t s = { .n = 1 };
        s.f[0] = full_traf(1, 0, 6, 512);
        s.f[0].first_flags_set = 1; s.f[0].first_flags = 0x01010000;
        write_segment(&m, &s);
        expect_stream("first-sample and per-sample flags", &m);
        model_free(&m);
    }

    // An mdat over max_mdat that holds none of the moof's frames is read past.
    {
        dinit_t d = av_init();
        write_init(&m, &d);
        size_t big = box(&m.out, "mdat");
        zeros(&m.out, 1000);
        close_box(&m.out, big);
        dseg_t s = { .n = 1 };
        s.f[0] = full_traf(1, 0, 3, 512);
        write_segment(&m, &s);
        os64_media_fragment_memory_t small = kMemory;
        small.max_mdat = 600;
        expect_stream_with("an mdat over the cap holding no frame", &m, small);
        model_free(&m);
    }
}

// ── One case per rule ──────────────────────────────────────────────────────

static void rules(void)
{
    model_t m = { 0 };
    dinit_t d;
    dseg_t s;
    os64_media_fragment_memory_t mem = kMemory;

#define FRESH() do { model_free(&m); d = av_init(); s = (dseg_t){ .n = 1 }; s.f[0] = full_traf(1, 0, 6, 512); } while (0)

    FRESH();
    write_segment(&m, &s);
    expect_rule("a media segment before any init segment", &m, OS64_MEDIA_FRAGMENT_MEDIA_BEFORE_INIT, mem);

    FRESH(); d.no_mvex = 1; write_init(&m, &d);
    expect_rule("an init without mvex", &m, OS64_MEDIA_FRAGMENT_NO_MVEX, mem);

    static const char *const tables[] = { "stts", "stsc", "stco", "co64", "stsz" };
    for (int i = 0; i < 5; i++) {
        FRESH(); d.t[1].table_with_entries = tables[i]; write_init(&m, &d);
        char name[64];
        snprintf(name, sizeof(name), "an init with samples in %s", tables[i]);
        expect_rule(name, &m, OS64_MEDIA_FRAGMENT_INIT_HAS_SAMPLES, mem);
    }

    FRESH(); d.n = 1; d.t[0] = (dtrack_t){ .id = 4, .kind = K_TEXT, .timescale = 1000 }; write_init(&m, &d);
    expect_rule("an init with only a text track", &m, OS64_MEDIA_FRAGMENT_NO_TRACKS, mem);

    FRESH(); d.t[1] = video(2); write_init(&m, &d);
    expect_rule("two picture tracks", &m, OS64_MEDIA_FRAGMENT_TOO_MANY_TRACKS, mem);

    FRESH();
    d.n = 1 + OS64_MEDIA_FRAGMENT_MAX_IGNORED + 1;
    for (int i = 1; i < d.n; i++) d.t[i] = (dtrack_t){ .id = 10 + (uint32_t)i, .kind = K_TEXT, .timescale = 1000 };
    write_init(&m, &d);
    expect_rule("more tracks of other kinds than the parser reads past", &m, OS64_MEDIA_FRAGMENT_TOO_MANY_TRACKS, mem);

    FRESH(); d.t[0].short_avcc = 1; write_init(&m, &d);
    expect_rule("an avcC shorter than its fixed fields", &m, OS64_MEDIA_FRAGMENT_MALFORMED, mem);

    FRESH(); d.t[1].asc = 0xF840; write_init(&m, &d);      // object type 31 + 2 = 34: MP3 in MP4
    expect_rule("MPEG-4 Audio that is not AAC", &m, OS64_MEDIA_FRAGMENT_UNSUPPORTED_CODEC, mem);

    FRESH(); d.t[1].id = 1; write_init(&m, &d);
    expect_rule("two tracks with one id", &m, OS64_MEDIA_FRAGMENT_DUPLICATE_TRACK, mem);

    FRESH(); d.t[2] = (dtrack_t){ .id = 2, .kind = K_TEXT, .timescale = 1000 }; d.n = 3; write_init(&m, &d);
    expect_rule("a text track sharing an id", &m, OS64_MEDIA_FRAGMENT_DUPLICATE_TRACK, mem);

    FRESH(); write_init(&m, &d); d.n = 1; write_init(&m, &d);
    expect_rule("a second init without the sound track", &m, OS64_MEDIA_FRAGMENT_TRACK_COUNT_CHANGED, mem);

    FRESH(); d.t[0].fourcc = "hvc1"; write_init(&m, &d);
    expect_rule("an hvc1 picture", &m, OS64_MEDIA_FRAGMENT_UNSUPPORTED_CODEC, mem);

    FRESH(); d.t[0].fourcc = "encv"; write_init(&m, &d);
    expect_rule("an encrypted picture", &m, OS64_MEDIA_FRAGMENT_UNSUPPORTED_CODEC, mem);

    FRESH(); d.t[1].oti = 0x6B; write_init(&m, &d);
    expect_rule("MP3 in mp4a", &m, OS64_MEDIA_FRAGMENT_UNSUPPORTED_CODEC, mem);

    FRESH(); d.t[1].no_dsi = 1; write_init(&m, &d);
    expect_rule("mp4a without its AudioSpecificConfig", &m, OS64_MEDIA_FRAGMENT_UNSUPPORTED_CODEC, mem);

    FRESH(); d.t[0].fourcc = "mp4a"; write_init(&m, &d);
    expect_rule("a sound entry in a picture track", &m, OS64_MEDIA_FRAGMENT_UNSUPPORTED_CODEC, mem);

    FRESH(); d.t[0].no_avcc = 1; write_init(&m, &d);
    expect_rule("avc1 without avcC", &m, OS64_MEDIA_FRAGMENT_MALFORMED, mem);

    FRESH(); d.t[0].entries = OS64_MEDIA_FRAGMENT_MAX_ENTRIES + 1; write_init(&m, &d);
    expect_rule("too many sample entries", &m, OS64_MEDIA_FRAGMENT_TOO_MANY_ENTRIES, mem);

    FRESH(); d.t[0].timescale = 0; write_init(&m, &d);
    expect_rule("a timescale of zero", &m, OS64_MEDIA_FRAGMENT_BAD_TIMESCALE, mem);

    FRESH(); d.movie_timescale = 0; d.t[0].elst = 2; d.t[0].empty_duration = 10; write_init(&m, &d);
    expect_rule("an empty edit in a movie timescale of zero", &m, OS64_MEDIA_FRAGMENT_BAD_TIMESCALE, mem);

    FRESH(); write_init(&m, &d); s.f[0].track_id = 5; write_segment(&m, &s);
    expect_rule("a traf for an unknown track", &m, OS64_MEDIA_FRAGMENT_UNKNOWN_TRACK, mem);

    FRESH(); write_init(&m, &d); s.f[0].tfhd_flags = 0x1; write_segment(&m, &s);
    expect_rule("a base data offset", &m, OS64_MEDIA_FRAGMENT_BASE_DATA_OFFSET, mem);

    FRESH(); write_init(&m, &d); s.f[0].omit_data_offset = 1; write_segment(&m, &s);
    expect_rule("a first trun without data offset", &m, OS64_MEDIA_FRAGMENT_NO_DATA_OFFSET, mem);

    FRESH(); write_init(&m, &d); s.f[0].no_tfdt = 1; write_segment(&m, &s);
    expect_rule("a traf without tfdt", &m, OS64_MEDIA_FRAGMENT_NO_TFDT, mem);

    FRESH(); write_init(&m, &d); s.f[0].tfhd_flags = 0x2; s.f[0].sdi = 2; write_segment(&m, &s);
    expect_rule("a sample description index past the entries", &m, OS64_MEDIA_FRAGMENT_BAD_ENTRY_INDEX, mem);

    FRESH(); write_init(&m, &d); s.f[0].tfhd_flags = 0x2; s.f[0].sdi = 0; write_segment(&m, &s);
    expect_rule("a sample description index of zero", &m, OS64_MEDIA_FRAGMENT_BAD_ENTRY_INDEX, mem);

    FRESH(); write_init(&m, &d); s.f[0].tfhd_flags = 0x10000; write_segment(&m, &s);
    expect_rule("duration-is-empty with frames", &m, OS64_MEDIA_FRAGMENT_EMPTY_FRAGMENT_HAS_SAMPLES, mem);

    FRESH(); write_init(&m, &d); s.f[0].data_offset_skew = -200; write_segment(&m, &s);
    expect_rule("a frame pointing into its moof", &m, OS64_MEDIA_FRAGMENT_SAMPLE_OUTSIDE_MDAT, mem);

    FRESH(); write_init(&m, &d); s.f[0].data_offset_skew = -100000; write_segment(&m, &s);
    expect_rule("a frame before its moof", &m, OS64_MEDIA_FRAGMENT_SAMPLE_OUTSIDE_MDAT, mem);

    FRESH(); write_init(&m, &d); s.truncate_last_mdat = 1; write_segment(&m, &s);
    expect_rule("a frame past its mdat's end", &m, OS64_MEDIA_FRAGMENT_SAMPLE_OUTSIDE_MDAT, mem);

    FRESH(); write_init(&m, &d); s.f[0].data_offset_skew = 100000; write_segment(&m, &s);
    write_segment(&m, &s);
    expect_rule("frames in no mdat before the next moof", &m, OS64_MEDIA_FRAGMENT_SAMPLE_OUTSIDE_MDAT, mem);

    // No edit list on the picture: an edit's subtraction would overflow too,
    // and hide which check caught it.
    FRESH(); d.t[0].elst = 0; write_init(&m, &d); s.f[0].tfdt_version = 1; s.f[0].base_time = (uint64_t)INT64_MAX - 1000;
    write_segment(&m, &s);
    expect_rule("times past 63 bits", &m, OS64_MEDIA_FRAGMENT_TIME_OVERFLOW, mem);

    FRESH(); d.t[0].elst = 0; write_init(&m, &d); s.f[0].tfdt_version = 1; s.f[0].base_time = (uint64_t)INT64_MAX + 1;
    s.f[0].trun_fields = 0x700;             // no composition offsets either: nothing else can overflow
    write_segment(&m, &s);
    expect_rule("a tfdt past 63 bits", &m, OS64_MEDIA_FRAGMENT_TIME_OVERFLOW, mem);

    FRESH(); write_init(&m, &d); s.no_traf = 1; write_segment(&m, &s);
    expect_rule("a moof with no traf", &m, OS64_MEDIA_FRAGMENT_MALFORMED, mem);

    // The caps.
    FRESH(); write_init(&m, &d);
    {
        os64_media_fragment_memory_t small = kMemory;
        small.max_box = 64;
        expect_rule("a moov over max_box", &m, OS64_MEDIA_FRAGMENT_BOX_TOO_LARGE, small);
        write_segment(&m, &s);
        small.max_box = 0;
        small.max_mdat = 16;
        expect_rule("an mdat over max_mdat", &m, OS64_MEDIA_FRAGMENT_MDAT_TOO_LARGE, small);
    }
    model_free(&m);
#undef FRESH
}

// Hand-written streams for what the writer will not produce.
static void raw_rules(void)
{
    model_t m = { 0 };
    os64_media_fragment_memory_t mem = kMemory;

    // A top-level box smaller than its header, and one of size zero.
    for (int z = 0; z < 2; z++) {
        buf_t *b = &m.out;
        u32(b, z ? 0 : 4); cc4(b, "free"); zeros(b, 8);
        expect_rule(z ? "a top-level box of size zero" : "a box smaller than its header", &m,
                    OS64_MEDIA_FRAGMENT_BOX_SIZE, mem);
        model_free(&m);
    }
    // A child box claiming more than its parent holds.
    {
        dinit_t d = av_init();
        write_init(&m, &d);
        // the mvhd's size field: the first box inside the moov
        size_t at = 0;
        while (at + 8 <= m.out.n && memcmp(m.out.d + at + 4, "mvhd", 4)) at++;
        patch32(&m.out, at, 0x7FFFFFFF);
        expect_rule("a child box larger than its parent", &m, OS64_MEDIA_FRAGMENT_MALFORMED, mem);
        model_free(&m);
    }
    // A trun claiming more per-sample fields than its box holds.
    {
        dinit_t d = av_init();
        write_init(&m, &d);
        dseg_t s = { .n = 1 };
        s.f[0] = full_traf(1, 0, 3, 512);
        write_segment(&m, &s);
        size_t at = 0;
        while (at + 8 <= m.out.n && memcmp(m.out.d + at + 4, "trun", 4)) at++;
        patch32(&m.out, at + 12, 1000);         // sample_count
        expect_rule("a trun counting past its box", &m, OS64_MEDIA_FRAGMENT_MALFORMED, mem);
        model_free(&m);
    }
    // A trun naming more frames than the parser holds, with nothing per sample.
    {
        dinit_t d = { .n = 1, .movie_timescale = 1000 };
        d.t[0] = video(1);
        d.t[0].trex_dur = 1; d.t[0].trex_size = 0;
        write_init(&m, &d);
        dseg_t s = { .n = 1 };
        s.f[0] = (dtraf_t){ .track_id = 1, .n = 1 };
        write_segment(&m, &s);
        size_t at = 0;
        while (at + 8 <= m.out.n && memcmp(m.out.d + at + 4, "trun", 4)) at++;
        patch32(&m.out, at + 12, OS64_MEDIA_FRAGMENT_MAX_SAMPLES + 1);
        expect_rule("a trun naming 65537 frames", &m, OS64_MEDIA_FRAGMENT_TOO_MANY_SAMPLES, mem);
        model_free(&m);
    }
    // An esds descriptor longer than its box.
    {
        dinit_t d = av_init();
        write_init(&m, &d);
        size_t at = 0;
        while (at + 8 <= m.out.n && memcmp(m.out.d + at + 4, "esds", 4)) at++;
        m.out.d[at + 13] = 0x7F;                // ES_Descriptor's length
        expect_rule("an esds descriptor past its box", &m, OS64_MEDIA_FRAGMENT_MALFORMED, mem);
        model_free(&m);
    }
    // Out of memory at each allocation in turn: always NO_MEMORY, never a leak.
    {
        dinit_t d = av_init();
        write_init(&m, &d);
        dseg_t s = { .n = 2, .interleave = 1 };
        s.f[0] = full_traf(1, 0, 8, 512); s.f[0].truns = 2;
        s.f[1] = full_traf(2, 0, 8, 1024); s.f[1].truns = 2;
        write_segment(&m, &s);
        for (long k = 1; k <= 6; k++) {
            sink_ctx_t c = { .hold = 1 };
            long live0 = s_live;
            s_fail_countdown = k;
            os64_media_fragment_rule_t rule = feed(&m.out, CUT_WHOLE, &c, mem);
            s_fail_countdown = 0;
            check(rule == OS64_MEDIA_FRAGMENT_NO_MEMORY || rule == OS64_MEDIA_FRAGMENT_OK,
                  "allocation %ld failing: \"%s\"", k, os64_media_fragment_rule_name(rule));
            release_held(&c);
            check(s_live == live0, "allocation %ld failing: %ld blocks not given back", k, s_live - live0);
            free(c.got.e);
            free(c.held);
        }
        model_free(&m);
    }
}

// ── Holds and reset ────────────────────────────────────────────────────────

static void holds_and_reset(void)
{
    model_t m = { 0 };
    dinit_t d = av_init();
    write_init(&m, &d);
    size_t init_end = m.out.n;
    dseg_t s = { .n = 1 };
    s.f[0] = full_traf(1, 0, 10, 512);
    write_segment(&m, &s);

    // Fed up to the middle of the mdat, then reset: the frames out keep their
    // slab, the parser lets go, and the last drop frees it.
    sink_ctx_t c = { .hold = 1 };
    os64_media_fragment_t p;
    os64_media_fragment_sink_t sink = { on_init, on_frame, &c };
    long live0 = s_live;
    os64_media_fragment_init(&p, &kMemory, &sink);
    size_t half = init_end + (m.out.n - init_end) * 3 / 4;
    check(os64_media_fragment_append(&p, m.out.d, half) == OS64_MEDIA_FRAGMENT_MORE, "half a stream");
    int frames_before = c.got.n - 1;
    check(frames_before > 0 && frames_before < 10, "some frames out before the reset (%d)", frames_before);
    os64_media_fragment_slab_t *slab = c.held[0];
    uint32_t holds = slab->holds;
    os64_media_fragment_reset(&p);
    check(slab->holds == holds - 1, "reset lets go of the parser's hold");
    check(slab->data[c.got.n > 1 ? 0 : 0] == pattern_byte(m.want.e[1].pattern, 0), "a held frame's bytes survive the reset");
    // The init is kept: the same segment again parses whole.
    size_t before = (size_t)c.got.n;
    check(os64_media_fragment_append(&p, m.out.d + init_end, m.out.n - init_end) == OS64_MEDIA_FRAGMENT_MORE,
          "a segment after reset parses");
    check((size_t)c.got.n == before + 10, "all ten frames after reset (%d)", c.got.n - (int)before);
    // A reset clears an error too.
    uint8_t bad[8] = { 0, 0, 0, 4, 'f', 'r', 'e', 'e' };
    check(os64_media_fragment_append(&p, bad, 8) < 0 && os64_media_fragment_error(&p) == OS64_MEDIA_FRAGMENT_BOX_SIZE,
          "an error");
    check(os64_media_fragment_append(&p, m.out.d + init_end, 4) < 0, "an error stays");
    os64_media_fragment_reset(&p);
    check(os64_media_fragment_append(&p, m.out.d + init_end, m.out.n - init_end) == OS64_MEDIA_FRAGMENT_MORE,
          "a reset clears it");
    os64_media_fragment_destroy(&p);
    release_held(&c);
    check(s_live == live0, "holds and reset: %ld blocks not given back", s_live - live0);
    free(c.got.e);
    free(c.held);

    // A sink that resets or destroys the parser from a frame (the third) or
    // from the init: the append ends there and nothing is touched after; a
    // reset parser takes the segment again whole (its init kept).
    for (int destroys = 0; destroys < 2; destroys++)
        for (int at = 1; at <= 3; at += 2)
            for (int hold = 0; hold < 2; hold++) {
                sink_ctx_t r = { .hold = hold, .reenter_at = at, .reenter_destroys = destroys };
                long live1 = s_live;
                os64_media_fragment_init(&p, &kMemory, &(os64_media_fragment_sink_t){ on_init, on_frame, &r });
                r.p = &p;
                check(os64_media_fragment_append(&p, m.out.d, m.out.n) == OS64_MEDIA_FRAGMENT_MORE,
                      "a sink %s the parser at event %d: the append ends quietly", destroys ? "destroying" : "resetting", at);
                check(r.got.n == at, "a sink %s the parser at event %d: %d events, want %d",
                      destroys ? "destroying" : "resetting", at, r.got.n, at);
                if (!destroys) {
                    r.reenter_at = 0;
                    check(os64_media_fragment_append(&p, m.out.d + init_end, m.out.n - init_end) == OS64_MEDIA_FRAGMENT_MORE
                          && r.got.n == at + 10, "after a sink's reset at event %d the segment parses whole (%d events)",
                          at, r.got.n);
                }
                os64_media_fragment_destroy(&p);
                release_held(&r);
                check(s_live == live1, "a sink %s the parser at event %d: %ld blocks not given back",
                      destroys ? "destroying" : "resetting", at, s_live - live1);
                free(r.got.e);
                free(r.held);
            }
    model_free(&m);
}

// ── The parsing-media-segment flag ─────────────────────────────────────────

static void parsing_media_flag(void)
{
    model_t m = { 0 };
    dinit_t d = av_init();
    write_init(&m, &d);
    size_t init_end = m.out.n;
    dseg_t s = { .n = 1, .spare_mdat = 1 };
    s.f[0] = full_traf(1, 0, 6, 512);
    write_segment(&m, &s);
    size_t spare_start = m.out.n - 12;          // the spare mdat: a header and four bytes
    sink_ctx_t c = { 0 };
    os64_media_fragment_t p;
    os64_media_fragment_sink_t sink = { on_init, on_frame, &c };
    long live0 = s_live;
    os64_media_fragment_init(&p, &kMemory, &sink);
    unsigned wrong = 0;
    for (size_t at = 0; at < m.out.n; at++) {
        os64_media_fragment_append(&p, m.out.d + at, 1);
        size_t fed = at + 1;
        bool want = fed >= init_end + 8 && fed < spare_start;
        if (os64_media_fragment_parsing_media(&p) != want && wrong++ < 3)
            printf("  parsing media after %zu bytes: %d, want %d\n", fed, !want, want);
    }
    check(wrong == 0, "the parsing-media-segment flag: %u bytes with it wrong", wrong);
    // Mid-segment, a reset clears it.
    os64_media_fragment_reset(&p);
    os64_media_fragment_append(&p, m.out.d + init_end, (spare_start - init_end) / 2);
    check(os64_media_fragment_parsing_media(&p), "mid-segment the flag is up");
    os64_media_fragment_reset(&p);
    check(!os64_media_fragment_parsing_media(&p), "a reset clears the flag");
    // An error while the moof's frames are still owed lowers it: the moof,
    // then a box of size 4 where its mdat should be.
    size_t moof_size = (size_t)m.out.d[init_end] << 24 | (size_t)m.out.d[init_end + 1] << 16 |
                       (size_t)m.out.d[init_end + 2] << 8 | m.out.d[init_end + 3];
    os64_media_fragment_append(&p, m.out.d + init_end, moof_size);
    check(os64_media_fragment_parsing_media(&p), "after a moof, frames owed: the flag is up");
    uint8_t bad[8] = { 0, 0, 0, 4, 'm', 'd', 'a', 't' };
    check(os64_media_fragment_append(&p, bad, 8) < 0 && !os64_media_fragment_parsing_media(&p),
          "an error lowers the flag");
    os64_media_fragment_destroy(&p);
    check(s_live == live0, "parsing media: %ld blocks not given back", s_live - live0);
    free(c.got.e);
    free(c.held);
    model_free(&m);
}

// ── Corrupted bytes: every outcome allowed except a read past what was given ──

static void corruption(void)
{
    model_t m = { 0 };
    dinit_t d = av_init();
    write_init(&m, &d);
    dseg_t s = { .n = 2, .interleave = 1 };
    s.f[0] = full_traf(1, 0, 6, 512); s.f[0].truns = 2;
    s.f[1] = full_traf(2, 0, 6, 1024); s.f[1].truns = 2;
    write_segment(&m, &s);
    uint64_t rng = 12345;
    unsigned outcomes[OS64_MEDIA_FRAGMENT_RULE_COUNT] = { 0 };
    for (size_t at = 0; at < m.out.n; at++)
        for (int k = 0; k < 3; k++) {
            buf_t b = { malloc(m.out.n), m.out.n, m.out.n };
            memcpy(b.d, m.out.d, m.out.n);
            rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
            b.d[at] = k == 0 ? 0xFF : k == 1 ? 0x00 : (uint8_t)rng;
            sink_ctx_t c = { .hold = k & 1 };
            long live0 = s_live;
            os64_media_fragment_rule_t rule = feed(&b, k == 2 ? CUT_RANDOM : CUT_WHOLE, &c, kMemory);
            outcomes[rule]++;
            release_held(&c);
            check(s_live == live0, "corrupt byte %zu: %ld blocks not given back", at, s_live - live0);
            free(c.got.e);
            free(c.held);
            free(b.d);
        }
    unsigned seen = 0;
    for (int i = 0; i < OS64_MEDIA_FRAGMENT_RULE_COUNT; i++) seen += outcomes[i] != 0;
    printf("corruption: %zu bytes x 3, %u different outcomes\n", m.out.n, seen);
    model_free(&m);
}

int main(void)
{
    streams();
    rules();
    raw_rules();
    holds_and_reset();
    parsing_media_flag();
    corruption();
    printf("media fragment: %lu checks, %lu failed\n", s_checks, s_failed);
    return s_failed != 0;
}
