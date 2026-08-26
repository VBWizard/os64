// desktop(1) — the os64 desktop shell.
//
// WHAT MOVED, AND WHY. Until 2026-08-25 the desktop lived in the kernel:
// gui/desktop.c read desktop.conf, decoded a PPM, and painted the
// compositor's background surface, while gui/startup.c read gui.conf and
// spawned the programs listed in it. That made the kernel a small `init`
// with an image decoder in it — ring-0 code parsing a file any user can
// write — and it meant a click on empty desktop landed nowhere, because
// there was no window there to land on.
//
// This program is the replacement, and it is an ORDINARY CLIENT. The
// compositor stays in the kernel; the shell is a program, exactly as X11 has
// always had it — the server owns the root window and compositing, while the
// thing drawing your wallpaper is just another connection. The only thing
// that makes this window the desktop is OS64_GUI_WINDOW_DESKTOP: the bottom
// z-band, no chrome, out of the Alt+Tab walk, and the WM verbs that would
// make it vanish or float declined — the whole contract is listed on the
// flag in os64/gui.h. What it keeps is the part that matters here: it is
// still an input target.
//
// WHY A WINDOW AND NOT A "SET WALLPAPER" CALL (Chris's ruling, the day this
// was designed): handing the kernel a bitmap would have been less work and
// would have given us wallpaper and nothing else. A window is an INPUT
// TARGET. A click that lands on no application lands here — and that click
// is where a root menu (twm, 1987) and the coming launcher come from.
//
// IT DOES NOT KNOW WHO STARTED IT, deliberately. Today the kernel spawns it
// as its one GUI program. When husk-as-init lands, that line moves to the
// init table and nothing in this file changes, because nothing in this file
// ever asked.

#include "os64/os64.h"
#include "os64/io.h"
#include "os64/conf.h"
#include "os64/image.h"
#include "os64/draw.h"
#include "os64/gui.h"
#include "os64/proc.h"
#include "os64/str.h"
#include "os64/fmt.h"       // os64_snprintf — the click position, as argv for the launcher
#include "os64/thread.h"

#define DESKTOP_PATH_MAX   192
#define DESKTOP_APPS_MAX   16

// EVERY COMPLAINT GOES TO THE KERNEL LOG AS WELL AS STDERR. The desktop's
// stderr is the console it was spawned from, which on a GUI boot is a VT
// nobody is looking at — so a mistyped `launcher =` line produced a
// perfectly good complaint that nobody could find (Chris, 2026-08-25:
// "there was no feedback. Nothing in the log."). os64_debug_log puts the
// same line where `cat /home/os64.log` and `tail` can see it.
static void complain(const char *fmt, ...)
{
    char line[256];
    va_list ap;
    va_start(ap, fmt);
    os64_vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    os64_hprintf(2 /* stderr */, "%s", line);
    // The log line without the trailing newline: the log adds its own.
    size_t n = os64_strlen(line);
    if (n && line[n - 1] == '\n')
        line[n - 1] = 0;
    os64_debug_log(line);
}

// The fallback ground. Only reached if desktop.conf names no color — and
// note the kernel's own test pattern is UNDER us regardless, which is what
// shows if this program never starts or dies.
#define DESKTOP_DEFAULT_COLOR 0xff0c1830u

typedef struct {
    uint32_t color;
    bool     have_color;
    char     image[DESKTOP_PATH_MAX];
    bool     have_image;
    char     conf_path[OS64_CONF_PATH_MAX];   // a LADDER path, sized by the ladder's own cap (Codex #31 rd7) — 192 refused a legal 200-byte resolve as NO_FILE
} desktop_config_t;

typedef struct {
    char   apps[DESKTOP_APPS_MAX][DESKTOP_PATH_MAX];
    size_t count;
    bool   overflowed;
    // `launcher = [left|right|middle] <program>` — the program a click on
    // bare wallpaper runs, one per button (indexed by OS64_GUI_MOUSE_*).
    // Empty = that button does nothing. See gui.conf for the ruling.
    char   launcher[3][DESKTOP_PATH_MAX];
    char   path[OS64_CONF_PATH_MAX];   // which gui.conf won — named in complaints; ladder-sized, not app-path-sized (rd7)
} startup_list_t;

// ── desktop.conf ────────────────────────────────────────────────────────────

static bool parse_hex_color(const char *s, uint32_t *out)
{
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
        s += 2;
    uint32_t v = 0;
    int digits = 0;
    for (; *s && *s != ' ' && *s != '\t' && *s != '\r'; s++, digits++) {
        char c = *s;
        uint32_t d;
        if (c >= '0' && c <= '9')      d = (uint32_t)(c - '0');
        else if (c >= 'a' && c <= 'f') d = (uint32_t)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') d = (uint32_t)(c - 'A' + 10);
        else return false;
        v = (v << 4) | d;
    }
    // Exactly six digits: '#' starts a comment in every os64 config, so the
    // hex must be written 0xRRGGBB and an alpha byte has nowhere to go.
    if (digits != 6)
        return false;
    *out = 0xff000000u | v;
    return true;
}

static bool desktop_conf_line(const char *key, const char *value, void *user)
{
    desktop_config_t *cfg = (desktop_config_t *)user;
    if (key == NULL) {
        complain("desktop: %s: not a `key = value` line: %s\n",
                     cfg->conf_path, value);
        return true;
    }
    // SETTING names fold case (they are names, not data); the VALUE never
    // does — a path is case-sensitive on ext2, and so is a file name.
    if (os64_streq_nocase(key, "color")) {
        if (!parse_hex_color(value, &cfg->color))
            complain("desktop: %s: color must be 0xRRGGBB, got \"%s\"\n",
                         cfg->conf_path, value);
        else
            cfg->have_color = true;
    } else if (os64_streq_nocase(key, "image")) {
        // REFUSE AN OVERLONG PATH, DO NOT TRUNCATE IT (Codex #31 rd2 — the
        // same finding as gui.conf's start lines one round earlier, on the
        // other file). os64_strcopy answers with the SOURCE length, so a
        // value that did not fit is detectable; a truncated path is a
        // different path, and a prefix that happens to exist would show the
        // wrong picture with no complaint. The kernel's reader carried a
        // 256-byte buffer; this one is 192, so the refusal has to be loud.
        size_t want = os64_strcopy(cfg->image, sizeof(cfg->image), value);
        if (want >= sizeof(cfg->image)) {
            complain(
                         "desktop: %s: image path over %d characters — ignored: %s\n",
                         cfg->conf_path, (int)sizeof(cfg->image) - 1, value);
            cfg->image[0] = 0;
            cfg->have_image = false;
            return true;
        }
        cfg->have_image = (cfg->image[0] != 0);
    } else {
        // A typo is not a default (Codex #31 rd2): `colour = ...` used to be
        // accepted and ignored, leaving the built-in ground with no hint
        // why. The kernel's reader named unknown keys; so does this one.
        complain("desktop: %s: unknown setting '%s' — ignored\n",
                     cfg->conf_path, key);
    }
    return true;
}

// ── gui.conf — the shell's rc ───────────────────────────────────────────────
//
// This file used to be read by the kernel. It is read HERE now, for the same
// reason husk reads husk.rc: a shell reads its own startup file. One file,
// one reader.

static bool gui_conf_line(const char *key, const char *value, void *user)
{
    startup_list_t *list = (startup_list_t *)user;

    // SAY SO. The kernel's reader complained about every one of these, and
    // this one silently swallowed them behind a comment claiming the caller
    // reported them — the caller reports truncation and overflow and nothing
    // else, so the claim was false and the diagnostic was simply gone
    // (Codex #31). "My startup entry does nothing and I cannot tell why" is
    // the exact afternoon a config file exists to prevent.
    if (key == NULL) {
        complain("desktop: %s: not a `key = value` line: %s\n",
                     list->path, value);
        return true;
    }
    if (os64_streq_nocase(key, "launcher")) {
        // `launcher = [left|right|middle] <program>`. The button word is a
        // setting name and folds case; the program is a path and does not.
        // No button word = right, the convention every desktop since
        // Windows 95 settled on for "what can I do here?" (twm used left,
        // in 1987, when the mouse had a spare button).
        const char *prog = value;
        int button = OS64_GUI_MOUSE_RIGHT;
        char word[8];
        size_t wl = 0;
        while (prog[wl] && prog[wl] != ' ' && prog[wl] != '\t' && wl < sizeof(word) - 1)
            word[wl] = prog[wl], wl++;
        word[wl] = 0;
        bool named = true;
        if (os64_streq_nocase(word, "left"))        button = OS64_GUI_MOUSE_LEFT;
        else if (os64_streq_nocase(word, "right"))  button = OS64_GUI_MOUSE_RIGHT;
        else if (os64_streq_nocase(word, "middle")) button = OS64_GUI_MOUSE_MIDDLE;
        else named = false;
        if (named) {
            prog += wl;
            while (*prog == ' ' || *prog == '\t')
                prog++;
        }
        if (prog[0] == 0) {
            complain("desktop: %s: 'launcher' with no program — ignored\n",
                         list->path);
            return true;
        }
        // Same refusal as `start` below: a truncated path is a different
        // program, and this one would run on every click.
        size_t want = os64_strcopy(list->launcher[button], DESKTOP_PATH_MAX, prog);
        if (want >= DESKTOP_PATH_MAX) {
            complain(
                         "desktop: %s: launcher path over %d characters — ignored: %s\n",
                         list->path, DESKTOP_PATH_MAX - 1, prog);
            list->launcher[button][0] = 0;
            return true;
        }
        // PROBE THE PATH NOW, not at the first click. A launcher that does
        // not exist is the classic mistyped line (`launcher = /bin/grootmenu
        // right` makes the whole value the path), and a click that silently
        // does nothing is the worst possible answer to it. Opening it is the
        // only existence test ring 3 has; a spawn failure at click time is
        // still reported, this just says it at startup, by name.
        int64_t probe = os64_open(list->launcher[button], "r");
        if (probe < 0) {
            complain("desktop: %s: launcher \"%s\" cannot be opened (%ld) — the form is `launcher = [left|right|middle] <program>`\n",
                     list->path, list->launcher[button], (long)probe);
            list->launcher[button][0] = 0;
            return true;
        }
        os64_close((int32_t)probe);
        return true;
    }
    if (!os64_streq_nocase(key, "start")) {
        complain("desktop: %s: unknown setting '%s' — ignored\n",
                     list->path, key);
        return true;
    }
    if (value[0] == 0) {
        complain("desktop: %s: 'start' with no program — ignored\n",
                     list->path);
        return true;
    }
    if (list->count >= DESKTOP_APPS_MAX) {
        list->overflowed = true;
        return true;
    }

    // REFUSE AN OVERLONG PATH, DO NOT TRUNCATE IT. os64_strcopy returns the
    // SOURCE length (strlcpy semantics), so a value that did not fit is
    // detectable — and must be, because a truncated path is a DIFFERENT path,
    // and a prefix that happens to exist would launch the wrong program
    // (Codex #31). The kernel's reader refused these explicitly; this one
    // inherited the job along with the file.
    size_t want = os64_strcopy(list->apps[list->count], DESKTOP_PATH_MAX, value);
    if (want >= DESKTOP_PATH_MAX) {
        complain(
                     "desktop: %s: start path over %d characters — ignored: %s\n",
                     list->path, DESKTOP_PATH_MAX - 1, value);
        list->apps[list->count][0] = 0;
        return true;
    }
    list->count++;
    return true;
}

// ── saying why a config did nothing ─────────────────────────────────────────
//
// ONE VOICE FOR EVERY WAY A CONFIG READ CAN FAIL (Codex #31 rd2 + rd4). Both
// files are read as find-then-read so "no file anywhere" and "found one and
// could not read it" stay different answers — the first is the only one
// allowed to mean "use the built-in default". Everything else the reader
// can answer is named here: a truncated file, an I/O error, no memory, and
// a found file that would not open. A config the operator wrote that
// silently does nothing is the afternoon a config file exists to prevent.
static void conf_report(const char *what, const char *path, bool found, int64_t rc)
{
    if (!found)
        return;   // absent everywhere: the default applies, nothing to say
    switch (rc) {
    case OS64_CONF_NO_FILE:
        complain("desktop: %s: found but could not be opened (%s)\n",
                     what, path);
        break;
    case OS64_CONF_TRUNCATED:
        complain("desktop: %s exceeds %d bytes; trailing settings ignored\n",
                     path, OS64_CONF_MAX - 1);
        break;
    case OS64_CONF_IO_ERROR:
        complain("desktop: %s: read error — settings ignored\n", path);
        break;
    case OS64_CONF_NO_MEMORY:
        complain("desktop: %s: out of memory reading it — settings ignored\n",
                     path);
        break;
    default:
        if (rc < 0)
            complain("desktop: %s: read failed (%ld) — settings ignored\n",
                         path, (long)rc);
        break;
    }
}

// ── the reaper ──────────────────────────────────────────────────────────────
//
// A DESKTOP THAT STARTS PROGRAMS MUST BURY THEM (Codex #30/#31). The kernel
// used to set `autoReap` on every app it launched from gui.conf; ring 3 has
// no such switch, so a startup app that exits stays a zombie for as long as
// the desktop lives — holding its task memory and its executable's resources
// — and the launcher will make that worse by design.
//
// It is a THREAD because the main loop blocks in os64_gui_event_wait, whose
// backstop loops INSIDE the kernel: it never returns without an event, so a
// reap folded into that loop would only happen when somebody touched the
// desktop. A picture nobody clicks would keep its corpses indefinitely.
//
// This is init's oldest job, and it wears init's shape: block until a child
// ends, collect it, repeat forever. os64_wait(0) answers NEGATIVE when there
// are no children AT ALL (task_wait says 0 in the kernel, and syscall_wait
// maps that to SYSCALL_RESULT_INVALID — -1 — before ring 3 ever sees it;
// Codex #31 rd5 read the kernel half and thought zero reached us), which
// would spin — so that case sleeps rather than retrying immediately. When
// children exist the wait blocks properly and costs nothing. The OTHER
// negative answer, OS64_INTERRUPTED, means a handler ran and the children
// are all still alive — that one waits again at once; the nap is for "nobody
// to wait for", not for "somebody dragged a window".
#define DESKTOP_REAP_IDLE_MS 1000

static int64_t reaper(void *arg)
{
    (void)arg;
    for (;;) {
        int32_t code = 0;
        int64_t pid = os64_wait(0, &code);
        if (pid > 0) {
            // Said out loud: a GUI app that dies leaves no other trace, and
            // "it was running a moment ago" is the whole of the evidence
            // otherwise.
            complain("desktop: task %ld exited (%d)\n",
                         (long)pid, (int)code);
            continue;
        }
        if (pid == OS64_INTERRUPTED)
            continue;                       // a handler ran; the children live on
        os64_sleep(DESKTOP_REAP_IDLE_MS);   // no children right now
    }
    return 0;
}

// ── painting ────────────────────────────────────────────────────────────────

static void paint(os64_draw_ctx_t *ctx, const desktop_config_t *cfg,
                  const os64_image_t *img)
{
    os64_draw_ctx_refresh(ctx);

    os64_gui_rect_t all = {0, 0, (int32_t)ctx->surf.width,
                                 (int32_t)ctx->surf.height};
    os64_draw_fill_rect(&ctx->surf, all,
                        cfg->have_color ? cfg->color : DESKTOP_DEFAULT_COLOR);

    if (img->pixels != NULL) {
        // Centered, never scaled — and an image larger than the screen is
        // cropped around its middle rather than shifted, which is the blit's
        // contract. (tools/mkwall.py sizes an image for the screen.)
        int32_t x = ((int32_t)ctx->surf.width  - (int32_t)img->width)  / 2;
        int32_t y = ((int32_t)ctx->surf.height - (int32_t)img->height) / 2;
        os64_draw_blit(&ctx->surf, x, y, img->pixels,
                       img->width, img->height, img->width);
    }
    os64_draw_publish(ctx, NULL);
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    uint32_t sw = 0, sh = 0;
    if (os64_gui_screen_info(&sw, &sh) != 0 || sw == 0 || sh == 0) {
        complain("desktop: no GUI here (screen_info)\n");
        return 1;
    }

    // The window IS the screen. START_UNFOCUSED asks that the programs this
    // shell is about to start get the keyboard rather than the wallpaper —
    // but on the ordinary boot this is the FIRST window, and the WM focuses
    // a first window regardless (something must hold focus, or keys route
    // to NULL — see the flag's comment in window.h). So the desktop IS
    // focused at birth, and stays so if gui.conf starts nothing; each
    // startup app takes focus as it appears. Clicking the wallpaper focuses
    // it again — START_UNFOCUSED is about birth only.
    int64_t win = os64_gui_window_create("desktop", 0, 0, sw, sh,
                                         OS64_GUI_WINDOW_DESKTOP |
                                         OS64_GUI_WINDOW_NO_DECORATIONS |
                                         OS64_GUI_WINDOW_START_UNFOCUSED);
    if (win <= 0) {
        complain("desktop: window_create failed (%ld)\n", (long)win);
        return 1;
    }

    os64_draw_ctx_t ctx;
    if (os64_draw_ctx_init(&ctx, win) != 0) {
        complain("desktop: get_surface failed\n");
        os64_gui_window_destroy(win);
        return 1;
    }

    // ── the wallpaper ───────────────────────────────────────────────────────
    desktop_config_t cfg;
    os64_memset(&cfg, 0, sizeof(cfg));
    // Find, then read, and REPORT every way the read can fail (Codex #31
    // rd2/rd4): a config the operator wrote that silently paints the
    // built-in colour is a wallpaper that "was ignored for no reason".
    bool found = (os64_conf_find("desktop.conf", cfg.conf_path, sizeof(cfg.conf_path)) == 0);
    int64_t rc = found ? os64_conf_read(cfg.conf_path, desktop_conf_line, &cfg)
                       : OS64_CONF_NO_FILE;
    conf_report("desktop.conf", cfg.conf_path, found, rc);
    if (rc == OS64_CONF_TRUNCATED) {
        // Same rule as gui.conf below: a cut-short `image =` is a different
        // path. Truncated means the built-in ground, and it says so.
        complain("desktop: %s: too large to trust — using the built-in colour\n",
                     cfg.conf_path);
        cfg.have_color = false;
        cfg.have_image = false;
    }

    os64_image_t img;
    os64_memset(&img, 0, sizeof(img));
    if (cfg.have_image) {
        os64_image_status_t st = os64_image_load(cfg.image, 0, &img);
        if (st != OS64_IMAGE_OK) {
            // A wallpaper that silently fails to appear is a config bug with
            // no handle on it. Name the file and the reason, then carry on
            // with the color — a bad image must not cost you a desktop.
            complain("desktop: %s: %s\n", cfg.image,
                         os64_image_status_name(st));
            os64_memset(&img, 0, sizeof(img));
        }
    }
    paint(&ctx, &cfg, &img);

    // ── gui.conf: what starts with the desktop ──────────────────────────────
    startup_list_t list;
    os64_memset(&list, 0, sizeof(list));
    // The reader fills list.path with the winning file, so every complaint
    // from gui_conf_line can name it — "which gui.conf did it read?" is the
    // first question anyone asks (and /sys/conf answers it too).
    char *gui_path = list.path;
    // FIND, THEN READ, AS TWO STEPS (Codex #31 rd2) — because the two
    // answers mean different things here. os64_conf_find_read folds "no
    // gui.conf anywhere" and "found one, could not open it" into one
    // NO_FILE, and only the FIRST of those may fall through to the demo
    // pair below: a config that exists is the operator's word, and a
    // desktop that starts gbounce because that file failed to open would
    // be inventing a configuration nobody wrote.
    found = (os64_conf_find("gui.conf", gui_path, sizeof(list.path)) == 0);
    rc = found ? os64_conf_read(gui_path, gui_conf_line, &list)
               : OS64_CONF_NO_FILE;
    conf_report("gui.conf", gui_path, found, rc);
    if (found && rc == OS64_CONF_NO_FILE)
        rc = 0;   // an unreadable config is an EMPTY one, never an absent one
    // A TRUNCATED FILE STARTS NOTHING (Codex #31 rd5). The reader hands over
    // whatever fits, including the line the cut fell in the middle of — and
    // a `start` path cut short is a DIFFERENT path, one that may name a
    // program that exists. The kernel's reader refused an oversized gui.conf
    // outright; so does this one. Refusing the LIST rather than the last
    // line, because "which line was cut" is not something the callback can
    // see, and a half-obeyed startup file is worse to debug than an ignored
    // one that said why.
    if (rc == OS64_CONF_TRUNCATED) {
        complain("desktop: %s: too large to trust — starting nothing\n",
                     gui_path);
        list.count = 0;
    }
    if (list.overflowed)
        complain("desktop: more than %d start lines; the rest ignored\n",
                     DESKTOP_APPS_MAX);

    // NO gui.conf ANYWHERE means the built-in demo pair — the promise the
    // kernel's reader made and this one inherits: "an absent config file
    // must leave the machine behaving exactly as it did before the config
    // file existed", the same promise /etc/os64.conf makes about the search
    // path. A gui.conf that EXISTS and lists nothing still starts nothing:
    // that is how "start nothing" is spelled, and the distinction is the
    // whole reason this check is on NO_FILE and not on the count.
    if (rc == OS64_CONF_NO_FILE) {
        os64_strcopy(list.apps[0], DESKTOP_PATH_MAX, "/bin/gbounce");
        os64_strcopy(list.apps[1], DESKTOP_PATH_MAX, "/bin/gkeys");
        list.count = 2;
    }

    // The undertaker starts BEFORE the first child, so nothing can finish
    // unwatched. A desktop with no startup apps still wants it: the launcher
    // will spawn into the same lap.
    int64_t reap_thread = os64_thread(reaper, NULL);
    if (reap_thread < 0)
        complain(
                     "desktop: no reaper thread (%ld) — exited apps will linger\n",
                     (long)reap_thread);

    for (size_t i = 0; i < list.count; i++) {
        char *const av[] = { list.apps[i], NULL };
        int64_t child = os64_spawn(list.apps[i], av);
        if (child < 0)
            complain("desktop: could not start %s (%ld)\n",
                         list.apps[i], (long)child);
    }

    // ── the loop ────────────────────────────────────────────────────────────
    // Blocking, because a wallpaper does not animate. The desktop costs
    // exactly nothing until something happens to it.
    for (;;) {
        os64_gui_event_t ev;
        int64_t r = os64_gui_event_wait(win, &ev);
        if (r != 1)
            break;   // the window died, or we are being killed

        switch (ev.type) {
        case OS64_GUI_EVENT_WINDOW_RESIZE:
            paint(&ctx, &cfg, &img);
            break;
        case OS64_GUI_EVENT_WINDOW_CLOSE:
            // The WM declines Alt+F4 on the desktop, so this should not
            // arrive — but a close is a REQUEST and this program's answer is
            // no. Closing your desktop is not something a keystroke should
            // be able to do by accident.
            break;
        case OS64_GUI_EVENT_MOUSE_BUTTON_DOWN: {
            // A click on bare wallpaper: the launcher bound to that button
            // runs, told where the click was. THE DESKTOP DOES NOT DRAW A
            // MENU — it starts the program gui.conf named, which draws
            // whatever its own config says, and exits. That is the whole
            // seam: swap the program in gui.conf and the wallpaper click
            // means something else, with nothing here changing. Screen
            // coordinates and content coordinates are the same thing on a
            // desktop window (no chrome; its frame IS its canvas).
            uint8_t b = ev.mouse.button;
            if (b > OS64_GUI_MOUSE_MIDDLE || list.launcher[b][0] == 0)
                break;
            char xs[12], ys[12];
            os64_snprintf(xs, sizeof(xs), "%d", ev.mouse.x);
            os64_snprintf(ys, sizeof(ys), "%d", ev.mouse.y);
            char *const av[] = { list.launcher[b], xs, ys, NULL };
            int64_t child = os64_spawn(list.launcher[b], av);
            if (child < 0)
                complain("desktop: could not start launcher %s (%ld)\n",
                             list.launcher[b], (long)child);
            break;
        }
        default:
            // Keys, mouse motion, focus: nothing here listens to them.
            break;
        }
    }

    os64_gui_window_destroy(win);
    os64_image_free(&img);
    return 0;
}
