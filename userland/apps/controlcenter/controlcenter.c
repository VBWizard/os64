// Control Center presents the configured "settings" menu as a persistent
// launcher. Each tool is an ordinary application with its own lifetime.
#include "os64/os64.h"
#include "os64/ui.h"
#include "os64/menu.h"

#define ROWS 5
#define DEPTH 16

static os64_ui_t gUi;
static os64_draw_ctx_t gCtx;
static os64_ui_widget_t gRoot, gTitle, gSubtitle, gRows[ROWS];
static os64_ui_widget_t gBack, gPrev, gNext, gStatus;
static os64_menu_t gMenu;
static int16_t gFirst = -1, gNodes[ROWS], gParents[DEPTH];
static unsigned gDepth, gOffset, gPageSize;
static bool gMore;
static char gCaptions[ROWS][OS64_MENU_LABEL_MAX];
static char gMessage[OS64_MENU_ERR_MAX], gStatusText[OS64_MENU_ERR_MAX];

static void layout(os64_ui_t *ui);

static void resized(os64_ui_t *ui)
{
    if (ui->grab) ui->grab->pressed = false;
    ui->grab = NULL;
    layout(ui);
}

static void message(const char *s)
{
    os64_strcopy(gMessage, sizeof(gMessage), s);
    layout(&gUi);
}

static int64_t reap_children(void *arg)
{
    (void)arg;
    for (;;) {
        int32_t code;
        int64_t pid = os64_wait(0, &code);
        if (pid > 0) {
            if (code != 0)
                os64_complain("controlcenter: tool task %ld exited (%d)\n", (long)pid, code);
        } else if (pid != OS64_INTERRUPTED) {
            os64_sleep(1000);
        }
    }
    return 0;
}

static void open_tool(os64_ui_widget_t *w, void *user)
{
    (void)w;
    unsigned row = (unsigned)(uintptr_t)user;
    if (row >= ROWS || gNodes[row] < 0) return;
    const os64_menu_node_t *node = &gMenu.nodes[gNodes[row]];
    if (node->kind == OS64_MENU_SUBMENU) {
        if (gDepth == DEPTH) {
            message("This category is nested too deeply.");
            return;
        }
        gParents[gDepth++] = gFirst;
        gFirst = node->first_child;
        gOffset = 0;
        message("Choose a tool. Each opens in its own window.");
        return;
    }
    char buf[OS64_MENU_COMMAND_MAX];
    char *argv[64];
    int64_t argc = os64_menu_argv(node->command, buf, sizeof(buf), argv, 64);
    if (argc <= 0) {
        message("Cannot open tool: invalid command. See VT1 for details.");
        os64_complain("controlcenter: invalid command for '%s': %s\n",
                      node->label, node->command);
        return;
    }
    int64_t pid = os64_spawn(argv[0], argv);
    if (pid < 0) {
        os64_snprintf(gMessage, sizeof(gMessage), "Cannot open %s (%ld).",
                      node->label, (long)pid);
        os64_complain("controlcenter: cannot spawn '%s' (%ld)\n", argv[0], (long)pid);
    } else {
        os64_snprintf(gMessage, sizeof(gMessage), "Opened %s.", node->label);
    }
    layout(&gUi);
}

static void back(os64_ui_widget_t *w, void *user)
{
    (void)w; (void)user;
    if (gDepth) {
        gFirst = gParents[--gDepth];
        gOffset = 0;
        message("Choose a tool. Each opens in its own window.");
    }
}

static void previous(os64_ui_widget_t *w, void *user)
{
    (void)w; (void)user;
    gOffset = gOffset > gPageSize ? gOffset - gPageSize : 0;
    layout(&gUi);
}

static void next(os64_ui_widget_t *w, void *user)
{
    (void)w; (void)user;
    if (gMore) gOffset += gPageSize;
    layout(&gUi);
}

static void fit_text(char *dst, size_t cap, const char *src, int pixels)
{
    size_t cells = pixels > 0 ? (size_t)pixels / 8 : 0;
    if (cells >= cap) cells = cap - 1;
    os64_strcopy(dst, cells + 1, src);
    if (os64_strlen(src) > cells && cells >= 3) {
        dst[cells - 3] = '.';
        dst[cells - 2] = '.';
        dst[cells - 1] = '.';
    }
}

static void layout(os64_ui_t *ui)
{
    int width = (int)gCtx.surf.width, height = (int)gCtx.surf.height;
    gRoot.bounds = (os64_gui_rect_t){0, 0, width, height};
    bool small = width < 360 || height < 250;
    for (os64_ui_widget_t *w = gRoot.first_child; w; w = w->next_sibling)
        w->hidden = small;
    gStatus.hidden = false;
    if (small) {
        gStatus.bounds = (os64_gui_rect_t){0, 0, width, height};
        fit_text(gStatusText, sizeof(gStatusText), "Enlarge this window.", width);
        os64_ui_mark_dirty(ui, &gRoot);
        return;
    }
    gTitle.bounds = (os64_gui_rect_t){24, 20, width - 48, 24};
    gSubtitle.bounds = (os64_gui_rect_t){24, 49, width - 48, 20};
    gPageSize = (unsigned)(height - 174) / 46;
    if (gPageSize > ROWS) gPageSize = ROWS;
    int16_t n = gFirst;
    unsigned skipped = 0;
    while (n >= 0 && skipped < gOffset) {
        if (gMenu.nodes[n].kind != OS64_MENU_SEPARATOR) skipped++;
        n = gMenu.nodes[n].next;
    }
    for (unsigned i = 0; i < ROWS; ++i) {
        while (n >= 0 && gMenu.nodes[n].kind == OS64_MENU_SEPARATOR)
            n = gMenu.nodes[n].next;
        gNodes[i] = -1;
        gRows[i].hidden = i >= gPageSize || n < 0;
        if (gRows[i].hidden) continue;
        gNodes[i] = n;
        gRows[i].bounds = (os64_gui_rect_t){24, 92 + (int)i * 46, width - 48, 36};
        fit_text(gCaptions[i], sizeof(gCaptions[i]), gMenu.nodes[n].label, width - 80);
        n = gMenu.nodes[n].next;
    }
    while (n >= 0 && gMenu.nodes[n].kind == OS64_MENU_SEPARATOR)
        n = gMenu.nodes[n].next;
    gMore = n >= 0;
    gBack.hidden = gDepth == 0;
    gPrev.hidden = gOffset == 0;
    gNext.hidden = !gMore;
    gBack.bounds = (os64_gui_rect_t){24, height - 78, 90, 30};
    gPrev.bounds = (os64_gui_rect_t){124, height - 78, 90, 30};
    gNext.bounds = (os64_gui_rect_t){224, height - 78, 90, 30};
    gStatus.bounds = (os64_gui_rect_t){24, height - 32, width - 48, 20};
    fit_text(gStatusText, sizeof(gStatusText), gMessage, width - 48);
    os64_ui_mark_dirty(ui, &gRoot);
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    int64_t win = os64_gui_window_create("Control Center", 64, 112, 468, 442, 0);
    if (win <= 0) {
        os64_complain("controlcenter: cannot create window (%ld)\n", (long)win);
        return 1;
    }
    if (os64_draw_ctx_init(&gCtx, win) != 0) {
        os64_gui_window_destroy(win);
        return 1;
    }
    os64_ui_init(&gUi, &gCtx);
    os64_ui_theme_defaults(&gUi.theme);
    os64_ui_theme_palette(&gUi.theme, OS64_UI_PALETTE_MIDNIGHT);
    os64_ui_theme_read_startup(&gUi.theme);
    gUi.appearance_generation = 0;
    os64_ui_theme_session(&gUi.theme, &gUi.appearance_generation, 0);
    gUi.on_resize = resized;
    os64_ui_panel(&gRoot);
    os64_ui_set_root(&gUi, &gRoot);
    os64_ui_label(&gTitle, "CONTROL CENTER");
    os64_ui_label(&gSubtitle, "Your workspace, your choices.");
    os64_ui_label(&gStatus, gStatusText);
    os64_ui_add_child(&gRoot, &gTitle);
    os64_ui_add_child(&gRoot, &gSubtitle);
    os64_ui_add_child(&gRoot, &gStatus);
    for (unsigned i = 0; i < ROWS; ++i) {
        os64_ui_button(&gRows[i], gCaptions[i], open_tool, (void *)(uintptr_t)i);
        os64_ui_add_child(&gRoot, &gRows[i]);
    }
    os64_ui_button(&gBack, "Back", back, NULL);
    os64_ui_button(&gPrev, "Previous", previous, NULL);
    os64_ui_button(&gNext, "Next", next, NULL);
    os64_ui_add_child(&gRoot, &gBack);
    os64_ui_add_child(&gRoot, &gPrev);
    os64_ui_add_child(&gRoot, &gNext);

    os64_menu_status_t status = os64_menu_load(&gMenu, "menu.conf", gMessage, sizeof(gMessage));
    if (status != OS64_MENU_OK) {
        os64_complain("controlcenter: %s\n", gMessage);
    } else if (!os64_menu_named_exists(&gMenu, "settings")) {
        os64_strcopy(gMessage, sizeof(gMessage), "No settings menu in menu.conf.");
    } else {
        gFirst = os64_menu_find(&gMenu, "settings");
        os64_strcopy(gMessage, sizeof(gMessage), gFirst < 0 ? "The settings menu is empty." :
                     "Choose a tool to open.");
    }
    // A persistent launcher collects exited children while its UI is idle.
    // On its own exit the kernel reparents surviving tools for auto-reaping.
    if (os64_thread(reap_children, NULL) < 0) {
        gFirst = -1;
        os64_strcopy(gMessage, sizeof(gMessage), "Cannot start tool reaper; launching disabled.");
    }
    layout(&gUi);
    os64_ui_run(&gUi, win, NULL);
    os64_gui_window_destroy(win);
    return 0;
}
