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
// THE PLAYER is libmedia's (os64/media_player.h): its threads decode,
// convert and keep the state, and its sound is a voice of the program's
// output (os64/media_output.h). gplay is the window over it: the letterbox,
// the canvas, the keys, the title and the readout. VIDEO.md § 3 is the
// design.
//
// ON A PLANE (VIDEO_PLANE.md § 6), which gplay asks the player for first:
// the player's show thread publishes every picture to a plane of the
// window, and main paints the window only when something but the picture
// changes: black bars opaque and the picture's rectangle see-through,
// placed with the plane in one publish once the plane is READY for that
// size; the readout an overlay rectangle over it; a new size by painting
// the video itself (`shown`) and hiding the plane until it is ready again.
// With no plane (refused, or ended) it is the window that paints every
// picture, as below.
//
// THE THREADS. main: the window, and without a plane the frame requests and
// at each frame answer the player's picture for that blank; every player
// call. keys: the media-key session and a --script's timed actions, handed
// to main through the window's doorbell. The app's lock guards only what
// the two share (the actions, whether it is playing).
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
#include "os64/media_output.h"
#include "os64/media_player.h"
#include "os64/mediakeys.h"
#include "os64/thread.h"

#include "view.h"

#define RATE OS64_AUDIO_RATE
#define SEEK_STEP (5 * (int64_t)RATE)
#define SEEK_FAR (60 * (int64_t)RATE)
#define SYNC_STEP ((int64_t)RATE / 100)   // 10 ms (signed: it is negated)
#define MARGIN 48                         // the first window leaves this much of the screen
#define ACTIONS 16
#define READOUT_LINES 8                   // what readout_lines writes at most
#define OPEN_WAIT_MS 30000                // a probe that says nothing for this long fails the open
#define TAIL_WAIT_MS 1000                 // at the end, the sound still in the device plays out this long

// The window's doorbell (os64_gui_event_ring): why main is to look.
#define RING_LOOK 1u                      // the player's facts changed
#define RING_ACTION 2u                    // a key or a script action is waiting

static const char *const kStateName[] = {"opening", "buffering", "playing", "paused", "ended", "failed"};

// A script's action, or a key's, waiting for main.
typedef struct {
    gplay_key_t key;
    uint32_t w, h;                        // a resize (key NONE, w > 0); with BACK_FAR, w = 1 is "to the start"
} action_t;

typedef struct {
    os64_lock_t lock;                     // the actions and `playing`, which the keys thread shares
    os64_media_player_t *player;
    os64_media_output_t *output;
    int64_t win;                          // 0 until the window exists; `look` rings it then
    os64_bell_t bell;                     // `look` and `finished` ring it before the window exists
    bool finished, quit, playing;
    action_t action[ACTIONS];
    uint32_t actions;
    // main's own
    bool muted;
    int64_t sync;
    uint64_t display_missed, last_frame_number;
} app_t;

static bool s_exit_at_end, s_check, s_cover, s_maximized, s_expect_refusal;
static const char *s_script;

static int64_t now_us(void)
{
    return os64_micros();
}

// The player's facts changed: called by its threads (and its output's) under
// the player's lock, so it only rings.
static void on_look(void *ctx)
{
    app_t *a = ctx;
    (void)os64_bell_ring(&a->bell);
    if (a->win > 0)
        os64_gui_event_ring(a->win, RING_LOOK);
}

static void on_finished(void *ctx)
{
    app_t *a = ctx;
    os64_lock_acquire(&a->lock);
    a->finished = true;
    (void)os64_bell_ring(&a->bell);
    os64_lock_release(&a->lock);
}

// ---------------------------------------------------------------- the keys thread

static void post_action(app_t *a, action_t act)
{
    os64_lock_acquire(&a->lock);
    if (a->actions < ACTIONS)
        a->action[a->actions++] = act;
    os64_gui_event_ring(a->win, RING_ACTION);
    os64_lock_release(&a->lock);
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
    app_t *a = arg;
    int64_t session = os64_open("/dev/mediakeys", "u");
    const char *script = s_script;
    int64_t start = now_us(), when = -1;
    action_t next = {0};
    if (script != 0 && !script_step(&script, &when, &next))
        script = 0;
    int said = -1;
    for (;;) {
        os64_lock_acquire(&a->lock);
        bool quit = a->quit;
        int say = a->playing ? 0 : 1;
        os64_lock_release(&a->lock);
        if (quit)
            break;
        if (session >= 0 && say != said) {
            const char *word = say == 0 ? "playing" : "paused";
            os64_write((int32_t)session, word, os64_strlen(word));
            said = say;
        }
        if (script != 0 && now_us() - start >= when * 1000) {
            post_action(a, next);
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
                    post_action(a, (action_t){key, k.key == OS64_MEDIA_KEY_PREVIOUS ? 1u : 0u, 0});
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
    bool frame_asked;
    bool dirty;                           // the canvas needs drawing and publishing
    bool poked;                           // the player said look: ask for a frame even while not playing
    uint8_t key_seq;                      // an arrow's burst, part read (gplay_key)
    os64_media_player_frame_t shown;      // the picture on the canvas (the player's pixels)
    // On a plane: the show thread paces and main does no per-picture work.
    bool plane;
    bool placed;                          // the plane is placed in the last publish
    gplay_rect_t hole;                    // the picture's rectangle in the content: the plane's place
    uint32_t refresh;                     // frames since the readout was last painted over a plane
} view_t;

static void readout_lines(app_t *a, const os64_media_player_facts_t *f, char lines[][96], uint32_t *count)
{
    char t[16], len[16];
    gplay_time_text(f->position, t, sizeof(t));
    gplay_time_text(f->length, len, sizeof(len));
    static const char *const clock_name[] = {"paused", "monotonic", "starting", "starting late", "awaiting",
                                             "following", "starved", "still"};
    static const char *const pace_name[] = {"on time", "decoding late", "far behind"};
    uint32_t n = 0;
    os64_snprintf(lines[n++], 96, "%s / %s  %s  clock %s, drift %ld ms", t, f->length >= 0 ? len : "?",
                  kStateName[f->state], clock_name[f->clock_state], (long)(f->drift * 1000 / RATE));
    os64_snprintf(lines[n++], 96, "pace %s, lead %ld ms", pace_name[f->pace.state],
                  (long)(f->pace.lead * 1000 / RATE));
    os64_snprintf(lines[n++], 96, "shown %lu dropped %lu skipped %lu starved %lu", (unsigned long)f->pace.shown,
                  (unsigned long)f->pace.dropped, (unsigned long)f->pace.skipped, (unsigned long)f->pace.starved);
    os64_snprintf(lines[n++], 96, "resyncs %lu (%lu ms holding), display missed %lu",
                  (unsigned long)f->pace.resyncs, (unsigned long)(f->pace.far_behind_us / 1000),
                  (unsigned long)(f->plane_state == OS64_MEDIA_PLANE_ON ? f->frames_missed : a->display_missed));
    os64_snprintf(lines[n++], 96, "queues: decoded %u/%u ready %u/%u sound %lu ms", f->decoded, f->decoded_depth,
                  f->ready, f->ready_depth, (unsigned long)(f->sound_queued * 1000ull / RATE));
    os64_snprintf(lines[n++], 96, "ear: stills %lu, held ahead %lu, stepped behind %lu",
                  (unsigned long)f->clock.stills, (unsigned long)f->clock.held_ahead,
                  (unsigned long)f->clock.jumped_behind);
    os64_snprintf(lines[n++], 96, "sync %ld ms  sink delay %u ms%s%s%s", (long)(f->sync * 1000 / RATE),
                  f->delay_us / 1000, f->muted ? "  muted" : "", f->refused ? "  sound: " : "",
                  f->refused && f->refused_why ? f->refused_why : "");
    if (f->plane_state == OS64_MEDIA_PLANE_ON)
        os64_snprintf(lines[n++], 96, "plane %u %ux%u: published %lu, waits %lu, %s", f->plane_name, f->plane_cap_w,
                      f->plane_cap_h, (unsigned long)f->plane_published, (unsigned long)f->waits_patience,
                      f->plane_stale ? "stale" : f->plane_ready ? "ready" : "not ready");
    *count = n;
}

// The canvas: black, the picture at its own size centred in the window (one
// converted before a resize is drawn at its size until the next arrives),
// and the readout over it.
static void draw_canvas(app_t *a, view_t *v)
{
    os64_gui_surface_t *s = &v->ctx.surf;
    os64_gui_rect_t all = {0, 0, (int32_t)s->width, (int32_t)s->height};
    os64_draw_fill_rect(s, all, 0xFF000000u);
    if (v->shown.pixels != 0 && v->shown.w > 0) {
        int32_t x = ((int32_t)s->width - (int32_t)v->shown.w) / 2;
        int32_t y = ((int32_t)s->height - (int32_t)v->shown.h) / 2;
        os64_draw_blit(s, x, y, v->shown.pixels, v->shown.w, v->shown.h, v->shown.w);
    }
    if (v->readout) {
        os64_media_player_facts_t f;
        os64_media_player_facts(a->player, &f);
        char lines[READOUT_LINES][96];
        uint32_t count = 0;
        readout_lines(a, &f, lines, &count);
        for (uint32_t i = 0; i < count; i++)
            os64_draw_text(s, 8, 8 + 16 * (int32_t)i, lines[i], os64_strlen(lines[i]), 0xFFFFFFFFu, 0xFF000000u);
    }
    os64_draw_publish(&v->ctx, 0);
}

// ---------------------------------------------------------------- main: on a plane

// The readout's box (content coordinates) as painted at (8, 8).
static os64_gui_rect_t readout_box(char lines[][96], uint32_t count)
{
    uint32_t widest = 0;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t n = (uint32_t)os64_strlen(lines[i]);
        widest = n > widest ? n : widest;
    }
    return (os64_gui_rect_t){8, 8, (int32_t)widest * 8, 16 * (int32_t)count};
}

// The window over a plane: black (opaque) everywhere, the picture's
// rectangle see-through when the plane is placed (`hole`) or the video
// itself painted there from `shown` when it is not, and the readout over
// both, opaque, in a box the publish names as an overlay. The box, cut to
// the hole, is returned in *overlay (empty when there is none).
static void paint_plane_window(app_t *a, view_t *v, bool hole, os64_gui_rect_t *overlay)
{
    os64_gui_surface_t *s = &v->ctx.surf;
    os64_draw_fill_rect(s, (os64_gui_rect_t){0, 0, (int32_t)s->width, (int32_t)s->height}, 0xFF000000u);
    os64_gui_rect_t r = {v->hole.x, v->hole.y, (int32_t)v->hole.w, (int32_t)v->hole.h};
    if (hole) {
        for (int32_t y = 0; y < r.h; y++)
            for (int32_t x = 0; x < r.w; x++)
                s->pixels[(size_t)(r.y + y) * s->pitch_px + (size_t)(r.x + x)] = 0;
    } else {
        os64_media_player_view_t view;
        if (os64_media_player_shown(a->player, &view) == 0) {
            int32_t x = r.x + (r.w - (int32_t)view.w) / 2, y = r.y + (r.h - (int32_t)view.h) / 2;
            os64_draw_blit(s, x, y, view.pixels, view.w, view.h, view.w);
            os64_media_player_shown_done(a->player, &view);
        }
    }
    *overlay = (os64_gui_rect_t){0, 0, 0, 0};
    if (v->readout) {
        os64_media_player_facts_t f;
        os64_media_player_facts(a->player, &f);
        char lines[READOUT_LINES][96];
        uint32_t count = 0;
        readout_lines(a, &f, lines, &count);
        for (uint32_t i = 0; i < count; i++)
            os64_draw_text(s, 8, 8 + 16 * (int32_t)i, lines[i], os64_strlen(lines[i]), 0xFFFFFFFFu, 0xFF000000u);
        os64_gui_rect_t box = readout_box(lines, count);
        int32_t x0 = box.x > r.x ? box.x : r.x, y0 = box.y > r.y ? box.y : r.y;
        int32_t x1 = box.x + box.w < r.x + r.w ? box.x + box.w : r.x + r.w;
        int32_t y1 = box.y + box.h < r.y + r.h ? box.y + box.h : r.y + r.h;
        if (x1 > x0 && y1 > y0)
            *overlay = (os64_gui_rect_t){x0, y0, x1 - x0, y1 - y0};
    }
}

// The window's publish with the plane placed in the hole (the readout its
// overlay) or hidden. False when the kernel refused it.
static bool plane_publish(app_t *a, view_t *v, bool place)
{
    os64_media_player_facts_t f;
    os64_media_player_facts(a->player, &f);
    os64_gui_rect_t overlay;
    paint_plane_window(a, v, place, &overlay);
    os64_gui_plane_edit_t e = {0};
    e.plane = f.plane_name;
    if (place) {
        e.origin = (os64_gui_rect_t){v->hole.x, v->hole.y, (int32_t)v->hole.w, (int32_t)v->hole.h};
        e.clip = e.origin;
        if (overlay.w > 0) {
            e.noverlays = 1;
            e.overlay[0] = overlay;
        }
    } else {
        e.hide = 1;
    }
    bool done = os64_gui_window_publish_edits(a->win, NULL, &e, 1) == 0;
    if (done)
        v->placed = place;
    return done;
}

// What the facts say to do with the plane (VIDEO_PLANE.md § 4.4): place it
// once it is ready for the hole's size; hide it, painting the video itself,
// when it has gone stale; fall back to painting every picture when it has
// gone.
static void plane_step(app_t *a, view_t *v)
{
    os64_media_player_facts_t f;
    os64_media_player_facts(a->player, &f);
    if (f.plane_state != OS64_MEDIA_PLANE_ON) {
        // Refused at a new size, or ended: every picture is main's again.
        v->plane = false;
        v->placed = false;
        v->dirty = true;
        return;
    }
    if (v->placed && f.plane_stale) {
        (void)plane_publish(a, v, false);
        return;
    }
    if (!v->placed && f.plane_ready && f.plane_pic_w == v->hole.w && f.plane_pic_h == v->hole.h) {
        os64_media_player_plane_want(a->player, true);
        (void)plane_publish(a, v, true);
    }
}

static void set_title(app_t *a, const os64_media_player_facts_t *f, const char *name)
{
    char title[160];
    bool refused = f->refused;
    const char *sound = !f->has_sound ? "" : f->muted ? " (muted)" : refused ? " (sound not playing: " : "";
    os64_snprintf(title, sizeof(title), "%s - %s%s%s%s%s", name, kStateName[f->state], sound,
                  refused && !f->muted && f->has_sound && f->refused_why ? f->refused_why : "",
                  refused && !f->muted && f->has_sound ? ")" : "",
                  f->info.rotation != 0 ? " (rotated, shown as stored)" : "");
    os64_gui_window_set_title(a->win, title, OS64_GUI_WINDOW_TITLE_UTF8);
}

// The picture's size inside the window's content: the letterbox, to the
// player.
static void layout(app_t *a, view_t *v, const os64_media_info_t *info, uint32_t content_w, uint32_t content_h)
{
    gplay_rect_t r = gplay_letterbox(info->width, info->height, content_w, content_h);
    v->hole = (gplay_rect_t){r.x, r.y, r.w > 0 ? r.w : 1, r.h > 0 ? r.h : 1};
    os64_media_player_size(a->player, v->hole.w, v->hole.h);
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

// One key's ask, as a player call.
static void do_key(app_t *a, view_t *v, gplay_key_t key, bool to_start, int state)
{
    switch (key) {
    case GPLAY_KEY_QUIT:
        a->quit = true;
        break;
    case GPLAY_KEY_READOUT:
        v->readout = !v->readout;
        v->dirty = true;
        if (v->plane)
            (void)plane_publish(a, v, v->placed);
        break;
    case GPLAY_KEY_LOUDER:
    case GPLAY_KEY_SOFTER:
        volume_step(key == GPLAY_KEY_LOUDER ? 5 : -5);
        break;
    case GPLAY_KEY_PAUSE:
        if (state == OS64_MEDIA_PLAYER_PLAYING || state == OS64_MEDIA_PLAYER_BUFFERING)
            os64_media_player_pause(a->player);
        else
            os64_media_player_play(a->player);
        break;
    case GPLAY_KEY_BACK:
    case GPLAY_KEY_FORWARD:
    case GPLAY_KEY_BACK_FAR:
    case GPLAY_KEY_FORWARD_FAR:
        if (to_start)
            (void)os64_media_player_seek(a->player, 0);
        else
            (void)os64_media_player_seek_by(a->player, key == GPLAY_KEY_BACK      ? -SEEK_STEP
                                                       : key == GPLAY_KEY_FORWARD ? SEEK_STEP
                                                       : key == GPLAY_KEY_BACK_FAR ? -SEEK_FAR
                                                                                   : SEEK_FAR);
        break;
    case GPLAY_KEY_MUTE:
        a->muted = !a->muted;
        os64_media_player_mute(a->player, a->muted);
        break;
    case GPLAY_KEY_SOUND_LATER:
    case GPLAY_KEY_SOUND_EARLIER:
        // An offset below 0 reports the ear behind where it is, so the clock
        // and the pictures come later: the sound then lands EARLIER against
        // them. A beep heard after its flash wants `[`; the dial's value is
        // then how much the sink's reported delay falls short of the truth.
        a->sync += key == GPLAY_KEY_SOUND_LATER ? SYNC_STEP : -SYNC_STEP;
        os64_media_player_sync(a->player, a->sync);
        break;
    default:
        break;
    }
    v->dirty = true;
}

static void run(app_t *a, view_t *v, const os64_media_info_t *info, const char *name)
{
    int last_state = -1;
    bool last_refused = false, last_muted = false;
    for (;;) {
        os64_media_player_facts_t f;
        os64_media_player_facts(a->player, &f);
        os64_lock_acquire(&a->lock);
        a->playing = f.state == OS64_MEDIA_PLAYER_PLAYING || f.state == OS64_MEDIA_PLAYER_BUFFERING;
        os64_lock_release(&a->lock);
        bool quit = a->quit || (s_exit_at_end && (f.state == OS64_MEDIA_PLAYER_ENDED ||
                                                  f.state == OS64_MEDIA_PLAYER_FAILED));
        if (f.state != last_state || f.refused != last_refused || f.muted != last_muted) {
            set_title(a, &f, name);
            last_state = f.state;
            last_refused = f.refused;
            last_muted = f.muted;
        }
        if (quit)
            break;
        // On a plane only the readout asks for frames (to refresh it).
        bool want_frame = v->plane ? v->readout
                                   : f.state == OS64_MEDIA_PLAYER_PLAYING || v->poked || v->dirty;
        if (want_frame && !v->frame_asked) {
            if (os64_gui_frame_request(a->win, 0) == 0)
                v->frame_asked = true;
        }
        os64_gui_event_t ev;
        if (os64_gui_event_wait(a->win, &ev) != 1)
            break;
        switch (ev.type) {
        case OS64_GUI_EVENT_FRAME: {
            v->frame_asked = false;
            v->poked = false;
            if (v->plane) {
                if (v->readout && ++v->refresh >= 15) {
                    v->refresh = 0;
                    (void)plane_publish(a, v, v->placed);
                }
                break;
            }
            uint64_t number = os64_gui_frame_number(&ev);
            int64_t period = (int64_t)ev.frame.period_ns / 1000;
            // A publish reaches the glass one refresh after the answer's blank.
            int64_t blank = (int64_t)os64_gui_frame_blank_us(&ev) + period;
            if (a->last_frame_number != 0 && number > a->last_frame_number + 1 &&
                f.state == OS64_MEDIA_PLAYER_PLAYING)
                a->display_missed += number - a->last_frame_number - 1;
            a->last_frame_number = f.state == OS64_MEDIA_PLAYER_PLAYING ? number : 0;
            os64_media_player_frame_t fr;
            (void)os64_media_player_frame(a->player, blank, period, &fr);
            if (fr.changed)
                v->shown = fr;
            if (fr.changed || v->dirty) {
                draw_canvas(a, v);
                v->dirty = false;
            }
            break;
        }
        case OS64_GUI_EVENT_DOORBELL:
            if (ev.doorbell.mask & RING_LOOK) {
                v->poked = true;
                if (v->plane)
                    plane_step(a, v);
            }
            if (ev.doorbell.mask & RING_ACTION) {
                action_t act[ACTIONS];
                os64_lock_acquire(&a->lock);
                uint32_t n = a->actions;
                for (uint32_t i = 0; i < n; i++)
                    act[i] = a->action[i];
                a->actions = 0;
                os64_lock_release(&a->lock);
                for (uint32_t i = 0; i < n; i++) {
                    if (act[i].key == GPLAY_KEY_NONE && act[i].w > 0 && act[i].h > 0)
                        os64_gui_window_set_content_size(a->win, act[i].w, act[i].h);
                    else if (act[i].key != GPLAY_KEY_NONE) {
                        os64_media_player_facts(a->player, &f);
                        do_key(a, v, act[i].key, act[i].key == GPLAY_KEY_BACK_FAR && act[i].w == 1, f.state);
                    }
                }
            }
            break;
        case OS64_GUI_EVENT_KEY_DOWN:
            do_key(a, v,
                   gplay_key(&v->key_seq, ev.key.ascii, ev.key.scancode,
                             (ev.key.modifiers & OS64_GUI_MOD_SHIFT) != 0),
                   false, f.state);
            break;
        case OS64_GUI_EVENT_WINDOW_RESIZE:
            os64_draw_ctx_refresh(&v->ctx);
            if (v->plane) {
                // The video painted by main in the new hole, the plane hidden,
                // until it is ready at the new size (plane_step places it).
                layout(a, v, info, v->ctx.surf.width, v->ctx.surf.height);
                (void)plane_publish(a, v, false);
                break;
            }
            layout(a, v, info, v->ctx.surf.width, v->ctx.surf.height);
            v->dirty = true;
            break;
        case OS64_GUI_EVENT_WINDOW_CLOSE:
            a->quit = true;
            break;
        default:
            break;
        }
    }
}

// ---------------------------------------------------------------- start and end

static void say_counts(app_t *a, const os64_media_player_facts_t *f, const char *name)
{
    char at[16];
    gplay_time_text(f->position, at, sizeof(at));
    os64_printf("gplay: %s: %s at %s\n", name, kStateName[f->state], at);
    os64_printf("  pictures: decoded %lu, converted %lu (again %lu), shown %lu, dropped %lu, skipped %lu, "
                "starved %lu\n",
                (unsigned long)f->pictures_decoded, (unsigned long)f->conversions, (unsigned long)f->reconversions,
                (unsigned long)f->pace.shown, (unsigned long)f->pace.dropped, (unsigned long)f->pace.skipped,
                (unsigned long)f->pace.starved);
    os64_printf("  late: resyncs %lu (%lu ms holding), into decoding late %lu, far by sound %lu, display missed "
                "%lu\n",
                (unsigned long)f->pace.resyncs, (unsigned long)(f->pace.far_behind_us / 1000),
                (unsigned long)f->pace.entered[OS64_MEDIA_PACE_DECODING_LATE], (unsigned long)f->pace.far_by_sound,
                (unsigned long)(f->plane_state == OS64_MEDIA_PLANE_ON || f->plane_published > 0 ? f->frames_missed
                                                                                               : a->display_missed));
    if (f->plane_published > 0 || f->plane_state != OS64_MEDIA_PLANE_NONE)
        os64_printf("  plane: %s %u (%ux%u), published %lu, recreates %lu, waits ended by patience %lu, "
                    "frame calls refused %lu, closes ended placed %lu, refusal %d\n",
                    f->plane_state == OS64_MEDIA_PLANE_ON ? "on" : f->plane_state == OS64_MEDIA_PLANE_REFUSED ? "refused"
                    : f->plane_state == OS64_MEDIA_PLANE_ENDED ? "ended" : "starting",
                    f->plane_name, f->plane_cap_w, f->plane_cap_h, (unsigned long)f->plane_published,
                    (unsigned long)f->plane_recreates, (unsigned long)f->waits_patience,
                    (unsigned long)f->frame_refused, (unsigned long)f->plane_ended_placed, f->plane_refusal);
    os64_printf("  sound: decoded %lu frames, written %lu, let go unheard %lu; buffering %lu, stalls %lu, "
                "refusals %lu\n",
                (unsigned long)f->sound_decoded, (unsigned long)f->sound_written, (unsigned long)f->sound_dropped,
                (unsigned long)f->buffers, (unsigned long)f->stalls, (unsigned long)f->refusals);
    os64_printf("  clock: reports %lu, stale %lu, starts late %lu, starves %lu; sink delay %u ms, sync %ld ms\n",
                (unsigned long)f->clock.reports, (unsigned long)f->clock.reports_stale,
                (unsigned long)f->clock.starts_late, (unsigned long)f->clock.starves, f->delay_us / 1000,
                (long)(f->sync * 1000 / RATE));
    const uint64_t *age = f->clock.report_ages;
    os64_printf("  ear: first heard after %ld ms; stills %lu, held ahead %lu, stepped behind %lu (trust %ld ms)\n",
                (long)(f->clock.start_wait_us / 1000), (unsigned long)f->clock.stills,
                (unsigned long)f->clock.held_ahead, (unsigned long)f->clock.jumped_behind,
                (long)(f->clock.still_us / 1000));
    os64_printf("  report ages: to 20 ms %lu, 40 %lu, 60 %lu, 80 %lu, 120 %lu, over %lu\n", (unsigned long)age[0],
                (unsigned long)age[1], (unsigned long)age[2], (unsigned long)age[3], (unsigned long)age[4],
                (unsigned long)age[5]);
    os64_printf("  media: picture packets %lu, run-in %lu, resync-dropped %lu\n",
                (unsigned long)f->media.picture_packets, (unsigned long)f->media.pictures_run_in,
                (unsigned long)f->media.pictures_resync);
}

// --check: the counts a run that played to its end must have.
static bool counts_hold(const os64_media_player_facts_t *f, const char *name)
{
    bool ok = true;
    if (f->state != OS64_MEDIA_PLAYER_ENDED) {
        os64_printf("gplay: check: %s did not end (%s)\n", name, kStateName[f->state]);
        ok = false;
    }
    if (s_cover) {
        // Covered, nobody asks for a frame and none is shown: the player
        // retires every picture by the clock, so each decoded one was shown,
        // dropped or skipped, and the end came all the same.
        uint64_t gone = f->pace.shown + f->pace.dropped + f->pace.skipped;
        if (gone != f->pictures_decoded) {
            os64_printf("gplay: check: covered, %lu pictures decoded and %lu shown, dropped or skipped\n",
                        (unsigned long)f->pictures_decoded, (unsigned long)gone);
            ok = false;
        }
    } else if (f->pace.shown == 0 || f->shown_at < 0) {
        os64_printf("gplay: check: no picture was shown\n");
        ok = false;
    } else if (f->length > 0 && f->pace.period > 0 && f->pace.resyncs == 0 &&
               f->shown_end < f->length - 3 * f->pace.period) {
        // The pictures reached the end: the last one shown ends within three
        // periods of the media's length (a clip whose picture runs as long
        // as its sound, as the shipped ones do). After a resync the late
        // policy may have let the tail go (the decoder skips to a keyframe
        // the tail does not have), so only the end itself is asked for.
        os64_printf("gplay: check: the last picture shown ends at frame %ld of %ld\n", (long)f->shown_end,
                    (long)f->length);
        ok = false;
    }
    // Every frame of sound decoded was written or let go for a reason that
    // counted it (a seek, a mute, a refusal, a failed write, a tail).
    if (f->has_sound && f->sound_written + f->sound_dropped != f->sound_decoded) {
        os64_printf("gplay: check: sound decoded %lu, written %lu, let go %lu\n", (unsigned long)f->sound_decoded,
                    (unsigned long)f->sound_written, (unsigned long)f->sound_dropped);
        ok = false;
    }
    if (s_expect_refusal && (f->refusals == 0 || f->refused || f->sound_written == 0)) {
        os64_printf("gplay: check: wanted the device refused and then regained (refusals %lu, refused now %d, "
                    "written %lu)\n",
                    (unsigned long)f->refusals, f->refused, (unsigned long)f->sound_written);
        ok = false;
    }
    return ok;
}

// Closes the player and waits for its `finished` (bounded: an output that
// never lets go keeps the player, and the program leaves without it).
static bool close_player(app_t *a)
{
    os64_media_player_close(a->player);
    for (int waited = 0; waited < 300; waited++) {
        os64_lock_acquire(&a->lock);
        bool done = a->finished;
        os64_lock_release(&a->lock);
        if (done)
            return true;
        (void)os64_bell_wait(&a->bell, 10);
    }
    return false;
}

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
    app_t *a = os64_calloc(1, sizeof(app_t));
    view_t v = {0};
    a->bell = (os64_bell_t)OS64_BELL_NONE;
    os64_bell_make(&a->bell);
    if (os64_media_output_create(&a->output) != 0) {
        os64_printf("gplay: the sound device's thread would not start\n");
        return 3;
    }
    os64_media_player_config_t cfg = {.want = OS64_MEDIA_WANT_SOUND | OS64_MEDIA_WANT_PICTURE,
                                      .output = a->output, .refused = OS64_MEDIA_VOICE_PLAY_ON, .loaded = true,
                                      .look = on_look, .finished = on_finished, .ctx = a};
    os64_media_file_source((int32_t)file, &cfg.source);
    if (os64_media_player_create(&cfg, &a->player) != 0) {
        os64_printf("gplay: %s: the player would not start\n", path);
        return 3;
    }
    os64_media_player_play(a->player);

    // The probe is the player's: wait for it, then the window takes the
    // picture's shape.
    os64_media_player_facts_t f;
    for (int waited = 0;; waited += 50) {
        os64_media_player_facts(a->player, &f);
        if (f.probed || waited >= OPEN_WAIT_MS)
            break;
        (void)os64_bell_wait(&a->bell, 50);
    }
    int refused_open = !f.probed ? OS64_MEDIA_IO : f.failure != 0 ? f.failure
                       : f.info.picture != OS64_MEDIA_PLAYABLE   ? OS64_MEDIA_NO_PICTURE
                                                                 : 0;
    if (refused_open != 0) {
        if (refused_open == OS64_MEDIA_NO_PICTURE)
            os64_printf("gplay: %s has no picture; `play %s` plays its sound\n", path, path);
        else
            os64_printf("gplay: %s: %s\n", path, os64_media_strerror(refused_open));
        if (close_player(a)) {
            if (a->output != 0)
                (void)os64_media_output_destroy(a->output);
            os64_close((int32_t)file);
        }
        return 2;
    }
    os64_media_info_t info = f.info;

    uint32_t sw = 0, sh = 0;
    os64_gui_screen_info(&sw, &sh);
    uint32_t w, h;
    gplay_first_size(info.width, info.height, sw, sh, MARGIN, &w, &h);
    int32_t x = sw > w ? (int32_t)(sw - w) / 2 : 0, y = sh > h ? (int32_t)(sh - h) / 2 : 0;
    int64_t win = os64_gui_window_create_content(name, x, y, w, h,
                                                 OS64_GUI_CREATE_FIT_SCREEN |
                                                     (s_maximized ? OS64_GUI_CREATE_MAXIMIZED : 0));
    if (win <= 0 || os64_draw_ctx_init(&v.ctx, win) != 0) {
        os64_printf(win == OS64_GUI_ERR_NOT_RUNNING ? "gplay: no desktop on this boot\n"
                                                    : "gplay: the window would not open\n");
        if (close_player(a)) {
            if (a->output != 0)
                (void)os64_media_output_destroy(a->output);
            os64_close((int32_t)file);
        }
        return 1;
    }
    a->win = win;
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
    layout(a, &v, &info, v.ctx.surf.width, v.ctx.surf.height);
    // A plane first; refused, the window paints every picture.
    v.plane = os64_media_player_plane(a->player, win) == 0;
    if (v.plane) {
        (void)plane_publish(a, &v, false);   // black, the video painted from `shown` once there is one
        plane_step(a, &v);
    } else {
        draw_canvas(a, &v);
    }

    int64_t keys = os64_thread(keys_thread, a);
    run(a, &v, &info, name);

    // At the end the sound still in the device plays out (up to a second),
    // as a close never cuts it; a person who left early is not made to wait.
    for (int waited = 0; waited < TAIL_WAIT_MS; waited += 10) {
        os64_media_player_facts(a->player, &f);
        if (f.state != OS64_MEDIA_PLAYER_ENDED || !f.sounding)
            break;
        os64_sleep(10);
    }
    os64_media_player_facts(a->player, &f);
    os64_lock_acquire(&a->lock);
    a->quit = true;
    os64_lock_release(&a->lock);
    if (keys > 0)
        os64_thread_join((int32_t)keys, 0);
    if (f.failure != 0)
        os64_printf("gplay: %s: %s\n", path, os64_media_strerror(f.failure));
    say_counts(a, &f, name);
    bool checked = !s_check || counts_hold(&f, name);
    int code = f.failure != 0 || !checked ? 3 : 0;

    // A plane leaves the window first (the show thread then destroys it and
    // ends at once, not at the closing bound).
    if (v.plane && v.placed)
        (void)plane_publish(a, &v, false);
    bool closed = close_player(a);
    if (cover > 0)
        os64_gui_window_destroy(cover);
    os64_gui_window_destroy(win);
    if (closed) {
        if (a->output != 0 && os64_media_output_destroy(a->output) != 0)
            os64_printf("gplay: the sound device's thread did not let go\n");
        os64_close((int32_t)file);
        os64_bell_close(&a->bell);
        os64_free(a);
    } else {
        os64_printf("gplay: the player did not let go\n");
    }
    return code;
}
