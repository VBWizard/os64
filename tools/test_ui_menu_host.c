/* Production menu widgets/controller/dispatcher against deterministic E1K
 * queues. Native transport alone is replaced; real font runs remain live. */
#define UI_MENU_TEST
#include "test_ui_action_host.c"
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
static void path_invariants(void);
static int64_t backend_poll(void *u,int64_t h,os64_gui_event_t *e)
{
    path_invariants();
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
static os64_gui_event_t mouse(uint32_t type,int x,int y,uint8_t button)
{ os64_gui_event_t e={.type=type};e.mouse.x=x;e.mouse.y=y;e.mouse.button=button;return e; }
static int64_t popup_handle(void)
{
    for(int64_t i=32;i>1;--i) if(transport.windows[i].live&&transport.windows[i].owner) return i;
    return 0;
}
static void commands(void *u,const os64_gui_event_t *e)
{
    gallery *a=u;
    if(e->type==OS64_GUI_EVENT_MOUSE_BUTTON_DOWN) ++transport.owner_down;
    if(e->type==OS64_GUI_EVENT_MOUSE_BUTTON_UP) ++transport.owner_up;
    if(e->type==OS64_GUI_EVENT_MOUSE_WHEEL) ++transport.owner_wheel;
    if(!gallery_route(a,e,NULL)) (void)os64_ui_dispatch(&a->ui,e);
}
static void no_paint(void *u) { (void)u; }
static void drain(void)
{
    for(unsigned i=0;i<40;++i) {
        int64_t n=os64_ui_dispatcher_turn(&g.dispatcher,false);CHECK(n>=0);path_invariants();
        if(n==0&&os64_ui_dispatcher_can_create(&g.dispatcher)) break;
    }
}
static void build(const char *dir,bool real)
{
    (void)dir;(void)real;
    memset(&transport,0,sizeof(transport));transport.sw=1024;transport.sh=768;transport.focused=1;
    g=(gallery){0};
    transport.windows[1]=(struct window_fixture){.live=true,.rect={41,31,878,718}};
    transport.windows[1].surface=(os64_gui_surface_t){calloc(878*718,sizeof(uint32_t)),878,718,878};
    g.draw.surf=transport.windows[1].surface;g.draw.win=1;
    CHECK(gallery_build(&g));
#ifdef UI_TEXT_REAL
    if(real) {
        os64_font_set_t *set=outline_set(os64_ui_font_context(&g.ui),dir,"DejaVuSans.ttf",24);
        os64_font_consumer_t c;os64_ui_font_consumer(&g.ui,&c);
        CHECK(os64_font_adopt(set,&c,1,NULL)==OS64_FONT_OK);os64_font_set_release(set);
    }
#endif
    CHECK(gallery_attach_menus(&g,1,&backend,true,true)); transport.dispatcher=&g.dispatcher;
    g.dispatcher.entries[0].event=commands;g.dispatcher.entries[0].paint=no_paint;
}
static void dispose(void)
{
    gallery_dispose(&g);drain();CHECK(os64_ui_dispatcher_destroy(&g.dispatcher,1)==0);
    for(int i=1;i<33;++i) CHECK(!transport.windows[i].live);
    CHECK(!g.targets[0].refs&&!g.targets[1].refs);
}
static void key(int64_t h,char ascii,uint8_t sc,uint8_t mods)
{
    queue(h,edge(OS64_GUI_EVENT_KEY_DOWN,ascii,sc,mods));drain();
    queue(transport.focused,edge(OS64_GUI_EVENT_KEY_UP,ascii,sc,mods));drain();
}
static void open_context(os64_ui_menu_model_t *m,os64_ui_action_view_t *v)
{
    CHECK(os64_ui_menu_open(g.menus,m,v,(os64_gui_rect_t){100,100,0,0},OS64_UI_MENU_CONTEXT)==OS64_UI_OK);
    drain();CHECK(os64_ui_menu_active(g.menus)&&popup_handle());
}
static void models(void)
{
    current="menu model";os64_ui_menu_model_t *m=NULL;
    os64_ui_menu_entry_t e[65];for(size_t i=0;i<65;++i)e[i]=(os64_ui_menu_entry_t){.action=RECORD};
    CHECK(os64_ui_menu_model_create(g.actions,e,65,&m)==OS64_UI_LIMIT&&!m);
    CHECK(os64_ui_menu_model_create(g.actions,e,2,&m)==OS64_UI_DUPLICATE&&!m);
    e[0].action=999;CHECK(os64_ui_menu_model_create(g.actions,e,1,&m)==OS64_UI_INVALID&&!m);
    CHECK(os64_ui_menu_model_create(g.actions,NULL,0,&m)==OS64_UI_OK);
    CHECK(os64_ui_menu_open(g.menus,m,g.views[0],(os64_gui_rect_t){0},OS64_UI_MENU_CONTEXT)==OS64_UI_EMPTY&&!popup_handle());
    e[0]=(os64_ui_menu_entry_t){.kind=OS64_UI_MENU_SUBMENU,.label="Empty",.child=m};
    os64_ui_menu_model_t *bad=NULL;CHECK(os64_ui_menu_model_create(g.actions,e,1,&bad)==OS64_UI_INVALID&&!bad);
    os64_ui_menu_model_release(m);
    e[0]=(os64_ui_menu_entry_t){.kind=OS64_UI_MENU_SEPARATOR};e[1]=e[0];e[2]=(os64_ui_menu_entry_t){.action=RECORD};e[3]=e[0];e[4]=e[0];
    CHECK(os64_ui_menu_model_create(g.actions,e,5,&m)==OS64_UI_OK&&os64_ui_menu_model_count(m)==1);os64_ui_menu_model_release(m);
    m=g.models[0];CHECK(os64_ui_menu_model_retain(m)==OS64_UI_OK);
    for(unsigned depth=2;depth<=16;++depth) {
        e[0]=(os64_ui_menu_entry_t){.kind=OS64_UI_MENU_SUBMENU,.label="Depth",.child=m};
        CHECK(os64_ui_menu_model_create(g.actions,e,1,&bad)==OS64_UI_OK);
        os64_ui_menu_model_release(m);m=bad;
    }
    e[0].child=m;CHECK(os64_ui_menu_model_create(g.actions,e,1,&bad)==OS64_UI_LIMIT&&!bad);os64_ui_menu_model_release(m);
    for(unsigned i=0;i<6;++i)e[i]=(os64_ui_menu_entry_t){.kind=OS64_UI_MENU_SUBMENU,.label="Graph",.child=g.models[3]};
    CHECK(os64_ui_menu_model_create(g.actions,e,6,&m)==OS64_UI_LIMIT&&!m);
    deny_countdown=0;CHECK(os64_ui_menu_model_create(g.actions,e,1,&m)==OS64_UI_NO_MEMORY&&!m);
    bool bound=false;
    for(long n=0;n<400;++n) {
        void *old_run=g.bar.labels[0];
        deny_countdown=n;os64_ui_status_code_t rc=os64_ui_menubar_bind(&g.bar,g.models[5]);deny_countdown=-1;
        if(rc==OS64_UI_OK) { bound=true; break; }
        CHECK(rc==OS64_UI_NO_MEMORY&&g.bar.model==g.models[5]&&g.bar.labels[0]==old_run);
    }
    CHECK(bound);
}
static void disabled_bar_and_overflow_failure(void)
{
    current="disabled hidden heading and failed overflow preparation";
    os64_ui_menu_model_t *leaf=NULL,*top=NULL;
    os64_ui_menu_entry_t action={.action=RECORD};
    CHECK(os64_ui_menu_model_create(g.actions,&action,1,&leaf)==OS64_UI_OK);
    os64_ui_menu_entry_t heading={.kind=OS64_UI_MENU_SUBMENU,.label="Disabled heading",.mnemonic='z',.child=leaf};
    CHECK(os64_ui_menu_model_create(g.actions,&heading,1,&top)==OS64_UI_OK);
    CHECK(os64_ui_menubar_bind(&g.bar,top)==OS64_UI_OK);
    g.targets[0].enabled=false;CHECK(os64_ui_actions_refresh(g.views[0])==OS64_UI_OK);
    os64_ui_set_hidden(&g.ui,&g.bar.w,true);gallery_layout(&g,false);
    key(1,'z',0x2c,OS64_GUI_MOD_ALT);
    CHECK(!popup_handle()&&!os64_ui_menu_active(g.menus)&&g.bar.w.hidden&&!g.bar.temporary);
    key(1,0,0x44,0);
    CHECK(!os64_ui_menu_active(g.menus)&&g.bar.w.hidden&&!g.bar.temporary);
    key(1,0,0x38,OS64_GUI_MOD_ALT);
    CHECK(!os64_ui_menu_active(g.menus)&&g.bar.w.hidden&&!g.bar.temporary);
    g.targets[0].enabled=true;CHECK(os64_ui_actions_refresh(g.views[0])==OS64_UI_OK);
    key(1,0,0x44,0);g.bar.w.bounds.w=1;
    deny_countdown=0;key(1,'\n',0x1c,0);deny_countdown=-1;
    CHECK(!popup_handle()&&!os64_ui_menu_active(g.menus)&&g.bar.w.hidden&&!g.bar.temporary);
    CHECK(os64_ui_menubar_bind(&g.bar,g.models[5])==OS64_UI_OK);
    os64_ui_menu_model_release(top);os64_ui_menu_model_release(leaf);
    os64_ui_set_hidden(&g.ui,&g.bar.w,false);gallery_layout(&g,false);
}

static void dismissal_orders(void)
{
    current="dismissal orders";
    for(unsigned order=0;order<2;++order) {
        open_context(g.models[0],g.views[0]);int64_t h=popup_handle();
        transport.focused=1;
        queue(h,focus_edge(false,1));queue(1,focus_edge(true,h));
        queue(1,mouse(OS64_GUI_EVENT_MOUSE_BUTTON_DOWN,50,130,OS64_GUI_MOUSE_LEFT));
        queue(1,mouse(OS64_GUI_EVENT_MOUSE_BUTTON_UP,50,130,OS64_GUI_MOUSE_LEFT));
        g.dispatcher.cursor=order?1:0;
        unsigned down=transport.owner_down,up=transport.owner_up;
        drain();CHECK(!os64_ui_menu_active(g.menus)&&!popup_handle());
        CHECK(transport.owner_down==down&&transport.owner_up==up);
        queue(1,mouse(OS64_GUI_EVENT_MOUSE_BUTTON_DOWN,50,130,OS64_GUI_MOUSE_LEFT));
        queue(1,mouse(OS64_GUI_EVENT_MOUSE_BUTTON_UP,50,130,OS64_GUI_MOUSE_LEFT));drain();
        CHECK(transport.owner_down==down+1&&transport.owner_up==up+1);
    }
    open_context(g.models[0],g.views[0]);int64_t h=popup_handle();transport.focused=1;
    queue(h,focus_edge(false,1));queue(1,focus_edge(true,h));drain();
    unsigned down=transport.owner_down;
    queue(1,mouse(OS64_GUI_EVENT_MOUSE_BUTTON_DOWN,50,130,OS64_GUI_MOUSE_LEFT));drain();CHECK(transport.owner_down==down+1);
    /* Deliberate close returns focus, with no swallow armed. */
    open_context(g.models[0],g.views[0]);os64_ui_menu_close(g.menus,OS64_UI_MENU_EXPLICIT);drain();
    down=transport.owner_down;queue(1,mouse(OS64_GUI_EVENT_MOUSE_BUTTON_DOWN,50,130,OS64_GUI_MOUSE_LEFT));drain();CHECK(transport.owner_down==down+1);
}
static void activation_and_loss(void)
{
    current="activation and input loss";unsigned hits=g.targets[0].hits;
    open_context(g.models[0],g.views[0]);int64_t h=popup_handle();
    queue(h,edge(OS64_GUI_EVENT_KEY_DOWN,'\n',0x1c,0));
    queue(h,edge(OS64_GUI_EVENT_KEY_DOWN,'\n',0x1c,0));
    queue(h,edge(OS64_GUI_EVENT_KEY_UP,'\n',0x1c,0));drain();
    CHECK(g.targets[0].hits==hits+1&&!popup_handle());
    open_context(g.models[0],g.views[0]);h=popup_handle();
    queue(h,edge(OS64_GUI_EVENT_KEY_DOWN,'\n',0x1c,0));
    queue(h,(os64_gui_event_t){.type=OS64_GUI_EVENT_INPUT_LOST});drain();
    CHECK(g.targets[0].hits==hits+1&&!popup_handle());
    /* A release alone cannot activate after loss/reopen. */
    open_context(g.models[0],g.views[0]);h=popup_handle();
    queue(h,edge(OS64_GUI_EVENT_KEY_UP,'\n',0x1c,0));drain();CHECK(g.targets[0].hits==hits+1);
    queue(h,edge(OS64_GUI_EVENT_KEY_DOWN,4,0x20,OS64_GUI_MOD_CTRL));drain();
    queue(1,edge(OS64_GUI_EVENT_KEY_UP,4,0x20,OS64_GUI_MOD_CTRL));drain();
    CHECK(g.targets[0].hits==hits+2&&!popup_handle());
    /* A saved target generation is checked again at invocation. */
    open_context(g.models[0],g.views[0]);h=popup_handle();++g.targets[0].generation;
    key(h,'\n',0x1c,0);CHECK(g.targets[0].hits==hits+2&&!popup_handle());--g.targets[0].generation;
    CHECK(os64_ui_actions_refresh(g.views[0])==OS64_UI_OK);
    /* Disabled commands skip highlight and refuse shortcuts. */
    g.targets[0].enabled=false;open_context(g.models[0],g.views[0]);h=popup_handle();
    queue(h,edge(OS64_GUI_EVENT_KEY_DOWN,4,0x20,OS64_GUI_MOD_CTRL));drain();
    queue(1,edge(OS64_GUI_EVENT_KEY_UP,4,0x20,OS64_GUI_MOD_CTRL));drain();
    CHECK(g.targets[0].hits==hits+2&&!popup_handle());g.targets[0].enabled=true;
    CHECK(os64_ui_actions_refresh(g.views[0])==OS64_UI_OK);
    open_context(g.models[2],g.views[0]);h=popup_handle();g.radio=0;
    key(h,'f',0x21,0);CHECK(g.radio==0&&popup_handle());
    key(h,'f',0x21,0);CHECK(g.radio==0&&popup_handle());
    key(h,'\n',0x1c,0);CHECK(g.radio==RADIO_A&&!popup_handle());
    open_context(g.models[0],g.views[0]);h=popup_handle();
    os64_ui_popup_t *p=NULL;CHECK(os64_ui_popup_create(&g.ui,g.models[0],g.views[0],NULL,1000,700,&p)==OS64_UI_OK);
    os64_gui_rect_t row=os64_ui_popup_row(p,0);os64_ui_popup_destroy(p);
    queue(h,mouse(OS64_GUI_EVENT_MOUSE_BUTTON_UP,10,row.y+2,OS64_GUI_MOUSE_LEFT));drain();CHECK(popup_handle()&&g.targets[0].hits==hits+2);
    queue(h,mouse(OS64_GUI_EVENT_MOUSE_BUTTON_DOWN,10,row.y+2,OS64_GUI_MOUSE_LEFT));
    queue(h,mouse(OS64_GUI_EVENT_MOUSE_MOVE,11,row.y+3,0));
    queue(h,mouse(OS64_GUI_EVENT_MOUSE_BUTTON_UP,11,row.y+3,OS64_GUI_MOUSE_LEFT));drain();CHECK(g.targets[0].hits==hits+3&&!popup_handle());
}
static void quarantine(void)
{
    current="quarantine across budgets";
    CHECK(os64_ui_menu_open(g.menus,g.models[0],g.views[0],(os64_gui_rect_t){20,20,0,0},OS64_UI_MENU_CONTEXT)==OS64_UI_OK);
    /* Replace before birth focus events are drained: both lost and gained
     * records naming the retired handle must leave the new intent alive. */
    CHECK(os64_ui_menu_open(g.menus,g.models[1],g.views[0],(os64_gui_rect_t){20,20,0,0},OS64_UI_MENU_CONTEXT)==OS64_UI_BUSY);
    drain();CHECK(popup_handle()&&os64_ui_menu_active(g.menus));
    os64_ui_menu_close(g.menus,OS64_UI_MENU_EXPLICIT);drain();
    open_context(g.models[0],g.views[0]);
    for(unsigned i=0;i<150;++i)queue(1,(os64_gui_event_t){.type=OS64_GUI_EVENT_DOORBELL});
    unsigned births=transport.creates;
    CHECK(os64_ui_menu_open(g.menus,g.models[1],g.views[0],(os64_gui_rect_t){20,20,0,0},OS64_UI_MENU_CONTEXT)==OS64_UI_BUSY);
    CHECK(!os64_ui_dispatcher_can_create(&g.dispatcher));
    CHECK(os64_ui_dispatcher_turn(&g.dispatcher,false)==64&&transport.creates==births);
    CHECK(os64_ui_dispatcher_turn(&g.dispatcher,false)==64&&transport.creates==births);
    drain();CHECK(transport.creates==births+1&&popup_handle());
    os64_ui_menu_close(g.menus,OS64_UI_MENU_EXPLICIT);drain();
}
static void pending_context_keys(void)
{
    current="keyboard input during pending context opening";
    os64_ui_menu_session_dispose(g.menus);g.menus=NULL;
    CHECK(os64_ui_menu_session_create(&g.dispatcher,&g.ui,1,NULL,&g.status,NULL,&g.menus)==OS64_UI_OK);
    CHECK(os64_ui_menu_open(g.menus,g.models[0],g.views[0],(os64_gui_rect_t){20,20,0,0},OS64_UI_MENU_CONTEXT)==OS64_UI_OK);
    CHECK(os64_ui_menu_open(g.menus,g.models[1],g.views[0],(os64_gui_rect_t){20,20,0,0},OS64_UI_MENU_CONTEXT)==OS64_UI_BUSY);
    unsigned hits=g.targets[0].hits;
    queue(1,edge(OS64_GUI_EVENT_KEY_DOWN,'\n',0x1c,0));drain();
    queue(transport.focused,edge(OS64_GUI_EVENT_KEY_UP,'\n',0x1c,0));drain();
    CHECK(g.targets[0].hits==hits&&os64_ui_menu_active(g.menus));
    os64_ui_menu_session_dispose(g.menus);g.menus=NULL;drain();
    CHECK(os64_ui_menu_session_create(&g.dispatcher,&g.ui,1,&g.bar,&g.status,&gallery_icons,&g.menus)==OS64_UI_OK);
    CHECK(os64_ui_menu_bar_view(g.menus,g.views[0])==OS64_UI_OK);
    os64_ui_menu_hooks(g.menus,gallery_menu_layout,gallery_menu_error,&g);
}

static void review_navigation(void)
{
    current="review: refresh closes invalid descendants and clears help";
    os64_ui_menu_model_t *leaf=NULL,*top=NULL;
    os64_ui_menu_entry_t action={.action=RECORD};
    CHECK(os64_ui_menu_model_create(g.actions,&action,1,&leaf)==OS64_UI_OK);
    os64_ui_menu_entry_t headings[]={
        {.kind=OS64_UI_MENU_SUBMENU,.label="Record only",.child=leaf},
        {.kind=OS64_UI_MENU_SUBMENU,.label="Other",.child=g.models[1]}};
    CHECK(os64_ui_menu_model_create(g.actions,headings,2,&top)==OS64_UI_OK);
    open_context(top,g.views[0]);int64_t root=popup_handle();
    key(root,0,0x4d,0);CHECK(popup_handle()!=root&&g.status.help.text);
    g.targets[0].enabled=false;
    CHECK(os64_ui_menu_refresh(g.menus)==OS64_UI_OK);
    CHECK(popup_handle()==root&&!g.status.help.text);
    os64_ui_menu_close(g.menus,OS64_UI_MENU_EXPLICIT);drain();

    current="review: bar mode skips disabled remembered heading";
    CHECK(os64_ui_menubar_bind(&g.bar,top)==OS64_UI_OK);
    g.bar.selected=0;key(1,0,0x44,0);
    CHECK(os64_ui_menu_active(g.menus)&&g.bar.selected==1);
    key(1,0,0x50,0);CHECK(popup_handle());
    os64_ui_menu_close(g.menus,OS64_UI_MENU_EXPLICIT);drain();
    g.bar.selected=0;key(1,0,0x38,OS64_GUI_MOD_ALT);
    CHECK(os64_ui_menu_active(g.menus)&&g.bar.selected==1);
    os64_ui_menu_close(g.menus,OS64_UI_MENU_EXPLICIT);drain();
    CHECK(os64_ui_menubar_bind(&g.bar,g.models[5])==OS64_UI_OK);
    g.targets[0].enabled=true;CHECK(os64_ui_actions_refresh(g.views[0])==OS64_UI_OK);
    os64_ui_menu_model_release(top);os64_ui_menu_model_release(leaf);

    current="review: reject unattached and sibling menu bars";
    os64_ui_menu_session_dispose(g.menus);g.menus=NULL;
    os64_ui_menubar_t foreign;os64_ui_menubar_init(&foreign);
    os64_ui_menu_session_t *session=NULL;
    CHECK(os64_ui_menu_session_create(&g.dispatcher,&g.ui,1,&foreign,NULL,NULL,&session)==OS64_UI_INVALID&&!session);
    if(session) os64_ui_menu_session_dispose(session);
    os64_ui_t sibling={0};foreign.w.ui=&sibling;
    CHECK(os64_ui_menu_session_create(&g.dispatcher,&g.ui,1,&foreign,NULL,NULL,&session)==OS64_UI_INVALID&&!session);
    if(session) os64_ui_menu_session_dispose(session);
    CHECK(os64_ui_menu_session_create(&g.dispatcher,&g.ui,1,&g.bar,&g.status,&gallery_icons,&g.menus)==OS64_UI_OK);
    CHECK(os64_ui_menu_bar_view(g.menus,g.views[0])==OS64_UI_OK);
    os64_ui_menu_hooks(g.menus,gallery_menu_layout,gallery_menu_error,&g);

    current="review: active menus consume owner wheel";
    open_context(g.models[0],g.views[0]);unsigned wheels=transport.owner_wheel;
    os64_gui_event_t wheel={.type=OS64_GUI_EVENT_MOUSE_WHEEL};wheel.mouse.dy=1;
    queue(1,wheel);drain();CHECK(transport.owner_wheel==wheels&&popup_handle());
    os64_ui_menu_close(g.menus,OS64_UI_MENU_EXPLICIT);drain();
    queue(1,wheel);drain();CHECK(transport.owner_wheel==wheels+1);
}

static void destruction_failure(const char *dir)
{
    current="review: failed root destruction preserves registrations and callbacks";
    open_context(g.models[0],g.views[0]);int64_t root=popup_handle();
    unsigned births=transport.creates,hits=g.targets[0].hits;
    transport.fail_destroy=root;
    CHECK(os64_ui_menu_close(g.menus,OS64_UI_MENU_EXPLICIT)==OS64_UI_ERROR);
    CHECK(os64_ui_dispatcher_live(&g.dispatcher,root)&&os64_ui_menu_active(g.menus));
    CHECK(os64_ui_menu_open(g.menus,g.models[1],g.views[0],(os64_gui_rect_t){0},OS64_UI_MENU_CONTEXT)==OS64_UI_ERROR);
    CHECK(transport.creates==births&&popup_handle()==root);
    key(root,'\n',0x1c,0);CHECK(g.targets[0].hits==hits&&popup_handle()==root);
    CHECK(os64_ui_menu_session_dispose(g.menus)==OS64_UI_ERROR);
    CHECK(g.ui.menu==g.menus&&g.bar.session==g.menus&&g.ui.menu_event&&g.ui.menu_before_font);
    CHECK(!g.ui.menu_before_font(&g.ui));
    void *font=g.ui.font;
    CHECK(os64_ui_font_release(&g.ui)==OS64_FONT_BUSY&&g.ui.font==font);
#ifdef UI_TEXT_REAL
    os64_font_set_t *set=outline_set(os64_ui_font_context(&g.ui),dir,"DejaVuSans.ttf",32);
    os64_font_consumer_t consumer;os64_ui_font_consumer(&g.ui,&consumer);
    CHECK(os64_font_adopt(set,&consumer,1,NULL)==OS64_FONT_BUSY&&g.ui.font==font);
    os64_font_set_release(set);
#else
    (void)dir;
#endif
    /* Dispatch and paint the retained registration after rejected disposal. */
    queue(root,mouse(OS64_GUI_EVENT_MOUSE_MOVE,3,3,0));drain();
    transport.fail_destroy=0;
    CHECK(os64_ui_menu_close(g.menus,OS64_UI_MENU_EXPLICIT)==OS64_UI_OK);drain();

    current="review: failed child replacement and partial close remain retryable";
    open_context(g.models[4],g.views[0]);root=popup_handle();
    key(root,0,0x4d,0);int64_t child=popup_handle();CHECK(child!=root);
    transport.fail_destroy=child;births=transport.creates;
    os64_ui_popup_t *p=NULL;
    CHECK(os64_ui_popup_create(&g.ui,g.models[4],g.views[0],NULL,1000,700,&p)==OS64_UI_OK);
    os64_gui_rect_t row=os64_ui_popup_row(p,1);os64_ui_popup_destroy(p);
    queue(root,mouse(OS64_GUI_EVENT_MOUSE_MOVE,10,row.y+2,0));drain();
    CHECK(transport.creates==births&&popup_handle()==child&&os64_ui_dispatcher_live(&g.dispatcher,child));
    transport.fail_destroy=root;
    CHECK(os64_ui_menu_close(g.menus,OS64_UI_MENU_EXPLICIT)==OS64_UI_ERROR);
    CHECK(!transport.windows[child].live&&transport.windows[root].live&&os64_ui_menu_active(g.menus));
    CHECK(os64_ui_menu_session_dispose(g.menus)==OS64_UI_ERROR&&g.ui.menu==g.menus);
    queue(root,(os64_gui_event_t){.type=OS64_GUI_EVENT_DOORBELL});drain();
    transport.fail_destroy=0;
    CHECK(os64_ui_menu_session_dispose(g.menus)==OS64_UI_OK);g.menus=NULL;drain();
    CHECK(!g.ui.menu&&!g.bar.session&&!popup_handle());
    CHECK(os64_ui_menu_session_create(&g.dispatcher,&g.ui,1,&g.bar,&g.status,&gallery_icons,&g.menus)==OS64_UI_OK);
    CHECK(os64_ui_menu_bar_view(g.menus,g.views[0])==OS64_UI_OK);
    os64_ui_menu_hooks(g.menus,gallery_menu_layout,gallery_menu_error,&g);
}

static void review_round_two(void)
{
    current="round two: stale refresh preserves destruction error";
    open_context(g.models[0],g.views[0]);int64_t root=popup_handle();
    ++g.targets[0].generation;transport.fail_destroy=root;
    CHECK(os64_ui_menu_refresh(g.menus)==OS64_UI_ERROR);
    CHECK(os64_ui_dispatcher_live(&g.dispatcher,root)&&g.ui.menu==g.menus);
    transport.fail_destroy=0;
    CHECK(os64_ui_menu_refresh(g.menus)==OS64_UI_STALE&&!popup_handle());drain();
    --g.targets[0].generation;CHECK(os64_ui_actions_refresh(g.views[0])==OS64_UI_OK);

    current="round two: scroll clears hidden selection and activation";
    os64_ui_popup_t *p=NULL;
    CHECK(os64_ui_popup_create(&g.ui,g.models[3],g.views[0],NULL,400,180,&p)==OS64_UI_OK);
    os64_ui_popup_select(p,0);os64_ui_popup_scroll(p,4);
    CHECK(os64_ui_popup_selected(p)==MENU_NONE);
    os64_ui_popup_select(p,p->top);size_t selected=p->selected;
    os64_ui_popup_scroll(p,0);CHECK(p->selected==selected);
    os64_ui_popup_scroll(p,INT32_MAX);CHECK(p->selected==MENU_NONE);
    os64_ui_popup_destroy(p);
    os64_ui_menu_entry_t rows[50];rows[0]=(os64_ui_menu_entry_t){.action=RECORD};
    for(unsigned i=1;i<50;++i) rows[i]=(os64_ui_menu_entry_t){.action=100+i};
    os64_ui_menu_model_t *tall=NULL;
    CHECK(os64_ui_menu_model_create(g.actions,rows,50,&tall)==OS64_UI_OK);
    transport.sh=220;open_context(tall,g.views[0]);root=popup_handle();
    unsigned hits=g.targets[0].hits;
    os64_gui_event_t wheel={.type=OS64_GUI_EVENT_MOUSE_WHEEL};wheel.mouse.dy=2;
    queue(root,wheel);drain();CHECK(!g.status.help.text);
    key(root,'\n',0x1c,0);CHECK(g.targets[0].hits==hits&&popup_handle()==root);
    key(root,0,0x50,0);
    CHECK(g.status.help.text&&strstr(g.status.help.text,"Record")==NULL);
    queue(root,mouse(OS64_GUI_EVENT_MOUSE_BUTTON_DOWN,10,(int)transport.windows[root].surface.height-1,OS64_GUI_MOUSE_LEFT));drain();
    key(root,'\n',0x1c,0);CHECK(g.targets[0].hits==hits&&popup_handle()==root);
    os64_ui_menu_close(g.menus,OS64_UI_MENU_EXPLICIT);drain();transport.sh=768;
    os64_ui_menu_model_release(tall);

    current="round two: keyboard cancels pending child at tombstone boundary";
    os64_ui_menu_entry_t entries[]={
        {.kind=OS64_UI_MENU_SUBMENU,.label="First child",.mnemonic='x',.child=g.models[2]},
        {.kind=OS64_UI_MENU_SUBMENU,.label="Second child",.mnemonic='x',.child=g.models[3]},
        {.action=RECORD}};
    os64_ui_menu_model_t *parent=NULL;
    CHECK(os64_ui_menu_model_create(g.actions,entries,3,&parent)==OS64_UI_OK);
    const uint8_t moves[]={0x48,0x50,0x47,0x4f,0x49,0x51,0x2d};
    for(size_t move=0;move<sizeof(moves);++move) {
        open_context(parent,g.views[0]);root=popup_handle();
        key(root,0,0x4d,0);CHECK(popup_handle()!=root);
        /* Parent hover queues a replacement, then keyboard movement wins
         * before the surviving queues reach the empty boundary. */
        CHECK(os64_ui_popup_create(&g.ui,parent,g.views[0],NULL,1000,700,&p)==OS64_UI_OK);
        os64_gui_rect_t row=os64_ui_popup_row(p,1);os64_ui_popup_destroy(p);
        unsigned births=transport.creates;
        queue(root,mouse(OS64_GUI_EVENT_MOUSE_MOVE,10,row.y+2,0));
        char ascii=move==6?'x':0;
        queue(root,edge(OS64_GUI_EVENT_KEY_DOWN,ascii,moves[move],0));
        queue(root,edge(OS64_GUI_EVENT_KEY_UP,ascii,moves[move],0));drain();
        CHECK(transport.creates==births&&popup_handle()==root);
        os64_ui_menu_close(g.menus,OS64_UI_MENU_EXPLICIT);drain();
    }
    os64_ui_menu_model_release(parent);

    current="round two: hidden mnemonic highlights overflow and preserves hover";
    g.bar.w.bounds.w=g.bar.widths[0]+2*MENU_GAP+g.bar.overflow_width+1;
    key(1,'l',0x26,OS64_GUI_MOD_ALT);root=popup_handle();CHECK(root);
    size_t visible=menu_bar_visible(&g.bar);CHECK(visible<g.bar.model->count);
    CHECK(g.bar.selected==visible);
    int32_t x=g.bar.w.bounds.x;
    for(size_t i=0;i<visible;++i) x+=g.bar.widths[i]+2*MENU_GAP;
    unsigned births=transport.creates;
    queue(1,mouse(OS64_GUI_EVENT_MOUSE_MOVE,x+2,g.bar.w.bounds.y+2,0));drain();
    CHECK(transport.creates==births&&popup_handle()==root);
    key(root,0,0x4b,0);CHECK(g.bar.selected==0&&popup_handle());
    os64_ui_menu_close(g.menus,OS64_UI_MENU_EXPLICIT);drain();gallery_layout(&g,false);
}

static unsigned r3_last_action,r3_calls;
static os64_ui_action_result_t r3_invoke(void *app,const os64_ui_action_context_t *ctx,uint32_t id)
{
    (void)app;(void)ctx;
    CHECK(!os64_ui_menu_active(g.menus)&&!popup_handle());
    r3_last_action=id;++r3_calls;return (os64_ui_action_result_t){OS64_UI_OK,0};
}
static void r3_query(void *app,const os64_ui_action_context_t *ctx,uint32_t id,os64_ui_action_state_t *state)
{ (void)ctx;state->enabled=app?id%10==1:true; }
static void review_round_three(void)
{
    current="round three: menu gestures precede registered shortcuts";
    const uint16_t keys[]={OS64_UI_KEY_ESCAPE,OS64_UI_KEY_ENTER,OS64_UI_KEY_TAB,
        OS64_UI_KEY_UP,OS64_UI_KEY_DOWN,OS64_UI_KEY_LEFT,OS64_UI_KEY_RIGHT,
        OS64_UI_KEY_HOME,OS64_UI_KEY_END,OS64_UI_KEY_PAGE_UP,OS64_UI_KEY_PAGE_DOWN,' ','x','z'};
    const uint8_t scans[]={0x29,0x28,0x2b,0x52,0x51,0x50,0x4f,0x4a,0x4d,0x4b,0x4e,0x2c,0x1b,0x1d};
    os64_ui_action_spec_t specs[15]={{.id=1,.label="Selected command",.mnemonic='x'}};
    for(size_t i=0;i<14;++i) specs[i+1]=(os64_ui_action_spec_t){.id=(uint32_t)i+2,.label="Shortcut",.shortcut={keys[i],0}};
    os64_ui_action_callbacks_t callbacks={r3_query,r3_invoke};
    os64_ui_actions_t *actions=NULL;os64_ui_action_view_t *view=NULL;
    action_test_target target={.gen=1,.enabled=true};
    os64_ui_action_context_t context={&target,1,at_retain,at_release,at_valid};
    CHECK(os64_ui_actions_create(specs,15,&callbacks,NULL,&actions)==OS64_UI_OK);
    CHECK(os64_ui_action_view_create(actions,&context,&view)==OS64_UI_OK);
    os64_ui_menu_model_t *leaf=NULL,*model=NULL;
    os64_ui_menu_entry_t command={.action=1};
    CHECK(os64_ui_menu_model_create(actions,&command,1,&leaf)==OS64_UI_OK);
    os64_ui_menu_entry_t rows[]={{.action=1},{.kind=OS64_UI_MENU_SUBMENU,.label="Nested",.child=leaf}};
    CHECK(os64_ui_menu_model_create(actions,rows,2,&model)==OS64_UI_OK);
    for(size_t i=0;i<14;++i) {
        open_context(model,view);int64_t root=popup_handle();unsigned before=r3_calls;
        char ascii=i==0?0x1b:i==1?'\n':i==2?'\t':i==11?' ':i==12?'x':i==13?'z':0;
        key(root,ascii,scans[i],OS64_GUI_MOD_HID);
        bool activate=i==1||i==11||i==12||i==13;
        CHECK(r3_calls==before+(activate?1:0));
        if(activate) CHECK(r3_last_action==(i==13?15u:1u));
        else if(i!=0&&i!=2) CHECK(popup_handle()==root);
        os64_ui_menu_close(g.menus,OS64_UI_MENU_EXPLICIT);drain();
    }
    os64_ui_menu_model_release(model);os64_ui_menu_model_release(leaf);
    CHECK(os64_ui_action_view_destroy(view)==OS64_UI_OK);
    CHECK(os64_ui_actions_destroy(actions)==OS64_UI_OK&&!target.refs);

    current="round three: failed stale hidden-bar reveal collapses";
    os64_ui_set_hidden(&g.ui,&g.bar.w,true);gallery_layout(&g,false);
    ++g.targets[0].generation;key(1,'a',0x1e,OS64_GUI_MOD_ALT);
    CHECK(g.bar.w.hidden&&!g.bar.temporary&&!os64_ui_menu_active(g.menus)&&!popup_handle());
    os64_ui_menu_close(g.menus,OS64_UI_MENU_EXPLICIT);drain();
    --g.targets[0].generation;CHECK(os64_ui_actions_refresh(g.views[0])==OS64_UI_OK);
    os64_ui_set_hidden(&g.ui,&g.bar.w,false);gallery_layout(&g,false);

    current="round three: page movement counts model rows";
    os64_ui_action_spec_t sparse[61];
    for(unsigned i=0;i<61;++i) sparse[i]=(os64_ui_action_spec_t){.id=i+1,.label="Sparse command"};
    CHECK(os64_ui_actions_create(sparse,61,&callbacks,&target,&actions)==OS64_UI_OK);
    CHECK(os64_ui_action_view_create(actions,&context,&view)==OS64_UI_OK);
    os64_ui_menu_entry_t entries[61];
    for(unsigned i=0;i<61;++i) entries[i]=i%10==5?
        (os64_ui_menu_entry_t){.kind=OS64_UI_MENU_SEPARATOR}:(os64_ui_menu_entry_t){.action=i+1};
    CHECK(os64_ui_menu_model_create(actions,entries,61,&model)==OS64_UI_OK);
    transport.sh=220;open_context(model,view);int64_t root=popup_handle();
    key(root,0,0x4e,OS64_GUI_MOD_HID);key(root,'\n',0x28,OS64_GUI_MOD_HID);
    CHECK(r3_last_action==11);
    open_context(model,view);root=popup_handle();key(root,0,0x4d,OS64_GUI_MOD_HID);
    key(root,0,0x4b,OS64_GUI_MOD_HID);key(root,'\n',0x28,OS64_GUI_MOD_HID);
    CHECK(r3_last_action==51);
    os64_ui_menu_model_release(model);CHECK(os64_ui_action_view_destroy(view)==OS64_UI_OK);
    CHECK(os64_ui_actions_destroy(actions)==OS64_UI_OK&&!target.refs);transport.sh=768;
}
static void collapse_pending_child(void)
{
    current="round three: collapse cancels orphaned pending descendants";
    os64_ui_menu_entry_t submenu={.kind=OS64_UI_MENU_SUBMENU,.label="Parent",.child=g.models[4]};
    os64_ui_menu_model_t *model=NULL;
    CHECK(os64_ui_menu_model_create(g.actions,&submenu,1,&model)==OS64_UI_OK);
    for(unsigned left=0;left<2;++left) {
        open_context(model,g.views[0]);int64_t root=popup_handle();
        key(root,0,0x4d,0);int64_t parent=popup_handle();CHECK(parent!=root);
        key(parent,0,0x4d,0);CHECK(popup_handle()!=parent);
        os64_ui_popup_t *p=NULL;
        CHECK(os64_ui_popup_create(&g.ui,g.models[4],g.views[0],NULL,1000,700,&p)==OS64_UI_OK);
        os64_gui_rect_t row=os64_ui_popup_row(p,1);os64_ui_popup_destroy(p);
        unsigned before=transport.creates;
        for(unsigned i=0;i<150;++i) queue(1,(os64_gui_event_t){.type=OS64_GUI_EVENT_DOORBELL});
        queue(parent,mouse(OS64_GUI_EVENT_MOUSE_MOVE,10,row.y+2,0));
        queue(parent,edge(OS64_GUI_EVENT_KEY_DOWN,left?0:0x1b,left?0x4b:0x01,0));drain();
        CHECK(transport.creates==before&&popup_handle()==root&&!transport.windows[parent].live);
        queue(root,edge(OS64_GUI_EVENT_KEY_UP,left?0:0x1b,left?0x4b:0x01,0));drain();
        os64_ui_menu_close(g.menus,OS64_UI_MENU_EXPLICIT);drain();
    }
    os64_ui_menu_model_release(model);
}

static void navigation(void)
{
    current="bar and cascades";
    unsigned hits=g.targets[0].hits;
    os64_gui_event_t press=mouse(OS64_GUI_EVENT_MOUSE_BUTTON_DOWN,g.record[0].bounds.x+3,g.record[0].bounds.y+3,OS64_GUI_MOUSE_LEFT);
    CHECK(os64_ui_dispatch(&g.ui,&press)&&g.record[0].pressed);
    key(1,0,0x44,0);CHECK(!g.ui.grab&&!g.record[0].pressed);
    press.type=OS64_GUI_EVENT_MOUSE_BUTTON_UP;queue(1,press);drain();CHECK(g.targets[0].hits==hits);
    key(1,0x1b,0x01,0);os64_ui_set_focus(&g.ui,&g.content);
    CHECK(os64_ui_action_invoke(g.views[2],BAR).status==OS64_UI_OK&&g.bar.w.hidden);
    key(1,0,0x44,0);CHECK(os64_ui_menu_active(g.menus)&&g.bar.temporary&&!g.bar.w.hidden);
    key(1,0x1b,0x01,0);CHECK(!os64_ui_menu_active(g.menus)&&g.bar.w.hidden);
    key(1,0,0xe2,OS64_GUI_MOD_HID|OS64_GUI_MOD_ALT);CHECK(g.bar.temporary&&!g.bar.w.hidden);
    key(1,0x1b,0x29,OS64_GUI_MOD_HID);CHECK(g.bar.w.hidden);
    /* Chord and pointer intervening gestures disarm bare Alt. */
    queue(1,edge(OS64_GUI_EVENT_KEY_DOWN,0,0x38,OS64_GUI_MOD_ALT));
    queue(1,edge(OS64_GUI_EVENT_KEY_DOWN,'x',0x2d,OS64_GUI_MOD_ALT));
    queue(1,edge(OS64_GUI_EVENT_KEY_UP,0,0x38,0));drain();CHECK(!os64_ui_menu_active(g.menus));
    key(1,'v',0x2f,OS64_GUI_MOD_ALT);CHECK(popup_handle()&&g.bar.temporary);
    key(popup_handle(),0x1b,0x01,0);CHECK(g.bar.w.hidden);
    key(1,0,0x44,0);g.bar.w.bounds.w=90;key(1,'\n',0x1c,0);CHECK(popup_handle());
    /* Overflow's first row is a complete top-level submenu. */
    key(popup_handle(),0,0x4d,0);CHECK(transport.windows[popup_handle()].owner!=1);
    key(popup_handle(),0x1b,0x01,0);CHECK(popup_handle());
    os64_ui_menu_close(g.menus,OS64_UI_MENU_EXPLICIT);drain();gallery_layout(&g,false);
    CHECK(os64_ui_action_invoke(g.views[2],BAR).status==OS64_UI_OK&&!g.bar.w.hidden);
    open_context(g.models[4],g.views[0]);key(popup_handle(),0,0x4d,0);CHECK(transport.windows[popup_handle()].owner!=1);
    key(popup_handle(),0x1b,0x01,0);CHECK(transport.windows[popup_handle()].owner==1);
    os64_ui_menu_close(g.menus,OS64_UI_MENU_EXPLICIT);drain();
}
static void placement_and_fonts(const char *dir)
{
    current="real fonts, placement, scroll and allocation failure";
    os64_gui_rect_t r;
    CHECK(os64_ui_menu_place((os64_gui_rect_t){1000,750,20,10},200,100,1024,768,true,&r)==OS64_UI_OK&&r.x==800&&r.y==667);
    CHECK(os64_ui_menu_place((os64_gui_rect_t){INT32_MIN,INT32_MIN,0,0},200,100,1024,768,false,&r)==OS64_UI_OK&&r.x==1&&r.y==1);
    CHECK(os64_ui_menu_place((os64_gui_rect_t){0},80,100,40,40,false,&r)==OS64_UI_LIMIT);
    os64_ui_popup_t *p=NULL;
    CHECK(os64_ui_popup_create(&g.ui,g.models[3],g.views[0],NULL,220,150,&p)==OS64_UI_OK);
    size_t last=g.models[3]->count-1;os64_ui_popup_select(p,last);
    CHECK(os64_ui_popup_row(p,last).h>0&&p->top>0);
    unsigned long before=allocations;
    canvas_t c; canvas_init(&c,0);
    os64_draw_ctx_t draw={.surf=c.s};
    p->w.cls->paint(&p->w,&draw,&g.ui.theme);CHECK(allocations==before);
    CHECK(strstr(p->rows[0].label,"\xe2\x80\xa6")!=NULL);
    os64_ui_popup_destroy(p);
    bool succeeded=false;unsigned failures_seen=0;
    for(long n=0;n<800;++n) {
        deny_countdown=n;
        os64_ui_status_code_t rc=os64_ui_popup_create(&g.ui,g.models[1],g.views[0],&gallery_icons,800,700,&p);
        deny_countdown=-1;
        if(rc==OS64_UI_OK) { os64_ui_popup_destroy(p);succeeded=true;break; }
        CHECK(rc==OS64_UI_NO_MEMORY&&!p);++failures_seen;
    }
    CHECK(succeeded&&failures_seen>6);
    transport.fail_create=true;
    CHECK(os64_ui_menu_open(g.menus,g.models[0],g.views[0],(os64_gui_rect_t){0},OS64_UI_MENU_CONTEXT)==OS64_UI_LIMIT);
    CHECK(!popup_handle()&&!os64_ui_menu_active(g.menus));transport.fail_create=false;
    transport.sw=70;transport.sh=40;
    CHECK(os64_ui_menu_open(g.menus,g.models[0],g.views[0],(os64_gui_rect_t){0},OS64_UI_MENU_CONTEXT)==OS64_UI_LIMIT);
    CHECK(!popup_handle());transport.sw=1024;transport.sh=768;
    open_context(g.models[0],g.views[0]);
    os64_font_set_t *set=NULL;
#ifdef UI_TEXT_REAL
    set=outline_set(os64_ui_font_context(&g.ui),dir,"DejaVuSans.ttf",32);
#else
    (void)dir;CHECK(os64_font_set_prepare(os64_ui_font_context(&g.ui),NULL,&set)==OS64_FONT_OK);
#endif
    CHECK(set!=NULL);os64_font_consumer_t consumer;os64_ui_font_consumer(&g.ui,&consumer);
    os64_font_set_t *prior=os64_ui_font_set(&g.ui);
    g.root.bounds.h=10;
    CHECK(os64_font_adopt(set,&consumer,1,NULL)==OS64_FONT_LIMIT&&!popup_handle());
    CHECK(os64_ui_font_set(&g.ui)==prior&&!os64_ui_menu_active(g.menus));drain();
    g.root.bounds.h=718;
    CHECK(os64_font_adopt(set,&consumer,1,NULL)==OS64_FONT_OK&&!popup_handle());
    CHECK(os64_ui_popup_create(&g.ui,g.models[1],g.views[0],&gallery_icons,900,700,&p)==OS64_UI_OK);
    os64_ui_widget_t *widget=os64_ui_popup_widget(p);
    widget->bounds.x=5;widget->bounds.y=7;
    uint32_t width=(uint32_t)widget->bounds.w+10,height=(uint32_t)widget->bounds.h+14;
    uint32_t *pixels=malloc((size_t)width*height*sizeof(*pixels));CHECK(pixels!=NULL);
    for(size_t i=0;i<(size_t)width*height;++i) pixels[i]=0xff010101;
    os64_draw_ctx_t target={.surf={pixels,width,height,width}};
    before=allocations;widget->cls->paint(widget,&target,&g.ui.theme);CHECK(allocations==before);
    os64_gui_rect_t checked=os64_ui_popup_row(p,1);bool mark=false,icon=false,outside=false;
    for(uint32_t y=0;y<height;++y) for(uint32_t x=0;x<width;++x) {
        uint32_t c=pixels[y*width+x];
        if((x<5||x>=width-5||y<7||y>=height-7)&&c!=0xff010101) outside=true;
        if(y>=(uint32_t)checked.y+7&&y<(uint32_t)(checked.y+checked.h)+7) {
            if(x>=8&&x<26&&c==g.ui.theme.menu_fg) mark=true;
            if(x>=26&&x<65&&((c&255)>((c>>16)&255)+30)&&(((c>>8)&255)>((c>>16)&255)+30)) icon=true;
        }
    }
    CHECK(mark&&icon&&!outside);free(pixels);
    os64_ui_popup_destroy(p);os64_font_set_release(set);
}
static void burst_and_pointer_navigation(void)
{
    current="PS/2/HID bursts and pointer cancellation";
    /* PS/2 extended navigation emits complete bursts without release events.
     * A Right used in bar mode must not suppress Right in its new popup. */
    key(1,0,0x44,0);
    const char right[]={0x1b,'[','C'};
    for(size_t i=0;i<3;++i) queue(1,edge(OS64_GUI_EVENT_KEY_DOWN,right[i],0x4d,0));
    drain();key(1,'\n',0x1c,0);
    int64_t first=popup_handle();CHECK(first);
    os64_ui_menu_close(g.menus,OS64_UI_MENU_EXPLICIT);drain();
    open_context(g.models[4],g.views[0]);first=popup_handle();
    for(size_t i=0;i<3;++i) queue(first,edge(OS64_GUI_EVENT_KEY_DOWN,right[i],0x4d,0));
    drain();CHECK(popup_handle()!=first&&transport.windows[popup_handle()].owner==first);
    os64_ui_menu_close(g.menus,OS64_UI_MENU_EXPLICIT);drain();
    for(unsigned hid=0;hid<2;++hid) {
        open_context(g.models[0],g.views[0]);int64_t h=popup_handle();
        uint8_t scan=hid?0x51:0x50,mods=hid?OS64_GUI_MOD_HID:0;
        const char burst[]={0x1b,'[','B'};
        for(size_t i=0;i<3;++i) queue(h,edge(OS64_GUI_EVENT_KEY_DOWN,burst[i],scan,mods));
        drain();CHECK(popup_handle());
        queue(h,edge(OS64_GUI_EVENT_KEY_UP,0,scan,mods));drain();
        key(h,'\n',hid?0x28:0x1c,mods);
        CHECK(!popup_handle()&&g.help_count>0);
    }
    unsigned hits=g.targets[0].hits;
    open_context(g.models[0],g.views[0]);int64_t h=popup_handle();
    queue(h,mouse(OS64_GUI_EVENT_MOUSE_BUTTON_DOWN,10,6,OS64_GUI_MOUSE_LEFT));
    queue(h,mouse(OS64_GUI_EVENT_MOUSE_MOVE,10,48,0));
    queue(h,mouse(OS64_GUI_EVENT_MOUSE_MOVE,10,6,0));
    queue(h,mouse(OS64_GUI_EVENT_MOUSE_BUTTON_UP,10,6,OS64_GUI_MOUSE_LEFT));drain();
    CHECK(g.targets[0].hits==hits&&popup_handle());
    os64_ui_menu_close(g.menus,OS64_UI_MENU_EXPLICIT);drain();
    /* Right click on an ordinary button remains unclaimed without a context
     * callback; the editor keeps its selection while the gallery context opens. */
    os64_ui_widget_t *w=&g.select[0];
    bool (*root_context)(os64_ui_widget_t *,const os64_gui_event_t *,void *)=g.root.on_context;
    g.root.on_context=NULL;
    os64_ui_set_focus(&g.ui,w);
    os64_gui_event_t keyboard=edge(OS64_GUI_EVENT_KEY_DOWN,0,0x44,OS64_GUI_MOD_SHIFT);
    CHECK(!g.ui.menu_event(&g.ui,&keyboard)&&!os64_ui_menu_active(g.menus));
    os64_gui_event_t e=mouse(OS64_GUI_EVENT_MOUSE_BUTTON_DOWN,w->bounds.x+1,w->bounds.y+1,OS64_GUI_MOUSE_RIGHT);
    CHECK(!os64_ui_dispatch(&g.ui,&e)&&!w->pressed);g.root.on_context=root_context;
    os64_ui_set_focus(&g.ui,&g.editor.w);
    g.editor.anchor=0;g.editor.cursor=6;
    e=mouse(OS64_GUI_EVENT_MOUSE_BUTTON_DOWN,g.editor.w.bounds.x+10,g.editor.w.bounds.y+5,OS64_GUI_MOUSE_RIGHT);
    queue(1,e);drain();CHECK(popup_handle()&&g.editor.anchor==0&&g.editor.cursor==6&&g.ui.focus==&g.editor.w);
    key(popup_handle(),'\t',0x0f,0);CHECK(!popup_handle()&&g.ui.focus!=&g.editor.w);
    CHECK(g.editor.anchor==0&&g.editor.cursor==6);
    os64_ui_set_focus(&g.ui,&g.content);
    os64_ui_menu_focus_fallback(g.menus,&g.content);
    os64_ui_set_focus(&g.ui,&g.select[0]);open_context(g.models[0],g.views[0]);
    os64_ui_set_hidden(&g.ui,&g.select[0],true);
    os64_ui_menu_close(g.menus,OS64_UI_MENU_EXPLICIT);drain();CHECK(g.ui.focus==&g.content);
    os64_ui_set_hidden(&g.ui,&g.select[0],false);
    key(1,0,0x44,OS64_GUI_MOD_SHIFT);CHECK(popup_handle());
    os64_ui_menu_close(g.menus,OS64_UI_MENU_EXPLICIT);drain();
    /* Same-task settings focus is external to the menu family. Covered alone
     * is a VT notification, and preserves the native panels. */
    open_context(g.models[0],g.views[0]);h=popup_handle();
    queue(h,(os64_gui_event_t){.type=OS64_GUI_EVENT_WINDOW_COVERED});drain();CHECK(popup_handle());
    transport.focused=1;queue(h,focus_edge(false,31));drain();CHECK(!popup_handle());
    unsigned down=transport.owner_down;
    queue(1,mouse(OS64_GUI_EVENT_MOUSE_BUTTON_DOWN,10,130,OS64_GUI_MOUSE_LEFT));drain();CHECK(transport.owner_down==down+1);
    os64_ui_cancel_gestures(&g.ui);
}
static void standalone(void)
{
    current="standalone shared controller";
    os64_ui_menu_session_dispose(g.menus);g.menus=NULL;
    CHECK(os64_ui_menu_session_create(&g.dispatcher,&g.ui,0,NULL,&g.status,NULL,&g.menus)==OS64_UI_INVALID&&!g.menus);
    os64_ui_t standalone_ui;os64_ui_init(&standalone_ui,NULL);standalone_ui.follow_session=false;
    CHECK(os64_ui_font_borrow_context(&standalone_ui,os64_ui_font_context(&g.ui))==OS64_FONT_OK);
    CHECK(os64_ui_font_bind(&standalone_ui,os64_ui_font_set(&g.ui))==OS64_FONT_OK);
    CHECK(os64_ui_menu_session_create(&g.dispatcher,&standalone_ui,0,NULL,&g.status,NULL,&g.menus)==OS64_UI_OK);
    CHECK(os64_ui_menu_open(g.menus,g.models[4],g.views[0],(os64_gui_rect_t){1000,700,0,0},OS64_UI_MENU_CONTEXT)==OS64_UI_OK);
    drain();
    int64_t root=transport.focused;CHECK(root>1&&transport.windows[root].owner==0);
    key(root,0,0x4d,0);CHECK(popup_handle()&&transport.windows[popup_handle()].owner==root);
    key(transport.focused,0,0x38,OS64_GUI_MOD_ALT);
    CHECK(!os64_ui_menu_active(g.menus));transport.focused=1;
    current="round two: standalone external zero-handle focus loss";
    for(unsigned cascades=0;cascades<2;++cascades) {
        CHECK(os64_ui_menu_open(g.menus,g.models[4],g.views[0],(os64_gui_rect_t){100,100,0,0},OS64_UI_MENU_CONTEXT)==OS64_UI_OK);
        drain();root=transport.focused;
        if(cascades) key(root,0,0x4d,0);
        int64_t focused=transport.focused;transport.focused=0;
        queue(focused,focus_edge(false,0));drain();
        CHECK(!os64_ui_menu_active(g.menus)&&!transport.windows[root].live);
        transport.focused=1;
    }
    os64_ui_menu_session_dispose(g.menus);g.menus=NULL;
    CHECK(os64_ui_font_release(&standalone_ui)==OS64_FONT_OK);
    CHECK(os64_ui_menu_session_create(&g.dispatcher,&g.ui,1,&g.bar,&g.status,&gallery_icons,&g.menus)==OS64_UI_OK);
    CHECK(os64_ui_menu_bar_view(g.menus,g.views[0])==OS64_UI_OK);
    os64_ui_menu_hooks(g.menus,gallery_menu_layout,gallery_menu_error,&g);
}
static unsigned callback_checks;
static os64_ui_action_result_t menu_callback(void *app,const os64_ui_action_context_t *ctx,uint32_t id)
{
    (void)app;(void)ctx;(void)id;
    CHECK(!popup_handle()&&!os64_ui_menu_active(g.menus));
    CHECK(!os64_ui_dispatcher_can_create(&g.dispatcher));
    ++callback_checks;g.ui.quit=true;
    return (os64_ui_action_result_t){OS64_UI_OK,0};
}
static void callback_and_icon_contract(void)
{
    current="callback teardown and icon leases";
    os64_ui_action_callbacks_t cb={at_query,menu_callback};
    os64_ui_actions_t *a=NULL;os64_ui_action_view_t *v=NULL;os64_ui_menu_model_t *m=NULL;
    CHECK(os64_ui_actions_create(at_specs,1,&cb,NULL,&a)==OS64_UI_OK);
    action_test_target target={.gen=1,.enabled=true};
    os64_ui_action_context_t ctx={&target,1,at_retain,at_release,at_valid};
    CHECK(os64_ui_action_view_create(a,&ctx,&v)==OS64_UI_OK);
    os64_ui_menu_entry_t e={.action=1};CHECK(os64_ui_menu_model_create(a,&e,1,&m)==OS64_UI_OK);
    open_context(m,v);key(popup_handle(),'\n',0x1c,0);
    CHECK(callback_checks==1&&g.ui.quit);g.ui.quit=false;
    os64_ui_menu_model_release(m);CHECK(os64_ui_action_view_destroy(v)==OS64_UI_OK);
    CHECK(os64_ui_actions_destroy(a)==OS64_UI_OK&&!target.refs);
    os64_ui_icon_lease_t lease={0};
    CHECK(os64_ui_icon_acquire(NULL,"synthetic-toggle",16,&lease)==OS64_UI_ICON_MISSING&&!lease.pixels);
    CHECK(os64_ui_icon_acquire(&gallery_icons,"../bad",16,&lease)==OS64_UI_ICON_INVALID&&!lease.pixels);
    CHECK(os64_ui_icon_acquire(&gallery_icons,"synthetic-toggle",16,&lease)==OS64_UI_ICON_OK);
    uint32_t original=lease.pixels[3*lease.pitch+3];
    canvas_t c;canvas_init(&c,0xff202020);
    unsigned long before=allocations;
    os64_ui_icon_draw(&c.s,(os64_gui_rect_t){0,0,16,16},&lease,false);
    os64_ui_icon_draw(&c.s,(os64_gui_rect_t){16,0,16,16},&lease,true);
    CHECK(c.px[3*SURF_W+3]!=c.px[3*SURF_W+19]);
    CHECK(lease.pixels[3*lease.pitch+3]==original&&allocations==before);
    os64_ui_icon_draw(&c.s,(os64_gui_rect_t){-8,-8,16,16},&lease,false);
    CHECK(c.px[31]==0xff202020);os64_ui_icon_release(&gallery_icons,&lease);CHECK(!lease.pixels);
    os64_ui_menu_session_t *session=NULL;
    /* Owning session allocation fails without attaching hooks. */
    os64_ui_menu_session_dispose(g.menus);g.menus=NULL;
    deny_countdown=0;
    CHECK(os64_ui_menu_session_create(&g.dispatcher,&g.ui,1,&g.bar,&g.status,NULL,&session)==OS64_UI_NO_MEMORY&&!session&&!g.ui.menu);
    CHECK(os64_ui_menu_session_create(&g.dispatcher,&g.ui,1,&g.bar,&g.status,NULL,&g.menus)==OS64_UI_OK);
    CHECK(os64_ui_menu_bar_view(g.menus,g.views[0])==OS64_UI_OK);
    os64_ui_menu_hooks(g.menus,gallery_menu_layout,gallery_menu_error,&g);
}

/* Contract checks run between routed events, not just after a gesture drains.
 * The native backend remains the seam; owner painting uses libui's dirty path. */
static bool audit_enabled;
static unsigned audit_boundaries,audit_sequences;
static void path_invariants(void)
{
    if(!audit_enabled||!g.menus) return;
    ++audit_boundaries;
    os64_ui_menu_session_t *s=g.menus;
    CHECK(s->depth<=OS64_UI_MENU_DEPTH);
    /* Active, a session carries a menu (its view) or a list, never both. */
    CHECK(s->active? (s->view!=NULL)!=(s->list!=NULL) : (!s->view&&!s->list&&!s->depth&&!s->pending&&!s->list_pending));
    CHECK(!s->list_pending||(s->list&&!s->depth));
    CHECK(g.bar.view==s->bar_view);
    if(g.bar.model&&g.bar.view) CHECK(os64_ui_action_view_registry(g.bar.view,g.bar.model->actions));
    if(s->active&&s->kind==OS64_UI_MENU_BAR&&!s->depth&&!s->pending)
        CHECK(s->bar&&menu_bar_enabled(s->bar,s->bar->selected));
    for(size_t i=0;i<s->depth;++i) {
        menu_panel *p=&s->panels[i];
        CHECK(p->widget&&p->handle>0&&p->index==i&&p->session==s);
        CHECK(os64_ui_dispatcher_live(&g.dispatcher,p->handle));
        CHECK(transport.windows[p->handle].live);
        CHECK(transport.windows[p->handle].owner==(i?s->panels[i-1].handle:s->owner));
        CHECK(p->widget->view==s->view);
        if(p->widget->list) {
            /* A list is one level, its popup reading the session's copy. */
            CHECK(i==0&&s->depth==1&&p->widget->list==s->list&&!p->widget->model);
            CHECK(p->widget->stage_count<=p->widget->stage_cap);
            CHECK(p->widget->selected==MENU_NONE||(p->widget->selected<s->list->count&&
                  os64_ui_popup_row(p->widget,p->widget->selected).h>0));
            continue;
        }
        CHECK(os64_ui_action_view_registry(p->widget->view,p->widget->model->actions));
        CHECK(p->widget->selected==MENU_NONE||p->widget->selected<p->widget->model->count);
        CHECK(p->widget->selected==MENU_NONE||os64_ui_popup_row(p->widget,p->widget->selected).h>0);
        if(i) {
            os64_ui_popup_t *parent=s->panels[i-1].widget;
            CHECK(parent->selected<parent->model->count);
            if(parent->selected<parent->model->count)
                CHECK(parent->model->entries[parent->selected].child==p->widget->model);
        }
    }
    if(s->pending) {
        CHECK(s->active&&s->pending_depth==s->depth);
        if(s->pending_depth) {
            os64_ui_popup_t *parent=s->panels[s->depth-1].widget;
            CHECK(parent->selected<parent->model->count);
            if(parent->selected<parent->model->count)
                CHECK(parent->model->entries[parent->selected].child==s->pending);
        }
    }
    for(size_t i=0;i<g.dispatcher.dead_count;++i)
        CHECK(!os64_ui_dispatcher_live(&g.dispatcher,g.dispatcher.tombstones[i]));
}
static uint64_t bar_pixels(void)
{
    os64_gui_rect_t r=g.bar.w.bounds;
    uint64_t hash=1469598103934665603ULL;
    for(int y=r.y;y<r.y+r.h;++y) for(int x=r.x;x<r.x+r.w;++x)
        hash=(hash^g.draw.surf.pixels[y*g.draw.surf.pitch_px+x])*1099511628211ULL;
    return hash;
}
static void owner_render(void *u)
{ gallery *a=u;os64_gui_rect_t damage;(void)os64_ui_render(&a->ui,&damage); }
static void external_loss(int64_t h,int64_t other)
{
    queue(h,focus_edge(false,other));drain();
    transport.focused=1;
    queue(1,focus_edge(true,other));drain();
}
static unsigned error_count;
static os64_ui_status_code_t last_error;
static void audit_error(void *u,os64_ui_status_code_t rc)
{ (void)u;++error_count;last_error=rc; }
static void review_round_four(void)
{
    os64_ui_menu_model_t *leaf=NULL,*parent=NULL,*top=NULL;
    os64_ui_menu_entry_t action={.action=RECORD};
    CHECK(os64_ui_menu_model_create(g.actions,&action,1,&leaf)==OS64_UI_OK);
    os64_ui_menu_entry_t headings[]={
        {.kind=OS64_UI_MENU_SUBMENU,.label="Record",.child=leaf},
        {.kind=OS64_UI_MENU_SUBMENU,.label="Always",.child=g.models[1]}};
    CHECK(os64_ui_menu_model_create(g.actions,headings,2,&parent)==OS64_UI_OK);
    os64_ui_menu_entry_t heading={.kind=OS64_UI_MENU_SUBMENU,.label="Branch",.child=parent};
    CHECK(os64_ui_menu_model_create(g.actions,&heading,1,&top)==OS64_UI_OK);
    audit_enabled=true;
    g.dispatcher.entries[0].paint=owner_render;
    os64_ui_menu_hooks(g.menus,gallery_menu_layout,audit_error,&g);
    /* Vary failure depth and poll cursor; refresh must preserve anchors through
     * refused and partially successful destruction, then commit on retry. */
    current="r4 I1/I2 refresh failure preserves surviving path";
    for(unsigned depth=1;depth<=2;++depth) for(unsigned order=0;order<2;++order) {
        ++audit_sequences;g.targets[0].enabled=true;
        CHECK(os64_ui_actions_refresh(g.views[0])==OS64_UI_OK);
        open_context(depth==1?parent:top,g.views[0]);
        key(popup_handle(),0,0x4d,0);
        if(depth==2) key(popup_handle(),0,0x4d,0);
        os64_ui_popup_t *p=g.menus->panels[depth-1].widget;
        size_t selected=p->selected;
        transport.fail_destroy=g.menus->panels[depth].handle;
        g.dispatcher.cursor=order;
        g.targets[0].enabled=false;
        CHECK(os64_ui_menu_refresh(g.menus)==OS64_UI_ERROR);
        CHECK(p->selected==selected);path_invariants();
        transport.fail_destroy=0;
        CHECK(os64_ui_menu_refresh(g.menus)==OS64_UI_OK);
        CHECK(g.menus->depth==depth&&p->selected==MENU_NONE);path_invariants();
        CHECK(os64_ui_menu_close(g.menus,OS64_UI_MENU_EXPLICIT)==OS64_UI_OK);drain();
    }
    current="r4 I5 production dirty rendering after refresh and view changes";
    CHECK(os64_ui_menubar_bind(&g.bar,parent)==OS64_UI_OK);
    g.targets[0].enabled=true;CHECK(os64_ui_actions_refresh(g.views[0])==OS64_UI_OK);
    CHECK(os64_ui_menu_bar_view(g.menus,g.views[0])==OS64_UI_OK);
    key(1,0,0x44,0);owner_render(&g);
    uint64_t pixels=bar_pixels();
    g.targets[0].enabled=false;
    CHECK(os64_ui_menu_refresh(g.menus)==OS64_UI_OK);
    os64_gui_rect_t damage={0},intersection;
    CHECK(os64_ui_render(&g.ui,&damage));
    CHECK(os64_rect_intersect(damage,g.bar.w.bounds,&intersection));
    CHECK(bar_pixels()!=pixels);
    CHECK(os64_ui_menu_close(g.menus,OS64_UI_MENU_EXPLICIT)==OS64_UI_OK);drain();
    g.targets[0].enabled=true;CHECK(os64_ui_actions_refresh(g.views[0])==OS64_UI_OK);
    CHECK(os64_ui_menu_bar_view(g.menus,g.views[0])==OS64_UI_OK);owner_render(&g);
    pixels=bar_pixels();
    CHECK(os64_ui_menu_bar_view(g.menus,g.views[1])==OS64_UI_OK);
    CHECK(os64_ui_render(&g.ui,&damage));CHECK(bar_pixels()!=pixels);
    CHECK(os64_ui_menu_bar_view(g.menus,g.views[1])==OS64_UI_OK);
    CHECK(!os64_ui_render(&g.ui,&damage));
    g.targets[1].enabled=true;CHECK(os64_ui_actions_refresh(g.views[1])==OS64_UI_OK);
    pixels=bar_pixels();CHECK(os64_ui_menu_bar_view(g.menus,g.views[1])==OS64_UI_OK);
    CHECK(os64_ui_render(&g.ui,&damage));CHECK(bar_pixels()!=pixels);
    CHECK(os64_ui_menu_bar_view(g.menus,g.views[0])==OS64_UI_OK);owner_render(&g);
    pixels=bar_pixels();open_context(leaf,g.views[0]);owner_render(&g);
    CHECK(bar_pixels()==pixels); /* Context invocation has no bar highlight. */
    CHECK(os64_ui_menu_close(g.menus,OS64_UI_MENU_EXPLICIT)==OS64_UI_OK);drain();
    CHECK(os64_ui_menubar_bind(&g.bar,g.models[5])==OS64_UI_OK);
    g.targets[1].enabled=false;
    CHECK(os64_ui_actions_refresh(g.views[1])==OS64_UI_OK);

    /* Deliberately interrupt down/up pairs at each delivery-loss transition.
     * Complete key() helpers cannot expose these stranded repeat guards. */
    current="r4 I4 interrupted input pairing";
    const uint32_t loss[]={OS64_GUI_EVENT_WINDOW_FOCUS,OS64_GUI_EVENT_INPUT_LOST,OS64_GUI_EVENT_WINDOW_COVERED};
    for(size_t which=0;which<3;++which) for(unsigned gesture=0;gesture<4;++gesture) for(unsigned order=0;order<2;++order) {
        ++audit_sequences;g.dispatcher.cursor=order;
        open_context(leaf,g.views[0]);int64_t h=popup_handle();
        os64_gui_event_t down=gesture==0?edge(OS64_GUI_EVENT_KEY_DOWN,0,0x44,0):
            gesture==1?edge(OS64_GUI_EVENT_KEY_DOWN,'\n',0x1c,0):
            gesture==2?edge(OS64_GUI_EVENT_KEY_DOWN,'d',0x20,OS64_GUI_MOD_CTRL):
            edge(OS64_GUI_EVENT_KEY_DOWN,0,0x38,OS64_GUI_MOD_ALT);
        queue(h,down);drain();
        if(loss[which]==OS64_GUI_EVENT_WINDOW_FOCUS) external_loss(transport.focused,order?31:0);
        else { queue(transport.focused,(os64_gui_event_t){.type=loss[which]});drain(); }
        CHECK(!g.menus->f10_held&&!g.menus->alt_armed&&!g.menus->suppress_pending&&!g.menus->suppress_button);
        CHECK(!g.menus->pressed_key&&g.menus->pressed_panel==MENU_NONE);
        if(which==2&&(gesture==1||gesture==3)) CHECK(os64_ui_menu_active(g.menus)&&popup_handle());
        for(size_t i=0;i<sizeof(g.menus->held);++i) CHECK(!g.menus->held[i]);
        CHECK(os64_ui_menu_close(g.menus,OS64_UI_MENU_EXPLICIT)==OS64_UI_OK);drain();
        if(gesture==0) {
            key(1,0,0x44,0);CHECK(os64_ui_menu_active(g.menus));
        } else if(gesture==3) {
            key(1,0,0x38,OS64_GUI_MOD_ALT);CHECK(os64_ui_menu_active(g.menus));
        } else {
            open_context(leaf,g.views[0]);unsigned hits=g.targets[0].hits;
            key(popup_handle(),down.key.ascii,down.key.scancode,down.key.modifiers);
            CHECK(g.targets[0].hits==hits+1&&!os64_ui_menu_active(g.menus));
        }
        CHECK(os64_ui_menu_close(g.menus,OS64_UI_MENU_EXPLICIT)==OS64_UI_OK);drain();
    }
    current="r4 I4 owner dismissal preserves key pair until external loss";
    open_context(leaf,g.views[0]);
    queue(popup_handle(),edge(OS64_GUI_EVENT_KEY_DOWN,'\n',0x1c,0));drain();
    int64_t h=popup_handle();transport.focused=1;
    queue(h,focus_edge(false,1));drain();
    CHECK(g.menus->held[0x1c/8]&(1u<<(0x1c%8)));
    CHECK(!os64_ui_menu_active(g.menus));
    unsigned hits=g.targets[0].hits;
    queue(1,edge(OS64_GUI_EVENT_KEY_UP,'\n',0x1c,0));drain();CHECK(g.targets[0].hits==hits);

    current="r4 I2 partial refresh teardown preserves the remaining anchor";
    os64_ui_menu_model_t *mid=NULL,*chain=NULL;
    os64_ui_menu_entry_t single={.kind=OS64_UI_MENU_SUBMENU,.label="Leaf",.child=leaf};
    CHECK(os64_ui_menu_model_create(g.actions,&single,1,&mid)==OS64_UI_OK);
    single.child=mid;CHECK(os64_ui_menu_model_create(g.actions,&single,1,&chain)==OS64_UI_OK);
    open_context(chain,g.views[0]);key(popup_handle(),0,0x4d,0);key(popup_handle(),0,0x4d,0);
    int64_t middle=g.menus->panels[1].handle,deepest=g.menus->panels[2].handle;
    transport.fail_destroy=middle;g.targets[0].enabled=false;
    CHECK(os64_ui_menu_refresh(g.menus)==OS64_UI_ERROR);
    CHECK(g.menus->depth==2&&!transport.windows[deepest].live);
    CHECK(g.menus->panels[0].widget->selected==0);path_invariants();
    transport.fail_destroy=0;CHECK(os64_ui_menu_refresh(g.menus)==OS64_UI_OK);
    CHECK(g.menus->depth==1&&g.menus->panels[0].widget->selected==MENU_NONE);
    CHECK(os64_ui_menu_close(g.menus,OS64_UI_MENU_EXPLICIT)==OS64_UI_OK);drain();
    os64_ui_menu_model_release(chain);os64_ui_menu_model_release(mid);
    g.targets[0].enabled=true;CHECK(os64_ui_actions_refresh(g.views[0])==OS64_UI_OK);

    current="r4 I2 failed bar replacement preserves heading and anchor";
    CHECK(os64_ui_menubar_bind(&g.bar,parent)==OS64_UI_OK);
    g.bar.selected=0;key(1,0,0x44,0);key(1,0,0x50,0);
    int64_t old_root=popup_handle();transport.fail_destroy=old_root;
    os64_gui_rect_t anchor=g.menus->anchor;
    key(old_root,0,0x4d,0);
    CHECK(g.bar.selected==0&&popup_handle()==old_root);
    CHECK(!memcmp(&g.menus->anchor,&anchor,sizeof(anchor)));path_invariants();
    queue(1,mouse(OS64_GUI_EVENT_MOUSE_MOVE,g.bar.w.bounds.x+g.bar.widths[0]+2*MENU_GAP+2,g.bar.w.bounds.y+2,0));drain();
    CHECK(g.bar.selected==0&&popup_handle()==old_root);
    transport.fail_destroy=0;CHECK(os64_ui_menu_close(g.menus,OS64_UI_MENU_EXPLICIT)==OS64_UI_OK);drain();
    CHECK(os64_ui_menubar_bind(&g.bar,g.models[5])==OS64_UI_OK);

    current="r4 I6 event-only close failures report once and remain retryable";
    const uint8_t refusals[]={0x44,0x01,0x0f,0x1c};
    for(size_t i=0;i<sizeof(refusals);++i) {
        open_context(leaf,g.views[0]);int64_t popup=popup_handle();
        transport.fail_destroy=popup;unsigned errors=error_count,hits=g.targets[0].hits;
        key(popup,refusals[i]==0x01?0x1b:refusals[i]==0x1c?'\n':0,refusals[i],0);
        CHECK(error_count==errors+1&&last_error==OS64_UI_ERROR);
        CHECK(g.targets[0].hits==hits&&popup_handle()==popup);path_invariants();
        transport.fail_destroy=0;CHECK(os64_ui_menu_close(g.menus,OS64_UI_MENU_EXPLICIT)==OS64_UI_OK);drain();
    }

    current="r4 I3 pending grandchild interrupted before queue boundary";
    const uint8_t interrupts[]={0x01,0x4b,0,1};
    for(unsigned order=0;order<2;++order) for(size_t interrupt=0;interrupt<4;++interrupt) {
        ++audit_sequences;
        open_context(top,g.views[0]);key(popup_handle(),0,0x4d,0);
        int64_t parent_handle=popup_handle();key(parent_handle,0,0x4d,0);
        unsigned births=transport.creates;
        os64_ui_popup_t *p=g.menus->panels[1].widget;
        os64_gui_rect_t row=os64_ui_popup_row(p,1);
        for(unsigned i=0;i<150;++i) queue(1,(os64_gui_event_t){.type=OS64_GUI_EVENT_DOORBELL});
        queue(parent_handle,mouse(OS64_GUI_EVENT_MOUSE_MOVE,10,row.y+2,0));
        g.dispatcher.cursor=order;
        CHECK(os64_ui_dispatcher_turn(&g.dispatcher,false)==64);
        CHECK(g.menus->pending&&g.menus->depth==2&&transport.creates==births);path_invariants();
        if(interrupt<2) queue(parent_handle,edge(OS64_GUI_EVENT_KEY_DOWN,interrupt?0:0x1b,interrupts[interrupt],0));
        else if(interrupt==2) queue(parent_handle,(os64_gui_event_t){.type=OS64_GUI_EVENT_INPUT_LOST});
        else queue(parent_handle,focus_edge(false,31));
        drain();CHECK(!g.menus->pending&&transport.creates==births);
        CHECK(g.menus->depth==(interrupt<2?1:0));path_invariants();
        if(interrupt<2) queue(transport.focused,edge(OS64_GUI_EVENT_KEY_UP,interrupt?0:0x1b,interrupts[interrupt],0));
        CHECK(os64_ui_menu_close(g.menus,OS64_UI_MENU_EXPLICIT)==OS64_UI_OK);drain();
    }
    current="r4 I4 mouse dismissal loses its release outside the family";
    for(unsigned order=0;order<2;++order) for(unsigned covered=0;covered<2;++covered) {
        ++audit_sequences;open_context(leaf,g.views[0]);
        transport.focused=1;int64_t popup=popup_handle();
        queue(popup,focus_edge(false,1));queue(1,focus_edge(true,popup));
        queue(1,mouse(OS64_GUI_EVENT_MOUSE_BUTTON_DOWN,800,600,OS64_GUI_MOUSE_LEFT));
        g.dispatcher.cursor=order;drain();CHECK(g.menus->suppress_button==OS64_GUI_MOUSE_LEFT+1);
        if(covered) { queue(1,(os64_gui_event_t){.type=OS64_GUI_EVENT_WINDOW_COVERED});drain(); }
        else external_loss(1,31);
        unsigned ups=transport.owner_up;
        queue(1,mouse(OS64_GUI_EVENT_MOUSE_BUTTON_UP,800,600,OS64_GUI_MOUSE_LEFT));drain();
        CHECK(transport.owner_up==ups+1&&!g.menus->suppress_button);
    }
    dismissal_orders();collapse_pending_child();

    current="r4 I5/I6 stale snapshot repaint and observable bar request";
    CHECK(os64_ui_menubar_bind(&g.bar,parent)==OS64_UI_OK);
    CHECK(os64_ui_menu_bar_view(g.menus,g.views[0])==OS64_UI_OK);owner_render(&g);
    pixels=bar_pixels();++g.targets[0].generation;
    key(1,0,0x44,0);CHECK(bar_pixels()!=pixels);
    current="r4 I6 stale Alt/F10 request has one error per fresh gesture";
    for(unsigned hidden=0;hidden<2;++hidden) for(unsigned alt=0;alt<2;++alt) {
        ++audit_sequences;
        os64_ui_set_hidden(&g.ui,&g.bar.w,hidden!=0);gallery_menu_layout(&g);
        unsigned errors=error_count;
        key(1,0,alt?0x38:0x44,alt?OS64_GUI_MOD_ALT:0);
        CHECK(error_count==errors+1&&last_error==OS64_UI_STALE);
        CHECK(!os64_ui_menu_active(g.menus)&&g.bar.w.hidden==(hidden!=0));
    }
    unsigned errors=error_count;
    queue(1,edge(OS64_GUI_EVENT_KEY_DOWN,0,0x44,0));
    queue(1,edge(OS64_GUI_EVENT_KEY_DOWN,0,0x44,0));drain();
    CHECK(error_count==errors+1);
    queue(1,edge(OS64_GUI_EVENT_KEY_UP,0,0x44,0));drain();
    key(1,0,0x44,0);CHECK(error_count==errors+2);
    --g.targets[0].generation;
    os64_ui_set_hidden(&g.ui,&g.bar.w,false);gallery_menu_layout(&g);
    CHECK(os64_ui_actions_refresh(g.views[0])==OS64_UI_OK);
    CHECK(os64_ui_menubar_bind(&g.bar,g.models[5])==OS64_UI_OK);
    audit_enabled=false;g.dispatcher.entries[0].paint=no_paint;
    os64_ui_menu_hooks(g.menus,gallery_menu_layout,gallery_menu_error,&g);
    os64_ui_menu_model_release(top);os64_ui_menu_model_release(parent);os64_ui_menu_model_release(leaf);
    printf("r4 transitions: %u sequences, %u invariant boundaries\n",audit_sequences,audit_boundaries);
}

static bool error_active;
static size_t error_depth;
static void lifecycle_error(void *u,os64_ui_status_code_t rc)
{
    audit_error(u,rc);
    error_active=os64_ui_menu_active(g.menus);error_depth=g.menus->depth;
    path_invariants();
}
static void deferred_error_precedence(void)
{
    current="r5 I2/I6 report deferred failure after close, prioritizing teardown";
    for(unsigned allocation=0;allocation<2;++allocation) for(unsigned refused=0;refused<2;++refused) {
        open_context(g.models[4],g.views[0]);int64_t root=popup_handle();
        key(root,0,0x4d,0);
        os64_ui_popup_t *p=g.menus->panels[0].widget;
        os64_gui_rect_t row=os64_ui_popup_row(p,1);
        for(unsigned i=0;i<150;++i) queue(1,(os64_gui_event_t){.type=OS64_GUI_EVENT_DOORBELL});
        queue(root,mouse(OS64_GUI_EVENT_MOUSE_MOVE,10,row.y+2,0));
        CHECK(os64_ui_dispatcher_turn(&g.dispatcher,false)==64);
        CHECK(g.menus->pending&&g.menus->depth==1);path_invariants();
        unsigned errors=error_count;
        transport.fail_destroy=refused?root:0;
        if(allocation) deny_countdown=0; else transport.fail_create=true;
        drain();deny_countdown=-1;transport.fail_create=false;
        CHECK(error_count==errors+1);
        CHECK(last_error==(refused?OS64_UI_ERROR:allocation?OS64_UI_NO_MEMORY:OS64_UI_LIMIT));
        CHECK(error_active==(refused!=0)&&error_depth==refused);
        CHECK(g.menus->depth==refused&&!g.menus->pending);
        transport.fail_destroy=0;
        CHECK(os64_ui_menu_close(g.menus,OS64_UI_MENU_EXPLICIT)==OS64_UI_OK);drain();
    }
    current="r5 I2/I6 overflow preparation failure also reports teardown first";
    key(1,0,0x44,0);key(1,0,0x50,0);
    int64_t root=popup_handle();CHECK(root>0);
    transport.fail_destroy=root;g.bar.w.bounds.w=1;
    unsigned errors=error_count;deny_countdown=0;
    queue(1,mouse(OS64_GUI_EVENT_MOUSE_BUTTON_DOWN,g.bar.w.bounds.x,g.bar.w.bounds.y+2,OS64_GUI_MOUSE_LEFT));drain();deny_countdown=-1;
    CHECK(error_count==errors+1&&last_error==OS64_UI_ERROR&&error_active);
    transport.fail_destroy=0;CHECK(os64_ui_menu_close(g.menus,OS64_UI_MENU_EXPLICIT)==OS64_UI_OK);drain();gallery_layout(&g,false);
}
static void refreshed_bar_selection(void)
{
    current="r5 I5/I9 refresh keeps bar-only selection usable";
    os64_ui_menu_model_t *leaf=NULL,*headings=NULL;
    os64_ui_menu_entry_t action={.action=RECORD};
    CHECK(os64_ui_menu_model_create(g.actions,&action,1,&leaf)==OS64_UI_OK);
    os64_ui_menu_entry_t entries[]={
        {.kind=OS64_UI_MENU_SUBMENU,.label="Target",.child=leaf},
        {.kind=OS64_UI_MENU_SUBMENU,.label="Other",.child=g.models[1]}};
    CHECK(os64_ui_menu_model_create(g.actions,entries,2,&headings)==OS64_UI_OK);
    CHECK(os64_ui_menubar_bind(&g.bar,headings)==OS64_UI_OK);
    for(unsigned hidden=0;hidden<2;++hidden) for(unsigned alt=0;alt<2;++alt) {
        g.targets[0].enabled=true;CHECK(os64_ui_actions_refresh(g.views[0])==OS64_UI_OK);
        g.bar.selected=0;os64_ui_set_hidden(&g.ui,&g.bar.w,hidden!=0);gallery_layout(&g,false);
        key(1,0,alt?0x38:0x44,alt?OS64_GUI_MOD_ALT:0);
        CHECK(g.menus->active&&!g.menus->depth&&!g.menus->pending);
        g.targets[0].enabled=false;
        CHECK(os64_ui_menu_refresh(g.menus)==OS64_UI_OK);
        CHECK(g.menus->active&&g.bar.selected==1&&menu_bar_enabled(&g.bar,g.bar.selected));
        key(1,0,0x50,0);CHECK(g.menus->depth&&g.menus->panels[0].widget->model==g.models[1]);
        CHECK(os64_ui_menu_close(g.menus,OS64_UI_MENU_EXPLICIT)==OS64_UI_OK);drain();
    }
    /* Same-view publication in an application's paint callback is another
     * state publication route; it must normalize without pumping events. */
    os64_ui_set_hidden(&g.ui,&g.bar.w,false);gallery_layout(&g,false);
    g.targets[0].enabled=true;CHECK(os64_ui_actions_refresh(g.views[0])==OS64_UI_OK);
    g.bar.selected=0;key(1,0,0x44,0);
    g.targets[0].enabled=false;CHECK(os64_ui_actions_refresh(g.views[0])==OS64_UI_OK);
    CHECK(os64_ui_menu_bar_view(g.menus,g.views[0])==OS64_UI_OK);
    CHECK(g.bar.selected==1&&menu_bar_enabled(&g.bar,g.bar.selected));
    CHECK(os64_ui_menu_close(g.menus,OS64_UI_MENU_EXPLICIT)==OS64_UI_OK);drain();

    current="r5 I3/I5 refresh cancels an invalid pending bar root before normalization";
    g.targets[0].enabled=true;CHECK(os64_ui_actions_refresh(g.views[0])==OS64_UI_OK);
    open_context(leaf,g.views[0]);g.bar.selected=0;
    for(unsigned i=0;i<150;++i) queue(1,(os64_gui_event_t){.type=OS64_GUI_EVENT_DOORBELL});
    unsigned births=transport.creates;
    CHECK(os64_ui_menu_open(g.menus,leaf,g.views[0],(os64_gui_rect_t){100,100,0,0},OS64_UI_MENU_BAR)==OS64_UI_BUSY);
    g.targets[0].enabled=false;CHECK(os64_ui_menu_refresh(g.menus)==OS64_UI_OK);
    CHECK(g.menus->active&&!g.menus->pending&&!g.menus->depth&&g.bar.selected==1);
    drain();CHECK(transport.creates==births);key(1,0,0x50,0);
    CHECK(g.menus->depth&&g.menus->panels[0].widget->model==g.models[1]);
    CHECK(os64_ui_menu_close(g.menus,OS64_UI_MENU_EXPLICIT)==OS64_UI_OK);drain();

    os64_ui_menu_model_release(headings);
    CHECK(os64_ui_menu_model_create(g.actions,entries,1,&headings)==OS64_UI_OK);
    CHECK(os64_ui_menubar_bind(&g.bar,headings)==OS64_UI_OK);
    g.targets[0].enabled=true;CHECK(os64_ui_actions_refresh(g.views[0])==OS64_UI_OK);
    os64_ui_set_hidden(&g.ui,&g.bar.w,true);gallery_layout(&g,false);
    g.bar.selected=0;key(1,0,0x44,0);g.targets[0].enabled=false;
    CHECK(os64_ui_menu_refresh(g.menus)==OS64_UI_OK);
    CHECK(!g.menus->active&&!g.menus->view&&!g.bar.temporary&&g.bar.w.hidden);
    CHECK(os64_ui_menu_close(g.menus,OS64_UI_MENU_EXPLICIT)==OS64_UI_OK);drain();
    /* Popup refresh also needs owner damage when bar selection is unchanged. */
    g.targets[0].enabled=true;CHECK(os64_ui_actions_refresh(g.views[0])==OS64_UI_OK);
    os64_ui_set_hidden(&g.ui,&g.bar.w,false);gallery_layout(&g,false);
    CHECK(os64_ui_menu_bar_view(g.menus,g.views[0])==OS64_UI_OK);owner_render(&g);
    open_context(leaf,g.views[0]);owner_render(&g);uint64_t pixels=bar_pixels();
    g.targets[0].enabled=false;CHECK(os64_ui_menu_refresh(g.menus)==OS64_UI_OK);
    os64_gui_rect_t damage={0},intersection;CHECK(os64_ui_render(&g.ui,&damage));
    CHECK(os64_rect_intersect(damage,g.bar.w.bounds,&intersection));CHECK(bar_pixels()!=pixels);
    CHECK(os64_ui_menu_close(g.menus,OS64_UI_MENU_EXPLICIT)==OS64_UI_OK);drain();
    CHECK(os64_ui_menubar_bind(&g.bar,g.models[5])==OS64_UI_OK);
    os64_ui_menu_model_release(headings);os64_ui_menu_model_release(leaf);
    g.targets[0].enabled=true;CHECK(os64_ui_actions_refresh(g.views[0])==OS64_UI_OK);
}
static void scroll_noop_and_tail(void)
{
    current="r5 I1/I3 no-op wheel preserves a short parent's child path";
    open_context(g.models[4],g.views[0]);int64_t root=popup_handle();key(root,0,0x4d,0);
    int64_t child_handle=popup_handle();unsigned deaths=transport.destroys;uint64_t token=g.menus->help_token;
    for(int rows=-1;rows<=1;++rows) {
        os64_gui_event_t wheel={.type=OS64_GUI_EVENT_MOUSE_WHEEL};wheel.mouse.dy=rows;
        queue(root,wheel);drain();CHECK(popup_handle()==child_handle&&transport.destroys==deaths);
        CHECK(g.menus->help_token==token);
    }
    CHECK(os64_ui_menu_close(g.menus,OS64_UI_MENU_EXPLICIT)==OS64_UI_OK);drain();
    current="r5 I3 no-op wheel preserves pending child intent";
    open_context(g.models[4],g.views[0]);root=popup_handle();key(root,0,0x4d,0);
    os64_ui_popup_t *parent=g.menus->panels[0].widget;
    os64_gui_rect_t row=os64_ui_popup_row(parent,1);
    for(unsigned i=0;i<150;++i) queue(1,(os64_gui_event_t){.type=OS64_GUI_EVENT_DOORBELL});
    queue(root,mouse(OS64_GUI_EVENT_MOUSE_MOVE,10,row.y+2,0));
    CHECK(os64_ui_dispatcher_turn(&g.dispatcher,false)==64);
    CHECK(g.menus->pending&&g.menus->pending==g.models[3]);unsigned births=transport.creates;
    os64_gui_event_t wheel={.type=OS64_GUI_EVENT_MOUSE_WHEEL};wheel.mouse.dy=1;
    queue(root,wheel);drain();
    CHECK(transport.creates==births+1&&g.menus->depth==2&&g.menus->panels[1].widget->model==g.models[3]);
    CHECK(os64_ui_menu_close(g.menus,OS64_UI_MENU_EXPLICIT)==OS64_UI_OK);drain();

    os64_ui_menu_model_t *tall=NULL;
    os64_ui_menu_entry_t entries[50];
    for(unsigned i=0;i<50;++i) entries[i]=(os64_ui_menu_entry_t){.kind=OS64_UI_MENU_SUBMENU,.label="Child",.child=g.models[2]};
    CHECK(os64_ui_menu_model_create(g.actions,entries,50,&tall)==OS64_UI_OK);
    transport.sh=220;
    for(unsigned end=0;end<2;++end) for(unsigned affordance=0;affordance<2;++affordance) {
        current="r5 I1/I3 clamped viewport preserves child and help";
        open_context(tall,g.views[0]);root=popup_handle();
        if(end) key(root,0,0x4f,0);
        key(root,0,0x4d,0);child_handle=popup_handle();deaths=transport.destroys;token=g.menus->help_token;
        if(affordance) {
            os64_ui_popup_t *p=g.menus->panels[0].widget;
            queue(root,mouse(OS64_GUI_EVENT_MOUSE_BUTTON_DOWN,10,end?p->w.bounds.h-1:1,OS64_GUI_MOUSE_LEFT));
        } else {
            os64_gui_event_t wheel={.type=OS64_GUI_EVENT_MOUSE_WHEEL};wheel.mouse.dy=end?1:-1;queue(root,wheel);
        }
        drain();CHECK(popup_handle()==child_handle&&transport.destroys==deaths&&g.menus->help_token==token);
        /* An actual viewport change must close children; failed destruction
         * leaves the old anchor and top intact until retry. */
        transport.fail_destroy=child_handle;
        size_t old_top=g.menus->panels[0].widget->top;
        os64_gui_event_t move={.type=OS64_GUI_EVENT_MOUSE_WHEEL};move.mouse.dy=end?-1:1;
        queue(root,move);drain();CHECK(g.menus->depth==2&&g.menus->panels[0].widget->top==old_top);
        transport.fail_destroy=0;queue(root,move);drain();CHECK(g.menus->depth==1&&g.menus->panels[0].widget->top!=old_top);
        CHECK(os64_ui_menu_close(g.menus,OS64_UI_MENU_EXPLICIT)==OS64_UI_OK);drain();
    }
    transport.sh=768;os64_ui_menu_model_release(tall);

    current="r5 I9 final viewport packs mixed-height rows";
    for(unsigned i=0;i<50;++i) entries[i]=i%3==1?(os64_ui_menu_entry_t){.kind=OS64_UI_MENU_SEPARATOR}:(os64_ui_menu_entry_t){.action=100+i};
    CHECK(os64_ui_menu_model_create(g.actions,entries,50,&tall)==OS64_UI_OK);
    const int heights[]={150,183,220};
    for(size_t size=0;size<sizeof(heights)/sizeof(*heights);++size) {
        os64_ui_popup_t *p=NULL;
        CHECK(os64_ui_popup_create(&g.ui,tall,g.views[0],NULL,400,heights[size],&p)==OS64_UI_OK);
        os64_ui_popup_scroll(p,INT32_MAX);
        CHECK(p->bottom==p->model->count&&p->bottom-p->top>1);
        int used=0;
        for(size_t i=p->top;i<p->bottom;++i) used+=os64_ui_popup_row(p,i).h;
        int room=p->w.bounds.h-2*p->row_h;
        CHECK(used<=room);
        if(p->top) {
            int preceding=p->model->entries[p->top-1].kind==OS64_UI_MENU_SEPARATOR?MENU_SEP:p->row_h;
            CHECK(used+preceding>room); /* No earlier row fits in the tail. */
        }
        size_t tail=p->top;os64_ui_popup_scroll(p,INT32_MAX);CHECK(p->top==tail);
        os64_ui_popup_scroll(p,INT32_MIN);CHECK(!p->top);
        for(unsigned i=0;i<100;++i) os64_ui_popup_scroll(p,1);
        CHECK(p->top==tail&&p->bottom==p->model->count);
        os64_ui_popup_select(p,p->model->count-1);CHECK(p->top==tail);
        os64_ui_popup_destroy(p);
    }
    os64_ui_menu_model_release(tall);
}
static void review_round_five(void)
{
    audit_enabled=true;
    os64_ui_menu_hooks(g.menus,gallery_menu_layout,lifecycle_error,&g);
    deferred_error_precedence();refreshed_bar_selection();scroll_noop_and_tail();
    audit_enabled=false;
    os64_ui_menu_hooks(g.menus,gallery_menu_layout,gallery_menu_error,&g);
}

static void registry_rebind(void)
{
    current="r6 I11 bar model and retained view share registry identity";
    os64_ui_actions_t *foreign=NULL;os64_ui_action_view_t *foreign_view=NULL;
    CHECK(os64_ui_actions_create(at_specs,1,&at_cb,NULL,&foreign)==OS64_UI_OK);
    action_test_target target={.gen=1,.enabled=false};
    os64_ui_action_context_t context={&target,1,at_retain,at_release,at_valid};
    CHECK(os64_ui_action_view_create(foreign,&context,&foreign_view)==OS64_UI_OK);
    os64_ui_menu_model_t *leaf=NULL,*headings=NULL;
    os64_ui_menu_entry_t command={.action=RECORD};
    CHECK(os64_ui_menu_model_create(foreign,&command,1,&leaf)==OS64_UI_OK);
    os64_ui_menu_entry_t heading={.kind=OS64_UI_MENU_SUBMENU,.label="Colliding ID",.child=leaf};
    CHECK(os64_ui_menu_model_create(foreign,&heading,1,&headings)==OS64_UI_OK);
    void *caption=g.bar.labels[0];os64_ui_menu_model_t *old=g.bar.model;
    os64_ui_action_view_t *view=g.bar.view;size_t old_refs=old->refs,foreign_refs=headings->refs;
    CHECK(os64_ui_menubar_bind(&g.bar,headings)==OS64_UI_INVALID);
    CHECK(g.bar.model==old&&g.bar.labels[0]==caption&&g.bar.view==view);
    CHECK(old->refs==old_refs&&headings->refs==foreign_refs);
    /* Restore the fixture if the reviewed implementation accepted the pair. */
    if(g.bar.model!=old) CHECK(os64_ui_menubar_bind(&g.bar,old)==OS64_UI_OK);
    unsigned long before=allocations;deny_countdown=0;
    CHECK(os64_ui_menubar_bind(&g.bar,headings)==OS64_UI_INVALID);
    deny_countdown=-1;CHECK(allocations==before&&g.bar.model==old);
    CHECK(os64_ui_menu_bar_view(g.menus,foreign_view)==OS64_UI_INVALID&&g.bar.view==view);
    open_context(g.models[0],g.views[0]);int64_t root=popup_handle();
    os64_ui_status_code_t rc=os64_ui_menubar_bind(&g.bar,headings);
    CHECK(rc==OS64_UI_INVALID||rc==OS64_UI_BUSY);
    CHECK(g.bar.model==old&&g.menus->view==g.views[0]&&popup_handle()==root);
    CHECK(os64_ui_menu_close(g.menus,OS64_UI_MENU_EXPLICIT)==OS64_UI_OK);drain();
    os64_ui_menu_model_release(headings);os64_ui_menu_model_release(leaf);
    CHECK(os64_ui_action_view_destroy(foreign_view)==OS64_UI_OK);
    CHECK(os64_ui_actions_destroy(foreign)==OS64_UI_OK&&!target.refs);
}
static void request_freshness(void)
{
    os64_ui_menu_model_t *leaf=NULL,*headings=NULL,*single=NULL;
    os64_ui_menu_entry_t action={.action=RECORD};
    CHECK(os64_ui_menu_model_create(g.actions,&action,1,&leaf)==OS64_UI_OK);
    os64_ui_menu_entry_t entries[]={
        {.kind=OS64_UI_MENU_SUBMENU,.label="Target",.mnemonic='r',.child=leaf},
        {.kind=OS64_UI_MENU_SUBMENU,.label="Other",.mnemonic='o',.child=g.models[1]}};
    CHECK(os64_ui_menu_model_create(g.actions,entries,2,&headings)==OS64_UI_OK);
    CHECK(os64_ui_menu_model_create(g.actions,entries,1,&single)==OS64_UI_OK);
    current="r6 I5/I9 opening decisions use the freshly queried snapshot";
    for(unsigned route=0;route<3;++route) for(unsigned enabled=0;enabled<2;++enabled) for(unsigned hidden=0;hidden<2;++hidden) {
        if(route&&hidden) continue;
        CHECK(os64_ui_menubar_bind(&g.bar,route==2?single:headings)==OS64_UI_OK);
        g.targets[0].enabled=!enabled;CHECK(os64_ui_actions_refresh(g.views[0])==OS64_UI_OK);
        os64_ui_set_hidden(&g.ui,&g.bar.w,hidden!=0);gallery_layout(&g,false);
        if(route==2) g.bar.w.bounds.w=1;
        g.targets[0].enabled=enabled!=0;
        unsigned hits=g.targets[0].hits;
        if(!route) key(1,'r',0x13,OS64_GUI_MOD_ALT);
        else {
            queue(1,mouse(OS64_GUI_EVENT_MOUSE_BUTTON_DOWN,g.bar.w.bounds.x,g.bar.w.bounds.y+2,OS64_GUI_MOUSE_LEFT));drain();
            queue(1,mouse(OS64_GUI_EVENT_MOUSE_BUTTON_UP,g.bar.w.bounds.x,g.bar.w.bounds.y+2,OS64_GUI_MOUSE_LEFT));drain();
        }
        CHECK((g.menus->depth!=0)==(enabled!=0));
        if(enabled&&g.menus->depth) {
            CHECK(g.menus->view==g.views[0]);
            if(route==2) key(popup_handle(),0,0x4d,0);
            key(popup_handle(),'\n',0x1c,0);CHECK(g.targets[0].hits==hits+1&&!g.menus->active);
        } else {
            CHECK(g.targets[0].hits==hits&&!g.menus->active&&!g.bar.temporary);
            CHECK(g.bar.w.hidden==(hidden!=0));
        }
        CHECK(os64_ui_menu_close(g.menus,OS64_UI_MENU_EXPLICIT)==OS64_UI_OK);drain();
        os64_ui_set_hidden(&g.ui,&g.bar.w,false);gallery_layout(&g,false);
    }
    current="r6 I5/I9 Down and horizontal navigation refresh eligibility too";
    CHECK(os64_ui_menubar_bind(&g.bar,headings)==OS64_UI_OK);
    for(unsigned right=0;right<2;++right) for(unsigned enabled=0;enabled<2;++enabled) {
        g.targets[0].enabled=!enabled;CHECK(os64_ui_actions_refresh(g.views[0])==OS64_UI_OK);
        g.bar.selected=right||enabled?1:0;key(1,0,0x44,0);
        if(right) key(1,0,0x50,0); /* Open the always-enabled Other root. */
        g.targets[0].enabled=enabled!=0;
        if(right) key(popup_handle(),0,0x4d,0);
        else {
            /* Left reaches the newly enabled Target before Down opens it. */
            if(enabled) { key(1,0,0x4b,0); }
            key(1,0,0x50,0);
        }
        if(right) CHECK(g.menus->depth&&g.menus->panels[0].widget->model==(enabled?leaf:g.models[1]));
        else if(!enabled) CHECK(!g.menus->depth&&g.bar.selected==1);
        else CHECK(g.menus->depth&&g.menus->panels[0].widget->model==leaf);
        CHECK(os64_ui_menu_close(g.menus,OS64_UI_MENU_EXPLICIT)==OS64_UI_OK);drain();
    }
    current="r6 I7 fresh candidate never overwrites an existing saved target";
    g.targets[0].enabled=true;CHECK(os64_ui_actions_refresh(g.views[0])==OS64_UI_OK);
    for(unsigned refuse=0;refuse<2;++refuse) {
        open_context(leaf,g.views[0]);int64_t root=popup_handle();
        g.targets[1].enabled=true;CHECK(os64_ui_actions_refresh(g.views[1])==OS64_UI_OK);
        CHECK(os64_ui_menu_bar_view(g.menus,g.views[1])==OS64_UI_OK);
        g.targets[1].enabled=refuse!=0;transport.fail_destroy=refuse?root:0;
        unsigned errors=error_count,hits=g.targets[0].hits;
        key(root,'r',0x13,OS64_GUI_MOD_ALT);
        CHECK(g.menus->view==g.views[0]&&popup_handle()==root);
        CHECK(error_count==errors+(refuse?1:0));
        if(refuse) CHECK(last_error==OS64_UI_ERROR);
        transport.fail_destroy=0;key(root,'\n',0x1c,0);CHECK(g.targets[0].hits==hits+1&&!g.menus->active);
        CHECK(os64_ui_menu_bar_view(g.menus,g.views[0])==OS64_UI_OK);drain();
    }
    current="r6 I1/I2 declined same-view request reconciles invalid descendants";
    for(unsigned refuse=0;refuse<2;++refuse) {
        g.targets[0].enabled=true;CHECK(os64_ui_actions_refresh(g.views[0])==OS64_UI_OK);
        open_context(headings,g.views[0]);int64_t root=popup_handle();key(root,0,0x4d,0);
        int64_t child=popup_handle();CHECK(child!=root);
        g.targets[0].enabled=false;transport.fail_destroy=refuse?child:0;
        unsigned errors=error_count;key(child,'r',0x13,OS64_GUI_MOD_ALT);
        CHECK(g.menus->depth==(refuse?2u:1u));
        CHECK(g.menus->panels[0].widget->model==headings);
        CHECK(g.menus->panels[0].widget->selected==(refuse?0:MENU_NONE));
        CHECK(error_count==errors+refuse);
        if(refuse) CHECK(last_error==OS64_UI_ERROR);
        transport.fail_destroy=0;CHECK(os64_ui_menu_refresh(g.menus)==OS64_UI_OK);
        CHECK(os64_ui_menu_close(g.menus,OS64_UI_MENU_EXPLICIT)==OS64_UI_OK);drain();
    }
    g.targets[0].enabled=true;CHECK(os64_ui_actions_refresh(g.views[0])==OS64_UI_OK);
    current="r6 I2/I7 stale candidate and refused teardown preserve the saved target";
    open_context(leaf,g.views[0]);int64_t root=popup_handle();
    CHECK(os64_ui_menu_bar_view(g.menus,g.views[1])==OS64_UI_OK);
    ++g.targets[1].generation;transport.fail_destroy=root;
    unsigned errors=error_count;key(root,'r',0x13,OS64_GUI_MOD_ALT);
    CHECK(error_count==errors+1&&last_error==OS64_UI_ERROR);
    CHECK(g.menus->view==g.views[0]&&popup_handle()==root);
    --g.targets[1].generation;transport.fail_destroy=0;
    CHECK(os64_ui_menu_close(g.menus,OS64_UI_MENU_EXPLICIT)==OS64_UI_OK);drain();
    CHECK(os64_ui_menu_bar_view(g.menus,g.views[0])==OS64_UI_OK);
    g.targets[1].enabled=false;CHECK(os64_ui_actions_refresh(g.views[1])==OS64_UI_OK);
    CHECK(os64_ui_menubar_bind(&g.bar,g.models[5])==OS64_UI_OK);
    os64_ui_menu_model_release(single);os64_ui_menu_model_release(headings);os64_ui_menu_model_release(leaf);
}
static void review_round_six(void)
{
    os64_ui_menu_hooks(g.menus,gallery_menu_layout,lifecycle_error,&g);
    registry_rebind();audit_enabled=true;request_freshness();audit_enabled=false;
    os64_ui_menu_hooks(g.menus,gallery_menu_layout,gallery_menu_error,&g);
}

/* A dropdown's list (DROPDOWN.md § Step 2; Quinn's acceptance checks):
 * rows copied at the call, the highlight not the pick, type-ahead that
 * only highlights, commits once and after the close, a detach that no
 * pick outlives (a refused close retried at the boundary), a list waiting
 * out the quarantine, and the anchor widget watched. */
static unsigned list_picks;
static size_t list_picked;
static int list_user;
static void list_pick(void *u,size_t row) { CHECK(u==&list_user);++list_picks;list_picked=row; }
static void list_mode(void)
{
    audit_enabled=true;
    static char text[200][16];
    const char *labels[200];bool enabled[200];
    const char *names[]={"Alpha","Beta","Canada","Chile","Delta"};
    for(size_t i=0;i<200;++i) {
        if(i<5) snprintf(text[i],sizeof(text[i]),"%s",names[i]); else snprintf(text[i],sizeof(text[i]),"Row %03zu",i);
        labels[i]=text[i];enabled[i]=i!=4;
    }
    os64_ui_menu_list_t l={labels,enabled,200,1,120,NULL,list_pick,&list_user};
    os64_gui_rect_t anchor={100,100,120,20};
    list_picks=0;
    current="list: opens on its value, rows copied and staged by viewport";
    CHECK(os64_ui_menu_open_list(g.menus,&l,anchor)==OS64_UI_OK);drain();
    int64_t h=popup_handle();CHECK(h);
    os64_ui_popup_t *p=g.menus->panels[0].widget;
    CHECK(p->list&&p->selected==1&&p->stage_count==p->bottom-p->top&&p->stage_count<200);
    snprintf(text[0],sizeof(text[0]),"Zulu");CHECK(!strcmp(g.menus->list->labels[0],"Alpha"));
    snprintf(text[0],sizeof(text[0]),"Alpha");
    current="list: arrows and letters move the highlight and pick nothing";
    key(h,0,0x50,0);CHECK(p->selected==2&&!list_picks&&popup_handle()==h);
    key(h,'b',0x30,0);CHECK(p->selected==1&&!list_picks&&popup_handle()==h);
    key(h,'c',0x2e,0);CHECK(p->selected==2&&!list_picks);
    key(h,'c',0x2e,0);CHECK(p->selected==3&&!list_picks);
    key(h,'d',0x20,0);CHECK(p->selected==3&&!list_picks&&popup_handle()==h);
    current="list: End reaches the last row, the staged window following";
    key(h,0,0x4f,0);CHECK(p->selected==199&&p->bottom==200&&p->stage_top==p->top&&p->stage_count==p->bottom-p->top);
    current="list: Escape closes and picks nothing";
    key(h,27,0x01,0);CHECK(!popup_handle()&&!os64_ui_menu_active(g.menus)&&!list_picks);drain();
    current="list: Enter commits once, after the close";
    CHECK(os64_ui_menu_open_list(g.menus,&l,anchor)==OS64_UI_OK);drain();h=popup_handle();
    key(h,0,0x50,0);key(h,'\n',0x1c,0);CHECK(list_picks==1&&list_picked==2&&!popup_handle());drain();
    current="list: a click commits the row clicked";
    CHECK(os64_ui_menu_open_list(g.menus,&l,anchor)==OS64_UI_OK);drain();h=popup_handle();p=g.menus->panels[0].widget;
    size_t row=p->top+2;os64_gui_rect_t r=os64_ui_popup_row(p,row);
    queue(h,mouse(OS64_GUI_EVENT_MOUSE_BUTTON_DOWN,r.x+2,r.y+2,OS64_GUI_MOUSE_LEFT));
    queue(h,mouse(OS64_GUI_EVENT_MOUSE_BUTTON_UP,r.x+2,r.y+2,OS64_GUI_MOUSE_LEFT));drain();
    CHECK(list_picks==2&&list_picked==row&&!popup_handle());drain();
    current="list: a detach closes it and no pick follows";
    CHECK(os64_ui_menu_open_list(g.menus,&l,anchor)==OS64_UI_OK);drain();h=popup_handle();
    os64_ui_menu_list_detach(g.menus,&list_user);CHECK(!os64_ui_menu_active(g.menus));drain();CHECK(!popup_handle()&&list_picks==2);
    current="list: a detach whose close is refused calls no one, and the boundary retries";
    CHECK(os64_ui_menu_open_list(g.menus,&l,anchor)==OS64_UI_OK);drain();h=popup_handle();
    transport.fail_destroy=h;os64_ui_menu_list_detach(g.menus,&list_user);
    CHECK(g.menus->close_retry&&popup_handle()==h&&!g.menus->list->pick);
    queue(h,edge(OS64_GUI_EVENT_KEY_DOWN,'\n',0x1c,0));queue(h,edge(OS64_GUI_EVENT_KEY_UP,'\n',0x1c,0));drain();
    CHECK(list_picks==2&&popup_handle()==h);
    transport.fail_destroy=0;drain();CHECK(!popup_handle()&&!os64_ui_menu_active(g.menus)&&list_picks==2);
    current="list: replacing a menu waits out the quarantine, then opens";
    open_context(g.models[4],g.views[0]);
    CHECK(os64_ui_menu_open_list(g.menus,&l,anchor)==OS64_UI_BUSY);
    CHECK(g.menus->list_pending&&!popup_handle());drain();
    h=popup_handle();CHECK(h&&g.menus->depth==1&&g.menus->panels[0].widget->list);
    key(h,27,0x01,0);drain();
    current="list: a list detached while it waits never opens";
    open_context(g.models[4],g.views[0]);
    CHECK(os64_ui_menu_open_list(g.menus,&l,anchor)==OS64_UI_BUSY);
    os64_ui_menu_list_detach(g.menus,&list_user);CHECK(!os64_ui_menu_active(g.menus));drain();
    CHECK(!popup_handle()&&list_picks==2);
    current="list: its box hidden or moved closes it";
    os64_ui_widget_t box={.bounds={100,100,120,20}};l.widget=&box;
    CHECK(os64_ui_menu_open_list(g.menus,&l,anchor)==OS64_UI_OK);drain();CHECK(popup_handle());
    box.hidden=true;queue(1,mouse(OS64_GUI_EVENT_MOUSE_MOVE,5,5,0));drain();CHECK(!popup_handle()&&!os64_ui_menu_active(g.menus));
    box.hidden=false;CHECK(os64_ui_menu_open_list(g.menus,&l,anchor)==OS64_UI_OK);drain();CHECK(popup_handle());
    box.bounds.y+=1;queue(1,mouse(OS64_GUI_EVENT_MOUSE_MOVE,5,5,0));drain();CHECK(!popup_handle()&&!os64_ui_menu_active(g.menus));
    CHECK(list_picks==2);
    audit_enabled=false;
}

/* The dropdown widget over the session: a click, Space and Alt+Down open
 * its list; a pick of the value it has changes nothing (Quinn's review, 4);
 * Up and Down on the closed box change the value; destroying it while the
 * list is open closes the list and leaves no pick to come. */
static unsigned drop_changes;
static void drop_changed(os64_ui_dropdown_t *d,void *u) { (void)d;(void)u;++drop_changes; }
static const char *drop_label(size_t i,void *u)
{
    (void)u;
    static const char *names[]={"Name (A to Z)","Name (Z to A)","Price (low to high)","Price (high to low)"};
    return i<4?names[i]:"";
}
static void dropdown_widget(void)
{
    audit_enabled=true;
    static os64_ui_dropdown_t d;
    os64_ui_dropdown(&d,4,drop_label,drop_changed,NULL);
    d.w.bounds=(os64_gui_rect_t){600,600,160,24};
    os64_ui_add_child(g.ui.root,&d.w);
    os64_ui_dropdown_set(&g.ui,&d,4,0);drop_changes=0;
    current="dropdown: a click opens its list on the value";
    queue(1,mouse(OS64_GUI_EVENT_MOUSE_BUTTON_DOWN,610,610,OS64_GUI_MOUSE_LEFT));
    queue(1,mouse(OS64_GUI_EVENT_MOUSE_BUTTON_UP,610,610,OS64_GUI_MOUSE_LEFT));drain();
    int64_t h=popup_handle();
    CHECK(h&&g.menus->list&&g.menus->list->user==&d&&g.menus->panels[0].widget->selected==0);
    current="dropdown: committing the value it has is not a change";
    key(h,'\n',0x1c,0);CHECK(!popup_handle()&&d.selected==0&&!drop_changes);
    current="dropdown: committing another is one change";
    queue(1,mouse(OS64_GUI_EVENT_MOUSE_BUTTON_DOWN,610,610,OS64_GUI_MOUSE_LEFT));drain();h=popup_handle();CHECK(h);
    key(h,0,0x50,0);key(h,0,0x50,0);CHECK(!drop_changes);
    key(h,'\n',0x1c,0);CHECK(!popup_handle()&&d.selected==2&&drop_changes==1);
    current="dropdown: Up and Down on the closed box change the value";
    CHECK(g.ui.focus==&d.w);
    key(1,0,0x50,0);CHECK(d.selected==3&&drop_changes==2&&!popup_handle());
    key(1,0,0x50,0);CHECK(d.selected==3&&drop_changes==2);
    key(1,0,0x48,0);CHECK(d.selected==2&&drop_changes==3);
    current="dropdown: Space and Alt+Down open it";
    key(1,' ',0x39,0);h=popup_handle();CHECK(h);key(h,27,0x01,0);CHECK(!popup_handle()&&drop_changes==3);
    key(1,0,0x50,OS64_GUI_MOD_ALT);h=popup_handle();CHECK(h);key(h,27,0x01,0);drain();
    current="dropdown: destroyed while open, its list closes and no pick follows";
    queue(1,mouse(OS64_GUI_EVENT_MOUSE_BUTTON_DOWN,610,610,OS64_GUI_MOUSE_LEFT));drain();h=popup_handle();CHECK(h);
    d.w.cls->destroy(&d.w);CHECK(!os64_ui_menu_active(g.menus));drain();CHECK(!popup_handle()&&drop_changes==3);
    d.w.hidden=true;os64_ui_dropdown_set(&g.ui,&d,0,-1);
    audit_enabled=false;
}

int main(int argc,char **argv)
{
    setvbuf(stdout,NULL,_IONBF,0);
    const char *dir=argc>1?argv[1]:"userland/libfreetype/fixtures";
    (void)action_suite(argc,argv);
    if(getenv("E1B_R3_COLLAPSE_ONLY")) {
        build(dir,true);collapse_pending_child();dispose();
        printf("collapse: %lu checks, %d failures\n",checks,failures);return failures?1:0;
    }
    if(getenv("E1B_R4_ONLY")) {
        build(dir,true);review_round_four();dispose();
        printf("r4: %lu checks, %d failures\n",checks,failures);return failures?1:0;
    }
    if(getenv("E1B_R5_ONLY")) {
        build(dir,true);review_round_five();dispose();
        printf("r5: %lu checks, %d failures\n",checks,failures);return failures?1:0;
    }
    if(getenv("E1B_R6_ONLY")) {
        build(dir,true);review_round_six();dispose();
        printf("r6: %lu checks, %d failures\n",checks,failures);return failures?1:0;
    }
    build(dir,true);models();disabled_bar_and_overflow_failure();dismissal_orders();activation_and_loss();quarantine();pending_context_keys();review_navigation();destruction_failure(dir);review_round_two();review_round_three();review_round_four();review_round_five();review_round_six();navigation();burst_and_pointer_navigation();standalone();callback_and_icon_contract();placement_and_fonts(dir);collapse_pending_child();list_mode();dropdown_widget();dispose();
    printf("menu/action/status: %lu checks, %d failures\n",checks,failures);return failures?1:0;
}
