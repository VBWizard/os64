/* Exercise gclock's production callbacks, font engine, and preserving config
 * writer. The host supplies files, a clock, and a bounded window surface. */
#define _GNU_SOURCE
#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#define main gclock_guest_main
#include "../userland/apps/gclock/gclock.c"
#undef main

static char test_root[256], font_path[256];
static os64_gui_window_state_t state = {.x = 600, .y = 400};
static os64_gui_surface_t surface;
static os64_font_config_t shared_fonts;
static uint64_t generation;
static bool refuse_resize, fail_read;
static unsigned resize_calls;
static size_t live_allocations;
static uint32_t pixels[1600 * 600];

void *os64_malloc(size_t n)
{ void *p = malloc(n); if (p) ++live_allocations; return p; }
void os64_free(void *p)
{ if (p) { assert(live_allocations); --live_allocations; } free(p); }
uint64_t os64_taskid(void) { return (uint64_t)getpid(); }
int64_t os64_open(const char *path, const char *mode)
{
    if (!strcmp(path, CLOCK_SCALABLE_FACE)) path = font_path;
    return open(path, *mode == 'w' ? O_WRONLY | O_CREAT | O_TRUNC : O_RDONLY, 0600);
}
int64_t os64_read(int32_t fd, void *p, size_t n) { return fail_read ? -1 : read(fd,p,n); }
int64_t os64_write(int32_t fd, const void *p, size_t n) { return write(fd,p,n); }
int64_t os64_close(int32_t fd) { return close(fd); }
int64_t os64_stat(const char *path, os64_dirent_t *out)
{
    if (!strcmp(path, CLOCK_SCALABLE_FACE)) path = font_path;
    struct stat st;
    if (stat(path, &st)) return -1;
    memset(out, 0, sizeof(*out)); out->size = st.st_size;
    return 0;
}
int64_t os64_sync(int32_t fd) { return fsync(fd); }
int64_t os64_unlink(const char *p) { return unlink(p); }
int64_t os64_rename(const char *a, const char *b) { return rename(a,b); }
int64_t os64_rename_with_flags(const char *a, const char *b, uint64_t flags)
{ (void)flags; return rename(a,b); }
void os64_debug_log(const char *s) { (void)s; }
void os64_complain(const char *s, ...) { fprintf(stderr,"%s\n",s); }
void os64_ui_theme_defaults(os64_ui_theme_t *t)
{ memset(t,0,sizeof(*t)); t->panel_bg=0xffeeeeee; t->label_fg=0xff000000; }
void os64_ui_theme_current(os64_ui_theme_t *t, uint64_t *g) { (void)t; *g=0; }
bool os64_ui_theme_session(os64_ui_theme_t *t, uint64_t *g, uint64_t hint)
{ (void)t; (void)g; (void)hint; return false; }
int os64_font_settings_current(os64_font_config_t *out, uint64_t *g)
{ *out=shared_fonts; *g=generation; return 0; }
int64_t os64_date_now(os64_date_t *date, os64_time_t *offset)
{ (void)offset; *date=(os64_date_t){.hour=11,.minute=28,.second=59}; return 0; }

uint64_t os64_syscall6(uint64_t nr, uint64_t name, uint64_t out, uint64_t cap,
                      uint64_t from, uint64_t any, uint64_t unused)
{
    (void)unused; assert(nr == SYSCALL_CONF_RESOLVE);
    if (from) return (uint64_t)-1;
    int n=snprintf((char *)out,cap,"%s/%s",test_root,(char *)name);
    assert(n>0 && (uint64_t)n<cap);
    if (any || !access((char *)out,F_OK)) return 1;
    n=snprintf((char *)out,cap,"%s/system/%s",test_root,(char *)name);
    assert(n>0 && (uint64_t)n<cap);
    return !access((char *)out,F_OK) ? 1 : (uint64_t)-1;
}
uint64_t os64_syscall2(uint64_t nr, uint64_t a, uint64_t b)
{
    (void)a;
    if (nr==SYSCALL_GUI_WINDOW_GET_STATE) { *(os64_gui_window_state_t *)b=state; return 0; }
    if (nr==SYSCALL_GUI_WINDOW_GET_SURFACE) { *(os64_gui_surface_t *)b=surface; return 0; }
    if (nr==SYSCALL_GUI_WINDOW_PUBLISH) return 0;
    assert(!"unexpected syscall2"); return (uint64_t)-1;
}
uint64_t os64_syscall3(uint64_t nr, uint64_t a, uint64_t w, uint64_t h)
{
    (void)a; assert(nr==SYSCALL_GUI_WINDOW_SET_MIN_SIZE); ++resize_calls;
    if (refuse_resize || w>1600 || h>600) return (uint64_t)-1;
    if (w>surface.width) surface.width=w;
    if (h>surface.height) surface.height=h;
    return 0;
}
static void write_config(const char *relative, const char *text)
{
    char path[512]; snprintf(path,sizeof(path),"%s/%s",test_root,relative);
    FILE *f=fopen(path,"w"); assert(f); assert(fputs(text,f)>=0); assert(!fclose(f));
}
static void read_config(char *out, size_t cap)
{
    char path[512]; snprintf(path,sizeof(path),"%s/gclock.conf",test_root);
    FILE *f=fopen(path,"r"); assert(f);
    size_t n=fread(out,1,cap-1,f); assert(feof(f)); out[n]=0; fclose(f);
}
static gclock_conf_t saved_config(void)
{
    char path[256]; gclock_conf_t conf=conf_defaults("test");
    assert(os64_conf_find_read("gclock.conf",conf_line,&conf,path,sizeof(path))>=0);
    return conf;
}
static void persistence(void)
{
    char dir[512]; snprintf(dir,sizeof(dir),"%s/system",test_root); assert(!mkdir(dir,0700));
    write_config("system/gclock.conf","Position=123,234\nSavePosition=false\nFontSize=32\nBlink=false\n");
    on_close_request(NULL);
    gclock_conf_t conf=saved_config();
    assert(conf.x==123 && conf.y==234 && !conf.save_position && conf.font_size==32 && !conf.blink);
    write_config("gclock.conf","# Keep this comment\nPosition = 12,34 # anchor\nSavePosition = false\nFontSize = 28\nBlink = false\n");
    on_close_request(NULL);
    char text[8192]; read_config(text,sizeof(text));
    assert(strstr(text,"Position = 12,34 # anchor\n"));
    assert(strstr(text,"# Keep this comment\n"));
    // The close callback rereads the setting, independently of startup state.
    gConf.save_position=false;
    write_config("gclock.conf","Position=12,34\nSavePosition=true\n");
    on_close_request(NULL); conf=saved_config(); assert(conf.x==600 && conf.y==400);
    state.flags=OS64_GUI_WINDOW_MAXIMIZED;
    write_config("gclock.conf","Position=12,34\n");
    on_close_request(NULL); conf=saved_config(); assert(conf.x==12 && conf.y==34);
    state.flags=0;
    fail_read=true; on_close_request(NULL); fail_read=false;
    conf=saved_config(); assert(conf.x==12 && conf.y==34);
    conf=conf_defaults("test");
    conf_line("FontSize","32",&conf); conf_line("FontSize","999999999999999999999999999999",&conf);
    assert(conf.font_size==32);
    conf_line("Position","2147483648,2",&conf); assert(conf.x==280 && conf.y==10);
    conf_line("SavePosition","purple",&conf); assert(conf.save_position);
    puts("gclock: persistence, first personal save, live config edits, maximize and malformed values PASS");
}
static void fonts(void)
{
    gRunning=true; gClockWin=0; gConf=conf_defaults("test");
    os64_font_config_defaults(&shared_fonts);
    os64_ui_init(&gUi,NULL); os64_ui_panel(&gRoot); os64_ui_set_root(&gUi,&gRoot);
    refresh_clock_text();
    for (int i=0;i<CLOCK_CHARS;++i) {
        os64_ui_label(&gDigits[i],gText[i]);
        if (!i) { gDigitClass=*gDigits[i].cls; gDigitClass.paint=digit_paint; }
        gDigits[i].cls=&gDigitClass;
        os64_ui_add_child(&gRoot,&gDigits[i]);
    }
    os64_ui_font_planner(&gUi,plan_clock_font,commit_clock_font,discard_clock_font,NULL);
    refresh_clock_font(&gUi); assert(gUi.font_settings_ready && gLayout.row_h==16);
    surface=(os64_gui_surface_t){.pixels=pixels,.width=gLayout.width,.height=gLayout.height,.pitch_px=1600};
    gClockWin=1; gUi.ctx=&gCtx; assert(!os64_draw_ctx_init(&gCtx,1));
    gUi.on_resize=layout_clock; gUi.font_session=refresh_clock_font;
    strcpy(shared_fonts.roles[OS64_FONT_ROLE_UI].face[0],font_path);
    shared_fonts.roles[OS64_FONT_ROLE_UI].size=32; ++generation;
    os64_gui_event_t appearance={.type=OS64_GUI_EVENT_APPEARANCE};
    os64_ui_dispatch(&gUi,&appearance);
    assert(gUi.font_generation==generation && gLayout.width>90 && gLayout.row_h>16);
    assert(surface.width>90 && gRoot.bounds.w==(int32_t)surface.width);
    clock_layout_t before=gLayout; uint64_t old_generation=gUi.font_generation;
    os64_gui_rect_t bounds=gDigits[7].bounds;
    shared_fonts.roles[OS64_FONT_ROLE_UI].size=48; ++generation;
    refuse_resize=true; refresh_clock_font(&gUi); refuse_resize=false;
    assert(!memcmp(&gLayout,&before,sizeof(before)) && gUi.font_generation==old_generation);
    assert(!memcmp(&bounds,&gDigits[7].bounds,sizeof(bounds)) && !gPendingLayout);
    refresh_clock_font(&gUi); assert(gUi.font_generation==generation && gLayout.row_h>before.row_h);
    surface.width+=100; surface.height+=50;
    os64_gui_event_t resize={.type=OS64_GUI_EVENT_WINDOW_RESIZE}; os64_ui_dispatch(&gUi,&resize);
    assert(gDigits[0].bounds.x==(int32_t)(surface.width-6*gLayout.digit_w-2*gLayout.colon_w)/2);
    // Blinking changes content, not geometry; painting clears the old colon.
    os64_ui_mark_dirty(&gUi,&gRoot); os64_ui_paint(&gUi);
    bounds=gDigits[2].bounds; separatorsShown=false; refresh_clock_text();
    assert(!gText[2][0] && !gText[5][0] && !memcmp(&bounds,&gDigits[2].bounds,sizeof(bounds)));
    digit_paint(&gDigits[2],&gCtx,&gUi.theme);
    for (int y=bounds.y;y<bounds.y+bounds.h;++y)
        for (int x=bounds.x;x<bounds.x+bounds.w;++x) assert(pixels[y*1600+x]==gUi.theme.panel_bg);
    gConf.blink=false; refresh_clock_text(); assert(gText[2][0]==':' && gText[5][0]==':');
    gConf.font_size=28; ++generation; refresh_clock_font(&gUi);
    int row=gLayout.row_h;
    shared_fonts.roles[OS64_FONT_ROLE_UI].size=72; ++generation; refresh_clock_font(&gUi);
    assert(gLayout.row_h==row);
    os64_font_config_defaults(&shared_fonts); ++generation; refresh_clock_font(&gUi);
    assert(gLayout.row_h==row); // builtin + override uses the included scalable face
    gConf.font_size=0; ++generation; refresh_clock_font(&gUi); assert(gLayout.row_h==16);
    assert(os64_ui_font_release(&gUi)==OS64_FONT_OK);
    assert(resize_calls>=6);
    puts("gclock: live scalable fonts, growth, refusal rollback, resize, blink stability and size override PASS");
}
int main(int argc, char **argv)
{
    assert(argc==3); assert(strlen(argv[1])<sizeof(test_root)); strcpy(test_root,argv[1]);
    assert(strlen(argv[2])<sizeof(font_path)); strcpy(font_path,argv[2]);
    persistence(); fonts(); assert(!live_allocations); return 0;
}
