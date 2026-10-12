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
        int64_t n=os64_ui_dispatcher_turn(&g.dispatcher,false);CHECK(n>=0);
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
    CHECK(gallery_attach_menus(&g,1,&backend,true)); transport.dispatcher=&g.dispatcher;
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

int main(int argc,char **argv)
{
    setvbuf(stdout,NULL,_IONBF,0);
    const char *dir=argc>1?argv[1]:"userland/libfreetype/fixtures";
    (void)action_suite(argc,argv);
    if(getenv("E1B_R3_COLLAPSE_ONLY")) {
        build(dir,true);collapse_pending_child();dispose();
        printf("collapse: %lu checks, %d failures\n",checks,failures);return failures?1:0;
    }
    build(dir,true);models();disabled_bar_and_overflow_failure();dismissal_orders();activation_and_loss();quarantine();pending_context_keys();review_navigation();destruction_failure(dir);review_round_two();review_round_three();navigation();burst_and_pointer_navigation();standalone();callback_and_icon_contract();placement_and_fonts(dir);collapse_pending_child();dispose();
    printf("menu/action/status: %lu checks, %d failures\n",checks,failures);return failures?1:0;
}
