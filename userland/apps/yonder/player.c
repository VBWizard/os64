// player.c — an audio element's controls (player.h; AUDIO_YONDER.md § The
// controls). Every size is a share of the box's height, so the controls
// zoom with the page that laid the box out.

#include "player.h"

#include <math.h>

#include "os64/draw.h"
#include "os64/fmt.h"
#include "os64/mem.h"
#include "os64/str.h"

// How far an arrow key moves: seconds along the bar, a tenth of the volume.
#define SEEK_STEP 5.0
#define VOLUME_STEP 0.1

// ── The arithmetic ──────────────────────────────────────────────────────

void yonder_player_parts(os64_gui_rect_t b, yonder_player_parts_t *out)
{
    os64_memset(out, 0, sizeof(*out));
    int32_t h = b.h > 0 ? b.h : 0;
    int32_t button = h * 32 / 54, pad = (h - button) / 2;
    if (pad < 1)
        pad = 1;
    int32_t y = b.y + pad;
    int64_t x = b.x + pad, end = (int64_t)b.x + b.w - pad;
    out->play = (os64_gui_rect_t){(int32_t)x, y, button, button};
    x += button + pad;
    if (end - x >= button) {
        out->mute = (os64_gui_rect_t){(int32_t)(end - button), y, button, button};
        end -= button + pad;
    }
    // The time when the bar keeps a button's width beside it; the volume
    // when the bar keeps two.
    int32_t time_w = h * 2, volume_w = h * 3 / 2;
    if (end - x - time_w >= button) {
        out->time = (os64_gui_rect_t){(int32_t)x, b.y, time_w, h};
        x += time_w;
    }
    if (out->mute.w > 0 && end - x - volume_w - pad >= 2 * button) {
        out->volume = (os64_gui_rect_t){(int32_t)(end - volume_w), b.y, volume_w, h};
        end -= volume_w + pad;
    }
    if (end > x)
        out->seek = (os64_gui_rect_t){(int32_t)x, b.y, (int32_t)(end - x), h};
}

// A clock past this many seconds (some 31,700 years) is a damaged header's,
// and held there: past 2^64 the conversion would be undefined.
#define CLOCK_MAX 1e12

static size_t clock_text(double seconds, bool hours, char *out, size_t cap)
{
    uint64_t s = seconds > 0 && isfinite(seconds) ? (uint64_t)(seconds < CLOCK_MAX ? seconds : CLOCK_MAX) : 0;
    if (hours)
        return (size_t)os64_snprintf(out, cap, "%lu:%02lu:%02lu", (unsigned long)(s / 3600),
                                     (unsigned long)(s / 60 % 60), (unsigned long)(s % 60));
    return (size_t)os64_snprintf(out, cap, "%lu:%02lu", (unsigned long)(s / 60), (unsigned long)(s % 60));
}

size_t yonder_player_time(double position, double duration, char *out, size_t cap)
{
    if (cap == 0)
        return 0;
    bool known = isfinite(duration) && duration >= 0;
    bool hours = (known ? duration : position) >= 3600;
    char now[24], all[24];
    clock_text(position, hours, now, sizeof(now));
    if (known)
        clock_text(duration, hours, all, sizeof(all));
    else
        os64_strcopy(all, sizeof(all), "--:--");
    // A stream's length is no end at all: only where it is.
    if (isinf(duration))
        return (size_t)os64_snprintf(out, cap, "%s", now);
    return (size_t)os64_snprintf(out, cap, "%s / %s", now, all);
}

// The bar's track: its middle third of a line, inset by the thumb's half
// so the thumb's centre reaches each end.
static os64_gui_rect_t track_of(os64_gui_rect_t bar)
{
    int32_t thumb = bar.h * 12 / 54;
    if (thumb < 4)
        thumb = 4;
    int32_t th = bar.h * 4 / 54;
    if (th < 2)
        th = 2;
    return (os64_gui_rect_t){bar.x + thumb / 2, bar.y + (bar.h - th) / 2, bar.w - thumb, th};
}

double yonder_player_value_at(os64_gui_rect_t bar, int32_t x, double length)
{
    os64_gui_rect_t t = track_of(bar);
    if (t.w <= 0 || !(length > 0) || !isfinite(length))
        return 0;
    double f = ((double)x - t.x) / t.w;
    f = f < 0 ? 0 : f > 1 ? 1 : f;
    return f * length;
}

// ── Painting ────────────────────────────────────────────────────────────

static bool in(os64_gui_rect_t r, int32_t x, int32_t y)
{
    return r.w > 0 && r.h > 0 && x >= r.x && y >= r.y && (int64_t)x < (int64_t)r.x + r.w &&
           (int64_t)y < (int64_t)r.y + r.h;
}

// A triangle pointing right, filling `r`'s middle.
static void play_icon(os64_gui_surface_t *s, os64_gui_rect_t r, uint32_t ink)
{
    int32_t h = r.h / 2, w = h * 7 / 8, x = r.x + (r.w - w) / 2 + r.w / 16, y = r.y + (r.h - h) / 2;
    for (int32_t row = 0; row < h; row++) {
        int32_t from_edge = row < h - 1 - row ? row : h - 1 - row;
        int32_t len = h > 1 ? (int32_t)((int64_t)from_edge * 2 * w / (h - 1)) : w;
        if (len > 0)
            os64_draw_hline(s, x, y + row, len, ink);
    }
}

static void pause_icon(os64_gui_surface_t *s, os64_gui_rect_t r, uint32_t ink)
{
    int32_t h = r.h / 2, bar = h / 3 > 1 ? h / 3 : 1, gap = bar;
    int32_t x = r.x + (r.w - 2 * bar - gap) / 2, y = r.y + (r.h - h) / 2;
    os64_draw_fill_rect(s, (os64_gui_rect_t){x, y, bar, h}, ink);
    os64_draw_fill_rect(s, (os64_gui_rect_t){x + bar + gap, y, bar, h}, ink);
}

// A speaker: its box and cone, then sound beside it, or a cross for muted.
static void speaker_icon(os64_gui_surface_t *s, os64_gui_rect_t r, bool muted, double volume, uint32_t ink)
{
    int32_t h = r.h / 2, unit = h / 8 > 0 ? h / 8 : 1;
    int32_t x = r.x + (r.w - h) / 2, y = r.y + (r.h - h) / 2;
    os64_draw_fill_rect(s, (os64_gui_rect_t){x, y + 3 * unit, 2 * unit, 2 * unit}, ink);
    for (int32_t i = 0; i < 3 * unit; i++)
        os64_draw_vline(s, x + 2 * unit + i, y + 3 * unit - i, 2 * unit + 2 * i, ink);
    int32_t sx = x + 6 * unit;
    if (muted) {
        for (int32_t i = 0; i < 3 * unit; i++) {
            os64_draw_fill_rect(s, (os64_gui_rect_t){sx + i, y + 2 * unit + i, unit, unit}, ink);
            os64_draw_fill_rect(s, (os64_gui_rect_t){sx + i, y + 5 * unit - i - 1, unit, unit}, ink);
        }
        return;
    }
    int32_t waves = volume <= 0 ? 0 : volume < 0.5 ? 1 : 2;
    for (int32_t k = 0; k < waves; k++) {
        int32_t wx = sx + k * 2 * unit, half = (k + 1) * unit;
        os64_draw_vline(s, wx, y + 4 * unit - half, 2 * half, ink);
    }
}

// A bar: the track, what has arrived of it (from `from` to `have`), what
// has played, and the thumb.
static void bar_paint(os64_gui_surface_t *s, os64_gui_rect_t bar, double from, double have, double at,
                      double length, bool live, const os64_ui_theme_t *t)
{
    os64_gui_rect_t track = track_of(bar);
    if (track.w <= 0)
        return;
    os64_draw_fill_rect(s, track, t->scroll_track);
    if (!(length > 0) || !isfinite(length))
        return;
    int32_t ahead = (int32_t)(have / length * track.w + 0.5), done = (int32_t)(at / length * track.w + 0.5);
    int32_t start = (int32_t)(from / length * track.w + 0.5);
    ahead = ahead < 0 ? 0 : ahead > track.w ? track.w : ahead;
    done = done < 0 ? 0 : done > track.w ? track.w : done;
    start = start < done ? done : start > track.w ? track.w : start;
    if (ahead > start)
        os64_draw_fill_rect(s, (os64_gui_rect_t){track.x + start, track.y, ahead - start, track.h}, t->scroll_thumb);
    if (done > 0)
        os64_draw_fill_rect(s, (os64_gui_rect_t){track.x, track.y, done, track.h},
                            live ? t->focus_ring : t->disabled_fg);
    int32_t thumb = bar.h * 12 / 54 < 4 ? 4 : bar.h * 12 / 54;
    os64_gui_rect_t knob = {track.x + done - thumb / 2, bar.y + (bar.h - thumb) / 2, thumb, thumb};
    os64_draw_fill_round_rect(s, knob, thumb / 2, live ? t->focus_ring : t->disabled_fg);
}

// The text a part shows, middle-aligned in it, from a retained run.
static void text_paint(os64_ui_widget_t *w, void **slot, os64_gui_surface_t *s, os64_gui_rect_t r,
                       const char *text, uint32_t ink, uint32_t paper)
{
    if (r.w <= 0)
        return;
    size_t len = os64_strlen(text);
    void *run = NULL;
    if (os64_ui_run_resolve(w->ui, slot, OS64_UI_FONT_APP, text, len, &run) != OS64_FONT_OK)
        return;
    int32_t row = w->ui != NULL ? os64_ui_font_row_height(w->ui, OS64_UI_FONT_APP) : 16;
    os64_ui_draw_run(w->ui, run, OS64_UI_FONT_APP, s, r, r.x, r.y + (r.h - row) / 2, text, len, ink, paper);
}

static void player_paint(os64_ui_widget_t *w, os64_draw_ctx_t *ctx, const os64_ui_theme_t *t)
{
    yonder_player_t *p = (yonder_player_t *)w;
    if (w->bounds.w <= 0 || w->bounds.h <= 0)
        return;
    os64_gui_surface_t *s = &ctx->surf;
    const os64_dom_media_view_t *v = &p->view;
    int32_t radius = w->bounds.h / 2;
    os64_draw_fill_round_rect(s, w->bounds, radius, t->panel_bg);
    os64_draw_round_rect(s, w->bounds, radius, w->focused ? t->focus_ring : t->panel_border,
                         w->focused ? t->focus_ring : t->panel_border);
    yonder_player_parts_t parts;
    yonder_player_parts(w->bounds, &parts);
    bool broken = v->error != 0;
    uint32_t ink = broken ? t->disabled_fg : t->label_fg;
    if (v->paused || v->ended)
        play_icon(s, parts.play, ink);
    else
        pause_icon(s, parts.play, ink);
    if (parts.mute.w > 0)
        speaker_icon(s, parts.mute, v->muted, v->volume, ink);
    // Busy and broken are said where the bar and the time are: there is
    // nothing to move along until the sound can be had.
    const char *note = broken ? "Cannot play this sound" : v->busy ? "Sound is busy" : NULL;
    if (note != NULL) {
        int32_t from = parts.time.w > 0 ? parts.time.x : parts.seek.x;
        int64_t to = (int64_t)parts.seek.x + parts.seek.w;
        os64_gui_rect_t room = {from, w->bounds.y, (int32_t)(to - from), w->bounds.h};
        text_paint(w, &p->note_run, s, room, note, t->label_fg, t->panel_bg);
    } else {
        double at = p->drag == YONDER_PLAYER_DRAG_SEEK ? p->drag_value : v->position;
        char clock[48];
        yonder_player_time(at, v->duration, clock, sizeof(clock));
        text_paint(w, &p->time_run, s, parts.time, clock, t->label_fg, t->panel_bg);
        bar_paint(s, parts.seek, v->arrived_from, v->arrived, at, v->duration, v->seekable, t);
    }
    if (parts.volume.w > 0) {
        double volume = p->drag == YONDER_PLAYER_DRAG_VOLUME ? p->drag_value : v->muted ? 0 : v->volume;
        bar_paint(s, parts.volume, 0, 1, volume, 1, !broken, t);
    }
}

// ── Interaction ─────────────────────────────────────────────────────────

static void ask(yonder_player_t *p, os64_dom_media_control_t what, double value)
{
    if (p->act != NULL)
        p->act(p, what, value, p->user);
}

// Each ask shows its change at once (yonder_player_t.view), so a second
// press before the element's next view asks from where the first left it;
// and repaints it, since the element's view, once it agrees, will not.
static void toggle(os64_ui_t *ui, yonder_player_t *p)
{
    bool play = p->view.paused || p->view.ended;
    ask(p, play ? OS64_DOM_MEDIA_PLAY : OS64_DOM_MEDIA_PAUSE, 0);
    p->view.paused = !play;
    if (play)
        p->view.ended = false;
    os64_ui_mark_dirty(ui, &p->w);
}

static void mute_flip(os64_ui_t *ui, yonder_player_t *p)
{
    bool on = !p->view.muted;
    ask(p, OS64_DOM_MEDIA_MUTE, on ? 1 : 0);
    p->view.muted = on;
    os64_ui_mark_dirty(ui, &p->w);
}

static void seek_to(os64_ui_t *ui, yonder_player_t *p, double t)
{
    ask(p, OS64_DOM_MEDIA_SEEK, t);
    p->view.position = t;
    os64_ui_mark_dirty(ui, &p->w);
}

static void volume_to(os64_ui_t *ui, yonder_player_t *p, double v)
{
    v = v < 0 ? 0 : v > 1 ? 1 : v;
    ask(p, OS64_DOM_MEDIA_VOLUME, v);
    p->view.volume = v;
    // A volume chosen is one to hear: other browsers unmute for it.
    if (p->view.muted && v > 0) {
        ask(p, OS64_DOM_MEDIA_MUTE, 0);
        p->view.muted = false;
    }
    os64_ui_mark_dirty(ui, &p->w);
}

static void drag_to(os64_ui_t *ui, yonder_player_t *p, int32_t x)
{
    yonder_player_parts_t parts;
    yonder_player_parts(p->w.bounds, &parts);
    if (p->drag == YONDER_PLAYER_DRAG_SEEK) {
        double t = yonder_player_value_at(parts.seek, x, p->view.duration);
        if (t != p->drag_value) {
            p->drag_value = t;
            seek_to(ui, p, t);
        }
    } else if (p->drag == YONDER_PLAYER_DRAG_VOLUME) {
        double v = yonder_player_value_at(parts.volume, x, 1);
        if (v != p->drag_value) {
            p->drag_value = v;
            volume_to(ui, p, v);
        }
    }
    os64_ui_mark_dirty(ui, &p->w);
}

static void player_cancel(os64_ui_widget_t *w)
{
    yonder_player_t *p = (yonder_player_t *)w;
    p->drag = YONDER_PLAYER_DRAG_NONE;
    p->seq = 0;
}

// The keys. An arrow arrives as ESC [ and its letter, one event each, and
// a key such as Page Up as ESC [ 5 ~: the whole of either is the player's,
// and a real Escape (os64_ui_key_is_esc) is not.
#define SEQ_ESC 1
#define SEQ_CSI 2
#define SEQ_PARAMS 3

static bool player_key(yonder_player_t *p, os64_ui_t *ui, const os64_gui_event_t *ev)
{
    if (ev->key.modifiers & (OS64_GUI_MOD_CTRL | OS64_GUI_MOD_ALT)) {
        p->seq = 0;
        return false;
    }
    char a = ev->key.ascii;
    if (p->seq == SEQ_ESC) {
        p->seq = 0;
        if (a == '[') {
            p->seq = SEQ_CSI;
            return true;
        }
        // Not a burst after all: this key is decoded as itself.
    } else if (p->seq == SEQ_CSI) {
        p->seq = 0;
        double now = p->view.position, v = p->view.volume;
        switch (a) {
        case 'D':
            if (p->view.seekable)
                seek_to(ui, p, now > SEEK_STEP ? now - SEEK_STEP : 0);
            return true;
        case 'C':
            if (p->view.seekable)
                seek_to(ui, p, now + SEEK_STEP < p->view.duration ? now + SEEK_STEP : p->view.duration);
            return true;
        case 'A': volume_to(ui, p, v + VOLUME_STEP); return true;
        case 'B': volume_to(ui, p, v - VOLUME_STEP); return true;
        default:
            if ((a >= '0' && a <= '9') || a == ';')
                p->seq = SEQ_PARAMS;
            return true;
        }
    } else if (p->seq == SEQ_PARAMS) {
        if (!((a >= '0' && a <= '9') || a == ';'))
            p->seq = 0;          // its final byte (`~`), the key's whole
        return true;
    }
    if (a == 0x1b) {
        if (os64_ui_key_is_esc(ev))
            return false;
        p->seq = SEQ_ESC;
        return true;
    }
    if (a == ' ' || a == '\r' || a == '\n') {
        toggle(ui, p);
        return true;
    }
    if (a == 'm' || a == 'M') {
        mute_flip(ui, p);
        return true;
    }
    return false;
}

static bool player_event(os64_ui_widget_t *w, os64_ui_t *ui, const os64_gui_event_t *ev)
{
    yonder_player_t *p = (yonder_player_t *)w;
    yonder_player_parts_t parts;
    yonder_player_parts(w->bounds, &parts);
    switch (ev->type) {
    case OS64_GUI_EVENT_MOUSE_BUTTON_DOWN: {
        if (ev->mouse.button != OS64_GUI_MOUSE_LEFT)
            return false;
        os64_ui_set_focus(ui, w);
        int32_t x = ev->mouse.x, y = ev->mouse.y;
        if (in(parts.play, x, y)) {
            toggle(ui, p);
        } else if (in(parts.mute, x, y)) {
            mute_flip(ui, p);
        } else if (in(parts.seek, x, y) && p->view.seekable && !p->view.busy && p->view.error == 0) {
            p->drag = YONDER_PLAYER_DRAG_SEEK;
            p->drag_value = -1;
            drag_to(ui, p, x);
        } else if (in(parts.volume, x, y) && p->view.error == 0) {
            p->drag = YONDER_PLAYER_DRAG_VOLUME;
            p->drag_value = -1;
            drag_to(ui, p, x);
        }
        os64_ui_mark_dirty(ui, w);
        return true;
    }
    case OS64_GUI_EVENT_MOUSE_MOVE:
    case OS64_GUI_EVENT_MOUSE_BUTTON_UP:
        if (p->drag == YONDER_PLAYER_DRAG_NONE)
            return ev->type == OS64_GUI_EVENT_MOUSE_BUTTON_UP;
        drag_to(ui, p, ev->mouse.x);
        if (ev->type == OS64_GUI_EVENT_MOUSE_BUTTON_UP)
            p->drag = YONDER_PLAYER_DRAG_NONE;
        return true;
    case OS64_GUI_EVENT_MOUSE_WHEEL: {
        // Over the speaker or the volume bar, the wheel is the volume: the
        // narrow player's way to it. Anywhere else it is the page's.
        int32_t notches = ev->mouse.dx ? ev->mouse.dx : -(int32_t)ev->mouse.dy;
        if (notches == 0 || !(in(parts.mute, ev->mouse.x, ev->mouse.y) ||
                              in(parts.volume, ev->mouse.x, ev->mouse.y)))
            return p->pass != NULL && p->pass(ev, p->user);
        volume_to(ui, p, p->view.volume + notches * VOLUME_STEP);
        return true;
    }
    case OS64_GUI_EVENT_KEY_DOWN:
        return player_key(p, ui, ev);
    default:
        return false;
    }
}

static void player_destroy(os64_ui_widget_t *w)
{
    yonder_player_release((yonder_player_t *)w);
}

const os64_ui_class_t yonder_player_class = {
    "player", player_paint, player_event, player_cancel, NULL, NULL, NULL, player_destroy, NULL, player_destroy,
    false,
};

void yonder_player(yonder_player_t *p, yonder_player_act_t act,
                   bool (*pass)(const os64_gui_event_t *ev, void *user), void *user)
{
    os64_memset(p, 0, sizeof(*p));
    p->w.cls = &yonder_player_class;
    p->w.focusable = true;
    p->act = act;
    p->pass = pass;
    p->user = user;
    p->view.paused = true;
    p->view.volume = 1;
    p->view.duration = NAN;
}

void yonder_player_release(yonder_player_t *p)
{
    os64_ui_run_release(p->time_run);
    os64_ui_run_release(p->note_run);
    p->time_run = p->note_run = NULL;
}

// What the drawing reads of a view, compared: a moving position matters
// only by the second the clock shows, or the pixel the bar moves.
static bool same_drawing(const yonder_player_t *p, const os64_dom_media_view_t *a, const os64_dom_media_view_t *b)
{
    if (a->paused != b->paused || a->ended != b->ended || a->busy != b->busy || a->seekable != b->seekable ||
        a->muted != b->muted || a->error != b->error || a->volume != b->volume)
        return false;
    bool a_nan = isnan(a->duration), b_nan = isnan(b->duration);
    if (a_nan != b_nan || (!a_nan && a->duration != b->duration))
        return false;
    if ((uint64_t)(a->position > 0 ? a->position : 0) != (uint64_t)(b->position > 0 ? b->position : 0))
        return false;
    yonder_player_parts_t parts;
    yonder_player_parts(p->w.bounds, &parts);
    double length = a_nan ? 0 : a->duration;
    if (!(length > 0) || !isfinite(length))
        return true;
    int32_t w = track_of(parts.seek).w;
    return (int32_t)(a->position / length * w) == (int32_t)(b->position / length * w) &&
           (int32_t)(a->arrived / length * w) == (int32_t)(b->arrived / length * w) &&
           (int32_t)(a->arrived_from / length * w) == (int32_t)(b->arrived_from / length * w);
}

bool yonder_player_show(os64_ui_t *ui, yonder_player_t *p, const os64_dom_media_view_t *view)
{
    bool same = same_drawing(p, &p->view, view);
    p->view = *view;
    if (same)
        return false;
    os64_ui_mark_dirty(ui, &p->w);
    return true;
}
