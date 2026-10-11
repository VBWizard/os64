/* Production action/status widgets and gallery driven with the existing font
 * fixtures and allocation injector. Window/file I/O is stubbed, not libui. */
#define main ui_action_unused_text_suite
#include "test_ui_text_host.c"
#undef main
#define UI_ACTION_HOST
#include "../userland/apps/actiongallery/actiongallery.c"

void os64_ui_theme_current(os64_ui_theme_t *t, uint64_t *installed)
{ *t=(os64_ui_theme_t){.pad=6,.gap=6,.button_h=28,.font_w=8,.font_h=16,
    .panel_bg=0xff202020,.panel_border=0xff888888,.label_fg=0xffeeeeee}; *installed=0; }
bool os64_ui_theme_session(os64_ui_theme_t *t,uint64_t *generation,uint64_t hint)
{ (void)t; (void)generation; (void)hint; return false; }
int64_t os64_open(const char *p,const char *m) { (void)p;(void)m;return -1; }
int64_t os64_read(int32_t fd,void *p,size_t n) { (void)fd;(void)p;(void)n;return -1; }
int64_t os64_write(int32_t fd,const void *p,size_t n) { (void)fd;(void)p;return (int64_t)n; }
int64_t os64_close(int32_t fd) { (void)fd;return 0; }

static os64_gui_event_t edge(uint32_t type,char ascii,uint8_t sc,uint8_t mods)
{
    os64_gui_event_t e={.type=type}; e.key.ascii=ascii;e.key.scancode=sc;e.key.modifiers=mods;
    return e;
}
typedef struct { unsigned refs,calls; uint64_t gen; bool enabled,checked,invalid_radio,close,refuse; }
    action_test_target;
static bool at_retain(void *cookie,uint64_t gen)
{ (void)gen;action_test_target *t=cookie;if(t->refuse)return false;++t->refs;return true; }
static void at_release(void *cookie,uint64_t gen)
{ (void)gen;--((action_test_target *)cookie)->refs; }
static bool at_valid(void *cookie,uint64_t gen)
{ return ((action_test_target *)cookie)->gen==gen; }
static void at_query(void *app,const os64_ui_action_context_t *c,uint32_t id,
                     os64_ui_action_state_t *s)
{
    (void)app;action_test_target *t=c->cookie;s->enabled=t->enabled;
    s->checked=id==2?t->checked:id>=3?t->invalid_radio:false;
}
static os64_ui_action_view_t *at_live_view;
static os64_ui_actions_t *at_live_actions;
static os64_ui_action_result_t at_invoke(void *app,const os64_ui_action_context_t *c,uint32_t id)
{
    (void)app;action_test_target *t=c->cookie;++t->calls;
    CHECK(os64_ui_action_view_destroy(at_live_view)==OS64_UI_BUSY);
    CHECK(os64_ui_actions_destroy(at_live_actions)==OS64_UI_BUSY);
    if(id==2)t->checked=!t->checked;
    return (os64_ui_action_result_t){t->close?OS64_UI_ERROR:OS64_UI_OK,t->close?73:0};
}
static const os64_ui_action_callbacks_t at_cb={at_query,at_invoke};
static os64_ui_action_spec_t at_specs[]={
    {1,OS64_UI_ACTION_COMMAND,0,"Command","Help",NULL,'C',{'C',OS64_GUI_MOD_CTRL}},
    {2,OS64_UI_ACTION_TOGGLE,0,"Toggle","Toggle help",NULL,'T',{'t',OS64_GUI_MOD_CTRL}},
    {3,OS64_UI_ACTION_RADIO,8,"First","",NULL,0,{0,0}},
    {4,OS64_UI_ACTION_RADIO,8,"Second","",NULL,0,{0,0}}
};
static void registration(void)
{
    current="registration";os64_ui_actions_t *a=(void *)1;
    deny_countdown=0;CHECK(os64_ui_actions_create(at_specs,4,&at_cb,NULL,&a)==OS64_UI_NO_MEMORY);CHECK(!a);
    os64_ui_action_spec_t bad[4];memcpy(bad,at_specs,sizeof(bad));
    bad[1].id=1;CHECK(os64_ui_actions_create(bad,4,&at_cb,NULL,&a)==OS64_UI_DUPLICATE);CHECK(!a);
    bad[1]=at_specs[1];bad[1].shortcut=(os64_ui_shortcut_t){'c',OS64_GUI_MOD_CTRL};
    CHECK(os64_ui_actions_create(bad,4,&at_cb,NULL,&a)==OS64_UI_DUPLICATE);
    bad[1]=at_specs[1];bad[1].shortcut=(os64_ui_shortcut_t){'x',OS64_GUI_MOD_CTRL|OS64_GUI_MOD_ALT};
    CHECK(os64_ui_actions_create(bad,4,&at_cb,NULL,&a)==OS64_UI_INVALID);
    const uint16_t reserved[]={OS64_UI_KEY_TAB,OS64_UI_KEY_F4,OS64_UI_KEY_LEFT,OS64_UI_KEY_RIGHT};
    for(size_t i=0;i<4;++i) {
        bad[1].shortcut=(os64_ui_shortcut_t){reserved[i],OS64_GUI_MOD_ALT};
        CHECK(os64_ui_actions_create(bad,4,&at_cb,NULL,&a)==OS64_UI_INVALID);
    }
    bad[1]=at_specs[1];bad[1].label="bad\xff";
    CHECK(os64_ui_actions_create(bad,4,&at_cb,NULL,&a)==OS64_UI_INVALID);
    char too_long[129];memset(too_long,'x',128);too_long[128]=0;bad[1].label=too_long;
    CHECK(os64_ui_actions_create(bad,4,&at_cb,NULL,&a)==OS64_UI_LIMIT);
    CHECK(os64_ui_actions_create(at_specs,257,&at_cb,NULL,&a)==OS64_UI_LIMIT);
    char copied[]="Original";bad[1]=at_specs[1];bad[1].label=copied;
    CHECK(os64_ui_actions_create(bad,4,&at_cb,NULL,&a)==OS64_UI_OK);copied[0]='X';
    os64_ui_action_spec_t spec;CHECK(os64_ui_action_spec(a,2,&spec)==OS64_UI_OK);
    CHECK(!strcmp(spec.label,"Original"));CHECK(spec.shortcut.key=='t');
    CHECK(os64_ui_actions_retain(a)==OS64_UI_OK);CHECK(os64_ui_actions_destroy(a)==OS64_UI_BUSY);
    os64_ui_actions_release(a);CHECK(os64_ui_actions_destroy(a)==OS64_UI_OK);
}
static void views_and_bindings(void)
{
    current="views and bindings";os64_ui_actions_t *a=NULL;
    CHECK(os64_ui_actions_create(at_specs,4,&at_cb,NULL,&a)==OS64_UI_OK);at_live_actions=a;
    action_test_target t={.gen=1,.enabled=true}, other={.gen=1};
    os64_ui_action_context_t c={&t,1,at_retain,at_release,at_valid};
    os64_ui_action_view_t *v=NULL,*v2=NULL;
    t.refuse=true;CHECK(os64_ui_action_view_create(a,&c,&v)==OS64_UI_REFUSED&&!v&&!t.refs);t.refuse=false;
    deny_countdown=0;CHECK(os64_ui_action_view_create(a,&c,&v)==OS64_UI_NO_MEMORY);CHECK(!v&&t.refs==0);
    c.generation=2;CHECK(os64_ui_action_view_create(a,&c,&v)==OS64_UI_STALE);CHECK(!v&&t.refs==0);c.generation=1;
    CHECK(os64_ui_action_view_create(a,&c,&v)==OS64_UI_OK);CHECK(t.refs==1);at_live_view=v;
    c.cookie=&other;CHECK(os64_ui_action_view_create(a,&c,&v2)==OS64_UI_OK);
    os64_ui_action_state_t state;
    CHECK(os64_ui_action_view_retain(v2)==OS64_UI_OK);
    CHECK(os64_ui_action_view_destroy(v2)==OS64_UI_BUSY);os64_ui_action_view_release(v2);
    CHECK(os64_ui_action_snapshot(v,1,&state)==OS64_UI_OK&&state.enabled);
    CHECK(os64_ui_action_snapshot(v2,1,&state)==OS64_UI_OK&&!state.enabled);
    CHECK(os64_ui_action_invoke(v2,1).status==OS64_UI_REFUSED&&other.calls==0);
    os64_ui_action_view_t *many[30];
    for(size_t i=0;i<30;++i)CHECK(os64_ui_action_view_create(a,&c,&many[i])==OS64_UI_OK);
    os64_ui_action_view_t *excess=NULL;CHECK(os64_ui_action_view_create(a,&c,&excess)==OS64_UI_LIMIT&&!excess);
    for(size_t i=0;i<30;++i)CHECK(os64_ui_action_view_destroy(many[i])==OS64_UI_OK);
    os64_ui_t ui={0};ui.theme=(os64_ui_theme_t){.pad=4,.gap=4,.button_h=28};
    os64_ui_widget_t root,b;os64_ui_panel(&root);root.bounds=(os64_gui_rect_t){0,0,200,100};
    os64_ui_button(&b,"Legacy",NULL,NULL);b.bounds=(os64_gui_rect_t){0,0,100,28};
    os64_ui_set_root(&ui,&root);os64_ui_add_child(&root,&b);
    deny_countdown=0;CHECK(os64_ui_action_bind(&b,v,1)==OS64_UI_NO_MEMORY);CHECK(!strcmp(b.text,"Legacy"));
    bool bound=false;
    for(long fail=1;fail<100&&!bound;++fail) {
        deny_countdown=fail;
        os64_ui_status_code_t rc=os64_ui_action_bind(&b,v,1);deny_countdown=-1;
        if(rc==OS64_UI_OK)bound=true;
        else CHECK(rc==OS64_UI_NO_MEMORY&&!strcmp(b.text,"Legacy"));
    }
    CHECK(bound);CHECK(os64_ui_action_view_destroy(v)==OS64_UI_BUSY);
    CHECK(os64_ui_action_bind(&b,v,1)==OS64_UI_DUPLICATE);
    os64_gui_event_t e={.type=OS64_GUI_EVENT_MOUSE_BUTTON_DOWN};e.mouse.button=OS64_GUI_MOUSE_LEFT;e.mouse.x=4;e.mouse.y=4;
    CHECK(os64_ui_dispatch(&ui,&e));CHECK(b.pressed);
    t.enabled=false;e.type=OS64_GUI_EVENT_MOUSE_BUTTON_UP;
    CHECK(os64_ui_dispatch(&ui,&e));CHECK(!t.calls&&!b.pressed&&b.disabled);
    t.enabled=true;CHECK(os64_ui_actions_refresh(v)==OS64_UI_OK);
    e.type=OS64_GUI_EVENT_MOUSE_BUTTON_DOWN;CHECK(os64_ui_dispatch(&ui,&e));
    e.type=OS64_GUI_EVENT_MOUSE_BUTTON_UP;CHECK(os64_ui_dispatch(&ui,&e));CHECK(t.calls==1);
    t.close=true;CHECK(os64_ui_action_invoke(v,1).app_result==73);
    e.type=OS64_GUI_EVENT_MOUSE_BUTTON_DOWN;CHECK(os64_ui_dispatch(&ui,&e)&&b.pressed);
    CHECK(os64_ui_action_unbind(&b)==OS64_UI_OK&&!b.pressed&&!ui.grab);
    unsigned calls=t.calls;e.type=OS64_GUI_EVENT_MOUSE_BUTTON_UP;
    CHECK(!os64_ui_dispatch(&ui,&e)&&t.calls==calls);
    CHECK(os64_ui_action_bind(&b,v,1)==OS64_UI_OK);
    t.invalid_radio=true;CHECK(os64_ui_actions_refresh(v)==OS64_UI_INVALID);
    CHECK(os64_ui_action_snapshot(v,1,&state)==OS64_UI_INVALID&&!state.enabled&&b.disabled);
    CHECK(os64_ui_action_invoke(v,1).status==OS64_UI_INVALID);
    t.invalid_radio=false;CHECK(os64_ui_actions_refresh(v)==OS64_UI_OK);
    CHECK(os64_ui_action_invoke(v,2).status==OS64_UI_ERROR);
    CHECK(os64_ui_action_snapshot(v,2,&state)==OS64_UI_OK&&state.checked);
    ++t.gen;CHECK(os64_ui_actions_refresh(v)==OS64_UI_STALE&&b.disabled);
    CHECK(os64_ui_action_invoke(v,1).status==OS64_UI_STALE);
    CHECK(os64_ui_action_unbind(&b)==OS64_UI_OK);CHECK(!b.text&&!b.on_click&&!b.pressed);
    CHECK(os64_ui_action_view_destroy(v)==OS64_UI_OK&&t.refs==0);
    CHECK(os64_ui_action_view_destroy(v2)==OS64_UI_OK&&other.refs==0);
    CHECK(os64_ui_actions_destroy(a)==OS64_UI_OK);
    CHECK(os64_ui_font_release(&ui)==OS64_FONT_OK);
}
static void shortcuts(void)
{
    current="shortcuts";os64_ui_shortcut_t ctrl_c={'c',OS64_GUI_MOD_CTRL};
    os64_gui_event_t e=edge(OS64_GUI_EVENT_KEY_DOWN,3,0x2e,OS64_GUI_MOD_CTRL|OS64_GUI_MOD_CAPS);
    CHECK(os64_ui_shortcut_match(ctrl_c,&e));e.key.modifiers|=OS64_GUI_MOD_SHIFT;
    CHECK(!os64_ui_shortcut_match(ctrl_c,&e));
    const uint8_t f10_codes[]={0x44,0x43};
    for(size_t i=0;i<2;++i) {
        e=edge(OS64_GUI_EVENT_KEY_DOWN,0,f10_codes[i],i?OS64_GUI_MOD_HID:0);
        CHECK(os64_ui_shortcut_match((os64_ui_shortcut_t){OS64_UI_KEY_F10,0},&e));
    }
    const uint8_t up_codes[]={0x48,0x52};
    const char burst[]={27,'[','A'};
    for(size_t i=0;i<2;++i)for(size_t j=0;j<sizeof(burst);++j) {
        e=edge(OS64_GUI_EVENT_KEY_DOWN,burst[j],up_codes[i],i?OS64_GUI_MOD_HID:0);
        CHECK(!os64_ui_shortcut_match((os64_ui_shortcut_t){'a',0},&e));
        CHECK(!os64_ui_shortcut_match((os64_ui_shortcut_t){'[',0},&e));
    }
    e=edge(OS64_GUI_EVENT_KEY_DOWN,'8',0x48,OS64_GUI_MOD_NUM);
    CHECK(os64_ui_shortcut_match((os64_ui_shortcut_t){'8',0},&e));
    e=edge(OS64_GUI_EVENT_KEY_DOWN,'3',0x53,0);
    CHECK(!os64_ui_shortcut_match((os64_ui_shortcut_t){'3',0},&e));
    e=edge(OS64_GUI_EVENT_KEY_DOWN,'.',0x53,OS64_GUI_MOD_NUM);
    CHECK(os64_ui_shortcut_match((os64_ui_shortcut_t){'.',0},&e));
    e=edge(OS64_GUI_EVENT_KEY_DOWN,'\t',0x0f,OS64_GUI_MOD_CTRL);
    CHECK(os64_ui_shortcut_match((os64_ui_shortcut_t){OS64_UI_KEY_TAB,OS64_GUI_MOD_CTRL},&e));
    os64_ui_actions_t *a;CHECK(os64_ui_actions_create(at_specs,4,&at_cb,NULL,&a)==OS64_UI_OK);at_live_actions=a;
    action_test_target t={.gen=1,.enabled=true};os64_ui_action_context_t c={&t,1,at_retain,at_release,at_valid};
    os64_ui_action_view_t *v;CHECK(os64_ui_action_view_create(a,&c,&v)==OS64_UI_OK);at_live_view=v;
    os64_ui_shortcut_router_t router;os64_ui_shortcut_reset(&router);os64_ui_action_result_t result;
    e=edge(OS64_GUI_EVENT_KEY_DOWN,20,0x14,OS64_GUI_MOD_CTRL);
    CHECK(os64_ui_shortcut_route(&router,v,&e,&result)&&result.status==OS64_UI_OK);
    for(size_t i=0;i<10;++i) { CHECK(os64_ui_shortcut_route(&router,v,&e,NULL)); }
    CHECK(t.calls==1&&t.checked);
    e.type=OS64_GUI_EVENT_KEY_UP;e.key.modifiers=0;CHECK(os64_ui_shortcut_route(&router,v,&e,NULL));
    t.enabled=false;e=edge(OS64_GUI_EVENT_KEY_DOWN,20,0x14,OS64_GUI_MOD_CTRL);
    CHECK(os64_ui_shortcut_route(&router,v,&e,&result)&&result.status==OS64_UI_REFUSED&&t.calls==1);
    e=(os64_gui_event_t){.type=OS64_GUI_EVENT_WINDOW_FOCUS};CHECK(!os64_ui_shortcut_route(&router,v,&e,NULL));
    t.enabled=true;e=edge(OS64_GUI_EVENT_KEY_DOWN,20,0x17,OS64_GUI_MOD_CTRL|OS64_GUI_MOD_HID);
    CHECK(os64_ui_shortcut_route(&router,v,&e,NULL)&&t.calls==2&&!t.checked);
    e=edge(OS64_GUI_EVENT_KEY_DOWN,22,0x2f,OS64_GUI_MOD_CTRL);
    CHECK(!os64_ui_shortcut_route(&router,v,&e,NULL));
    CHECK(os64_ui_action_view_destroy(v)==OS64_UI_OK);CHECK(os64_ui_actions_destroy(a)==OS64_UI_OK);
}
static void ctrl_punctuation(void)
{
    current="Ctrl punctuation";
    const char keys[]={'[','\\',']','^','_'};
    const uint8_t codes[2][5]={{0x1a,0x2b,0x1b,0x07,0x0c},
                              {0x2f,0x31,0x30,0x23,0x2d}};
    os64_ui_action_spec_t specs[5];
    for(size_t i=0;i<5;++i) {
        uint8_t mods=OS64_GUI_MOD_CTRL | (i>=3?OS64_GUI_MOD_SHIFT:0);
        specs[i]=(os64_ui_action_spec_t){.id=10+(uint32_t)i,
            .kind=OS64_UI_ACTION_COMMAND,.label="Punctuation",
            .shortcut={keys[i],mods}};
    }
    os64_ui_actions_t *a=NULL;
    CHECK(os64_ui_actions_create(specs,5,&at_cb,NULL,&a)==OS64_UI_OK);
    at_live_actions=a;
    action_test_target t={.gen=1,.enabled=true};
    os64_ui_action_context_t c={&t,1,at_retain,at_release,at_valid};
    os64_ui_action_view_t *v=NULL;
    CHECK(os64_ui_action_view_create(a,&c,&v)==OS64_UI_OK);at_live_view=v;
    os64_ui_shortcut_router_t router;os64_ui_shortcut_reset(&router);
    for(size_t dialect=0;dialect<2;++dialect) {
        uint8_t tag=dialect?OS64_GUI_MOD_HID:0;
        for(size_t i=0;i<5;++i) {
            os64_gui_event_t e=edge(OS64_GUI_EVENT_KEY_DOWN,(char)(27+i),
                codes[dialect][i],specs[i].shortcut.modifiers|tag|OS64_GUI_MOD_CAPS|OS64_GUI_MOD_NUM);
            CHECK(os64_ui_shortcut_match(specs[i].shortcut,&e));
            os64_gui_event_t printable=e;printable.key.ascii=keys[i];
            CHECK(os64_ui_shortcut_match(specs[i].shortcut,&printable));
            unsigned before=t.calls;os64_ui_action_result_t result;
            CHECK(os64_ui_shortcut_route(&router,v,&e,&result)&&result.status==OS64_UI_OK);
            CHECK(t.calls==before+1);
            CHECK(os64_ui_shortcut_route(&router,v,&e,NULL)&&t.calls==before+1);
            e.type=OS64_GUI_EVENT_KEY_UP;e.key.modifiers=tag;
            CHECK(os64_ui_shortcut_route(&router,v,&e,NULL));
        }
        os64_gui_event_t esc=edge(OS64_GUI_EVENT_KEY_DOWN,27,dialect?0x29:0x01,
                                  OS64_GUI_MOD_CTRL|tag);
        CHECK(os64_ui_shortcut_match((os64_ui_shortcut_t){OS64_UI_KEY_ESCAPE,OS64_GUI_MOD_CTRL},&esc));
        CHECK(!os64_ui_shortcut_match(specs[0].shortcut,&esc));
        const char burst[]={27,'[','A'};
        for(size_t i=0;i<sizeof(burst);++i) {
            os64_gui_event_t e=edge(OS64_GUI_EVENT_KEY_DOWN,burst[i],dialect?0x52:0x48,
                                   OS64_GUI_MOD_CTRL|tag);
            CHECK(!os64_ui_shortcut_match(specs[0].shortcut,&e));
            CHECK(!os64_ui_shortcut_route(&router,v,&e,NULL));
        }
    }
    CHECK(os64_ui_action_view_destroy(v)==OS64_UI_OK&&!t.refs);
    CHECK(os64_ui_actions_destroy(a)==OS64_UI_OK);
}

static void status_fit_tests(const char *dir)
{
    (void)dir;current="status complete secondary fit";
    os64_ui_t ui={0};ui.theme=(os64_ui_theme_t){.pad=4,.gap=4,.button_h=28,
        .panel_bg=0xff202020,.label_fg=0xffeeeeee};
    os64_ui_status_t s;os64_ui_status_init(&s);
    s.w.bounds=(os64_gui_rect_t){0,0,300,48};os64_ui_set_root(&ui,&s.w);
#ifdef UI_TEXT_REAL
    os64_font_set_t *set=outline_set(os64_ui_font_context(&ui),dir,"DejaVuSans.ttf",16);
    CHECK(set!=NULL);
    if(set) {
        CHECK(os64_ui_font_bind(&ui,set)==OS64_FONT_OK);
        os64_font_set_release(set);
    }
#endif
    const char *notice="Notice",*secondary="WWWWW";
    int32_t notice_width=0,secondary_width=0;
    CHECK(os64_ui_text_measure(&ui,OS64_FONT_ROLE_UI,notice,strlen(notice),&notice_width)==OS64_FONT_OK);
    CHECK(os64_ui_text_measure(&ui,OS64_FONT_ROLE_UI,secondary,strlen(secondary),&secondary_width)==OS64_FONT_OK);
    CHECK(notice_width>0&&secondary_width>2);
    uint64_t notice_token=0;
    CHECK(os64_ui_status_notice(&s,notice,&notice_token)==OS64_UI_OK);
    const int32_t remaining[]={1,secondary_width/2,secondary_width-1,
                               secondary_width,secondary_width+1};
    for(unsigned source=0;source<2;++source) {
        for(size_t i=0;i<sizeof(remaining)/sizeof(*remaining);++i) {
            s.w.bounds.w=2*ui.theme.pad+notice_width+ui.theme.gap+remaining[i];
            CHECK(s.w.bounds.w<=SURF_W);
            canvas_t reference,actual;canvas_init(&reference,0);canvas_init(&actual,0);
            os64_draw_ctx_t dc={.surf=reference.s};
            s.w.cls->paint(&s.w,&dc,&ui.theme);
            uint64_t token=0;
            CHECK((source?os64_ui_status_base(&s,secondary):
                os64_ui_status_help(&s,secondary,&token))==OS64_UI_OK);
            dc.surf=actual.s;s.w.cls->paint(&s.w,&dc,&ui.theme);
            bool equal=memcmp(reference.px,actual.px,sizeof(actual.px))==0;
            CHECK(equal==(remaining[i]<secondary_width));
            CHECK(rows_touched_outside(&actual,0,0,48)==0);
            CHECK((source?os64_ui_status_base(&s,""):
                os64_ui_status_clear_help(&s,token))==OS64_UI_OK);
        }
    }
    CHECK(os64_ui_status_clear_notice(&s,notice_token)==OS64_UI_OK);
    CHECK(os64_ui_font_release(&ui)==OS64_FONT_OK);
}

static void status_tests(const char *dir)
{
    (void)dir;current="status";
    os64_ui_t ui={0};ui.theme=(os64_ui_theme_t){.pad=4,.gap=4,.button_h=28,.panel_bg=0xff202020,.label_fg=0xffeeeeee};
    os64_ui_status_t s;os64_ui_status_init(&s);s.w.bounds=(os64_gui_rect_t){0,0,300,28};os64_ui_set_root(&ui,&s.w);
    char source[]="Base information";CHECK(os64_ui_status_base(&s,source)==OS64_UI_OK);source[0]='X';CHECK(!strcmp(s.base.text,"Base information"));
    uint64_t old,new,notice;
    CHECK(os64_ui_status_help(&s,"Producer one",&old)==OS64_UI_OK);
    CHECK(os64_ui_status_notice(&s,"Persistent error",&notice)==OS64_UI_OK);
    CHECK(os64_ui_status_help(&s,"Producer two",&new)==OS64_UI_OK&&new>notice&&notice>old);
    CHECK(os64_ui_status_clear_help(&s,old)==OS64_UI_STALE&&!strcmp(s.help.text,"Producer two"));
    CHECK(os64_ui_status_clear_notice(&s,old)==OS64_UI_STALE);
    void *standing=s.help.run;
    deny_countdown=0;uint64_t refused=99;CHECK(os64_ui_status_help(&s,"New",&refused)==OS64_UI_NO_MEMORY);
    CHECK(!refused&&s.help.run==standing&&s.help.token==new);
    bool succeeded=false;
    for(long i=1;i<100&&!succeeded;++i) {
        deny_countdown=i;os64_ui_status_code_t rc=os64_ui_status_help(&s,"Replacement help",&refused);deny_countdown=-1;
        if(rc==OS64_UI_OK)succeeded=true;
        else CHECK(rc==OS64_UI_NO_MEMORY&&s.help.run==standing&&s.help.token==new);
    }
    CHECK(succeeded);
    char too_long[513];memset(too_long,'x',512);too_long[512]=0;
    CHECK(os64_ui_status_base(&s,too_long)==OS64_UI_LIMIT&&!strcmp(s.base.text,"Base information"));
    CHECK(os64_ui_status_base(&s,"bad\xff")==OS64_UI_INVALID);
    /* Compare pixels at a width entirely occupied by the notice: replacing
     * help cannot change the persistent message or paint outside the widget. */
    canvas_t a,b;canvas_init(&a,0);canvas_init(&b,0);os64_draw_ctx_t dc={.surf=a.s};
    s.w.bounds.w=80;s.w.cls->paint(&s.w,&dc,&ui.theme);
    CHECK(os64_ui_status_help(&s,"Different help",&new)==OS64_UI_OK);dc.surf=b.s;s.w.cls->paint(&s.w,&dc,&ui.theme);
    CHECK(!memcmp(a.px,b.px,sizeof(a.px)));CHECK(rows_touched_outside(&b,0,0,28)==0);
    for(size_t i=0;i<30;++i) { os64_ui_set_hidden(&ui,&s.w,true);os64_ui_set_hidden(&ui,&s.w,false); }
    CHECK(!strcmp(s.notice.text,"Persistent error"));
    s.generation=UINT64_MAX;CHECK(os64_ui_status_help(&s,"Overflow",&old)==OS64_UI_LIMIT&&s.help.token==new);
#ifdef UI_TEXT_REAL
    os64_ui_set_hidden(&ui,&s.w,true);
    os64_font_set_t *set=outline_set(os64_ui_font_context(&ui),dir,"DejaVuSans.ttf",32);
    CHECK(set!=NULL);if(set) {
        os64_font_consumer_t consumer;os64_ui_font_consumer(&ui,&consumer);
        void *adopt=NULL;void *prior=s.notice.run;
        ui_test_deny_all=true;CHECK(consumer.prepare(consumer.user,set,&adopt)!=OS64_FONT_OK);ui_test_deny_all=false;
        CHECK(!adopt&&s.notice.run==prior&&!s.notice.staged);
        bool prepared=false;
        for(long fail=0;fail<200&&!prepared;++fail) {
            deny_countdown=fail;
            os64_font_status_t rc=consumer.prepare(consumer.user,set,&adopt);
            deny_countdown=-1;
            if(rc==OS64_FONT_OK) prepared=true;
            else CHECK(!adopt&&s.notice.run==prior&&!s.base.staged&&!s.help.staged&&!s.notice.staged);
        }
        CHECK(prepared);
        ui_test_deny_all=true;consumer.commit(consumer.user,adopt);ui_test_deny_all=false;
        CHECK(s.notice.run!=prior&&s.w.natural_h>=32&&s.help.token==new);
        os64_font_set_release(set);
    }
    os64_ui_set_hidden(&ui,&s.w,false);
#endif
    CHECK(os64_ui_status_clear_help(&s,new)==OS64_UI_OK&&!s.help.text);
    CHECK(os64_ui_status_clear_notice(&s,notice)==OS64_UI_OK&&!s.notice.text);
    CHECK(os64_ui_font_release(&ui)==OS64_FONT_OK);CHECK(!s.base.text&&!s.base.run&&!s.base.staged);
}
static void gallery_tests(const char *dir)
{
    (void)dir;current="gallery";
    uint32_t *pixels=calloc(878u*718u,sizeof(*pixels));CHECK(pixels!=NULL);
    g=(gallery){0};g.draw.surf=(os64_gui_surface_t){.pixels=pixels,.width=878,.height=718,.pitch_px=878};
    CHECK(gallery_build(&g));os64_ui_action_state_t state;
    CHECK(os64_ui_action_snapshot(g.views[0],RECORD,&state)==OS64_UI_OK&&state.enabled);
    CHECK(os64_ui_action_snapshot(g.views[1],RECORD,&state)==OS64_UI_OK&&!state.enabled);
    os64_gui_event_t e=edge(OS64_GUI_EVENT_KEY_DOWN,4,0x20,OS64_GUI_MOD_CTRL);
    CHECK(gallery_route(&g,&e,NULL));CHECK(g.targets[0].hits==1);
    CHECK(gallery_route(&g,&e,NULL));CHECK(g.targets[0].hits==1);
    e.type=OS64_GUI_EVENT_KEY_UP;CHECK(gallery_route(&g,&e,NULL));
    os64_ui_widget_t *record=&g.record[0];os64_ui_set_focus(&g.ui,record);
    e=edge(OS64_GUI_EVENT_KEY_DOWN,' ',0x39,0);CHECK(os64_ui_dispatch(&g.ui,&e));
    e.type=OS64_GUI_EVENT_KEY_UP;CHECK(os64_ui_dispatch(&g.ui,&e));CHECK(g.targets[0].hits==2);
    CHECK(os64_ui_action_invoke(g.views[2],PANEL).status==OS64_UI_OK&&g.panel.hidden&&g.ui.focus==&g.content);
    CHECK(os64_ui_action_snapshot(g.views[2],PANEL,&state)==OS64_UI_OK&&!state.checked);
    CHECK(os64_ui_action_invoke(g.views[2],PANEL).status==OS64_UI_OK&&!g.panel.hidden);
    CHECK(os64_ui_action_invoke(g.views[2],STATUS).status==OS64_UI_OK&&g.status.w.hidden);
    CHECK(os64_ui_action_invoke(g.views[2],HELP).status==OS64_UI_OK);
    CHECK(os64_ui_action_invoke(g.views[2],HELP).status==OS64_UI_OK);
    CHECK(os64_ui_action_invoke(g.views[2],CLEAR_HELP).status==OS64_UI_REFUSED&&g.status.help.token==g.help_token);
    CHECK(os64_ui_action_invoke(g.views[2],STATUS).status==OS64_UI_OK&&!g.status.w.hidden);
#ifdef UI_TEXT_REAL
    os64_font_set_t *set=outline_set(os64_ui_font_context(&g.ui),dir,"DejaVuSans.ttf",32);
    CHECK(set!=NULL);if(set) {
        os64_font_consumer_t consumer;os64_ui_font_consumer(&g.ui,&consumer);void *adopt=NULL;
        int32_t old_height=g.record[0].bounds.h;
        CHECK(consumer.prepare(consumer.user,set,&adopt)==OS64_FONT_OK);
        ui_test_deny_all=true;consumer.commit(consumer.user,adopt);ui_test_deny_all=false;
        CHECK(g.record[0].bounds.h>old_height&&g.status.help.token==g.help_token&&g.targets[0].hits==2);
        CHECK(g.status.w.bounds.y+g.status.w.bounds.h<=g.root.bounds.h);
        os64_font_set_release(set);
    }
#endif
    /* Close callback returns through production button dispatch before the
     * app detaches/destroys its tree. References protect the view meanwhile. */
    os64_ui_set_focus(&g.ui,&g.controls[5]);
    e=edge(OS64_GUI_EVENT_KEY_DOWN,' ',0x39,0);CHECK(os64_ui_dispatch(&g.ui,&e));
    e.type=OS64_GUI_EVENT_KEY_UP;CHECK(os64_ui_dispatch(&g.ui,&e)&&g.ui.quit);
    gallery_dispose(&g);CHECK(!g.targets[0].refs&&!g.targets[1].refs&&g.ui.font==NULL);
    free(pixels);
}
static void gallery_font_visibility_tests(const char *dir)
{
    (void)dir;current="gallery font visibility";
    for(unsigned hidden_panel=0;hidden_panel<2;++hidden_panel) {
        uint32_t *pixels=calloc(878u*718u,sizeof(*pixels));CHECK(pixels!=NULL);
        g=(gallery){0};g.draw.surf=(os64_gui_surface_t){.pixels=pixels,.width=878,.height=718,.pitch_px=878};
        CHECK(gallery_build(&g));
        if(hidden_panel) CHECK(os64_ui_action_invoke(g.views[2],PANEL).status==OS64_UI_OK);
        CHECK(os64_ui_action_invoke(g.views[2],HELP).status==OS64_UI_OK);
        os64_font_set_t *set=NULL;
#ifdef UI_TEXT_REAL
        set=outline_set(os64_ui_font_context(&g.ui),dir,"DejaVuSans.ttf",32);
#else
        CHECK(os64_font_set_prepare(os64_ui_font_context(&g.ui),NULL,&set)==OS64_FONT_OK);
#endif
        CHECK(set!=NULL);
        if(set) {
            os64_font_consumer_t consumer;os64_ui_font_consumer(&g.ui,&consumer);void *adopt=NULL;
            /* Derive fit boundaries from the production candidate layout's
             * visible bottom edges, independently of the planner's row count. */
            CHECK(consumer.prepare(consumer.user,set,&adopt)==OS64_FONT_OK);
            if(adopt) {
                os64_gui_rect_t last=os64_ui_widget_planned_bounds(&g.controls[5]);
                os64_gui_rect_t status=os64_ui_widget_planned_bounds(&g.status.w);
                int32_t controls_fit=last.y+last.h+g.ui.theme.pad;
                int32_t status_fit=status.y+status.h+g.ui.theme.pad;
                consumer.abort(consumer.user,adopt);adopt=NULL;
                g.root.bounds.h=controls_fit;
                CHECK(consumer.prepare(consumer.user,set,&adopt)==OS64_FONT_LIMIT&&!adopt);
                CHECK(os64_ui_action_invoke(g.views[2],STATUS).status==OS64_UI_OK&&g.status.w.hidden);
                os64_gui_rect_t prior=g.controls[5].bounds;
                g.root.bounds.h=controls_fit-1;
                CHECK(consumer.prepare(consumer.user,set,&adopt)==OS64_FONT_LIMIT&&!adopt);
                CHECK(!memcmp(&prior,&g.controls[5].bounds,sizeof(prior))&&!g.controls[5].bounds_staged_valid);
                g.root.bounds.h=controls_fit;
                CHECK(consumer.prepare(consumer.user,set,&adopt)==OS64_FONT_OK);
                CHECK(!memcmp(&prior,&g.controls[5].bounds,sizeof(prior)));
                if(adopt) {
                    ui_test_deny_all=true;consumer.commit(consumer.user,adopt);ui_test_deny_all=false;adopt=NULL;
                    CHECK(g.controls[5].bounds.y+g.controls[5].bounds.h+g.ui.theme.pad==g.root.bounds.h);
                    CHECK(g.status.w.hidden&&g.status.help.token==g.help_token&&g.status.notice.token==g.notice_token);
                    CHECK(g.status.help.run&&g.status.notice.run);
                }
                CHECK(os64_ui_action_invoke(g.views[2],STATUS).status==OS64_UI_OK&&!g.status.w.hidden);
                CHECK(consumer.prepare(consumer.user,set,&adopt)==OS64_FONT_LIMIT&&!adopt);
                g.root.bounds.h=status_fit;
                CHECK(consumer.prepare(consumer.user,set,&adopt)==OS64_FONT_OK);
                if(adopt) {
                    consumer.commit(consumer.user,adopt);adopt=NULL;
                    CHECK(g.status.w.bounds.y+g.status.w.bounds.h+g.ui.theme.pad==g.root.bounds.h);
                }
            }
            os64_font_set_release(set);
        }
        gallery_dispose(&g);CHECK(!g.targets[0].refs&&!g.targets[1].refs&&g.ui.font==NULL);
        free(pixels);
    }
}
int main(int argc,char **argv)
{
    const char *dir=argc>1?argv[1]:"userland/libfreetype/fixtures";
    registration();views_and_bindings();shortcuts();ctrl_punctuation();
    status_tests(dir);status_fit_tests(dir);gallery_tests(dir);gallery_font_visibility_tests(dir);
    printf("action/status: %lu checks, %d failures\n",checks,failures);
    return failures?1:0;
}
