// media_fragment.c — fragmented MP4 turned into coded frames
// (os64/media_fragment.h; MEDIA_SOURCE.md § 3).
//
// Two halves. The FRAMER (os64_media_fragment_append) walks top-level boxes
// across appends: a header is gathered (8 bytes, 16 with a 64-bit size),
// then the box is held whole (moov, moof), counted past (anything else), or
// for an mdat copied into a slab that frames are handed out of as it fills.
// The READER (parse_moov, parse_moof) runs on a held box, every field read
// through rd_t, which answers zero and marks itself bad rather than read past
// the box: a reader that finishes bad is MALFORMED.

#include "os64/media_fragment.h"

enum { WAIT_HEADER, IN_BOX, IN_SKIP, IN_MDAT };

#define FOURCC(a, b, c, d) ((uint32_t)(a) << 24 | (uint32_t)(b) << 16 | (uint32_t)(c) << 8 | (uint32_t)(d))

#define BOX_MOOV FOURCC('m', 'o', 'o', 'v')
#define BOX_MOOF FOURCC('m', 'o', 'o', 'f')
#define BOX_MDAT FOURCC('m', 'd', 'a', 't')
#define BOX_MVHD FOURCC('m', 'v', 'h', 'd')
#define BOX_MVEX FOURCC('m', 'v', 'e', 'x')
#define BOX_TREX FOURCC('t', 'r', 'e', 'x')
#define BOX_TRAK FOURCC('t', 'r', 'a', 'k')
#define BOX_TKHD FOURCC('t', 'k', 'h', 'd')
#define BOX_EDTS FOURCC('e', 'd', 't', 's')
#define BOX_ELST FOURCC('e', 'l', 's', 't')
#define BOX_MDIA FOURCC('m', 'd', 'i', 'a')
#define BOX_MDHD FOURCC('m', 'd', 'h', 'd')
#define BOX_HDLR FOURCC('h', 'd', 'l', 'r')
#define BOX_MINF FOURCC('m', 'i', 'n', 'f')
#define BOX_STBL FOURCC('s', 't', 'b', 'l')
#define BOX_STSD FOURCC('s', 't', 's', 'd')
#define BOX_STTS FOURCC('s', 't', 't', 's')
#define BOX_STSC FOURCC('s', 't', 's', 'c')
#define BOX_STCO FOURCC('s', 't', 'c', 'o')
#define BOX_CO64 FOURCC('c', 'o', '6', '4')
#define BOX_STSZ FOURCC('s', 't', 's', 'z')
#define BOX_AVC1 FOURCC('a', 'v', 'c', '1')
#define BOX_AVC3 FOURCC('a', 'v', 'c', '3')
#define BOX_AVCC FOURCC('a', 'v', 'c', 'C')
#define BOX_MP4A FOURCC('m', 'p', '4', 'a')
#define BOX_ESDS FOURCC('e', 's', 'd', 's')
#define BOX_TRAF FOURCC('t', 'r', 'a', 'f')
#define BOX_TFHD FOURCC('t', 'f', 'h', 'd')
#define BOX_TFDT FOURCC('t', 'f', 'd', 't')
#define BOX_TRUN FOURCC('t', 'r', 'u', 'n')
#define HANDLER_VIDE FOURCC('v', 'i', 'd', 'e')
#define HANDLER_SOUN FOURCC('s', 'o', 'u', 'n')

// tfhd's flags (ISO 14496-12 § 8.8.7).
#define TFHD_BASE_DATA_OFFSET 0x000001u
#define TFHD_SAMPLE_DESCRIPTION 0x000002u
#define TFHD_DEFAULT_DURATION 0x000008u
#define TFHD_DEFAULT_SIZE 0x000010u
#define TFHD_DEFAULT_FLAGS 0x000020u
#define TFHD_DURATION_IS_EMPTY 0x010000u
#define TFHD_DEFAULT_BASE_IS_MOOF 0x020000u
// trun's (§ 8.8.8).
#define TRUN_DATA_OFFSET 0x000001u
#define TRUN_FIRST_FLAGS 0x000004u
#define TRUN_DURATION 0x000100u
#define TRUN_SIZE 0x000200u
#define TRUN_FLAGS 0x000400u
#define TRUN_CTO 0x000800u
// A sample's flags (§ 8.8.3.1).
#define SAMPLE_NON_SYNC 0x00010000u
#define SAMPLE_DEPENDS_ON(f) (((f) >> 24) & 3u)

// ── The reader ─────────────────────────────────────────────────────────────

typedef struct {
    const uint8_t *p;
    uint64_t left;
    bool bad;
} rd_t;

// `bytes` big-endian bytes, or 0 with the reader marked bad if they are not there.
static uint64_t rd_n(rd_t *r, unsigned bytes)
{
    if (r->bad || r->left < bytes) {
        r->bad = true;
        return 0;
    }
    uint64_t v = 0;
    for (unsigned i = 0; i < bytes; i++)
        v = v << 8 | r->p[i];
    r->p += bytes;
    r->left -= bytes;
    return v;
}

static void rd_skip(rd_t *r, uint64_t n)
{
    if (r->bad || r->left < n) {
        r->bad = true;
        return;
    }
    r->p += n;
    r->left -= n;
}

typedef struct {
    uint32_t type;
    rd_t body;
} box_t;

// The next box in a container. False at the container's end, and false with
// the container marked bad when a box claims more than the container holds.
static bool rd_box(rd_t *c, box_t *b)
{
    if (c->bad || c->left == 0)
        return false;
    uint64_t size = rd_n(c, 4);
    uint32_t type = (uint32_t)rd_n(c, 4);
    uint64_t head = 8;
    if (size == 1) {
        size = rd_n(c, 8);
        head = 16;
    } else if (size == 0) {
        size = head + c->left;          // inside a box: to the end of its container
    }
    if (c->bad || size < head || size - head > c->left) {
        c->bad = true;
        return false;
    }
    b->type = type;
    b->body.p = c->p;
    b->body.left = size - head;
    b->body.bad = false;
    c->p += size - head;
    c->left -= size - head;
    return true;
}

// The first child of `type`: 1 found, 0 absent, -1 a child lies about its size.
static int find_box(rd_t c, uint32_t type, box_t *out)
{
    box_t b;
    while (rd_box(&c, &b))
        if (b.type == type) {
            *out = b;
            return 1;
        }
    return c.bad ? -1 : 0;
}

// An MPEG-4 descriptor's length: up to four bytes of seven bits.
static uint64_t rd_descriptor_length(rd_t *r)
{
    uint64_t length = 0;
    for (int i = 0; i < 4; i++) {
        uint8_t c = (uint8_t)rd_n(r, 1);
        length = length << 7 | (c & 0x7Fu);
        if (!(c & 0x80u))
            break;
    }
    return length;
}

// A descriptor of `tag` inside `r`, its contents bounded to its own length.
static bool rd_descriptor(rd_t *r, uint8_t tag, rd_t *out)
{
    while (!r->bad && r->left > 0) {
        uint8_t t = (uint8_t)rd_n(r, 1);
        uint64_t length = rd_descriptor_length(r);
        if (r->bad || length > r->left) {
            r->bad = true;
            return false;
        }
        rd_t inner = { r->p, length, false };
        rd_skip(r, length);
        if (t == tag) {
            *out = inner;
            return true;
        }
    }
    return false;
}

// ── Memory ─────────────────────────────────────────────────────────────────

static void *mem_alloc(os64_media_fragment_t *p, uint64_t bytes)
{
    if (bytes == 0 || bytes > SIZE_MAX)
        return 0;
    return p->memory.alloc(p->memory.context, (size_t)bytes);
}

static void mem_release(os64_media_fragment_t *p, void *block)
{
    if (block != 0)
        p->memory.release(p->memory.context, block);
}

void os64_media_fragment_slab_hold(os64_media_fragment_slab_t *slab)
{
    slab->holds++;
}

void os64_media_fragment_slab_drop(os64_media_fragment_slab_t *slab)
{
    if (--slab->holds == 0)
        slab->release(slab->context, slab);
}

// ── The init segment ───────────────────────────────────────────────────────

// avcC: configurationVersion 1, then profile, compatibility, level.
static os64_media_fragment_rule_t parse_avc(box_t entry, os64_media_fragment_entry_t *e)
{
    rd_t r = entry.body;
    rd_skip(&r, 24);                    // SampleEntry's 8, VisualSampleEntry's predefined 16
    e->width = (uint16_t)rd_n(&r, 2);
    e->height = (uint16_t)rd_n(&r, 2);
    rd_skip(&r, 50);                    // resolutions, frame count, compressor name, depth
    if (r.bad)
        return OS64_MEDIA_FRAGMENT_MALFORMED;
    box_t avcc;
    int found = find_box(r, BOX_AVCC, &avcc);
    if (found <= 0)
        return OS64_MEDIA_FRAGMENT_MALFORMED;
    if (avcc.body.left < 4 || avcc.body.p[0] != 1)
        return OS64_MEDIA_FRAGMENT_MALFORMED;
    if (avcc.body.left > UINT32_MAX)
        return OS64_MEDIA_FRAGMENT_MALFORMED;
    e->config = avcc.body.p;
    e->config_size = (uint32_t)avcc.body.left;
    e->profile = avcc.body.p[1];
    e->level = avcc.body.p[3];
    return OS64_MEDIA_FRAGMENT_OK;
}

// mp4a: AudioSampleEntry, then esds's ES_Descriptor → DecoderConfigDescriptor
// → DecoderSpecificInfo (the AudioSpecificConfig).
static os64_media_fragment_rule_t parse_mp4a(box_t entry, os64_media_fragment_entry_t *e)
{
    rd_t r = entry.body;
    rd_skip(&r, 16);                    // SampleEntry's 8, reserved 8
    e->channels = (uint16_t)rd_n(&r, 2);
    rd_skip(&r, 6);                     // sample size, predefined, reserved
    e->sample_rate = (uint32_t)(rd_n(&r, 4) >> 16);
    if (r.bad)
        return OS64_MEDIA_FRAGMENT_MALFORMED;
    box_t esds;
    if (find_box(r, BOX_ESDS, &esds) <= 0)
        return OS64_MEDIA_FRAGMENT_MALFORMED;
    rd_t d = esds.body, es, config, specific;
    rd_skip(&d, 4);                     // version, flags
    if (!rd_descriptor(&d, 3, &es))
        return OS64_MEDIA_FRAGMENT_MALFORMED;
    rd_skip(&es, 2);                    // ES_ID
    uint8_t flags = (uint8_t)rd_n(&es, 1);
    if (flags & 0x80u)
        rd_skip(&es, 2);                // dependsOn_ES_ID
    if (flags & 0x40u)
        rd_skip(&es, rd_n(&es, 1));     // URL
    if (flags & 0x20u)
        rd_skip(&es, 2);                // OCR_ES_Id
    if (es.bad || !rd_descriptor(&es, 4, &config))
        return OS64_MEDIA_FRAGMENT_MALFORMED;
    uint8_t object_type = (uint8_t)rd_n(&config, 1);
    rd_skip(&config, 12);               // stream type, buffer size, bit rates
    if (config.bad)
        return OS64_MEDIA_FRAGMENT_MALFORMED;
    // MPEG-4 audio, and MPEG-2 AAC's three profiles: what the AAC decoder takes.
    if (object_type != 0x40 && object_type != 0x66 && object_type != 0x67 && object_type != 0x68)
        return OS64_MEDIA_FRAGMENT_UNSUPPORTED_CODEC;
    if (!rd_descriptor(&config, 5, &specific) || specific.left < 2 || specific.left > UINT32_MAX)
        return config.bad ? OS64_MEDIA_FRAGMENT_MALFORMED : OS64_MEDIA_FRAGMENT_UNSUPPORTED_CODEC;
    e->config = specific.p;
    e->config_size = (uint32_t)specific.left;
    e->profile = object_type;
    uint8_t aot = specific.p[0] >> 3;
    e->level = aot == 31 ? (uint8_t)(32 + (((specific.p[0] & 7u) << 3) | (specific.p[1] >> 5))) : aot;
    return OS64_MEDIA_FRAGMENT_OK;
}

// A table that would list samples: its entry count must be zero (format § 3).
static bool table_empty(rd_t stbl, uint32_t type, bool *bad)
{
    box_t b;
    int found = find_box(stbl, type, &b);
    if (found < 0) {
        *bad = true;
        return false;
    }
    if (found == 0)
        return true;
    rd_skip(&b.body, 4);                // version, flags
    if (type == BOX_STSZ)
        rd_skip(&b.body, 4);            // sample_size; sample_count follows
    uint64_t count = rd_n(&b.body, 4);
    if (b.body.bad) {
        *bad = true;
        return false;
    }
    return count == 0;
}

// The edit list's offset (header § The edit list), in the track's timescale.
static os64_media_fragment_rule_t parse_edits(box_t elst, uint32_t movie_scale, uint32_t track_scale, int64_t *offset)
{
    rd_t r = elst.body;
    uint8_t version = (uint8_t)rd_n(&r, 1);
    rd_skip(&r, 3);
    uint64_t count = rd_n(&r, 4);
    uint64_t each = version == 1 ? 20 : 12;
    if (r.bad || count > r.left / each)
        return OS64_MEDIA_FRAGMENT_MALFORMED;
    uint64_t empty = 0;
    int64_t media_time = 0;
    for (uint64_t i = 0; i < count; i++) {
        uint64_t duration = version == 1 ? rd_n(&r, 8) : rd_n(&r, 4);
        int64_t time = version == 1 ? (int64_t)rd_n(&r, 8) : (int64_t)(int32_t)(uint32_t)rd_n(&r, 4);
        rd_skip(&r, 4);                 // media rate
        if (time == -1) {
            if (__builtin_add_overflow(empty, duration, &empty))
                return OS64_MEDIA_FRAGMENT_TIME_OVERFLOW;
            continue;
        }
        if (time < 0)
            return OS64_MEDIA_FRAGMENT_MALFORMED;
        media_time = time;
        break;                          // later edits are not followed
    }
    if (r.bad)
        return OS64_MEDIA_FRAGMENT_MALFORMED;
    int64_t shift = 0;
    if (empty != 0) {
        if (movie_scale == 0)
            return OS64_MEDIA_FRAGMENT_BAD_TIMESCALE;
        unsigned __int128 scaled = (unsigned __int128)empty * track_scale / movie_scale;
        if (scaled > (unsigned __int128)INT64_MAX)
            return OS64_MEDIA_FRAGMENT_TIME_OVERFLOW;
        shift = (int64_t)scaled;
    }
    if (__builtin_sub_overflow(media_time, shift, offset))
        return OS64_MEDIA_FRAGMENT_TIME_OVERFLOW;
    return OS64_MEDIA_FRAGMENT_OK;
}

// One trak. *kind is 0 for a track of another kind (its id still read).
static os64_media_fragment_rule_t parse_trak(box_t trak, uint32_t movie_scale, os64_media_fragment_track_t *t, int *kind)
{
    box_t tkhd, mdia, mdhd, hdlr, minf, stbl, stsd, edts, elst;
    *kind = 0;
    if (find_box(trak.body, BOX_TKHD, &tkhd) <= 0 || find_box(trak.body, BOX_MDIA, &mdia) <= 0 ||
        find_box(mdia.body, BOX_MDHD, &mdhd) <= 0 || find_box(mdia.body, BOX_HDLR, &hdlr) <= 0)
        return OS64_MEDIA_FRAGMENT_MALFORMED;
    uint8_t version = (uint8_t)rd_n(&tkhd.body, 1);
    rd_skip(&tkhd.body, 3 + (version == 1 ? 16 : 8));
    t->id = (uint32_t)rd_n(&tkhd.body, 4);
    version = (uint8_t)rd_n(&mdhd.body, 1);
    rd_skip(&mdhd.body, 3 + (version == 1 ? 16 : 8));
    t->timescale = (uint32_t)rd_n(&mdhd.body, 4);
    rd_skip(&hdlr.body, 8);             // version, flags, predefined
    uint32_t handler = (uint32_t)rd_n(&hdlr.body, 4);
    if (tkhd.body.bad || mdhd.body.bad || hdlr.body.bad)
        return OS64_MEDIA_FRAGMENT_MALFORMED;
    if (handler != HANDLER_VIDE && handler != HANDLER_SOUN)
        return OS64_MEDIA_FRAGMENT_OK;  // another kind: read past
    t->kind = handler == HANDLER_VIDE ? OS64_MEDIA_FRAGMENT_VIDEO : OS64_MEDIA_FRAGMENT_AUDIO;
    if (t->timescale == 0)
        return OS64_MEDIA_FRAGMENT_BAD_TIMESCALE;
    if (find_box(mdia.body, BOX_MINF, &minf) <= 0 || find_box(minf.body, BOX_STBL, &stbl) <= 0 ||
        find_box(stbl.body, BOX_STSD, &stsd) <= 0)
        return OS64_MEDIA_FRAGMENT_MALFORMED;
    static const uint32_t tables[] = { BOX_STTS, BOX_STSC, BOX_STCO, BOX_CO64, BOX_STSZ };
    for (unsigned i = 0; i < sizeof(tables) / sizeof(tables[0]); i++) {
        bool bad = false;
        if (!table_empty(stbl.body, tables[i], &bad))
            return bad ? OS64_MEDIA_FRAGMENT_MALFORMED : OS64_MEDIA_FRAGMENT_INIT_HAS_SAMPLES;
    }
    rd_skip(&stsd.body, 4);             // version, flags
    uint64_t count = rd_n(&stsd.body, 4);
    if (stsd.body.bad || count == 0)
        return OS64_MEDIA_FRAGMENT_MALFORMED;
    if (count > OS64_MEDIA_FRAGMENT_MAX_ENTRIES)
        return OS64_MEDIA_FRAGMENT_TOO_MANY_ENTRIES;
    t->entry_count = (uint32_t)count;
    for (uint32_t i = 0; i < t->entry_count; i++) {
        box_t entry;
        if (!rd_box(&stsd.body, &entry))
            return OS64_MEDIA_FRAGMENT_MALFORMED;
        os64_media_fragment_entry_t *e = &t->entry[i];
        e->fourcc = entry.type;
        os64_media_fragment_rule_t rule;
        if ((entry.type == BOX_AVC1 || entry.type == BOX_AVC3) && t->kind == OS64_MEDIA_FRAGMENT_VIDEO)
            rule = parse_avc(entry, e);
        else if (entry.type == BOX_MP4A && t->kind == OS64_MEDIA_FRAGMENT_AUDIO)
            rule = parse_mp4a(entry, e);
        else
            rule = OS64_MEDIA_FRAGMENT_UNSUPPORTED_CODEC;
        if (rule != OS64_MEDIA_FRAGMENT_OK)
            return rule;
    }
    t->edit_offset = 0;
    int has_edts = find_box(trak.body, BOX_EDTS, &edts);
    if (has_edts < 0)
        return OS64_MEDIA_FRAGMENT_MALFORMED;
    if (has_edts > 0) {
        int has_elst = find_box(edts.body, BOX_ELST, &elst);
        if (has_elst < 0)
            return OS64_MEDIA_FRAGMENT_MALFORMED;
        if (has_elst > 0) {
            os64_media_fragment_rule_t rule = parse_edits(elst, movie_scale, t->timescale, &t->edit_offset);
            if (rule != OS64_MEDIA_FRAGMENT_OK)
                return rule;
        }
    }
    *kind = t->kind;
    return OS64_MEDIA_FRAGMENT_OK;
}

static os64_media_fragment_rule_t parse_moov(os64_media_fragment_t *p, rd_t moov)
{
    box_t b, mvex;
    uint32_t movie_scale = 0;
    int found = find_box(moov, BOX_MVHD, &b);
    if (found < 0)
        return OS64_MEDIA_FRAGMENT_MALFORMED;
    if (found > 0) {
        uint8_t version = (uint8_t)rd_n(&b.body, 1);
        rd_skip(&b.body, 3 + (version == 1 ? 16 : 8));
        movie_scale = (uint32_t)rd_n(&b.body, 4);
        if (b.body.bad)
            return OS64_MEDIA_FRAGMENT_MALFORMED;
    }
    found = find_box(moov, BOX_MVEX, &mvex);
    if (found < 0)
        return OS64_MEDIA_FRAGMENT_MALFORMED;
    if (found == 0)
        return OS64_MEDIA_FRAGMENT_NO_MVEX;

    os64_media_fragment_init_t next = { 0 };
    uint32_t ignored[OS64_MEDIA_FRAGMENT_MAX_IGNORED];
    uint32_t ignored_count = 0;
    rd_t walk = moov;
    while (rd_box(&walk, &b)) {
        if (b.type != BOX_TRAK)
            continue;
        os64_media_fragment_track_t t = { 0 };
        int kind;
        os64_media_fragment_rule_t rule = parse_trak(b, movie_scale, &t, &kind);
        if (rule != OS64_MEDIA_FRAGMENT_OK)
            return rule;
        if (kind == 0) {
            if (ignored_count == OS64_MEDIA_FRAGMENT_MAX_IGNORED)
                return OS64_MEDIA_FRAGMENT_TOO_MANY_TRACKS;
            ignored[ignored_count++] = t.id;
            continue;
        }
        for (uint32_t i = 0; i < next.track_count; i++)
            if (next.track[i].kind == kind)
                return OS64_MEDIA_FRAGMENT_TOO_MANY_TRACKS;
        next.track[next.track_count++] = t;
    }
    if (walk.bad)
        return OS64_MEDIA_FRAGMENT_MALFORMED;
    if (next.track_count == 0)
        return OS64_MEDIA_FRAGMENT_NO_TRACKS;
    // Track ids are unique across every trak, read past or not (format § 2).
    uint32_t ids[OS64_MEDIA_FRAGMENT_MAX_TRACKS + OS64_MEDIA_FRAGMENT_MAX_IGNORED];
    uint32_t n = 0;
    for (uint32_t i = 0; i < next.track_count; i++)
        ids[n++] = next.track[i].id;
    for (uint32_t i = 0; i < ignored_count; i++)
        ids[n++] = ignored[i];
    for (uint32_t i = 0; i < n; i++)
        for (uint32_t j = i + 1; j < n; j++)
            if (ids[i] == ids[j])
                return OS64_MEDIA_FRAGMENT_DUPLICATE_TRACK;
    // mvex's trex: each track's fragment defaults.
    walk = mvex.body;
    while (rd_box(&walk, &b)) {
        if (b.type != BOX_TREX)
            continue;
        rd_skip(&b.body, 4);            // version, flags
        uint32_t id = (uint32_t)rd_n(&b.body, 4);
        uint32_t entry = (uint32_t)rd_n(&b.body, 4);
        uint32_t duration = (uint32_t)rd_n(&b.body, 4);
        uint32_t size = (uint32_t)rd_n(&b.body, 4);
        uint32_t flags = (uint32_t)rd_n(&b.body, 4);
        if (b.body.bad)
            return OS64_MEDIA_FRAGMENT_MALFORMED;
        for (uint32_t i = 0; i < next.track_count; i++)
            if (next.track[i].id == id) {
                next.track[i].trex_entry = entry;
                next.track[i].trex_duration = duration;
                next.track[i].trex_size = size;
                next.track[i].trex_flags = flags;
            }
    }
    if (walk.bad)
        return OS64_MEDIA_FRAGMENT_MALFORMED;
    // MSE § 5.5.7: a later init segment keeps the number of tracks of each kind.
    if (p->have_init) {
        for (int kind = OS64_MEDIA_FRAGMENT_VIDEO; kind <= OS64_MEDIA_FRAGMENT_AUDIO; kind++) {
            uint32_t before = 0, after = 0;
            for (uint32_t i = 0; i < p->init.track_count; i++)
                before += p->init.track[i].kind == kind;
            for (uint32_t i = 0; i < next.track_count; i++)
                after += next.track[i].kind == kind;
            if (before != after)
                return OS64_MEDIA_FRAGMENT_TRACK_COUNT_CHANGED;
        }
    }
    next.generation = p->init.generation + 1;
    p->init = next;
    for (uint32_t i = 0; i < ignored_count; i++)
        p->ignored[i] = ignored[i];
    p->ignored_count = ignored_count;
    p->have_init = true;
    return OS64_MEDIA_FRAGMENT_OK;
}

// ── A media segment's moof ─────────────────────────────────────────────────

static os64_media_fragment_rule_t add_sample(os64_media_fragment_t *p, const os64_media_fragment_sample_t *s)
{
    if (p->sample_count == p->sample_capacity) {
        uint32_t capacity = p->sample_capacity ? p->sample_capacity * 2 : 256;
        if (capacity > OS64_MEDIA_FRAGMENT_MAX_SAMPLES)
            capacity = OS64_MEDIA_FRAGMENT_MAX_SAMPLES;
        os64_media_fragment_sample_t *grown = mem_alloc(p, (uint64_t)capacity * sizeof(*grown));
        if (grown == 0)
            return OS64_MEDIA_FRAGMENT_NO_MEMORY;
        if (p->sample_count != 0)
            __builtin_memcpy(grown, p->sample, (size_t)p->sample_count * sizeof(*grown));
        mem_release(p, p->sample);
        p->sample = grown;
        p->sample_capacity = capacity;
    }
    p->sample[p->sample_count++] = *s;
    return OS64_MEDIA_FRAGMENT_OK;
}

// Samples are handed out in stream order: a frame goes when its last byte is
// in. Sorted in place by (start, order of appearance), so equal starts keep
// the trun's order; a heap sort, because a hostile moof can interleave tens
// of thousands of samples from many truns and an insertion sort would make
// that quadratic.
static bool sample_before(const os64_media_fragment_sample_t *a, uint32_t ia,
                          const os64_media_fragment_sample_t *b, uint32_t ib)
{
    return a->start < b->start || (a->start == b->start && ia < ib);
}

static void sift(os64_media_fragment_sample_t *s, uint32_t *order, uint32_t root, uint32_t count)
{
    for (;;) {
        uint32_t child = 2 * root + 1;
        if (child >= count)
            return;
        if (child + 1 < count && sample_before(&s[child], order[child], &s[child + 1], order[child + 1]))
            child++;
        if (!sample_before(&s[root], order[root], &s[child], order[child]))
            return;
        os64_media_fragment_sample_t t = s[root];
        s[root] = s[child];
        s[child] = t;
        uint32_t o = order[root];
        order[root] = order[child];
        order[child] = o;
        root = child;
    }
}

// The order of appearance is kept beside the table in a scratch array, made
// for the sort and released after it; a moof whose samples already run in
// stream order (one traf, or trafs in data order) is left alone and needs none.
static os64_media_fragment_rule_t sort_samples(os64_media_fragment_t *p)
{
    uint32_t count = p->sample_count;
    if (count < 2)
        return OS64_MEDIA_FRAGMENT_OK;
    bool sorted = true;
    for (uint32_t i = 1; i < count && sorted; i++)
        sorted = p->sample[i - 1].start <= p->sample[i].start;
    if (sorted)
        return OS64_MEDIA_FRAGMENT_OK;  // the usual case: one traf, or trafs in data order
    uint32_t *order = mem_alloc(p, (uint64_t)count * sizeof(uint32_t));
    if (order == 0)
        return OS64_MEDIA_FRAGMENT_NO_MEMORY;
    for (uint32_t i = 0; i < count; i++)
        order[i] = i;
    for (uint32_t i = count / 2; i-- > 0;)
        sift(p->sample, order, i, count);
    for (uint32_t end = count - 1; end > 0; end--) {
        os64_media_fragment_sample_t t = p->sample[0];
        p->sample[0] = p->sample[end];
        p->sample[end] = t;
        uint32_t o = order[0];
        order[0] = order[end];
        order[end] = o;
        sift(p->sample, order, 0, end);
    }
    mem_release(p, order);
    return OS64_MEDIA_FRAGMENT_OK;
}

static const os64_media_fragment_track_t *track_of(const os64_media_fragment_t *p, uint32_t id, bool *ignored)
{
    *ignored = false;
    for (uint32_t i = 0; i < p->init.track_count; i++)
        if (p->init.track[i].id == id)
            return &p->init.track[i];
    for (uint32_t i = 0; i < p->ignored_count; i++)
        if (p->ignored[i] == id) {
            *ignored = true;
            return 0;
        }
    return 0;
}

static os64_media_fragment_rule_t parse_traf(os64_media_fragment_t *p, box_t traf, uint64_t moof_position,
                                             bool first_traf, int64_t *chain_end)
{
    box_t tfhd, tfdt, trun;
    if (find_box(traf.body, BOX_TFHD, &tfhd) <= 0)
        return OS64_MEDIA_FRAGMENT_MALFORMED;
    uint32_t flags = (uint32_t)rd_n(&tfhd.body, 4) & 0xFFFFFFu;
    uint32_t id = (uint32_t)rd_n(&tfhd.body, 4);
    if (tfhd.body.bad)
        return OS64_MEDIA_FRAGMENT_MALFORMED;
    bool ignored;
    const os64_media_fragment_track_t *t = track_of(p, id, &ignored);
    if (t == 0 && !ignored)
        return OS64_MEDIA_FRAGMENT_UNKNOWN_TRACK;
    if (flags & TFHD_BASE_DATA_OFFSET)
        return OS64_MEDIA_FRAGMENT_BASE_DATA_OFFSET;
    uint32_t entry = t ? t->trex_entry : 1, duration = t ? t->trex_duration : 0;
    uint32_t size = t ? t->trex_size : 0, sample_flags = t ? t->trex_flags : 0;
    if (flags & TFHD_SAMPLE_DESCRIPTION)
        entry = (uint32_t)rd_n(&tfhd.body, 4);
    if (flags & TFHD_DEFAULT_DURATION)
        duration = (uint32_t)rd_n(&tfhd.body, 4);
    if (flags & TFHD_DEFAULT_SIZE)
        size = (uint32_t)rd_n(&tfhd.body, 4);
    if (flags & TFHD_DEFAULT_FLAGS)
        sample_flags = (uint32_t)rd_n(&tfhd.body, 4);
    if (tfhd.body.bad)
        return OS64_MEDIA_FRAGMENT_MALFORMED;
    int found = find_box(traf.body, BOX_TFDT, &tfdt);
    if (found < 0)
        return OS64_MEDIA_FRAGMENT_MALFORMED;
    if (found == 0)
        return OS64_MEDIA_FRAGMENT_NO_TFDT;
    uint8_t version = (uint8_t)rd_n(&tfdt.body, 1);
    rd_skip(&tfdt.body, 3);
    uint64_t base_time = version == 1 ? rd_n(&tfdt.body, 8) : rd_n(&tfdt.body, 4);
    if (tfdt.body.bad)
        return OS64_MEDIA_FRAGMENT_MALFORMED;
    if (base_time > (uint64_t)INT64_MAX)
        return OS64_MEDIA_FRAGMENT_TIME_OVERFLOW;
    int64_t time = (int64_t)base_time;
    int64_t edit = t ? t->edit_offset : 0;
    // Where this traf's data begins, from the moof's first byte: the moof
    // itself with default-base-is-moof or as the first traf, otherwise where
    // the previous traf's data ended (ISO 14496-12 § 8.8.7).
    int64_t position = (flags & TFHD_DEFAULT_BASE_IS_MOOF) || first_traf ? 0 : *chain_end;
    bool first_trun = true;
    rd_t walk = traf.body;
    while (rd_box(&walk, &trun)) {
        if (trun.type != BOX_TRUN)
            continue;
        rd_t r = trun.body;
        uint8_t trun_version = (uint8_t)rd_n(&r, 1);
        uint32_t trun_flags = (uint32_t)rd_n(&r, 3);
        uint64_t count = rd_n(&r, 4);
        if (r.bad)
            return OS64_MEDIA_FRAGMENT_MALFORMED;
        if (count != 0 && (flags & TFHD_DURATION_IS_EMPTY))
            return OS64_MEDIA_FRAGMENT_EMPTY_FRAGMENT_HAS_SAMPLES;
        if (trun_flags & TRUN_DATA_OFFSET) {
            int64_t offset = (int32_t)(uint32_t)rd_n(&r, 4);
            int64_t base = (flags & TFHD_DEFAULT_BASE_IS_MOOF) || first_traf ? 0 : *chain_end;
            position = base + offset;
        } else if (first_trun) {
            return OS64_MEDIA_FRAGMENT_NO_DATA_OFFSET;
        }
        first_trun = false;
        uint32_t first_flags = sample_flags;
        bool have_first = (trun_flags & TRUN_FIRST_FLAGS) != 0;
        if (have_first)
            first_flags = (uint32_t)rd_n(&r, 4);
        unsigned per = 4u * (unsigned)(((trun_flags & TRUN_DURATION) != 0) + ((trun_flags & TRUN_SIZE) != 0) +
                                       ((trun_flags & TRUN_FLAGS) != 0) + ((trun_flags & TRUN_CTO) != 0));
        if (r.bad || (per != 0 && count > r.left / per))
            return OS64_MEDIA_FRAGMENT_MALFORMED;
        if (count > OS64_MEDIA_FRAGMENT_MAX_SAMPLES - p->sample_count)
            return OS64_MEDIA_FRAGMENT_TOO_MANY_SAMPLES;
        if (count != 0 && t != 0 && (entry < 1 || entry > t->entry_count))
            return OS64_MEDIA_FRAGMENT_BAD_ENTRY_INDEX;
        for (uint64_t i = 0; i < count; i++) {
            uint32_t d = duration, s = size, f = (i == 0 && have_first) ? first_flags : sample_flags;
            int64_t cto = 0;
            if (trun_flags & TRUN_DURATION)
                d = (uint32_t)rd_n(&r, 4);
            if (trun_flags & TRUN_SIZE)
                s = (uint32_t)rd_n(&r, 4);
            if (trun_flags & TRUN_FLAGS)
                f = (uint32_t)rd_n(&r, 4);
            if (trun_flags & TRUN_CTO)
                cto = trun_version == 0 ? (int64_t)rd_n(&r, 4) : (int64_t)(int32_t)(uint32_t)rd_n(&r, 4);
            if (r.bad)
                return OS64_MEDIA_FRAGMENT_MALFORMED;
            if (position < 0)
                return OS64_MEDIA_FRAGMENT_SAMPLE_OUTSIDE_MDAT;
            os64_media_fragment_sample_t sample = {
                .start = moof_position + (uint64_t)position,
                .size = s,
                .duration = d,
                .track = id,
                .entry = entry ? entry - 1 : 0,
                .rap = !(f & SAMPLE_NON_SYNC) && SAMPLE_DEPENDS_ON(f) != 1,
                .ignored = ignored,
            };
            int64_t pts;
            if (__builtin_add_overflow(time, cto, &pts) || __builtin_sub_overflow(time, edit, &sample.dts) ||
                __builtin_sub_overflow(pts, edit, &sample.pts))
                return OS64_MEDIA_FRAGMENT_TIME_OVERFLOW;
            os64_media_fragment_rule_t rule = add_sample(p, &sample);
            if (rule != OS64_MEDIA_FRAGMENT_OK)
                return rule;
            if (__builtin_add_overflow(position, (int64_t)s, &position) ||
                __builtin_add_overflow(time, (int64_t)d, &time))
                return OS64_MEDIA_FRAGMENT_TIME_OVERFLOW;
        }
    }
    if (walk.bad)
        return OS64_MEDIA_FRAGMENT_MALFORMED;
    *chain_end = position;
    return OS64_MEDIA_FRAGMENT_OK;
}

static os64_media_fragment_rule_t parse_moof(os64_media_fragment_t *p, rd_t moof, uint64_t moof_position)
{
    p->sample_count = 0;
    p->sample_next = 0;
    box_t b;
    bool first = true;
    int64_t chain_end = 0;
    rd_t walk = moof;
    while (rd_box(&walk, &b)) {
        if (b.type != BOX_TRAF)
            continue;
        os64_media_fragment_rule_t rule = parse_traf(p, b, moof_position, first, &chain_end);
        if (rule != OS64_MEDIA_FRAGMENT_OK)
            return rule;
        first = false;
    }
    if (walk.bad || first)              // a moof with no traf (format § 4)
        return OS64_MEDIA_FRAGMENT_MALFORMED;
    return sort_samples(p);
}

// ── The framer ─────────────────────────────────────────────────────────────

static int fail(os64_media_fragment_t *p, os64_media_fragment_rule_t rule)
{
    p->error = rule;
    return -1;
}

// Hand out every sample of the moof whose bytes are now all in this mdat.
static void deliver(os64_media_fragment_t *p)
{
    uint64_t have = p->mdat_start + p->slab->filled;
    while (p->sample_next < p->sample_count) {
        // Samples run in stream order and the first one pending was checked
        // to lie inside this mdat when the mdat began (begin_box), so none
        // here starts before it; one that runs past its end never completes,
        // and the mdat's end says so.
        const os64_media_fragment_sample_t *s = &p->sample[p->sample_next];
        if (s->start >= p->mdat_end)
            return;                     // a later mdat's
        if (s->start > have || s->size > have - s->start)
            return;                     // not all in yet (a gap may lie before it)
        if (!s->ignored) {
            const os64_media_fragment_track_t *t = 0;
            for (uint32_t i = 0; i < p->init.track_count; i++)
                if (p->init.track[i].id == s->track)
                    t = &p->init.track[i];
            os64_media_fragment_frame_t frame = {
                .track_id = s->track,
                .kind = t ? t->kind : 0,
                .generation = p->init.generation,
                .entry = s->entry,
                .timescale = t ? t->timescale : 0,
                .dts = s->dts,
                .pts = s->pts,
                .duration = s->duration,
                .rap = s->rap,
                .slab = p->slab,
                .offset = s->start - p->mdat_start,
                .size = s->size,
            };
            if (p->sink.frame)
                p->sink.frame(p->sink.context, &frame);
        }
        p->sample_next++;
    }
}

// The moof's samples must all have been handed out before another moof or
// moov: what is left lay outside every mdat that followed it.
static bool samples_pending(const os64_media_fragment_t *p)
{
    return p->sample_next < p->sample_count;
}

// A whole header is in: decide what the box is.
static int begin_box(os64_media_fragment_t *p)
{
    const uint8_t *h = p->header;
    uint64_t size = (uint64_t)h[0] << 24 | (uint64_t)h[1] << 16 | (uint64_t)h[2] << 8 | h[3];
    uint32_t type = (uint32_t)h[4] << 24 | (uint32_t)h[5] << 16 | (uint32_t)h[6] << 8 | h[7];
    if (p->header_size == 16)
        for (int i = 8; i < 16; i++)
            size = i == 8 ? h[i] : size << 8 | h[i];
    if (size == 0 || size < p->header_size)
        return fail(p, OS64_MEDIA_FRAGMENT_BOX_SIZE);
    p->box_size = size;
    uint64_t payload = size - p->header_size;
    uint64_t box_start = p->position - p->header_size;

    if (type == BOX_MDAT) {
        p->mdat_start = p->position;
        p->mdat_end = p->position + payload;
        // A slab only for an mdat that holds the next frame: one that holds
        // none (before any moof, or a spare after its frames) is read past.
        if (!samples_pending(p) || payload == 0) {
            p->skip_left = payload;
            p->state = IN_SKIP;
            return 0;
        }
        const os64_media_fragment_sample_t *next = &p->sample[p->sample_next];
        if (next->start < p->mdat_start)
            return fail(p, OS64_MEDIA_FRAGMENT_SAMPLE_OUTSIDE_MDAT);
        if (next->start >= p->mdat_end) {
            p->skip_left = payload;
            p->state = IN_SKIP;
            return 0;
        }
        uint64_t cap = p->memory.max_mdat ? p->memory.max_mdat : OS64_MEDIA_FRAGMENT_DEFAULT_MAX_MDAT;
        if (payload > cap)
            return fail(p, OS64_MEDIA_FRAGMENT_MDAT_TOO_LARGE);
        os64_media_fragment_slab_t *slab = mem_alloc(p, sizeof(*slab) + payload);
        if (slab == 0)
            return fail(p, OS64_MEDIA_FRAGMENT_NO_MEMORY);
        slab->data = (uint8_t *)(slab + 1);
        slab->size = payload;
        slab->filled = 0;
        slab->holds = 1;                // the parser's, until the mdat ends
        slab->release = p->memory.release;
        slab->context = p->memory.context;
        p->slab = slab;
        p->state = IN_MDAT;
        return 0;
    }
    if (type == BOX_MOOV || type == BOX_MOOF) {
        if (samples_pending(p))
            return fail(p, OS64_MEDIA_FRAGMENT_SAMPLE_OUTSIDE_MDAT);
        if (type == BOX_MOOF && !p->have_init)
            return fail(p, OS64_MEDIA_FRAGMENT_MEDIA_BEFORE_INIT);
        uint64_t cap = p->memory.max_box ? p->memory.max_box : OS64_MEDIA_FRAGMENT_DEFAULT_MAX_BOX;
        if (size > cap)
            return fail(p, OS64_MEDIA_FRAGMENT_BOX_TOO_LARGE);
        if (size > p->box_capacity) {
            uint8_t *grown = mem_alloc(p, size);
            if (grown == 0)
                return fail(p, OS64_MEDIA_FRAGMENT_NO_MEMORY);
            mem_release(p, p->box);
            p->box = grown;
            p->box_capacity = size;
        }
        __builtin_memcpy(p->box, p->header, p->header_size);
        p->box_have = p->header_size;
        p->box_start = box_start;
        p->state = IN_BOX;
        return 0;
    }
    p->skip_left = payload;
    p->state = IN_SKIP;
    return 0;
}

// A held box is whole: read it.
static int end_box(os64_media_fragment_t *p)
{
    uint32_t type = (uint32_t)p->box[4] << 24 | (uint32_t)p->box[5] << 16 | (uint32_t)p->box[6] << 8 | p->box[7];
    rd_t body = { p->box + p->header_size, p->box_size - p->header_size, false };
    uint64_t box_start = p->box_start;
    p->state = WAIT_HEADER;
    p->header_have = 0;
    if (type == BOX_MOOV) {
        os64_media_fragment_rule_t rule = parse_moov(p, body);
        if (rule != OS64_MEDIA_FRAGMENT_OK)
            return fail(p, rule);
        // The configurations point into this buffer: it becomes the init's,
        // and the old init's buffer becomes the box buffer.
        uint8_t *held = p->box;
        uint64_t held_capacity = p->box_capacity;
        p->box = p->init_box;
        p->box_capacity = p->init_capacity;
        p->init_box = held;
        p->init_capacity = held_capacity;
        if (p->sink.init)
            p->sink.init(p->sink.context, &p->init);
        return 0;
    }
    os64_media_fragment_rule_t rule = parse_moof(p, body, box_start);
    if (rule != OS64_MEDIA_FRAGMENT_OK)
        return fail(p, rule);
    return 0;
}

int os64_media_fragment_append(os64_media_fragment_t *p, const uint8_t *bytes, size_t length)
{
    if (p->error != OS64_MEDIA_FRAGMENT_OK)
        return -1;
    while (length > 0) {
        size_t take;
        switch (p->state) {
        case WAIT_HEADER: {
            uint32_t want = p->header_have < 8 ? 8 : p->header_size;
            take = want - p->header_have;
            if (take > length)
                take = length;
            __builtin_memcpy(p->header + p->header_have, bytes, take);
            p->header_have += (uint32_t)take;
            p->position += take;
            bytes += take;
            length -= take;
            if (p->header_have == 8) {
                bool large = p->header[0] == 0 && p->header[1] == 0 && p->header[2] == 0 && p->header[3] == 1;
                p->header_size = large ? 16 : 8;
            }
            if (p->header_have >= 8 && p->header_have == p->header_size) {
                p->header_have = 0;
                if (begin_box(p) < 0)
                    return -1;
                if (p->state == IN_BOX && p->box_have == p->box_size && end_box(p) < 0)
                    return -1;
            }
            break;
        }
        case IN_BOX:
            take = p->box_size - p->box_have < length ? (size_t)(p->box_size - p->box_have) : length;
            __builtin_memcpy(p->box + p->box_have, bytes, take);
            p->box_have += take;
            p->position += take;
            bytes += take;
            length -= take;
            if (p->box_have == p->box_size && end_box(p) < 0)
                return -1;
            break;
        case IN_SKIP:
            take = p->skip_left < length ? (size_t)p->skip_left : length;
            p->skip_left -= take;
            p->position += take;
            bytes += take;
            length -= take;
            if (p->skip_left == 0) {
                p->state = WAIT_HEADER;
                p->header_have = 0;
            }
            break;
        case IN_MDAT:
        default: {
            os64_media_fragment_slab_t *slab = p->slab;
            take = slab->size - slab->filled < length ? (size_t)(slab->size - slab->filled) : length;
            __builtin_memcpy(slab->data + slab->filled, bytes, take);
            slab->filled += take;
            p->position += take;
            bytes += take;
            length -= take;
            deliver(p);
            if (slab->filled == slab->size) {
                // The mdat is whole: a frame that began in it and is still not
                // out runs past its end.
                if (samples_pending(p) && p->sample[p->sample_next].start < p->mdat_end)
                    return fail(p, OS64_MEDIA_FRAGMENT_SAMPLE_OUTSIDE_MDAT);
                p->slab = 0;
                os64_media_fragment_slab_drop(slab);
                p->state = WAIT_HEADER;
                p->header_have = 0;
            }
            break;
        }
        }
    }
    return OS64_MEDIA_FRAGMENT_MORE;
}

os64_media_fragment_rule_t os64_media_fragment_error(const os64_media_fragment_t *p)
{
    return p->error;
}

const char *os64_media_fragment_rule_name(os64_media_fragment_rule_t rule)
{
    static const char *const names[OS64_MEDIA_FRAGMENT_RULE_COUNT] = {
        [OS64_MEDIA_FRAGMENT_OK] = "ok",
        [OS64_MEDIA_FRAGMENT_NO_MEMORY] = "out of memory",
        [OS64_MEDIA_FRAGMENT_BOX_SIZE] = "a box's size is zero or smaller than its header",
        [OS64_MEDIA_FRAGMENT_BOX_TOO_LARGE] = "a moov or moof larger than the parser holds",
        [OS64_MEDIA_FRAGMENT_MDAT_TOO_LARGE] = "an mdat larger than the parser holds",
        [OS64_MEDIA_FRAGMENT_TOO_MANY_SAMPLES] = "a moof names more frames than the parser holds",
        [OS64_MEDIA_FRAGMENT_MALFORMED] = "a field runs past the box it sits in",
        [OS64_MEDIA_FRAGMENT_MEDIA_BEFORE_INIT] = "a media segment before any init segment",
        [OS64_MEDIA_FRAGMENT_NO_MVEX] = "an init segment without mvex",
        [OS64_MEDIA_FRAGMENT_INIT_HAS_SAMPLES] = "an init segment with samples of its own",
        [OS64_MEDIA_FRAGMENT_NO_TRACKS] = "an init segment with no picture or sound track",
        [OS64_MEDIA_FRAGMENT_TOO_MANY_TRACKS] = "more than one track of a kind",
        [OS64_MEDIA_FRAGMENT_DUPLICATE_TRACK] = "two tracks with one id",
        [OS64_MEDIA_FRAGMENT_TRACK_COUNT_CHANGED] = "an init segment with another number of tracks of a kind",
        [OS64_MEDIA_FRAGMENT_UNSUPPORTED_CODEC] = "a sample entry v1 does not decode",
        [OS64_MEDIA_FRAGMENT_TOO_MANY_ENTRIES] = "more sample entries than the parser holds",
        [OS64_MEDIA_FRAGMENT_BAD_TIMESCALE] = "a timescale of zero",
        [OS64_MEDIA_FRAGMENT_UNKNOWN_TRACK] = "a traf for a track the init segment has not got",
        [OS64_MEDIA_FRAGMENT_BASE_DATA_OFFSET] = "a traf with a base data offset",
        [OS64_MEDIA_FRAGMENT_NO_DATA_OFFSET] = "a traf whose first trun has no data offset",
        [OS64_MEDIA_FRAGMENT_NO_TFDT] = "a traf without tfdt",
        [OS64_MEDIA_FRAGMENT_BAD_ENTRY_INDEX] = "a sample description index that names no entry",
        [OS64_MEDIA_FRAGMENT_EMPTY_FRAGMENT_HAS_SAMPLES] = "a duration-is-empty fragment that names frames",
        [OS64_MEDIA_FRAGMENT_SAMPLE_OUTSIDE_MDAT] = "a frame's bytes are not all inside one mdat",
        [OS64_MEDIA_FRAGMENT_TIME_OVERFLOW] = "a time that does not fit in 63 bits",
    };
    if ((unsigned)rule >= OS64_MEDIA_FRAGMENT_RULE_COUNT || names[rule] == 0)
        return "unknown";
    return names[rule];
}

void os64_media_fragment_init(os64_media_fragment_t *p, const os64_media_fragment_memory_t *memory,
                              const os64_media_fragment_sink_t *sink)
{
    *p = (os64_media_fragment_t){ 0 };
    p->memory = *memory;
    p->sink = *sink;
    p->state = WAIT_HEADER;
}

void os64_media_fragment_reset(os64_media_fragment_t *p)
{
    if (p->slab != 0) {
        os64_media_fragment_slab_t *slab = p->slab;
        p->slab = 0;
        os64_media_fragment_slab_drop(slab);
    }
    p->state = WAIT_HEADER;
    p->header_have = 0;
    p->box_have = 0;
    p->skip_left = 0;
    p->sample_count = 0;
    p->sample_next = 0;
    p->error = OS64_MEDIA_FRAGMENT_OK;
}

void os64_media_fragment_destroy(os64_media_fragment_t *p)
{
    os64_media_fragment_reset(p);
    mem_release(p, p->box);
    mem_release(p, p->init_box);
    mem_release(p, p->sample);
    p->box = p->init_box = 0;
    p->sample = 0;
    p->box_capacity = p->init_capacity = 0;
    p->sample_capacity = 0;
}
