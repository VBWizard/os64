// media_output.c — the sound device, shared by every sound a program plays
// (os64/media_output.h).
//
// THE ROUND. Each turn of the audio thread decides who is in the mix,
// opens or gives back the device, takes a block from every voice that
// plays, mixes and writes it, and reads where the ear is, for each voice.
// It waits on the output's bell when it wrote nothing, and is otherwise
// paced by the write (which waits at most a write's patience for room).
//
// THE COUNTERS. The thread counts the frames it has written since the
// device's counters last began (an open, a flush, or a generation it did
// not start), and each voice keeps anchors: where in the stream each run of
// its frames went. What the ear heard is /dev/audiopos's `heard`, read
// through a voice's anchors.
//
// LOCKS: the output's lock guards the voice list and each voice's `mixed`;
// a voice's own fields are its owner's lock's. The output's is taken first.

#include "os64/media_output.h"

#include "os64/audio.h"
#include "os64/bell.h"
#include "os64/media.h"
#include "os64/os64.h"
#include "os64/thread.h"

#define RATE OS64_AUDIO_RATE
#define WRITE_FRAMES 1024u
// The runs a block may take from one voice (a short clip on loop wraps more
// than once in a block), and the least a block is cut to for a voice its
// runs or anchors hold back (see the block, below).
#define VOICE_PIECES 8u              // os64_media_output_voice_t's priv.piece
#define BLOCK_FLOOR 64u
// While a voice with no clock sounds, its owner is rung this often (its
// `position` moves, which an element's timeupdate reads).
#define RING_SOUNDING_US 250000
#define HOLD_PAST_DELAY_US 150000
#define DROP_CHUNK 4096u

struct os64_media_output {
    os64_lock_t lock;
    os64_bell_t bell;
    bool quit;
    int64_t thread;
    int16_t *buf, *carry;               // the audio thread's: a take, and a write's untaken tail
    int32_t *acc;                       // the mix
    os64_media_output_voice_t *voices;  // linked through priv.next, under `lock`
    os64_media_output_facts_t facts;    // under `lock`
};

typedef struct {
    int64_t audio, position, ctl;       // -1 when shut
    uint32_t generation;
    bool generation_known;
    bool paused;                        // `pause` written
    bool refused;
    const char *refused_why;
    uint64_t written;                   // stream frames written since the counters began
    int64_t idle_since, paused_since, tried_at, rang_at;
    int64_t moved_at;                   // the ear last moved, or the counters began
    uint64_t last_heard;                // the ear's stream frame then
    int64_t drained_since;              // the ring first seen dry with all of it taken, 0 not
    uint32_t delay_us;
    int16_t *carry;                     // a block's tail a write did not take, written first next round
    uint32_t carry_frames, carry_at;
} device_t;

// What the stream says of the ear, read once a round.
typedef struct {
    bool read;                          // a position record was read (its delay holds)
    bool known;                         // the position could say
    uint64_t raw;                       // `heard` as the device stamped it
    int64_t micros;                     // when
    uint64_t heard;                     // `raw` carried on to now while the stream plays
    uint64_t passed;                    // a frame the ear has certainly passed (what prunes anchors)
    bool playing;
    bool drained;                       // the sink has every frame written and the ring ran dry
} ear_t;

// A voice, for one round.
typedef struct {
    os64_media_output_voice_t *v;
    bool wants, plays, remove, finished;
    double gain;
} round_t;

static int64_t now_us(void)
{
    return (int64_t)os64_micros();
}

int64_t os64_media_output_hold_bound(uint32_t delay_us)
{
    int64_t past = (int64_t)delay_us + HOLD_PAST_DELAY_US;
    return past > OS64_MEDIA_CLOCK_START_HOLD_US ? past : OS64_MEDIA_CLOCK_START_HOLD_US;
}

static void control(device_t *d, const char *verb)
{
    if (d->ctl >= 0)
        (void)os64_write((int32_t)d->ctl, verb, os64_strlen(verb));
}

static void counters_restart(device_t *d, int64_t now)
{
    d->written = 0;
    d->generation_known = false;
    d->drained_since = 0;
    d->carry_frames = 0;
    d->moved_at = now;
    d->last_heard = 0;
}

// `cut`: what the device holds is let go (a refusal, a stall) rather than
// left to play out as a close otherwise leaves it. The counters are the
// caller's to begin again (voices_restart, which settles a carry first).
static void device_close(device_t *d, bool cut)
{
    if (cut) {
        control(d, "flush\n");
        if (d->paused)
            control(d, "resume\n");
    }
    if (d->audio >= 0)
        os64_close((int32_t)d->audio);
    if (d->position >= 0)
        os64_close((int32_t)d->position);
    if (d->ctl >= 0)
        os64_close((int32_t)d->ctl);
    d->audio = d->position = d->ctl = -1;
    d->paused = false;
}

// Where the ear is; `*lost` when the stream began a generation this thread
// did not start (a sink attached or detached, a flush from elsewhere).
// `drained` comes with the delay the sink reports: the last frame reaches
// the ear that long after the ring runs dry.
static void device_heard(device_t *d, ear_t *ear, bool *lost)
{
    *ear = (ear_t){0};
    *lost = false;
    if (d->position < 0)
        return;
    os64_audio_position_t pos;
    if (os64_read((int32_t)d->position, &pos, sizeof(pos)) < (int64_t)sizeof(pos))
        return;
    ear->read = true;
    d->delay_us = pos.delay_us;
    if (!d->generation_known) {
        d->generation = pos.generation;
        d->generation_known = true;
    }
    if (pos.generation != d->generation) {
        *lost = true;
        return;
    }
    if (pos.accepted > d->written)
        return;
    ear->known = true;
    ear->raw = pos.heard;
    ear->micros = (int64_t)pos.micros;
    ear->drained = pos.taken >= d->written && pos.state == OS64_AUDIO_STARVED;
    uint64_t heard = pos.heard;
    if (pos.state == OS64_AUDIO_PLAYING) {
        ear->playing = true;
        int64_t now = now_us();
        if (now > ear->micros)
            heard += (uint64_t)(now - ear->micros) * pos.rate / 1000000u;
        if (heard > pos.taken)
            heard = pos.taken;
    }
    ear->heard = heard;
    // Once the ring runs dry with all of it taken, `heard` stops short of
    // `taken` by the sink's delay and moves no more until something is
    // written, so the anchors are pruned by `taken` then, or a voice with
    // every anchor in use would wait for room forever.
    ear->passed = ear->drained ? pos.taken : heard;
}

// Writes the carry, then `frames` of `pcm` (none when NULL), each at most a
// write's patience; what the device does not take is the new carry. False
// when the device failed.
static bool device_write(device_t *d, const int16_t *pcm, uint32_t frames)
{
    if (pcm != NULL) {
        os64_memcpy(d->carry, pcm, (size_t)frames * OS64_AUDIO_FRAME_BYTES);
        d->carry_frames = frames;
        d->carry_at = 0;
    }
    while (d->carry_frames > 0) {
        const uint8_t *bytes = (const uint8_t *)(d->carry + 2 * d->carry_at);
        int64_t n = os64_write_for((int32_t)d->audio, bytes, (size_t)d->carry_frames * OS64_AUDIO_FRAME_BYTES,
                                   OS64_MEDIA_OUTPUT_WRITE_PATIENCE_MS);
        if (n == OS64_INTERRUPTED)
            continue;
        if (n == OS64_AUDIO_FLUSHED)
            break;   // a flush from elsewhere: the next position read finds the generation lost,
                     // and the restart counts the carry dropped
        if (n == OS64_ERR_TIMEOUT)
            break;
        if (n < 0)
            return false;
        uint32_t took = (uint32_t)((uint64_t)n / OS64_AUDIO_FRAME_BYTES);
        d->carry_at += took;
        d->carry_frames -= took;
        if (took == 0)
            break;
    }
    return true;
}

// Frames from the front of a voice's queue that its clock has passed, let
// go unheard. Under the voice's lock.
static void drop_behind(os64_media_output_voice_t *v, int64_t now)
{
    if (v->clock == NULL || os64_media_clock_state(v->clock, now) == OS64_MEDIA_CLOCK_PAUSED)
        return;
    int64_t clock = os64_media_clock_read(v->clock, now);
    int64_t front;
    bool took = false;
    while ((front = os64_media_queue_front(v->queue)) >= 0 && front < clock) {
        uint64_t want = (uint64_t)(clock - front);
        uint32_t n = os64_media_queue_take(v->queue, NULL, want < DROP_CHUNK ? (uint32_t)want : DROP_CHUNK, NULL);
        if (n == 0)
            break;
        v->dropped += n;
        took = true;
    }
    if (took && v->room != NULL)
        v->room(v->ctx);
}

static void look(os64_media_output_voice_t *v)
{
    if (v->look != NULL)
        v->look(v->ctx);
}

// Whether a voice has frames in the stream the ear has not reached.
static bool unheard(const os64_media_output_voice_t *v, const ear_t *ear)
{
    return v->priv.anchors.anchors > 0 &&
           (!ear->known || ear->heard < os64_media_voice_stream_end(&v->priv.anchors));
}

// A voice's frames in the carry, which never reached the device.
static uint32_t carried(const device_t *d, const os64_media_output_voice_t *v)
{
    uint32_t from = d->carry_at, to = d->carry_at + d->carry_frames, n = 0;
    for (uint32_t k = 0; k < v->priv.pieces; k++) {
        uint32_t a = v->priv.piece[k].offset, b = a + v->priv.piece[k].frames;
        uint32_t lo = a > from ? a : from, hi = b < to ? b : to;
        if (hi > lo)
            n += hi - lo;
    }
    return n;
}

// The block last written is all in the device: no voice waits on it.
static void carry_done(round_t *r, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) {
        os64_media_output_voice_t *v = r[i].v;
        if (v->priv.pieces == 0)
            continue;
        v->priv.pieces = 0;
        os64_lock_acquire(v->lock);
        v->unwritten = false;
        os64_lock_release(v->lock);
    }
}

// The device's counters begin again (an open, a flush, a lost generation,
// a close): no voice has a frame in the stream, and a carry is let go, each
// voice's part of it counted dropped rather than written.
// The output is about to let go of a voice's frames still in the device by
// its own choice (a long pause's close, a resume's flush, a full mix): the
// owner is told, so the frames can be had again (`rewind`). The voice's lock
// held.
static void tell_rewind_locked(os64_media_output_voice_t *v)
{
    if (v->remove || v->priv.anchors.anchors == 0 || (v->clock == NULL && v->position < 0))
        return;
    v->rewind = true;
    v->rewind_to = v->position;
    look(v);
}

// `told`: the output chose to let go of what the device held, and each voice
// not playing is told to rewind (a playing one's frames there were heard, or
// are from before its own flush); otherwise it was lost (a refusal, a
// failure, a sink that changed) or nobody's.
static void voices_restart(device_t *d, round_t *r, uint32_t n, int64_t now, bool told)
{
    for (uint32_t i = 0; i < n; i++) {
        os64_media_output_voice_t *v = r[i].v;
        if (told && !r[i].plays) {
            os64_lock_acquire(v->lock);
            tell_rewind_locked(v);
            os64_lock_release(v->lock);
        }
        uint32_t lost = d->carry_frames > 0 ? carried(d, v) : 0;
        v->priv.pieces = 0;
        v->priv.stale = false;
        os64_media_voice_reset(&v->priv.anchors);
        v->priv.last_heard = 0;
        os64_lock_acquire(v->lock);
        v->written -= lost;
        v->dropped += lost;
        v->unwritten = false;
        v->position = -1;   // until the ear reaches its next frames; the owner keeps the last
        if (v->sounding) {
            v->sounding = false;
            look(v);
        }
        os64_lock_release(v->lock);
    }
    counters_restart(d, now);
}

// The device refused, failed or stalled: every voice is told, and a clock
// that plays on goes monotonic. The output's lock held.
static void tell_refused_locked(os64_media_output_t *o, device_t *d, bool stalled, int64_t now)
{
    o->facts.refused = d->refused;
    o->facts.refused_why = d->refused_why;
    if (d->refused) {
        o->facts.refusals++;
        if (stalled)
            o->facts.stalls++;
    }
    for (os64_media_output_voice_t *v = o->voices; v != NULL; v = v->priv.next) {
        os64_lock_acquire(v->lock);
        bool was = v->refused_device;
        v->refused_device = d->refused;
        v->refused_why = d->refused ? d->refused_why : NULL;
        if (d->refused && !was) {
            v->refusals++;
            if (stalled)
                v->stalls++;
        }
        if (was != d->refused)
            look(v);
        (void)now;
        os64_lock_release(v->lock);
    }
}

// Each voice's busy fact, and its clock's source: a voice with a clock is
// heard while it wants sound and the device is not refused, and runs on the
// monotonic clock otherwise (PLAY_ON, or not wanting). A WAIT voice that
// wants sound and is refused, or wants it and is left out of a full mix,
// is busy. The output's lock held.
static void tell_voices_locked(os64_media_output_t *o, device_t *d, int64_t now)
{
    for (os64_media_output_voice_t *v = o->voices; v != NULL; v = v->priv.next) {
        os64_lock_acquire(v->lock);
        bool wants = v->wants && !v->remove;
        bool busy = wants && ((d->refused && v->refused == OS64_MEDIA_VOICE_WAIT) || !v->priv.mixed);
        if (busy != v->busy) {
            v->busy = busy;
            if (busy)
                v->sounding = false;
            look(v);
        }
        if (!v->priv.mixed)
            v->heard_all = false;   // out of the mix it hears nothing, and what it heard is history
        if (v->clock != NULL) {
            bool heard = wants && !(d->refused && v->refused == OS64_MEDIA_VOICE_PLAY_ON);
            int source = heard ? OS64_MEDIA_CLOCK_HEARD : OS64_MEDIA_CLOCK_MONOTONIC;
            if (v->clock->source != source)
                os64_media_clock_source(v->clock, source, now);
            if (!heard || d->refused)
                drop_behind(v, now);
        }
        os64_lock_release(v->lock);
    }
}

// Who is in the mix: the voices already there (never more than the most),
// then those that want sound while there is room. A full mix with one
// waiting gives up the voices that do not play. A voice removed and not in
// the mix is let go. The output's lock held.
static uint32_t members_locked(os64_media_output_t *o, round_t *r)
{
    uint32_t n = 0, waiting = 0;
    os64_media_output_voice_t **link = &o->voices;
    while (*link != NULL) {
        os64_media_output_voice_t *v = *link;
        os64_lock_acquire(v->lock);
        bool remove = v->remove, wants = v->wants && !remove;
        if (remove && !v->priv.mixed) {
            *link = v->priv.next;
            o->facts.voices--;
            v->gone = true;
            look(v);
            os64_lock_release(v->lock);
            continue;
        }
        os64_lock_release(v->lock);
        if (v->priv.mixed && n < OS64_MEDIA_OUTPUT_VOICES)
            r[n++] = (round_t){.v = v};
        else if (!v->priv.mixed && wants)
            waiting++;
        link = &v->priv.next;
    }
    if (n == OS64_MEDIA_OUTPUT_VOICES && waiting > 0) {
        uint32_t kept = 0;
        for (uint32_t i = 0; i < n; i++) {
            os64_media_output_voice_t *v = r[i].v;
            os64_lock_acquire(v->lock);
            bool plays = v->wants && !v->remove && (v->clock == NULL || v->runs);
            os64_lock_release(v->lock);
            if (plays) {
                r[kept++] = r[i];
                continue;
            }
            os64_lock_acquire(v->lock);
            if (v->sounding) {
                v->sounding = false;
                look(v);
            }
            tell_rewind_locked(v);   // its frames still in the device go with its anchors
            v->unwritten = false;
            v->position = -1;
            os64_lock_release(v->lock);
            os64_media_voice_reset(&v->priv.anchors);
            v->priv.pieces = 0;
            v->priv.mixed = false;
        }
        n = kept;
    }
    for (os64_media_output_voice_t *v = o->voices; v != NULL && n < OS64_MEDIA_OUTPUT_VOICES; v = v->priv.next) {
        if (v->priv.mixed)
            continue;
        os64_lock_acquire(v->lock);
        bool wants = v->wants && !v->remove;
        if (wants) {
            v->priv.generation = v->queue->generation;
            v->priv.clock_generation = v->clock != NULL ? os64_media_clock_generation(v->clock) : 0;
        }
        os64_lock_release(v->lock);
        if (!wants)
            continue;
        v->priv.mixed = true;
        v->priv.stale = false;
        v->priv.pieces = 0;
        os64_media_voice_reset(&v->priv.anchors);
        v->priv.last_heard = 0;
        r[n++] = (round_t){.v = v};
    }
    return n;
}

// A voice leaves the mix: its facts settle, and it joins again when it next
// wants sound.
static void release(os64_media_output_t *o, os64_media_output_voice_t *v)
{
    os64_lock_acquire(&o->lock);
    os64_lock_acquire(v->lock);
    if (v->sounding) {
        v->sounding = false;
        look(v);
    }
    v->unwritten = false;
    os64_lock_release(v->lock);
    v->priv.mixed = false;
    v->priv.pieces = 0;
    os64_lock_release(&o->lock);
}

static void refuse(os64_media_output_t *o, device_t *d, const char *why, bool stalled, round_t *r, uint32_t n,
                   int64_t now)
{
    device_close(d, true);
    voices_restart(d, r, n, now, false);
    d->refused = true;
    d->refused_why = why;
    d->tried_at = now;
    os64_lock_acquire(&o->lock);
    tell_refused_locked(o, d, stalled, now);
    o->facts.open = false;
    os64_lock_release(&o->lock);
}

// The ear, for each voice: its clock's report or its position, whether it
// sounds, and its end. A voice done (removed, or not wanting with its tail
// heard) leaves the mix.
static void hear(os64_media_output_t *o, device_t *d, round_t *r, uint32_t n, const ear_t *ear, int64_t now)
{
    if (!ear->drained)
        d->drained_since = 0;
    else if (d->drained_since == 0)
        d->drained_since = now;
    bool tail_heard = ear->drained && now - d->drained_since >= (int64_t)d->delay_us;
    bool moved = ear->known && ear->raw > d->last_heard;
    bool ring = ear->playing && now - d->rang_at >= RING_SOUNDING_US;
    if (ring)
        d->rang_at = now;
    for (uint32_t i = 0; i < n; i++) {
        os64_media_output_voice_t *v = r[i].v;
        int64_t frame = -1;
        if (ear->known) {
            os64_media_voice_prune(&v->priv.anchors, ear->passed);
            frame = os64_media_voice_heard(&v->priv.anchors, ear->heard);
        }
        bool all_heard = v->priv.anchors.anchors == 0 ||
                         (ear->known && (ear->heard >= os64_media_voice_stream_end(&v->priv.anchors) || tail_heard));
        os64_lock_acquire(v->lock);
        v->delay_us = d->delay_us;
        if (ear->known && !v->delay_known) {
            v->delay_known = true;
            look(v);
        }
        bool synced = v->queue->generation == v->priv.generation;
        bool changed = false;
        if (v->clock != NULL && synced && ear->known && v->priv.anchors.anchors > 0) {
            // A report is advancing only when the ear has moved: PLAYING says
            // the ring holds sound, not that the ear has heard it, and a
            // still ear while PLAYING is the clock's trust's to notice. Any
            // state but PLAYING is said at once (the stream stopped).
            bool voice_moved = ear->raw > v->priv.last_heard;
            v->priv.last_heard = ear->raw;
            if (voice_moved || !ear->playing) {
                int64_t heard = os64_media_voice_heard(&v->priv.anchors, ear->raw);
                os64_media_clock_heard(v->clock, v->priv.clock_generation, heard + v->sync, ear->micros,
                                       voice_moved && ear->playing, now);
            }
        }
        if (v->clock == NULL && synced && frame >= 0)
            v->position = frame;
        bool sounding = ear->playing && synced && v->wants && v->priv.anchors.anchors > 0 && !all_heard;
        if (sounding != v->sounding) {
            v->sounding = sounding;
            changed = true;
        }
        bool heard_all = synced && r[i].finished && all_heard;
        if (heard_all != v->heard_all) {
            v->heard_all = heard_all;
            changed = true;
        }
        if (changed || (ring && sounding && v->clock == NULL))
            look(v);
        bool done = v->remove || (!v->wants && all_heard);
        os64_lock_release(v->lock);
        if (done)
            release(o, v);
    }
    if (moved || !ear->playing || d->written <= ear->raw) {
        d->moved_at = now;
        if (ear->known)
            d->last_heard = ear->raw;
    }
}

// Opens the device, at most once a retry's time; a refusal tells every
// voice. True when it is open.
static bool device_open(os64_media_output_t *o, device_t *d, round_t *r, uint32_t n, int64_t now)
{
    if (d->tried_at != 0 && now - d->tried_at < (int64_t)OS64_MEDIA_OUTPUT_RETRY_MS * 1000) {
        (void)os64_bell_wait(&o->bell, OS64_MEDIA_OUTPUT_RETRY_MS);
        return false;
    }
    d->tried_at = now;
    int64_t h = os64_open("/dev/audio", "w");
    if (h < 0) {
        bool was = d->refused;
        d->refused = true;
        d->refused_why = h == OS64_AUDIO_BUSY ? "the device is busy" : "the device would not open";
        os64_lock_acquire(&o->lock);
        if (!was)
            tell_refused_locked(o, d, false, now);
        os64_lock_release(&o->lock);
        return false;
    }
    d->audio = h;
    int64_t p = os64_open("/dev/audiopos", "r");
    int64_t c = os64_open("/dev/audioctl", "w");
    d->position = p >= 0 ? p : -1;
    d->ctl = c >= 0 ? c : -1;
    d->tried_at = 0;
    voices_restart(d, r, n, now, false);
    bool was = d->refused;
    d->refused = false;
    d->refused_why = NULL;
    os64_lock_acquire(&o->lock);
    o->facts.open = true;
    if (was)
        tell_refused_locked(o, d, false, now);
    tell_voices_locked(o, d, now);
    os64_lock_release(&o->lock);
    return true;
}

// The sink's delay, to each voice: an owner with a clock gives its start a
// hold bound made from it before anything is written.
static void tell_delay(device_t *d, round_t *r, uint32_t n)
{
    ear_t ear;
    bool lost;
    device_heard(d, &ear, &lost);
    if (!ear.read)
        return;
    for (uint32_t i = 0; i < n; i++) {
        os64_media_output_voice_t *v = r[i].v;
        os64_lock_acquire(v->lock);
        v->delay_us = d->delay_us;
        if (!v->delay_known) {
            v->delay_known = true;
            look(v);
        }
        os64_lock_release(v->lock);
    }
}

static int64_t audio_main(void *arg)
{
    os64_media_output_t *o = arg;
    device_t d = {.audio = -1, .position = -1, .ctl = -1};
    int16_t *buf = o->buf;
    int32_t *acc = o->acc;
    d.carry = o->carry;
    round_t r[OS64_MEDIA_OUTPUT_VOICES];
    d.idle_since = now_us();
    for (;;) {
        int64_t now = now_us();
        os64_lock_acquire(&o->lock);
        if (o->quit) {
            os64_lock_release(&o->lock);
            break;
        }
        uint32_t n = members_locked(o, r);
        tell_voices_locked(o, &d, now);
        o->facts.mixed = n;
        os64_lock_release(&o->lock);

        uint32_t playing = 0;
        for (uint32_t i = 0; i < n; i++) {
            os64_media_output_voice_t *v = r[i].v;
            os64_lock_acquire(v->lock);
            r[i].remove = v->remove;
            r[i].wants = v->wants && !v->remove;
            r[i].plays = r[i].wants && (v->clock == NULL || v->runs);
            r[i].gain = v->gain;
            os64_lock_release(v->lock);
            // A voice with a clock that stops wanting sound (muted) goes on
            // by its clock, dropping what it passes: what the device holds
            // of it is from before, not a tail to play out later.
            if (v->clock != NULL && !r[i].wants)
                v->priv.stale = true;
            if (r[i].plays)
                playing++;
        }

        if (n == 0) {
            if (d.audio >= 0 && now - d.idle_since >= (int64_t)OS64_MEDIA_OUTPUT_IDLE_CLOSE_MS * 1000) {
                device_close(&d, false);
                counters_restart(&d, now);
                os64_lock_acquire(&o->lock);
                o->facts.open = false;
                os64_lock_release(&o->lock);
            }
            (void)os64_bell_wait(&o->bell, 250);
            continue;
        }
        d.idle_since = now;

        if (playing == 0) {
            // A voice that wants sound and does not yet know the sink's
            // delay (an owner buffering before its start) has the device
            // opened for it, the delay read, and the device paused.
            bool unknown = false;
            for (uint32_t i = 0; i < n; i++) {
                os64_lock_acquire(r[i].v->lock);
                unknown |= r[i].wants && !r[i].v->delay_known;
                os64_lock_release(r[i].v->lock);
            }
            if (unknown && (d.audio >= 0 || device_open(o, &d, r, n, now)))
                tell_delay(&d, r, n);
            // Nobody plays: the stream's own pause keeps what it holds and
            // stops the ear where it is. A long pause gives the device back,
            // and a voice with no frames left in it (closed, or played out)
            // leaves the mix.
            if (d.audio >= 0 && !d.paused) {
                control(&d, "pause\n");
                d.paused = true;
                d.paused_since = now;
            }
            bool close = d.audio < 0 || now - d.paused_since >= (int64_t)OS64_MEDIA_OUTPUT_PAUSE_KEEP_MS * 1000;
            if (close && d.audio >= 0) {
                device_close(&d, false);
                voices_restart(&d, r, n, now, true);
                os64_lock_acquire(&o->lock);
                o->facts.open = false;
                os64_lock_release(&o->lock);
            }
            for (uint32_t i = 0; i < n; i++) {
                os64_media_output_voice_t *v = r[i].v;
                os64_lock_acquire(v->lock);
                if (v->sounding) {
                    v->sounding = false;
                    look(v);
                }
                bool done = v->remove || (!v->wants && v->priv.anchors.anchors == 0);
                os64_lock_release(v->lock);
                if (done || (close && !r[i].wants))
                    release(o, v);
            }
            (void)os64_bell_wait(&o->bell, 250);
            continue;
        }

        if (d.audio < 0) {
            // No device, so no tail: a voice that does not play has nothing
            // left to hear and leaves the mix now.
            uint32_t kept = 0;
            for (uint32_t i = 0; i < n; i++) {
                if (r[i].wants)
                    r[kept++] = r[i];
                else
                    release(o, r[i].v);
            }
            n = kept;
            if (!device_open(o, &d, r, n, now))
                continue;
        }

        ear_t ear;
        bool lost;
        device_heard(&d, &ear, &lost);
        if (lost) {
            // What the device held is gone; each voice carries on from its
            // queue (a voice with a clock drops what the clock has passed
            // before its next write).
            voices_restart(&d, r, n, now, false);
            continue;
        }
        if (d.paused) {
            // The stream comes back. What it holds that nobody playing owns
            // (the tails of voices paused or closed, heard much later than
            // they were meant to be, or a playing voice's frames from before
            // its flush) is let go.
            bool owned = false;
            for (uint32_t i = 0; i < n; i++) {
                os64_lock_acquire(r[i].v->lock);
                bool current = r[i].v->queue->generation == r[i].v->priv.generation;
                os64_lock_release(r[i].v->lock);
                owned |= r[i].plays && current && !r[i].v->priv.stale && unheard(r[i].v, &ear);
            }
            if (!owned) {
                control(&d, "flush\n");
                voices_restart(&d, r, n, now, true);
            }
            control(&d, "resume\n");
            d.paused = false;
            d.moved_at = now;
        }

        // A voice's queue that moved to another generation (its owner
        // flushed it for a seek): what the device holds of it is from
        // before. When it alone has frames there, they are flushed; with
        // others sounding, its last fifth of a second plays out under them.
        for (uint32_t i = 0; i < n; i++) {
            os64_media_output_voice_t *v = r[i].v;
            os64_lock_acquire(v->lock);
            uint32_t generation = v->queue->generation;
            uint32_t clock_generation = v->clock != NULL ? os64_media_clock_generation(v->clock) : 0;
            if (generation != v->priv.generation) {
                v->position = -1;   // the seek's owner says where it landed
                v->heard_all = false;
            }
            os64_lock_release(v->lock);
            if (generation == v->priv.generation)
                continue;
            bool others = false;
            for (uint32_t j = 0; j < n; j++)
                others |= j != i && unheard(r[j].v, &ear);
            if (!others && unheard(v, &ear)) {
                control(&d, "flush\n");
                voices_restart(&d, r, n, now, false);
            }
            v->priv.generation = generation;
            v->priv.clock_generation = clock_generation;
            v->priv.stale = false;
            os64_media_voice_reset(&v->priv.anchors);
            v->priv.last_heard = 0;
        }

        // A block's tail the device did not take goes first, and nothing new
        // is mixed until it has.
        bool failed = false;
        if (d.carry_frames > 0) {
            failed = !device_write(&d, NULL, 0);
            if (!failed && d.carry_frames == 0)
                carry_done(r, n);
        }

        // The block: what each voice playing has, up to WRITE_FRAMES, mixed.
        // A take never crosses a run of the queue (a loop's wrap), so a voice
        // takes again until the block is full or it has no more; each piece
        // is a run of its own, and no take starts a run its voice has no
        // anchor for. A voice held back by those budgets with more queued
        // would fall silent for the rest of a block another voice filled, so
        // the block is cut to what it can give (never below BLOCK_FLOOR).
        uint32_t block = 0;
        if (!failed && d.carry_frames == 0) {
            uint32_t limit = WRITE_FRAMES;
            for (uint32_t i = 0; i < n; i++) {
                os64_media_output_voice_t *v = r[i].v;
                uint32_t budget = d.position < 0 ? VOICE_PIECES
                                  : v->priv.anchors.anchors >= OS64_MEDIA_MIX_ANCHORS
                                      ? 0
                                      : OS64_MEDIA_MIX_ANCHORS - v->priv.anchors.anchors;
                if (budget > VOICE_PIECES)
                    budget = VOICE_PIECES;
                os64_lock_acquire(v->lock);
                uint32_t avail = 0, runs = v->queue->segments < budget ? v->queue->segments : budget;
                for (uint32_t k = 0; k < runs; k++)
                    avail += v->queue->segment[k].frames;
                bool held = r[i].plays && v->queue->segments > budget;
                os64_lock_release(v->lock);
                if (held && avail >= BLOCK_FLOOR && avail < limit)
                    limit = avail;
            }
            os64_memset(acc, 0, sizeof(int32_t) * 2 * WRITE_FRAMES);
            for (uint32_t i = 0; i < n; i++) {
                os64_media_output_voice_t *v = r[i].v;
                v->priv.pieces = 0;
                os64_lock_acquire(v->lock);
                // Taken under the same lock as the look at its generation, so
                // a flush between the two cannot slip a frame of the new
                // generation in under the old one's anchors.
                bool can = r[i].plays && v->wants && !v->remove && (v->clock == NULL || v->runs) &&
                           v->queue->generation == v->priv.generation;
                if (can && v->clock != NULL && v->priv.anchors.anchors == 0)
                    drop_behind(v, now);   // a start, or after a gap: from the clock's reading
                uint32_t filled = 0;
                while (can && filled < limit && v->priv.pieces < VOICE_PIECES &&
                       (v->priv.anchors.anchors + v->priv.pieces < OS64_MEDIA_MIX_ANCHORS || d.position < 0)) {
                    int64_t at;
                    uint32_t got = os64_media_queue_take(v->queue, buf, limit - filled, &at);
                    if (got == 0)
                        break;
                    os64_media_mix_add(acc + 2 * filled, buf, got, v->gain);
                    os64_media_voice_wrote(&v->priv.anchors, d.written + filled, at, got);
                    v->priv.piece[v->priv.pieces].offset = filled;
                    v->priv.piece[v->priv.pieces].frames = got;
                    v->priv.pieces++;
                    filled += got;
                }
                if (filled > 0 && v->room != NULL)
                    v->room(v->ctx);
                r[i].finished = os64_media_queue_finished(v->queue);
                if (filled > 0) {
                    v->written += filled;
                    v->unwritten = true;
                }
                os64_lock_release(v->lock);
                if (filled > block)
                    block = filled;
            }
            if (block > 0) {
                os64_media_mix_out(acc, buf, block);
                d.written += block;
                failed = !device_write(&d, buf, block);
                if (!failed && d.carry_frames == 0)
                    carry_done(r, n);
                os64_lock_acquire(&o->lock);
                o->facts.written += block;
                os64_lock_release(&o->lock);
            }
        } else {
            for (uint32_t i = 0; i < n; i++) {
                os64_lock_acquire(r[i].v->lock);
                r[i].finished = os64_media_queue_finished(r[i].v->queue);
                os64_lock_release(r[i].v->lock);
            }
        }
        if (failed) {
            refuse(o, &d, "the device failed", false, r, n, now);
            continue;
        }

        device_heard(&d, &ear, &lost);
        if (lost) {
            voices_restart(&d, r, n, now, false);
            continue;
        }
        hear(o, &d, r, n, &ear, now);
        os64_lock_acquire(&o->lock);
        o->facts.delay_us = d.delay_us;
        os64_lock_release(&o->lock);

        // THE STALL: sound written that the ear has not reached, the stream
        // playing (not paused, by this thread or anyone else), and the ear
        // still for the stall's time past the start's hold bound. It covers
        // a sink that stops pulling mid-play, one that never starts, and one
        // that does not start again after the device returns: in each the
        // stream says it plays and the ear does not move. A stream someone
        // else paused is not a stall: the sound is the master, and a clock
        // that follows it holds until it resumes.
        if (!d.paused && ear.known && d.written > ear.raw &&
            now - d.moved_at >= os64_media_output_hold_bound(d.delay_us) + OS64_MEDIA_OUTPUT_STALL_US) {
            refuse(o, &d, "the sink stopped", true, r, n, now);
            continue;
        }
        if (block == 0)
            (void)os64_bell_wait(&o->bell, 10);
    }
    device_close(&d, false);
    return 0;
}

int os64_media_output_create(os64_media_output_t **out)
{
    if (out == NULL)
        return OS64_MEDIA_BAD_ARGS;
    *out = NULL;
    os64_media_output_t *o = os64_malloc(sizeof(*o));
    if (o == NULL)
        return OS64_MEDIA_NO_MEMORY;
    *o = (os64_media_output_t){.bell = OS64_BELL_NONE};
    o->buf = os64_malloc(sizeof(int16_t) * 2 * WRITE_FRAMES);
    o->carry = os64_malloc(sizeof(int16_t) * 2 * WRITE_FRAMES);
    o->acc = os64_malloc(sizeof(int32_t) * 2 * WRITE_FRAMES);
    if (o->buf == NULL || o->carry == NULL || o->acc == NULL || os64_bell_make(&o->bell) < 0) {
        os64_free(o->buf);
        os64_free(o->carry);
        os64_free(o->acc);
        os64_free(o);
        return OS64_MEDIA_NO_MEMORY;
    }
    o->thread = os64_thread_named(audio_main, o, "audio-out");
    if (o->thread < 0) {
        // A start that reports failure may still be running (the kernel,
        // out of handles, starts it detached): it keeps the output, which is
        // never freed here.
        return OS64_MEDIA_NO_MEMORY;
    }
    *out = o;
    return 0;
}

int os64_media_output_destroy(os64_media_output_t *o)
{
    if (o == NULL)
        return OS64_MEDIA_BAD_ARGS;
    os64_lock_acquire(&o->lock);
    if (o->voices != NULL) {
        os64_lock_release(&o->lock);
        return OS64_MEDIA_BAD_ARGS;
    }
    o->quit = true;
    (void)os64_bell_ring(&o->bell);
    os64_lock_release(&o->lock);
    (void)os64_thread_join((int32_t)o->thread, NULL);
    os64_close((int32_t)o->thread);   // a join only reads the handle
    os64_bell_close(&o->bell);
    os64_free(o->buf);
    os64_free(o->carry);
    os64_free(o->acc);
    os64_free(o);
    return 0;
}

int os64_media_output_add(os64_media_output_t *o, os64_media_output_voice_t *v)
{
    if (o == NULL || v == NULL || v->lock == NULL || v->queue == NULL)
        return OS64_MEDIA_BAD_ARGS;
    os64_lock_acquire(&o->lock);
    v->priv = (__typeof__(v->priv)){0};
    v->priv.next = o->voices;
    o->voices = v;
    o->facts.voices++;
    (void)os64_bell_ring(&o->bell);
    os64_lock_release(&o->lock);
    return 0;
}

void os64_media_output_ring(os64_media_output_t *o)
{
    if (o == NULL)
        return;
    os64_lock_acquire(&o->lock);
    (void)os64_bell_ring(&o->bell);
    os64_lock_release(&o->lock);
}

void os64_media_output_facts(os64_media_output_t *o, os64_media_output_facts_t *out)
{
    os64_lock_acquire(&o->lock);
    *out = o->facts;
    os64_lock_release(&o->lock);
}
