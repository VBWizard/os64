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
    unsigned creates,destroys,owner_down,owner_up,peer_down,invocations;
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
    build(dir,true);models();disabled_bar_and_overflow_failure();dismissal_orders();activation_and_loss();quarantine();pending_context_keys();navigation();burst_and_pointer_navigation();standalone();callback_and_icon_contract();placement_and_fonts(dir);dispose();
    printf("menu/action/status: %lu checks, %d failures\n",checks,failures);return failures?1:0;
}
