// gplay — play a video file in a window, picture and sound in step.
//
//     gplay film.mp4                 play it; q or Escape leaves
//     gplay --exit-at-end film.mp4   and leave when it has played
//     gplay --maximized film.mp4     start with the window maximised
//
// For tests (/tests/gplaytest): --check plays to the end and judges the
// counts (exit 3 when one is wrong), and with --expect-refusal also that the
// device was refused and then regained; --cover opens a window of its own over
// the whole screen, so gplay's is answered no frames; --script "ms:action,..."
// performs keys at times from the start (pause, back, forward, backfar,
// forwardfar, mute, louder, softer, later, earlier, readout, quit, or
// resize=WxH).
//
// Keys: space pauses and resumes; left and right seek five seconds, with
// Shift a minute (presses while a seek is under way count from its target);
// m mutes; up and down move /dev/volume; [ and ] move the sound against the
// picture by 10 ms (the sync dial, shown in the readout, not saved); i
// shows the readout over the picture; q or Escape leaves. The headphones'
// buttons (/dev/mediakeys): Play and Pause toggle, Previous goes to the
// start, Stop leaves. Every number the readout shows prints at exit.
//
// THE DESIGN is VIDEO.md §§ 3 to 6 (the threads, the clock, the late
// policy, the queues) with ~/os64-opus-kit's VIDEO_V4_TABLES.md as agreed;
// the rules that decide are libmedia's pure cores (os64/media_pictures.h,
// media_clock.h, media_pace.h), and this program obeys them.
//
// THE THREADS AND THE LOCK.
//
//   main      the window and its canvas, the keys, the player's STATE, the
//             frame requests, and at each frame answer the pace's pick and
//             judge; the picture on the screen (`shown`).
//   decoding  every libmedia call but release and the scaler: pulls with
//             os64_media_next, asking only for the kinds its queues have
//             room for, and pushes into the decoded queue and the sound
//             queue; carries out seeks and the pace's skip and resync.
//   picture   the scaler and the frames (converted pixels): takes from the
//             decoded queue, skips what the pace says is overdue, converts
//             to the window's picture size, pushes into the ready queue.
//   audio     /dev/audio (open only while the sound is audible), its
//             position and control; takes from the sound queue, writes, and
//             reports what the ear heard to the clock.
//   keys      the media-key session, and a --script's timed actions; hands
//             each to main through the window's doorbell.
//
// ONE LOCK, the player's, guards everything two threads touch: the three
// queues, the clock, the pace, the requests, the shared facts and the free
// frames. It is never held across a libmedia call, a conversion, a device
// call or a window call that draws, waits or asks, and every core call is
// made under it, so there is no order to keep. Pictures to give back to
// libmedia are COLLECTED under it and released after
// (os64_media_picture_release takes a lock of its own). Bells, and the
// window's doorbell (os64_gui_event_ring, which never blocks), are rung with
// it held (os64/bell.h's rule).
//
// THE STATES (VIDEO_V4_TABLES.md § 2): buffering (after the open, a seek
// while playing, or the sound running dry: the clock paused, NOTHING written
// to the device, until a picture is ready and half a second of sound is
// queued), playing, paused, ended, failed. A picture is OWED after a seek:
// the next frame answer shows the first one ready, whatever its time. A
// resize while not playing converts the SHOWN picture again at the new size
// (each frame keeps the decoded picture it came from until it is retired),
// and hands it to main directly, past the ready queue.
//
// A COVERED WINDOW is answered no frames, so while playing the audio thread
// rings the window when none has come for TICK_US, and main retires the
// overdue pictures by the clock without publishing: the queues keep moving
// and the sound plays on. The conversions that nobody sees are wasted
// (DEBTS: a covered video converts for nobody).
//
// Exit codes: 0 played (or left); 1 usage, or no desktop on this boot; 2 the
// file would not open, has no picture, or is not media; 3 the media failed
// part way, or --check found a count wrong.

#include "os64/os64.h"
#include "os64/audio.h"
#include "os64/bell.h"
#include "os64/draw.h"
#include "os64/lock.h"
#include "os64/media.h"
#include "os64/media_clock.h"
#include "os64/media_mix.h"
#include "os64/media_pace.h"
#include "os64/media_pictures.h"
#include "os64/media_queue.h"
#include "os64/mediakeys.h"
#include "os64/thread.h"

#include "view.h"

#define RATE OS64_AUDIO_RATE
#define DECODED_DEPTH 8
#define READY_DEPTH 3
#define FRAMES (READY_DEPTH + 3)          // the ready ones, the one shown, one being converted, one reconverted
#define PCM_FRAMES (2u * RATE)            // two seconds of sound queued
#define CHUNK_FRAMES 4096u                // what the decoding thread asks for at once
#define WRITE_FRAMES 1024u                // what the audio thread writes at once
#define WRITE_PATIENCE_MS 20u
#define BUFFER_SOUND (RATE / 2u)          // buffering ends with this much sound queued
#define SOUND_FLOOR (RATE / 4u)           // under it is far behind (the pace's twin door)
#define STALL_US 1000000                  // a sink not advancing with sound queued: refused
// A report's trust (os64_media_clock_still): longer than the oldest a
// healthy report can be when main reads the clock. The A2DP pump pulls a
// packet (640 frames, 13.3 ms; the null sink pulls each tick, less) in
// kusb's pass, which may come a tick late; the audio thread reads the
// position after a write that waits up to WRITE_PATIENCE_MS; main reads
// for the blank a period ahead; and a margin.
#define PULL_US 14000
#define PASS_LATE_US 10000
#define STILL_MARGIN_US 20000
#define PERIOD_GUESS_US 16667             // before the first frame answer: 60 Hz
#define RETRY_US 2000000                  // a refused device is tried again this often
#define TICK_US 100000                    // no frame answer this long while playing: retire by the clock
#define HOLD_MIN_US OS64_MEDIA_CLOCK_START_HOLD_US
#define HOLD_PAST_DELAY_US 150000         // a start holds past the sink's delay by this
#define SEEK_STEP (5 * (int64_t)RATE)
#define SEEK_FAR (60 * (int64_t)RATE)
#define SYNC_STEP ((int64_t)RATE / 100)   // 10 ms (signed: it is negated)
#define MARGIN 48                         // the first window leaves this much of the screen
#define ACTIONS 16

// The window's doorbell (os64_gui_event_ring): why main is to look.
#define RING_LOOK 1u                      // a picture owed is ready, a failure, the end of decoding
#define RING_ACTION 2u                    // a key or a script action is waiting
#define RING_TICK 4u                      // no frame answer for TICK_US while playing

enum { ST_BUFFERING, ST_PLAYING, ST_PAUSED, ST_ENDED, ST_FAILED };

static const char *const kStateName[] = {"buffering", "playing", "paused", "ended", "failed"};

// A converted picture: its pixels, at the size it was converted to, and the
// decoded picture it came from, kept so a resize while paused or ended can
// convert the shown one again (released when the frame is retired).
typedef struct frame {
    uint32_t *pixels;
    size_t capacity;                      // pixels allocated
    uint32_t w, h;
    int64_t at, duration;
    os64_media_picture_t *source;
    struct frame *next_free;
} frame_t;

// A script's action, or a key's, waiting for main.
typedef struct {
    gplay_key_t key;
    uint32_t w, h;                        // a resize (key NONE, w > 0); with BACK_FAR, w = 1 is "to the start"
} action_t;

typedef struct {
    os64_lock_t lock;
    os64_media_t *media;
    os64_media_info_t info;
    bool has_sound;                       // a sound this media decodes
    int64_t length;                       // output frames, -1 when not known

    // The queues and the cores.
    os64_media_pictures_entry_t decoded_slot[DECODED_DEPTH], ready_slot[READY_DEPTH];
    os64_media_pictures_t decoded, ready;
    os64_media_queue_t pcm;
    os64_media_clock_t clock;
    os64_media_pace_t pace;
    frame_t frame[FRAMES];
    frame_t *free_frames;
    frame_t *shown;                       // main's: the picture on the screen (or the last retired)
    frame_t *reconverted;                 // the shown one converted again at a new size, for main
    uint32_t shown_serial;                // moved each time `shown` changes

    // The state and the requests.
    int state;
    bool owed;                            // show the first ready picture at the next frame answer
    bool quit;
    int exit_code;
    uint32_t seek_serial;                 // moved by main at each seek
    int64_t seek_target;
    bool seek_pending;                    // until the first picture after it is shown
    bool muted;                           // the person's
    bool refused;                         // the device's: busy, stalled, or failed
    const char *refused_why;
    int64_t retry_at;                     // wall: when the audio thread tries the device again
    int64_t stall_since;                  // wall: the sink stalled (service's rule), since
    int64_t awaiting_since;               // wall: the clock has awaited a first report, since
    int64_t sync_offset;                  // frames added to every heard report
    uint32_t pic_w, pic_h;                // the size the picture thread converts to
    uint32_t layout_serial;               // moved by every resize
    bool reconvert;                       // convert the shown picture again at the new size
    action_t action[ACTIONS];
    uint32_t actions;

    // Facts.
    int64_t newest;                       // the newest picture the decoder pushed, -1 none
    int failure;                          // a libmedia result, 0 none
    int64_t last_blank, period_us;        // the last frame answer's blank and period
    int64_t last_answer;                  // wall of the last frame answer
    int64_t last_tick;                    // wall of the audio thread's last tick ring
    int64_t conversion_us;                // what the last conversion took
    uint32_t delay_us;                    // the sink's reported delay
    bool delay_known;                     // read from the device at least once
    uint32_t carry_frames;                // the audio thread's write tail not yet taken
    bool converting;                      // a decoded picture is out with the picture thread
    int64_t buffering_since;

    // Bells.
    os64_bell_t decode_bell, picture_bell, audio_bell;
    int64_t win;

    // Counts, for the readout and the exit.
    uint64_t pcm_pushed, pcm_written, pcm_dropped, pictures_pushed;
    uint64_t buffers, stalls, refusals, display_missed, conversions, reconversions;
    uint64_t last_frame_number;
} player_t;

static bool s_exit_at_end, s_check, s_cover, s_maximized, s_expect_refusal;
static const char *s_script;

// ---------------------------------------------------------------- collecting releases

#define COLLECT 64                        // a seek's flush plus a paint's drops, with room
typedef struct {
    os64_media_picture_t *picture[COLLECT];
    uint32_t n;
} collect_t;

static void collect_add(collect_t *c, os64_media_picture_t *p)
{
    if (p != 0 && c->n < COLLECT)
        c->picture[c->n++] = p;
}

static void collect_release(player_t *pl, collect_t *c)
{
    for (uint32_t i = 0; i < c->n; i++)
        os64_media_picture_release(pl->media, c->picture[i]);
    c->n = 0;
}

static void release_decoded(void *context, void *handle)
{
    collect_add(context, handle);
}

// A frame back on the free list, its source collected. Under the lock.
static void frame_retire(player_t *p, frame_t *f, collect_t *c)
{
    if (f == 0)
        return;
    collect_add(c, f->source);
    f->source = 0;
    f->next_free = p->free_frames;
    p->free_frames = f;
    os64_bell_ring(&p->picture_bell);
}

static void release_ready(void *context, void *handle)
{
    player_t *p = ((void **)context)[0];
    frame_retire(p, handle, ((void **)context)[1]);
}

static int64_t now_us(void)
{
    return os64_micros();
}

// The bound a start on the heard source holds for (Fable's C): the first
// report cannot come before the sink's delay has passed.
static int64_t hold_bound(const player_t *p)
{
    int64_t past = (int64_t)p->delay_us + HOLD_PAST_DELAY_US;
    return past > HOLD_MIN_US ? past : HOLD_MIN_US;
}

static int64_t still_bound(const player_t *p)
{
    int64_t period = p->period_us > 0 ? p->period_us : PERIOD_GUESS_US;
    return PULL_US + PASS_LATE_US + (int64_t)WRITE_PATIENCE_MS * 1000 + period + STILL_MARGIN_US;
}

static bool audible(const player_t *p)
{
    return p->has_sound && !p->muted && !p->refused;
}

// ---------------------------------------------------------------- the decoding thread

static int64_t decoding_thread(void *arg)
{
    player_t *p = arg;
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
    while (!p->quit && pcm != 0) {
        if (p->seek_serial != served) {
            served = p->seek_serial;
            int64_t target = p->seek_target;
            gen_decoded = os64_media_pictures_generation(&p->decoded);
            gen_pcm = p->pcm.generation;
            sound_done = pictures_done = false;
            full = 0;
            resync_waiting = false;
            os64_lock_release(&p->lock);
            int r = os64_media_seek(p->media, target);
            os64_lock_acquire(&p->lock);
            if (r < 0 && p->failure == 0) {
                p->failure = r;
                os64_gui_event_ring(p->win, RING_LOOK);
            }
            continue;
        }
        int want_skip = p->pace.skip_unused ? OS64_MEDIA_SKIP_UNUSED : OS64_MEDIA_SKIP_NONE;
        bool resync = os64_media_pace_resync_take(&p->pace);
        if (want_skip != applied_skip || resync) {
            os64_lock_release(&p->lock);
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
        if (!pictures_done && !os64_media_pictures_full(&p->decoded))
            accept |= OS64_MEDIA_WANT_PICTURE;
        if (full != 0 && (accept & full) == 0)
            accept = 0;                   // park until the kind libmedia named has room
        if (accept == 0 || p->failure != 0) {
            os64_lock_release(&p->lock);
            os64_bell_wait(&p->decode_bell, 200);
            os64_lock_acquire(&p->lock);
            continue;
        }
        full = 0;
        os64_lock_release(&p->lock);
        os64_media_got_t got = {0};
        int r = os64_media_next(p->media, accept, pcm, CHUNK_FRAMES, &got);
        os64_lock_acquire(&p->lock);
        if (r < 0) {
            if (got.kind == OS64_MEDIA_GOT_PICTURE && got.picture != 0)
                collect_add(&c, got.picture);
            if (p->failure == 0)
                p->failure = r;
            os64_gui_event_ring(p->win, RING_LOOK);
        } else if (got.kind == OS64_MEDIA_GOT_SOUND) {
            uint32_t n = os64_media_queue_push(&p->pcm, gen_pcm, pcm, got.frames, got.at);
            if (n > 0) {
                p->pcm_pushed += n;
                os64_bell_ring(&p->audio_bell);
                // Buffering ends on the sound's depth too, and main looks
                // only when told.
                if (p->state == ST_BUFFERING)
                    os64_gui_event_ring(p->win, RING_LOOK);
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
                os64_bell_ring(&p->picture_bell);
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
            os64_bell_ring(&p->picture_bell);
            os64_bell_ring(&p->audio_bell);
            os64_gui_event_ring(p->win, RING_LOOK);
        }
        if (c.n > 0) {
            os64_lock_release(&p->lock);
            collect_release(p, &c);
            os64_lock_acquire(&p->lock);
        }
    }
    os64_lock_release(&p->lock);
    os64_free(pcm);
    return 0;
}

// ---------------------------------------------------------------- the picture thread

static bool frame_fit(frame_t *f, uint32_t w, uint32_t h)
{
    size_t need = (size_t)w * h;
    if (need <= f->capacity)
        return true;
    os64_free(f->pixels);
    f->pixels = os64_malloc_aligned(need * sizeof(uint32_t), 64);
    f->capacity = f->pixels != 0 ? need : 0;
    return f->pixels != 0;
}

static int64_t picture_thread(void *arg)
{
    player_t *p = arg;
    os64_media_scaler_t *scaler = 0;
    uint32_t scaler_w = 0, scaler_h = 0;
    collect_t c = {0};
    os64_lock_acquire(&p->lock);
    while (!p->quit) {
        // A resize while not playing: the shown picture again, at the new size.
        // A shown frame whose source is out (a reconversion in flight, its
        // result not yet taken by main) waits: the newest size is converted
        // once main has swapped the result in.
        bool again = p->reconvert && p->shown != 0 && p->shown->source != 0 && p->free_frames != 0;
        if (p->reconvert && p->shown == 0)
            p->reconvert = false;
        const os64_media_pictures_entry_t *front = os64_media_pictures_peek(&p->decoded, 0);
        if (!again && (front == 0 || os64_media_pictures_full(&p->ready) || p->free_frames == 0)) {
            os64_lock_release(&p->lock);
            collect_release(p, &c);
            os64_bell_wait(&p->picture_bell, 200);
            os64_lock_acquire(&p->lock);
            continue;
        }
        os64_media_picture_t *source;
        int64_t at, duration;
        frame_t *shown_from = 0;
        if (again) {
            // The source goes with the conversion, so a retire of the shown
            // frame meanwhile cannot release it under the scaler.
            p->reconvert = false;
            shown_from = p->shown;
            source = shown_from->source;
            shown_from->source = 0;
            at = shown_from->at;
            duration = shown_from->duration;
        } else {
            // Skip what is overdue at the blank this conversion could reach.
            int64_t now = now_us();
            int64_t reach = os64_media_pace_reach(now, p->conversion_us, p->last_blank, p->period_us);
            int64_t clock = os64_media_clock_read(&p->clock, reach);
            while (os64_media_pace_skip(&p->pace, &p->decoded, clock)) {
                os64_media_pictures_entry_t e;
                os64_media_pictures_take(&p->decoded, &e);
                collect_add(&c, e.handle);
                os64_bell_ring(&p->decode_bell);
            }
            os64_media_pictures_entry_t e;
            os64_media_pictures_take(&p->decoded, &e);
            os64_bell_ring(&p->decode_bell);
            source = e.handle;
            at = e.at;
            duration = e.duration;
        }
        frame_t *f = p->free_frames;
        p->free_frames = f->next_free;
        uint32_t w = p->pic_w, h = p->pic_h, gen = os64_media_pictures_generation(&p->ready);
        p->converting = true;
        os64_lock_release(&p->lock);

        collect_release(p, &c);
        bool made = false;
        int64_t t0 = now_us();
        if (w != scaler_w || h != scaler_h || scaler == 0) {
            os64_media_scaler_destroy(scaler);
            scaler = 0;
            if (os64_media_scaler_create(w, h, OS64_MEDIA_SCALE_FAST, &scaler) == 0) {
                scaler_w = w;
                scaler_h = h;
            }
        }
        if (scaler != 0 && frame_fit(f, w, h))
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
        if (shown_from != 0) {
            // To main directly while the frame it replaces is still shown;
            // otherwise (a seek or a play moved on) the source goes back to
            // it if it is still shown, or is released with this frame.
            p->reconversions++;
            if (made && p->shown == shown_from && p->state != ST_PLAYING &&
                gen == os64_media_pictures_generation(&p->ready)) {
                frame_retire(p, p->reconverted, &c);
                p->reconverted = f;
                os64_gui_event_ring(p->win, RING_LOOK);
            } else {
                if (p->shown == shown_from && shown_from->source == 0) {
                    shown_from->source = source;
                    f->source = 0;
                }
                frame_retire(p, f, &c);
            }
            continue;
        }
        p->conversions++;
        int pushed = made ? os64_media_pictures_push(&p->ready, gen, f, at, duration) : OS64_MEDIA_PICTURES_REFUSED;
        if (pushed != OS64_MEDIA_PICTURES_TAKEN)
            frame_retire(p, f, &c);
        if (pushed == OS64_MEDIA_PICTURES_TAKEN && (p->owed || p->state != ST_PLAYING))
            os64_gui_event_ring(p->win, RING_LOOK);
    }
    os64_lock_release(&p->lock);
    collect_release(p, &c);
    os64_media_scaler_destroy(scaler);
    return 0;
}

// ---------------------------------------------------------------- the audio thread

typedef struct {
    int32_t audio, position, ctl;
    os64_media_voice_t voice;
    uint64_t written;                     // stream frames written this device generation
    uint32_t generation;
    bool generation_known;
    uint64_t last_heard;                  // the ear's stream frame at the last read
    bool paused;                          // `pause` sent
    bool fresh;                           // just opened or begun again: start from the clock's reading
    int16_t carry[2 * WRITE_FRAMES];      // a write's untaken tail, first next time
    uint32_t carry_frames;
    int64_t carry_at;
} device_t;

static void device_control(device_t *d, const char *verb)
{
    if (d->ctl >= 0)
        os64_write(d->ctl, verb, os64_strlen(verb));
}

static void device_restart(device_t *d)
{
    os64_media_voice_reset(&d->voice);
    d->written = 0;
    d->generation_known = false;
    d->last_heard = 0;
    d->carry_frames = 0;
}

static void device_close(device_t *d)
{
    device_control(d, "flush");
    if (d->paused)
        device_control(d, "resume");
    if (d->audio >= 0)
        os64_close(d->audio);
    if (d->position >= 0)
        os64_close(d->position);
    if (d->ctl >= 0)
        os64_close(d->ctl);
    d->audio = d->position = d->ctl = -1;
    d->paused = false;
    device_restart(d);
}

// Frames from the front of the sound queue that the clock has passed, let
// go unheard. Under the lock.
static void drop_behind(player_t *p, int64_t clock)
{
    int64_t front;
    while ((front = os64_media_queue_front(&p->pcm)) >= 0 && front < clock) {
        uint32_t n = os64_media_queue_take(&p->pcm, 0, (uint32_t)(clock - front < CHUNK_FRAMES ? clock - front
                                                                                                : CHUNK_FRAMES), 0);
        if (n == 0)
            break;
        p->pcm_dropped += n;
        os64_bell_ring(&p->decode_bell);
    }
}

static int64_t audio_thread(void *arg)
{
    player_t *p = arg;
    device_t d = {.audio = -1, .position = -1, .ctl = -1};
    device_restart(&d);
    int16_t buf[2 * WRITE_FRAMES];
    uint32_t served = 0, clock_gen = 0;
    os64_lock_acquire(&p->lock);
    while (!p->quit) {
        int64_t now = now_us();
        bool flush = p->seek_serial != served;
        if (flush) {
            served = p->seek_serial;
            clock_gen = os64_media_clock_generation(&p->clock);
        }
        bool hear = audible(p);
        // The device is wanted whenever the person has not muted a media
        // with sound; a refused one is tried again at retry_at.
        bool wanted = p->has_sound && !p->muted;
        // Writing: playing, or ended with a write's tail still to go;
        // nothing before the clock plays (Fable's B).
        bool run = p->state == ST_PLAYING || p->state == ST_ENDED;
        bool hold = p->state == ST_PAUSED;    // ended: the tail plays out
        bool try_open = wanted && d.audio < 0 && (!p->refused || now >= p->retry_at);
        int64_t clock_now = os64_media_clock_read(&p->clock, now);
        if (!hear && os64_media_clock_state(&p->clock, now) != OS64_MEDIA_CLOCK_PAUSED)
            drop_behind(p, clock_now);
        // Tick the window when no frame answer has come for a while (A).
        if (p->state == ST_PLAYING && now - p->last_answer >= TICK_US && now - p->last_tick >= TICK_US) {
            p->last_tick = now;
            os64_gui_event_ring(p->win, RING_TICK);
        }
        os64_lock_release(&p->lock);

        if (d.carry_frames > 0 && (flush || (!hear && d.audio >= 0))) {
            // A write's tail let go with the device's sound: counted.
            os64_lock_acquire(&p->lock);
            p->pcm_dropped += d.carry_frames;
            os64_lock_release(&p->lock);
            d.carry_frames = 0;
        }
        if (flush && d.audio >= 0) {
            device_control(&d, "flush");
            device_restart(&d);
        }
        if (!hear && d.audio >= 0)
            device_close(&d);
        if (try_open) {
            int64_t h = os64_open("/dev/audio", "w");
            os64_lock_acquire(&p->lock);
            if (h < 0) {
                p->refused = true;
                p->refused_why = h == OS64_AUDIO_BUSY ? "the device is busy" : "the device would not open";
                p->retry_at = now + RETRY_US;
                p->refusals++;
                os64_media_clock_source(&p->clock, OS64_MEDIA_CLOCK_MONOTONIC, now);
                os64_gui_event_ring(p->win, RING_LOOK);
            } else {
                p->refused = false;
                p->refused_why = 0;
                if (!p->muted)
                    os64_media_clock_source(&p->clock, OS64_MEDIA_CLOCK_HEARD, now);
                os64_gui_event_ring(p->win, RING_LOOK);
            }
            os64_lock_release(&p->lock);
            if (h >= 0) {
                d.audio = (int32_t)h;
                int64_t pos = os64_open("/dev/audiopos", "r");
                int64_t ctl = os64_open("/dev/audioctl", "w");
                d.position = pos >= 0 ? (int32_t)pos : -1;
                d.ctl = ctl >= 0 ? (int32_t)ctl : -1;
                d.fresh = true;
                device_restart(&d);
            }
        }
        if (d.audio >= 0 && hold != d.paused) {
            device_control(&d, hold ? "pause" : "resume");
            d.paused = hold;
        }

        // Write: nothing before the clock is playing (Fable's B).
        bool wrote_any = false;
        if (d.audio >= 0 && run && !d.paused) {
            uint32_t n = d.carry_frames;
            int64_t at = d.carry_at;
            if (n == 0) {
                os64_lock_acquire(&p->lock);
                if (d.fresh) {
                    drop_behind(p, os64_media_clock_read(&p->clock, now_us()));
                    d.fresh = false;
                }
                // A seek or a pause since this round looked: take nothing,
                // so the device is flushed before any sound after it goes in.
                bool still = p->seek_serial == served && (p->state == ST_PLAYING || p->state == ST_ENDED);
                n = still ? os64_media_queue_take(&p->pcm, buf, WRITE_FRAMES, &at) : 0;
                if (n > 0)
                    os64_bell_ring(&p->decode_bell);
                p->carry_frames = n;          // in flight from the take: the end waits for it
                os64_lock_release(&p->lock);
            } else {
                os64_memcpy(buf, d.carry, (size_t)n * 4);
            }
            if (n > 0) {
                int64_t w = os64_write_for(d.audio, buf, (size_t)n * 4, WRITE_PATIENCE_MS);
                uint32_t took = w > 0 ? (uint32_t)(w / 4) : 0;
                if (w < 0 && w != OS64_ERR_TIMEOUT && w != OS64_AUDIO_FLUSHED) {
                    os64_lock_acquire(&p->lock);
                    p->refused = true;
                    p->refused_why = "the device failed";
                    p->retry_at = now + RETRY_US;
                    os64_media_clock_source(&p->clock, OS64_MEDIA_CLOCK_MONOTONIC, now);
                    p->pcm_dropped += n;      // what the failed write held, counted
                    os64_lock_release(&p->lock);
                    took = 0;
                    n = 0;
                }
                if (took > 0) {
                    os64_media_voice_wrote(&d.voice, d.written, at, took);
                    d.written += took;
                    wrote_any = true;
                    os64_lock_acquire(&p->lock);
                    p->pcm_written += took;
                    os64_lock_release(&p->lock);
                }
                d.carry_frames = n - took;
                d.carry_at = at + took;
                if (d.carry_frames > 0)
                    os64_memcpy(d.carry, buf + 2 * took, (size_t)d.carry_frames * 4);
            }
        }
        os64_lock_acquire(&p->lock);
        p->carry_frames = d.carry_frames;     // the end waits for it (service)
        os64_lock_release(&p->lock);

        // What the ear heard, to the clock.
        if (d.position >= 0) {
            os64_audio_position_t pos;
            if (os64_read(d.position, &pos, sizeof(pos)) == (int64_t)sizeof(pos)) {
                // The sink's delay, from every read: a start's hold bound
                // needs it before anything is written (buffering waits).
                os64_lock_acquire(&p->lock);
                p->delay_us = pos.delay_us;
                if (!p->delay_known) {
                    p->delay_known = true;
                    if (p->state == ST_BUFFERING)
                        os64_gui_event_ring(p->win, RING_LOOK);   // buffering waited on it
                }
                os64_lock_release(&p->lock);
                if (!d.generation_known) {
                    d.generation = pos.generation;
                    d.generation_known = true;
                } else if (pos.generation != d.generation) {
                    // A generation this thread did not start: a sink attached
                    // or detached, or a flush from elsewhere, threw away what
                    // the stream held. Begin again from the clock's reading.
                    os64_lock_acquire(&p->lock);
                    p->pcm_dropped += d.carry_frames;
                    os64_lock_release(&p->lock);
                    device_restart(&d);
                    d.fresh = true;
                }
                if (pos.generation == d.generation && pos.accepted <= d.written && d.voice.anchors > 0) {
                    // PLAYING says the ring holds sound, not that the ear has
                    // heard it: `heard` stays put until the sink's delay has
                    // passed (after a start, a seek's flush or a resume). So
                    // a report is advancing only when the ear has moved, and
                    // a PLAYING stream whose ear has not moved says nothing
                    // (neither ending a wait nor starving the clock). An ear
                    // that stops while PLAYING (a sink no longer pulling, or
                    // pulling late) is the clock's to notice, by the report's
                    // trust (os64_media_clock_still); any state but PLAYING
                    // is said at once. A wait refuses such a report, so a
                    // start's protection holds.
                    int64_t t = now_us();
                    bool playing = pos.state == OS64_AUDIO_PLAYING;
                    bool moved = pos.heard > d.last_heard;
                    bool stopped = !playing;
                    d.last_heard = pos.heard;
                    if (moved || stopped) {
                        int64_t heard = os64_media_voice_heard(&d.voice, pos.heard);
                        os64_lock_acquire(&p->lock);
                        os64_media_clock_heard(&p->clock, clock_gen, heard + p->sync_offset, (int64_t)pos.micros,
                                               moved && playing, t);
                        os64_lock_release(&p->lock);
                    }
                    os64_media_voice_prune(&d.voice, pos.heard);
                }
            }
        }
        if (!wrote_any)
            os64_bell_wait(&p->audio_bell, 10);
        os64_lock_acquire(&p->lock);
    }
    p->pcm_dropped += d.carry_frames;         // a tail never written, counted
    os64_lock_release(&p->lock);
    if (d.audio >= 0) {
        // Leaving: what is queued plays out (a close never cuts the sound),
        // unless the person left early.
        if (!(p->state == ST_ENDED) && d.ctl >= 0)
            device_control(&d, "flush");
        if (d.paused)
            device_control(&d, "resume");
        os64_close(d.audio);
        if (d.position >= 0)
            os64_close(d.position);
        if (d.ctl >= 0)
            os64_close(d.ctl);
    }
    return 0;
}

// ---------------------------------------------------------------- the keys thread

static void post_action(player_t *p, action_t a)
{
    os64_lock_acquire(&p->lock);
    if (p->actions < ACTIONS)
        p->action[p->actions++] = a;
    os64_gui_event_ring(p->win, RING_ACTION);
    os64_lock_release(&p->lock);
}

// One `--script` step: "<ms>:<action>", the actions gplay_key's names
// (pause, back, forward, backfar, forwardfar, mute, louder, softer, later,
// earlier, readout, quit) or "resize=WxH".
static bool script_step(const char **cursor, int64_t *ms, action_t *a)
{
    const char *s = *cursor;
    while (*s == ',' || *s == ' ')
        s++;
    if (*s == '\0')
        return false;
    int64_t t = 0;
    while (*s >= '0' && *s <= '9')
        t = t * 10 + (*s++ - '0');
    if (*s++ != ':')
        return false;
    const char *word = s;
    while (*s != '\0' && *s != ',')
        s++;
    size_t len = (size_t)(s - word);
    static const struct { const char *name; gplay_key_t key; } names[] = {
        {"pause", GPLAY_KEY_PAUSE}, {"back", GPLAY_KEY_BACK}, {"forward", GPLAY_KEY_FORWARD},
        {"backfar", GPLAY_KEY_BACK_FAR}, {"forwardfar", GPLAY_KEY_FORWARD_FAR}, {"mute", GPLAY_KEY_MUTE},
        {"louder", GPLAY_KEY_LOUDER}, {"softer", GPLAY_KEY_SOFTER}, {"later", GPLAY_KEY_SOUND_LATER},
        {"earlier", GPLAY_KEY_SOUND_EARLIER}, {"readout", GPLAY_KEY_READOUT}, {"quit", GPLAY_KEY_QUIT},
    };
    *a = (action_t){GPLAY_KEY_NONE, 0, 0};
    if (len > 7 && os64_memcmp(word, "resize=", 7) == 0) {
        const char *n = word + 7;
        uint32_t w = 0, h = 0;
        while (*n >= '0' && *n <= '9')
            w = w * 10 + (uint32_t)(*n++ - '0');
        if (*n++ == 'x')
            while (*n >= '0' && *n <= '9')
                h = h * 10 + (uint32_t)(*n++ - '0');
        a->w = w;
        a->h = h;
    } else {
        for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++)
            if (os64_strlen(names[i].name) == len && os64_memcmp(names[i].name, word, len) == 0)
                a->key = names[i].key;
    }
    *ms = t;
    *cursor = s;
    return a->key != GPLAY_KEY_NONE || a->w > 0;
}

static int64_t keys_thread(void *arg)
{
    player_t *p = arg;
    int64_t session = os64_open("/dev/mediakeys", "u");
    const char *script = s_script;
    int64_t start = now_us(), when = -1;
    action_t next = {0};
    if (script != 0 && !script_step(&script, &when, &next))
        script = 0;
    int said = -1;
    for (;;) {
        os64_lock_acquire(&p->lock);
        bool quit = p->quit;
        int say = p->state == ST_PLAYING || p->state == ST_BUFFERING ? 0 : 1;
        os64_lock_release(&p->lock);
        if (quit)
            break;
        if (session >= 0 && say != said) {
            const char *word = say == 0 ? "playing" : "paused";
            os64_write((int32_t)session, word, os64_strlen(word));
            said = say;
        }
        if (script != 0 && now_us() - start >= when * 1000) {
            post_action(p, next);
            if (!script_step(&script, &when, &next))
                script = 0;
        }
        // A read with patience, so a quit is seen within it and the join
        // never waits on a press that does not come.
        if (session >= 0) {
            os64_media_key_t k;
            if (os64_read_for((int32_t)session, &k, sizeof(k), 50) == (int64_t)sizeof(k)) {
                gplay_key_t key = k.key == OS64_MEDIA_KEY_PLAY || k.key == OS64_MEDIA_KEY_PAUSE ? GPLAY_KEY_PAUSE
                                  : k.key == OS64_MEDIA_KEY_STOP                               ? GPLAY_KEY_QUIT
                                  : k.key == OS64_MEDIA_KEY_PREVIOUS                           ? GPLAY_KEY_BACK_FAR
                                                                                               : GPLAY_KEY_NONE;
                // Previous goes to the start: BACK_FAR with w = 1 says so.
                if (key != GPLAY_KEY_NONE)
                    post_action(p, (action_t){key, k.key == OS64_MEDIA_KEY_PREVIOUS ? 1u : 0u, 0});
            }
        } else {
            os64_sleep(50);
        }
    }
    if (session >= 0)
        os64_close((int32_t)session);
    return 0;
}

// ---------------------------------------------------------------- main: the window

typedef struct {
    os64_draw_ctx_t ctx;
    bool readout;
    // What the canvas holds, by the shown picture's serial and not its
    // frame's address: frames are reused, so a later picture can sit at the
    // address of the one drawn last.
    uint32_t canvas_shown;
    uint32_t canvas_layout;               // the layout serial it was drawn under
    bool frame_asked;
    bool dirty;                           // the canvas needs drawing and publishing
    uint8_t key_seq;                      // an arrow's burst, part read (gplay_key)
} view_t;

static void readout_lines(player_t *p, char lines[][96], uint32_t *count, int64_t now)
{
    char t[16], len[16];
    int64_t at = os64_media_clock_read(&p->clock, now);
    gplay_time_text(at, t, sizeof(t));
    gplay_time_text(p->length, len, sizeof(len));
    static const char *const clock_name[] = {"paused", "monotonic", "starting", "starting late", "awaiting",
                                             "following", "starved"};
    static const char *const pace_name[] = {"on time", "decoding late", "far behind"};
    int cs = os64_media_clock_state(&p->clock, now);
    int64_t drift = os64_media_clock_drift(&p->clock, now);
    uint32_t n = 0;
    os64_snprintf(lines[n++], 96, "%s / %s  %s  clock %s, drift %ld ms", t, p->length >= 0 ? len : "?",
                  kStateName[p->state], clock_name[cs], (long)(drift * 1000 / RATE));
    os64_snprintf(lines[n++], 96, "pace %s, lead %ld ms", pace_name[p->pace.state],
                  (long)(p->pace.lead * 1000 / RATE));
    os64_snprintf(lines[n++], 96, "shown %lu dropped %lu skipped %lu starved %lu", (unsigned long)p->pace.shown,
                  (unsigned long)p->pace.dropped, (unsigned long)p->pace.skipped, (unsigned long)p->pace.starved);
    os64_snprintf(lines[n++], 96, "resyncs %lu (%lu ms holding), display missed %lu",
                  (unsigned long)p->pace.resyncs, (unsigned long)(p->pace.far_behind_us / 1000),
                  (unsigned long)p->display_missed);
    os64_snprintf(lines[n++], 96, "queues: decoded %u/%u ready %u/%u sound %lu ms", os64_media_pictures_count(&p->decoded),
                  DECODED_DEPTH, os64_media_pictures_count(&p->ready), READY_DEPTH,
                  (unsigned long)(os64_media_queue_count(&p->pcm) * 1000ull / RATE));
    os64_snprintf(lines[n++], 96, "ear: stills %lu, held ahead %lu, stepped behind %lu",
                  (unsigned long)p->clock.stills, (unsigned long)p->clock.held_ahead,
                  (unsigned long)p->clock.jumped_behind);
    os64_snprintf(lines[n++], 96, "sync %ld ms  sink delay %u ms%s%s%s", (long)(p->sync_offset * 1000 / RATE),
                  p->delay_us / 1000, p->muted ? "  muted" : "", p->refused ? "  sound: " : "",
                  p->refused && p->refused_why ? p->refused_why : "");
    *count = n;
}

// The canvas: black, the frame at its own size centred in the window (a
// frame converted before a resize is drawn at its size until the next
// arrives), and the readout over it.
static void draw_canvas(player_t *p, view_t *v, frame_t *f, int64_t now)
{
    os64_gui_surface_t *s = &v->ctx.surf;
    os64_gui_rect_t all = {0, 0, (int32_t)s->width, (int32_t)s->height};
    os64_draw_fill_rect(s, all, 0xFF000000u);
    if (f != 0 && f->w > 0) {
        int32_t x = ((int32_t)s->width - (int32_t)f->w) / 2;
        int32_t y = ((int32_t)s->height - (int32_t)f->h) / 2;
        os64_draw_blit(s, x, y, f->pixels, f->w, f->h, f->w);
    }
    if (v->readout) {
        char lines[8][96];
        uint32_t count = 0;
        os64_lock_acquire(&p->lock);
        readout_lines(p, lines, &count, now);
        os64_lock_release(&p->lock);
        for (uint32_t i = 0; i < count; i++)
            os64_draw_text(s, 8, 8 + 16 * (int32_t)i, lines[i], os64_strlen(lines[i]), 0xFFFFFFFFu, 0xFF000000u);
    }
    os64_draw_publish(&v->ctx, 0);
}

static void set_title(player_t *p, const char *name)
{
    char title[160];
    const char *sound = !p->has_sound ? "" : p->muted ? " (muted)" : p->refused ? " (sound not playing: " : "";
    os64_snprintf(title, sizeof(title), "%s - %s%s%s%s%s", name, kStateName[p->state], sound,
                  p->refused && !p->muted && p->has_sound && p->refused_why ? p->refused_why : "",
                  p->refused && !p->muted && p->has_sound ? ")" : "",
                  p->info.rotation != 0 ? " (rotated, shown as stored)" : "");
    os64_gui_window_set_title(p->win, title, OS64_GUI_WINDOW_TITLE_UTF8);
}

// The picture size inside the window's content. Under the lock.
static void layout(player_t *p, uint32_t content_w, uint32_t content_h)
{
    gplay_rect_t r = gplay_letterbox(p->info.width, p->info.height, content_w, content_h);
    p->pic_w = r.w > 0 ? r.w : 1;
    p->pic_h = r.h > 0 ? r.h : 1;
    p->layout_serial++;
    if (p->state != ST_PLAYING)
        p->reconvert = true;
    os64_bell_ring(&p->picture_bell);
}

// A seek to `target`: every queue flushed, the clock and the pace told, the
// decoding thread asked. Under the lock; the pictures to release go to `c`.
static void seek_to(player_t *p, int64_t target, collect_t *c, int64_t now, bool play)
{
    void *ready_context[2] = {p, c};
    os64_media_pictures_flush(&p->decoded, release_decoded, c);
    os64_media_pictures_flush(&p->ready, release_ready, ready_context);
    p->pcm_dropped += os64_media_queue_count(&p->pcm);
    os64_media_queue_flush(&p->pcm);
    if (os64_media_clock_state(&p->clock, now) != OS64_MEDIA_CLOCK_PAUSED)
        os64_media_clock_pause(&p->clock, now);
    os64_media_clock_seek(&p->clock, target, now);
    os64_media_pace_reset(&p->pace, now);
    p->seek_target = target;
    p->seek_serial++;
    p->seek_pending = true;
    p->newest = -1;
    p->owed = true;
    p->stall_since = 0;
    // A reconversion of the picture from before the seek is not shown after it.
    frame_retire(p, p->reconverted, c);
    p->reconverted = 0;
    p->reconvert = false;
    p->state = play ? ST_BUFFERING : ST_PAUSED;
    if (play)
        p->buffering_since = now;
    os64_bell_ring(&p->decode_bell);
    os64_bell_ring(&p->picture_bell);
    os64_bell_ring(&p->audio_bell);
}

static void start_playing(player_t *p, int64_t now)
{
    os64_media_clock_hold(&p->clock, hold_bound(p));
    os64_media_clock_still(&p->clock, still_bound(p));
    os64_media_clock_play(&p->clock, now);
    p->state = ST_PLAYING;
    p->stall_since = 0;
    p->last_frame_number = 0;             // refreshes while not playing are not missed
    os64_bell_ring(&p->audio_bell);
}

static void volume_step(int delta)
{
    int64_t h = os64_open("/dev/volume", "r");
    char buf[16];
    int64_t n = h >= 0 ? os64_read((int32_t)h, buf, sizeof(buf) - 1) : -1;
    if (h >= 0)
        os64_close((int32_t)h);
    if (n <= 0)
        return;
    int v = 0;
    for (int64_t i = 0; i < n && buf[i] >= '0' && buf[i] <= '9'; i++)
        v = v * 10 + (buf[i] - '0');
    v += delta;
    v = v < 0 ? 0 : v > 100 ? 100 : v;
    char out[8];
    int len = os64_snprintf(out, sizeof(out), "%d\n", v);
    h = os64_open("/dev/volume", "w");
    if (h >= 0) {
        os64_write((int32_t)h, out, (size_t)len);
        os64_close((int32_t)h);
    }
}

// One key's ask. Under no lock; takes it.
static void do_key(player_t *p, view_t *v, gplay_key_t key, bool to_start, collect_t *c)
{
    int64_t now = now_us();
    switch (key) {
    case GPLAY_KEY_QUIT:
        os64_lock_acquire(&p->lock);
        p->quit = true;
        os64_lock_release(&p->lock);
        return;
    case GPLAY_KEY_READOUT:
        v->readout = !v->readout;
        v->dirty = true;
        return;
    case GPLAY_KEY_LOUDER:
    case GPLAY_KEY_SOFTER:
        volume_step(key == GPLAY_KEY_LOUDER ? 5 : -5);
        return;
    default:
        break;
    }
    os64_lock_acquire(&p->lock);
    switch (key) {
    case GPLAY_KEY_PAUSE:
        if (p->state == ST_PLAYING) {
            os64_media_clock_pause(&p->clock, now);
            p->state = ST_PAUSED;
        } else if (p->state == ST_BUFFERING) {
            p->state = ST_PAUSED;
        } else if (p->state == ST_PAUSED) {
            if (p->seek_pending || os64_media_pictures_count(&p->ready) == 0) {
                p->state = ST_BUFFERING;
                p->buffering_since = now;
            } else {
                start_playing(p, now);
            }
        } else if (p->state == ST_ENDED) {
            seek_to(p, 0, c, now, true);
        }
        os64_bell_ring(&p->audio_bell);
        break;
    case GPLAY_KEY_BACK:
    case GPLAY_KEY_FORWARD:
    case GPLAY_KEY_BACK_FAR:
    case GPLAY_KEY_FORWARD_FAR:
        if (p->state != ST_FAILED) {
            int64_t step = key == GPLAY_KEY_BACK ? -SEEK_STEP : key == GPLAY_KEY_FORWARD ? SEEK_STEP
                           : key == GPLAY_KEY_BACK_FAR                                  ? -SEEK_FAR
                                                                                        : SEEK_FAR;
            int64_t base = p->seek_pending ? p->seek_target : os64_media_clock_read(&p->clock, now);
            int64_t target = to_start ? 0 : gplay_seek_target(base, step, p->length);
            bool play = p->state == ST_PLAYING || p->state == ST_BUFFERING || p->state == ST_ENDED;
            seek_to(p, target, c, now, play);
        }
        break;
    case GPLAY_KEY_MUTE:
        p->muted = !p->muted;
        if (p->has_sound)
            os64_media_clock_source(&p->clock, audible(p) ? OS64_MEDIA_CLOCK_HEARD : OS64_MEDIA_CLOCK_MONOTONIC,
                                    now);
        os64_bell_ring(&p->audio_bell);
        break;
    case GPLAY_KEY_SOUND_LATER:
    case GPLAY_KEY_SOUND_EARLIER:
        // An offset below 0 reports the ear behind where it is, so the clock
        // and the pictures come later: the sound then lands EARLIER against
        // them. A beep heard after its flash wants `[`; the dial's value is
        // then how much the sink's reported delay falls short of the truth.
        p->sync_offset += key == GPLAY_KEY_SOUND_LATER ? SYNC_STEP : -SYNC_STEP;
        break;
    default:
        break;
    }
    os64_lock_release(&p->lock);
    v->dirty = true;
}

// Which picture the paint shows at `clock`: the pace's pick (forced to the
// first ready one when a picture is owed), the dropped ones collected. The
// frame to show comes back, NULL when the screen keeps what it has. Under
// the lock.
static frame_t *pick(player_t *p, int64_t clock, bool judging, collect_t *c, int64_t now)
{
    if (p->reconverted != 0 && !p->owed) {
        frame_t *old = p->shown;
        p->shown_serial++;
        p->shown = p->reconverted;
        p->reconverted = 0;
        frame_retire(p, old, c);
        return p->shown;
    }
    os64_media_pictures_entry_t shown_entry;
    const os64_media_pictures_entry_t *shown = 0;
    if (p->shown != 0) {
        shown_entry = (os64_media_pictures_entry_t){p->shown, p->shown->at, p->shown->duration};
        shown = &shown_entry;
    }
    os64_media_pace_pick_t k = os64_media_pace_pick(&p->pace, &p->ready, clock, shown, judging);
    if (!k.show && p->owed && os64_media_pictures_count(&p->ready) > 0)
        k = (os64_media_pace_pick_t){true, 0, false};
    if (!k.show)
        return 0;
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
    if (old != 0)
        frame_retire(p, old, c);
    p->owed = false;
    p->seek_pending = false;
    (void)now;
    return p->shown;
}

// The producers' state, once a paint. Under the lock.
static void judge(player_t *p, int64_t clock, int64_t now)
{
    bool judging = p->state == ST_PLAYING && !os64_media_pictures_ended(&p->decoded);
    bool sound_counts = audible(p) && !os64_media_queue_ended(&p->pcm) && !os64_media_pictures_ended(&p->decoded);
    os64_media_pace_measure_t m = {now, clock, judging, p->newest, os64_media_queue_count(&p->pcm),
                                   p->state == ST_PLAYING && sound_counts ? SOUND_FLOOR : 0};
    bool skip_was = p->pace.skip_unused;
    os64_media_pace_judge(&p->pace, &m);
    if (p->pace.skip_unused != skip_was || p->pace.resync_asked)
        os64_bell_ring(&p->decode_bell);
}

// The moves between states no event makes: buffering's end, a sound that
// ran dry or a sink that stalled, the sound's end, the media's end. Under
// the lock.
static void service(player_t *p, int64_t now)
{
    if (p->failure != 0 && p->state != ST_FAILED) {
        if (os64_media_clock_state(&p->clock, now) != OS64_MEDIA_CLOCK_PAUSED)
            os64_media_clock_pause(&p->clock, now);
        p->state = ST_FAILED;
        os64_bell_ring(&p->audio_bell);
        return;
    }
    if (p->state == ST_BUFFERING) {
        bool picture = os64_media_pictures_count(&p->ready) > 0 || (p->shown != 0 && !p->owed) ||
                       os64_media_pictures_finished(&p->decoded);
        // The sound is ready with its depth queued (or its end), and once the
        // sink's delay is known, which the start's hold bound is made from.
        bool sound = !audible(p) || ((os64_media_queue_count(&p->pcm) >= BUFFER_SOUND ||
                                      os64_media_queue_ended(&p->pcm)) &&
                                     p->delay_known);
        if (picture && sound)
            start_playing(p, now);
        return;
    }
    if (p->state != ST_PLAYING)
        return;
    int cs = os64_media_clock_state(&p->clock, now);
    // A STALLED sink: sound queued and the ear not advancing (starved), or a
    // start whose hold ran out with nothing heard at all (starting late:
    // its bound was already the sink's delay plus a margin).
    bool stalled = false;
    if (cs == OS64_MEDIA_CLOCK_STARVED) {
        if (os64_media_queue_finished(&p->pcm)) {
            // The sound has ended and the picture goes on.
            os64_media_clock_source(&p->clock, OS64_MEDIA_CLOCK_MONOTONIC, now);
        } else if (os64_media_queue_count(&p->pcm) == 0) {
            // The decoder fell behind the sound: a hold is buffering.
            os64_media_clock_pause(&p->clock, now);
            p->state = ST_BUFFERING;
            p->buffering_since = now;
            p->buffers++;
        } else {
            stalled = true;
        }
    } else if (cs == OS64_MEDIA_CLOCK_STARTING_LATE) {
        stalled = true;
    }
    // Awaiting (an unmute, a regained device) has no hold of its own to run
    // out: a wait longer than a start's bound is a stalled sink as well.
    if (cs != OS64_MEDIA_CLOCK_AWAITING)
        p->awaiting_since = 0;
    else if (p->awaiting_since == 0)
        p->awaiting_since = now;
    else if (now - p->awaiting_since >= hold_bound(p))
        stalled = true;
    if (!stalled) {
        p->stall_since = 0;
    } else if (p->stall_since == 0) {
        p->stall_since = now;
    } else if (now - p->stall_since >= STALL_US) {
        // A sink stalled for a second is a refused device.
        p->refused = true;
        p->refused_why = "the sink stopped";
        p->retry_at = now + RETRY_US;
        p->stalls++;
        p->stall_since = 0;
        os64_media_clock_source(&p->clock, OS64_MEDIA_CLOCK_MONOTONIC, now);
        os64_bell_ring(&p->audio_bell);
    }
    // The end: every picture shown (none in a queue or being converted) and
    // every frame of sound given to the device (none in a write's tail).
    bool pictures_done = os64_media_pictures_finished(&p->decoded) && os64_media_pictures_count(&p->ready) == 0 &&
                         !p->converting;
    bool sound_done = !p->has_sound || (os64_media_queue_finished(&p->pcm) && p->carry_frames == 0);
    if (pictures_done && sound_done) {
        int64_t end = -1;
        if (p->shown != 0) {
            int64_t lasts = p->shown->duration > 0 ? p->shown->duration : p->pace.period;
            end = p->shown->at + lasts;
        }
        if (end < 0 || os64_media_clock_read(&p->clock, now) >= end) {
            os64_media_clock_pause(&p->clock, now);
            p->state = ST_ENDED;
            os64_bell_ring(&p->audio_bell);
        }
    }
}

static int run(player_t *p, view_t *v, const char *name)
{
    int last_state = -1;
    bool last_refused = false, last_muted = false;
    collect_t c = {0};
    for (;;) {
        int64_t now = now_us();
        os64_lock_acquire(&p->lock);
        service(p, now);
        bool quit = p->quit || (s_exit_at_end && (p->state == ST_ENDED || p->state == ST_FAILED));
        bool want_frame = p->state == ST_PLAYING || (p->owed && os64_media_pictures_count(&p->ready) > 0) ||
                          (p->reconverted != 0 && !p->owed) ||
                          v->dirty || v->canvas_shown != p->shown_serial || v->canvas_layout != p->layout_serial;
        bool title = p->state != last_state || p->refused != last_refused || p->muted != last_muted;
        last_state = p->state;
        last_refused = p->refused;
        last_muted = p->muted;
        os64_lock_release(&p->lock);
        if (title)
            set_title(p, name);
        collect_release(p, &c);
        if (quit)
            break;
        if (want_frame && !v->frame_asked) {
            if (os64_gui_frame_request(p->win, 0) == 0)
                v->frame_asked = true;
        }
        os64_gui_event_t ev;
        if (os64_gui_event_wait(p->win, &ev) != 1)
            break;
        now = now_us();
        switch (ev.type) {
        case OS64_GUI_EVENT_FRAME: {
            v->frame_asked = false;
            uint64_t number = os64_gui_frame_number(&ev);
            int64_t period = (int64_t)ev.frame.period_ns / 1000;
            // A publish reaches the glass one refresh after the answer's blank.
            int64_t blank = (int64_t)os64_gui_frame_blank_us(&ev) + period;
            os64_lock_acquire(&p->lock);
            if (p->last_frame_number != 0 && number > p->last_frame_number + 1 && p->state == ST_PLAYING)
                p->display_missed += number - p->last_frame_number - 1;
            p->last_frame_number = number;
            p->last_blank = blank;
            p->period_us = period;
            os64_media_clock_still(&p->clock, still_bound(p));   // main reads a period ahead
            p->last_answer = now;
            int64_t clock = os64_media_clock_read(&p->clock, blank);
            bool judging = p->state == ST_PLAYING && !os64_media_pictures_ended(&p->decoded);
            frame_t *show = pick(p, clock, judging, &c, now);
            if (p->state == ST_PLAYING)
                judge(p, clock, now);
            frame_t *f = show != 0 ? show : p->shown;
            bool draw = show != 0 || v->dirty || v->canvas_shown != p->shown_serial ||
                        v->canvas_layout != p->layout_serial;
            uint32_t layout_serial = p->layout_serial, shown_serial = p->shown_serial;
            os64_lock_release(&p->lock);
            if (draw) {
                draw_canvas(p, v, f, now);
                v->canvas_shown = shown_serial;
                v->canvas_layout = layout_serial;
                v->dirty = false;
            }
            break;
        }
        case OS64_GUI_EVENT_DOORBELL:
            if (ev.doorbell.mask & RING_TICK) {
                // No frame answer for a while: retire by the clock, unseen (A).
                os64_lock_acquire(&p->lock);
                if (p->state == ST_PLAYING && now - p->last_answer >= TICK_US)
                    pick(p, os64_media_clock_read(&p->clock, now), false, &c, now);
                os64_lock_release(&p->lock);
            }
            if (ev.doorbell.mask & RING_ACTION) {
                action_t a[ACTIONS];
                os64_lock_acquire(&p->lock);
                uint32_t n = p->actions;
                for (uint32_t i = 0; i < n; i++)
                    a[i] = p->action[i];
                p->actions = 0;
                os64_lock_release(&p->lock);
                for (uint32_t i = 0; i < n; i++) {
                    if (a[i].key == GPLAY_KEY_NONE && a[i].w > 0 && a[i].h > 0)
                        os64_gui_window_set_content_size(p->win, a[i].w, a[i].h);
                    else if (a[i].key != GPLAY_KEY_NONE)
                        do_key(p, v, a[i].key, a[i].key == GPLAY_KEY_BACK_FAR && a[i].w == 1, &c);
                    collect_release(p, &c);
                }
            }
            break;
        case OS64_GUI_EVENT_KEY_DOWN:
            do_key(p, v,
                   gplay_key(&v->key_seq, ev.key.ascii, ev.key.scancode,
                             (ev.key.modifiers & OS64_GUI_MOD_SHIFT) != 0),
                   false, &c);
            break;
        case OS64_GUI_EVENT_WINDOW_RESIZE:
            os64_draw_ctx_refresh(&v->ctx);
            os64_lock_acquire(&p->lock);
            layout(p, v->ctx.surf.width, v->ctx.surf.height);
            os64_lock_release(&p->lock);
            v->dirty = true;
            break;
        case OS64_GUI_EVENT_WINDOW_CLOSE:
            os64_lock_acquire(&p->lock);
            p->quit = true;
            os64_lock_release(&p->lock);
            break;
        default:
            break;
        }
    }
    collect_release(p, &c);
    return 0;
}

// ---------------------------------------------------------------- start and end

static void say_counts(player_t *p, const char *name)
{
    os64_media_counts_t mc;
    os64_media_counts(p->media, &mc);
    char at[16];
    gplay_time_text(os64_media_clock_read(&p->clock, now_us()), at, sizeof(at));
    os64_printf("gplay: %s: %s at %s\n", name, kStateName[p->state], at);
    os64_printf("  pictures: decoded %lu, converted %lu (again %lu), shown %lu, dropped %lu, skipped %lu, "
                "starved %lu\n",
                (unsigned long)p->pictures_pushed, (unsigned long)p->conversions, (unsigned long)p->reconversions,
                (unsigned long)p->pace.shown, (unsigned long)p->pace.dropped, (unsigned long)p->pace.skipped,
                (unsigned long)p->pace.starved);
    os64_printf("  late: resyncs %lu (%lu ms holding), into decoding late %lu, far by sound %lu, display missed "
                "%lu\n",
                (unsigned long)p->pace.resyncs, (unsigned long)(p->pace.far_behind_us / 1000),
                (unsigned long)p->pace.entered[OS64_MEDIA_PACE_DECODING_LATE], (unsigned long)p->pace.far_by_sound,
                (unsigned long)p->display_missed);
    os64_printf("  sound: decoded %lu frames, written %lu, let go unheard %lu; buffering %lu, stalls %lu, "
                "refusals %lu\n",
                (unsigned long)p->pcm_pushed, (unsigned long)p->pcm_written, (unsigned long)p->pcm_dropped,
                (unsigned long)p->buffers, (unsigned long)p->stalls, (unsigned long)p->refusals);
    os64_printf("  clock: reports %lu, stale %lu, starts late %lu, starves %lu; sink delay %u ms, sync %ld ms\n",
                (unsigned long)p->clock.reports, (unsigned long)p->clock.reports_stale,
                (unsigned long)p->clock.starts_late, (unsigned long)p->clock.starves, p->delay_us / 1000,
                (long)(p->sync_offset * 1000 / RATE));
    const uint64_t *age = p->clock.report_ages;
    os64_printf("  ear: first heard after %ld ms; stills %lu, held ahead %lu, stepped behind %lu (trust %ld ms)\n",
                (long)(p->clock.start_wait_us / 1000), (unsigned long)p->clock.stills,
                (unsigned long)p->clock.held_ahead, (unsigned long)p->clock.jumped_behind,
                (long)(p->clock.still_us / 1000));
    os64_printf("  report ages: to 20 ms %lu, 40 %lu, 60 %lu, 80 %lu, 120 %lu, over %lu\n", (unsigned long)age[0],
                (unsigned long)age[1], (unsigned long)age[2], (unsigned long)age[3], (unsigned long)age[4],
                (unsigned long)age[5]);
    os64_printf("  media: picture packets %lu, run-in %lu, resync-dropped %lu\n", (unsigned long)mc.picture_packets,
                (unsigned long)mc.pictures_run_in, (unsigned long)mc.pictures_resync);
}

// --check: the counts a run that played to its end must have.
static bool counts_hold(player_t *p, const char *name)
{
    bool ok = true;
    if (p->state != ST_ENDED) {
        os64_printf("gplay: check: %s did not end (%s)\n", name, kStateName[p->state]);
        ok = false;
    }
    if (p->pace.shown == 0 || p->shown == 0) {
        os64_printf("gplay: check: no picture was shown\n");
        ok = false;
    } else if (p->length > 0 && p->pace.period > 0) {
        // The pictures reached the end: the last one shown ends within
        // three periods of the media's length (a clip whose picture runs
        // as long as its sound, as the shipped ones do).
        int64_t lasts = p->shown->duration > 0 ? p->shown->duration : p->pace.period;
        int64_t end = p->shown->at + lasts;
        if (end < p->length - 3 * p->pace.period) {
            os64_printf("gplay: check: the last picture shown ends at frame %ld of %ld\n", (long)end,
                        (long)p->length);
            ok = false;
        }
    }
    // Every frame of sound decoded was written or let go for a reason that
    // counted it (a seek, a mute, a refusal, a failed write, a tail).
    if (p->has_sound && p->pcm_written + p->pcm_dropped != p->pcm_pushed) {
        os64_printf("gplay: check: sound decoded %lu, written %lu, let go %lu\n", (unsigned long)p->pcm_pushed,
                    (unsigned long)p->pcm_written, (unsigned long)p->pcm_dropped);
        ok = false;
    }
    if (s_expect_refusal && (p->refusals == 0 || p->refused || p->pcm_written == 0)) {
        os64_printf("gplay: check: wanted the device refused and then regained (refusals %lu, refused now %d, "
                    "written %lu)\n",
                    (unsigned long)p->refusals, p->refused, (unsigned long)p->pcm_written);
        ok = false;
    }
    return ok;
}

static int64_t s_threads[4];

int main(int argc, char **argv)
{
    int first = 1;
    while (first < argc && argv[first][0] == '-' && argv[first][1] == '-') {
        if (os64_strcmp(argv[first], "--exit-at-end") == 0)
            s_exit_at_end = true;
        else if (os64_strcmp(argv[first], "--check") == 0)
            s_check = s_exit_at_end = true;
        else if (os64_strcmp(argv[first], "--expect-refusal") == 0)
            s_expect_refusal = true;
        else if (os64_strcmp(argv[first], "--maximized") == 0)
            s_maximized = true;
        else if (os64_strcmp(argv[first], "--cover") == 0)
            s_cover = true;
        else if (os64_strcmp(argv[first], "--script") == 0 && first + 1 < argc)
            s_script = argv[++first];
        else
            break;
        first++;
    }
    if (first != argc - 1) {
        os64_printf("usage: gplay [--exit-at-end] [--maximized] [--check [--expect-refusal]] [--cover] "
                    "[--script steps] file\n");
        return 1;
    }
    const char *path = argv[first];
    const char *name = path;
    for (const char *s = path; *s; s++)
        if (*s == '/')
            name = s + 1;

    int64_t file = os64_open(path, "r");
    if (file < 0) {
        os64_printf("gplay: %s: cannot open\n", path);
        return 2;
    }
    player_t *p = os64_calloc(1, sizeof(player_t));
    view_t v = {0};
    os64_media_source_t src;
    os64_media_file_source((int32_t)file, &src);
    int r = os64_media_open_for(&src, OS64_MEDIA_WANT_SOUND | OS64_MEDIA_WANT_PICTURE, &p->media, &p->info);
    if (r == 0 && p->info.picture != OS64_MEDIA_PLAYABLE) {
        os64_media_close(p->media);
        r = OS64_MEDIA_NO_PICTURE;
    }
    if (r != 0) {
        if (r == OS64_MEDIA_NO_PICTURE)
            os64_printf("gplay: %s has no picture; `play %s` plays its sound\n", path, path);
        else
            os64_printf("gplay: %s: %s\n", path, os64_media_strerror(r));
        os64_close((int32_t)file);
        os64_free(p);
        return 2;
    }
    p->has_sound = p->info.sound == OS64_MEDIA_PLAYABLE;
    p->length = p->info.frames;

    uint32_t sw = 0, sh = 0;
    os64_gui_screen_info(&sw, &sh);
    uint32_t w, h;
    gplay_first_size(p->info.width, p->info.height, sw, sh, MARGIN, &w, &h);
    int32_t x = sw > w ? (int32_t)(sw - w) / 2 : 0, y = sh > h ? (int32_t)(sh - h) / 2 : 0;
    p->win = os64_gui_window_create_content(name, x, y, w, h,
                                            OS64_GUI_CREATE_FIT_SCREEN |
                                                (s_maximized ? OS64_GUI_CREATE_MAXIMIZED : 0));
    if (p->win <= 0 || os64_draw_ctx_init(&v.ctx, p->win) != 0) {
        os64_printf(p->win == OS64_GUI_ERR_NOT_RUNNING ? "gplay: no desktop on this boot\n"
                                                       : "gplay: the window would not open\n");
        os64_media_close(p->media);
        os64_close((int32_t)file);
        os64_free(p);
        return 1;
    }
    int64_t cover = -1;
    if (s_cover) {
        // A window of its own over its own, the whole screen: gplay's is
        // then answered no frames, as a covered or minimised one is.
        cover = os64_gui_window_create("gplay cover", 0, 0, sw, sh, OS64_GUI_WINDOW_NO_DECORATIONS);
        os64_draw_ctx_t cc;
        if (cover > 0 && os64_draw_ctx_init(&cc, cover) == 0) {
            os64_gui_rect_t all = {0, 0, (int32_t)cc.surf.width, (int32_t)cc.surf.height};
            os64_draw_fill_rect(&cc.surf, all, 0xFF202020u);
            os64_draw_publish(&cc, 0);
        }
    }

    os64_media_pictures_init(&p->decoded, p->decoded_slot, DECODED_DEPTH);
    os64_media_pictures_init(&p->ready, p->ready_slot, READY_DEPTH);
    int16_t *pcm_storage = os64_malloc(sizeof(int16_t) * 2 * PCM_FRAMES);
    os64_media_queue_init(&p->pcm, pcm_storage, pcm_storage != 0 ? PCM_FRAMES : 0);
    os64_media_clock_init(&p->clock, p->has_sound ? OS64_MEDIA_CLOCK_HEARD : OS64_MEDIA_CLOCK_MONOTONIC);
    int64_t period = p->info.rate_num > 0 ? (int64_t)RATE * p->info.rate_den / p->info.rate_num : 0;
    os64_media_pace_init(&p->pace, DECODED_DEPTH, period);
    for (int i = 0; i < FRAMES; i++) {
        p->frame[i].next_free = p->free_frames;
        p->free_frames = &p->frame[i];
    }
    p->newest = -1;
    p->state = ST_BUFFERING;
    p->owed = true;
    p->buffering_since = now_us();
    layout(p, v.ctx.surf.width, v.ctx.surf.height);
    os64_bell_make(&p->decode_bell);
    os64_bell_make(&p->picture_bell);
    os64_bell_make(&p->audio_bell);
    draw_canvas(p, &v, 0, now_us());

    s_threads[0] = os64_thread(decoding_thread, p);
    s_threads[1] = os64_thread(picture_thread, p);
    s_threads[2] = os64_thread(audio_thread, p);
    s_threads[3] = os64_thread(keys_thread, p);
    run(p, &v, name);

    os64_lock_acquire(&p->lock);
    p->quit = true;
    os64_bell_ring(&p->decode_bell);
    os64_bell_ring(&p->picture_bell);
    os64_bell_ring(&p->audio_bell);
    os64_lock_release(&p->lock);
    for (int i = 0; i < 4; i++)
        if (s_threads[i] > 0)
            os64_thread_join((int32_t)s_threads[i], 0);

    if (p->failure != 0)
        os64_printf("gplay: %s: %s\n", path, os64_media_strerror(p->failure));
    say_counts(p, name);
    bool checked = !s_check || counts_hold(p, name);

    // Every picture back before the media closes.
    collect_t c = {0};
    void *ready_context[2] = {p, &c};
    os64_media_pictures_flush(&p->decoded, release_decoded, &c);
    collect_release(p, &c);
    os64_media_pictures_flush(&p->ready, release_ready, ready_context);
    collect_release(p, &c);
    frame_retire(p, p->shown, &c);
    frame_retire(p, p->reconverted, &c);
    collect_release(p, &c);
    for (int i = 0; i < FRAMES; i++)
        os64_free(p->frame[i].pixels);
    os64_media_close(p->media);
    os64_close((int32_t)file);
    os64_bell_close(&p->decode_bell);
    os64_bell_close(&p->picture_bell);
    os64_bell_close(&p->audio_bell);
    if (cover > 0)
        os64_gui_window_destroy(cover);
    os64_gui_window_destroy(p->win);
    os64_free(pcm_storage);
    int code = p->failure != 0 || !checked ? 3 : 0;
    os64_free(p);
    return code;
}
