/* Production placement and checked config writer; deterministic storage/native
 * boundaries exercise failures without executing Os64 syscalls on the host. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "os64/ui_window.h"
#include "os64/conf.h"
#include "os64/io.h"

static char data[OS64_CONF_MAX+8], temporary[OS64_CONF_MAX*2];
static size_t data_len, cursor, temporary_len;
static bool exists, inherited, unreadable, read_failure, write_failure, sync_failure, rename_failure, no_memory;
static bool temporary_exists, destroy_busy, draw_failure, native_alive;
static bool create_failure;
static unsigned publications, ui_inits, creates;
static uint32_t screen_w=1024, screen_h=768;
static os64_window_geometry_t created;
void *os64_malloc(size_t n) { return no_memory ? NULL : malloc(n); }
void os64_free(void *p) { free(p); }
uint64_t os64_taskid(void) { return 42; }
int64_t os64_open(const char *path,const char *mode)
{
    (void)path;
    if (!strcmp(mode,"w")) { temporary_len=0; temporary_exists=true; return 12; }
    assert(!strcmp(mode,"r"));
    if (!exists || unreadable) return -1;
    cursor=0; return 11;
}
int64_t os64_read(int32_t fd,void *out,uint64_t count)
{
    assert(fd==11);
    if (read_failure) return -1;
    if (count>data_len-cursor) count=data_len-cursor;
    if (count>7) count=7;
    memcpy(out,data+cursor,(size_t)count);cursor+=(size_t)count;return (int64_t)count;
}
int64_t os64_write(int32_t fd,const void *in,uint64_t count)
{
    assert(fd==12);
    if (write_failure) return -1;
    if (count>7) count=7;
    assert(temporary_len+count<sizeof(temporary));
    memcpy(temporary+temporary_len,in,(size_t)count);temporary_len+=(size_t)count;
    temporary[temporary_len]=0;return (int64_t)count;
}
int64_t os64_close(int32_t fd) { (void)fd;return 0; }
int64_t os64_sync(int32_t fd) { assert(fd==12);return sync_failure ? -1 : 0; }
int64_t os64_unlink(const char *path) { (void)path;temporary_exists=false;return 0; }
int64_t os64_rename_with_flags(const char *from,const char *to,uint64_t flags)
{
    (void)from; assert(!strcmp(to,"/config/window.conf"));
    assert(flags==OS64_RENAME_NOREPLACE || flags==OS64_RENAME_REQUIRE_ATOMIC_REPLACE);
    if (rename_failure || (flags==OS64_RENAME_NOREPLACE && exists && !inherited)) return -1;
    assert(temporary_len<sizeof(data)); memcpy(data,temporary,temporary_len+1);data_len=temporary_len;
    exists=true;inherited=false;temporary_exists=false;++publications;return 0;
}
int64_t os64_rename(const char *a,const char *b)
{ return os64_rename_with_flags(a,b,OS64_RENAME_REQUIRE_ATOMIC_REPLACE); }
uint64_t file_test_syscall(uint64_t nr,uint64_t a,uint64_t b,uint64_t c,uint64_t d,uint64_t e,uint64_t f)
{
    switch (nr) {
    case SYSCALL_CONF_RESOLVE:
        assert(!strcmp((const char *)(uintptr_t)a,"window.conf"));
        if (!e && !exists) return (uint64_t)-1;
        snprintf((char *)(uintptr_t)b,(size_t)c,"/%s/window.conf",!e && inherited ? "etc" : "config");
        return 1;
    case SYSCALL_GUI_SCREEN_INFO:
        *(uint32_t *)(uintptr_t)a=screen_w;*(uint32_t *)(uintptr_t)b=screen_h;return 0;
    case SYSCALL_GUI_WINDOW_CREATE:
        if (create_failure) return (uint64_t)OS64_GUI_ERR_NOT_RUNNING;
        assert(!native_alive); assert(f & OS64_GUI_CREATE_FIT_SCREEN);
        created=(os64_window_geometry_t){(int32_t)b,(int32_t)c,(uint32_t)d,(uint32_t)e};
        native_alive=true;++creates;return 17;
    case SYSCALL_GUI_WINDOW_DESTROY:
        assert(a==17); if (destroy_busy) return (uint64_t)OS64_GUI_ERR_BUSY;
        native_alive=false;return 0;
    default: assert(!"unexpected native call");return (uint64_t)-1;
    }
}
int64_t os64_draw_ctx_init(os64_draw_ctx_t *ctx,int64_t win)
{ if (draw_failure) return -1;ctx->win=win;return 0; }
void os64_ui_init(os64_ui_t *ui,os64_draw_ctx_t *ctx)
{ *ui=(os64_ui_t){.ctx=ctx};++ui_inits; }
static void record(const char *s)
{
    strcpy(data,s);data_len=strlen(s);exists=true;inherited=false;
}
static os64_window_placement_t tracker(os64_window_remember_mode_t mode)
{
    os64_window_placement_t p;
    assert(os64_window_placement_init(&p,(os64_window_remember_t){"window.conf",mode},
        (os64_window_geometry_t){10,20,640,440}));
    return p;
}
static void edit(os64_window_placement_t *p,uint32_t mask,int32_t x,uint32_t w,uint32_t h)
{
    os64_gui_event_t e={.type=OS64_GUI_EVENT_WINDOW_PLACEMENT,
        .placement={.x=x,.y=-20,.width=w,.height=h,.changed=mask}};
    assert(os64_window_placement_event(p,&e));
}
static void geometry_tests(void)
{
    record("# keep\nother = yes\ngeometry = 1,-10,20,2500,1360\n");inherited=true;
    os64_window_placement_t p=tracker(OS64_WINDOW_REMEMBER_POSITION_AND_SIZE);
    assert(!p.status && p.preferred.width==2500);
    os64_window_geometry_t live=os64_window_placement_prepare(&p);
    assert(live.width==1024 && live.height==768 && !publications);
    edit(&p,OS64_GUI_PLACEMENT_X,80,1024,768);
    assert(!p.status && strstr(data,"1,80,20,2500,1360") && strstr(data,"# keep") && strstr(data,"other = yes"));
    edit(&p,OS64_GUI_PLACEMENT_HEIGHT,0,1024,600);
    assert(strstr(data,"1,80,20,2500,600"));
    screen_w=2560;screen_h=1440;live=os64_window_placement_prepare(&p);
    assert(live.width==2500 && live.height==600);
    os64_window_placement_t old=p;
    edit(&p,OS64_GUI_PLACEMENT_WIDTH,0,900,0);
    edit(&old,OS64_GUI_PLACEMENT_X,100,0,0);
    assert(strstr(data,"1,100,20,900,600"));
    unsigned saved=publications;
    unsigned automatic[]={OS64_GUI_EVENT_WINDOW_RESIZE,OS64_GUI_EVENT_APPEARANCE,OS64_GUI_EVENT_WINDOW_CLOSE};
    for (unsigned i=0;i<3;++i) {
        os64_gui_event_t e={.type=automatic[i]};assert(!os64_window_placement_event(&old,&e));
    }
    assert(publications==saved);
    record("size = 1900,1000\n");p=tracker(OS64_WINDOW_REMEMBER_SIZE);
    edit(&p,OS64_GUI_PLACEMENT_X,50,0,0);assert(publications==saved);
    edit(&p,OS64_GUI_PLACEMENT_HEIGHT,0,0,700);
    assert(strstr(data,"size = 1900,700"));
}
static void failure_tests(void)
{
    const char *bad[]={"", "2,1,2,640,440", "1,2147483648,2,640,440", "1,-2147483649,2,640,440",
        "1,1,2,0,440", "1,1,2,32768,440", "1,1,2,640,440,5", "1,1,2,640,99999999999999999999999"};
    for (size_t i=0;i<sizeof(bad)/sizeof(*bad);++i) {
        char text[160];snprintf(text,sizeof(text),"geometry = %s\n",bad[i]);record(text);
        os64_window_placement_t p=tracker(OS64_WINDOW_REMEMBER_POSITION_AND_SIZE);
        assert(p.preferred.width==640 && p.preferred.x==10);
        edit(&p,OS64_GUI_PLACEMENT_X,30,0,0);
        assert(!p.status && strstr(data,"1,30,20,640,440"));
    }
    record("geometry = 1,-2147483648,2147483647,32767,32767\n");
    os64_window_placement_t p=tracker(OS64_WINDOW_REMEMBER_POSITION_AND_SIZE);
    assert(p.preferred.x==INT32_MIN && p.preferred.y==INT32_MAX);
    bool *failures[]={&unreadable,&read_failure,&write_failure,&sync_failure,&rename_failure,&no_memory};
    for (unsigned i=0;i<sizeof(failures)/sizeof(*failures);++i) {
        record("geometry = 1,1,2,900,700\n");char before[128];strcpy(before,data);unsigned saved=publications;
        p=tracker(OS64_WINDOW_REMEMBER_POSITION_AND_SIZE);*failures[i]=true;
        edit(&p,OS64_GUI_PLACEMENT_X,99,0,0);*failures[i]=false;
        assert(p.status && publications==saved && !strcmp(before,data) && !temporary_exists);
    }
    record("broken setting\ngeometry = 1,1,2,900,700\n");p=tracker(OS64_WINDOW_REMEMBER_POSITION_AND_SIZE);
    assert(p.status);unsigned saved=publications;edit(&p,1,99,0,0);assert(publications==saved);
    record("geometry = 1,1,2,900,700\n");data[4]=0;p=tracker(OS64_WINDOW_REMEMBER_POSITION_AND_SIZE);
    assert(p.status);edit(&p,1,99,0,0);assert(publications==saved);
    memset(data,'x',OS64_CONF_MAX);data_len=OS64_CONF_MAX;p=tracker(OS64_WINDOW_REMEMBER_POSITION_AND_SIZE);
    assert(p.status==OS64_CONF_TRUNCATED);edit(&p,1,99,0,0);assert(publications==saved);
    exists=false;p=tracker(OS64_WINDOW_REMEMBER_POSITION_AND_SIZE);edit(&p,1,55,0,0);
    assert(!p.status && strstr(data,"1,55,20,640,440"));
    os64_conf_pair_t pair={"geometry","1,99,20,640,440"};saved=publications;
    assert(os64_conf_create_checked("/config/window.conf",&pair,1,NULL,NULL)!=0);
    unreadable=true;assert(os64_conf_create_checked("/config/window.conf",&pair,1,NULL,NULL)!=0);unreadable=false;
    assert(publications==saved && strstr(data,"1,55,20,640,440") && !temporary_exists);
}
static void ownership_tests(void)
{
    os64_ui_t ui;os64_draw_ctx_t ctx;int64_t win=0;
    char name[]="window.conf";
    os64_ui_window_options_t o={.title="test",.content_width=640,.content_height=440,
        .remember={name,OS64_WINDOW_REMEMBER_POSITION_AND_SIZE}};
    record("geometry = 1,30,40,900,700\n");
    assert(os64_ui_window_open(&ui,&ctx,&win,&o)==OS64_UI_OK);
    name[0]='X';assert(!strcmp(ui.placement.config,"window.conf"));name[0]='w';
    assert(win==17 && ui_inits==1 && created.x==30 && created.width==900 && ui.placement_event);
    assert(os64_gui_window_destroy(win)==0);
    draw_failure=true;destroy_busy=true;
    assert(os64_ui_window_open(&ui,&ctx,&win,&o)==OS64_UI_ERROR && win==17 && native_alive);
    assert(ui.ctx==&ctx && ctx.win==win && ui_inits==2);
    destroy_busy=false;assert(os64_gui_window_destroy(win)==0);
    assert(os64_ui_window_open(&ui,&ctx,&win,&o)==OS64_UI_ERROR && !win && !native_alive);
    draw_failure=false;
    o.content_width=0;unsigned before=creates;
    assert(os64_ui_window_open(&ui,&ctx,&win,&o)==OS64_UI_INVALID && !win && creates==before);
    o.content_width=640;o.remember.config=NULL;
    create_failure=true;
    assert(os64_ui_window_open(&ui,&ctx,&win,&o)==OS64_UI_ERROR && win==OS64_GUI_ERR_NOT_RUNNING && !native_alive);
    create_failure=false;
    assert(os64_ui_window_open(&ui,&ctx,&win,&o)==OS64_UI_OK && !ui.placement_event && created.width==640);
    assert(os64_gui_window_destroy(win)==0);
}
int main(void)
{
    geometry_tests();failure_tests();ownership_tests();
    puts("placement: real parser/writer, component merge, display fitting, failures, NOREPLACE and initializer ownership passed");
}
