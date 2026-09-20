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
    /* Widget clipping uses measured glyphs. Cell-based truncation would cut
     * UTF-8 bytes and mismeasure a proportional interface face. */
    (void)pixels;
    os64_strcopy(dst, cap, src);
}

typedef struct {
    int row, button, nav, top, footer, min_w, min_h, width, height;
    int back_w, prev_w, next_w;
} center_layout_t;
static center_layout_t gLayout;
static bool gHaveLayout;
static int max_int(int a,int b) { return a>b?a:b; }
static os64_font_status_t measured_width(os64_ui_t *ui,const char *s,int *out)
{
    int32_t w;
    os64_font_status_t status=os64_ui_text_measure(ui,OS64_FONT_ROLE_UI,s,os64_strlen(s),&w);
    if(!status) *out=w+20;
    return status;
}
static os64_font_status_t measure_layout(os64_ui_t *ui,center_layout_t *m)
{
    *m=(center_layout_t){0};
    m->row=max_int(20,os64_ui_font_row_height(ui,OS64_FONT_ROLE_UI));
    m->button=max_int(36,m->row+12); m->nav=max_int(30,m->row+10);
    m->top=48+2*m->row; m->footer=m->nav+14+m->row+12;
    int title,subtitle;
    os64_font_status_t status;
    if((status=measured_width(ui,"Back",&m->back_w)) ||
       (status=measured_width(ui,"Previous",&m->prev_w)) ||
       (status=measured_width(ui,"Next",&m->next_w)) ||
       (status=measured_width(ui,"CONTROL CENTER",&title)) ||
       (status=measured_width(ui,"Your workspace, your choices.",&subtitle))) return status;
    m->min_w=max_int(360,48+max_int(max_int(title,subtitle),m->back_w+m->prev_w+m->next_w+20));
    m->min_h=max_int(250,m->top+m->button+12+m->footer);
    m->width=max_int((int)gCtx.surf.width,m->min_w);
    m->height=max_int((int)gCtx.surf.height,m->min_h);
    return OS64_FONT_OK;
}
static void place(os64_ui_widget_t *w,os64_gui_rect_t r,bool staged)
{ if(staged) os64_ui_widget_stage_bounds(w,r); else w->bounds=r; }
static void arrange(const center_layout_t *m,bool staged)
{
    int W=m->width,H=m->height;
    place(&gRoot,(os64_gui_rect_t){0,0,W,H},staged);
    place(&gTitle,(os64_gui_rect_t){24,20,W-48,m->row},staged);
    place(&gSubtitle,(os64_gui_rect_t){24,28+m->row,W-48,m->row},staged);
    unsigned page=(unsigned)max_int(1,(H-m->top-m->footer-12)/(m->button+10));
    if(page>ROWS) page=ROWS;
    for(unsigned i=0;i<ROWS;++i)
        place(&gRows[i],(os64_gui_rect_t){24,m->top+(int)i*(m->button+10),W-48,m->button},staged);
    int y=H-m->footer;
    place(&gBack,(os64_gui_rect_t){24,y,m->back_w,m->nav},staged);
    place(&gPrev,(os64_gui_rect_t){34+m->back_w,y,m->prev_w,m->nav},staged);
    place(&gNext,(os64_gui_rect_t){44+m->back_w+m->prev_w,y,m->next_w,m->nav},staged);
    place(&gStatus,(os64_gui_rect_t){24,H-m->row-12,W-48,m->row},staged);
    if(staged) return;
    gPageSize=page;
    int16_t n=gFirst; unsigned skipped=0;
    while(n>=0 && skipped<gOffset) {
        if(gMenu.nodes[n].kind!=OS64_MENU_SEPARATOR) ++skipped;
        n=gMenu.nodes[n].next;
    }
    for(unsigned i=0;i<ROWS;++i) {
        while(n>=0 && gMenu.nodes[n].kind==OS64_MENU_SEPARATOR) n=gMenu.nodes[n].next;
        gNodes[i]=-1;
        os64_ui_set_hidden(&gUi, &gRows[i], i>=page || n<0);
        if(gRows[i].hidden) continue;
        gNodes[i]=n;
        fit_text(gCaptions[i],sizeof(gCaptions[i]),gMenu.nodes[n].label,W-80);
        n=gMenu.nodes[n].next;
    }
    while(n>=0 && gMenu.nodes[n].kind==OS64_MENU_SEPARATOR) n=gMenu.nodes[n].next;
    gMore=n>=0;
    os64_ui_set_hidden(&gUi, &gBack, gDepth==0);
    os64_ui_set_hidden(&gUi, &gPrev, gOffset==0);
    os64_ui_set_hidden(&gUi, &gNext, !gMore);
    fit_text(gStatusText,sizeof(gStatusText),gMessage,W-48);
    os64_ui_mark_dirty(&gUi,&gRoot);
}
static void layout(os64_ui_t *ui)
{
    if(!gHaveLayout) {
        if(measure_layout(ui,&gLayout)) return;
        gHaveLayout=true;
    }
    center_layout_t m=gLayout;
    m.width=(int)gCtx.surf.width; m.height=(int)gCtx.surf.height;
    arrange(&m,false);
}
static os64_font_status_t plan_font(os64_ui_t *ui,void *user,void **out)
{
    (void)user; *out=NULL;
    center_layout_t *m=os64_malloc(sizeof(*m));
    if(!m) return OS64_FONT_NO_MEMORY;
    os64_font_status_t status=measure_layout(ui,m);
    if(!status && (m->width>(int)gCtx.surf.width || m->height>(int)gCtx.surf.height)) {
        uint32_t sw,sh; os64_gui_window_state_t state;
        if(gCtx.win<=0 || os64_gui_screen_info(&sw,&sh) || os64_gui_window_get_state(gCtx.win,&state)) status=OS64_FONT_LIMIT;
        else if(max_int(0,state.x)+(int64_t)m->width+state.width-gCtx.surf.width>sw ||
                max_int(0,state.y)+(int64_t)m->height+state.height-gCtx.surf.height>sh) status=OS64_FONT_LIMIT;
    }
    if(status) { os64_free(m); return status; }
    arrange(m,true); *out=m; return OS64_FONT_OK;
}
static void discard_font(os64_ui_t *ui,void *user,void *plan)
{ (void)ui; (void)user; os64_free(plan); }
static void commit_font(os64_ui_t *ui,void *user,void *plan)
{
    (void)user; center_layout_t *m=plan;
    // The plan fits the live window's reservation; this minimum needs no
    // allocation. Re-fetch the surface if setting it grows the window.
    if(gCtx.win>0 && (os64_gui_window_set_min_size(gCtx.win,m->min_w,m->min_h) ||
                     os64_draw_ctx_refresh(&gCtx))) {
        os64_complain("controlcenter: cannot update window minimum"); ui->quit=true;
    } else {
        gLayout=*m; gHaveLayout=true;
        m->width=(int)gCtx.surf.width; m->height=(int)gCtx.surf.height;
        arrange(m,false);
        os64_printf("controlcenter: interface row %d, minimum %dx%d\n",m->row,m->min_w,m->min_h);
    }
    os64_free(m);
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    int64_t win = os64_gui_window_create("Control Center", 64, 112, 468, 442, 0);
    if (win <= 0) {
        os64_complain("controlcenter: cannot create window (%ld)\n", (long)win);
        return 1;
    }
    // Keep the builtin layout usable even if initial configured-font adoption
    // is refused. Successful adoption replaces this floor with its own plan.
    if (os64_gui_window_set_min_size(win, 360, 250) != 0) {
        os64_complain("controlcenter: cannot set window minimum\n");
        os64_gui_window_destroy(win);
        return 1;
    }
    if (os64_draw_ctx_init(&gCtx, win) != 0) {
        os64_gui_window_destroy(win);
        return 1;
    }
    os64_ui_init(&gUi, &gCtx);
    os64_ui_theme_defaults(&gUi.theme);
    os64_ui_theme_palette(&gUi.theme, OS64_UI_PALETTE_MIDNIGHT);
    gUi.appearance_generation = 0;
    os64_ui_theme_current(&gUi.theme, &gUi.appearance_generation);
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
    (void)os64_ui_font_planner(&gUi,plan_font,commit_font,discard_font,NULL);
    layout(&gUi);
    os64_ui_run(&gUi, win, NULL);
    os64_ui_font_release(&gUi);
    os64_gui_window_destroy(win);
    return 0;
}
