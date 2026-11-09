/* Production Scribe/grootmenu integration with real fonts, editor/file fixtures,
 * and deterministic native window queues. */
#define SCRIBE_MENU_HOST
#include "test_scribe_host.c"
#include "ui_menu_internal.h"
#define QUEUE 512
struct window_fixture {
    bool live;
    int64_t owner;
    os64_gui_rect_t rect;
    os64_gui_surface_t surface;
    os64_gui_event_t queue[QUEUE];
    size_t head,tail;
};
static struct {
    struct window_fixture windows[33];
    uint32_t sw,sh;
    int64_t focused;
    unsigned creates,destroys,owner_down,owner_up,owner_wheel,peer_down,invocations;
    int64_t fail_destroy;
    bool fail_create;
    os64_ui_dispatcher_t *dispatcher;
} transport;
static void queue(int64_t h,os64_gui_event_t e)
{
    CHECK(h>0&&h<33&&transport.windows[h].live);
    struct window_fixture *w=&transport.windows[h]; CHECK(w->tail-w->head<QUEUE);
    w->queue[w->tail++%QUEUE]=e;
}
static os64_gui_event_t focus_edge(bool gain,int64_t other)
{
    os64_gui_event_t e={.type=OS64_GUI_EVENT_WINDOW_FOCUS};
    e.focus.gained=gain;e.focus.sibling=other!=0;e.focus.other_handle=(uint16_t)other;return e;
}

static int64_t backend_poll(void *u,int64_t h,os64_gui_event_t *e)
{
    (void)u; if(!transport.windows[h].live) return -1;
    struct window_fixture *w=&transport.windows[h]; if(w->head==w->tail) return 0;
    *e=w->queue[w->head++%QUEUE];return 1;
}
static int64_t backend_wait(void *u,int64_t *h)
{
    (void)u;
    for(int i=1;i<33;++i) if(transport.windows[i].live&&transport.windows[i].head<transport.windows[i].tail) { *h=i;return 1; }
    return -1;
}
static int64_t backend_destroy(void *u,int64_t h)
{
    (void)u;
    if(h==transport.fail_destroy) return OS64_GUI_ERR_BUSY;
    for(int i=1;i<33;++i) if(transport.windows[i].live&&transport.windows[i].owner==h) return OS64_GUI_ERR_BUSY;
    struct window_fixture *w=&transport.windows[h];
    CHECK(w->live);
    if(transport.focused==h&&w->owner) { transport.focused=w->owner;queue(w->owner,focus_edge(true,h)); }
    free(w->surface.pixels); *w=(struct window_fixture){0};++transport.destroys;return 0;
}
static int64_t backend_create(void *u,int64_t parent,os64_gui_rect_t r)
{
    (void)u;CHECK(os64_ui_dispatcher_can_create(transport.dispatcher));
    if(transport.fail_create) return OS64_GUI_ERR_NO_RESOURCES;
    if(parent&&transport.focused!=parent) return OS64_GUI_ERR_NOT_FOCUSED;
    for(int64_t h=1;h<33;++h) if(!transport.windows[h].live) {
        struct window_fixture *w=&transport.windows[h];
        *w=(struct window_fixture){.live=true,.owner=parent,.rect=r};
        w->surface=(os64_gui_surface_t){calloc((size_t)r.w*r.h,sizeof(uint32_t)),(uint32_t)r.w,(uint32_t)r.h,(uint32_t)r.w};
        if(transport.focused) queue(transport.focused,focus_edge(false,h));
        queue(h,focus_edge(true,transport.focused));
        transport.focused=h;++transport.creates;return h;
    }
    return OS64_GUI_ERR_NO_RESOURCES;
}
static int64_t backend_rect(void *u,int64_t h,os64_gui_rect_t *r)
{ (void)u;if(!transport.windows[h].live)return -1;*r=transport.windows[h].rect;return 0; }
static int64_t backend_surface(void *u,int64_t h,os64_gui_surface_t *s)
{ (void)u;*s=transport.windows[h].surface;return 0; }
static int64_t backend_screen(void *u,uint32_t *w,uint32_t *h)
{ (void)u;*w=transport.sw;*h=transport.sh;return 0; }
static void backend_publish(void *u,int64_t h,os64_gui_rect_t r) { (void)u;(void)h;(void)r; }
static os64_ui_window_backend_t backend={NULL,backend_poll,backend_wait,backend_destroy,backend_create,backend_rect,backend_surface,backend_screen,backend_publish};

static void menu_drain(void)
{
    for (unsigned i=0;i<40;++i) {
        int64_t n=os64_ui_dispatcher_turn(transport.dispatcher,false);
        CHECK(n>=0);
        if (n<=0 && os64_ui_dispatcher_can_create(transport.dispatcher)) return;
    }
    CHECK(false);
}
static void menu_key(char ascii, uint8_t scan, uint8_t mods)
{
    os64_gui_event_t e={.type=OS64_GUI_EVENT_KEY_DOWN};
    e.key.ascii=ascii; e.key.scancode=scan; e.key.modifiers=mods;
    queue(transport.focused,e); menu_drain();
    e.type=OS64_GUI_EVENT_KEY_UP;
    queue(transport.focused,e); menu_drain();
}
static void owner_no_paint(void *user) { (void)user; }
static void menu_begin(void)
{
    sh_scribe(16,"document text\n",14);
    memset(&transport,0,sizeof(transport));
    transport.sw=1024; transport.sh=768; transport.focused=1;
    transport.windows[1]=(struct window_fixture){.live=true,.rect={40,40,SH_W,SH_H}};
    g.win=1; g.running=true;
    CHECK(attach_menus(&backend)); transport.dispatcher=&g.dispatcher;
    /* The owner canvas is rendered explicitly; popup publishes use the
     * injected transport and never issue a host syscall. */
    g.dispatcher.entries[0].paint=owner_no_paint;
    g.dispatcher.entries[0].ui=&g.ui;
}
static void menu_end(void)
{
    CHECK(scribe_dispose());
    CHECK(os64_ui_dispatcher_destroy(&g.dispatcher,1)==0);
    for (unsigned i=1;i<33;++i) CHECK(!transport.windows[i].live);
}
static void menu_open_on(os64_ui_widget_t *w)
{
    os64_gui_event_t e={.type=OS64_GUI_EVENT_KEY_DOWN};
    e.key.scancode=0x44; e.key.modifiers=OS64_GUI_MOD_SHIFT;
    CHECK(context_menu(w,&e,NULL)); menu_drain();
    CHECK(os64_ui_menu_active(g.menus));
}
static void scribe_menu_targets(void)
{
    current="Scribe menu targets"; menu_begin();
    os64_ui_textview_select(&g.ui,&g.view,0,0,0,8);
    size_t caret=g.view.cur_col;
    os64_gui_event_t right={.type=OS64_GUI_EVENT_MOUSE_BUTTON_DOWN};
    right.mouse.x=g.view.w.bounds.x+80; right.mouse.y=g.view.w.bounds.y+8; right.mouse.button=1;
    queue(1,right); menu_drain();
    CHECK(os64_ui_menu_active(g.menus));
    CHECK(g.view.cur_col==caret && g.view.sel);
    menu_key(3,0x2e,OS64_GUI_MOD_CTRL);
    CHECK(!os64_ui_menu_active(g.menus));
    sh_file_t *clip=sh_find(OS64_CLIPBOARD_PATH,false);
    CHECK(clip && clip->len==8 && !memcmp(clip->bytes,"document",8));

    enter_mode(MODE_FIND,"find:","field words");
    g.field.anchor=0; g.field.cursor=5; g.field.selected=true;
    action_target(); menu_open_on(&g.field.w);
    // Changing owner focus cannot change the target saved by the menu.
    os64_ui_set_focus(&g.ui,&g.view.w);
    menu_key(24,0x2d,OS64_GUI_MOD_CTRL);
    CHECK(!strcmp(g.field_buf," words"));
    CHECK(SH_DOC_IS("document text\n"));
    clip=sh_find(OS64_CLIPBOARD_PATH,false);
    CHECK(clip && clip->len==5 && !memcmp(clip->bytes,"field",5));

    // The bar saves the last editing target before it takes keyboard focus.
    os64_ui_set_focus(&g.ui,&g.field.w); action_target();
    g.field.anchor=0; g.field.cursor=g.field.len; g.field.selected=true;
    menu_key(0,0x44,0); CHECK(os64_ui_menu_active(g.menus));
    menu_key(22,0x2f,OS64_GUI_MOD_CTRL);
    CHECK(!strcmp(g.field_buf,"field"));
    CHECK(SH_DOC_IS("document text\n"));
    menu_end(); sh_rm(OS64_CLIPBOARD_PATH);
}
static void scribe_menu_visibility_and_help(void)
{
    current="Scribe menu visibility and help"; menu_begin();
    int32_t top=g.view.w.bounds.y,height=g.view.w.bounds.h;
    CHECK(os64_ui_action_invoke(g.action_views[0],ACT_BAR).status==OS64_UI_OK);
    CHECK(os64_ui_action_invoke(g.action_views[0],ACT_STATUS).status==OS64_UI_OK);
    CHECK(g.bar.w.hidden && g.status.w.hidden && g.view.w.bounds.y<top && g.view.w.bounds.h>height);
    int32_t hidden_top=g.view.w.bounds.y;
    menu_key(0,0x44,0);
    CHECK(g.bar.temporary && !g.bar.w.hidden && g.view.w.bounds.y==top);
    menu_key(27,1,0);
    CHECK(!os64_ui_menu_active(g.menus) && g.bar.w.hidden && g.view.w.bounds.y==hidden_top);
    menu_open_on(&g.view.w);
    menu_key('m',0x32,0);
    CHECK(!g.bar.w.hidden && !os64_ui_menu_active(g.menus));
    menu_key(7,0x22,OS64_GUI_MOD_CTRL); CHECK(g.help_active);
    menu_open_on(&g.view.w);
    menu_key(27,1,0); CHECK(g.help_active);
    menu_key(27,1,0); CHECK(!g.help_active);
    menu_end();
}
static void check_scroll_viewport(void)
{
    int32_t rows=os64_ui_textview_rows(&g.view,&g.ui.theme);
    CHECK(g.scroll.visible==rows);
    CHECK(g.hscroll.visible==os64_ui_textview_width(&g.view));
    // Dragging to the end must use the current viewport, without an edit first.
    g.scroll.pos=g.scroll.total-g.scroll.visible;
    scrolled(&g.scroll,NULL);
    CHECK(g.view.top==(size_t)(g.scroll.total-rows));
}
static void scribe_chrome_scrollbars(void)
{
    current="Scribe chrome scrollbars"; menu_begin();
    char doc[1600]; size_t n=0;
    for(unsigned i=0;i<100;++i) n+=(size_t)snprintf(doc+n,sizeof(doc)-n,"line %u\n",i);
    sh_put("/long",doc,n); do_load("/long");
    check_scroll_viewport();
    CHECK(os64_ui_action_invoke(g.action_views[0],ACT_BAR).status==OS64_UI_OK);
    check_scroll_viewport();
    CHECK(os64_ui_action_invoke(g.action_views[0],ACT_STATUS).status==OS64_UI_OK);
    check_scroll_viewport();
    menu_key(0,0x44,0); CHECK(g.bar.temporary); check_scroll_viewport();
    menu_key(27,1,0); CHECK(g.bar.w.hidden); check_scroll_viewport();
    CHECK(os64_ui_action_invoke(g.action_views[0],ACT_BAR).status==OS64_UI_OK);
    check_scroll_viewport();
    CHECK(os64_ui_action_invoke(g.action_views[0],ACT_STATUS).status==OS64_UI_OK);
    check_scroll_viewport();
    enter_mode(MODE_FIND,"find:",""); check_scroll_viewport();
    field_cancel(&g.field,NULL); check_scroll_viewport();
    g.root.bounds.w-=80; g.root.bounds.h-=30;
    on_resize(&g.ui); check_scroll_viewport();
    menu_end(); sh_rm("/long");
}
static void scribe_menu_file_picker(void)
{
    current="Scribe menu file picker"; menu_begin();
    menu_key('f',0x21,OS64_GUI_MOD_ALT);
    CHECK(os64_ui_menu_active(g.menus));
    menu_key('o',0x18,0);
    CHECK(!os64_ui_menu_active(g.menus) && g.file_operation==FILE_OPEN);
    CHECK(transport.creates==transport.destroys);
    file_step();
    CHECK(g.file_dialog.window>0 && g.root.disabled && g.bar.w.disabled);
    menu_key(0,0x44,0); // F10 cannot reopen menus while choosing
    CHECK(!os64_ui_menu_active(g.menus));
    sh_dialog_finish(NULL,false);
    CHECK(!g.root.disabled && !g.bar.w.disabled && SH_DOC_IS("document text\n"));
    menu_key('f',0x21,OS64_GUI_MOD_ALT); menu_key('a',0x1e,0);
    CHECK(g.file_operation==FILE_SAVE && !os64_ui_menu_active(g.menus));
    file_step(); sh_dialog_finish(NULL,false);
    menu_end();
}
static void scribe_build_refusals(void)
{
    current="Scribe initialization refusals";
    for (long k=0;k<160;++k) {
        memset(&g,0,sizeof(g));
        g.ctx.surf=(os64_gui_surface_t){sh_px,SH_W,SH_H,SH_W};
        deny_countdown=k;
        os64_ui_init(&g.ui, &g.ctx);
        (void)scribe_build();
        deny_countdown=-1;
        CHECK(scribe_dispose());
    }
}
static void scribe_menu_quit_and_cleanup(void)
{
    current="Scribe menu quit and cleanup"; menu_begin();
    sh_key('!',0); CHECK(g.buf.dirty);
    menu_open_on(&g.view.w);
    menu_key(17,0x10,OS64_GUI_MOD_CTRL);
    CHECK(g.mode==MODE_QUITSAVE && g.running && !os64_ui_menu_active(g.menus));
    menu_key(17,0x10,OS64_GUI_MOD_CTRL);
    CHECK(!g.running);
    menu_end();
    menu_begin();
    for (unsigned i=0;i<32;++i) {
        menu_open_on(&g.view.w); menu_key(27,1,0);
        CHECK(transport.creates==transport.destroys);
    }
    menu_open_on(&g.view.w);
    transport.fail_destroy=transport.focused;
    CHECK(!scribe_dispose()); CHECK(g.menus && g.actions);
    transport.fail_destroy=0;
    menu_end();
}
#define GROOTMENU_HOST
#include "../userland/apps/grootmenu/grootmenu.c"
#include "os64/slurp.h"
static const char *menu_config;
static unsigned spawned, complaints;
static char spawned_path[128], spawned_arg[128];
int64_t os64_conf_find(const char *name,char *out,size_t cap)
{
    (void)name;
    if (!menu_config) return -1;
    os64_strcopy(out,cap,"/home/menu.conf"); return 0;
}
os64_slurp_status_t os64_slurp(const char *path,size_t cap,uint8_t **out,size_t *len)
{
    (void)path; *out=NULL; *len=0;
    if (!menu_config) return OS64_SLURP_NO_FILE;
    size_t n=strlen(menu_config);
    if (n>cap) return OS64_SLURP_TOO_BIG;
    *out=os64_malloc(n+1);
    if (!*out) return OS64_SLURP_NO_MEMORY;
    memcpy(*out,menu_config,n+1); *len=n; return OS64_SLURP_OK;
}
void os64_complain(const char *fmt,...) { (void)fmt; ++complaints; }
int64_t os64_spawn(const char *path,char *const argv[])
{
    CHECK(!os64_ui_menu_active(session));
    ++spawned;
    os64_strcopy(spawned_path,sizeof(spawned_path),path);
    os64_strcopy(spawned_arg,sizeof(spawned_arg),argv[1]?argv[1]:"");
    return 100;
}
static os64_menu_status_t load_config(const char *text)
{
    menu_config=text;
    char err[OS64_MENU_ERR_MAX];
    return os64_menu_load(&gMenu,"menu.conf",err,sizeof(err));
}
static void grootmenu_models_and_launch(void)
{
    current="grootmenu adapter";
    (void)grootmenu_main;
    CHECK(os64_streq(command_icon("/bin/scribe /home/notes"),"file"));
    CHECK(os64_streq(command_icon("/bin/appearance"),"settings"));
    CHECK(os64_streq(command_icon("/bin/gterm /bin/appearance"),"app"));
    CHECK(os64_streq(command_icon("/bin/scribe-helper"),"app"));
    CHECK(load_config(NULL)==OS64_MENU_NO_FILE);
    CHECK(load_config("menu root {\n menu Missing nope\n}\n")==OS64_MENU_UNRESOLVED);
    CHECK(load_config("menu root {\n}\n")==OS64_MENU_OK);
    CHECK(os64_menu_named_exists(&gMenu,"root") && os64_menu_find(&gMenu,"root")==-1);
    CHECK(!os64_menu_named_exists(&gMenu,"absent"));
    CHECK(load_config("menu 123 {\n item Notes /bin/scribe \"/home/my notes.txt\"\n}\n")==OS64_MENU_OK);
    int16_t first=os64_menu_find(&gMenu,"123");
    os64_ui_init(&ui,NULL);
    CHECK(build(first)==OS64_UI_OK);
    CHECK(ui_test_adopt(&ui,sh_dir,24)==OS64_FONT_OK);
    memset(&transport,0,sizeof(transport)); transport.sw=1024; transport.sh=768;
    os64_ui_dispatcher_init(&dispatcher,&backend); transport.dispatcher=&dispatcher;
    CHECK(os64_ui_menu_session_create(&dispatcher,&ui,0,NULL,NULL,NULL,&session)==OS64_UI_OK);
    CHECK(os64_ui_menu_open(session,models[first],view,(os64_gui_rect_t){1010,750,0,0},OS64_UI_MENU_CONTEXT)==OS64_UI_OK);
    menu_drain();
    CHECK(transport.windows[transport.focused].owner==0);
    menu_key('\r',0x1c,0);
    CHECK(spawned==1 && !strcmp(spawned_path,"/bin/scribe") && !strcmp(spawned_arg,"/home/my notes.txt"));
    CHECK(dispose()); CHECK(transport.creates==transport.destroys);

    CHECK(load_config("menu root {\n menu Loop root\n item Fine /bin/gclock\n}\n")==OS64_MENU_OK);
    os64_ui_init(&ui,NULL);
    CHECK(build(os64_menu_find(&gMenu,"root"))==OS64_UI_INVALID); CHECK(dispose());
    char text[8192]; size_t at=0;
    at+=(size_t)snprintf(text+at,sizeof(text)-at,"menu root {\n");
    for(unsigned i=0;i<65;++i) at+=(size_t)snprintf(text+at,sizeof(text)-at,"item Row%u /bin/gclock\n",i);
    snprintf(text+at,sizeof(text)-at,"}\n");
    CHECK(load_config(text)==OS64_MENU_OK);
    os64_ui_init(&ui,NULL); CHECK(build(0)==OS64_UI_LIMIT); CHECK(dispose());

    // The parser's maximum accepted inline depth is also renderable.
    at=(size_t)snprintf(text,sizeof(text),"menu root {\n");
    for(unsigned i=1;i<OS64_MENU_DEPTH_MAX;++i) at+=(size_t)snprintf(text+at,sizeof(text)-at,"menu Level%u {\n",i);
    at+=(size_t)snprintf(text+at,sizeof(text)-at,"item Clock /bin/gclock\n");
    for(unsigned i=0;i<OS64_MENU_DEPTH_MAX;++i) at+=(size_t)snprintf(text+at,sizeof(text)-at,"}\n");
    CHECK(load_config(text)==OS64_MENU_OK);
    os64_ui_init(&ui,NULL); CHECK(build(0)==OS64_UI_OK); CHECK(dispose());

    // An unavailable child window reports the refusal and keeps the root
    // usable, matching the launcher's existing cascade failure policy.
    CHECK(load_config("menu root {\n menu Tools {\n item Clock /bin/gclock\n}\n}\n")==OS64_MENU_OK);
    os64_ui_init(&ui,NULL); CHECK(build(0)==OS64_UI_OK);
    memset(&transport,0,sizeof(transport)); transport.sw=1024; transport.sh=768;
    os64_ui_dispatcher_init(&dispatcher,&backend); transport.dispatcher=&dispatcher;
    CHECK(os64_ui_menu_session_create(&dispatcher,&ui,0,NULL,NULL,NULL,&session)==OS64_UI_OK);
    os64_ui_menu_hooks(session,NULL,launcher_error,NULL);
    CHECK(os64_ui_menu_open(session,models[0],view,(os64_gui_rect_t){10,10,0,0},OS64_UI_MENU_CONTEXT)==OS64_UI_OK);
    menu_drain(); transport.fail_create=true;
    unsigned errors=complaints;
    menu_key('\r',0x1c,0);
    CHECK(complaints==errors+1 && !failed && os64_ui_menu_active(session));
    CHECK(session->depth==1);
    transport.fail_create=false; CHECK(dispose());

    // Each owning allocation can refuse without leaking partial graphs.
    CHECK(load_config("menu root {\n menu Tools tools\n menu Again tools\n}\nmenu tools {\n item Clock /bin/gclock\n}\n")==OS64_MENU_OK);
    for(long k=0;k<45;++k) {
        os64_ui_init(&ui,NULL); deny_countdown=k;
        os64_ui_status_code_t rc=build(0); deny_countdown=-1;
        CHECK(rc==OS64_UI_OK || rc==OS64_UI_NO_MEMORY);
        CHECK(dispose());
    }
    os64_menu_node_t bad={0}; strcpy(bad.label,"Bad command");
    strcpy(bad.command,"/bin/a b c d e f g h i j k l m n o p");
    unsigned before=spawned,old=complaints;
    launch(&bad); CHECK(spawned==before && complaints==old+1);
}

static void grootmenu_empty_cascades(void)
{
    current="grootmenu empty cascades";
    const char *configs[]={
        "menu root {\n menu Empty {\n }\n item Clock /bin/gclock\n}\n",
        "menu root {\n menu Empty empty\n item Clock /bin/gclock\n}\nmenu empty {\n}\n",
        "menu root {\n menu Empty empty\n menu Again empty\n item Clock /bin/gclock\n}\nmenu empty {\n menu Nested {\n }\n}\n",
        "menu root {\n menu Empty {\n separator\n }\n item Clock /bin/gclock\n}\n"
    };
    for(size_t i=0;i<sizeof(configs)/sizeof(configs[0]);++i) {
        CHECK(load_config(configs[i])==OS64_MENU_OK);
        int16_t first=os64_menu_find(&gMenu,"root");
        os64_ui_init(&ui,NULL); CHECK(build(first)==OS64_UI_OK);
        CHECK(os64_ui_menu_model_count(models[first])==1);
        memset(&transport,0,sizeof(transport)); transport.sw=1024; transport.sh=768;
        os64_ui_dispatcher_init(&dispatcher,&backend); transport.dispatcher=&dispatcher;
        CHECK(os64_ui_menu_session_create(&dispatcher,&ui,0,NULL,NULL,NULL,&session)==OS64_UI_OK);
        CHECK(os64_ui_menu_open(session,models[first],view,(os64_gui_rect_t){10,10,0,0},OS64_UI_MENU_CONTEXT)==OS64_UI_OK);
        menu_drain(); unsigned before=spawned;
        menu_key('\r',0x1c,0);
        CHECK(spawned==before+1 && !strcmp(spawned_path,"/bin/gclock"));
        CHECK(dispose()); CHECK(transport.creates==transport.destroys);
    }
    CHECK(load_config("menu root {\n menu Empty {\n }\n}\nmenu other {\n item Clock /bin/gclock\n}\n")==OS64_MENU_OK);
    os64_ui_init(&ui,NULL);
    CHECK(build(os64_menu_find(&gMenu,"root"))==OS64_UI_EMPTY); CHECK(dispose());
}
static void grootmenu_startup_errors(void)
{
    current="grootmenu startup errors";
    for(unsigned mode=0;mode<3;++mode) {
        CHECK(load_config("menu root {\n item Clock /bin/gclock\n}\n")==OS64_MENU_OK);
        int16_t first=os64_menu_find(&gMenu,"root");
        os64_ui_init(&ui,NULL); CHECK(build(first)==OS64_UI_OK);
        memset(&transport,0,sizeof(transport)); transport.sw=1024; transport.sh=768;
        transport.fail_create=mode==0;
        if(mode==1) transport.sw=transport.sh=1;
        os64_ui_dispatcher_init(&dispatcher,&backend); transport.dispatcher=&dispatcher;
        CHECK(os64_ui_menu_session_create(&dispatcher,&ui,0,NULL,NULL,NULL,&session)==OS64_UI_OK);
        os64_ui_menu_hooks(session,NULL,launcher_error,NULL);
        failed=error_reported=false;
        unsigned before=complaints;
        os64_gui_rect_t anchor={10,10,mode==2?-1:0,0};
        os64_ui_status_code_t rc=os64_ui_menu_open(session,models[first],view,anchor,OS64_UI_MENU_CONTEXT);
        CHECK(rc!=OS64_UI_OK && rc!=OS64_UI_BUSY);
        CHECK(complaints==before+(mode==2?0:1));
        startup_result(rc);
        CHECK(failed && complaints==before+1);
        CHECK(dispose()); CHECK(transport.creates==transport.destroys);
    }
    failed=error_reported=false;
}

int main(int argc,char **argv)
{
    sh_dir=argc>1?argv[1]:"userland/libfreetype/fixtures";
    scribe_menu_targets();
    scribe_menu_visibility_and_help();
    scribe_chrome_scrollbars();
    scribe_menu_quit_and_cleanup();
    scribe_menu_file_picker();
    scribe_build_refusals();
    grootmenu_models_and_launch();
    grootmenu_empty_cascades();
    grootmenu_startup_errors();
    for (size_t i=0;i<8;++i) free(sh_fs[i].bytes);
    printf("test_menu_apps_host: %lu checks, %d failures\n",checks,failures);
    return failures ? 1 : 0;
}
