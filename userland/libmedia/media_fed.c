// libmedia's fed media (os64/media_fed.h): buffers of parsers and track
// buffers under one lock, fed by a page's appends and read by a player's
// decoding thread (media_fed_internal.h). MEDIA_SOURCE.md § 5 is the design.
//
// NOTHING IS FREED UNDER THE LOCK. The parser's slabs and the track
// buffers' blocks come back through `release`, which may be called under
// the lock (a cut, an eviction, a slab's last hold dropped by a decoder);
// it only threads the block onto the FREE QUEUE through the block's own
// header, so it allocates nothing, and media_fed_unlock frees the queue
// after letting go. A slab is the big free: hundreds of kilobytes, an
// unmap under the heap's lock.
//
// EVERY BLOCK HAS 64 ZEROED BYTES AFTER IT (MEDIA_FED_PADDING): FFmpeg's
// decoders read up to that far past a packet's data. A frame at the end of
// a slab is followed by those zeros; a frame inside one is followed by the
// next frame's bytes, which is safe for what the parser takes: H.264 in
// avcC is length-prefixed, so its parser stops at the frame's stated end
// and the over-read is only its bitstream reader's look-ahead, and an AAC
// raw data block ends by its own syntax, its reader's look-ahead likewise
// changing no decoded value.
#include "os64/media_fed.h"
#include "media_fed_internal.h"
#include "os64/lock.h"
#include "os64/media.h"
#include "os64/media_fragment.h"
#include "os64/mem.h"
#include "os64/str.h"

#define PIECE (64u * 1024u)     // a parse holds the lock for this much input at a time

// The header before every block the memory functions give: its size, and
// the free queue's link once it is released.
typedef struct block {
    struct block *next;
    uint64_t size;
} block_t;
_Static_assert(sizeof(block_t) == 16, "a block's payload stays 16-aligned");

typedef struct {
    os64_media_fed_t *fed;
    bool slabs;                 // counted as slab bytes
} memory_t;

struct os64_media_fed_buffer {
    os64_media_fed_t *fed;
    os64_media_fragment_t parser;
    os64_media_buffer_t track;
    uint8_t *input;             // taken, not yet parsed: grown by doubling, kept while open
    size_t input_length, input_capacity;
    os64_media_fed_result_t last;
    // What the parse in hand saw, from the parser's sink.
    uint32_t inits;
    bool frames;
    os64_media_track_rule_t refused;    // the first frame or init the track buffer refused
    double init_duration;
    bool have_init, audio, video;
    struct os64_media_fed_buffer *next;
};

struct os64_media_fed {
    os64_lock_t lock;
    memory_t slab_memory, block_memory;
    int holders;                // the creator and the player
    bool closed;                // the creator let go
    os64_media_fed_buffer_t *buffers;
    bool picked;
    os64_media_fed_buffer_t *picked_buffer[3];  // by OS64_MEDIA_FRAGMENT_VIDEO / _AUDIO
    uint64_t changes;           // moves when a buffer goes
    bool ended;
    double duration;
    block_t *free_queue;
    os64_bell_t *bell;
    os64_media_fed_counts_t counts;
};

// ---------------------------------------------------------------- memory

static void *fed_alloc(void *context, size_t bytes)
{
    memory_t *m = context;
    if (bytes > SIZE_MAX - sizeof(block_t) - MEDIA_FED_PADDING)
        return 0;
    block_t *b = os64_malloc(sizeof(block_t) + bytes + MEDIA_FED_PADDING);
    if (b == 0)
        return 0;
    b->next = 0;
    b->size = bytes;
    uint8_t *payload = (uint8_t *)(b + 1);
    os64_memset(payload + bytes, 0, MEDIA_FED_PADDING);
    if (m->slabs)
        m->fed->counts.slab_bytes += bytes + MEDIA_FED_PADDING;
    return payload;
}

// Under the lock (every caller holds it): onto the free queue.
static void fed_release(void *context, void *block)
{
    memory_t *m = context;
    if (block == 0)
        return;
    block_t *b = (block_t *)block - 1;
    if (m->slabs)
        m->fed->counts.slab_bytes -= b->size + MEDIA_FED_PADDING;
    b->next = m->fed->free_queue;
    m->fed->free_queue = b;
}

void media_fed_lock(os64_media_fed_t *fed)
{
    os64_lock_acquire(&fed->lock);
}

void media_fed_unlock(os64_media_fed_t *fed)
{
    block_t *queue = fed->free_queue;
    fed->free_queue = 0;
    os64_lock_release(&fed->lock);
    while (queue) {
        block_t *next = queue->next;
        os64_free(queue);
        queue = next;
    }
}

static void ring(os64_media_fed_t *fed)
{
    if (fed->bell)
        (void)os64_bell_ring(fed->bell);
}

// ---------------------------------------------------------------- times

static int64_t ns_of(double seconds, int64_t otherwise)
{
    int64_t ns;
    return os64_media_seconds_to_ns(seconds, &ns) ? ns : otherwise;
}

static double seconds_of(int64_t ns)
{
    return ns == OS64_MEDIA_NO_TIME ? __builtin_nan("") : os64_media_ns_to_seconds(ns);
}

// ---------------------------------------------------------------- the parser's sink

static void sink_init(void *context, const os64_media_fragment_init_t *init)
{
    os64_media_fed_buffer_t *b = context;
    if (b->refused != OS64_MEDIA_TRACK_OK)
        return;
    os64_media_track_rule_t r = os64_media_buffer_init_segment(&b->track, init);
    if (r != OS64_MEDIA_TRACK_OK) {
        b->refused = r;
        return;
    }
    b->inits++;
    b->init_duration = init->duration == 0 || init->movie_timescale == 0
                           ? __builtin_nan("")
                           : (double)init->duration / (double)init->movie_timescale;
    if (!b->have_init) {
        b->have_init = true;
        for (uint32_t i = 0; i < init->track_count; i++) {
            if (init->track[i].kind == OS64_MEDIA_FRAGMENT_VIDEO)
                b->video = true;
            if (init->track[i].kind == OS64_MEDIA_FRAGMENT_AUDIO)
                b->audio = true;
        }
    }
}

static void sink_frame(void *context, const os64_media_fragment_frame_t *frame)
{
    os64_media_fed_buffer_t *b = context;
    if (b->refused != OS64_MEDIA_TRACK_OK)
        return;
    os64_media_track_rule_t r = os64_media_buffer_frame(&b->track, frame);
    if (r != OS64_MEDIA_TRACK_OK)
        b->refused = r;
    else
        b->frames = true;
}

// ---------------------------------------------------------------- results

static void result(os64_media_fed_buffer_t *b, os64_media_fed_result_t *out)
{
    out->init_duration = b->inits > 0 ? b->init_duration : __builtin_nan("");
    out->inits = b->inits;
    out->audio = b->audio;
    out->video = b->video;
    out->frames = b->frames;
    out->parsing_media = os64_media_fragment_parsing_media(&b->parser);
    out->group_end = seconds_of(b->track.group_end);
    out->highest_pts = seconds_of(os64_media_buffer_highest_pts(&b->track));
    out->timestamp_offset = os64_media_ns_to_seconds(b->track.offset);
}

static void begin(os64_media_fed_buffer_t *b, os64_media_fed_result_t *out)
{
    *out = (os64_media_fed_result_t){ .status = OS64_MEDIA_FED_DONE };
    b->inits = 0;
    b->frames = false;
}

// ---------------------------------------------------------------- the creator's calls

int os64_media_fed_create(os64_media_fed_t **out)
{
    if (out == 0)
        return OS64_MEDIA_BAD_ARGS;
    *out = 0;
    os64_media_fed_t *fed = os64_malloc(sizeof(*fed));
    if (fed == 0)
        return OS64_MEDIA_NO_MEMORY;
    os64_memset(fed, 0, sizeof(*fed));
    fed->slab_memory = (memory_t){ fed, true };
    fed->block_memory = (memory_t){ fed, false };
    fed->holders = 1;
    fed->duration = __builtin_nan("");
    *out = fed;
    return 0;
}

// Under the lock: the buffer's parser and track buffer go (their blocks to
// the free queue), then the buffer itself.
static void buffer_destroy(os64_media_fed_t *fed, os64_media_fed_buffer_t *b)
{
    os64_media_fragment_destroy(&b->parser);
    os64_media_buffer_destroy(&b->track);
    for (int k = 0; k < 3; k++)
        if (fed->picked_buffer[k] == b)
            fed->picked_buffer[k] = 0;
    fed_release(&fed->block_memory, b->input);
    fed_release(&fed->block_memory, b);
    fed->changes++;
}

// Under the lock: a holder lets go. Answers whether the caller frees it.
static bool let_go(os64_media_fed_t *fed)
{
    return --fed->holders == 0;
}

void os64_media_fed_close(os64_media_fed_t *fed)
{
    if (fed == 0)
        return;
    media_fed_lock(fed);
    fed->closed = true;
    while (fed->buffers) {
        os64_media_fed_buffer_t *b = fed->buffers;
        fed->buffers = b->next;
        buffer_destroy(fed, b);
    }
    ring(fed);
    bool last = let_go(fed);
    media_fed_unlock(fed);
    if (last)
        os64_free(fed);
}

static bool lower_prefix(const char *s, const char *prefix)
{
    for (; *prefix; s++, prefix++) {
        char c = *s >= 'A' && *s <= 'Z' ? (char)(*s - 'A' + 'a') : *s;
        if (c != *prefix)
            return false;
    }
    return true;
}

bool os64_media_fed_type_supported(const char *type)
{
    if (type == 0 || !(lower_prefix(type, "video/mp4") || lower_prefix(type, "audio/mp4")))
        return false;
    // Each codec named is one the fragment parser takes: H.264 (avc1, avc3)
    // or AAC (mp4a.40.x). libmedia's own answer then says libav decodes it.
    const char *codecs = type;
    while (*codecs && !lower_prefix(codecs, "codecs="))
        codecs++;
    if (!*codecs)
        return false;
    const char *p = codecs + 7;
    if (*p == '"')
        p++;
    bool any = false;
    while (*p && *p != '"' && *p != ';') {
        while (*p == ' ' || *p == ',')
            p++;
        if (!*p || *p == '"' || *p == ';')
            break;
        if (!(lower_prefix(p, "avc1") || lower_prefix(p, "avc3") || lower_prefix(p, "mp4a.40")))
            return false;
        any = true;
        while (*p && *p != ',' && *p != '"' && *p != ';')
            p++;
    }
    return any && os64_media_type_supported(type) == OS64_MEDIA_TYPE_PROBABLY;
}

os64_media_fed_buffer_t *os64_media_fed_add(os64_media_fed_t *fed, const char *type)
{
    if (fed == 0 || !os64_media_fed_type_supported(type))
        return 0;
    media_fed_lock(fed);
    os64_media_fed_buffer_t *b = 0;
    if (!fed->picked && !fed->closed)
        b = fed_alloc(&fed->block_memory, sizeof(*b));
    if (b != 0) {
        os64_memset(b, 0, sizeof(*b));
        b->fed = fed;
        os64_media_fragment_memory_t pm = { fed_alloc, fed_release, &fed->slab_memory, 0, 0 };
        os64_media_fragment_sink_t sink = { sink_init, sink_frame, b };
        os64_media_fragment_init(&b->parser, &pm, &sink);
        os64_media_track_memory_t tm = { fed_alloc, fed_release, &fed->block_memory };
        os64_media_buffer_init(&b->track, &tm);
        b->init_duration = __builtin_nan("");
        os64_media_fed_buffer_t **tail = &fed->buffers;
        while (*tail)
            tail = &(*tail)->next;
        *tail = b;
    }
    media_fed_unlock(fed);
    return b;
}

void os64_media_fed_remove_buffer(os64_media_fed_t *fed, os64_media_fed_buffer_t *buf)
{
    if (fed == 0 || buf == 0)
        return;
    media_fed_lock(fed);
    for (os64_media_fed_buffer_t **p = &fed->buffers; *p; p = &(*p)->next)
        if (*p == buf) {
            *p = buf->next;
            buffer_destroy(fed, buf);
            break;
        }
    ring(fed);
    media_fed_unlock(fed);
}

bool os64_media_fed_evict(os64_media_fed_t *fed, os64_media_fed_buffer_t *buf, size_t incoming, double play)
{
    if (fed == 0 || buf == 0)
        return false;
    media_fed_lock(fed);
    bool fits = os64_media_buffer_make_room(&buf->track, incoming, ns_of(play, OS64_MEDIA_NO_TIME)) ==
                OS64_MEDIA_TRACK_OK;
    ring(fed);          // an eviction may take what a cursor was in
    media_fed_unlock(fed);
    return fits;
}

bool os64_media_fed_take(os64_media_fed_t *fed, os64_media_fed_buffer_t *buf, const uint8_t *bytes, size_t length)
{
    if (fed == 0 || buf == 0 || (bytes == 0 && length > 0))
        return false;
    media_fed_lock(fed);
    fed->ended = false;
    bool ok = true;
    if (buf->input_length + length > buf->input_capacity) {
        size_t want = buf->input_capacity ? buf->input_capacity : PIECE;
        while (want < buf->input_length + length)
            want *= 2;
        uint8_t *grown = fed_alloc(&fed->block_memory, want);
        if (grown == 0) {
            ok = false;
        } else {
            if (buf->input_length)
                os64_memcpy(grown, buf->input, buf->input_length);
            fed_release(&fed->block_memory, buf->input);
            buf->input = grown;
            buf->input_capacity = want;
        }
    }
    media_fed_unlock(fed);
    if (!ok)
        return false;
    // The copy is made outside the lock: only this thread touches the input.
    os64_memcpy(buf->input + buf->input_length, bytes, length);
    buf->input_length += length;
    return true;
}

void os64_media_fed_parse(os64_media_fed_t *fed, os64_media_fed_buffer_t *buf, os64_media_fed_result_t *out)
{
    if (fed == 0 || buf == 0 || out == 0)
        return;
    media_fed_lock(fed);
    begin(buf, out);
    buf->refused = OS64_MEDIA_TRACK_OK;
    size_t at = 0;
    const char *why = 0;
    while (at < buf->input_length && why == 0) {
        size_t n = buf->input_length - at < PIECE ? buf->input_length - at : PIECE;
        if (os64_media_fragment_append(&buf->parser, buf->input + at, n) < 0)
            why = os64_media_fragment_rule_name(os64_media_fragment_error(&buf->parser));
        else if (buf->refused != OS64_MEDIA_TRACK_OK)
            why = os64_media_track_rule_name(buf->refused);
        at += n;
        if (at < buf->input_length && why == 0) {
            media_fed_unlock(fed);          // frees what the piece released
            media_fed_lock(fed);
        }
    }
    buf->input_length = 0;
    if (why != 0) {
        // The append error: the parser reset (MSE § 3.5.6 via reset parser state).
        os64_media_fragment_reset(&buf->parser);
        os64_media_buffer_reset_parser(&buf->track);
        out->status = OS64_MEDIA_FED_ERROR;
        os64_strcopy(out->message, sizeof(out->message), why);
    }
    result(buf, out);
    buf->last = *out;
    if (buf->inits > 0 || buf->frames) {
        // The first time every buffer holds an initialization segment, the
        // tracks the player decodes are picked.
        if (!fed->picked && fed->buffers) {
            bool all = true;
            for (os64_media_fed_buffer_t *b = fed->buffers; b; b = b->next)
                all &= b->have_init;
            if (all) {
                for (os64_media_fed_buffer_t *b = fed->buffers; b; b = b->next) {
                    if (b->video && !fed->picked_buffer[OS64_MEDIA_FRAGMENT_VIDEO])
                        fed->picked_buffer[OS64_MEDIA_FRAGMENT_VIDEO] = b;
                    if (b->audio && !fed->picked_buffer[OS64_MEDIA_FRAGMENT_AUDIO])
                        fed->picked_buffer[OS64_MEDIA_FRAGMENT_AUDIO] = b;
                }
                fed->picked = true;
            }
        }
        ring(fed);
    }
    media_fed_unlock(fed);
}

void os64_media_fed_poll(os64_media_fed_t *fed, os64_media_fed_buffer_t *buf, os64_media_fed_result_t *out)
{
    if (fed == 0 || buf == 0 || out == 0)
        return;
    media_fed_lock(fed);
    *out = buf->last;
    media_fed_unlock(fed);
}

void os64_media_fed_reset(os64_media_fed_t *fed, os64_media_fed_buffer_t *buf, os64_media_fed_result_t *out)
{
    if (fed == 0 || buf == 0 || out == 0)
        return;
    media_fed_lock(fed);
    begin(buf, out);
    buf->input_length = 0;
    os64_media_fragment_reset(&buf->parser);
    os64_media_buffer_reset_parser(&buf->track);
    result(buf, out);
    media_fed_unlock(fed);
}

void os64_media_fed_remove(os64_media_fed_t *fed, os64_media_fed_buffer_t *buf, double start, double end,
                           os64_media_fed_result_t *out)
{
    if (fed == 0 || buf == 0 || out == 0)
        return;
    media_fed_lock(fed);
    begin(buf, out);
    fed->ended = false;
    os64_media_buffer_remove(&buf->track, ns_of(start, 0), ns_of(end, OS64_MEDIA_TIME_MAX));
    result(buf, out);
    ring(fed);
    media_fed_unlock(fed);
}

void os64_media_fed_set(os64_media_fed_t *fed, os64_media_fed_buffer_t *buf, os64_media_fed_mode_t mode,
                        double timestamp_offset, os64_media_fed_group_t group, double window_start,
                        double window_end)
{
    if (fed == 0 || buf == 0)
        return;
    media_fed_lock(fed);
    os64_media_buffer_t *t = &buf->track;
    int m = mode == OS64_MEDIA_FED_SEQUENCE ? OS64_MEDIA_SEQUENCE : OS64_MEDIA_SEGMENTS;
    int64_t offset = ns_of(timestamp_offset, t->offset);
    if (group == OS64_MEDIA_FED_GROUP_AT_END)
        os64_media_buffer_set_mode(t, m);
    else
        t->mode = m;
    if (group == OS64_MEDIA_FED_GROUP_AT_OFFSET)
        os64_media_buffer_set_offset(t, offset);
    else
        t->offset = offset;
    t->window_start = ns_of(window_start, 0);
    t->window_end = ns_of(window_end, OS64_MEDIA_TIME_MAX);
    media_fed_unlock(fed);
}

uint32_t os64_media_fed_ranges(os64_media_fed_t *fed, os64_media_fed_buffer_t *buf, bool ended,
                               os64_media_fed_range_t *out, uint32_t cap, double *highest_end)
{
    if (highest_end)
        *highest_end = __builtin_nan("");
    if (fed == 0 || buf == 0)
        return 0;
    os64_media_time_range_t ns[64];     // ranges are few (DEBTS: they never merge)
    uint32_t fill = cap < 64 ? cap : 64;
    media_fed_lock(fed);
    uint32_t n = os64_media_buffer_buffered(&buf->track, ns, fill, ended);
    if (highest_end)
        *highest_end = seconds_of(os64_media_buffer_highest_end(&buf->track));
    media_fed_unlock(fed);
    for (uint32_t i = 0; i < n && i < fill; i++)
        out[i] = (os64_media_fed_range_t){ os64_media_ns_to_seconds(ns[i].start), os64_media_ns_to_seconds(ns[i].end) };
    return n;
}

void os64_media_fed_duration(os64_media_fed_t *fed, double seconds)
{
    if (fed == 0)
        return;
    media_fed_lock(fed);
    fed->duration = seconds;
    ring(fed);
    media_fed_unlock(fed);
}

void os64_media_fed_end(os64_media_fed_t *fed)
{
    if (fed == 0)
        return;
    media_fed_lock(fed);
    fed->ended = true;
    ring(fed);
    media_fed_unlock(fed);
}

void os64_media_fed_counts(os64_media_fed_t *fed, os64_media_fed_counts_t *out)
{
    if (out == 0)
        return;
    *out = (os64_media_fed_counts_t){ 0 };
    if (fed == 0)
        return;
    media_fed_lock(fed);
    *out = fed->counts;
    out->frame_bytes = 0;
    for (os64_media_fed_buffer_t *b = fed->buffers; b; b = b->next)
        for (uint32_t k = 0; k < b->track.track_count; k++)
            out->frame_bytes += b->track.track[k].bytes;
    media_fed_unlock(fed);
}

// ---------------------------------------------------------------- the decoding side

void media_fed_hold(os64_media_fed_t *fed, os64_bell_t *bell)
{
    media_fed_lock(fed);
    fed->holders++;
    fed->bell = bell;
    media_fed_unlock(fed);
}

void media_fed_release(os64_media_fed_t *fed)
{
    media_fed_lock(fed);
    fed->bell = 0;
    bool last = let_go(fed);
    media_fed_unlock(fed);
    if (last)
        os64_free(fed);
}

bool media_fed_picked(os64_media_fed_t *fed)
{
    return fed->picked && !fed->closed;
}

os64_media_track_t *media_fed_track(os64_media_fed_t *fed, int kind, uint64_t *changes)
{
    if (changes)
        *changes = fed->changes;
    if (fed->closed || kind < 1 || kind > 2 || fed->picked_buffer[kind] == 0)
        return 0;
    return os64_media_buffer_track(&fed->picked_buffer[kind]->track, kind);
}

bool media_fed_ended(const os64_media_fed_t *fed)
{
    return fed->ended;
}

double media_fed_duration(const os64_media_fed_t *fed)
{
    return fed->duration;
}

void media_fed_count_resync(os64_media_fed_t *fed)
{
    fed->counts.resyncs++;
}

void media_fed_count_sound(os64_media_fed_t *fed, uint64_t silence_frames, uint64_t cut_frames)
{
    fed->counts.silence_frames += silence_frames;
    fed->counts.cut_frames += cut_frames;
}

void media_fed_count_reopen(os64_media_fed_t *fed)
{
    fed->counts.reopens++;
}

void media_fed_slab_drop(os64_media_fragment_slab_t *slab)
{
    memory_t *m = slab->context;        // the slab memory's: its fed media
    os64_media_fed_t *fed = m->fed;
    media_fed_lock(fed);
    os64_media_fragment_slab_drop(slab);
    media_fed_unlock(fed);
}
