/* Real dialog, directory, browser, data-view and font code. Filesystem and
 * worker completion are controlled so races can be exercised deterministically. */
#define main file_unused_text_suite
#include "test_ui_text_host.c"
#undef main
#include "../userland/libos64/ui_file_dialog.c"
#include "os64/work.h"
#include "os64/date.h"
#include <malloc.h>
#include <time.h>

static os64_gui_surface_t native_surface;
static bool native_owned, destroy_refused;
static unsigned minimum_calls, size_writes;
static char saved_size[64];
static bool fail_size_write, fail_screen_info;
static uint32_t native_flags, screen_width = 1600, screen_height = 1000;
static const char *startup_fonts;
static uint32_t startup_font_size=32;
static void native_resize(uint32_t width, uint32_t height)
{
    free(native_surface.pixels);
    native_surface=(os64_gui_surface_t){calloc((size_t)width*height,4),width,height,width};
    CHECK(native_surface.pixels != NULL);
}
int64_t os64_conf_get(const char *name,const char *key,char *out,size_t cap)
{
    CHECK(!strcmp(name,"file-dialog.conf") && !strcmp(key,"size"));
    if (!*saved_size) { *out=0;return OS64_CONF_NO_FILE; }
    if (strlen(saved_size)>=cap) { *out=0;return OS64_CONF_TRUNCATED; }
    snprintf(out,cap,"%s",saved_size);return 0;
}
int64_t os64_conf_write_checked(const char *name,const os64_conf_pair_t *pairs,size_t count,
    bool (*validate)(const char *,size_t,void *),void *user)
{
    (void)validate;(void)user;
    CHECK(!strcmp(name,"file-dialog.conf") && count==1 && !strcmp(pairs[0].key,"size"));
    ++size_writes;
    if(fail_size_write)return OS64_CONF_IO_ERROR;
    snprintf(saved_size,sizeof(saved_size),"%s",pairs[0].value);return 0;
}
uint64_t file_test_syscall(uint64_t nr,uint64_t a,uint64_t b,uint64_t c,
    uint64_t e,uint64_t f,uint64_t g)
{
    (void)a;(void)c;(void)g;
    switch(nr){
    case SYSCALL_GUI_SCREEN_INFO:
        if(fail_screen_info)return (uint64_t)OS64_GUI_ERR_NOT_RUNNING;
        *(uint32_t *)(uintptr_t)a=screen_width;
        *(uint32_t *)(uintptr_t)b=screen_height;return 0;
    case SYSCALL_GUI_WINDOW_CREATE: {
        /* Match the native guard: it rejects before applying FIT_SCREEN. */
        uint32_t limit=OS64_GUI_WINDOW_DIM_MAX;
        if(screen_width>limit)limit=screen_width;
        if(screen_height>limit)limit=screen_height;
        if(e>limit || f>limit)return (uint64_t)OS64_GUI_ERR_BAD_ARGS;
        CHECK(!native_owned);
        CHECK(g & OS64_GUI_CREATE_FIT_SCREEN);
        if(e>screen_width)e=screen_width;
        if(f>screen_height)f=screen_height;
        native_resize(e,f);native_flags=0;
        native_owned=true;return 91;
    }
    case SYSCALL_GUI_WINDOW_GET_SURFACE:
        if(!native_surface.pixels)return (uint64_t)-1;
        *(os64_gui_surface_t *)(uintptr_t)b=native_surface;return 0;
    case SYSCALL_GUI_WINDOW_DESTROY:
        if(destroy_refused)return (uint64_t)OS64_GUI_ERR_BUSY;
        if(native_owned)free(native_surface.pixels);
        native_surface=(os64_gui_surface_t){0};native_owned=false;return 0;
    case SYSCALL_GUI_WINDOW_GET_STATE:
        *(os64_gui_window_state_t *)(uintptr_t)b=(os64_gui_window_state_t){.flags=native_flags};return 0;
    case SYSCALL_GUI_WINDOW_SET_MIN_SIZE:
        ++minimum_calls;
        if(b>screen_width || c>screen_height)return (uint64_t)OS64_GUI_ERR_BAD_ARGS;
        if(b>native_surface.width || c>native_surface.height)
            native_resize(b>native_surface.width?b:native_surface.width,
                c>native_surface.height?c:native_surface.height);
        return 0;
    case SYSCALL_GUI_WINDOW_FOCUS:
    case SYSCALL_GUI_WINDOW_PUBLISH:
    case SYSCALL_GUI_EVENT_POLL: return 0;
    default: return (uint64_t)-1;
    }
}

int os64_ui_font_follow(os64_ui_t *ui)
{ return startup_fonts ? ui_test_adopt(ui,startup_fonts,startup_font_size) : OS64_FONT_OK; }

void *os64_calloc(size_t n, size_t size)
{ void *p=os64_malloc(n*size); if(p)memset(p,0,n*size); return p; }
size_t os64_malloc_size(const void *p) { return p?malloc_usable_size((void *)p):0; }
int64_t os64_micros(void)
{ struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec*INT64_C(1000000)+t.tv_nsec/1000; }
void os64_ui_theme_current(os64_ui_theme_t *t, uint64_t *generation)
{
    *t=(os64_ui_theme_t){.pad=6,.gap=6,.button_h=28,.font_w=8,.font_h=16,.checkbox_size=16,
        .panel_bg=0xff202020,.panel_border=0xff888888,.label_fg=0xffeeeeee,
        .button_face=0xff454545,.button_fg=0xffeeeeee,.button_face_hover=0xff606060,
        .field_bg=0xff303030,.field_fg=0xffeeeeee,.field_border=0xff777777,
        .field_border_focus=0xff5599ee,.focus_ring=0xff5599ee,.text_fg=0xffeeeeee,
        .text_bg=0xff303030,.text_sel_bg=0xff3264c8,.text_sel_fg=0xffffffff,
        .menu_bg=0xff202020,.menu_fg=0xffeeeeee,.menu_hi_bg=0xff3264c8,
        .menu_hi_fg=0xffffffff,.menu_sep=0xff888888,.disabled_fg=0xff888888};
    *generation=0;
}
bool os64_ui_theme_session(os64_ui_theme_t *t,uint64_t *g,uint64_t hint)
{ (void)t;(void)g;(void)hint;return false; }
const char *os64_getenv(const char *key) { return !strcmp(key,"HOME")?"/home":NULL; }
int64_t os64_open(const char *path,const char *mode)
{ (void)path;CHECK(!mode || !strcmp(mode,"r"));return -1; }
int64_t os64_read(int32_t h,void *p,size_t n) { (void)h;(void)p;(void)n;return -1; }
int64_t os64_write(int32_t h,const void *p,size_t n) { (void)h;(void)p;return n; }
int64_t os64_close(int32_t h) { (void)h;return 0; }
int64_t os64_localtime(int64_t epoch,os64_date_t *out)
{ (void)epoch;memset(out,0,sizeof(*out));out->year=2026;out->month=10;out->day=19;return 0; }
size_t os64_strftime(char *out,size_t cap,const char *format,const os64_date_t *date)
{ (void)format;(void)date;return snprintf(out,cap,"2026-10-19 12:00"); }

typedef struct { char path[256]; bool dir; } fs_entry;
static fs_entry files[32];
static size_t file_count, dir_cursor;
static char open_dir[256];
static bool fail_stat, fail_mkdir;
static unsigned mkdir_calls;
static void add_file(const char *path,bool dir)
{ snprintf(files[file_count].path,256,"%s",path);files[file_count++].dir=dir; }
int64_t os64_stat(const char *path,os64_dirent_t *out)
{
    if(fail_stat)return -1;
    for(size_t i=0;i<file_count;++i)if(!strcmp(files[i].path,path)){
        memset(out,0,sizeof(*out));out->flags=files[i].dir?OS64_DE_DIR:0;
        const char *leaf=strrchr(path,'/');snprintf(out->name,sizeof(out->name),"%s",leaf?leaf+1:path);
        return 0;
    }
    return -1;
}
int64_t os64_opendir(const char *path)
{
    os64_dirent_t e;
    if(os64_stat(path,&e) || !(e.flags&OS64_DE_DIR))return -1;
    snprintf(open_dir,sizeof(open_dir),"%s",path);dir_cursor=0;return 10;
}
int64_t os64_readdir(int32_t h,os64_dirent_t *out)
{
    CHECK(h==10);
    size_t n=strlen(open_dir);
    while(dir_cursor<file_count){
        fs_entry *e=&files[dir_cursor++];
        if(strncmp(e->path,open_dir,n))continue;
        const char *leaf=e->path+n;
        if(n>1){if(*leaf!='/')continue;++leaf;}
        if(!*leaf || strchr(leaf,'/'))continue;
        memset(out,0,sizeof(*out));out->flags=e->dir?OS64_DE_DIR:0;
        snprintf(out->name,sizeof(out->name),"%s",leaf);return 1;
    }
    return 0;
}
int64_t os64_mkdir(const char *path)
{
    ++mkdir_calls;
    os64_dirent_t e;
    if(fail_mkdir || !os64_stat(path,&e))return -1;
    add_file(path,true);return 0;
}

struct os64_work_pool {
    uint64_t next;
    struct { os64_work_id_t id; os64_work_t work; int64_t rc; void *product; } slots[8];
};
static bool not_cancelled(void *ctx) { (void)ctx;return false; }
os64_work_pool_t *os64_work_pool_create(int32_t n,size_t cap,int64_t win,uint32_t bit)
{ (void)n;(void)cap;(void)win;(void)bit;return os64_calloc(1,sizeof(os64_work_pool_t)); }
os64_work_id_t os64_work_submit(os64_work_pool_t *p,const os64_work_t *work)
{
    for(size_t i=0;i<8;++i)if(!p->slots[i].id){
        p->slots[i].work=*work;p->slots[i].id=++p->next;
        p->slots[i].rc=work->run(work->job,not_cancelled,NULL,&p->slots[i].product);
        return p->slots[i].id;
    }
    return 0;
}
void os64_work_cancel(os64_work_pool_t *p,os64_work_id_t id)
{
    for(size_t i=0;i<8;++i)if(p->slots[i].id==id){
        p->slots[i].work.release(p->slots[i].work.job,p->slots[i].product);
        memset(&p->slots[i],0,sizeof(p->slots[i]));
    }
}
bool os64_work_reap(os64_work_pool_t *p,os64_work_id_t *id,int64_t *rc,void **job,void **product)
{
    for(size_t i=0;i<8;++i)if(p->slots[i].id){
        *id=p->slots[i].id;*rc=p->slots[i].rc;*job=p->slots[i].work.job;
        *product=p->slots[i].product;memset(&p->slots[i],0,sizeof(p->slots[i]));return true;
    }
    return false;
}
int64_t os64_work_pool_error(os64_work_pool_t *p) { return p?0:-1; }
void os64_work_pool_destroy(os64_work_pool_t *p)
{ if(!p)return;for(size_t i=0;i<8;++i)if(p->slots[i].id)os64_work_cancel(p,p->slots[i].id);os64_free(p); }

static os64_ui_file_dialog_t *dialog_new(os64_ui_file_mode_t mode)
{
    os64_ui_file_dialog_t *d=calloc(1,sizeof(*d));CHECK(d!=NULL);
    d->mode=mode;d->window=1;
    d->draw.surf=(os64_gui_surface_t){calloc(880u*700u,4),880,700,880};
    native_surface=d->draw.surf;
    os64_ui_file_options_t options={.mode=mode,.filename=mode==OS64_UI_FILE_SAVE?"download.html":NULL};
    CHECK(file_compose(d,&options)==OS64_UI_OK);
    CHECK(os64_browser_navigate(d->browser,"/home")==OS64_UI_OK);
    os64_ui_file_dialog_update(d);
    CHECK(!strcmp(os64_browser_location(d->browser),"/home"));
    return d;
}
static void dialog_free(os64_ui_file_dialog_t *d)
{
    os64_browser_destroy(d->browser);
    CHECK(os64_ui_font_release(&d->ui)==OS64_FONT_OK);
    free(d->draw.surf.pixels);free(d);native_surface=(os64_gui_surface_t){0};
}
static void input(os64_ui_file_dialog_t *d,const char *name)
{ os64_ui_textfield_set(&d->ui,&d->name,name);file_submit(d); }
static void key(os64_ui_file_dialog_t *d,char ascii,uint8_t scan,uint8_t mods)
{
    os64_gui_event_t e={.type=OS64_GUI_EVENT_KEY_DOWN};
    e.key.ascii=ascii;e.key.scancode=scan;e.key.modifiers=mods;
    os64_ui_file_dialog_event(d,&e);
    e.type=OS64_GUI_EVENT_KEY_UP;os64_ui_file_dialog_event(d,&e);
}
static uint64_t id_named(os64_ui_file_dialog_t *d,const char *name)
{
    const os64_directory_t *dir=os64_browser_directory(d->browser);
    for(size_t i=0;i<os64_directory_count(dir);++i){
        const os64_directory_entry_t *e=os64_directory_at(dir,i);
        if(!strcmp(e->entry.name,name))return e->id;
    }
    CHECK(false);return 0;
}
static void save_tests(void)
{
    current="Save";
    os64_ui_file_dialog_t *d=dialog_new(OS64_UI_FILE_SAVE);
    input(d,"");CHECK(d->result==OS64_UI_FILE_PENDING);
    input(d,"/missing/new.txt");CHECK(d->result==OS64_UI_FILE_PENDING);
    input(d,"bad\nname");CHECK(d->result==OS64_UI_FILE_PENDING);
    input(d,"existing.txt/");CHECK(d->result==OS64_UI_FILE_PENDING);
    input(d,"existing.txt");CHECK(d->confirm && !strcmp(d->pending_path,"/home/existing.txt"));
    CHECK(d->ui.focus==&d->keep && !os64_ui_widget_enabled(&d->name.w));
    key(d,'\n',0x1c,0);CHECK(!d->confirm && d->result==OS64_UI_FILE_PENDING);
    input(d,"existing.txt");key(d,27,0x01,0);CHECK(!d->confirm && d->result==OS64_UI_FILE_PENDING);
    input(d,"existing.txt");
    files[2].dir=true;
    file_clicked(&d->replace,d);CHECK(!d->confirm && d->result==OS64_UI_FILE_PENDING);
    files[2].dir=false;
    input(d,"existing.txt");file_clicked(&d->replace,d);
    CHECK(d->result==OS64_UI_FILE_ACCEPTED && d->replace_existing);
    CHECK(!strcmp(d->path,"/home/existing.txt"));
    key(d,27,0x01,0);CHECK(d->result==OS64_UI_FILE_ACCEPTED);
    dialog_free(d);
    d=dialog_new(OS64_UI_FILE_SAVE);
    fail_stat=true;input(d,"new.txt");CHECK(d->result==OS64_UI_FILE_PENDING);fail_stat=false;
    input(d,"folder");CHECK(os64_browser_loading(d->browser));
    input(d,"too-soon.txt");CHECK(d->result==OS64_UI_FILE_PENDING);
    os64_ui_file_dialog_update(d);
    CHECK(!strcmp(os64_browser_location(d->browser),"/home/folder"));
    input(d,"../fresh.txt");CHECK(d->result==OS64_UI_FILE_ACCEPTED && !d->replace_existing);
    CHECK(!strcmp(d->path,"/home/fresh.txt"));dialog_free(d);
}
static void open_tests(void)
{
    current="Open";
    os64_ui_file_dialog_t *d=dialog_new(OS64_UI_FILE_OPEN);
    input(d,"missing.txt");CHECK(d->result==OS64_UI_FILE_PENDING);
    input(d,"existing.txt/");CHECK(d->result==OS64_UI_FILE_PENDING);
    input(d,"folder");os64_ui_file_dialog_update(d);
    CHECK(!strcmp(os64_browser_location(d->browser),"/home/folder") && !*d->name.buf);
    input(d,"/home/existing.txt");CHECK(d->result==OS64_UI_FILE_ACCEPTED && !d->replace_existing);
    dialog_free(d);
    d=dialog_new(OS64_UI_FILE_OPEN);
    os64_ui_set_focus(&d->ui,&d->browser_ui.details.w);
    key(d,0,0x4f,0);
    CHECK(!strcmp(d->name.buf,"existing.txt"));
    key(d,'\n',0x1c,0);
    CHECK(d->result==OS64_UI_FILE_ACCEPTED && !strcmp(d->path,"/home/existing.txt"));
    dialog_free(d);
    d=dialog_new(OS64_UI_FILE_OPEN);
    key(d,27,0x01,0);CHECK(d->result==OS64_UI_FILE_CANCELLED && !*d->path);dialog_free(d);
}
static void folder_tests(void)
{
    current="New folder";
    os64_ui_file_dialog_t *d=dialog_new(OS64_UI_FILE_SAVE);
    file_clicked(&d->new_folder,d);CHECK(d->making_folder && d->ui.focus==&d->folder.w);
    os64_ui_textfield_set(&d->ui,&d->folder,"../escape");file_create_folder(d);
    CHECK(d->making_folder && mkdir_calls==0);
    os64_ui_textfield_set(&d->ui,&d->folder,"created");
    fail_mkdir=true;file_create_folder(d);CHECK(d->making_folder);fail_mkdir=false;
    file_create_folder(d);CHECK(!d->making_folder);
    os64_ui_file_dialog_update(d);
    CHECK(!strcmp(os64_browser_location(d->browser),"/home/created"));
    CHECK(!strcmp(d->name.buf,"download.html"));dialog_free(d);
}
static void navigation_tests(void)
{
    current="navigation and selection";
    os64_ui_file_dialog_t *d=dialog_new(OS64_UI_FILE_OPEN);
    key(d,12,0x26,OS64_GUI_MOD_CTRL);CHECK(d->ui.focus==&d->browser_ui.address.w);
    key(d,6,0x21,OS64_GUI_MOD_CTRL);CHECK(d->ui.focus==&d->browser_ui.filter.w);
    key(d,8,0x23,OS64_GUI_MOD_CTRL);CHECK(os64_browser_hidden(d->browser));
    uint64_t id=id_named(d,"existing.txt");
    CHECK(os64_ui_dataview_command(&d->browser_ui.details,OS64_UI_SELECTION_CHOOSE,id,true)==OS64_UI_OK);
    CHECK(os64_ui_dataview_command(&d->browser_ui.details,OS64_UI_SELECTION_ALL,0,false)==OS64_UI_OK);
    file_activate_pending(d);
    CHECK(os64_ui_selection_count(os64_ui_dataview_selection(&d->browser_ui.details))==1);
    uint64_t domain=os64_directory_domain(os64_browser_directory(d->browser));
    d->pending_domain=domain+1;d->pending_id=id;
    os64_ui_file_dialog_update(d);CHECK(d->result==OS64_UI_FILE_PENDING);
    CHECK(os64_browser_navigate(d->browser,"/missing")==OS64_UI_OK);
    os64_ui_file_dialog_update(d);CHECK(!strcmp(os64_browser_location(d->browser),"/home"));
    CHECK(d->result==OS64_UI_FILE_PENDING);
    dialog_free(d);
    d=dialog_new(OS64_UI_FILE_SAVE);
    id=id_named(d,"folder");
    CHECK(os64_ui_dataview_command(&d->browser_ui.details,OS64_UI_SELECTION_CHOOSE,id,true)==OS64_UI_OK);
    os64_ui_dataview_change_t change={.flags=OS64_UI_DATAVIEW_SELECTION,
        .domain=os64_directory_domain(os64_browser_directory(d->browser))};
    file_changed(&d->browser_ui.details,&change,d);
    CHECK(d->selected_folder==id);
    CHECK(os64_browser_filter(d->browser,"existing",false)==OS64_UI_OK);
    file_submit(d);
    CHECK(d->result==OS64_UI_FILE_ACCEPTED && !strcmp(d->path,"/home/download.html"));
    dialog_free(d);
}
static void screenshot(os64_ui_file_dialog_t *d,const char *filename)
{
    if(!filename)return;
    os64_ui_mark_dirty(&d->ui,&d->root);os64_ui_paint(&d->ui);
    FILE *f=fopen(filename,"wb");CHECK(f!=NULL);if(!f)return;
    fprintf(f,"P6\n%u %u\n255\n",d->draw.surf.width,d->draw.surf.height);
    for(size_t y=0;y<d->draw.surf.height;++y)for(size_t x=0;x<d->draw.surf.width;++x){
        uint32_t p=d->draw.surf.pixels[y*d->draw.surf.pitch_px+x];
        unsigned char rgb[]={(unsigned char)(p>>16),(unsigned char)(p>>8),(unsigned char)p};
        fwrite(rgb,1,3,f);
    }
    fclose(f);
}
static void layout_tests(const char *font_dir,const char *shot)
{
    current="fonts and failure recovery";
    os64_ui_file_dialog_t *d=dialog_new(OS64_UI_FILE_SAVE);
    CHECK(ui_test_adopt(&d->ui,font_dir,24)==OS64_FONT_OK);
    CHECK(d->name.w.bounds.w>0 && d->browser_ui.details.w.bounds.h>0);
    screenshot(d,shot);
    size_t refused=0;bool succeeded=false;
    for(long nth=0;nth<1024;++nth){
        os64_gui_rect_t before=d->name.w.bounds;
        d->root.bounds.w=d->root.bounds.w==880?900:880;
        deny_countdown=nth;file_resize(&d->ui);deny_countdown=-1;
        if(d->layout_pending){
            ++refused;CHECK(!memcmp(&before,&d->name.w.bounds,sizeof(before)));
            os64_ui_file_dialog_update(d);CHECK(!d->layout_pending);
        }else succeeded=true;
        if(succeeded)break;
    }
    CHECK(succeeded && refused>0);
    os64_gui_rect_t before=d->name.w.bounds;
    os64_font_set_t *candidate=outline_set(os64_ui_font_context(&d->ui),font_dir,"DejaVuSans.ttf",72);
    CHECK(candidate!=NULL);
    os64_font_consumer_t consumer;os64_ui_font_consumer(&d->ui,&consumer);
    CHECK(os64_font_adopt(candidate,&consumer,1,NULL)!=OS64_FONT_OK);
    CHECK(!memcmp(&before,&d->name.w.bounds,sizeof(before)));
    os64_font_set_release(candidate);dialog_free(d);
}
static void lifecycle_tests(void)
{
    current="window lifetime and allocation failure";
    os64_ui_file_dialog_t *d=calloc(1,sizeof(*d));
    os64_ui_dispatcher_t dispatcher;os64_ui_dispatcher_init(&dispatcher,NULL);
    os64_ui_file_options_t options={.mode=OS64_UI_FILE_SAVE,.directory="/home",.filename="new.txt"};
    CHECK(os64_ui_file_dialog_open(d,&dispatcher,0,&options)==OS64_UI_OK);
    CHECK(d->registered && os64_ui_dispatcher_live(&dispatcher,d->window));
    CHECK(os64_ui_file_dialog_open(d,&dispatcher,0,&options)==OS64_UI_BUSY);
    destroy_refused=true;
    CHECK(os64_ui_file_dialog_close(d)==OS64_UI_ERROR && d->window>0);
    CHECK(os64_ui_dispatcher_turn(&dispatcher,false)>=0);
    destroy_refused=false;
    CHECK(os64_ui_file_dialog_close(d)==OS64_UI_OK && !d->window);
    CHECK(!os64_ui_dispatcher_can_create(&dispatcher));
    CHECK(os64_ui_dispatcher_turn(&dispatcher,false)>=0);
    CHECK(os64_ui_dispatcher_can_create(&dispatcher));
    size_t refused=0;bool success=false;
    for(long nth=0;nth<1024;++nth){
        deny_countdown=nth;
        os64_ui_status_code_t rc=os64_ui_file_dialog_open(d,NULL,0,&options);
        deny_countdown=-1;
        if(rc!=OS64_UI_OK)++refused;else success=true;
        CHECK(os64_ui_file_dialog_close(d)==OS64_UI_OK);
        if(rc==OS64_UI_OK && refused>0)break;
    }
    CHECK(success && refused>0 && !native_owned);
    options.filename="../bad";
    CHECK(os64_ui_file_dialog_open(d,NULL,0,&options)==OS64_UI_INVALID && !d->window);
    free(d);
}
static void resize_event(os64_ui_file_dialog_t *d,uint32_t w,uint32_t h,uint32_t flags)
{
    native_flags=flags;native_resize(w,h);
    os64_gui_event_t event={.type=OS64_GUI_EVENT_WINDOW_RESIZE};
    os64_ui_file_dialog_event(d,&event);
    os64_ui_file_dialog_update(d);
}
static void size_tests(const char *font_dir)
{
    current="shared size preference";
    os64_ui_file_dialog_t *d=calloc(1,sizeof(*d));
    os64_ui_file_options_t options={.mode=OS64_UI_FILE_OPEN,.directory="/home"};
    size_writes=0;
    CHECK(os64_ui_file_dialog_open(d,NULL,0,&options)==OS64_UI_OK);
    CHECK(d->draw.surf.width==880 && d->draw.surf.height==700 && size_writes==0);
    resize_event(d,960,760,0);
    CHECK(size_writes==1 && !strcmp(saved_size,"960,760"));
    key(d,27,0x01,0);CHECK(d->result==OS64_UI_FILE_CANCELLED);
    CHECK(os64_ui_file_dialog_close(d)==OS64_UI_OK && size_writes==1);
    options.mode=OS64_UI_FILE_SAVE;
    CHECK(os64_ui_file_dialog_open(d,NULL,0,&options)==OS64_UI_OK);
    CHECK(d->draw.surf.width==960 && d->draw.surf.height==760);
    /* Another process publishes a new size while this dialog stays open. */
    strcpy(saved_size,"1100,800");
    resize_event(d,960,760,0);
    CHECK(size_writes==1 && !strcmp(saved_size,"1100,800"));
    resize_event(d,1600,1000,OS64_GUI_WINDOW_MAXIMIZED);
    resize_event(d,960,760,0);
    CHECK(size_writes==1 && !strcmp(saved_size,"1100,800"));
    fail_size_write=true;resize_event(d,1000,780,0);fail_size_write=false;
    CHECK(size_writes==2 && !strcmp(saved_size,"1100,800") && d->result==OS64_UI_FILE_PENDING);
    CHECK(os64_ui_file_dialog_close(d)==OS64_UI_OK && size_writes==2);
    CHECK(os64_ui_file_dialog_open(d,NULL,0,&options)==OS64_UI_OK);
    CHECK(d->draw.surf.width==1100 && d->draw.surf.height==800);
    CHECK(os64_ui_file_dialog_close(d)==OS64_UI_OK);
    const char *bad[]={"-1,700","900,0","1000,garbage","1,2,3","99999999999999999999,700","900x700"};
    for(size_t i=0;i<sizeof(bad)/sizeof(*bad);++i){
        strcpy(saved_size,bad[i]);
        CHECK(os64_ui_file_dialog_open(d,NULL,0,&options)==OS64_UI_OK);
        CHECK(d->draw.surf.width==880 && d->draw.surf.height==700);
        CHECK(os64_ui_file_dialog_close(d)==OS64_UI_OK);
    }
    strcpy(saved_size,"32767,32767");
    CHECK(os64_ui_file_dialog_open(d,NULL,0,&options)==OS64_UI_OK);
    CHECK(d->draw.surf.width==screen_width && d->draw.surf.height==screen_height);
    CHECK(os64_ui_file_dialog_close(d)==OS64_UI_OK && size_writes==2);
    strcpy(saved_size,"1,1");startup_fonts=font_dir;
    CHECK(os64_ui_file_dialog_open(d,NULL,0,&options)==OS64_UI_OK);
    CHECK(d->draw.surf.width>400 && d->draw.surf.height>300 && !d->layout_pending);
    CHECK(d->hidden.w.bounds.y<d->accept.bounds.y);
    CHECK(d->name.w.bounds.h>32);
    /* A queued native resize caused by setting the minimum is not a user choice. */
    os64_gui_event_t event={.type=OS64_GUI_EVENT_WINDOW_RESIZE};
    os64_ui_file_dialog_event(d,&event);
    CHECK(size_writes==2 && !strcmp(saved_size,"1,1"));
    CHECK(os64_ui_file_dialog_close(d)==OS64_UI_OK);
    startup_fonts=NULL;saved_size[0]=0;free(d);
}
static void display_size_tests(void)
{
    current="saved size after display change";
    os64_ui_file_dialog_t *d=calloc(1,sizeof(*d));CHECK(d!=NULL);
    const uint32_t displays[][2]={{2560,1440},{1920,1080},{1024,768},{5120,2880}};
    const uint32_t preferences[][2]={{2500,1360},{5000,2700},{800,5000}};
    const os64_ui_file_mode_t modes[]={OS64_UI_FILE_OPEN,OS64_UI_FILE_SAVE};
    unsigned before=size_writes;
    for(size_t p=0;p<sizeof(preferences)/sizeof(*preferences);++p){
        char original[64];
        snprintf(original,sizeof(original),"%u,%u",preferences[p][0],preferences[p][1]);
        strcpy(saved_size,original);
        for(size_t i=0;i<sizeof(displays)/sizeof(*displays);++i){
            screen_width=displays[i][0];screen_height=displays[i][1];
            for(size_t m=0;m<sizeof(modes)/sizeof(*modes);++m){
                os64_ui_file_options_t options={.mode=modes[m],.directory="/home"};
                os64_ui_status_code_t rc=os64_ui_file_dialog_open(d,NULL,0,&options);
                CHECK(rc==OS64_UI_OK);
                if(rc==OS64_UI_OK){
                    CHECK(d->draw.surf.width==(preferences[p][0]<screen_width?preferences[p][0]:screen_width));
                    CHECK(d->draw.surf.height==(preferences[p][1]<screen_height?preferences[p][1]:screen_height));
                    CHECK(!d->layout_pending && d->result==OS64_UI_FILE_PENDING);
                    key(d,27,0x01,0);
                }
                CHECK(os64_ui_file_dialog_close(d)==OS64_UI_OK && !d->window);
                CHECK(size_writes==before && !strcmp(saved_size,original));
            }
        }
    }
    screen_width=1024;screen_height=768;fail_screen_info=true;
    strcpy(saved_size,"5000,2700");
    os64_ui_file_options_t options={.mode=OS64_UI_FILE_OPEN,.directory="/home"};
    CHECK(os64_ui_file_dialog_open(d,NULL,0,&options)==OS64_UI_OK);
    CHECK(d->draw.surf.width==1024 && d->draw.surf.height==768);
    CHECK(os64_ui_file_dialog_close(d)==OS64_UI_OK);
    CHECK(size_writes==before && !strcmp(saved_size,"5000,2700"));
    fail_screen_info=false;screen_width=1600;screen_height=1000;saved_size[0]=0;
    free(d);
}
static void review_resize_tests(const char *font_dir)
{
    current="review: oversized startup font and manual resize from maximized";
    os64_ui_file_dialog_t *d=calloc(1,sizeof(*d));CHECK(d!=NULL);
    unsigned before=size_writes;
    screen_width=640;screen_height=480;
    strcpy(saved_size,"2500,1360");startup_fonts=font_dir;startup_font_size=96;
    for(int mode=0;mode<2;++mode) {
        os64_ui_file_options_t options={.mode=mode?OS64_UI_FILE_OPEN:OS64_UI_FILE_SAVE,.directory="/home"};
        os64_ui_status_code_t rc=os64_ui_file_dialog_open(d,NULL,0,&options);
        CHECK(rc==OS64_UI_OK);
        if(rc==OS64_UI_OK) {
            CHECK(!d->layout_pending && d->name.w.bounds.h<96);
            CHECK(d->accept.bounds.y+d->accept.bounds.h<=(int32_t)d->draw.surf.height);
            CHECK(d->min_width<=640 && d->min_height<=480);
            key(d,27,0x01,0);CHECK(d->result==OS64_UI_FILE_CANCELLED);
        }
        CHECK(os64_ui_file_dialog_close(d)==OS64_UI_OK);
        CHECK(size_writes==before && !strcmp(saved_size,"2500,1360"));
    }
    startup_fonts=NULL;startup_font_size=32;screen_width=1600;screen_height=1000;
    strcpy(saved_size,"960,760");
    os64_ui_file_options_t options={.mode=OS64_UI_FILE_OPEN,.directory="/home"};
    CHECK(os64_ui_file_dialog_open(d,NULL,0,&options)==OS64_UI_OK);
    resize_event(d,1600,1000,OS64_GUI_WINDOW_MAXIMIZED);
    resize_event(d,1200,850,0); /* WM clears MAXIMIZED before the drag's event. */
    CHECK(size_writes==before+1 && !strcmp(saved_size,"1200,850"));
    key(d,27,0x01,0);
    CHECK(os64_ui_file_dialog_close(d)==OS64_UI_OK);
    options.mode=OS64_UI_FILE_SAVE;
    CHECK(os64_ui_file_dialog_open(d,NULL,0,&options)==OS64_UI_OK);
    CHECK(d->draw.surf.width==1200 && d->draw.surf.height==850);
    resize_event(d,1600,1000,OS64_GUI_WINDOW_MAXIMIZED);
    resize_event(d,1200,850,0); /* A true restore must not republish stale settings. */
    CHECK(size_writes==before+1);
    CHECK(os64_ui_file_dialog_close(d)==OS64_UI_OK);
    saved_size[0]=0;free(d);
}
static void compact_tests(void)
{
    current="compact and wrapped footer";
    os64_ui_file_dialog_t *d=dialog_new(OS64_UI_FILE_SAVE);
    int32_t top=d->browser_area.bounds.h;
    CHECK(d->new_folder.bounds.y==d->accept.bounds.y);
    CHECK(d->new_folder.bounds.x+d->new_folder.bounds.w<d->cancel.bounds.x);
    CHECK(d->cancel.bounds.x<d->accept.bounds.x);
    CHECK(d->message.bounds.h==0);
    input(d,"existing.txt");os64_ui_file_dialog_update(d);
    CHECK(d->confirm && d->message.bounds.h>0 && d->browser_area.bounds.h<top);
    file_cancel_prompt(d);os64_ui_file_dialog_update(d);
    CHECK(d->browser_area.bounds.h==top && d->message.bounds.h==0);
    d->root.bounds.w=d->min_width;
    file_resize(&d->ui);
    CHECK(!d->layout_pending && d->new_folder.bounds.y<d->accept.bounds.y);
    CHECK(d->new_folder.bounds.x+d->new_folder.bounds.w<=d->root.bounds.w);
    CHECK(d->name.w.bounds.w>0 && d->accept.bounds.y+d->accept.bounds.h<=d->root.bounds.h);
    dialog_free(d);
}
int main(int argc,char **argv)
{
    add_file("/",true);add_file("/home",true);add_file("/home/existing.txt",false);
    add_file("/home/folder",true);add_file("/home/.hidden",false);
    save_tests();open_tests();folder_tests();navigation_tests();lifecycle_tests();
    compact_tests();size_tests(argc>1?argv[1]:"userland/libfreetype/fixtures");
    display_size_tests();
    review_resize_tests(argc>1?argv[1]:"userland/libfreetype/fixtures");
    layout_tests(argc>1?argv[1]:"userland/libfreetype/fixtures",argc>2?argv[2]:NULL);
    printf("file dialogs: %lu checks, %d failures\n",checks,failures);
    return failures?1:0;
}
