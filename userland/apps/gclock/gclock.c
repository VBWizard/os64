// gclock — a retained-mode clock following the Appearance Interface face.
// Digit slots keep proportional figures and blinking separators stationary.
// See LIBDRAW.md for the retained-mode event/paint loop this app illustrates.

#include "os64/os64.h"
#include "os64/gui.h"
#include "os64/draw.h"
#include "os64/fmt.h"
#include "os64/ui.h"
#include "os64/conf.h"
#include "os64/font_settings.h"

#define CLOCK_CHARS 8
#define CLOCK_SCALABLE_FACE "/etc/fonts/DejaVuSans.ttf"

typedef struct {
    int32_t x, y;
    bool titlebar, pinned, save_position, blink;
    uint32_t font_size; // Zero inherits the Interface size.
    const char *path;
} gclock_conf_t;

typedef struct {
    int32_t advances[11]; // Digits 0..9, then the colon.
    int32_t digit_w, colon_w, row_h, width, height;
} clock_layout_t;

static int64_t gClockWin;
static os64_draw_ctx_t gCtx;
static os64_ui_t gUi;
static os64_ui_widget_t gRoot, gDigits[CLOCK_CHARS];
static os64_ui_class_t gDigitClass;
static char gText[CLOCK_CHARS][2];
static gclock_conf_t gConf;
static clock_layout_t gLayout, *gPendingLayout;
static bool gRunning = true, separatorsShown = true;

static gclock_conf_t conf_defaults(const char *path)
{
    return (gclock_conf_t){.x = 280, .y = 10, .titlebar = true,
        .save_position = true, .blink = true, .path = path};
}

static bool parse_i32(const char **text, int32_t *out)
{
    const char *p = *text;
    bool negative = false;
    uint64_t value = 0;
    uint64_t limit;

    if (*p == '+' || *p == '-') {
        negative = (*p == '-');
        p++;
    }
    if (*p < '0' || *p > '9')
        return false;
    limit = negative ? 2147483648ULL : 2147483647ULL;
    while (*p >= '0' && *p <= '9') {
        uint64_t digit = (uint64_t)(*p++ - '0');
        if (value > (limit - digit) / 10)
            return false;
        value = value * 10 + digit;
    }
    *out = negative ? (int32_t)(-(int64_t)value) : (int32_t)value;
    *text = p;
    return true;
}

static bool parse_position(const char *value, int32_t *x, int32_t *y)
{
    int32_t px, py;
    const char *p = value;

    if (!parse_i32(&p, &px) || *p++ != ',' || !parse_i32(&p, &py) || *p != '\0')
        return false;
    *x = px;
    *y = py;
    return true;
}

static bool conf_line(const char *key, const char *value, void *user)
{
    gclock_conf_t *conf = user;
    bool valid = false;
    const char *expected = "unknown setting";
    if (!key) {
        expected = "expected key = value";
    } else if (os64_streq_nocase(key, "position")) {
        valid = parse_position(value, &conf->x, &conf->y);
        expected = "Position must be x,y";
    } else if (os64_streq_nocase(key, "fontsize")) {
        int32_t size;
        const char *p = value;
        if (os64_streq_nocase(value, "inherit")) {
            conf->font_size = 0;
            valid = true;
        } else if (parse_i32(&p, &size) && !*p && size >= 8 && size <= 96) {
            conf->font_size = (uint32_t)size;
            valid = true;
        }
        expected = "FontSize must be inherit or 8..96 pixels";
    } else {
        bool *dest = NULL;
        if (os64_streq_nocase(key, "titlebar")) dest = &conf->titlebar;
        else if (os64_streq_nocase(key, "pinned")) dest = &conf->pinned;
        else if (os64_streq_nocase(key, "saveposition")) dest = &conf->save_position;
        else if (os64_streq_nocase(key, "blink")) dest = &conf->blink;
        if (dest) {
            valid = os64_conf_get_bool(value, dest);
            expected = "expected true/false or on/off";
        }
    }
    if (!valid)
        os64_hprintf(OS64_STDERR, "gclock: %s: %s (%s = %s); ignored\n",
                     conf->path, expected, key ? key : "?", value);
    return true;
}

static void refresh_clock_text(void)
{
    os64_date_t now = {0};
    char text[CLOCK_CHARS + 1];
    os64_date_now(&now, NULL);
    os64_snprintf(text, sizeof(text), "%02d:%02d:%02d",
                  now.hour, now.minute, now.second);
    for (int i = 0; i < CLOCK_CHARS; ++i) {
        bool colon = i == 2 || i == 5;
        gText[i][0] = colon && gConf.blink && !separatorsShown ? '\0' : text[i];
    }
}

static int32_t max_i32(int32_t a, int32_t b) { return a > b ? a : b; }

static os64_font_status_t measure_clock(os64_ui_t *ui, clock_layout_t *m)
{
    *m = (clock_layout_t){0};
    for (int i = 0; i < 11; ++i) {
        char c = i == 10 ? ':' : '0' + i;
        os64_font_status_t status = os64_ui_text_measure(ui, OS64_FONT_ROLE_UI,
                                                         &c, 1, &m->advances[i]);
        if (status) return status;
        if (i < 10) m->digit_w = max_i32(m->digit_w, m->advances[i]);
    }
    m->digit_w = max_i32(1, m->digit_w);
    m->colon_w = max_i32(1, m->advances[10]);
    m->row_h = os64_ui_font_row_height(ui, OS64_FONT_ROLE_UI);
    int32_t pad = max_i32(6, m->row_h / 3);
    m->width = max_i32(64, 6 * m->digit_w + 2 * m->colon_w + 2 * pad);
    m->height = max_i32(32, m->row_h + 2 * pad);
    return OS64_FONT_OK;
}

static void arrange_clock(const clock_layout_t *m, int32_t width, int32_t height,
                          bool stage)
{
    os64_gui_rect_t box = {0, 0, width, height};
    if (stage) os64_ui_widget_stage_bounds(&gRoot, box);
    else gRoot.bounds = box;
    int32_t x = (width - 6 * m->digit_w - 2 * m->colon_w) / 2;
    for (int i = 0; i < CLOCK_CHARS; ++i) {
        int32_t w = i == 2 || i == 5 ? m->colon_w : m->digit_w;
        box = (os64_gui_rect_t){x, (height - m->row_h) / 2, w, m->row_h};
        if (stage) os64_ui_widget_stage_bounds(&gDigits[i], box);
        else gDigits[i].bounds = box;
        x += w;
    }
}

static void layout_clock(os64_ui_t *ui)
{
    (void)ui;
    arrange_clock(&gLayout, (int32_t)gCtx.surf.width, (int32_t)gCtx.surf.height, false);
}

static void digit_paint(os64_ui_widget_t *w, os64_draw_ctx_t *ctx,
                        const os64_ui_theme_t *theme)
{
    os64_draw_fill_rect(&ctx->surf, w->bounds, theme->panel_bg);
    char c = w->text[0];
    if (!c) return;
    if (c != ':' && (c < '0' || c > '9')) return;
    int index = c == ':' ? 10 : c - '0';
    os64_ui_draw_text(w->ui, &w->run, OS64_FONT_ROLE_UI, &ctx->surf, w->bounds,
        w->bounds.x + (w->bounds.w - gLayout.advances[index]) / 2, w->bounds.y,
        w->text, 1, theme->label_fg, theme->panel_bg);
}

static os64_font_status_t plan_clock_font(os64_ui_t *ui, void *user, void **out)
{
    (void)user;
    *out = NULL;
    clock_layout_t *m = os64_malloc(sizeof(*m));
    if (!m) return OS64_FONT_NO_MEMORY;
    os64_font_status_t status = measure_clock(ui, m);
    if (status) { os64_free(m); return status; }
    int32_t width = m->width, height = m->height;
    if (gClockWin > 0) {
        width = max_i32(width, (int32_t)gCtx.surf.width);
        height = max_i32(height, (int32_t)gCtx.surf.height);
    }
    arrange_clock(m, width, height, true);
    gPendingLayout = m;
    *out = m;
    return OS64_FONT_OK;
}

static void discard_clock_font(os64_ui_t *ui, void *user, void *plan)
{
    (void)ui; (void)user;
    gPendingLayout = NULL;
    os64_free(plan);
}

// The window grows after font and widget preparation succeeds. A refused
// minimum leaves the old window/font intact; commit cannot need allocation.
static os64_font_status_t resize_for_font(void *user, void *plan)
{
    (void)user; (void)plan;
    if (gClockWin > 0 && os64_gui_window_set_min_size(gClockWin,
            (uint32_t)gPendingLayout->width, (uint32_t)gPendingLayout->height))
        return OS64_FONT_LIMIT;
    return OS64_FONT_OK;
}

static void commit_clock_font(os64_ui_t *ui, void *user, void *plan)
{
    (void)user;
    gLayout = *(clock_layout_t *)plan;
    if (gClockWin > 0) {
        if (os64_draw_ctx_refresh(&gCtx)) {
            os64_complain("gclock: cannot refresh resized window");
            gRunning = false;
        } else layout_clock(ui);
    }
    discard_clock_font(ui, NULL, plan);
}

// An app-local size changes the Interface role in this window's candidate,
// leaving the shared selection and other applications untouched.
static void clock_font_config(const os64_font_config_t *shared, os64_font_config_t *local)
{
    os64_font_config_defaults(local);
    local->roles[OS64_FONT_ROLE_UI] = shared->roles[OS64_FONT_ROLE_UI];
    os64_font_config_role_t *role = &local->roles[OS64_FONT_ROLE_UI];
    if (gConf.font_size) {
        role->size = gConf.font_size;
        if (role->size != 16 && os64_streq(role->face[0], "builtin")) {
            os64_memset(role->face, 0, sizeof(role->face));
            os64_strcopy(role->face[0], sizeof(role->face[0]), CLOCK_SCALABLE_FACE);
        }
    }
}

static void refresh_clock_font(os64_ui_t *ui)
{
    os64_font_config_t shared, local;
    uint64_t generation;
    if (os64_font_settings_current(&shared, &generation)) {
        os64_debug_log("gclock: cannot read Interface font; keeping current font");
        return;
    }
    if (ui->font_settings_ready && ui->font_generation == generation) return;
    clock_font_config(&shared, &local);
    os64_font_set_t *set = NULL;
    os64_font_config_error_t error;
    if (os64_font_config_prepare(os64_ui_font_context(ui), &local, &set, &error)) {
        os64_debug_log("gclock: cannot prepare Interface font/size; keeping current font");
        return;
    }
    os64_font_consumer_t consumer;
    os64_ui_font_consumer(ui, &consumer);
    consumer.barrier = resize_for_font;
    os64_font_status_t status = os64_font_adopt(set, &consumer, 1, NULL);
    os64_font_set_release(set);
    if (status) {
        os64_debug_log("gclock: cannot fit Interface font; keeping current font");
    } else {
        ui->font_generation = generation;
        ui->font_settings_ready = true;
    }
}

static void on_close_request(os64_ui_t *ui)
{
    (void)ui;
    os64_gui_window_state_t st;
    char path[OS64_CONF_PATH_MAX] = "", target[OS64_CONF_PATH_MAX];
    gclock_conf_t conf = conf_defaults(path);
    // Respect edits made while the clock was open, including SavePosition.
    int64_t rc = os64_conf_find_read("gclock.conf", conf_line, &conf, path, sizeof(path));
    bool readable = rc >= 0 || (rc == OS64_CONF_NO_FILE && !path[0]);
    if (!readable || os64_conf_target("gclock.conf", target, sizeof(target))) {
        os64_complain("gclock: cannot read configuration; window state not saved");
    } else if (!os64_gui_window_get_state(gClockWin, &st)) {
        char pos[32], size[16];
        bool save_position = conf.save_position && !(st.flags & OS64_GUI_WINDOW_MAXIMIZED);
        os64_snprintf(pos, sizeof(pos), "%d,%d", save_position ? st.x : conf.x,
                      save_position ? st.y : conf.y);
        if (conf.font_size) os64_snprintf(size, sizeof(size), "%u", conf.font_size);
        else os64_strcopy(size, sizeof(size), "inherit");
        os64_conf_pair_t save[] = {
            {"titlebar", (st.flags & OS64_GUI_WINDOW_NO_DECORATIONS) ? "off" : "on"},
            {"pinned", (st.flags & OS64_GUI_WINDOW_PINNED) ? "true" : "false"},
            {"position", pos},
            {"saveposition", conf.save_position ? "true" : "false"},
            {"fontsize", size},
            {"blink", conf.blink ? "true" : "false"},
        };
        // A first personal file shadows the system copy wholesale. Seed the
        // effective settings so a frozen startup position survives that save.
        size_t count = !os64_streq(path, target) ? 6 : save_position ? 3 : 2;
        if (os64_conf_write("gclock.conf", save, count))
            os64_complain("gclock: unable to save window state");
    }
    gRunning = false;
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    char conf_path[OS64_CONF_PATH_MAX] = "";
    gConf = conf_defaults(conf_path);
    int64_t rc = os64_conf_find_read("gclock.conf", conf_line, &gConf,
                                    conf_path, sizeof(conf_path));
    if (rc < 0 && rc != OS64_CONF_NO_FILE)
        os64_complain("gclock: configuration read incomplete; using available settings");

    // Prepare fonts and measured furniture before requesting content size;
    // the WM supplies the current decoration's frame dimensions.
    os64_ui_init(&gUi, NULL);
    gUi.on_close = on_close_request;
    gUi.on_resize = layout_clock;
    gUi.font_session = refresh_clock_font;
    os64_ui_panel(&gRoot);
    os64_ui_set_root(&gUi, &gRoot);
    refresh_clock_text();
    for (int i = 0; i < CLOCK_CHARS; ++i) {
        os64_ui_label(&gDigits[i], gText[i]);
        if (i == 0) {
            // Borrow the constructed label's callbacks without a data-symbol
            // import, which would require an unsupported ELF COPY relocation.
            gDigitClass = *gDigits[i].cls;
            gDigitClass.paint = digit_paint;
        }
        gDigits[i].cls = &gDigitClass;
        os64_ui_add_child(&gRoot, &gDigits[i]);
    }
    os64_ui_font_planner(&gUi, plan_clock_font, commit_clock_font, discard_clock_font, NULL);
    refresh_clock_font(&gUi);
    if (!gUi.font_settings_ready) {
        if (measure_clock(&gUi, &gLayout)) {
            os64_complain("gclock: cannot measure clock");
            os64_ui_font_release(&gUi);
            return 1;
        }
    }
    uint64_t flags = (gConf.titlebar ? 0 : OS64_GUI_WINDOW_NO_DECORATIONS) |
                     (gConf.pinned ? OS64_GUI_WINDOW_PINNED : 0);
    gClockWin = os64_gui_window_create_content("gclock", gConf.x, gConf.y,
                                               gLayout.width, gLayout.height, flags);
    if (gClockWin <= 0) {
        os64_complain("gclock: cannot create GUI window");
        os64_ui_font_release(&gUi);
        return 1;
    }
    if (os64_gui_window_set_min_size(gClockWin, gLayout.width, gLayout.height) ||
        os64_draw_ctx_init(&gCtx, gClockWin)) {
        os64_complain("gclock: cannot initialize window");
        os64_gui_window_destroy(gClockWin);
        os64_ui_font_release(&gUi);
        return 1;
    }
    gUi.ctx = &gCtx;
    layout_clock(&gUi);
    os64_ui_mark_dirty(&gUi, &gRoot);
    os64_ui_paint(&gUi);

    // Event wakes handle appearance/resize/close before the next tick. The
    // frame clock sleeps while the clock is covered, waking for queued events.
    os64_frame_clock_t frameClock;
    os64_frame_clock_init(&frameClock);
    os64_frame_clock_bind(&frameClock, gClockWin);
    while (gRunning) {
        os64_gui_event_t ev;
        while (gRunning && os64_gui_event_poll(gClockWin, &ev) == 1)
            os64_ui_dispatch(&gUi, &ev);
        if (!gRunning) break;
        refresh_clock_text();
        for (int i = 0; i < CLOCK_CHARS; ++i) os64_ui_mark_dirty(&gUi, &gDigits[i]);
        os64_ui_paint(&gUi);
        os64_frame_wait(&frameClock, 500);
        separatorsShown = !separatorsShown;
    }
    os64_ui_font_release(&gUi);
    os64_gui_window_destroy(gClockWin);
    return 0;
}
