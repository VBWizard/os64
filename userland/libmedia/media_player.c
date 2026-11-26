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
//             skip and resync. With a picture asked for it starts the
//             picture thread and waits for it at the end; it is the last
//             out: it frees the player.
//   picture   with a picture asked for, the scaler and the frames: takes
//             from the decoded queue,
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
#include "os64/gui.h"
#include "os64/media_pictures.h"
#include "os64/media_queue.h"
#include "os64/media_show.h"
#include "os64/os64.h"
#include "os64/thread.h"

#define RATE OS64_AUDIO_RATE
#define DECODED_DEPTH 8
#define READY_DEPTH 3
#define SHOWN_VIEWS 2                     // os64_media_player_shown's pins held at once, at most
// The ready ones, the one shown, one being converted, one reconverted, and
// one for each view `shown` may hold (a pinned frame retired is not free
// until it is given back). Frames are allocated at first use.
#define FRAMES (READY_DEPTH + 3 + SHOWN_VIEWS)
#define PCM_FRAMES (2u * RATE)            // two seconds of sound queued
#define CHUNK_FRAMES 4096u                // what the decoding thread asks for at once
#define BUFFER_SOUND (RATE / 2u)          // buffering ends with this much sound queued
#define POSITION_LOOK_US 250000           // a sound alone rings `look` this often while it plays
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
    // PINNED (os64_media_player_shown, the show thread's copy): a retire
    // is owed and done at the last unpin, so a pinned frame's pixels never
    // change and it is never on the free list. Under the lock.
    uint32_t pins;
    bool retire_owed;
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
    uint32_t told_readiness;              // readiness() when `look` was last rung for it
    bool loop;                            // os64_media_player_loop: the decoding thread reads it at the end
    double gain;                          // os64_media_player_gain's; the voice's is 0 for a muted sound alone
    int64_t loop_len;                     // a run's length once the media has looped, 0 before
    bool loop_restart;                    // looping, the end came with no next run queued: from the start
    bool open_seek;                       // a seek asked while OPENING, to `open_seek_target` once open
    int64_t open_seek_target;
    // Looping, the kind whose run has ended while the other's goes on: it
    // is treated as over for this run (its queue is left open for the
    // next), so a sound that runs dry is no buffering and pictures that
    // stop coming are not judged late. Cleared as the next run starts.
    bool run_sound_done, run_pictures_done;
    int64_t newest;
    int failure;
    int64_t last_blank, period_us, last_frame_call, conversion_us;
    bool converting;
    int64_t picture_thread;

    // The plane (os64/media_player.h § A plane; VIDEO_PLANE.md § 4). Under
    // the lock, but for the canvas, which only the show thread writes, and
    // `plane_win`, set before the show thread starts and never changed.
    int plane_state;                      // OS64_MEDIA_PLANE_*
    int64_t plane_win;
    uint32_t plane_name;
    uint32_t *plane_canvas;               // the show thread's
    uint32_t plane_pitch;
    bool plane_wanted;
    bool plane_poke;                      // a poke owed: unlock gives it, after the lock
    bool show_paces;                      // the show thread owns the pace: `frame` is refused
    bool show_quit;                       // the plane call's tripwire: the show thread is to end
    int plane_refusal;                    // the GUI's answer when the plane could not be had, or that ended it
    int64_t show_thread;
    os64_bell_t show_bell;                // the show thread's first verdict, to the plane call
    os64_media_show_t show;
    uint64_t frames_missed, waits_patience, frame_refused, start_tripwires, last_number;
    // What reached the glass from the plane: pictures published while it was
    // placed and shown (the pass's wait answered a display frame) and the
    // clock ran; how many runs of them there have been; when the first of
    // the current run of them was, the last of the run before it, and the last
    // (kept after the run ends, for the caller to read); the run's anchor,
    // the last of them while the run lasts and 0 once it ends (not placed,
    // the clock still, or a wait whose patience ended: not shown); and the
    // longest time between two of them in one run since the last
    // os64_media_player_plane_gap_take.
    uint64_t placed_published, placed_runs;
    int64_t placed_first_us, placed_last_us, placed_prev_last_us, placed_anchor_us, placed_gap_max_us;
    uint32_t pinned;                      // frames pinned by `shown` (at most two)

    os64_bell_t decode_bell, picture_bell;   // picture_bell only with a picture asked for
    bool bell_borrowed;                   // decode_bell is the caller's (cfg.bell): never closed here
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

// A frame back on the free list, its source collected; a pinned one at its
// last unpin. Under the lock.
static void frame_retire(os64_media_player_t *p, frame_t *f, collect_t *c)
{
    if (f == NULL)
        return;
    if (f->pins > 0) {
        f->retire_owed = true;
        return;
    }
    collect_add(c, f->source);
    f->source = NULL;
    f->next_free = p->free_frames;
    p->free_frames = f;
    (void)os64_bell_ring(&p->picture_bell);
}

static void frame_unpin(os64_media_player_t *p, frame_t *f, collect_t *c)
{
    if (--f->pins == 0 && f->retire_owed) {
        f->retire_owed = false;
        frame_retire(p, f, c);
    }
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

// A mute takes the voice away from a media with a picture (its clock goes
// monotonic); a sound alone is muted by a gain of 0, a silent voice still
// mixed, because with no ear its queue would never drain and it would
// never end.
static bool mute_drops_voice(const os64_media_player_t *p)
{
    return p->muted && p->has_picture;
}

static double voice_gain(const os64_media_player_t *p, bool picture)
{
    return p->muted && !picture ? 0 : p->gain;
}

// Whether the clock is the ear. A refused device takes a picture's ear
// away (it plays on, unheard); a sound alone WAITs for the device (its
// voice's `refused`), and its clock is still the ear, which stands still.
static bool audible(const os64_media_player_t *p)
{
    return p->has_sound && p->voiced && !mute_drops_voice(p) && !p->sound_over &&
           (!p->voice.refused_device || !p->has_picture);
}

// What the voice asks of the output, from the state; a change is rung once
// the lock is let go. Under the lock.
static void voice_asks(os64_media_player_t *p)
{
    if (!p->voiced)
        return;
    bool wants = p->has_sound && !mute_drops_voice(p) && !p->sound_over && !p->quit;
    p->voice.gain = voice_gain(p, p->has_picture);
    bool runs = p->state == OS64_MEDIA_PLAYER_PLAYING || p->state == OS64_MEDIA_PLAYER_ENDED;
    if (wants != p->voice.wants || runs != p->voice.runs || p->sync != p->voice.sync || p->quit != p->voice.remove)
        p->ring_output = true;
    p->voice.wants = wants;
    p->voice.runs = runs;
    p->voice.sync = p->sync;
    p->voice.remove = p->quit;
}

// Lets go of the lock, and rings the output if the voice's asks changed.
// Everything is read before the release: once it is down, a close may let
// the decoding thread free the player.
// The lock let go, then what was owed outside it: the output rung, and the
// plane poked (a GUI call is never made under the player's lock).
static void unlock(os64_media_player_t *p)
{
    os64_media_output_t *ring = p->ring_output ? p->cfg.output : NULL;
    p->ring_output = false;
    uint32_t poke = p->plane_poke && p->plane_state == OS64_MEDIA_PLANE_ON ? p->plane_name : 0;
    int64_t win = p->plane_win;
    p->plane_poke = false;
    os64_lock_release(&p->lock);
    if (ring != NULL)
        os64_media_output_ring(ring);
    if (poke != 0)
        (void)os64_gui_plane_poke(win, poke);
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
    p->voice.heard_all = false;           // the old queue's (os64/media_output.h)
    p->run_sound_done = p->run_pictures_done = false;
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

// A reading of the clock as a place in the media: once it has looped, the
// timeline runs on past the end (os64_media_player_loop), and a place is
// within one run (a run's end is its length, not the next run's start).
static int64_t folded(const os64_media_player_t *p, int64_t at)
{
    if (p->loop_len <= 0 || at <= p->loop_len)
        return at;
    return at - ((at - 1) / p->loop_len) * p->loop_len;
}

// No more sound is coming in this run: the queue told its end, or a loop's
// run ended it while its pictures go on. Under the lock.
static bool sound_all_queued(const os64_media_player_t *p)
{
    return os64_media_queue_ended(&p->pcm) || p->run_sound_done;
}

// Readiness at the position (facts' current, future, enough), as bits 0 to
// 2. Under the lock.
static uint32_t readiness(const os64_media_player_t *p)
{
    bool queued = os64_media_queue_count(&p->pcm) > 0 || sound_all_queued(p);
    bool current = p->probed && p->failure == 0 &&
                   (p->has_picture ? p->shown != NULL || os64_media_pictures_count(&p->ready) > 0 : queued);
    bool future = current &&
                  (!p->has_picture || os64_media_pictures_count(&p->ready) > 0 ||
                   os64_media_pictures_finished(&p->decoded)) &&
                  (!p->has_sound || os64_media_queue_count(&p->pcm) >= BUFFER_SOUND || sound_all_queued(p));
    bool enough = future && (p->loaded || ((!p->has_sound || os64_media_queue_room(&p->pcm) < CHUNK_FRAMES) &&
                                           (!p->has_picture || os64_media_pictures_full(&p->decoded))));
    return (current ? 1u : 0u) | (future ? 2u : 0u) | (enough ? 4u : 0u);
}

static const int16_t kSilence[2 * CHUNK_FRAMES];

static uint32_t seek_locked(os64_media_player_t *p, int64_t target, collect_t *c, int64_t now);

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
                     ((os64_media_queue_count(&p->pcm) >= BUFFER_SOUND || sound_all_queued(p)) && p->voice.delay_known);
        if (picture && sound)
            start_playing(p, now);
    } else if (p->state == OS64_MEDIA_PLAYER_PLAYING) {
        // A loop's sound that ran dry before its run's pictures ended
        // (run_sound_done) is heard again from the next run's first frame,
        // once the clock reaches it: given to the voice sooner, it would be
        // heard early and the ear would pull the clock ahead of the pictures.
        if (p->sound_over && os64_media_queue_count(&p->pcm) > 0 &&
            os64_media_clock_read(&p->clock, now) >= os64_media_queue_front(&p->pcm)) {
            p->sound_over = false;
            os64_media_clock_source(&p->clock, audible(p) ? OS64_MEDIA_CLOCK_HEARD : OS64_MEDIA_CLOCK_MONOTONIC, now);
            voice_asks(p);
        }
        // A sound that stopped, said by the device (starved): at its end the
        // picture goes on monotonic; with the queue run dry the decoder fell
        // behind and a hold is buffering. A sink that stops pulling, never
        // starts, or does not start again is the output's to refuse; a still
        // ear is waited out until it moves, and a stream someone else paused
        // holds the picture until it resumes.
        if (os64_media_clock_state(&p->clock, now) == OS64_MEDIA_CLOCK_STARVED) {
            if (os64_media_queue_finished(&p->pcm) ||
                (p->run_sound_done && os64_media_queue_count(&p->pcm) == 0)) {
                // A sound alone waits for its tail to be HEARD (heard_all,
                // below): a starved ear may still have the device's delay
                // to play, and a voice no longer wanted is not told it.
                if (p->has_picture) {
                    p->sound_over = true;
                    os64_media_clock_source(&p->clock, OS64_MEDIA_CLOCK_MONOTONIC, now);
                    voice_asks(p);
                }
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
        // A sound alone ends when its last frame is heard, not written: its
        // clock is the ear, and the end is the length (a picture's end is
        // its last picture's).
        bool sound_done = !p->has_sound || (os64_media_queue_finished(&p->pcm) && !p->voice.unwritten &&
                                            (p->has_picture || !p->voiced || p->voice.heard_all));
        if (p->state == OS64_MEDIA_PLAYER_PLAYING && pictures_done && sound_done) {
            int64_t end = -1;
            if (p->shown != NULL) {
                int64_t lasts = p->shown->duration > 0 ? p->shown->duration : p->pace.period;
                end = p->shown->at + lasts;
            }
            if (end < 0 || os64_media_clock_read(&p->clock, now) >= end) {
                if (p->loop) {
                    // A loop turned on too late for the next run to be
                    // queued after this one: the decoding thread starts it
                    // from the start (a gap, and still no end).
                    p->loop_restart = true;
                    (void)os64_bell_ring(&p->decode_bell);
                } else {
                    os64_media_clock_pause(&p->clock, now);
                    p->state = OS64_MEDIA_PLAYER_ENDED;
                    voice_asks(p);
                }
            }
        }
    }
    // A caller that reads the facts only when told (a page's element) hears
    // readiness move as it does a state.
    uint32_t ready = readiness(p);
    if (p->state != was || ready != p->told_readiness)
        look(p);
    p->told_readiness = ready;
}

// The output's audio thread: the voice's facts moved. Under the lock.
static void voice_look(void *ctx)
{
    os64_media_player_t *p = ctx;
    service(p, now_us());
    if (p->voice.rewind)
        (void)os64_bell_ring(&p->decode_bell);   // the decoding thread carries it out
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
    // A reconversion is of the picture shown before: it goes with it.
    frame_retire(p, p->reconverted, c);
    p->reconverted = NULL;
    if (p->owed) {
        p->owed = false;
        p->seek_pending = false;
        p->seeked = p->seek_generation;
        look(p);
    }
}

// No more pictures are coming in this run: the queue told its end, or a
// loop's run ended them while its sound goes on. Under the lock.
static bool pictures_over(const os64_media_player_t *p)
{
    return os64_media_pictures_ended(&p->decoded) || p->run_pictures_done;
}

// The producers' state, once a paint. Under the lock.
static void judge(os64_media_player_t *p, int64_t clock, int64_t now)
{
    bool judging = p->state == OS64_MEDIA_PLAYER_PLAYING && !pictures_over(p);
    bool sound_counts = audible(p) && !os64_media_queue_ended(&p->pcm) && !p->run_sound_done && !pictures_over(p);
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
    if (now - p->last_frame_call < OS64_MEDIA_PLAYER_TICK_US || p->state == OS64_MEDIA_PLAYER_OPENING ||
        p->state == OS64_MEDIA_PLAYER_FAILED)
        return;
    if (p->owed && os64_media_pictures_count(&p->ready) > 0) {
        // The picture owed after a seek (or the open) has nobody to show it,
        // playing or paused: the seek has landed all the same, and the
        // pictures go by the clock (a paused one is shown, at the clock's
        // reading, at the next frame call).
        p->owed = false;
        p->seek_pending = false;
        p->seeked = p->seek_generation;
        look(p);
    }
    if (p->state != OS64_MEDIA_PLAYER_PLAYING)
        return;
    int64_t clock = os64_media_clock_read(&p->clock, now);
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
            if (made && p->shown == shown_from && p->state != OS64_MEDIA_PLAYER_PLAYING && !p->owed &&
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
            if (p->owed || p->state != OS64_MEDIA_PLAYER_PLAYING) {
                look(p);
                p->plane_poke = true;   // a show thread waiting out a pause picks it now
            }
        }
    }
    (void)os64_bell_ring(&p->decode_bell);
    unlock(p);
    collect_release(p, &c);
    os64_media_scaler_destroy(scaler);
    return 0;
}

// ---------------------------------------------------------------- the show thread

// The pace's pick and judge for the frame shown at `blank`, as a frame call
// makes them, by whoever owns the pace. Under the lock.
static void pick_for(os64_media_player_t *p, int64_t blank, int64_t period, int64_t now, collect_t *c)
{
    p->last_blank = blank;
    p->period_us = period;
    p->last_frame_call = now;
    os64_media_clock_still(&p->clock, still_bound(p));   // the picker reads a period ahead
    int64_t clock = os64_media_clock_read(&p->clock, blank);
    bool judging = p->state == OS64_MEDIA_PLAYER_PLAYING && !pictures_over(p);
    if (p->state != OS64_MEDIA_PLAYER_OPENING && p->state != OS64_MEDIA_PLAYER_FAILED)
        pick(p, clock, judging, c);
    if (p->state == OS64_MEDIA_PLAYER_PLAYING)
        judge(p, clock, now);
    service(p, now);
}

#define SHOW_CLOSING_PATIENCE_MS 50
// Paused, a wake is a poke (a play, a seek, a size, want, and the picture
// thread's push of the picture owed after a seek); this is its backstop.
#define SHOW_PAUSED_PATIENCE_MS 250
#define SHOW_START_TRIPWIRE_US 10000000

// Playing: the time until the next ready picture is due, capped at a tick,
// so a plane with no frame answers (hidden, wanted, or its window covered)
// is kept current at the pictures' own pace. A frame answers sooner while
// the plane is shown. Under the lock.
static uint32_t show_patience_ms(os64_media_player_t *p)
{
    const os64_media_pictures_entry_t *next = os64_media_pictures_peek(&p->ready, 0);
    if (next == NULL)
        return OS64_MEDIA_PLAYER_TICK_US / 1000;
    int64_t clock = os64_media_clock_read(&p->clock, now_us());
    int64_t ms = next->at > clock ? (next->at - clock) * 1000 / RATE : 0;
    return ms < 1 ? 1 : ms > OS64_MEDIA_PLAYER_TICK_US / 1000 ? OS64_MEDIA_PLAYER_TICK_US / 1000 : (uint32_t)ms;
}

// The plane's creator, and from the plane's making the pace's one owner
// (P2's design state § 1 has the pass, step by step). Every GUI call and
// every copy is made with the player's lock let go; the frame it copies is
// pinned. It ends by returning, through the thread exit the kernel takes a
// thread's planes on, so a plane still placed when the closing bound
// passes is taken out by the kernel (hidden, then destroyed).
static int64_t show_thread(void *arg)
{
    os64_media_player_t *p = arg;
    collect_t c = {0};
    os64_lock_acquire(&p->lock);
    uint32_t w = p->pic_w ? p->pic_w : 640, h = p->pic_h ? p->pic_h : 360;
    int64_t win = p->plane_win;
    os64_lock_release(&p->lock);

    int64_t name = os64_gui_plane_create(win, w, h);
    os64_gui_plane_surface_t surf = {0};
    int64_t got = name > 0 ? os64_gui_plane_surface(win, (uint32_t)name, &surf) : name;
    os64_lock_acquire(&p->lock);
    bool quit = p->show_quit;
    if (got != 0 || quit) {
        // Refused, or the plane call stopped waiting: no plane, no pace.
        p->plane_state = OS64_MEDIA_PLANE_REFUSED;
        p->plane_refusal = got != 0 ? (int)got : 0;
        (void)os64_bell_ring(&p->show_bell);
        os64_lock_release(&p->lock);
        if (name > 0)
            (void)os64_gui_plane_destroy(win, (uint32_t)name);
        return 0;
    }
    p->plane_name = (uint32_t)name;
    p->plane_canvas = surf.pixels;
    p->plane_pitch = surf.pitch_px;
    os64_media_show_init(&p->show, surf.cap_w, surf.cap_h);
    p->plane_state = OS64_MEDIA_PLANE_ON;
    p->show_paces = true;
    (void)os64_bell_ring(&p->show_bell);   // under the lock: the plane call reads the state under it
    look(p);
    os64_lock_release(&p->lock);

    bool placed = false;
    for (;;) {
        os64_lock_acquire(&p->lock);
        uint32_t patience = p->quit ? SHOW_CLOSING_PATIENCE_MS
                          : p->state == OS64_MEDIA_PLAYER_PLAYING ? show_patience_ms(p)
                          : SHOW_PAUSED_PATIENCE_MS;
        os64_lock_release(&p->lock);

        os64_gui_plane_frame_t f = {0};
        int64_t rc = os64_gui_plane_frame_wait(win, (uint32_t)name, patience, &f);
        int64_t now = now_us();

        os64_lock_acquire(&p->lock);
        // The window or the plane is no more (the task's exit, a caller
        // that broke the rules): nothing is left to destroy.
        bool gone = rc != 0 && rc != OS64_GUI_ERR_INTERRUPTED;
        bool frame = rc == 0 && f.why == OS64_GUI_PLANE_FRAME;
        if (rc == 0)
            placed = f.placed != 0;
        if (frame && p->state == OS64_MEDIA_PLAYER_PLAYING) {
            if (p->last_number != 0 && f.number > p->last_number + 1)
                p->frames_missed += f.number - p->last_number - 1;
            p->last_number = f.number;
        } else if (!(rc == 0 && f.why == OS64_GUI_PLANE_POKED)) {
            // A pause, or a wait its patience ended (the plane not shown),
            // is no missed frame; a poke changes nothing of the count.
            p->last_number = 0;
        }
        if (rc == 0 && f.why == OS64_GUI_PLANE_PATIENCE)
            p->waits_patience++;
        if (!placed || p->state != OS64_MEDIA_PLAYER_PLAYING || (rc == 0 && f.why == OS64_GUI_PLANE_PATIENCE))
            p->placed_anchor_us = 0;    // the run ends: a gap off the glass, or with the clock still, is none of the plane's
        int64_t period = frame && f.period_ns != 0 ? (int64_t)(f.period_ns / 1000)
                       : p->period_us != 0 ? p->period_us : PERIOD_GUESS_US;
        int64_t blank = frame ? (int64_t)f.blank_us + period : now;
        bool closing = p->quit;
        if (!closing && !gone)
            pick_for(p, blank, period, now, &c);
        os64_media_show_input_t in = {.now = now, .placed = placed, .wanted = p->plane_wanted, .closing = closing,
                                      .gone = gone, .have = p->shown != NULL && !closing,
                                      .target_w = p->pic_w, .target_h = p->pic_h};
        if (in.have) {
            in.w = p->shown->w;
            in.h = p->shown->h;
            in.serial = p->shown_serial;
        }
        os64_media_show_act_t act = os64_media_show_decide(&p->show, &in);
        if (act.look)
            look(p);
        frame_t *pin = NULL;
        if (act.publish) {
            pin = p->shown;
            pin->pins++;
        }
        unlock(p);
        collect_release(p, &c);

        if (act.recreate && os64_gui_plane_destroy(win, (uint32_t)name) != 0) {
            // The place was empty at the wait, but the caller placed the
            // plane since: it stays, and the next pass sees it placed.
            os64_lock_acquire(&p->lock);
            frame_unpin(p, pin, &c);
            unlock(p);
            collect_release(p, &c);
            continue;
        }
        if (act.recreate) {
            // The old stores go to the task's kept ones, and a plane of the
            // class this size needs.
            name = os64_gui_plane_create(win, in.w, in.h);
            got = name > 0 ? os64_gui_plane_surface(win, (uint32_t)name, &surf) : name;
            os64_lock_acquire(&p->lock);
            if (got != 0) {
                // No plane of that size: the pace goes back to the caller.
                os64_media_show_recreated(&p->show, 0, 0);
                p->plane_state = OS64_MEDIA_PLANE_REFUSED;
                p->plane_refusal = (int)got;
                p->show_paces = false;
                p->plane_name = 0;
                frame_unpin(p, pin, &c);
                look(p);
                unlock(p);
                collect_release(p, &c);
                if (name > 0)
                    (void)os64_gui_plane_destroy(win, (uint32_t)name);
                return 0;
            }
            os64_media_show_recreated(&p->show, surf.cap_w, surf.cap_h);
            p->plane_name = (uint32_t)name;
            p->plane_canvas = surf.pixels;
            p->plane_pitch = surf.pitch_px;
            look(p);   // the plane's name changed
            unlock(p);
        }
        if (act.publish) {
            for (uint32_t y = 0; y < in.h; y++)
                os64_memcpy(p->plane_canvas + (size_t)y * p->plane_pitch, pin->pixels + (size_t)y * in.w,
                            (size_t)in.w * 4);
            bool done = os64_gui_plane_publish(win, (uint32_t)name, in.w, in.h) == 0;
            os64_lock_acquire(&p->lock);
            if (os64_media_show_published(&p->show, done, in.w, in.h, in.serial))
                look(p);
            if (done && placed && frame && p->state == OS64_MEDIA_PLAYER_PLAYING) {
                int64_t at = now_us();
                p->placed_published++;
                if (p->placed_anchor_us == 0) {
                    p->placed_prev_last_us = p->placed_last_us;     // the run before ended there
                    p->placed_first_us = at;
                    p->placed_runs++;
                } else if (at - p->placed_anchor_us > p->placed_gap_max_us)
                    p->placed_gap_max_us = at - p->placed_anchor_us;
                p->placed_anchor_us = p->placed_last_us = at;
            }
            frame_unpin(p, pin, &c);
            unlock(p);
            collect_release(p, &c);
        }
        if (act.destroy)
            (void)os64_gui_plane_destroy(win, (uint32_t)name);
        if (act.end) {
            os64_lock_acquire(&p->lock);
            p->plane_state = OS64_MEDIA_PLANE_ENDED;
            p->plane_refusal = gone ? (int)rc : 0;  // the frame wait's answer that ended it; 0 for a close
            p->show_paces = false;
            p->plane_name = 0;
            look(p);
            unlock(p);
            return 0;
        }
    }
}

int os64_media_player_plane(os64_media_player_t *p, int64_t win)
{
    if (p == NULL || win <= 0 || (p->cfg.want & OS64_MEDIA_WANT_PICTURE) == 0)
        return OS64_MEDIA_BAD_ARGS;
    os64_lock_acquire(&p->lock);
    bool again = false;
    if (p->plane_state == OS64_MEDIA_PLANE_ENDED && !p->quit && p->show_thread > 0) {
        // A plane that ended while the player plays may be asked for again:
        // its show thread has returned, so it is joined and a new one
        // starts as the first did. STARTING across the join, so another
        // call meanwhile is answered BUSY.
        int64_t ended = p->show_thread;
        p->show_thread = 0;
        p->plane_state = OS64_MEDIA_PLANE_STARTING;
        p->show_quit = false;
        os64_lock_release(&p->lock);
        (void)os64_thread_join((int32_t)ended, NULL);
        os64_close((int32_t)ended);
        os64_lock_acquire(&p->lock);
        if (p->quit) {
            // Closed during the join: no new plane for a closing player.
            p->plane_state = OS64_MEDIA_PLANE_ENDED;
            os64_lock_release(&p->lock);
            return OS64_MEDIA_BUSY;
        }
        again = true;
    }
    if (!again && p->plane_state != OS64_MEDIA_PLANE_NONE) {
        os64_lock_release(&p->lock);
        return OS64_MEDIA_BUSY;
    }
    if (os64_bell_make(&p->show_bell) < 0) {
        if (again)
            p->plane_state = OS64_MEDIA_PLANE_ENDED;    // as it was before the ask
        os64_lock_release(&p->lock);
        return OS64_MEDIA_NO_MEMORY;
    }
    p->plane_win = win;
    p->plane_state = OS64_MEDIA_PLANE_STARTING;
    os64_lock_release(&p->lock);
    // A handle reserved before the thread exists: an error means no thread.
    int64_t h = os64_thread(show_thread, p);
    if (h < 0) {
        os64_lock_acquire(&p->lock);
        p->plane_state = OS64_MEDIA_PLANE_REFUSED;
        p->plane_refusal = 0;   // no GUI answer: the thread could not start
        os64_lock_release(&p->lock);
        os64_bell_close(&p->show_bell);
        return OS64_MEDIA_NO_PLANE;
    }
    // Two answers, made or refused. The bound is a tripwire only (a create
    // takes milliseconds, more to zero a large pair): past it the thread is
    // told to end, joined, and the plane counted refused.
    int64_t until = now_us() + SHOW_START_TRIPWIRE_US;
    int state;
    for (;;) {
        os64_lock_acquire(&p->lock);
        state = p->plane_state;
        if (state == OS64_MEDIA_PLANE_STARTING && now_us() >= until) {
            p->show_quit = true;
            p->start_tripwires++;
        }
        bool quit = p->show_quit;
        os64_lock_release(&p->lock);
        if (state != OS64_MEDIA_PLANE_STARTING || quit)
            break;
        (void)os64_bell_wait(&p->show_bell, 100);
    }
    if (state != OS64_MEDIA_PLANE_ON) {
        (void)os64_thread_join((int32_t)h, NULL);
        os64_close((int32_t)h);   // a join only reads the handle
        os64_lock_acquire(&p->lock);
        p->plane_state = OS64_MEDIA_PLANE_REFUSED;
        os64_lock_release(&p->lock);
        os64_bell_close(&p->show_bell);
        return OS64_MEDIA_NO_PLANE;
    }
    os64_lock_acquire(&p->lock);
    p->show_thread = h;
    os64_lock_release(&p->lock);
    os64_bell_close(&p->show_bell);
    return 0;
}

int64_t os64_media_player_plane_gap_take(os64_media_player_t *p)
{
    os64_lock_acquire(&p->lock);
    int64_t gap = p->placed_gap_max_us;
    p->placed_gap_max_us = 0;
    os64_lock_release(&p->lock);
    return gap;
}

void os64_media_player_plane_want(os64_media_player_t *p, bool want)
{
    os64_lock_acquire(&p->lock);
    p->plane_wanted = want;
    p->plane_poke = true;
    unlock(p);
}

int os64_media_player_shown(os64_media_player_t *p, os64_media_player_view_t *out)
{
    os64_lock_acquire(&p->lock);
    if (p->shown == NULL) {
        os64_lock_release(&p->lock);
        return OS64_MEDIA_BAD_ARGS;
    }
    if (p->pinned >= SHOWN_VIEWS) {
        os64_lock_release(&p->lock);
        return OS64_MEDIA_BUSY;
    }
    frame_t *f = p->shown;
    f->pins++;
    p->pinned++;
    *out = (os64_media_player_view_t){f->pixels, f->w, f->h, p->shown_serial, f};
    os64_lock_release(&p->lock);
    return 0;
}

void os64_media_player_shown_done(os64_media_player_t *p, os64_media_player_view_t *view)
{
    if (view == NULL || view->frame == NULL)
        return;
    collect_t c = {0};
    os64_lock_acquire(&p->lock);
    frame_unpin(p, view->frame, &c);
    p->pinned--;
    os64_lock_release(&p->lock);
    collect_release(p, &c);
    view->frame = NULL;
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
        // Built under the lock (the picture thread reads it), keeping a gain
        // asked while opening; added outside it (the output's lock first).
        os64_lock_acquire(&p->lock);
        p->voice = (os64_media_output_voice_t){.lock = &p->lock, .queue = &p->pcm, .clock = &p->clock,
                                               .refused = picture ? p->cfg.refused : OS64_MEDIA_VOICE_WAIT,
                                               .look = voice_look, .room = voice_room,
                                               .ctx = p, .gain = voice_gain(p, picture), .position = -1};
        os64_lock_release(&p->lock);
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
    // A mute asked while opening holds: the clock is heard only if the sound is.
    os64_media_clock_source(&p->clock, audible(p) ? OS64_MEDIA_CLOCK_HEARD : OS64_MEDIA_CLOCK_MONOTONIC, now_us());
    p->probed = true;
    p->state = p->want_play ? OS64_MEDIA_PLAYER_BUFFERING : OS64_MEDIA_PLAYER_PAUSED;
    collect_t c = {0};
    if (p->open_seek && p->failure == 0) {
        p->open_seek = false;
        (void)seek_locked(p, p->open_seek_target, &c, now_us());   // nothing is queued yet
    }
    voice_asks(p);
    look(p);
    (void)os64_bell_ring(&p->picture_bell);
    unlock(p);
    collect_release(p, &c);
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
    // A view is dead after close (os64/media_player.h): a pin still held
    // must not keep its picture from going back.
    for (int i = 0; i < FRAMES; i++) {
        p->frame[i].pins = 0;
        p->frame[i].retire_owed = false;
    }
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
    if (!p->bell_borrowed)
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
    // A loop's timeline (os64_media_player_loop): each run is queued after
    // the last, `base` frames on, and `run_end` is where this run reached.
    // `ended_queued`: the end was told to the queues with loop off.
    // `run_sound_end` is where this run's sound reached: when the pictures
    // outlast it, the next run waits behind `pad` frames of silence from
    // `pad_at`, since the output plays queued sound back to back.
    int64_t base = 0, run_end = 0, run_sound_end = 0, pad_at = 0, pad = 0;
    bool ended_queued = false;
    int64_t position_looked = 0;          // a sound alone's last periodic `look`
    collect_t c = {0};
    os64_lock_acquire(&p->lock);
    if (pcm == NULL && p->failure == 0) {
        p->failure = OS64_MEDIA_NO_MEMORY;
        service(p, now_us());
        look(p);
    }
    // Only a close ends this loop: the player is the caller's until then,
    // failed or not.
    while (!p->quit) {
        if (p->failure != 0 || p->media == NULL) {
            unlock(p);
            collect_release(p, &c);
            (void)os64_bell_wait(&p->decode_bell, 200);
            os64_lock_acquire(&p->lock);
            continue;
        }
        if (p->voice.rewind) {
            // The output let go of sound of ours still in the device (a long
            // pause, another sound starting): a seek to where the clock
            // stands queues it again. Done here, never on the caller's thread.
            p->voice.rewind = false;
            int64_t now = now_us();
            if (p->state == OS64_MEDIA_PLAYER_PAUSED || p->state == OS64_MEDIA_PLAYER_BUFFERING)
                (void)seek_to(p, folded(p, os64_media_clock_read(&p->clock, now)), &c, now,
                              p->state == OS64_MEDIA_PLAYER_BUFFERING);
        }
        if (p->loop_restart) {
            p->loop_restart = false;
            if (p->loop && p->state == OS64_MEDIA_PLAYER_PLAYING)
                (void)seek_to(p, 0, &c, now_us(), true);   // carried out just below
        }
        if (ended_queued && p->loop && p->has_sound && !p->has_picture && gen_pcm == p->pcm.generation &&
            !os64_media_queue_finished(&p->pcm)) {
            // Loop turned on after the end was queued but before the output
            // took the last frame (a short sound is queued whole at once): the end is
            // taken back, and the media's next GOT_END starts the next run
            // after this one. A picture's queue keeps its end: the end
            // then starts from the start (loop_restart).
            os64_media_queue_reopen(&p->pcm);
            ended_queued = false;
            sound_done = false;
        }
        if (p->seek_serial != served) {
            served = p->seek_serial;
            int64_t target = p->seek_target;
            gen_decoded = os64_media_pictures_generation(&p->decoded);
            gen_pcm = p->pcm.generation;
            sound_done = pictures_done = false;
            full = 0;
            resync_waiting = false;
            base = run_end = run_sound_end = pad = 0;   // a seek's place is the media's own
            ended_queued = false;
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
        if (!p->has_picture && p->state == OS64_MEDIA_PLAYER_PLAYING) {
            // A sound alone's position moves with nothing else to say so
            // (a picture's caller asks at every frame): `look`, for a
            // page's timeupdate and its controls' time.
            int64_t now = now_us();
            if (now - position_looked >= POSITION_LOOK_US) {
                position_looked = now;
                look(p);
            }
        }
        if (pad > 0) {
            uint32_t room = os64_media_queue_room(&p->pcm);
            if (room > 0) {
                int64_t n = pad < (int64_t)room ? pad : (int64_t)room;
                if (n > (int64_t)CHUNK_FRAMES)
                    n = CHUNK_FRAMES;
                uint32_t pushed = os64_media_queue_push(&p->pcm, gen_pcm, kSilence, (uint32_t)n, pad_at);
                pad = pushed == 0 ? 0 : pad - pushed;   // none taken: a seek came between
                pad_at += pushed;
                continue;
            }
        }
        uint32_t accept = 0;
        if (p->has_sound && !sound_done && pad == 0 && os64_media_queue_room(&p->pcm) >= CHUNK_FRAMES)
            accept |= OS64_MEDIA_WANT_SOUND;
        if (p->has_picture && !pictures_done && !os64_media_pictures_full(&p->decoded))
            accept |= OS64_MEDIA_WANT_PICTURE;
        if (full != 0 && (accept & full) == 0)
            accept = 0;                   // park until the kind libmedia named has room
        if (accept == 0 || p->failure != 0) {
            service(p, now_us());
            unlock(p);
            // Nothing to read now: the source may let a fetch go (media.h).
            if (p->cfg.source.idle != NULL)
                p->cfg.source.idle(p->cfg.source.ctx);
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
            p->pcm_pushed += os64_media_queue_push(&p->pcm, gen_pcm, pcm, got.frames, got.at + base);
            if (got.at + (int64_t)got.frames > run_end)
                run_end = got.at + (int64_t)got.frames;
            if (got.at + (int64_t)got.frames > run_sound_end)
                run_sound_end = got.at + (int64_t)got.frames;
            if (!p->has_picture && p->seek_pending && os64_media_queue_count(&p->pcm) > 0) {
                // With no picture, a seek has landed once sound from there is queued.
                p->seek_pending = false;
                p->owed = false;
                p->seeked = p->seek_generation;
                look(p);
            }
        } else if (got.kind == OS64_MEDIA_GOT_PICTURE) {
            int pushed = os64_media_pictures_push(&p->decoded, gen_decoded, got.picture, got.picture->at + base,
                                                  got.picture->duration);
            int64_t ends = got.picture->at + (got.picture->duration > 0 ? got.picture->duration : 0);
            if (ends > run_end)
                run_end = ends;
            if (pushed == OS64_MEDIA_PICTURES_TAKEN) {
                p->pictures_pushed++;
                p->newest = got.picture->at + base;
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
            if ((accept & OS64_MEDIA_WANT_SOUND) != 0)
                sound_done = true;
            if ((accept & OS64_MEDIA_WANT_PICTURE) != 0)
                pictures_done = true;
            bool all_done = (!p->has_sound || sound_done) && (!p->has_picture || pictures_done);
            if (all_done && run_end > 0 && p->length < 0) {
                // A media whose length the probe could not say has one now
                // (an MP3 with no header served with no length).
                p->length = run_end;
                look(p);
            }
            if (!all_done && p->loop && run_end > 0) {
                // Looping, a kind that is done waits for the other: the run
                // starts again only once both have reached its end.
                p->run_sound_done = p->has_sound && sound_done;
                p->run_pictures_done = p->has_picture && pictures_done;
                service(p, now_us());
                continue;
            }
            // A picture queue already told its end (loop turned on after the
            // pictures ended) cannot take the next run: the end then starts
            // from the start (loop_restart). A sound queue's end is taken back.
            bool pictures_closed = p->has_picture && os64_media_pictures_ended(&p->decoded);
            if (all_done && p->loop && run_end > 0 && !pictures_closed) {
                // The start again, queued after this run: nothing ends.
                unlock(p);
                int sought = os64_media_seek(p->media, 0);
                os64_lock_acquire(&p->lock);
                if (sought == 0) {
                    if (p->has_sound && gen_pcm == p->pcm.generation)
                        os64_media_queue_reopen(&p->pcm);
                    // Pictures that outlast the sound: silence to the run's
                    // end, so the next run's sound is heard at its time. A
                    // sound already run dry waits for the clock instead
                    // (sound_over; service).
                    pad = p->has_sound && !p->sound_over && run_sound_end < run_end ? run_end - run_sound_end : 0;
                    pad_at = base + run_sound_end;
                    // A sound that ran dry before the pictures ended stays
                    // over (the clock monotonic) until the clock reaches the
                    // next run's first frame (service).
                    p->run_sound_done = p->run_pictures_done = false;
                    p->loop_len = run_end;
                    base += run_end;
                    run_end = run_sound_end = 0;
                    sound_done = pictures_done = false;
                    service(p, now_us());
                    continue;
                }
            }
            // The end (with loop off, a run that gave nothing, or a seek back
            // that failed): each kind ends as it is done.
            if (sound_done && gen_pcm == p->pcm.generation)
                os64_media_queue_end(&p->pcm);
            if (pictures_done)
                os64_media_pictures_end(&p->decoded, gen_decoded);
            ended_queued = all_done;
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
    // A join only reads a thread's handle; each is closed after it.
    if (p->picture_thread > 0) {
        (void)os64_thread_join((int32_t)p->picture_thread, NULL);
        os64_close((int32_t)p->picture_thread);
    }
    // The show thread ends after its plane is destroyed (the caller hid it),
    // or at the closing bound with it placed: `finished` comes after both.
    if (p->show_thread > 0) {
        (void)os64_thread_join((int32_t)p->show_thread, NULL);
        os64_close((int32_t)p->show_thread);
    }
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
    p->show_bell = (os64_bell_t)OS64_BELL_NONE;
    p->gain = 1.0;                        // until os64_media_player_gain
    p->pcm_storage = os64_malloc(sizeof(int16_t) * 2 * PCM_FRAMES);
    // A bell is a handle, and a page may hold dozens of players: a sound
    // alone makes no picture bell, and a caller's bell is used as given.
    p->bell_borrowed = cfg->bell != NULL;
    if (p->bell_borrowed)
        p->decode_bell = *cfg->bell;
    bool pictures = (cfg->want & OS64_MEDIA_WANT_PICTURE) != 0;
    if (p->pcm_storage == NULL || (!p->bell_borrowed && os64_bell_make(&p->decode_bell) < 0) ||
        (pictures && os64_bell_make(&p->picture_bell) < 0)) {
        if (!p->bell_borrowed)
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
    // act. A thread's handle is reserved before the thread exists, so a
    // start that reports failure started nothing, and the player is freed.
    int64_t h = os64_thread(decoding_thread, p);
    if (h < 0) {
        if (!p->bell_borrowed)
            os64_bell_close(&p->decode_bell);
        os64_bell_close(&p->picture_bell);
        os64_free(p->pcm_storage);
        os64_free(p);
        return OS64_MEDIA_NO_MEMORY;
    }
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
    p->plane_poke = true;   // the show thread starts its close at once
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
    p->plane_poke = true;
    if (p->state == OS64_MEDIA_PLAYER_PAUSED) {
        // Through buffering, whose exit (a picture, half a second of sound,
        // the sink's delay) service decides below, at once when it holds.
        p->state = OS64_MEDIA_PLAYER_BUFFERING;
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
    p->plane_poke = true;
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
    uint32_t g = p->seek_generation;
    if (p->state == OS64_MEDIA_PLAYER_OPENING) {
        // Carried out once the media is open (opened), under the next
        // generation, which is the one answered now.
        p->open_seek = true;
        p->open_seek_target = frame;
        g = p->seek_generation + 1;
    } else if (p->state != OS64_MEDIA_PLAYER_FAILED) {
        g = seek_locked(p, frame, &c, now);
    }
    p->plane_poke = true;
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
        int64_t base = p->seek_pending ? p->seek_target : folded(p, os64_media_clock_read(&p->clock, now));
        g = seek_locked(p, base + delta, &c, now);
    }
    p->plane_poke = true;
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
    p->gain = gain < 0 ? 0 : gain > 1 ? 1 : gain;
    p->voice.gain = voice_gain(p, p->has_picture);
    os64_lock_release(&p->lock);
}

void os64_media_player_loop(os64_media_player_t *p, bool loop)
{
    os64_lock_acquire(&p->lock);
    p->loop = loop;
    (void)os64_bell_ring(&p->decode_bell);   // one waiting at the end looks again
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
    p->plane_poke = true;
    (void)os64_bell_ring(&p->picture_bell);
    unlock(p);
}

bool os64_media_player_frame(os64_media_player_t *p, int64_t blank, int64_t period, os64_media_player_frame_t *out)
{
    collect_t c = {0};
    int64_t now = now_us();
    os64_lock_acquire(&p->lock);
    *out = (os64_media_player_frame_t){0};
    if (p->show_paces) {
        // One owner of the pace: the show thread's while a plane is on.
        p->frame_refused++;
        os64_lock_release(&p->lock);
        return false;
    }
    pick_for(p, blank, period, now, &c);
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
    // At the end the position is the end (HTML's: currentTime is the
    // duration), whatever the ear last reported.
    f.position = p->state == OS64_MEDIA_PLAYER_ENDED && p->length >= 0
                     ? p->length
                     : folded(p, os64_media_clock_read(&p->clock, now));
    uint32_t ready = readiness(p);
    f.current = (ready & 1u) != 0;
    f.future = (ready & 2u) != 0;
    f.enough = (ready & 4u) != 0;
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
    f.plane_state = p->plane_state;
    f.plane_name = p->plane_name;
    f.plane_refusal = p->plane_refusal;
    f.plane_cap_w = p->show.cap_w;
    f.plane_cap_h = p->show.cap_h;
    f.plane_pic_w = p->show.pic_w;
    f.plane_pic_h = p->show.pic_h;
    // Against the size asked as it is now: a size asked since the show
    // thread's last pass is not ready, whatever that pass said.
    f.plane_ready = p->show.ready && p->show.pic_w == p->pic_w && p->show.pic_h == p->pic_h;
    f.plane_stale = p->show.stale;
    f.plane_published = p->show.publishes;
    f.plane_serial = p->show.published;
    f.plane_recreates = p->show.recreates;
    f.plane_ended_placed = p->show.ended_placed;
    f.frames_missed = p->frames_missed;
    f.plane_placed_published = p->placed_published;
    f.plane_placed_runs = p->placed_runs;
    f.plane_placed_first_us = p->placed_first_us;
    f.plane_placed_prev_last_us = p->placed_prev_last_us;
    f.plane_placed_last_us = p->placed_last_us;
    f.waits_patience = p->waits_patience;
    f.frame_refused = p->frame_refused;
    f.plane_start_tripwires = p->start_tripwires;
    unlock(p);
    *out = f;
}
