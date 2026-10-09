// settings.c — yonder's Settings window (settings.h).

#include <stddef.h>

#include "settings.h"
#include "agent.h"
#include "os64/conf.h"
#include "os64/fmt.h"
#include "os64/gui.h"
#include "os64/io.h"
#include "os64/mem.h"
#include "os64/proc.h"
#include "os64/str.h"
#include "os64/thread.h"
#include "os64/ui_settings.h"

#define CONF_NAME "yonder.conf"
#define CONF_AGENT "agent"
#define CONF_SCRIPTS "scripts"
#define CONF_CACHE "cache"
#define CONF_CACHE_DIR "cache_dir"
#define CONF_CACHE_MB "cache_mb"

#define CONF_ZOOM "zoom"
#define CONF_DIAGNOSTICS "diagnostics"
#define CONF_APPEARANCE "appearance"
#define CONF_SCRIPT_SECONDS "script_seconds"

// The list's height, in rows; the dialog's rows are a line of words, the
// list, field, cache row, zoom row, and the page-script switch with the
// script time limit beside it. Cache usage appears on the dialog's status
// line; a body must fit the helper's window, which is sized for this many
// rows and no more.
#define LIST_ROWS 6
#define BODY_ROWS (LIST_ROWS + 5)

// Whole-percent zoom limits; yonder's keyboard steps lie between them.
#define ZOOM_MIN 25
#define ZOOM_MAX 500

static struct {
    os64_ui_settings_t d;
    os64_ui_widget_t heading;
    os64_ui_listbox_t list;
    os64_ui_textfield_t field;
    char field_buf[YONDER_AGENT_MAX];
    void (*use)(const char *agent, bool scripts, uint32_t script_seconds);
    os64_ui_checkbox_t scripts;
    // "Script time limit: [----o----] 5 s", beside the switch: the slider,
    // and its value in words.
    os64_ui_widget_t limit_words, limit_value;
    os64_ui_slider_t limit;
    char limit_text[16];
    way_cache_t *cache;
    bool usable;                    // the cache has a directory it can keep things in
    os64_ui_checkbox_t keep;
    os64_ui_textfield_t size;       // megabytes the cache may hold
    char size_buf[8];
    os64_ui_widget_t mb, empty;
    os64_ui_widget_t zoom_words, percent;
    os64_ui_textfield_t zoom;       // the default zoom, in whole percent
    char zoom_buf[8];
    void (*zoom_use)(uint32_t thousandths);
    // Dark pages, at the zoom row's right end.
    os64_ui_checkbox_t dark;
    void (*dark_use)(bool dark);
    // The relay: `thread` waits for events at the dialog's window and
    // rings `bell` on `window`, then waits on `pumped` until the loop has
    // handled them — one ring outstanding at a time, so it never spins on
    // events the loop has not taken yet.
    int64_t window, thread;
    uint32_t bell;
    bool piped;
    int32_t pumped[2];
} s;

// Every typed agent ever applied, newest first (yonder_agent_keep).
typedef struct Kept {
    struct Kept *next;
    char agent[];
} Kept;
static Kept *s_kept;

const char *yonder_agent_keep(const char *agent)
{
    for (size_t i = 0; i < yonder_agent_npresets(); i++)
        if (os64_streq(agent, yonder_agent_preset(i)->agent))
            return yonder_agent_preset(i)->agent;
    for (Kept *k = s_kept; k != NULL; k = k->next)
        if (os64_streq(agent, k->agent))
            return k->agent;
    size_t n = os64_strlen(agent) + 1;
    Kept *k = os64_malloc(sizeof(*k) + n);
    if (k == NULL)
        return NULL;
    os64_memcpy(k->agent, agent, n);
    k->next = s_kept;
    s_kept = k;
    return k->agent;
}

void yonder_agents_release(void)
{
    while (s_kept != NULL) {
        Kept *k = s_kept;
        s_kept = k->next;
        os64_free(k);
    }
}

bool yonder_settings_saved_dark(void)
{
    char value[8];
    return os64_conf_get(CONF_NAME, CONF_APPEARANCE, value, sizeof(value)) == 0 &&
           os64_streq_nocase(value, "dark");
}

bool yonder_settings_saved_scripts(void)
{
    char value[8];
    return os64_conf_get(CONF_NAME, CONF_SCRIPTS, value, sizeof(value)) == 0 &&
           os64_streq(value, "on");
}

uint32_t yonder_settings_saved_script_seconds(void)
{
    char v[16];
    uint64_t seconds;
    if (os64_conf_get(CONF_NAME, CONF_SCRIPT_SECONDS, v, sizeof(v)) != 0)
        return YONDER_SCRIPT_SECONDS_DEFAULT;
    if (!os64_parse_u64(v, &seconds) || seconds < YONDER_SCRIPT_SECONDS_MIN ||
        seconds > YONDER_SCRIPT_SECONDS_MAX) {
        os64_debug_log("yonder: yonder.conf's script_seconds is not 1 to 60; using 5");
        return YONDER_SCRIPT_SECONDS_DEFAULT;
    }
    return (uint32_t)seconds;
}

bool yonder_settings_saved_agent(char *out, size_t cap)
{
    int64_t rc = os64_conf_get(CONF_NAME, CONF_AGENT, out, cap);
    if (rc == OS64_CONF_NO_FILE || rc == OS64_CONF_NO_KEY)
        return false;
    if (rc != 0 || !yonder_agent_valid(out)) {
        os64_debug_log("yonder: yonder.conf's agent is unreadable or not an agent; using yonder's");
        return false;
    }
    return true;
}

// A whole percent from ZOOM_MIN to ZOOM_MAX, in thousandths; 0 for
// anything else.
static uint32_t zoom_of(const char *percent)
{
    uint64_t pct;
    if (!os64_parse_u64(percent, &pct) || pct < ZOOM_MIN || pct > ZOOM_MAX)
        return 0;
    return (uint32_t)pct * 10;
}

uint32_t yonder_settings_saved_zoom(void)
{
    char v[16];
    if (os64_conf_get(CONF_NAME, CONF_ZOOM, v, sizeof(v)) != 0)
        return 1000;
    uint32_t zoom = zoom_of(v);
    if (zoom == 0)
        os64_debug_log("yonder: yonder.conf's zoom is not a percent from 25 to 500; using 100");
    return zoom != 0 ? zoom : 1000;
}

bool yonder_settings_saved_diagnostics(char *out, size_t cap)
{
    int64_t rc = os64_conf_get(CONF_NAME, CONF_DIAGNOSTICS, out, cap);
    if (rc == OS64_CONF_NO_FILE || rc == OS64_CONF_NO_KEY)
        return false;
    size_t n = rc == 0 ? os64_strlen(out) : 0;
    while (n > 1 && out[n - 1] == '/')
        out[--n] = '\0';
    if (n == 0 || out[0] != '/') {
        os64_debug_log("yonder: yonder.conf's diagnostics is not a directory's full path; no page files");
        return false;
    }
    return true;
}

way_cache_t *yonder_settings_cache_open(void)
{
    char dir[OS64_PATH_MAX], v[32];
    if (os64_conf_get(CONF_NAME, CONF_CACHE_DIR, dir, sizeof(dir)) != 0 || dir[0] != '/')
        os64_strcopy(dir, sizeof(dir), WAY_CACHE_DIR);
    uint64_t mb = WAY_CACHE_MB_DEFAULT;
    if (os64_conf_get(CONF_NAME, CONF_CACHE_MB, v, sizeof(v)) == 0 &&
        (!os64_parse_u64(v, &mb) || mb == 0 || mb > WAY_CACHE_MB_MAX)) {
        os64_debug_log("yonder: yonder.conf's cache_mb is not a size from 1 to 65536; using 256");
        mb = WAY_CACHE_MB_DEFAULT;
    }
    way_cache_t *cache = way_cache_open(dir, mb * 1024 * 1024);
    if (cache != NULL && os64_conf_get(CONF_NAME, CONF_CACHE, v, sizeof(v)) == 0 &&
        os64_streq_nocase(v, "off"))
        way_cache_enable(cache, false);
    return cache;
}

// What the cache holds, on the dialog's status line, and whether it can
// hold anything at all.
static void holds_say(void)
{
    way_cache_stats_t st = {0};
    if (s.cache != NULL)
        way_cache_stats(s.cache, &st);
    s.usable = st.usable;
    char line[160];
    if (s.cache == NULL) {
        os64_strcopy(line, sizeof(line), "There was no memory for a cache.");
    } else if (!st.usable) {
        os64_snprintf(line, sizeof(line), "The cache cannot be kept: %s cannot be made or written.",
                      way_cache_dir(s.cache));
    } else {
        // Under a megabyte it is counted in kilobytes, or it reads as 0.0.
        char size[32];
        if (st.bytes < 1024 * 1024) {
            os64_snprintf(size, sizeof(size), "%lu KB", (unsigned long)((st.bytes + 1023) / 1024));
        } else {
            uint64_t tenths = st.bytes * 10 / (1024 * 1024);
            os64_snprintf(size, sizeof(size), "%lu.%lu MB", (unsigned long)(tenths / 10),
                          (unsigned long)(tenths % 10));
        }
        os64_snprintf(line, sizeof(line),
                      "The cache holds %d files, %s of %lu MB; this window took %lu from it.",
                      (int)st.entries, size, (unsigned long)(st.cap / (1024 * 1024)),
                      (unsigned long)(st.hits + st.confirmed + st.stale));
    }
    os64_ui_settings_report(&s.d, line);
}

static void emptied(os64_ui_widget_t *w, void *user)
{
    (void)w;
    (void)user;
    if (s.cache == NULL)
        return;
    char line[160];
    os64_snprintf(line, sizeof(line), "Emptied the cache: %d files gone.",
                  (int)way_cache_clear(s.cache));
    os64_ui_settings_report(&s.d, line);
}

// ── The relay ───────────────────────────────────────────────────────────

// `arg` is the dialog's window, handed over rather than read from `s`,
// which the loop clears when it closes the dialog.
static int64_t relay(void *arg)
{
    int64_t dialog = (int64_t)(intptr_t)arg;
    for (;;) {
        // Waits without taking: the events stay queued for the pump.
        if (os64_gui_event_wait(dialog, NULL) != 1)
            return 0;
        (void)os64_gui_event_ring(s.window, s.bell);
        char byte;
        if (os64_read_for(s.pumped[0], &byte, 1, OS64_WAIT_FOREVER) != 1)
            return 0;
    }
}

// The relay's end: the dialog's window is gone, so its wait answers at
// once, and a byte ends a wait for the loop.
static void relay_stop(void)
{
    if (s.thread > 0) {
        const char byte = 1;
        (void)os64_write(s.pumped[1], &byte, 1);
        (void)os64_thread_join((int32_t)s.thread, NULL);
        os64_close((int32_t)s.thread);
    }
    if (s.piped) {
        os64_close(s.pumped[0]);
        os64_close(s.pumped[1]);
    }
    s.thread = 0;
    s.piped = false;
}

// ── The dialog ──────────────────────────────────────────────────────────

static const char *preset_label(size_t i, void *user)
{
    (void)user;
    const yonder_agent_preset_t *p = yonder_agent_preset(i);
    return p != NULL ? p->name : "";
}

static void picked(os64_ui_listbox_t *list, void *user)
{
    (void)user;
    const yonder_agent_preset_t *p = list->selected >= 0 ? yonder_agent_preset((size_t)list->selected)
                                                        : NULL;
    if (p != NULL)
        os64_ui_textfield_set(&s.d.ui, &s.field, p->agent);
}

static void place(os64_ui_widget_t *w, os64_gui_rect_t r, bool staged)
{
    if (staged)
        os64_ui_widget_stage_bounds(w, r);
    else
        w->bounds = r;
}

static bool arrange(os64_ui_settings_t *d, os64_gui_rect_t b, int32_t row, bool staged)
{
    // The cache's row reads as a sentence — the box, its size, "MB" — with
    // the button at its right, and the zoom's as one too. Each field is as
    // wide as the largest number it takes.
    int32_t widths[8];
    const char *words[] = {s.heading.text, s.keep.w.text, s.empty.text,    s.mb.text,
                           "65536",        s.zoom_words.text, s.percent.text, "500"};
    for (size_t i = 0; i < sizeof(words) / sizeof(words[0]); i++)
        if (os64_ui_text_measure(&d->ui, OS64_FONT_ROLE_UI, words[i], os64_strlen(words[i]),
                                 &widths[i]) ||
            widths[i] > b.w)
            return false;
    int32_t box = widths[1] + 2 * row, size = widths[4] + row, mb = widths[3] + row / 2;
    int32_t button = widths[2] + 2 * row;
    if (box + size + mb + row + button > b.w)
        return false;
    int32_t last = b.y + (LIST_ROWS + 2) * row;
    place(&s.heading, (os64_gui_rect_t){b.x, b.y, b.w, row}, staged);
    place(&s.list.w, (os64_gui_rect_t){b.x, b.y + row, b.w, LIST_ROWS * row}, staged);
    place(&s.field.w, (os64_gui_rect_t){b.x, b.y + (LIST_ROWS + 1) * row, b.w, row}, staged);
    place(&s.keep.w, (os64_gui_rect_t){b.x, last, box, row}, staged);
    place(&s.size.w, (os64_gui_rect_t){b.x + box, last, size, row}, staged);
    place(&s.mb, (os64_gui_rect_t){b.x + box + size + row / 2, last, mb, row}, staged);
    place(&s.empty, (os64_gui_rect_t){b.x + b.w - button, last, button, row}, staged);
    int32_t words_w = widths[5] + row / 2, field = widths[7] + row, zoom_y = last + row;
    if (words_w + field + widths[6] + row > b.w)
        return false;
    place(&s.zoom_words, (os64_gui_rect_t){b.x, zoom_y, words_w, row}, staged);
    place(&s.zoom.w, (os64_gui_rect_t){b.x + words_w, zoom_y, field, row}, staged);
    place(&s.percent, (os64_gui_rect_t){b.x + words_w + field + row / 2, zoom_y,
                                        widths[6] + row / 2, row}, staged);
    int32_t dark_w = 0;
    if (os64_ui_text_measure(&d->ui, OS64_FONT_ROLE_UI, s.dark.w.text, os64_strlen(s.dark.w.text),
                             &dark_w))
        return false;
    dark_w += 2 * row;
    if (words_w + field + widths[6] + row + dark_w > b.w)
        return false;
    place(&s.dark.w, (os64_gui_rect_t){b.x + b.w - dark_w, zoom_y, dark_w, row}, staged);
    // The switch, and on the same row its time limit: "Script time limit:",
    // the slider, and the number it is at.
    int32_t switch_w = 0, limit_words = 0, limit_value = 0;
    if (os64_ui_text_measure(&d->ui, OS64_FONT_ROLE_UI, s.scripts.w.text,
                             os64_strlen(s.scripts.w.text), &switch_w) ||
        os64_ui_text_measure(&d->ui, OS64_FONT_ROLE_UI, s.limit_words.text,
                             os64_strlen(s.limit_words.text), &limit_words) ||
        os64_ui_text_measure(&d->ui, OS64_FONT_ROLE_UI, "60 s", 4, &limit_value))
        return false;
    switch_w += 2 * row;
    limit_words += row / 2;
    limit_value += row;
    int32_t slider = b.w - switch_w - limit_words - limit_value;
    if (slider < 3 * row)
        return false;
    int32_t script_y = last + 2 * row, at = b.x;
    place(&s.scripts.w, (os64_gui_rect_t){at, script_y, switch_w, row}, staged);
    at += switch_w;
    place(&s.limit_words, (os64_gui_rect_t){at, script_y, limit_words, row}, staged);
    at += limit_words;
    place(&s.limit.w, (os64_gui_rect_t){at, script_y, slider, row}, staged);
    at += slider;
    place(&s.limit_value, (os64_gui_rect_t){at + row / 2, script_y, limit_value - row / 2, row}, staged);
    return true;
}

static void limit_say(void)
{
    os64_snprintf(s.limit_text, sizeof(s.limit_text), "%d s", (int)s.limit.value);
}

static void limit_moved(os64_ui_slider_t *slider, void *user)
{
    (void)slider;
    (void)user;
    limit_say();
    os64_ui_mark_dirty(&s.d.ui, &s.limit_value);
}

static void apply(os64_ui_settings_t *d, bool save)
{
    const char *typed = s.field_buf;
    uint64_t mb = 0;
    if (s.usable && (!os64_parse_u64(s.size_buf, &mb) || mb == 0 || mb > WAY_CACHE_MB_MAX)) {
        os64_ui_settings_report(d, "The cache's size is a number of megabytes, 1 to 65536.");
        return;
    }
    uint32_t zoom = zoom_of(s.zoom_buf);
    if (zoom == 0) {
        os64_ui_settings_report(d, "The zoom is a whole percent, 25 to 500.");
        return;
    }
    if (!yonder_agent_valid(typed)) {
        os64_ui_settings_report(d, "An agent is printable ASCII, without #, under 256 characters.");
        return;
    }
    const char *agent = yonder_agent_keep(typed);
    if (agent == NULL) {
        os64_ui_settings_report(d, "No memory to keep that agent; nothing changed.");
        return;
    }
    uint32_t seconds = (uint32_t)s.limit.value;
    char seconds_text[8];
    os64_snprintf(seconds_text, sizeof(seconds_text), "%u", (unsigned)seconds);
    s.use(agent, s.scripts.checked, seconds);
    s.zoom_use(zoom);
    s.dark_use(s.dark.checked);
    const char *name = yonder_agent_name(agent);
    // A cache that cannot keep anything has no box to tick, and saves no
    // `cache = off` that would outlive whatever stopped it.
    bool keeping = s.usable && s.keep.checked;
    if (s.usable) {
        way_cache_enable(s.cache, keeping);
        way_cache_set_cap(s.cache, mb * 1024 * 1024);
    }
    char line[200], kept[40];
    os64_snprintf(kept, sizeof(kept), "keeping up to %lu MB", (unsigned long)mb);
    bool saved = !save || (os64_conf_set(CONF_NAME, CONF_AGENT, agent) == 0 &&
                           os64_conf_set(CONF_NAME, CONF_SCRIPTS,
                                          s.scripts.checked ? "on" : "off") == 0 &&
                           os64_conf_set(CONF_NAME, CONF_SCRIPT_SECONDS, seconds_text) == 0 &&
                           os64_conf_set(CONF_NAME, CONF_ZOOM, s.zoom_buf) == 0 &&
                           os64_conf_set(CONF_NAME, CONF_APPEARANCE,
                                          s.dark.checked ? "dark" : "light") == 0 &&
                           (!s.usable || (os64_conf_set(CONF_NAME, CONF_CACHE,
                                                        keeping ? "on" : "off") == 0 &&
                                          os64_conf_set(CONF_NAME, CONF_CACHE_MB,
                                                        s.size_buf) == 0)));
    if (!saved)
        os64_strcopy(line, sizeof(line), "Applied here, but yonder.conf could not be written.");
    else
        os64_snprintf(line, sizeof(line), "%s: zoom %d%%%s, asking as %s, %s, scripts %s (%u s)%s",
                      save ? "Saved" : "Applied", (int)(zoom / 10), s.dark.checked ? ", dark" : "",
                      name != NULL ? name : "the agent typed", keeping ? kept : "keeping nothing",
                      s.scripts.checked ? "on" : "off", (unsigned)seconds,
                      save ? ", here and in new windows." : ", here.");
    os64_ui_settings_report(d, line);
}

void yonder_settings_open(int64_t parent, uint32_t bell, const char *agent,
                          void (*use)(const char *agent, bool scripts, uint32_t script_seconds),
                          way_cache_t *cache, bool scripts, uint32_t script_seconds, uint32_t zoom,
                          void (*zoom_use)(uint32_t thousandths), bool dark,
                          void (*dark_use)(bool dark))
{
    // Open already: the helper brings it forward and keeps what was typed.
    if (!os64_ui_settings_open(&s.d, parent, "yonder Settings", BODY_ROWS, arrange, apply, NULL))
        return;
    s.use = use;
    s.zoom_use = zoom_use;
    s.dark_use = dark_use;
    s.cache = cache;
    s.usable = false;
    s.window = parent;
    s.bell = bell;
    os64_ui_label(&s.heading, "Identify as - pick what every site is told, or type another:");
    int selected = -1;
    for (size_t i = 0; i < yonder_agent_npresets(); i++)
        if (os64_streq(agent, yonder_agent_preset(i)->agent))
            selected = (int)i;
    os64_ui_listbox(&s.list, yonder_agent_npresets(), preset_label, picked, NULL);
    os64_ui_textfield(&s.field, s.field_buf, sizeof(s.field_buf), NULL, NULL, NULL);
    os64_ui_checkbox(&s.keep, "Keep pictures and style sheets, up to", way_cache_enabled(cache),
                     NULL, NULL);
    os64_ui_textfield(&s.size, s.size_buf, sizeof(s.size_buf), NULL, NULL, NULL);
    os64_ui_checkbox(&s.scripts, "Run page scripts", scripts, NULL, NULL);
    // Enabled whether or not scripts are on: it is the limit they will
    // have when they are.
    os64_ui_label(&s.limit_words, "Script time limit:");
    os64_ui_slider(&s.limit, YONDER_SCRIPT_SECONDS_MIN, YONDER_SCRIPT_SECONDS_MAX, 1,
                   (int32_t)script_seconds, limit_moved, NULL);
    limit_say();
    os64_ui_label(&s.limit_value, s.limit_text);
    os64_ui_label(&s.mb, "MB");
    os64_ui_button(&s.empty, "Empty the cache", emptied, NULL);
    os64_ui_label(&s.zoom_words, "Zoom every page to");
    os64_ui_textfield(&s.zoom, s.zoom_buf, sizeof(s.zoom_buf), NULL, NULL, NULL);
    os64_ui_label(&s.percent, "%");
    os64_ui_checkbox(&s.dark, "Dark pages", dark, NULL, NULL);
    os64_ui_widget_t *kids[] = {&s.heading, &s.list.w,     &s.field.w, &s.keep.w,  &s.size.w,
                                &s.mb,      &s.empty,      &s.zoom_words, &s.zoom.w, &s.percent,
                                &s.scripts.w, &s.limit_words, &s.limit.w, &s.limit_value,
                                &s.dark.w};
    for (size_t i = 0; i < sizeof(kids) / sizeof(kids[0]); i++)
        os64_ui_add_child(&s.d.body, kids[i]);
    os64_ui_listbox_set(&s.d.ui, &s.list, yonder_agent_npresets(), selected);
    os64_ui_textfield_set(&s.d.ui, &s.field, agent);
    char pct[sizeof(s.zoom_buf)];
    os64_snprintf(pct, sizeof(pct), "%d", (int)((zoom + 5) / 10));
    os64_ui_textfield_set(&s.d.ui, &s.zoom, pct);
    os64_ui_settings_ready(&s.d);
    holds_say();
    os64_ui_set_enabled(&s.d.ui, &s.empty, s.usable);
    os64_ui_set_enabled(&s.d.ui, &s.keep.w, s.usable);
    os64_ui_set_enabled(&s.d.ui, &s.size.w, s.usable);
    if (s.usable) {
        way_cache_stats_t st;
        way_cache_stats(cache, &st);
        char now[sizeof(s.size_buf)];
        os64_snprintf(now, sizeof(now), "%lu", (unsigned long)(st.cap / (1024 * 1024)));
        os64_ui_textfield_set(&s.d.ui, &s.size, now);
    }
    os64_ui_set_focus(&s.d.ui, &s.list.w);
    os64_ui_paint(&s.d.ui);
    // Without the relay the dialog would answer only when yonder's own
    // window had an event: it is closed rather than left half alive.
    if (os64_pipe(s.pumped) < 0) {
        os64_ui_settings_close(&s.d);
        return;
    }
    s.piped = true;
    s.thread = os64_thread(relay, (void *)(intptr_t)s.d.window);
    if (s.thread < 0) {
        s.thread = 0;
        os64_ui_settings_close(&s.d);
        relay_stop();
    }
}

void yonder_settings_rung(void)
{
    if (s.d.window <= 0)
        return;
    os64_ui_settings_pump(&s.d);
    if (s.d.window <= 0) {
        relay_stop();
        return;
    }
    const char byte = 1;
    (void)os64_write(s.pumped[1], &byte, 1);
}

void yonder_settings_close(void)
{
    if (s.d.window > 0)
        os64_ui_settings_close(&s.d);
    relay_stop();
}
