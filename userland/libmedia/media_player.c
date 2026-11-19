// media_player.c — one media playing (os64/media_player.h).
//
// THE THREADS AND THE LOCK.
//
//   caller    every public call: the controls, the size, `frame` (the
//             pace's pick and judge, the picture on the screen, `shown`)
//             and the facts.
//   decoding  opens the media, then every libmedia call but release and
//             the scaler: pulls with os64_media_next, asking only for the
//             kinds its queues have room for, and pushes into the decoded
//             queue and the sound queue; carries out seeks and the pace's
//             skip and resync. It starts the picture thread, waits for it
//             at the end, and is the last out: it frees the player.
//   picture   the scaler and the frames: takes from the decoded queue,
//             skips what the pace says is overdue, converts to the size
//             asked, pushes into the ready queue; and with a timed wait,
//             retires what the clock passed while nobody asks for frames.
//   output's  the voice (os64/media_output.h): takes from the sound queue,
//             reports what the ear heard to the clock, says what it was told.
//
// ONE LOCK, the player's, guards everything two threads touch. It is never
// held across a libmedia call, a conversion, or a call into the output (a
// change the output must see sets `ring_output`, rung once the lock is let
// go). Pictures to give back to libmedia are COLLECTED under it and released
// after. Bells are rung with it held (os64/bell.h's rule), and so is `look`.

#include "os64/media_player.h"

#include "os64/audio.h"
#include "os64/bell.h"
#include "os64/lock.h"
#include "os64/media_pictures.h"
#include "os64/media_queue.h"
#include "os64/os64.h"
#include "os64/thread.h"

#define RATE OS64_AUDIO_RATE
#define DECODED_DEPTH 8
#define READY_DEPTH 3
#define FRAMES (READY_DEPTH + 3)          // the ready ones, the one shown, one being converted, one reconverted
#define PCM_FRAMES (2u * RATE)            // two seconds of sound queued
#define CHUNK_FRAMES 4096u                // what the decoding thread asks for at once
#define BUFFER_SOUND (RATE / 2u)          // buffering ends with this much sound queued
#define SOUND_FLOOR (RATE / 4u)           // under it is far behind (the pace's twin door)
#define COLLECT 64                        // a seek's flush plus a paint's drops, with room
// A report's trust (os64_media_clock_still): longer than the oldest a
// healthy report can be when the caller reads the clock. The A2DP pump
// pulls a packet (640 frames, 13.3 ms; the null sink pulls each tick, less)
// in kusb's pass, which may come a tick late; the output reads the position
// after a write that waits up to its patience; the caller reads for the
// blank a period ahead; and a margin.
#define PULL_US 14000
#define PASS_LATE_US 10000
#define STILL_MARGIN_US 20000
#define PERIOD_GUESS_US 16667             // before the first frame call: 60 Hz

typedef struct frame {
    uint32_t *pixels;
    size_t capacity;                      // pixels allocated
    uint32_t w, h;
    int64_t at, duration;
    os64_media_picture_t *source;         // kept until retired: a resize converts it again
    struct frame *next_free;
} frame_t;

typedef struct {
    os64_media_picture_t *picture[COLLECT];
    uint32_t n;
} collect_t;

struct os64_media_player {
    os64_lock_t lock;
    os64_media_player_config_t cfg;
    os64_media_t *media;
    os64_media_info_t info;
    bool probed, has_sound, has_picture;
    int64_t length;

    os64_media_pictures_entry_t decoded_slot[DECODED_DEPTH], ready_slot[READY_DEPTH];
    os64_media_pictures_t decoded, ready;
    int16_t *pcm_storage;
    os64_media_queue_t pcm;
    os64_media_clock_t clock;
    os64_media_pace_t pace;
    frame_t frame[FRAMES];
    frame_t *free_frames;
    frame_t *shown;                       // the caller's: the picture on the screen
    frame_t *reconverted;                 // the shown one converted again at a new size
    uint32_t shown_serial, told_serial;

    int state;
    bool want_play;                       // asked while opening
    bool owed;                            // show the first ready picture at the next frame call
    bool quit;
    uint32_t seek_serial;
    int64_t seek_target;
    bool seek_pending;
    uint32_t seek_generation, seeked;
    bool muted, sound_over, loaded, ring_output;
    int64_t sync;
    uint32_t pic_w, pic_h;
    bool reconvert;
    int64_t newest;
    int failure;
    int64_t last_blank, period_us, last_frame_call, conversion_us;
    bool converting;
    bool picture_done;                    // the picture thread's last touch
    int64_t picture_thread;

    os64_bell_t decode_bell, picture_bell;
    os64_media_output_voice_t voice;
    bool voiced;

    uint64_t pcm_pushed, pcm_flushed, pictures_pushed, conversions, reconversions, buffers;
    os64_media_counts_t media_counts;     // the decoding thread's snapshot (os64_media_counts is its call)
};

static int64_t now_us(void)
{
    return (int64_t)os64_micros();
}

static void look(os64_media_player_t *p)
{
    if (p->cfg.look != NULL)
        p->cfg.look(p->cfg.ctx);
}

// ---------------------------------------------------------------- collecting releases

static void collect_add(collect_t *c, os64_media_picture_t *pic)
{
    if (pic != NULL && c->n < COLLECT)
        c->picture[c->n++] = pic;
}

static void collect_release(os64_media_player_t *p, collect_t *c)
{
    for (uint32_t i = 0; i < c->n; i++)
        os64_media_picture_release(p->media, c->picture[i]);
    c->n = 0;
}

static void release_decoded(void *context, void *handle)
{
    collect_add(context, handle);
}

// A frame back on the free list, its source collected. Under the lock.
static void frame_retire(os64_media_player_t *p, frame_t *f, collect_t *c)
{
    if (f == NULL)
        return;
    collect_add(c, f->source);
    f->source = NULL;
    f->next_free = p->free_frames;
    p->free_frames = f;
    (void)os64_bell_ring(&p->picture_bell);
}

static void release_ready(void *context, void *handle)
{
    void **pair = context;
    frame_retire(pair[0], handle, pair[1]);
}

// ---------------------------------------------------------------- the rules

static int64_t still_bound(const os64_media_player_t *p)
{
    int64_t period = p->period_us > 0 ? p->period_us : PERIOD_GUESS_US;
    return PULL_US + PASS_LATE_US + (int64_t)OS64_MEDIA_OUTPUT_WRITE_PATIENCE_MS * 1000 + period + STILL_MARGIN_US;
}

static bool audible(const os64_media_player_t *p)
{
    return p->has_sound && p->voiced && !p->muted && !p->sound_over && !p->voice.refused_device;
}

// What the voice asks of the output, from the state; a change is rung once
// the lock is let go. Under the lock.
static void voice_asks(os64_media_player_t *p)
{
    if (!p->voiced)
        return;
    bool wants = p->has_sound && !p->muted && !p->sound_over && !p->quit;
    bool runs = p->state == OS64_MEDIA_PLAYER_PLAYING || p->state == OS64_MEDIA_PLAYER_ENDED;
    if (wants != p->voice.wants || runs != p->voice.runs || p->sync != p->voice.sync || p->quit != p->voice.remove)
        p->ring_output = true;
    p->voice.wants = wants;
    p->voice.runs = runs;
    p->voice.sync = p->sync;
    p->voice.remove = p->quit;
}

// Lets go of the lock, and rings the output if the voice's asks changed.
static void unlock(os64_media_player_t *p)
{
    bool ring = p->ring_output;
    p->ring_output = false;
    os64_lock_release(&p->lock);
    if (ring)
        os64_media_output_ring(p->cfg.output);
}

static void start_playing(os64_media_player_t *p, int64_t now)
{
    os64_media_clock_hold(&p->clock, os64_media_output_hold_bound(p->voice.delay_us));
    os64_media_clock_still(&p->clock, still_bound(p));
    os64_media_clock_play(&p->clock, now);
    p->state = OS64_MEDIA_PLAYER_PLAYING;
    voice_asks(p);
}

// A seek to `target`: every queue flushed, the clock and the pace told, the
// decoding thread asked. Under the lock.
static uint32_t seek_to(os64_media_player_t *p, int64_t target, collect_t *c, int64_t now, bool play)
{
    void *ready_context[2] = {p, c};
    os64_media_pictures_flush(&p->decoded, release_decoded, c);
    os64_media_pictures_flush(&p->ready, release_ready, ready_context);
    p->pcm_flushed += os64_media_queue_count(&p->pcm);
    os64_media_queue_flush(&p->pcm);
    if (os64_media_clock_state(&p->clock, now) != OS64_MEDIA_CLOCK_PAUSED)
        os64_media_clock_pause(&p->clock, now);
    os64_media_clock_seek(&p->clock, target, now);
    os64_media_pace_reset(&p->pace, now);
    p->seek_target = target;
    p->seek_serial++;
    p->seek_pending = true;
    p->seek_generation++;
    p->newest = -1;
    p->owed = true;
    p->sound_over = false;
    // A reconversion of the picture from before the seek is not shown after it.
    frame_retire(p, p->reconverted, c);
    p->reconverted = NULL;
    p->reconvert = false;
    p->state = play ? OS64_MEDIA_PLAYER_BUFFERING : OS64_MEDIA_PLAYER_PAUSED;
    (void)os64_bell_ring(&p->decode_bell);
    (void)os64_bell_ring(&p->picture_bell);
    voice_asks(p);
    return p->seek_generation;
}

// The moves between states no call makes: buffering's end, a sound that
// stopped, the end. Run under the lock by every thread that may have made
// one true, and in every public call; `look` when the state moved.
static void service(os64_media_player_t *p, int64_t now)
{
    int was = p->state;
    if (p->failure != 0 && p->state != OS64_MEDIA_PLAYER_FAILED) {
        if (os64_media_clock_state(&p->clock, now) != OS64_MEDIA_CLOCK_PAUSED)
            os64_media_clock_pause(&p->clock, now);
        p->state = OS64_MEDIA_PLAYER_FAILED;
        voice_asks(p);
    } else if (p->state == OS64_MEDIA_PLAYER_BUFFERING) {
        bool picture = !p->has_picture || os64_media_pictures_count(&p->ready) > 0 ||
                       (p->shown != NULL && !p->owed) || os64_media_pictures_finished(&p->decoded);
        // The sound is ready with its depth queued (or its end), and once the
        // sink's delay is known, which the start's hold bound is made from.
        bool sound = !audible(p) ||
                     ((os64_media_queue_count(&p->pcm) >= BUFFER_SOUND || os64_media_queue_ended(&p->pcm)) &&
                      p->voice.delay_known);
        if (picture && sound)
            start_playing(p, now);
    } else if (p->state == OS64_MEDIA_PLAYER_PLAYING) {
        // A sound that stopped, said by the device (starved): at its end the
        // picture goes on monotonic; with the queue run dry the decoder fell
        // behind and a hold is buffering. A sink that stops pulling, never
        // starts, or does not start again is the output's to refuse; a still
        // ear is waited out until it moves, and a stream someone else paused
        // holds the picture until it resumes.
        if (os64_media_clock_state(&p->clock, now) == OS64_MEDIA_CLOCK_STARVED) {
            if (os64_media_queue_finished(&p->pcm)) {
                p->sound_over = true;
                os64_media_clock_source(&p->clock, OS64_MEDIA_CLOCK_MONOTONIC, now);
                voice_asks(p);
            } else if (os64_media_queue_count(&p->pcm) == 0) {
                os64_media_clock_pause(&p->clock, now);
                p->state = OS64_MEDIA_PLAYER_BUFFERING;
                p->buffers++;
                voice_asks(p);
            }
        }
        // The end: every picture shown (none in a queue or being converted)
        // and every frame of sound given to the device.
        bool pictures_done = !p->has_picture || (os64_media_pictures_finished(&p->decoded) &&
                                                 os64_media_pictures_count(&p->ready) == 0 && !p->converting);
        bool sound_done = !p->has_sound || (os64_media_queue_finished(&p->pcm) && !p->voice.unwritten);
        if (p->state == OS64_MEDIA_PLAYER_PLAYING && pictures_done && sound_done) {
            int64_t end = -1;
            if (p->shown != NULL) {
                int64_t lasts = p->shown->duration > 0 ? p->shown->duration : p->pace.period;
                end = p->shown->at + lasts;
            }
            if (end < 0 || os64_media_clock_read(&p->clock, now) >= end) {
                os64_media_clock_pause(&p->clock, now);
                p->state = OS64_MEDIA_PLAYER_ENDED;
                voice_asks(p);
            }
        }
    }
    if (p->state != was)
        look(p);
}

// The output's audio thread: the voice's facts moved. Under the lock.
static void voice_look(void *ctx)
{
    os64_media_player_t *p = ctx;
    service(p, now_us());
    look(p);
}

static void voice_room(void *ctx)
{
    os64_media_player_t *p = ctx;
    (void)os64_bell_ring(&p->decode_bell);
}

// The pace's pick at `clock`, the picture owed shown whatever its time, and
// a reconversion swapped in. Under the lock.
static void pick(os64_media_player_t *p, int64_t clock, bool judging, collect_t *c)
{
    if (p->reconverted != NULL && !p->owed) {
        frame_t *old = p->shown;
        p->shown_serial++;
        p->shown = p->reconverted;
        p->reconverted = NULL;
        frame_retire(p, old, c);
        return;
    }
    os64_media_pictures_entry_t shown_entry;
    const os64_media_pictures_entry_t *shown = NULL;
    if (p->shown != NULL) {
        shown_entry = (os64_media_pictures_entry_t){p->shown, p->shown->at, p->shown->duration};
        shown = &shown_entry;
    }
    os64_media_pace_pick_t k = os64_media_pace_pick(&p->pace, &p->ready, clock, shown, judging);
    if (!k.show && p->owed && os64_media_pictures_count(&p->ready) > 0)
        k = (os64_media_pace_pick_t){true, 0, false};
    if (!k.show)
        return;
    for (uint32_t i = 0; i < k.drop; i++) {
        os64_media_pictures_entry_t e;
        os64_media_pictures_take(&p->ready, &e);
        frame_retire(p, e.handle, c);
    }
    os64_media_pictures_entry_t e;
    os64_media_pictures_take(&p->ready, &e);
    frame_t *old = p->shown;
    p->shown_serial++;
    p->shown = e.handle;
    if (old != NULL)
        frame_retire(p, old, c);
    if (p->owed) {
        p->owed = false;
        p->seek_pending = false;
        p->seeked = p->seek_generation;
        look(p);
    }
}

// The producers' state, once a paint. Under the lock.
static void judge(os64_media_player_t *p, int64_t clock, int64_t now)
{
    bool judging = p->state == OS64_MEDIA_PLAYER_PLAYING && !os64_media_pictures_ended(&p->decoded);
    bool sound_counts = audible(p) && !os64_media_queue_ended(&p->pcm) && !os64_media_pictures_ended(&p->decoded);
    os64_media_pace_measure_t m = {now, clock, judging, p->newest, os64_media_queue_count(&p->pcm),
                                   p->state == OS64_MEDIA_PLAYER_PLAYING && sound_counts ? SOUND_FLOOR : 0};
    bool skip_was = p->pace.skip_unused;
    os64_media_pace_judge(&p->pace, &m);
    if (p->pace.skip_unused != skip_was || p->pace.resync_asked)
        (void)os64_bell_ring(&p->decode_bell);
}

// Nobody has asked for a frame for a while: what the clock passed is retired
// from the ready queue (counted dropped), the latest one due kept for the
// next frame call, the shown picture left alone; at the end of the pictures
// the last one goes too once its time is over, or the end would never come.
// Under the lock.
static void retire_unasked(os64_media_player_t *p, int64_t now, collect_t *c)
{
    if (p->state != OS64_MEDIA_PLAYER_PLAYING || now - p->last_frame_call < OS64_MEDIA_PLAYER_TICK_US)
        return;
    int64_t clock = os64_media_clock_read(&p->clock, now);
    if (p->owed && os64_media_pictures_count(&p->ready) > 0) {
        // The picture owed after a seek (or the open) has nobody to show it:
        // the seek has landed all the same, and the pictures go by the clock.
        p->owed = false;
        p->seek_pending = false;
        p->seeked = p->seek_generation;
        look(p);
    }
    const os64_media_pictures_entry_t *next;
    while (os64_media_pictures_count(&p->ready) > 1 && (next = os64_media_pictures_peek(&p->ready, 1)) != NULL &&
           next->at <= clock) {
        os64_media_pictures_entry_t e;
        os64_media_pictures_take(&p->ready, &e);
        frame_retire(p, e.handle, c);
        p->pace.dropped++;
    }
    const os64_media_pictures_entry_t *last = os64_media_pictures_peek(&p->ready, 0);
    if (last != NULL && os64_media_pictures_count(&p->ready) == 1 && os64_media_pictures_finished(&p->decoded)) {
        int64_t lasts = last->duration > 0 ? last->duration : p->pace.period;
        if (last->at + lasts <= clock) {
            os64_media_pictures_entry_t e;
            os64_media_pictures_take(&p->ready, &e);
            frame_retire(p, e.handle, c);
            p->pace.dropped++;
        }
    }
}

// ---------------------------------------------------------------- the picture thread

static bool frame_fit(frame_t *f, uint32_t w, uint32_t h)
{
    size_t need = (size_t)w * h;
    if (need <= f->capacity)
        return true;
    os64_free(f->pixels);
    f->pixels = os64_malloc_aligned(need * sizeof(uint32_t), 64);
    f->capacity = f->pixels != NULL ? need : 0;
    return f->pixels != NULL;
}

static int64_t picture_thread(void *arg)
{
    os64_media_player_t *p = arg;
    os64_media_scaler_t *scaler = NULL;
    uint32_t scaler_w = 0, scaler_h = 0;
    collect_t c = {0};
    os64_lock_acquire(&p->lock);
    while (!p->quit) {
        int64_t now = now_us();
        retire_unasked(p, now, &c);
        service(p, now);
        // A resize while not playing: the shown picture again, at the new
        // size. A shown frame whose source is out (a reconversion in flight,
        // its result not yet taken) waits for the result to be swapped in.
        bool again = p->reconvert && p->shown != NULL && p->shown->source != NULL && p->free_frames != NULL;
        if (p->reconvert && p->shown == NULL)
            p->reconvert = false;
        const os64_media_pictures_entry_t *front = os64_media_pictures_peek(&p->decoded, 0);
        if (!again && (front == NULL || os64_media_pictures_full(&p->ready) || p->free_frames == NULL ||
                       p->pic_w == 0)) {
            // A TIMED wait while playing: nothing else wakes this thread to
            // retire what nobody asks for (a muted or silent media has no
            // audio round to ring it).
            uint64_t wait = p->state == OS64_MEDIA_PLAYER_PLAYING ? OS64_MEDIA_PLAYER_TICK_US / 1000 : 200;
            unlock(p);
            collect_release(p, &c);
            (void)os64_bell_wait(&p->picture_bell, wait);
            os64_lock_acquire(&p->lock);
            continue;
        }
        os64_media_picture_t *source;
        int64_t at, duration;
        frame_t *shown_from = NULL;
        if (again) {
            // The source goes with the conversion, so a retire of the shown
            // frame meanwhile cannot release it under the scaler.
            p->reconvert = false;
            shown_from = p->shown;
            source = shown_from->source;
            shown_from->source = NULL;
            at = shown_from->at;
            duration = shown_from->duration;
        } else {
            // Skip what is overdue at the blank this conversion could reach.
            int64_t reach = os64_media_pace_reach(now, p->conversion_us, p->last_blank, p->period_us);
            int64_t clock = os64_media_clock_read(&p->clock, reach);
            while (os64_media_pace_skip(&p->pace, &p->decoded, clock)) {
                os64_media_pictures_entry_t e;
                os64_media_pictures_take(&p->decoded, &e);
                collect_add(&c, e.handle);
                (void)os64_bell_ring(&p->decode_bell);
            }
            os64_media_pictures_entry_t e;
            os64_media_pictures_take(&p->decoded, &e);
            (void)os64_bell_ring(&p->decode_bell);
            source = e.handle;
            at = e.at;
            duration = e.duration;
        }
        frame_t *f = p->free_frames;
        p->free_frames = f->next_free;
        uint32_t w = p->pic_w, h = p->pic_h, gen = os64_media_pictures_generation(&p->ready);
        p->converting = true;
        unlock(p);

        collect_release(p, &c);
        bool made = false;
        int64_t t0 = now_us();
        if (w != scaler_w || h != scaler_h || scaler == NULL) {
            os64_media_scaler_destroy(scaler);
            scaler = NULL;
            if (os64_media_scaler_create(w, h, OS64_MEDIA_SCALE_FAST, &scaler) == 0) {
                scaler_w = w;
                scaler_h = h;
            }
        }
        if (scaler != NULL && frame_fit(f, w, h))
            made = os64_media_scaler_convert(scaler, source, f->pixels, w * 4) == 0;
        int64_t cost = now_us() - t0;

        os64_lock_acquire(&p->lock);
        p->converting = false;
        p->conversion_us = cost;
        f->w = w;
        f->h = h;
        f->at = at;
        f->duration = duration;
        f->source = source;
        if (shown_from != NULL) {
            // To the caller directly while the frame it replaces is still
            // shown; otherwise (a seek or a play moved on) the source goes
            // back to it if it is still shown, or is released with this one.
            p->reconversions++;
            if (made && p->shown == shown_from && p->state != OS64_MEDIA_PLAYER_PLAYING &&
                gen == os64_media_pictures_generation(&p->ready)) {
                frame_retire(p, p->reconverted, &c);
                p->reconverted = f;
                look(p);
            } else {
                if (p->shown == shown_from && shown_from->source == NULL) {
                    shown_from->source = source;
                    f->source = NULL;
                }
                frame_retire(p, f, &c);
            }
            continue;
        }
        p->conversions++;
        int pushed = made ? os64_media_pictures_push(&p->ready, gen, f, at, duration) : OS64_MEDIA_PICTURES_REFUSED;
        if (pushed != OS64_MEDIA_PICTURES_TAKEN)
            frame_retire(p, f, &c);
        if (pushed == OS64_MEDIA_PICTURES_TAKEN) {
            service(p, now_us());
            if (p->owed || p->state != OS64_MEDIA_PLAYER_PLAYING)
                look(p);
        }
    }
    p->picture_done = true;
    (void)os64_bell_ring(&p->decode_bell);
    unlock(p);
    collect_release(p, &c);
    os64_media_scaler_destroy(scaler);
    return 0;
}

// ---------------------------------------------------------------- the decoding thread

// The media is open: the voice is added, the pace and the clock are set,
// and the state leaves OPENING. Not under the lock.
static void opened(os64_media_player_t *p)
{
    bool sound = p->info.sound == OS64_MEDIA_PLAYABLE && (p->cfg.want & OS64_MEDIA_WANT_SOUND) != 0 &&
                 p->cfg.output != NULL;
    bool picture = p->info.picture == OS64_MEDIA_PLAYABLE && (p->cfg.want & OS64_MEDIA_WANT_PICTURE) != 0;
    bool voiced = false;
    if (sound && p->cfg.output != NULL) {
        p->voice = (os64_media_output_voice_t){.lock = &p->lock, .queue = &p->pcm, .clock = &p->clock,
                                               .refused = p->cfg.refused, .look = voice_look, .room = voice_room,
                                               .ctx = p, .gain = 1.0, .position = -1};
        voiced = os64_media_output_add(p->cfg.output, &p->voice) == 0;
    }
    os64_lock_acquire(&p->lock);
    if (sound && !voiced && p->failure == 0)
        p->failure = OS64_MEDIA_NO_MEMORY;   // its queue would fill with nothing to drain it
    p->has_sound = sound;
    p->has_picture = picture;
    p->voiced = voiced;
    p->length = p->info.frames;
    int64_t period = p->info.rate_num > 0 ? (int64_t)RATE * p->info.rate_den / p->info.rate_num : 0;
    os64_media_pace_period(&p->pace, period);
    os64_media_clock_source(&p->clock, voiced ? OS64_MEDIA_CLOCK_HEARD : OS64_MEDIA_CLOCK_MONOTONIC, now_us());
    p->probed = true;
    p->state = p->want_play ? OS64_MEDIA_PLAYER_BUFFERING : OS64_MEDIA_PLAYER_PAUSED;
    voice_asks(p);
    look(p);
    (void)os64_bell_ring(&p->picture_bell);
    unlock(p);
}

// The last out frees everything: every picture back before the media
// closes (on this thread, FFmpeg's pools' rule), then the player.
static void finish(os64_media_player_t *p)
{
    collect_t c = {0};
    void *ready_context[2] = {p, &c};
    os64_lock_acquire(&p->lock);
    os64_media_pictures_flush(&p->decoded, release_decoded, &c);
    os64_lock_release(&p->lock);
    collect_release(p, &c);
    os64_lock_acquire(&p->lock);
    os64_media_pictures_flush(&p->ready, release_ready, ready_context);
    frame_retire(p, p->shown, &c);
    frame_retire(p, p->reconverted, &c);
    p->shown = p->reconverted = NULL;
    os64_lock_release(&p->lock);
    collect_release(p, &c);
    for (int i = 0; i < FRAMES; i++)
        os64_free(p->frame[i].pixels);
    if (p->media != NULL)
        os64_media_close(p->media);
    void (*finished)(void *) = p->cfg.finished;
    void *ctx = p->cfg.ctx;
    os64_bell_close(&p->decode_bell);
    os64_bell_close(&p->picture_bell);
    os64_free(p->pcm_storage);
    os64_free(p);
    if (finished != NULL)
        finished(ctx);
}

static int64_t decoding_thread(void *arg)
{
    os64_media_player_t *p = arg;
    // With no output the sound is not opened: nothing would drain its queue.
    uint32_t want = p->cfg.output != NULL ? p->cfg.want : p->cfg.want & ~OS64_MEDIA_WANT_SOUND;
    int r = want != 0 ? os64_media_open_for(&p->cfg.source, want, &p->media, &p->info) : OS64_MEDIA_BAD_ARGS;
    if (r == 0 && (p->cfg.want & OS64_MEDIA_WANT_PICTURE) != 0)
        p->picture_thread = os64_thread(picture_thread, p);
    if (r != 0) {
        p->media = NULL;
        os64_lock_acquire(&p->lock);
        p->failure = r;
        p->probed = true;
        service(p, now_us());
        look(p);
        unlock(p);
    } else {
        opened(p);
    }
    int16_t *pcm = os64_malloc(sizeof(int16_t) * 2 * CHUNK_FRAMES);
    uint32_t served = 0, gen_decoded = 0, gen_pcm = 0;
    int applied_skip = OS64_MEDIA_SKIP_NONE;
    // GOT_END is the end of the kinds ASKED for: asked for sound alone, the
    // sound has ended and pictures may still be listed. Each kind ends on
    // its own, and the media when both have.
    bool resync_waiting = false, sound_done = false, pictures_done = false;
    uint32_t full = 0;                    // GOT_FULL's kind: park until it has room
    collect_t c = {0};
    os64_lock_acquire(&p->lock);
    while (!p->quit && pcm != NULL && p->media != NULL) {
        if (p->seek_serial != served) {
            served = p->seek_serial;
            int64_t target = p->seek_target;
            gen_decoded = os64_media_pictures_generation(&p->decoded);
            gen_pcm = p->pcm.generation;
            sound_done = pictures_done = false;
            full = 0;
            resync_waiting = false;
            unlock(p);
            int sought = os64_media_seek(p->media, target);
            os64_lock_acquire(&p->lock);
            if (sought < 0 && p->failure == 0) {
                p->failure = sought;
                service(p, now_us());
            }
            continue;
        }
        int want_skip = p->pace.skip_unused ? OS64_MEDIA_SKIP_UNUSED : OS64_MEDIA_SKIP_NONE;
        bool resync = os64_media_pace_resync_take(&p->pace);
        if (want_skip != applied_skip || resync) {
            unlock(p);
            if (want_skip != applied_skip)
                os64_media_picture_skip(p->media, want_skip);
            if (resync)
                os64_media_picture_resync(p->media);
            os64_lock_acquire(&p->lock);
            applied_skip = want_skip;
            resync_waiting |= resync;
            continue;
        }
        uint32_t accept = 0;
        if (p->has_sound && !sound_done && os64_media_queue_room(&p->pcm) >= CHUNK_FRAMES)
            accept |= OS64_MEDIA_WANT_SOUND;
        if (p->has_picture && !pictures_done && !os64_media_pictures_full(&p->decoded))
            accept |= OS64_MEDIA_WANT_PICTURE;
        if (full != 0 && (accept & full) == 0)
            accept = 0;                   // park until the kind libmedia named has room
        if (accept == 0 || p->failure != 0) {
            service(p, now_us());
            unlock(p);
            (void)os64_bell_wait(&p->decode_bell, 200);
            os64_lock_acquire(&p->lock);
            continue;
        }
        full = 0;
        unlock(p);
        os64_media_got_t got = {0};
        r = os64_media_next(p->media, accept, pcm, CHUNK_FRAMES, &got);
        os64_media_counts_t counts;
        os64_media_counts(p->media, &counts);
        os64_lock_acquire(&p->lock);
        p->media_counts = counts;
        if (r < 0) {
            if (got.kind == OS64_MEDIA_GOT_PICTURE && got.picture != NULL)
                collect_add(&c, got.picture);
            if (p->failure == 0)
                p->failure = r;
        } else if (got.kind == OS64_MEDIA_GOT_SOUND) {
            p->pcm_pushed += os64_media_queue_push(&p->pcm, gen_pcm, pcm, got.frames, got.at);
            if (!p->has_picture && p->seek_pending && os64_media_queue_count(&p->pcm) > 0) {
                // With no picture, a seek has landed once sound from there is queued.
                p->seek_pending = false;
                p->owed = false;
                p->seeked = p->seek_generation;
                look(p);
            }
        } else if (got.kind == OS64_MEDIA_GOT_PICTURE) {
            int pushed = os64_media_pictures_push(&p->decoded, gen_decoded, got.picture, got.picture->at,
                                                  got.picture->duration);
            if (pushed == OS64_MEDIA_PICTURES_TAKEN) {
                p->pictures_pushed++;
                p->newest = got.picture->at;
                if (resync_waiting) {
                    os64_media_pace_resynced(&p->pace, now_us());
                    resync_waiting = false;
                }
                (void)os64_bell_ring(&p->picture_bell);
            } else {
                collect_add(&c, got.picture);   // REFUSED: a seek came between
            }
        } else if (got.kind == OS64_MEDIA_GOT_FULL) {
            full = got.full;
        } else {
            if ((accept & OS64_MEDIA_WANT_SOUND) != 0) {
                sound_done = true;
                if (gen_pcm == p->pcm.generation)
                    os64_media_queue_end(&p->pcm);
            }
            if ((accept & OS64_MEDIA_WANT_PICTURE) != 0) {
                pictures_done = true;
                os64_media_pictures_end(&p->decoded, gen_decoded);
            }
            (void)os64_bell_ring(&p->picture_bell);
            look(p);
        }
        service(p, now_us());
        if (c.n > 0) {
            unlock(p);
            collect_release(p, &c);
            os64_lock_acquire(&p->lock);
        }
    }
    // Closing: the voice goes, the picture thread ends, and the output lets
    // go of the voice; the last of them is waited for here, then everything
    // is freed. An output that never lets go keeps the player (never freed).
    p->quit = true;
    voice_asks(p);
    (void)os64_bell_ring(&p->picture_bell);
    unlock(p);
    os64_free(pcm);
    if (p->picture_thread > 0)
        (void)os64_thread_join((int32_t)p->picture_thread, NULL);
    for (;;) {
        os64_lock_acquire(&p->lock);
        bool gone = !p->voiced || p->voice.gone;
        os64_lock_release(&p->lock);
        if (gone)
            break;
        os64_media_output_ring(p->cfg.output);
        (void)os64_sleep(10);
    }
    finish(p);
    return 0;
}

// ---------------------------------------------------------------- the caller's calls

int os64_media_player_create(const os64_media_player_config_t *cfg, os64_media_player_t **out)
{
    if (cfg == NULL || out == NULL || cfg->source.read == NULL || cfg->want == 0)
        return OS64_MEDIA_BAD_ARGS;
    *out = NULL;
    os64_media_player_t *p = os64_calloc(1, sizeof(*p));
    if (p == NULL)
        return OS64_MEDIA_NO_MEMORY;
    p->cfg = *cfg;
    p->decode_bell = (os64_bell_t)OS64_BELL_NONE;
    p->picture_bell = (os64_bell_t)OS64_BELL_NONE;
    p->pcm_storage = os64_malloc(sizeof(int16_t) * 2 * PCM_FRAMES);
    if (p->pcm_storage == NULL || os64_bell_make(&p->decode_bell) < 0 || os64_bell_make(&p->picture_bell) < 0) {
        os64_bell_close(&p->decode_bell);
        os64_bell_close(&p->picture_bell);
        os64_free(p->pcm_storage);
        os64_free(p);
        return OS64_MEDIA_NO_MEMORY;
    }
    os64_media_pictures_init(&p->decoded, p->decoded_slot, DECODED_DEPTH);
    os64_media_pictures_init(&p->ready, p->ready_slot, READY_DEPTH);
    os64_media_queue_init(&p->pcm, p->pcm_storage, PCM_FRAMES);
    os64_media_clock_init(&p->clock, OS64_MEDIA_CLOCK_MONOTONIC);
    os64_media_pace_init(&p->pace, DECODED_DEPTH, 0);
    for (int i = 0; i < FRAMES; i++) {
        p->frame[i].next_free = p->free_frames;
        p->free_frames = &p->frame[i];
    }
    p->state = OS64_MEDIA_PLAYER_OPENING;
    p->owed = true;
    p->newest = -1;
    p->length = -1;
    p->loaded = cfg->loaded;
    p->last_frame_call = now_us();
    // The decoding thread is never joined: it frees the player as its last
    // act. A start that reports failure may still be running (the kernel,
    // out of handles, starts it detached): the player is then left to it,
    // never freed here.
    int64_t h = os64_thread(decoding_thread, p);
    if (h < 0)
        return OS64_MEDIA_NO_MEMORY;
    os64_close((int32_t)h);
    *out = p;
    return 0;
}

void os64_media_player_close(os64_media_player_t *p)
{
    if (p == NULL)
        return;
    os64_lock_acquire(&p->lock);
    p->quit = true;
    voice_asks(p);
    (void)os64_bell_ring(&p->decode_bell);
    (void)os64_bell_ring(&p->picture_bell);
    unlock(p);
}

void os64_media_player_play(os64_media_player_t *p)
{
    collect_t c = {0};
    int64_t now = now_us();
    os64_lock_acquire(&p->lock);
    p->want_play = true;
    if (p->state == OS64_MEDIA_PLAYER_PAUSED) {
        if (p->seek_pending || (p->has_picture && os64_media_pictures_count(&p->ready) == 0 && p->shown == NULL))
            p->state = OS64_MEDIA_PLAYER_BUFFERING;
        else
            start_playing(p, now);
    } else if (p->state == OS64_MEDIA_PLAYER_ENDED) {
        (void)seek_to(p, 0, &c, now, true);
    }
    service(p, now);
    voice_asks(p);
    unlock(p);
    collect_release(p, &c);
}

void os64_media_player_pause(os64_media_player_t *p)
{
    int64_t now = now_us();
    os64_lock_acquire(&p->lock);
    p->want_play = false;
    if (p->state == OS64_MEDIA_PLAYER_PLAYING) {
        os64_media_clock_pause(&p->clock, now);
        p->state = OS64_MEDIA_PLAYER_PAUSED;
    } else if (p->state == OS64_MEDIA_PLAYER_BUFFERING) {
        p->state = OS64_MEDIA_PLAYER_PAUSED;
    }
    voice_asks(p);
    unlock(p);
}

static uint32_t seek_locked(os64_media_player_t *p, int64_t target, collect_t *c, int64_t now)
{
    if (target < 0)
        target = 0;
    if (p->length >= 0 && target > p->length - 1)
        target = p->length > 0 ? p->length - 1 : 0;
    bool play = p->state == OS64_MEDIA_PLAYER_PLAYING || p->state == OS64_MEDIA_PLAYER_BUFFERING ||
                p->state == OS64_MEDIA_PLAYER_ENDED;
    return seek_to(p, target, c, now, play);
}

uint32_t os64_media_player_seek(os64_media_player_t *p, int64_t frame)
{
    collect_t c = {0};
    int64_t now = now_us();
    os64_lock_acquire(&p->lock);
    uint32_t g = p->state == OS64_MEDIA_PLAYER_OPENING || p->state == OS64_MEDIA_PLAYER_FAILED
                     ? p->seek_generation
                     : seek_locked(p, frame, &c, now);
    unlock(p);
    collect_release(p, &c);
    return g;
}

uint32_t os64_media_player_seek_by(os64_media_player_t *p, int64_t delta)
{
    collect_t c = {0};
    int64_t now = now_us();
    os64_lock_acquire(&p->lock);
    uint32_t g = p->seek_generation;
    if (p->state != OS64_MEDIA_PLAYER_OPENING && p->state != OS64_MEDIA_PLAYER_FAILED) {
        int64_t base = p->seek_pending ? p->seek_target : os64_media_clock_read(&p->clock, now);
        g = seek_locked(p, base + delta, &c, now);
    }
    unlock(p);
    collect_release(p, &c);
    return g;
}

void os64_media_player_mute(os64_media_player_t *p, bool muted)
{
    int64_t now = now_us();
    os64_lock_acquire(&p->lock);
    p->muted = muted;
    if (p->has_sound && p->voiced)
        os64_media_clock_source(&p->clock, audible(p) ? OS64_MEDIA_CLOCK_HEARD : OS64_MEDIA_CLOCK_MONOTONIC, now);
    voice_asks(p);
    unlock(p);
}

void os64_media_player_gain(os64_media_player_t *p, double gain)
{
    os64_lock_acquire(&p->lock);
    p->voice.gain = gain < 0 ? 0 : gain > 1 ? 1 : gain;
    os64_lock_release(&p->lock);
}

void os64_media_player_sync(os64_media_player_t *p, int64_t frames)
{
    os64_lock_acquire(&p->lock);
    p->sync = frames;
    voice_asks(p);
    unlock(p);
}

void os64_media_player_loaded(os64_media_player_t *p, bool loaded)
{
    os64_lock_acquire(&p->lock);
    p->loaded = loaded;
    os64_lock_release(&p->lock);
}

void os64_media_player_size(os64_media_player_t *p, uint32_t w, uint32_t h)
{
    os64_lock_acquire(&p->lock);
    p->pic_w = w > 0 ? w : 1;
    p->pic_h = h > 0 ? h : 1;
    if (p->state != OS64_MEDIA_PLAYER_PLAYING)
        p->reconvert = true;
    (void)os64_bell_ring(&p->picture_bell);
    os64_lock_release(&p->lock);
}

bool os64_media_player_frame(os64_media_player_t *p, int64_t blank, int64_t period, os64_media_player_frame_t *out)
{
    collect_t c = {0};
    int64_t now = now_us();
    os64_lock_acquire(&p->lock);
    p->last_blank = blank;
    p->period_us = period;
    p->last_frame_call = now;
    os64_media_clock_still(&p->clock, still_bound(p));   // the caller reads a period ahead
    int64_t clock = os64_media_clock_read(&p->clock, blank);
    bool judging = p->state == OS64_MEDIA_PLAYER_PLAYING && !os64_media_pictures_ended(&p->decoded);
    if (p->state != OS64_MEDIA_PLAYER_OPENING && p->state != OS64_MEDIA_PLAYER_FAILED)
        pick(p, clock, judging, &c);
    if (p->state == OS64_MEDIA_PLAYER_PLAYING)
        judge(p, clock, now);
    service(p, now);
    *out = (os64_media_player_frame_t){0};
    bool have = p->shown != NULL;
    if (have) {
        out->pixels = p->shown->pixels;
        out->w = p->shown->w;
        out->h = p->shown->h;
    }
    out->serial = p->shown_serial;
    out->changed = p->shown_serial != p->told_serial;
    p->told_serial = p->shown_serial;
    unlock(p);
    collect_release(p, &c);
    return have;
}

void os64_media_player_facts(os64_media_player_t *p, os64_media_player_facts_t *out)
{
    int64_t now = now_us();
    os64_lock_acquire(&p->lock);
    service(p, now);
    os64_media_player_facts_t f = {0};
    f.state = p->state;
    f.probed = p->probed;
    f.info = p->info;
    f.has_sound = p->has_sound;
    f.muted = p->muted;
    f.length = p->length;
    f.position = os64_media_clock_read(&p->clock, now);
    bool queued = os64_media_queue_count(&p->pcm) > 0 || os64_media_queue_ended(&p->pcm);
    f.current = p->probed && p->failure == 0 &&
                (p->has_picture ? p->shown != NULL || os64_media_pictures_count(&p->ready) > 0 : queued);
    f.future = f.current &&
               (!p->has_picture || os64_media_pictures_count(&p->ready) > 0 || os64_media_pictures_finished(&p->decoded)) &&
               (!p->has_sound || os64_media_queue_count(&p->pcm) >= BUFFER_SOUND || os64_media_queue_ended(&p->pcm));
    f.enough = f.future && (p->loaded || ((!p->has_sound || os64_media_queue_room(&p->pcm) < CHUNK_FRAMES) &&
                                          (!p->has_picture || os64_media_pictures_full(&p->decoded))));
    f.seeked = p->seeked;
    f.busy = p->voice.busy;
    f.sounding = p->voice.sounding;
    f.refused = p->voice.refused_device;
    f.refused_why = p->voice.refused_why;
    f.failure = p->failure;
    f.clock_state = os64_media_clock_state(&p->clock, now);
    f.drift = os64_media_clock_drift(&p->clock, now);
    f.shown_at = p->shown != NULL ? p->shown->at : -1;
    f.shown_end = p->shown != NULL ? p->shown->at + (p->shown->duration > 0 ? p->shown->duration : p->pace.period) : -1;
    f.decoded = os64_media_pictures_count(&p->decoded);
    f.ready = os64_media_pictures_count(&p->ready);
    f.decoded_depth = DECODED_DEPTH;
    f.ready_depth = READY_DEPTH;
    f.sound_queued = os64_media_queue_count(&p->pcm);
    f.delay_us = p->voice.delay_us;
    f.sync = p->sync;
    f.conversion_us = p->conversion_us;
    f.pace = p->pace;
    f.clock = p->clock;
    f.pictures_decoded = p->pictures_pushed;
    f.conversions = p->conversions;
    f.reconversions = p->reconversions;
    f.buffers = p->buffers;
    f.sound_decoded = p->pcm_pushed;
    f.sound_written = p->voice.written;
    f.sound_dropped = p->voice.dropped + p->pcm_flushed;
    f.refusals = p->voice.refusals;
    f.stalls = p->voice.stalls;
    f.media = p->media_counts;
    unlock(p);
    *out = f;
}
