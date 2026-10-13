/* Reuse allocator/font/window seams. E3 tests call public APIs and dispatch
 * production events; they do not include either library implementation. */
#define os64_malloc e3_base_allocator
#define main e3_existing_font_suite
#include "test_ui_text_host.c"
#undef main
#undef os64_malloc
static size_t reserve_next;
void *os64_malloc(size_t n)
{
    size_t reserve=reserve_next; reserve_next=0;
    return e3_base_allocator(n+reserve);
}
#define E3_CORE_EMBED
#include "test_ui_data_host.c"
#include <time.h>
#include <malloc.h>
size_t os64_malloc_size(const void *p) { return p?malloc_usable_size((void *)p):0; }
#include "os64/ui_dataview.h"
#include "image/icon_theme.h"
#include "../userland/tests/actiongallery/details_page.c"

int64_t os64_micros(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return t.tv_sec*INT64_C(1000000)+t.tv_nsec/1000; }
static size_t io_calls;
int64_t os64_open(const char *p,const char *m) { ++io_calls; (void)p; (void)m; return -1; }
int64_t os64_read(int32_t fd,void *p,size_t n) { ++io_calls; (void)fd; (void)p; (void)n; return -1; }
int64_t os64_write(int32_t fd,const void *p,size_t n) { ++io_calls; (void)fd; (void)p; return (int64_t)n; }
int64_t os64_close(int32_t fd) { ++io_calls; (void)fd; return 0; }
bool os64_ui_theme_session(os64_ui_theme_t *t,uint64_t *generation,uint64_t hint) { (void)t; (void)generation; (void)hint; return false; }
void os64_ui_theme_current(os64_ui_theme_t *t,uint64_t *generation) { os64_ui_theme_defaults(t); *generation=0; }
os64_ui_status_code_t os64_icon_theme_create(os64_icon_theme_t **out) { *out=NULL; return OS64_UI_NO_MEMORY; }
const os64_ui_icon_provider_t *os64_icon_theme_provider(os64_icon_theme_t *t) { (void)t; return NULL; }
size_t os64_icon_theme_cache_bytes(const os64_icon_theme_t *t) { (void)t; return 0; }
os64_ui_status_code_t os64_icon_theme_destroy(os64_icon_theme_t *t) { (void)t; return OS64_UI_OK; }

typedef struct {
    os64_ui_t ui;
    os64_draw_ctx_t draw;
    os64_ui_widget_t root;
    os64_ui_dataview_t view;
    os64_ui_selection_t selection;
    os64_ui_data_t *data;
    os64_ui_icon_provider_t provider;
    size_t acquires, releases, attempts, changes, errors, activates;
    uint64_t activated, flags;
    uint32_t icon_pixels[32*32];
    bool icon_fail;
    uint32_t pixels[800*480];
} view_fixture;
static int64_t test_time=1000000;
static int64_t fixture_clock(void *u) { (void)u; return test_time; }
static os64_ui_icon_status_t fixture_icon(void *user,const char *key,uint32_t size,os64_ui_icon_lease_t *out)
{
    view_fixture *f=user; (void)key; ++f->attempts;
    if (f->icon_fail) return OS64_UI_ICON_NO_MEMORY;
    ++f->acquires;
    *out=(os64_ui_icon_lease_t){f->icon_pixels,size,size,32,1,(void *)1};
    return OS64_UI_ICON_OK;
}
static void fixture_release(void *user,os64_ui_icon_lease_t *lease)
{ view_fixture *f=user; CHECK(lease->cookie==(void *)1); ++f->releases; }
static void fixture_changed(os64_ui_dataview_t *v,const os64_ui_dataview_change_t *change,void *user)
{
    view_fixture *f=user; ++f->changes; f->flags=change->flags;
    if (change->activated) { ++f->activates; f->activated=change->activated; }
    CHECK(change->revision==os64_ui_dataview_revision(v));
    CHECK(os64_ui_dataview_bind(v,os64_ui_selection_projection(&f->selection),OS64_UI_DATA_PRESERVE)==OS64_UI_BUSY);
    CHECK(os64_ui_dataview_scroll(v,0,0)==OS64_UI_BUSY);
    CHECK(os64_ui_dataview_dispose(v)==OS64_UI_BUSY);
    CHECK(os64_ui_selection_command(&f->selection,OS64_UI_SELECTION_CLEAR,0)==OS64_UI_BUSY);
    CHECK(os64_ui_selection_dispose(&f->selection)==OS64_UI_BUSY);
}
static void fixture_error(os64_ui_dataview_t *v,os64_ui_dataview_operation_t op,os64_ui_status_code_t rc,void *user)
{
    (void)op; CHECK(rc!=OS64_UI_OK); ++((view_fixture *)user)->errors;
    CHECK(os64_ui_dataview_layout(v)==OS64_UI_BUSY);
    CHECK(os64_ui_dataview_dispose(v)==OS64_UI_BUSY);
    CHECK(os64_ui_dataview_status(v,NULL)==rc);
}
static view_fixture *view_new(size_t count,const char *dir)
{
    view_fixture *f=calloc(1,sizeof(*f)); CHECK(f!=NULL); if (!f) return NULL;
    f->provider=(os64_ui_icon_provider_t){f,fixture_icon,fixture_release};
    for (size_t i=0;i<32*32;++i) f->icon_pixels[i]=0xff18cc67;
    f->draw.surf=(os64_gui_surface_t){.pixels=f->pixels,.width=800,.height=480,.pitch_px=800};
    os64_ui_theme_defaults(&f->ui.theme); f->ui.ctx=&f->draw;
    os64_ui_panel(&f->root); f->root.bounds=(os64_gui_rect_t){0,0,800,480};
    os64_ui_dataview_init(&f->view); f->view.w.bounds=(os64_gui_rect_t){11,13,640,420};
    os64_ui_add_child(&f->root,&f->view.w); os64_ui_set_root(&f->ui,&f->root);
#ifdef UI_TEXT_REAL
    os64_font_set_t *set=outline_set(os64_ui_font_context(&f->ui),dir,"DejaVuSans.ttf",17);
    os64_font_consumer_t consumer; os64_ui_font_consumer(&f->ui,&consumer);
    CHECK(os64_font_adopt(set,&consumer,1,NULL)==OS64_FONT_OK); os64_font_set_release(set);
#else
    (void)dir;
#endif
    CHECK(details_fixture(count,1,0,0,0,&f->data)==OS64_UI_OK);
    os64_ui_data_projection_t *p=NULL; CHECK(os64_ui_data_projection_identity(f->data,&p)==OS64_UI_OK);
    CHECK(os64_ui_selection_bind(&f->selection,p,OS64_UI_DATA_RESET)==OS64_UI_OK); os64_ui_data_projection_release(p);
    CHECK(os64_ui_dataview_attach(&f->view,&f->selection,&f->provider)==OS64_UI_OK);
    f->view.on_change=fixture_changed; f->view.on_error=fixture_error; f->view.callback_user=f;
    CHECK(os64_ui_dataview_clock(&f->view,fixture_clock,NULL)==OS64_UI_OK);
    CHECK(!os64_ui_dataview_diagnostics(&f->view).clock_queried && !os64_ui_dataview_diagnostics(&f->view).clock_available);
    os64_ui_set_focus(&f->ui,&f->view.w);
    return f;
}
static void view_delete(view_fixture *f)
{
    CHECK(os64_ui_font_release(&f->ui)==OS64_FONT_OK);
    CHECK(f->acquires==f->releases);
    CHECK(os64_ui_dataview_dispose(&f->view)==OS64_UI_OK);
    CHECK(os64_ui_selection_dispose(&f->selection)==OS64_UI_OK);
    os64_ui_data_release(f->data); CHECK(os64_ui_font_live_bytes(&f->ui)==0); free(f);
}
static void render(view_fixture *f)
{
    unsigned long before=allocations; size_t attempts=f->attempts, ios=io_calls;
    size_t prepared=os64_ui_dataview_diagnostics(&f->view).prepared_rows;
    f->view.w.cls->paint(&f->view.w,&f->draw,&f->ui.theme);
    CHECK(allocations==before && f->attempts==attempts && io_calls==ios);
    CHECK(os64_ui_dataview_diagnostics(&f->view).prepared_rows==prepared);
}
static void key_event(view_fixture *f,uint32_t type,char ascii,uint8_t scan,uint8_t mods)
{
    os64_gui_event_t e={.type=type}; e.key.ascii=ascii; e.key.scancode=scan; e.key.modifiers=mods;
    (void)os64_ui_dispatch(&f->ui,&e);
}
static void burst(view_fixture *f,const char *tail,uint8_t mods)
{
    key_event(f,OS64_GUI_EVENT_KEY_DOWN,27,0x48,mods);
    while (*tail) key_event(f,OS64_GUI_EVENT_KEY_DOWN,*tail++,0x48,mods);
}
static os64_gui_event_t pointer_event(view_fixture *f,uint32_t type,size_t row,uint8_t mods)
{
    int32_t line=os64_ui_font_row_height(&f->ui,OS64_FONT_ROLE_UI);
    int32_t pitch=(line>(int32_t)os64_ui_icon_menu_size(line)?line:(int32_t)os64_ui_icon_menu_size(line))+8;
    os64_gui_event_t e={.type=type}; e.mouse.button=OS64_GUI_MOUSE_LEFT;
    e.mouse.x=f->view.w.bounds.x+60; e.mouse.y=f->view.w.bounds.y+1+line+12+(int32_t)row*pitch+6; e.mouse.modifiers=mods;
    return e;
}
static void click(view_fixture *f,size_t row,uint8_t mods)
{
    os64_gui_event_t e=pointer_event(f,OS64_GUI_EVENT_MOUSE_BUTTON_DOWN,row,mods); CHECK(os64_ui_dispatch(&f->ui,&e));
    e.type=OS64_GUI_EVENT_MOUSE_BUTTON_UP; CHECK(os64_ui_dispatch(&f->ui,&e));
}
static uint64_t id_at(view_fixture *f,size_t at)
{ return os64_ui_data_projection_id(os64_ui_selection_projection(&f->selection),at); }
static void widget_input(const char *dir)
{
    current="real widget selection / keyboard / context / typeahead"; view_fixture *f=view_new(10000,dir);
    os64_gui_event_t f2={.type=OS64_GUI_EVENT_KEY_DOWN}; f2.key.scancode=0x3c;
    CHECK(!os64_ui_dispatch(&f->ui,&f2)); f2.key.scancode=0x3b; f2.key.modifiers=OS64_GUI_MOD_HID;
    CHECK(!os64_ui_dispatch(&f->ui,&f2));
    uint64_t b=id_at(f,1), c=id_at(f,2), d=id_at(f,3);
    click(f,1,0); click(f,3,OS64_GUI_MOD_CTRL); tuple(&f->selection,(uint64_t[]){b,d},2,d,d);
    click(f,1,OS64_GUI_MOD_SHIFT); tuple(&f->selection,(uint64_t[]){b,c,d},3,b,d);
    click(f,2,OS64_GUI_MOD_SHIFT); tuple(&f->selection,(uint64_t[]){c,d},2,c,d);
    click(f,1,0); burst(f,"[B",OS64_GUI_MOD_CTRL); tuple(&f->selection,(uint64_t[]){b},1,c,b);
    key_event(f,OS64_GUI_EVENT_KEY_DOWN,' ',0x39,OS64_GUI_MOD_CTRL); tuple(&f->selection,(uint64_t[]){b,c},2,c,c);
    key_event(f,OS64_GUI_EVENT_KEY_DOWN,' ',0x39,OS64_GUI_MOD_CTRL); tuple(&f->selection,(uint64_t[]){b,c},2,c,c);
    key_event(f,OS64_GUI_EVENT_KEY_UP,0,0x39,0); key_event(f,OS64_GUI_EVENT_KEY_DOWN,' ',0x39,OS64_GUI_MOD_CTRL); tuple(&f->selection,(uint64_t[]){b},1,c,c);
    key_event(f,OS64_GUI_EVENT_KEY_UP,0,0x39,0);
    unsigned long calls=f->changes;
    CHECK(os64_ui_dataview_scroll(&f->view,100,32)==OS64_UI_OK && f->changes==calls);
    tuple(&f->selection,(uint64_t[]){b},1,c,c);
    burst(f,"[B",OS64_GUI_MOD_CTRL); CHECK(os64_ui_selection_focus(&f->selection)==d && os64_ui_dataview_top(&f->view)<=3);
    key_event(f,OS64_GUI_EVENT_KEY_DOWN,1,0x1e,OS64_GUI_MOD_CTRL); CHECK(os64_ui_selection_count(&f->selection)==10000);
    key_event(f,OS64_GUI_EVENT_KEY_DOWN,'\n',0x1c,0); key_event(f,OS64_GUI_EVENT_KEY_DOWN,'\n',0x1c,0);
    CHECK(f->activates==1 && f->activated==d);
    CHECK(os64_ui_dataview_sort(&f->view,(os64_ui_data_sort_t){2,true})==OS64_UI_OK);
    key_event(f,OS64_GUI_EVENT_KEY_DOWN,'\n',0x1c,0); CHECK(f->activates==1);
    key_event(f,OS64_GUI_EVENT_KEY_UP,0,0x1c,0);
    CHECK(os64_ui_dataview_bind(&f->view,os64_ui_selection_projection(&f->selection),OS64_UI_DATA_RESET)==OS64_UI_OK);
    os64_ui_data_projection_t *source_order=NULL;
    CHECK(os64_ui_data_projection_identity(f->data,&source_order)==OS64_UI_OK);
    CHECK(os64_ui_dataview_bind(&f->view,source_order,OS64_UI_DATA_RESET)==OS64_UI_OK);
    os64_ui_data_projection_release(source_order);
    CHECK(os64_ui_dataview_command(&f->view,OS64_UI_SELECTION_CHOOSE,b,true)==OS64_UI_OK);
    key_event(f,OS64_GUI_EVENT_KEY_DOWN,'a',0x1e,0); uint64_t first=os64_ui_selection_focus(&f->selection);
    CHECK(os64_ui_dataview_diagnostics(&f->view).clock_queried && os64_ui_dataview_diagnostics(&f->view).clock_available);
    key_event(f,OS64_GUI_EVENT_KEY_DOWN,'a',0x1e,0); uint64_t second=os64_ui_selection_focus(&f->selection); CHECK(first!=second);
    key_event(f,OS64_GUI_EVENT_KEY_DOWN,'l',0x26,0); CHECK(os64_ui_selection_focus(&f->selection)==second);
    key_event(f,OS64_GUI_EVENT_KEY_DOWN,'p',0x19,0); CHECK(os64_ui_selection_focus(&f->selection)==second);
    key_event(f,OS64_GUI_EVENT_KEY_DOWN,'h',0x23,0); uint64_t narrowed=os64_ui_selection_focus(&f->selection);
    CHECK(narrowed!=second && !strncmp(os64_ui_data_row(f->data,narrowed)->search.bytes,"Alpha",5));
    key_event(f,OS64_GUI_EVENT_KEY_DOWN,'z',0x2c,0); CHECK(os64_ui_selection_focus(&f->selection)==narrowed);
    key_event(f,OS64_GUI_EVENT_KEY_DOWN,'\b',0x0e,0); CHECK(os64_ui_selection_focus(&f->selection)==narrowed);
    test_time+=1000001; key_event(f,OS64_GUI_EVENT_KEY_DOWN,'b',0x30,0);
    CHECK(os64_ui_data_row(f->data,os64_ui_selection_focus(&f->selection))->search.bytes[0]=='B');
    test_time=-1;
    key_event(f,OS64_GUI_EVENT_KEY_DOWN,'a',0x1e,0);
    uint64_t unavailable_focus=os64_ui_selection_focus(&f->selection);
    key_event(f,OS64_GUI_EVENT_KEY_DOWN,'l',0x26,0);
    CHECK(os64_ui_selection_focus(&f->selection)==unavailable_focus && os64_ui_dataview_diagnostics(&f->view).clock_queried && !os64_ui_dataview_diagnostics(&f->view).clock_available);
    test_time=4000000;
    key_event(f,OS64_GUI_EVENT_KEY_DOWN,'a',0x1e,0);
    CHECK(os64_ui_dataview_diagnostics(&f->view).clock_queried && os64_ui_dataview_diagnostics(&f->view).clock_available);
    CHECK(os64_ui_dataview_scroll(&f->view,0,0)==OS64_UI_OK);
    CHECK(os64_ui_dataview_command(&f->view,OS64_UI_SELECTION_CHOOSE,b,false)==OS64_UI_OK);
    CHECK(os64_ui_dataview_command(&f->view,OS64_UI_SELECTION_TOGGLE,d,false)==OS64_UI_OK);
    os64_gui_event_t e=pointer_event(f,OS64_GUI_EVENT_MOUSE_BUTTON_DOWN,1,0); e.mouse.button=OS64_GUI_MOUSE_RIGHT;
    uint64_t target=0; CHECK(os64_ui_dataview_context_target(&f->view,&e,&target)==OS64_UI_OK && target==b);
    tuple(&f->selection,(uint64_t[]){b,d},2,b,d);
    render(f); CHECK(f->acquires-f->releases==os64_ui_dataview_diagnostics(&f->view).icon_leases);
    view_delete(f);
}
static void interruptions(const char *dir)
{
    current="interrupted deliveries and revision-bound presses"; view_fixture *f=view_new(100,dir);
    uint64_t id=id_at(f,1);
    for (unsigned mode=0;mode<7;++mode) {
        os64_gui_event_t down=pointer_event(f,OS64_GUI_EVENT_MOUSE_BUTTON_DOWN,1,0);
        CHECK(os64_ui_dispatch(&f->ui,&down));
        if (mode==0) CHECK(os64_ui_dataview_sort(&f->view,(os64_ui_data_sort_t){2,true})==OS64_UI_OK);
        if (mode==1) { os64_gui_event_t loss={.type=OS64_GUI_EVENT_INPUT_LOST}; os64_ui_dispatch(&f->ui,&loss); }
        if (mode==2) { os64_gui_event_t loss={.type=OS64_GUI_EVENT_WINDOW_COVERED}; os64_ui_dispatch(&f->ui,&loss); }
        if (mode==3) { os64_gui_event_t loss={.type=OS64_GUI_EVENT_WINDOW_FOCUS}; loss.focus.gained=false; os64_ui_dispatch(&f->ui,&loss); loss.focus.gained=true; os64_ui_dispatch(&f->ui,&loss); }
        if (mode==4) { os64_ui_set_hidden(&f->ui,&f->view.w,true); os64_ui_set_hidden(&f->ui,&f->view.w,false); }
        if (mode==5) { os64_ui_set_enabled(&f->ui,&f->view.w,false); os64_ui_set_enabled(&f->ui,&f->view.w,true); }
        if (mode==6) { os64_ui_set_root(&f->ui,NULL); os64_ui_set_root(&f->ui,&f->root); }
        down.type=OS64_GUI_EVENT_MOUSE_BUTTON_UP; (void)os64_ui_dispatch(&f->ui,&down);
        CHECK(os64_ui_selection_count(&f->selection)==(mode?1u:0u));
        click(f,1,0); CHECK(os64_ui_selection_count(&f->selection)==1);
        os64_ui_set_focus(&f->ui,&f->view.w);
        key_event(f,OS64_GUI_EVENT_KEY_DOWN,27,0x48,0);
        os64_gui_event_t loss={.type=OS64_GUI_EVENT_INPUT_LOST}; os64_ui_dispatch(&f->ui,&loss);
        key_event(f,OS64_GUI_EVENT_KEY_DOWN,'b',0x30,0);
        CHECK(os64_ui_data_row(f->data,os64_ui_selection_focus(&f->selection))->search.bytes[0]=='B');
    }
    /* Orphaned burst bytes after cancellation must not enter the prefix. */
    CHECK(os64_ui_dataview_command(&f->view,OS64_UI_SELECTION_CHOOSE,id_at(f,0),true)==OS64_UI_OK);
    key_event(f,OS64_GUI_EVENT_KEY_DOWN,27,0x48,0);
    os64_gui_event_t orphan_loss={.type=OS64_GUI_EVENT_INPUT_LOST}; os64_ui_dispatch(&f->ui,&orphan_loss);
    key_event(f,OS64_GUI_EVENT_KEY_DOWN,'[',0x48,0); key_event(f,OS64_GUI_EVENT_KEY_DOWN,'A',0x48,0);
    key_event(f,OS64_GUI_EVENT_KEY_DOWN,'b',0x30,0);
    CHECK(os64_ui_data_row(f->data,os64_ui_selection_focus(&f->selection))->search.bytes[0]=='B');
    /* HID navigation, Ctrl selection, and a lost Space release. */
    uint64_t old_focus=os64_ui_selection_focus(&f->selection);
    key_event(f,OS64_GUI_EVENT_KEY_DOWN,0,0x51,OS64_GUI_MOD_HID|OS64_GUI_MOD_CTRL);
    CHECK(os64_ui_selection_focus(&f->selection)!=old_focus && os64_ui_selection_contains(&f->selection,old_focus));
    key_event(f,OS64_GUI_EVENT_KEY_DOWN,' ',0x2c,OS64_GUI_MOD_HID|OS64_GUI_MOD_CTRL);
    uint64_t toggled=os64_ui_selection_focus(&f->selection); CHECK(os64_ui_selection_contains(&f->selection,toggled));
    os64_ui_dispatch(&f->ui,&orphan_loss);
    key_event(f,OS64_GUI_EVENT_KEY_DOWN,' ',0x2c,OS64_GUI_MOD_HID|OS64_GUI_MOD_CTRL);
    CHECK(!os64_ui_selection_contains(&f->selection,toggled));
    key_event(f,OS64_GUI_EVENT_KEY_UP,0,0x2c,OS64_GUI_MOD_HID);
    size_t before=f->activates;
    key_event(f,OS64_GUI_EVENT_KEY_DOWN,'\n',0x1c,0); key_event(f,OS64_GUI_EVENT_KEY_DOWN,'\n',0x1c,0); CHECK(f->activates==before+1);
    os64_gui_event_t loss={.type=OS64_GUI_EVENT_WINDOW_FOCUS}; os64_ui_dispatch(&f->ui,&loss);
    loss.focus.gained=true; os64_ui_dispatch(&f->ui,&loss);
    key_event(f,OS64_GUI_EVENT_KEY_DOWN,'\n',0x1c,0); CHECK(f->activates==before+2);
    key_event(f,OS64_GUI_EVENT_KEY_UP,0,0x1c,0);
    CHECK(os64_ui_dataview_scroll(&f->view,0,0)==OS64_UI_OK);
    before=f->activates; test_time+=1000000;
    click(f,1,0); click(f,1,0); click(f,1,0); CHECK(f->activates==before+1);
    CHECK(os64_ui_dataview_scroll(&f->view,2,0)==OS64_UI_OK); click(f,1,0); CHECK(f->activates==before+1);
    (void)id; view_delete(f);
}
static void allocation_sweep(const char *dir)
{
    current="successive replacement allocation failures preserve pixels/tuple/order/top";
    view_fixture *f=view_new(100,dir);
    CHECK(os64_ui_dataview_sort(&f->view,(os64_ui_data_sort_t){2,true})==OS64_UI_OK);
    uint64_t id=id_at(f,1); CHECK(os64_ui_dataview_command(&f->view,OS64_UI_SELECTION_CHOOSE,id,false)==OS64_UI_OK);
    CHECK(os64_ui_dataview_scroll(&f->view,9,24)==OS64_UI_OK);
    os64_ui_data_projection_t *p=NULL; CHECK(os64_ui_data_projection_sort(os64_ui_selection_projection(&f->selection),(os64_ui_data_sort_t){2,false},&p)==OS64_UI_OK);
    render(f); uint32_t *pixels=malloc(sizeof(f->pixels)); memcpy(pixels,f->pixels,sizeof(f->pixels));
    bool success=false; size_t points=0;
    for (long fail=0;fail<1500 && !success;++fail) {
        uint64_t rev=os64_ui_dataview_revision(&f->view); size_t changes=f->changes;
        deny_countdown=fail; os64_ui_status_code_t rc=os64_ui_dataview_bind(&f->view,p,OS64_UI_DATA_PRESERVE); deny_countdown=-1;
        if (!rc) { success=true; points=(size_t)fail; }
        else {
            CHECK(rc==OS64_UI_NO_MEMORY || rc==OS64_UI_LIMIT);
            CHECK(os64_ui_dataview_revision(&f->view)==rev && f->changes==changes);
            CHECK(os64_ui_dataview_top(&f->view)==9 && os64_ui_dataview_left(&f->view)==24);
            tuple(&f->selection,&id,1,id,id);
            CHECK(os64_ui_data_projection_order(os64_ui_selection_projection(&f->selection)).descending);
            render(f); CHECK(!memcmp(pixels,f->pixels,sizeof(f->pixels)));
            CHECK(f->acquires-f->releases==os64_ui_dataview_diagnostics(&f->view).icon_leases);
        }
    }
    CHECK(success); printf("replacement allocation sweep: %zu preparation failure points\n",points);
    os64_ui_data_projection_release(p); free(pixels);
    f->icon_fail=true; CHECK(os64_ui_dataview_scroll(&f->view,0,0)==OS64_UI_OK);
    CHECK(os64_ui_dataview_diagnostics(&f->view).icon_leases==0 && os64_ui_dataview_diagnostics(&f->view).icon_failures>0); render(f);
    f->icon_fail=false; CHECK(os64_ui_dataview_layout(&f->view)==OS64_UI_OK);
    CHECK(os64_ui_dataview_command(&f->view,OS64_UI_SELECTION_CHOOSE,id_at(f,0),false)==OS64_UI_OK);
    uint64_t focus=os64_ui_selection_focus(&f->selection), rev=os64_ui_dataview_revision(&f->view);
    deny_countdown=1; CHECK(os64_ui_dataview_command(&f->view,OS64_UI_SELECTION_CHOOSE,id_at(f,99),true)==OS64_UI_NO_MEMORY); deny_countdown=-1;
    CHECK(os64_ui_selection_focus(&f->selection)==focus && os64_ui_dataview_revision(&f->view)==rev && os64_ui_dataview_top(&f->view)==0);
    deny_countdown=0; CHECK(os64_ui_dataview_scroll(&f->view,30,0)==OS64_UI_NO_MEMORY);
#ifdef UI_TEXT_REAL
    os64_font_set_t *candidate=outline_set(os64_ui_font_context(&f->ui),dir,"DejaVuSans.ttf",25);
    os64_font_consumer_t consumer; os64_ui_font_consumer(&f->ui,&consumer);
    deny_countdown=0; CHECK(os64_font_adopt(candidate,&consumer,1,NULL)==OS64_FONT_NO_MEMORY); deny_countdown=-1;
    CHECK(os64_ui_dataview_top(&f->view)==0 && os64_ui_selection_focus(&f->selection)==focus);
    /* Sweep through widget text preparation under the candidate metrics. */
    render(f); uint32_t *old_pixels=malloc(sizeof(f->pixels)); memcpy(old_pixels,f->pixels,sizeof(f->pixels));
    int32_t old_height=os64_ui_font_row_height(&f->ui,OS64_FONT_ROLE_UI);
    bool adopted=false; size_t adoption_points=0;
    for (long fail=0;fail<1200 && !adopted;++fail) {
        deny_countdown=fail; os64_font_status_t status=os64_font_adopt(candidate,&consumer,1,NULL); deny_countdown=-1;
        if (status==OS64_FONT_OK) { adopted=true; adoption_points=(size_t)fail; }
        else {
            CHECK(status==OS64_FONT_NO_MEMORY || status==OS64_FONT_LIMIT || status==OS64_FONT_ENGINE_ERROR);
            CHECK(os64_ui_font_row_height(&f->ui,OS64_FONT_ROLE_UI)==old_height);
            CHECK(os64_ui_selection_focus(&f->selection)==focus && os64_ui_dataview_top(&f->view)==0);
            render(f); CHECK(!memcmp(old_pixels,f->pixels,sizeof(f->pixels)));
            CHECK(f->acquires-f->releases==os64_ui_dataview_diagnostics(&f->view).icon_leases);
        }
    }
    CHECK(adopted); printf("font adoption allocation sweep: %zu failure points\n",adoption_points);
    void *stage=NULL;
    CHECK(consumer.prepare(consumer.user,candidate,&stage)==OS64_FONT_OK && stage);
    CHECK(os64_ui_dataview_dispose(&f->view)==OS64_UI_BUSY);
    CHECK(os64_ui_dataview_scroll(&f->view,1,0)==OS64_UI_BUSY);
    consumer.abort(consumer.user,stage);
    CHECK(os64_ui_selection_focus(&f->selection)==focus && !os64_ui_dataview_blocked(&f->view));
    free(old_pixels); os64_font_set_release(candidate);
#endif
    f->view.w.bounds.w=480; f->view.w.bounds.h=203;
    deny_countdown=0; CHECK(os64_ui_dataview_layout(&f->view)==OS64_UI_NO_MEMORY); CHECK(os64_ui_dataview_blocked(&f->view)); render(f);
    CHECK(os64_ui_dataview_diagnostics(&f->view).painted_rows==0);
    uint64_t selected=os64_ui_selection_focus(&f->selection); size_t activations=f->activates;
    os64_gui_event_t e=pointer_event(f,OS64_GUI_EVENT_MOUSE_BUTTON_DOWN,0,0); os64_ui_dispatch(&f->ui,&e); e.type=OS64_GUI_EVENT_MOUSE_BUTTON_UP; os64_ui_dispatch(&f->ui,&e);
    CHECK(os64_ui_selection_focus(&f->selection)==selected && f->activates==activations);
    deny_countdown=0; CHECK(os64_ui_dataview_layout(&f->view)==OS64_UI_NO_MEMORY); CHECK(os64_ui_dataview_blocked(&f->view));
    CHECK(os64_ui_dataview_layout(&f->view)==OS64_UI_OK && !os64_ui_dataview_blocked(&f->view)); render(f);
    for (unsigned i=0;i<3;++i) { CHECK(os64_ui_dataview_column_set(&f->view,2,4096,false,3)==OS64_UI_OK); CHECK(os64_ui_dataview_column_set(&f->view,2,48,true,0)==OS64_UI_OK); render(f); }
    CHECK(os64_ui_dataview_column_set(&f->view,1,100,false,1)==OS64_UI_INVALID);
    /* Failed scrollbar preparation restores its speculative position. */
    int64_t committed=f->view.vertical.pos;
    f->view.vertical.pos=37; deny_countdown=0;
    f->view.vertical.on_scroll(&f->view.vertical,f->view.vertical.scroll_user);
    CHECK(f->view.vertical.pos==committed && os64_ui_dataview_top(&f->view)==(size_t)committed);
    os64_ui_dataview_column_t before_column, after_column;
    CHECK(os64_ui_dataview_column(&f->view,0,&before_column));
    deny_countdown=0;
    CHECK(os64_ui_dataview_column_set(&f->view,before_column.id,400,!before_column.visible,2)==OS64_UI_NO_MEMORY);
    CHECK(os64_ui_dataview_column(&f->view,0,&after_column));
    CHECK(before_column.id==after_column.id && before_column.width==after_column.width && before_column.visible==after_column.visible);
    f->view.w.cls->reface(&f->view.w); CHECK(os64_ui_dataview_blocked(&f->view)); render(f);
    CHECK(os64_ui_dataview_layout(&f->view)==OS64_UI_OK); view_delete(f);
}
static void bounds_and_independence(const char *dir)
{
    current="bounded viewport resources / independent views";
    view_fixture *f=view_new(10000,dir), *other=view_new(1,dir);
    CHECK(os64_ui_dataview_bind(&other->view,os64_ui_selection_projection(&f->selection),OS64_UI_DATA_RESET)==OS64_UI_OK);
    CHECK(os64_ui_dataview_command(&f->view,OS64_UI_SELECTION_ALL,0,false)==OS64_UI_OK);
    CHECK(os64_ui_dataview_scroll(&f->view,110,INT64_MAX)==OS64_UI_OK); render(f);
    size_t large=os64_ui_dataview_diagnostics(&f->view).painted_rows;
    os64_ui_data_projection_t *subset=NULL;
    uint64_t ids[100]; for (size_t i=0;i<100;++i) ids[i]=os64_ui_data_row_at(f->data,i)->id;
    CHECK(os64_ui_data_projection_create(f->data,ids,100,&subset)==OS64_UI_OK);
    CHECK(os64_ui_dataview_bind(&f->view,subset,OS64_UI_DATA_RESET)==OS64_UI_OK); render(f);
    CHECK(os64_ui_dataview_diagnostics(&f->view).painted_rows==large);
    CHECK(os64_ui_data_projection_count(os64_ui_selection_projection(&other->selection))==10000 && os64_ui_selection_count(&other->selection)==0 && os64_ui_dataview_top(&other->view)==0);
    for (int h=0;h<70;++h) {
        f->view.w.bounds=(os64_gui_rect_t){11,13,h%23,h}; CHECK(os64_ui_dataview_layout(&f->view)==OS64_UI_OK); render(f);
        CHECK(os64_ui_dataview_diagnostics(&f->view).painted_rows<=2);
    }
    os64_ui_data_projection_release(subset); view_delete(f); render(other); view_delete(other);
}
static int compare_duration(const void *a,const void *b) { int64_t x=*(const int64_t *)a,y=*(const int64_t *)b; return x<y?-1:x>y; }
static void allocation_capacity(const char *dir)
{
    current="shared immutable capacity / initial font allocation refusal";
    view_fixture *f=view_new(100,dir);
    /* Preparation before tree attachment cannot borrow a later UI's fonts. */
    os64_ui_selection_t early_selection={0}; os64_ui_dataview_t early_view;
    os64_ui_dataview_init(&early_view); early_view.w.bounds=(os64_gui_rect_t){0,0,200,100};
    CHECK(os64_ui_selection_bind(&early_selection,os64_ui_selection_projection(&f->selection),OS64_UI_DATA_RESET)==OS64_UI_OK);
    CHECK(os64_ui_dataview_attach(&early_view,&early_selection,&f->provider)==OS64_UI_OK);
    os64_ui_add_child(&f->root,&early_view.w);
    CHECK(os64_ui_dataview_blocked(&early_view));
    CHECK(os64_ui_dataview_layout(&early_view)==OS64_UI_OK && !os64_ui_dataview_blocked(&early_view));
    CHECK(os64_ui_dataview_dispose(&early_view)==OS64_UI_OK);
    CHECK(os64_ui_selection_dispose(&early_selection)==OS64_UI_OK);
    os64_ui_data_projection_t *p=NULL;
    reserve_next=12u*1024u*1024u;
    CHECK(os64_ui_data_projection_identity(f->data,&p)==OS64_UI_OK);
    CHECK(os64_ui_dataview_bind(&f->view,p,OS64_UI_DATA_RESET)==OS64_UI_OK);
    CHECK(os64_ui_dataview_bind(&f->view,p,OS64_UI_DATA_PRESERVE)==OS64_UI_OK);
    CHECK(os64_ui_dataview_sort(&f->view,(os64_ui_data_sort_t){2,false})==OS64_UI_OK);
    os64_ui_data_projection_release(p);
    CHECK(os64_ui_font_release(&f->ui)==OS64_FONT_OK);
    size_t errors=f->errors;
    deny_countdown=1;
    CHECK(os64_ui_dataview_attach(&f->view,&f->selection,&f->provider)==OS64_UI_NO_MEMORY);
    deny_countdown=-1;
    CHECK(!f->view.state && f->errors==errors+1);
    CHECK(os64_ui_dataview_status(&f->view,NULL)==OS64_UI_NO_MEMORY);
    CHECK(os64_ui_selection_command(&f->selection,OS64_UI_SELECTION_CLEAR,0)==OS64_UI_OK);
    CHECK(os64_ui_dataview_attach(&f->view,&f->selection,&f->provider)==OS64_UI_OK);
    render(f); view_delete(f);
}
static void repeated_lifetime(const char *dir)
{
    current="repeated replacement/create/dispose baseline";
    for (unsigned cycle=0;cycle<5;++cycle) {
        view_fixture *f=view_new(10000,dir);
        os64_ui_data_projection_t *p=NULL;
        CHECK(os64_ui_data_projection_sort(os64_ui_selection_projection(&f->selection),(os64_ui_data_sort_t){1,true},&p)==OS64_UI_OK);
        size_t peak=0;
        for (unsigned i=0;i<20;++i) {
            CHECK(os64_ui_dataview_bind(&f->view,p,OS64_UI_DATA_PRESERVE)==OS64_UI_OK);
            CHECK(os64_ui_dataview_scroll(&f->view,i*10,0)==OS64_UI_OK); render(f);
            CHECK(f->acquires-f->releases==os64_ui_dataview_diagnostics(&f->view).icon_leases);
            size_t bytes=os64_ui_dataview_diagnostics(&f->view).metadata_bytes;
            if (i>2) CHECK(bytes==peak);
            peak=bytes;
        }
        os64_ui_data_projection_release(p); view_delete(f);
    }
}
static void performance(const char *dir)
{
    current="10000-row host resource/timing measurements";
    int64_t begin=os64_micros(); view_fixture *f=view_new(10000,dir); int64_t construction=os64_micros()-begin;
    int64_t events[100];
    for (size_t i=0;i<110;++i) {
        int64_t start=os64_micros(); CHECK(os64_ui_dataview_scroll(&f->view,i+1,0)==OS64_UI_OK); render(f);
        if (i>=10) events[i-10]=os64_micros()-start;
    }
    qsort(events,100,sizeof(*events),compare_duration);
    int64_t start=os64_micros(); CHECK(os64_ui_dataview_sort(&f->view,(os64_ui_data_sort_t){2,true})==OS64_UI_OK); int64_t sorting=os64_micros()-start;
    start=os64_micros(); key_event(f,OS64_GUI_EVENT_KEY_DOWN,'z',0x2c,0); int64_t nomatch=os64_micros()-start;
    os64_ui_dataview_diagnostics_t d=os64_ui_dataview_diagnostics(&f->view);
    printf("host 10000 rows: snapshot=%zu metadata=%zu peak=%zu font-engine=%zu font-peak=%zu runs=%zu leases=%zu visible=%zu construction-us=%ld sort-us=%ld no-match-us=%ld\n",os64_ui_data_bytes(f->data),d.metadata_bytes,d.peak_metadata_bytes,os64_ui_font_live_bytes(&f->ui),os64_ui_font_peak_bytes(&f->ui),d.runs,d.icon_leases,d.painted_rows,(long)construction,(long)sorting,(long)nomatch);
    CHECK(os64_ui_font_peak_bytes(&f->ui)>=os64_ui_font_live_bytes(&f->ui));
    printf("host 100 scroll-to-paint events after 10 warmups: median-us=%ld p95-us=%ld max-us=%ld\n",(long)events[50],(long)events[94],(long)events[99]);
    view_delete(f);
}
static struct { os64_gui_event_t event; bool delivered, consume; unsigned calls; } delivery;
static int64_t delivery_poll(void *user,int64_t handle,os64_gui_event_t *out)
{
    (void)user; (void)handle;
    if (delivery.delivered) return 0;
    delivery.delivered=true; *out=delivery.event; return 1;
}
static bool delivery_menu(os64_ui_t *ui,const os64_gui_event_t *event)
{ (void)ui; (void)event; ++delivery.calls; return delivery.consume; }
static void gallery_local_paint(void *user)
{
    details_page *a=user;
    a->views[0].w.cls->paint(&a->views[0].w,&a->draw,&a->ui.theme);
    a->status.w.cls->paint(&a->status.w,&a->draw,&a->ui.theme);
}
static void gallery_behavior(const char *dir)
{
    current="Details gallery fixtures, actions and independent datasets";
    details_page *a=calloc(1,sizeof(*a)); CHECK(a!=NULL); if (!a) return;
    uint32_t *pixels=calloc(1000*720,sizeof(uint32_t)); a->draw.surf=(os64_gui_surface_t){.pixels=pixels,.width=1000,.height=720,.pitch_px=1000};
    CHECK(details_setup(a));
    /* The production dispatcher owns menu precedence for each delivery. */
    os64_ui_window_backend_t transport={.poll=delivery_poll};
    os64_ui_dispatcher_init(&a->dispatcher,&transport);
    CHECK(os64_ui_dispatcher_add(&a->dispatcher,1,&a->ui,details_commands,gallery_local_paint,a)==OS64_UI_OK);
    a->dispatcher.entries[0].boundary=details_boundary; a->dispatcher.entries[0].boundary_user=a;
    CHECK(strstr(a->diagnostics.base.text,"clock not checked yet")!=NULL);
    CHECK(os64_ui_dataview_clock(&a->views[0],fixture_clock,NULL)==OS64_UI_OK);
    test_time=-1;
    delivery.event=(os64_gui_event_t){.type=OS64_GUI_EVENT_KEY_DOWN};
    delivery.event.key.ascii='z'; delivery.event.key.scancode=0x2c;
    CHECK(os64_ui_dispatcher_turn(&a->dispatcher,false)==1);
    CHECK(os64_ui_selection_count(&a->selections[0])==0);
    CHECK(strstr(a->diagnostics.base.text,"clock unavailable")!=NULL && !a->report_pending);
    test_time=1000000; delivery.delivered=false;
    CHECK(os64_ui_dispatcher_turn(&a->dispatcher,false)==1);
    CHECK(os64_ui_selection_count(&a->selections[0])==0);
    CHECK(strstr(a->diagnostics.base.text,"clock")==NULL && !a->report_pending);
    CHECK(os64_ui_dataview_clock(&a->views[0],NULL,NULL)==OS64_UI_OK);
    details_report(a);
    CHECK(strstr(a->diagnostics.base.text,"clock not checked yet")!=NULL);
    delivery.delivered=false; delivery.calls=0;
    a->ui.menu_event=delivery_menu;
    delivery.event=(os64_gui_event_t){.type=OS64_GUI_EVENT_KEY_DOWN};
    delivery.event.key.ascii='b'; delivery.event.key.scancode=0x30;
    CHECK(os64_ui_dispatcher_turn(&a->dispatcher,false)==1 && delivery.calls==1);
    uint64_t focus=os64_ui_selection_focus(&a->selections[0]);
    delivery.delivered=false; delivery.consume=true; delivery.calls=0;
    delivery.event.key.ascii=1; delivery.event.key.scancode=0x1e; delivery.event.key.modifiers=OS64_GUI_MOD_CTRL;
    CHECK(os64_ui_dispatcher_turn(&a->dispatcher,false)==1 && delivery.calls==1);
    CHECK(os64_ui_selection_focus(&a->selections[0])==focus && os64_ui_selection_count(&a->selections[0])==1);
    a->ui.menu_event=NULL;
#ifdef UI_TEXT_REAL
    /* An E3 font refusal must not install notice runs from the candidate. */
    os64_font_set_t *candidate=outline_set(os64_ui_font_context(&a->ui),dir,"DejaVuSans.ttf",24);
    os64_font_consumer_t consumer; os64_ui_font_consumer(&a->ui,&consumer);
    void *old_notice=a->status.notice.run;
    int32_t old_line=os64_ui_font_row_height(&a->ui,OS64_FONT_ROLE_UI);
    bool observed=false;
    for (long fail=0;fail<1200 && !observed;++fail) {
        a->error_pending=false; a->report_pending=false;
        deny_countdown=fail;
        os64_font_status_t rc=os64_font_adopt(candidate,&consumer,1,NULL);
        deny_countdown=-1;
        if (a->error_pending) {
            observed=true; CHECK(rc!=OS64_FONT_OK);
            CHECK(a->status.notice.run==old_notice);
            CHECK(os64_ui_font_row_height(&a->ui,OS64_FONT_ROLE_UI)==old_line);
            delivery.delivered=false; delivery.consume=false;
            delivery.event=(os64_gui_event_t){.type=OS64_GUI_EVENT_INPUT_LOST};
            CHECK(os64_ui_dispatcher_turn(&a->dispatcher,false)==1 && !a->error_pending);
            CHECK(a->status.notice.run && os64_ui_font_row_height(&a->ui,OS64_FONT_ROLE_UI)==old_line);
        } else CHECK(rc!=OS64_FONT_OK);
    }
    CHECK(observed);
    CHECK(os64_font_adopt(candidate,&consumer,1,NULL)==OS64_FONT_OK);
    os64_font_set_release(candidate);
#else
    (void)dir;
#endif
    CHECK(details_invoke(a,NULL,D_SECOND).status==OS64_UI_OK);
    CHECK(a->second && os64_ui_selection_projection(&a->selections[0])==os64_ui_selection_projection(&a->selections[1]));
    CHECK(details_invoke(a,NULL,D_ONE).status==OS64_UI_OK);
    CHECK(os64_ui_data_projection_count(os64_ui_selection_projection(&a->selections[1]))==10000);
    CHECK(details_invoke(a,NULL,D_FILTER).status==OS64_UI_OK);
    CHECK(details_invoke(a,NULL,D_FILTER).status==OS64_UI_OK);
    CHECK(details_invoke(a,NULL,D_EMPTY).status==OS64_UI_OK);
    CHECK(details_invoke(a,NULL,D_TENK).status==OS64_UI_OK);
    deny_countdown=0; CHECK(details_invoke(a,NULL,D_MAX).status==OS64_UI_NO_MEMORY); deny_countdown=-1;
    CHECK(os64_ui_data_count(a->data[0])==10000);
    CHECK(details_invoke(a,NULL,D_MAX).status==OS64_UI_OK);
    CHECK(os64_ui_data_count(a->data[0])==100000);
    CHECK(details_invoke(a,NULL,D_SECOND).status==OS64_UI_OK && !a->second);
    CHECK(details_cleanup(a)); free(pixels); free(a);
}
int main(int argc,char **argv)
{
    const char *dir=argc>1?argv[1]:"userland/libfreetype/fixtures";
    core_suite(); widget_input(dir); interruptions(dir); allocation_sweep(dir); bounds_and_independence(dir); allocation_capacity(dir); repeated_lifetime(dir); performance(dir); gallery_behavior(dir);
    printf("Details host: %lu checks, %d failures\n",checks,failures); return failures?1:0;
}
