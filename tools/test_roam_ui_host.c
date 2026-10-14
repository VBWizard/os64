#define UI_MENU_TEST
#include "test_ui_action_host.c"
#define app_retain roam_app_retain
#define app_release roam_app_release
#define app_valid roam_app_valid
#define target_valid roam_target_valid
#define resize roam_resize
#define plan roam_plan
#define main roam_unused_main
#include "../userland/apps/roam/roam.c"
#undef main
#undef plan
#undef resize
#undef target_valid
#undef app_valid
#undef app_release
#undef app_retain
#include <malloc.h>
#include <time.h>

size_t os64_malloc_size(const void *p) { return p?malloc_usable_size((void *)p):0; }
int64_t os64_micros(void)
{ struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec*INT64_C(1000000)+t.tv_nsec/1000; }

struct os64_directory {
    uint64_t domain;
    size_t count;
    bool incomplete;
    os64_directory_entry_t entries[2];
};
struct os64_browser {
    char location[OS64_DIRECTORY_PATH], requested[OS64_DIRECTORY_PATH];
    char filter[OS64_DIRECTORY_FILTER];
    bool hidden, loading;
    uint64_t revision;
    struct os64_directory directory;
    os64_directory_places_t places;
    unsigned activates, polls, backs, forwards;
};
static struct os64_browser fake;
static os64_ui_status_code_t filter_error;

os64_ui_status_code_t os64_browser_create(os64_ui_dataview_t *v,
    const os64_ui_icon_provider_t *icons,int64_t window,uint32_t bit,os64_browser_t **out)
{ (void)v;(void)icons;(void)window;(void)bit;*out=&fake;return OS64_UI_OK; }
void os64_browser_destroy(os64_browser_t *b) { (void)b; }
os64_ui_status_code_t os64_browser_navigate(os64_browser_t *b,const char *path)
{ snprintf(b->requested,sizeof(b->requested),"%s",path);b->loading=true;++b->revision;return OS64_UI_OK; }
os64_ui_status_code_t os64_browser_back(os64_browser_t *b)
{ ++b->backs;return OS64_UI_OK; }
os64_ui_status_code_t os64_browser_forward(os64_browser_t *b)
{ ++b->forwards;return OS64_UI_OK; }
os64_ui_status_code_t os64_browser_up(os64_browser_t *b)
{ return os64_browser_navigate(b,"/"); }
os64_ui_status_code_t os64_browser_refresh(os64_browser_t *b)
{ return os64_browser_navigate(b,b->location); }
void os64_browser_cancel(os64_browser_t *b) { b->loading=false; }
os64_ui_status_code_t os64_browser_poll(os64_browser_t *b)
{ ++b->polls;return OS64_UI_OK; }
os64_ui_status_code_t os64_browser_filter(os64_browser_t *b,const char *text,bool hidden)
{ if(filter_error) return filter_error;snprintf(b->filter,sizeof(b->filter),"%s",text);b->hidden=hidden;++b->revision;return OS64_UI_OK; }
os64_ui_status_code_t os64_browser_activate(os64_browser_t *b,uint64_t domain,os64_ui_data_id_t id)
{ (void)domain;(void)id;++b->activates;return os64_browser_navigate(b,"/home/folder"); }
const char *os64_browser_location(const os64_browser_t *b) { return b->location; }
const char *os64_browser_requested(const os64_browser_t *b) { return b->requested; }
const char *os64_browser_filter_text(const os64_browser_t *b) { return b->filter; }
bool os64_browser_hidden(const os64_browser_t *b) { return b->hidden; }
bool os64_browser_loading(const os64_browser_t *b) { return b->loading; }
bool os64_browser_can_back(const os64_browser_t *b) { return b->backs!=0; }
bool os64_browser_can_forward(const os64_browser_t *b) { return b->forwards!=0; }
const os64_directory_t *os64_browser_directory(const os64_browser_t *b)
{ return &b->directory; }
const os64_directory_places_t *os64_browser_places(const os64_browser_t *b)
{ return &b->places; }
os64_ui_status_code_t os64_browser_status(const os64_browser_t *b,int64_t *error)
{ (void)b;*error=0;return OS64_UI_OK; }
uint64_t os64_browser_revision(const os64_browser_t *b) { return b->revision; }
uint64_t os64_directory_domain(const os64_directory_t *d) { return d->domain; }
size_t os64_directory_count(const os64_directory_t *d) { return d->count; }
bool os64_directory_incomplete(const os64_directory_t *d) { return d->incomplete; }
const os64_directory_entry_t *os64_directory_entry(const os64_directory_t *d,os64_ui_data_id_t id)
{ for(size_t i=0;i<d->count;++i) if(d->entries[i].id==id) return &d->entries[i];return NULL; }
os64_ui_status_code_t os64_directory_path(const char *base,const char *input,
    char out[OS64_DIRECTORY_PATH])
{ (void)base;snprintf(out,OS64_DIRECTORY_PATH,"%s",input);return OS64_UI_OK; }
os64_ui_status_code_t os64_directory_home(char out[OS64_DIRECTORY_PATH])
{ snprintf(out,OS64_DIRECTORY_PATH,"/home");return OS64_UI_OK; }
const char *os64_getenv(const char *key)
{ return !strcmp(key,"HOME")?"/home":NULL; }

static void breadcrumb_failure_tests(void)
{
    os64_ui_browser_t *p=&app.browser_ui;
    size_t refused=0;
    bool accepted=false;
    for (long nth=0;nth<1024;++nth) {
        size_t old_count=p->crumb_count;
        char old_location[OS64_DIRECTORY_PATH];
        char names[OS64_UI_BROWSER_CRUMBS][OS64_DIRECTORY_PATH];
        char paths[OS64_UI_BROWSER_CRUMBS][OS64_DIRECTORY_PATH];
        os64_gui_rect_t bounds[OS64_UI_BROWSER_CRUMBS];
        bool hidden[OS64_UI_BROWSER_CRUMBS];
        strcpy(old_location,p->seen_location);
        memcpy(names,p->crumb_names,sizeof(names));
        memcpy(paths,p->crumb_paths,sizeof(paths));
        for (size_t i=0;i<OS64_UI_BROWSER_CRUMBS;++i) {
            bounds[i]=p->crumbs[i].bounds;hidden[i]=p->crumbs[i].hidden;
        }
        os64_gui_rect_t overflow=p->overflow.bounds;
        snprintf(fake.location,sizeof(fake.location),"/allocation-%ld/child-%ld",nth,nth);
        ++fake.revision;
        deny_countdown=nth;
        os64_ui_browser_sync(p);
        deny_countdown=-1;
        if (strcmp(p->seen_location,fake.location)) {
            ++refused;
            CHECK(!strcmp(p->seen_location,old_location));
            CHECK(p->crumb_count==old_count);
            for (size_t i=0;i<OS64_UI_BROWSER_CRUMBS;++i) {
                CHECK(!memcmp(&bounds[i],&p->crumbs[i].bounds,sizeof(bounds[i])));
                CHECK(hidden[i]==p->crumbs[i].hidden);
                if (i<old_count) {
                    CHECK(!strcmp(names[i],p->crumbs[i].text));
                    CHECK(!strcmp(paths[i],p->crumb_paths[i]));
                }
            }
            CHECK(!memcmp(&overflow,&p->overflow.bounds,sizeof(overflow)));
            /* The same location/revision must retry without a resize. */
            os64_ui_browser_sync(p);
        } else accepted=true;
        CHECK(!strcmp(p->seen_location,fake.location));
        CHECK(p->crumb_count==3);
        CHECK(p->crumbs[2].bounds.w>0 && !p->crumbs[2].hidden);
        p->crumbs[2].on_click(&p->crumbs[2],p);
        CHECK(!strcmp(fake.requested,fake.location));
        if (accepted) break;
    }
    CHECK(refused>0 && accepted);
    printf("breadcrumb allocation sweep: %zu refusals recovered\n",refused);
    strcpy(fake.location,"/home");fake.requested[0]=0;fake.loading=false;
    ++fake.revision;os64_ui_browser_sync(p);
}

static void message_filter_failure_tests(void)
{
    os64_ui_browser_t *p=&app.browser_ui;
    bool replaced=false;size_t refusals=0;
    for (long nth=0;nth<256;++nth) {
        os64_ui_browser_message(p,"Existing help");
        uint64_t old=p->help_token;
        CHECK(old!=0);
        deny_countdown=nth;
        os64_ui_browser_message(p,"Replacement help");
        deny_countdown=-1;
        if (!strcmp(p->status.help.text,"Existing help")) {
            ++refusals;CHECK(p->help_token==old);
        } else {replaced=true;CHECK(p->help_token!=old && p->help_token!=0);}
        ++fake.revision;os64_ui_browser_sync(p);
        CHECK(p->help_token==0 && p->status.help.token==0 && !p->status.help.text);
        if(replaced) break;
    }
    CHECK(replaced && refusals>1);
    CHECK(os64_browser_filter(&fake,"accepted",false)==OS64_UI_OK);
    os64_ui_set_focus(&app.ui,&p->details.w);os64_ui_browser_sync(p);
    os64_ui_set_focus(&app.ui,&p->filter.w);
    uint64_t revision=fake.revision;
    filter_error=OS64_UI_NO_MEMORY;
    os64_ui_textfield_set(&app.ui,&p->filter,"rejected");
    p->filter.on_change(&p->filter,p);
    CHECK(!strcmp(p->filter.buf,"accepted") && !strcmp(fake.filter,"accepted"));
    CHECK(fake.revision==revision && app.ui.focus==&p->filter.w);
    p->filter.on_cancel(&p->filter,p);
    CHECK(!strcmp(p->filter.buf,"accepted") && !strcmp(fake.filter,"accepted"));
    CHECK(fake.revision==revision);
    filter_error=OS64_UI_OK;
    p->filter.on_cancel(&p->filter,p);
    CHECK(!*p->filter.buf && !*fake.filter);
    ++fake.revision;os64_ui_browser_sync(p);
}

static void details_layout_failure_tests(void)
{
    os64_ui_browser_t *p=&app.browser_ui;
    for (int mode=0;mode<2;++mode) {
        size_t refusals=0,blocked=0;bool succeeded=false;
        for (long nth=0;nth<1024;++nth) {
            os64_gui_rect_t old_details=p->details.w.bounds;
            os64_gui_rect_t old_crumb=p->crumbs[1].bounds;
            os64_gui_rect_t old_toolbar=p->toolbar.bounds;
            if (!mode) app.root.bounds.w=app.root.bounds.w==880?840:880;
            deny_countdown=nth;
            os64_font_status_t rc=OS64_FONT_OK;
            if (!mode) rc=layout(&app,false);
            else os64_ui_browser_show(p,OS64_UI_BROWSER_STATUS,
                !os64_ui_browser_visible(p,OS64_UI_BROWSER_STATUS));
            deny_countdown=-1;
            if (p->layout_pending) {
                ++refusals;
                if (!mode) CHECK(rc!=OS64_FONT_OK);
                if (os64_ui_dataview_blocked(&p->details)) ++blocked;
                CHECK(!memcmp(&old_details,&p->details.w.bounds,sizeof(old_details)));
                CHECK(!memcmp(&old_crumb,&p->crumbs[1].bounds,sizeof(old_crumb)));
                CHECK(!memcmp(&old_toolbar,&p->toolbar.bounds,sizeof(old_toolbar)));
                os64_ui_browser_sync(p);
            } else succeeded=true;
            CHECK(!p->layout_pending && !os64_ui_dataview_blocked(&p->details));
            CHECK(memcmp(&old_details,&p->details.w.bounds,sizeof(old_details))!=0);
            if (succeeded) break;
        }
        CHECK(succeeded && refusals>0 && blocked>0);
        printf("%s layout: %zu refusals, %zu Details failures recovered\n",
            mode?"pane":"resize",refusals,blocked);
    }
    app.root.bounds.w=880;
    os64_ui_browser_show(p,OS64_UI_BROWSER_STATUS,true);
    CHECK(layout(&app,false)==OS64_FONT_OK);
}

static void roam_ui_tests(const char *font_dir)
{
    current="roam UI";
    memset(&app,0,sizeof(app));memset(&fake,0,sizeof(fake));
    snprintf(fake.location,sizeof(fake.location),"/home");
    fake.revision=1;fake.directory.domain=8;fake.directory.count=2;
    fake.directory.entries[0].id=7;
    strcpy(fake.directory.entries[0].entry.name,"file.txt");
    fake.directory.entries[1].id=8;
    fake.directory.entries[1].entry.flags=OS64_DE_DIR;
    strcpy(fake.directory.entries[1].entry.name,"folder");
    fake.places.count=2;fake.places.favorite_count=1;
    strcpy(fake.places.items[0].label,"Root");
    strcpy(fake.places.items[0].path,"/");
    strcpy(fake.places.items[1].label,"Volume");
    strcpy(fake.places.items[1].path,"/mnt/volume");
    uint32_t *pixels=calloc(880u*630u,sizeof(*pixels));CHECK(pixels!=NULL);
    app.draw.surf=(os64_gui_surface_t){pixels,880,630,880};
    os64_ui_init(&app.ui,&app.draw);
    os64_ui_panel(&app.root);app.root.bounds=(os64_gui_rect_t){0,0,880,630};
    app.root.user=&app;os64_ui_set_root(&app.ui,&app.root);
    os64_ui_menubar_init(&app.bar);os64_ui_add_child(&app.root,&app.bar.w);
    os64_ui_panel(&app.browser_area);os64_ui_add_child(&app.root,&app.browser_area);
    os64_ui_browser_init(&app.browser_ui,&app.ui,&app.browser_area);
    app.browser=&fake;app.menu_visible=true;
    CHECK(os64_ui_font_planner(&app.ui,roam_plan,plan_done,plan_done,&app)==OS64_FONT_OK);
    CHECK(layout(&app,false)==OS64_FONT_OK);
    os64_ui_browser_bind(&app.browser_ui,&fake);
    CHECK(app.browser_ui.favorites.count==1 && app.browser_ui.mounts.count==1);
    CHECK(app.browser_ui.crumb_count==2);
    CHECK(app.browser_ui.crumbs[1].bounds.w>0);
    app.ui.any_dirty=false;
    os64_ui_browser_sync(&app.browser_ui);
    CHECK(!app.ui.any_dirty);
    strcpy(fake.places.items[1].label,"Replacement volume");
    ++fake.revision;
    os64_ui_browser_sync(&app.browser_ui);
    CHECK(app.ui.any_dirty && app.ui.dirty.x<=app.browser_ui.sidebar.bounds.x &&
        app.ui.dirty.y<=app.browser_ui.sidebar.bounds.y &&
        app.ui.dirty.x+app.ui.dirty.w>=app.browser_ui.sidebar.bounds.x+app.browser_ui.sidebar.bounds.w &&
        app.ui.dirty.y+app.ui.dirty.h>=app.browser_ui.sidebar.bounds.y+app.browser_ui.sidebar.bounds.h);

    CHECK(create_actions(&app) && create_menus(&app));
    os64_ui_action_state_t state;
    CHECK(os64_ui_actions_refresh(app.action_view)==OS64_UI_OK);
    CHECK(os64_ui_action_snapshot(app.action_view,A_COLUMN_1+1,&state)==OS64_UI_OK && !state.enabled && !state.checked);
    os64_ui_data_column_t columns[]={
        {1,{"Name",4},OS64_UI_DATA_TEXT_KEY,200,false,true,true,true},
        {2,{"Size",4},OS64_UI_DATA_UNSIGNED,90,true,true,false,false},
        {3,{"Type",4},OS64_UI_DATA_TEXT_KEY,120,false,true,false,false},
        {4,{"Modified",8},OS64_UI_DATA_UNSIGNED,150,false,true,false,false}
    };
    os64_ui_data_t *data=NULL;
    os64_ui_data_projection_t *projection=NULL;
    os64_ui_selection_t selection={0};
    CHECK(os64_ui_data_create(8,columns,4,NULL,0,&data)==OS64_UI_OK);
    CHECK(os64_ui_data_projection_identity(data,&projection)==OS64_UI_OK);
    os64_ui_selection_init(&selection);
    CHECK(os64_ui_selection_bind(&selection,projection,OS64_UI_DATA_RESET)==OS64_UI_OK);
    os64_ui_data_projection_release(projection);
    CHECK(os64_ui_dataview_attach(&app.browser_ui.details,&selection,NULL)==OS64_UI_OK);
    CHECK(os64_ui_dataview_column_set(&app.browser_ui.details,2,90,true,0)==OS64_UI_OK);
    CHECK(os64_ui_dataview_column_set(&app.browser_ui.details,4,150,true,1)==OS64_UI_OK);
    app.context_column=4;
    CHECK(os64_ui_actions_refresh(app.action_view)==OS64_UI_OK);
    CHECK(os64_ui_action_snapshot(app.action_view,A_COLUMN_RIGHT,&state)==OS64_UI_OK && state.enabled);
    CHECK(os64_ui_action_invoke(app.action_view,A_COLUMN_RIGHT).status==OS64_UI_OK);
    os64_ui_dataview_column_t moved;
    CHECK(os64_ui_dataview_column(&app.browser_ui.details,2,&moved) && moved.id==4);
    CHECK(ui_test_adopt(&app.ui,font_dir,32)==OS64_FONT_OK);
    int32_t text_width=0;
    CHECK(os64_ui_text_measure(&app.ui,OS64_FONT_ROLE_UI,"Forward",7,&text_width)==OS64_FONT_OK);
    CHECK(app.browser_ui.forward.bounds.w>=text_width);
    breadcrumb_failure_tests();
    message_filter_failure_tests();
    details_layout_failure_tests();
    fake.directory.incomplete=true;
    os64_ui_browser_sync(&app.browser_ui);
    uint64_t notice=app.browser_ui.notice_token;
    CHECK(notice!=0);
    os64_ui_browser_sync(&app.browser_ui);
    CHECK(app.browser_ui.notice_token==notice);
    fake.directory.incomplete=false;
    os64_ui_browser_sync(&app.browser_ui);
    CHECK(app.browser_ui.notice_token==0);
    os64_ui_browser_show(&app.browser_ui,OS64_UI_BROWSER_ADDRESS,false);
    os64_ui_browser_focus_address(&app.browser_ui);
    CHECK(os64_ui_browser_visible(&app.browser_ui,OS64_UI_BROWSER_ADDRESS));
    os64_ui_textfield_set(&app.ui,&app.browser_ui.address,"/home");
    os64_ui_set_focus(&app.ui,&app.browser_ui.details.w);
    os64_gui_event_t location={.type=OS64_GUI_EVENT_KEY_DOWN};
    location.key.ascii=12;location.key.scancode=0x26;
    location.key.modifiers=OS64_GUI_MOD_CTRL;
    commands(&app,&location);
    CHECK(app.ui.focus==&app.browser_ui.address.w);
    location.type=OS64_GUI_EVENT_KEY_UP;commands(&app,&location);
    os64_ui_set_focus(&app.ui,&app.browser_ui.details.w);
    location.type=OS64_GUI_EVENT_KEY_DOWN;commands(&app,&location);
    CHECK(app.ui.focus==&app.browser_ui.address.w);
    location.type=OS64_GUI_EVENT_KEY_UP;commands(&app,&location);
    os64_gui_event_t select={.type=OS64_GUI_EVENT_KEY_DOWN};
    select.key.ascii=1;select.key.modifiers=OS64_GUI_MOD_CTRL;
    commands(&app,&select);
    CHECK(app.browser_ui.address.selected);
    select.type=OS64_GUI_EVENT_KEY_UP;commands(&app,&select);
    os64_ui_textfield_set(&app.ui,&app.browser_ui.address,"/does-not-exist");
    os64_gui_event_t enter={.type=OS64_GUI_EVENT_KEY_DOWN};
    enter.key.ascii='\n';enter.key.scancode=0x1c;
    commands(&app,&enter);
    CHECK(app.ui.focus==&app.browser_ui.details.w);
    CHECK(!strcmp(fake.requested,"/does-not-exist"));
    fake.loading=false;fake.requested[0]=0;++fake.revision;
    os64_ui_browser_sync(&app.browser_ui);
    CHECK(!strcmp(app.browser_ui.address.buf,"/home"));
    location.type=OS64_GUI_EVENT_KEY_DOWN;commands(&app,&location);
    CHECK(app.ui.focus==&app.browser_ui.address.w);
    location.type=OS64_GUI_EVENT_KEY_UP;commands(&app,&location);
    os64_ui_set_focus(&app.ui,&app.browser_ui.details.w);
    os64_ui_dataview_change_t change={.domain=8,.activated=7};
    changed(&app.browser_ui.details,&change,&app);
    prepare_turn(&app);
    CHECK(fake.activates==0 && app.pending_id==0);
    CHECK(app.browser_ui.help_token!=0);
    change.activated=8;changed(&app.browser_ui.details,&change,&app);
    prepare_turn(&app);
    CHECK(fake.activates==1 && fake.polls>=2);
    CHECK(app.browser_ui.help_token==0);
    CHECK(os64_ui_action_invoke(app.action_view,A_SHOW_MENU).status==OS64_UI_OK);
    CHECK(!app.menu_visible && app.bar.w.hidden);
    CHECK(os64_ui_action_invoke(app.action_view,A_SHOW_MENU).status==OS64_UI_OK);
    CHECK(app.menu_visible && !app.bar.w.hidden);
    os64_font_set_t *candidate=outline_set(os64_ui_font_context(&app.ui),font_dir,
        "DejaVuSans.ttf",28);
    CHECK(candidate!=NULL);
    if(candidate) {
        os64_font_consumer_t consumer;
        os64_ui_font_consumer(&app.ui,&consumer);
        os64_gui_rect_t before=app.browser_ui.crumbs[0].bounds;
        deny_countdown=0;
        CHECK(os64_font_adopt(candidate,&consumer,1,NULL)!=OS64_FONT_OK);
        deny_countdown=-1;
        CHECK(!memcmp(&before,&app.browser_ui.crumbs[0].bounds,sizeof(before)));
        os64_font_set_release(candidate);
    }
    app.root.bounds=(os64_gui_rect_t){0,0,260,300};
    CHECK(layout(&app,false)==OS64_FONT_OK);
    snprintf(fake.location,sizeof(fake.location),"/alpha/beta/gamma/delta/epsilon");
    ++fake.revision;os64_ui_browser_sync(&app.browser_ui);
    CHECK(app.browser_ui.overflow.bounds.w>0);
    CHECK(app.browser_ui.crumbs[app.browser_ui.crumb_count-1].bounds.w>0);
    app.browser_ui.overflow.on_click(&app.browser_ui.overflow,&app.browser_ui);
    CHECK(app.ui.focus==&app.browser_ui.address.w);
    app.root.bounds=(os64_gui_rect_t){0,0,100,70};
    CHECK(layout(&app,false)==OS64_FONT_OK);
    CHECK(app.browser_ui.details.w.bounds.w>=0 && app.browser_ui.details.w.bounds.h>=0);
    CHECK(app.browser_ui.crumbs[0].bounds.w<=app.root.bounds.w);
    CHECK(app.browser_ui.back.bounds.h==0 && app.browser_ui.address.w.bounds.h==0);
    os64_ui_menubar_destroy(&app.bar);
    for(size_t i=0;i<M_COUNT;++i) os64_ui_menu_model_release(app.models[i]);
    CHECK(os64_ui_action_view_destroy(app.action_view)==OS64_UI_OK);
    CHECK(os64_ui_actions_destroy(app.actions)==OS64_UI_OK);
    CHECK(os64_ui_dataview_dispose(&app.browser_ui.details)==OS64_UI_OK);
    CHECK(os64_ui_selection_dispose(&selection)==OS64_UI_OK);
    os64_ui_data_release(data);
    CHECK(os64_ui_font_release(&app.ui)==OS64_FONT_OK);
    free(pixels);
}
int main(int argc,char **argv)
{
    if(getenv("ROAM_RUN_ACTION_SUITE")) (void)action_suite(argc,argv);
    const char *dir=argc>1?argv[1]:"userland/libfreetype/fixtures";
    roam_ui_tests(dir);
    printf("roam UI: %lu checks, %d failures\n",checks,failures);
    return failures?1:0;
}
